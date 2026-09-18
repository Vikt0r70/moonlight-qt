#include "session_lifecycle.h"

#include <QLoggingCategory>
#include <QTimer>
#include <QWindow>

#include <SDL.h>

#include "streaming/session.h"

Q_LOGGING_CATEGORY(seathubLifecycle, "seathub.lifecycle")

namespace {

// The tracer's stream-window title suffix. This is the same literal the fork applies to
// `app/streaming/session.cpp`'s non-Darwin branch under ADR-0046, so the stubbed window
// exercises the real title format rather than a stand-in of its own.
const char* kWindowTitleSuffix = " - SeatHub";

// Tracer timing. Long enough for each phase to be observed by eye and by a window-title
// probe; short enough that a manual run finishes in seconds.
constexpr int kStageStepMs = 800;
constexpr int kHideToWindowMs = 300;
constexpr int kStubStreamMs = 4000;

} // namespace

SessionLifecycle::SessionLifecycle(QObject* parent)
    : QObject(parent),
      m_rigName(QStringLiteral("SeatHub tracer"))
{
}

SessionLifecycle::~SessionLifecycle()
{
    stopStubSequence();
    destroyStubStreamWindow();
}

QObject* SessionLifecycle::upstreamSession() const
{
    return m_session;
}

void SessionLifecycle::setRigName(const QString& name)
{
    if (m_rigName == name) {
        return;
    }
    m_rigName = name;
    emit rigNameChanged();
}

void SessionLifecycle::attachSession(QObject* session)
{
    Session* engineSession = qobject_cast<Session*>(session);
    if (!engineSession || engineSession == m_session) {
        return;
    }

    // A real engine Session exists, so the tracer's stubbed path is no longer reachable.
    stopStubSequence();
    disconnectEngineSignals();

    m_session = engineSession;
    m_sessionGuard = session;
    m_stubbed = false;
    connectEngineSignals();

    emit stubbedChanged();
    emit upstreamSessionChanged();
}

void SessionLifecycle::connectEngineSignals()
{
    if (!m_session) {
        return;
    }

    Session* s = m_session;
    connect(s, &Session::stageStarting, this, &SessionLifecycle::stageStarting);
    connect(s, &Session::stageFailed, this, &SessionLifecycle::stageFailed);
    connect(s, &Session::connectionStarted, this, &SessionLifecycle::connectionStarted);
    connect(s, &Session::displayLaunchError, this, &SessionLifecycle::displayLaunchError);
    connect(s, &Session::displayLaunchWarning, this, &SessionLifecycle::displayLaunchWarning);
    connect(s, &Session::quitStarting, this, &SessionLifecycle::quitStarting);
    connect(s, &Session::sessionFinished, this, &SessionLifecycle::sessionFinished);
    connect(s, &Session::readyForDeletion, this, [this]() {
        // The engine has finished tearing the SDL window down; it is safe to release the
        // Session and to let the QML window come back (D-03). Upstream's QML does the
        // equivalent with `session = null; gc()`.
        emit readyForDeletion();

        disconnectEngineSignals();
        Session* doomed = m_session;
        m_session = nullptr;
        if (doomed) {
            doomed->deleteLater();
        }
        m_active = false;
        emit activeChanged();
        emit upstreamSessionChanged();
    });
}

void SessionLifecycle::disconnectEngineSignals()
{
    if (m_session) {
        m_session->disconnect(this);
    }
}

bool SessionLifecycle::start(QWindow* window)
{
    if (m_active) {
        return false;
    }

    m_qtWindow = window;
    m_active = true;
    emit activeChanged();

    qCInfo(seathubLifecycle) << "session start requested;"
                             << "engine session attached =" << (m_session != nullptr)
                             << "qt window visible =" << qtWindowVisible();

    if (m_session) {
        // Attached path: hand the engine the Qt window and let it run the session to
        // completion. `Session::exec()` blocks and pumps the Qt event loop itself, which
        // is exactly how upstream's StreamSegue.qml invokes it.
        m_session->exec(window);
        return true;
    }

    beginStubSequence();
    return true;
}

bool SessionLifecycle::qtWindowVisible() const
{
    return m_qtWindow && m_qtWindow->isVisible();
}

void SessionLifecycle::beginStubSequence()
{
    m_stubbed = true;
    emit stubbedChanged();

    m_stubStep = 0;
    if (!m_stubTimer) {
        m_stubTimer = new QTimer(this);
        m_stubTimer->setSingleShot(true);
        connect(m_stubTimer, &QTimer::timeout, this, [this]() { runStubStage(m_stubStep); });
    }

    // Stage 1 fires immediately so the customer sees the first line as the view changes.
    runStubStage(m_stubStep);
}

void SessionLifecycle::runStubStage(int step)
{
    // Stages mirror `docs/spec/copy.md` §Play flow, in order. Each one is emitted from the
    // event loop rather than in a loop, so the QML window actually repaints between them.
    switch (step) {
    case 0:
        emit stageStarting(QStringLiteral("Waiting for a free rig"));
        m_stubStep = 1;
        m_stubTimer->start(kStageStepMs);
        break;
    case 1:
        emit stageStarting(QStringLiteral("Preparing the rig"));
        m_stubStep = 2;
        m_stubTimer->start(kStageStepMs);
        break;
    case 2:
        emit stageStarting(QStringLiteral("Preparing the stream"));
        m_stubStep = 3;
        m_stubTimer->start(kStageStepMs);
        break;
    case 3:
        // D-01: the Qt window is hidden on connectionStarted, *before* the stream window
        // exists, so the two are never visible together (and there is no launcher frame
        // behind the stream). The engine does the same: it emits connectionStarted and
        // only then creates its SDL window.
        emit connectionStarted();
        // SessionSegue's handler ran synchronously with the emission above, so the Qt
        // window's state here is the state the customer would see.
        qCInfo(seathubLifecycle) << "connectionStarted emitted;"
                                 << "qt window visible =" << qtWindowVisible()
                                 << "(must be false: the stream window does not exist yet)";
        m_stubStep = 4;
        m_stubTimer->start(kHideToWindowMs);
        break;
    case 4:
        createStubStreamWindow();
        m_stubStep = 5;
        m_stubTimer->start(kStubStreamMs);
        break;
    case 5:
        finishStubSequence();
        break;
    default:
        break;
    }
}

void SessionLifecycle::createStubStreamWindow()
{
    // The tracer owns this window so the visibility sequence is observable without a
    // Sunshine host (there is none on this machine). It is created with the same title
    // format the engine uses, so "no Moonlight on screen" is provable here.
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "SeatHub tracer: SDL_InitSubSystem(SDL_INIT_VIDEO) failed: %s",
                        SDL_GetError());
            return;
        }
        m_stubOwnsVideoSubsystem = true;
    }

    const QByteArray title = QString(m_rigName + QString::fromLatin1(kWindowTitleSuffix)).toUtf8();
    m_stubWindow = SDL_CreateWindow(title.constData(),
                                    SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED,
                                    960,
                                    540,
                                    SDL_WINDOW_SHOWN);
    if (!m_stubWindow) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SeatHub tracer: SDL_CreateWindow() failed: %s", SDL_GetError());
        return;
    }

    // The engine sets the same title format in app/streaming/session.cpp's non-Darwin
    // branch (ADR-0046). Logging it here is what makes "no Moonlight on screen" provable
    // from the run itself rather than by reading the source.
    qCInfo(seathubLifecycle) << "stream window created; title =" << QString::fromUtf8(title)
                             << "qt window visible =" << qtWindowVisible();
}

void SessionLifecycle::destroyStubStreamWindow()
{
    if (m_stubWindow) {
        SDL_DestroyWindow(static_cast<SDL_Window*>(m_stubWindow));
        m_stubWindow = nullptr;
    }
    if (m_stubOwnsVideoSubsystem) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        m_stubOwnsVideoSubsystem = false;
    }
}

void SessionLifecycle::finishStubSequence()
{
    stopStubSequence();

    // D-03 / Pitfall 1: SDL destruction is *proven* here - the window is gone before the
    // lifecycle reports the session finished. `quitStarting` deliberately carries no
    // window-restore meaning; upstream fires it before deferred cleanup completes, so
    // restoring there would put the Qt window on screen while the SDL window still exists.
    emit quitStarting();
    qCInfo(seathubLifecycle) << "quitStarting emitted;"
                             << "qt window visible =" << qtWindowVisible()
                             << "(must still be false: SDL has not been destroyed yet)";

    destroyStubStreamWindow();
    qCInfo(seathubLifecycle) << "stream window destroyed; SDL video was initialised =" << SDL_WasInit(SDL_INIT_VIDEO)
                             << "; qt window visible =" << qtWindowVisible();

    emit sessionFinished(0);
    emit readyForDeletion();

    // readyForDeletion is the only signal SessionSegue restores the Qt window on, and its
    // handler has already run by the time the emission returns.
    qCInfo(seathubLifecycle) << "readyForDeletion emitted;"
                             << "qt window visible =" << qtWindowVisible()
                             << "(must be true: SDL destruction is proven)";

    m_stubbed = false;
    m_active = false;
    emit stubbedChanged();
    emit activeChanged();
}

void SessionLifecycle::stopStubSequence()
{
    if (m_stubTimer) {
        m_stubTimer->stop();
    }
    m_stubStep = 0;
}

void SessionLifecycle::interrupt()
{
    if (!m_active) {
        return;
    }

    if (!m_session) {
        // Tracer path: jump straight to the teardown leg.
        finishStubSequence();
        return;
    }

    // Attached path. The engine owns the quit keystroke: SdlInputHandler maps
    // Ctrl+Alt+Shift+Q to KeyComboQuit and, on match, pushes SDL_QUIT for the engine's own
    // event loop to handle as an ordinary session end. Hand it that exact event so D-02 is
    // satisfied without editing any file under app/streaming/.
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
