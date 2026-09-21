#include "moonlight_engine_session.h"

#include <QLoggingCategory>
#include <QSslCertificate>

#include <SDL.h>

#include "backend/nvapp.h"
#include "backend/nvcomputer.h"
#include "backend/nvhttp.h"
#include "backend/nvpairingmanager.h"
#include "streaming/session.h"
#include "stream_window_name.h"

Q_LOGGING_CATEGORY(seathubEngine, "seathub.engine")

MoonlightPairedHost::MoonlightPairedHost(NvHTTP& http, const QString& serverInfo)
    : m_computer(new NvComputer(http, serverInfo))
{
    // The engine titles its stream window with this record's name, and the record was just built from
    // what the rig says about itself. A customer must never read a rig's name, and the window title
    // shows in the taskbar and the alt-tab switcher, so the name is replaced here - in this file, not
    // in the engine's - before the engine can see it (CUST-01, OD-13; `stream_window_name.h`).
    SeatHubStreamWindow::applyNeutralName(m_computer);
}

MoonlightPairedHost::~MoonlightPairedHost()
{
    delete m_computer;
}

void MoonlightPairedHost::pinCertificate(const QByteArray& pem)
{
    m_computer->serverCert = QSslCertificate(pem);
    m_paired = true;
}

void MoonlightPairedHost::fetchAppList()
{
    try {
        NvHTTP http(m_computer);
        QVector<NvApp> apps = http.getAppList();
        if (apps.isEmpty()) {
            return;
        }

        // Upstream keeps the list on the computer record the same way
        // (`ComputerManager::updateComputer`), so the record this client hands the engine has the
        // shape the engine expects when it resolves `currentGameId` itself. The list is kept in the
        // host's own order: upstream's `sortAppList()` is private to `NvComputer`, and nothing here
        // depends on the order - `launchApp()` matches on `currentGameId` or counts, either way.
        m_computer->appList = apps;
        m_appListRead = true;
    }
    catch (const QtNetworkReplyException& e) {
        qCWarning(seathubEngine) << "error reading the host's application list:" << e.what();
    }
    catch (const GfeHttpResponseException& e) {
        qCWarning(seathubEngine) << "the host refused the application list request;"
                                 << "status = " << e.getStatusCode();
    }
    catch (const std::exception& e) {
        qCWarning(seathubEngine) << "error reading the host's application list:" << e.what();
    }
}

bool MoonlightPairedHost::launchApp(NvApp* out) const
{
    if (out == nullptr || !m_paired) {
        return false;
    }

    // The host says it is already running something. That is the application to attach to, and
    // the only case where the host names one itself (`NvComputer::currentGameId`, parsed from
    // `serverinfo`). Upstream has the same path - it is what "resume" means.
    if (m_computer->currentGameId != 0) {
        for (const NvApp& app : m_computer->appList) {
            if (app.id == m_computer->currentGameId) {
                *out = app;
                return true;
            }
        }
    }

    QVector<NvApp> candidates;
    // By value: `NvApp::isInitialized()` is not a const member upstream, so a const reference
    // cannot be asked.
    for (NvApp app : m_computer->appList) {
        // `hidden` and `isInitialized()` are upstream's own filters (`AppModel` shows exactly the
        // applications that pass them), so "how many applications does this rig serve" is counted
        // the way a customer would count them.
        if (app.isInitialized() && !app.hidden) {
            candidates.append(app);
        }
    }

    if (candidates.size() != 1) {
        // Includes the zero case: the host never answered, or answered with nothing. Both are
        // "this client cannot name an application", and both fail closed above.
        qCWarning(seathubEngine) << "the rig's application list names" << candidates.size()
                                 << "applications to launch; the contract names none";
        return false;
    }

    *out = candidates.first();
    return true;
}

MoonlightEngineSession* MoonlightEngineSession::create(const PairedHostPtr& host, QObject* parent)
{
    auto* paired = dynamic_cast<MoonlightPairedHost*>(host.get());
    if (paired == nullptr || !paired->isPaired()) {
        qCWarning(seathubEngine) << "no paired host to stream from";
        return nullptr;
    }

    NvApp app;
    if (!paired->launchApp(&app)) {
        return nullptr;
    }

    // The computer's name is the neutral word by now, so it says nothing worth logging; the
    // application is what the rig served.
    qCInfo(seathubEngine) << "engine session for application" << app.name;
    return new MoonlightEngineSession(host, app, parent);
}

MoonlightEngineSession::MoonlightEngineSession(PairedHostPtr host, const NvApp& app, QObject* parent)
    : EngineSession(parent),
      m_host(std::move(host))
{
    // Upstream's own construction and wiring order, from `StreamSegue.qml`: the `Session` exists
    // and is connected before any window is handed to it, and `exec()` is what starts the stream.
    // `Session` takes the application by reference and keeps its own copy.
    NvApp launchApp = app;
    m_engine = new Session(static_cast<MoonlightPairedHost*>(m_host.get())->computer(), launchApp);

    // The seven upstream lifecycle signals, plus the launch warning. Forwarded rather than
    // re-interpreted: `SessionLifecycle` is the one place SeatHub maps them onto its own view
    // state, and `SessionSegue.qml` the one place the visibility sequence is applied.
    connect(m_engine, &Session::stageStarting, this, &EngineSession::stageStarting);
    connect(m_engine, &Session::stageFailed, this, &EngineSession::stageFailed);
    connect(m_engine, &Session::connectionStarted, this, &EngineSession::connectionStarted);
    connect(m_engine, &Session::displayLaunchError, this, &EngineSession::displayLaunchError);
    connect(m_engine, &Session::displayLaunchWarning, this, &EngineSession::displayLaunchWarning);
    connect(m_engine, &Session::quitStarting, this, &EngineSession::quitStarting);
    connect(m_engine, &Session::sessionFinished, this, &EngineSession::sessionFinished);
    connect(m_engine, &Session::readyForDeletion, this, &EngineSession::readyForDeletion);
}

MoonlightEngineSession::~MoonlightEngineSession()
{
    // The engine's `Session` lives exactly as long as this object. Upstream releases it the same
    // way (`StreamSegue.qml` sets `session = null` and lets the QML engine collect it); here the
    // lifecycle detaches before the facade destroys this, so nothing holds a dangling pointer.
    delete m_engine;
}

void MoonlightEngineSession::run(QWindow* window)
{
    // Blocks for the whole stream and pumps the Qt event loop itself - upstream's own contract,
    // and the reason the control-plane objects own a thread (see `SeatHubClient`'s header).
    m_engine->exec(window);
}

void MoonlightEngineSession::interrupt()
{
    // The engine owns the quit keystroke: `SdlInputHandler` maps Ctrl+Alt+Shift+Q to
    // `KeyComboQuit` and, on a match, pushes `SDL_QUIT` for the engine's own event loop to handle
    // as an ordinary session end. v6.1.0 has no `Session::interrupt()`, so handing it that exact
    // event is how D-02 is satisfied without editing any file under `app/streaming/`.
    SDL_Event quit;
    SDL_zero(quit);
    quit.type = SDL_KEYDOWN;
    quit.key.type = SDL_KEYDOWN;
    quit.key.timestamp = SDL_GetTicks();
    quit.key.windowID = 0;
    quit.key.state = SDL_PRESSED;
    quit.key.repeat = 0;
    quit.key.keysym.scancode = SDL_SCANCODE_Q;
    quit.key.keysym.sym = SDLK_q;
    quit.key.keysym.mod = static_cast<Uint16>(KMOD_CTRL | KMOD_ALT | KMOD_SHIFT);

    if (SDL_PushEvent(&quit) != 1) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SeatHub: SDL_PushEvent() for the quit combo failed: %s", SDL_GetError());
    }
}

bool MoonlightEngineSession::publishOverlaySurface(SDL_Surface* surface)
{
    // The overlay manager ADR-0045 added a bitmap input to. Reached through this session object
    // rather than through the engine's `Session::get()` global, so the publisher's "is there a
    // stream to composite into" question is answered by whether a session is attached at all.
    Overlay::OverlayManager& manager = m_engine->getOverlayManager();

    if (surface == nullptr) {
        manager.setOverlayState(Overlay::OverlayStatusUpdate, false);
        return true;
    }

    // Enable before publishing. The manager only notifies on a state change, so this is
    // idempotent, and it means a HUD published before the renderer has registered is still picked
    // up on the first frame after it does - the HUD's first publish happens on
    // `connectionStarted`, which is before the engine creates its stream window (D-01).
    manager.setOverlayState(Overlay::OverlayStatusUpdate, true);
    return manager.updateOverlaySurface(Overlay::OverlayStatusUpdate, surface);
}
