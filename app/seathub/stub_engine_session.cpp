#include "stub_engine_session.h"

#include <QLoggingCategory>
#include <QTimer>
#include <QWindow>

#include <SDL.h>

Q_LOGGING_CATEGORY(seathubStub, "seathub.stub")

namespace {

// The stream window's title suffix. This is the same literal the fork applies to
// `app/streaming/session.cpp`'s non-Darwin branch under ADR-0046, so the stubbed window exercises
// the real title format rather than a stand-in of its own.
const char* kWindowTitleSuffix = " - SeatHub";

// Timing. Long enough for each phase to be observed by eye and by a window-title probe; short
// enough that a manual run finishes in seconds.
constexpr int kStageStepMs = 800;
constexpr int kHideToWindowMs = 300;
constexpr int kStreamMs = 4000;

} // namespace

StubEngineSession::StubEngineSession(QObject* parent)
    : EngineSession(parent),
      m_rigName(QStringLiteral("SeatHub tracer"))
{
}

StubEngineSession::~StubEngineSession()
{
    stop();
    destroyStreamWindow();
}

void StubEngineSession::setRigName(const QString& name)
{
    m_rigName = name;
}

bool StubEngineSession::publishOverlaySurface(SDL_Surface* surface)
{
    if (surface != nullptr) {
        // Nothing composites here, so the surface is released rather than held. Refusing without
        // freeing is the one way a publisher can leak under this contract.
        SDL_FreeSurface(surface);
    }
    return false;
}

void StubEngineSession::run(QWindow* window)
{
    m_qtWindow = window;
    m_step = 0;

    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setSingleShot(true);
        connect(m_timer, &QTimer::timeout, this, [this]() { runStage(m_step); });
    }

    // Stage 1 fires immediately so the customer sees the first line as the view changes.
    runStage(m_step);
}

void StubEngineSession::runStage(int step)
{
    // Stages are named the way the engine names its own (`LiGetStageName()`), in order; the facade
    // reads only that one began, and never shows the name. Each one is emitted from the event loop
    // rather than in a loop, so the QML window actually repaints between them.
    switch (step) {
    case 0:
        emit stageStarting(QStringLiteral("Platform initialization"));
        m_step = 1;
        m_timer->start(kStageStepMs);
        break;
    case 1:
        emit stageStarting(QStringLiteral("Name resolution"));
        m_step = 2;
        m_timer->start(kStageStepMs);
        break;
    case 2:
        emit stageStarting(QStringLiteral("RTSP handshake"));
        m_step = 3;
        m_timer->start(kStageStepMs);
        break;
    case 3:
        // D-01: the Qt window is hidden on connectionStarted, *before* the stream window exists,
        // so the two are never visible together (and there is no launcher frame behind the
        // stream). The engine does the same: it emits connectionStarted and only then creates its
        // SDL window.
        emit connectionStarted();
        // SessionSegue's handler ran synchronously with the emission above, so the Qt window's
        // state here is the state the customer would see.
        qCInfo(seathubStub) << "connectionStarted emitted;"
                            << "qt window visible =" << qtWindowVisible()
                            << "(must be false: the stream window does not exist yet)";
        m_step = 4;
        m_timer->start(kHideToWindowMs);
        break;
    case 4:
        createStreamWindow();
        m_step = 5;
        m_timer->start(kStreamMs);
        break;
    case 5:
        finish();
        break;
    default:
        break;
    }
}

void StubEngineSession::createStreamWindow()
{
    // This class owns the window so the visibility sequence is observable without a Sunshine host.
    // It is created with the same title format the engine uses, so "no Moonlight on screen" is
    // provable here.
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "SeatHub tracer: SDL_InitSubSystem(SDL_INIT_VIDEO) failed: %s",
                        SDL_GetError());
            return;
        }
        m_ownsVideoSubsystem = true;
    }

    const QByteArray title = QString(m_rigName + QString::fromLatin1(kWindowTitleSuffix)).toUtf8();
    m_window = SDL_CreateWindow(title.constData(),
                                SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED,
                                960,
                                540,
                                SDL_WINDOW_SHOWN);
    if (!m_window) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SeatHub tracer: SDL_CreateWindow() failed: %s", SDL_GetError());
        return;
    }

    // The engine sets the same title format in app/streaming/session.cpp's non-Darwin branch
    // (ADR-0046). Logging it here is what makes "no Moonlight on screen" provable from the run
    // itself rather than by reading the source.
    qCInfo(seathubStub) << "stream window created; title =" << QString::fromUtf8(title)
                        << "qt window visible =" << qtWindowVisible();
}

void StubEngineSession::destroyStreamWindow()
{
    if (m_window) {
        SDL_DestroyWindow(static_cast<SDL_Window*>(m_window));
        m_window = nullptr;
    }
    if (m_ownsVideoSubsystem) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        m_ownsVideoSubsystem = false;
    }
}

void StubEngineSession::finish()
{
    stop();

    // D-03 / Pitfall 1: SDL destruction is *proven* here - the window is gone before the lifecycle
    // reports the session finished. `quitStarting` deliberately carries no window-restore meaning;
    // upstream fires it before deferred cleanup completes, so restoring there would put the Qt
    // window on screen while the SDL window still exists.
    emit quitStarting();
    qCInfo(seathubStub) << "quitStarting emitted;"
                        << "qt window visible =" << qtWindowVisible()
                        << "(must still be false: SDL has not been destroyed yet)";

    destroyStreamWindow();
    qCInfo(seathubStub) << "stream window destroyed; SDL video was initialised ="
                        << SDL_WasInit(SDL_INIT_VIDEO)
                        << "; qt window visible =" << qtWindowVisible();

    emit sessionFinished(0);
    emit readyForDeletion();

    // readyForDeletion is the only signal SessionSegue restores the Qt window on, and its handler
    // has already run by the time the emission returns.
    qCInfo(seathubStub) << "readyForDeletion emitted;"
                        << "qt window visible =" << qtWindowVisible()
                        << "(must be true: SDL destruction is proven)";
}

void StubEngineSession::stop()
{
    if (m_timer) {
        m_timer->stop();
    }
    m_step = 0;
}

void StubEngineSession::interrupt()
{
    finish();
}

bool StubEngineSession::qtWindowVisible() const
{
    return m_qtWindow && m_qtWindow->isVisible();
}
