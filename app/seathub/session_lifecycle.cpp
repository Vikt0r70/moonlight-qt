#include "session_lifecycle.h"

#include <QLoggingCategory>
#include <QWindow>

Q_LOGGING_CATEGORY(seathubLifecycle, "seathub.lifecycle")

SessionLifecycle::SessionLifecycle(QObject* parent)
    : QObject(parent)
{
}

SessionLifecycle::~SessionLifecycle()
{
    // The session belongs to whoever attached it, so it is detached and not destroyed. Clearing
    // `m_session` is what stops a signal still in flight from arriving at a half-destroyed object.
    disconnectEngineSignals();
    m_session = nullptr;
}

QObject* SessionLifecycle::upstreamSession() const
{
    return m_session;
}

void SessionLifecycle::attachSession(EngineSession* session)
{
    if (session == m_session) {
        return;
    }

    if (m_active) {
        // A running session cannot be swapped out. `start()` refuses to run two at once and this
        // refuses to change the one that is running; both fail closed.
        qCWarning(seathubLifecycle) << "refusing to attach a session while one is active";
        return;
    }

    disconnectEngineSignals();

    m_session = session;
    m_sessionGuard = session;
    connectEngineSignals();

    qCInfo(seathubLifecycle) << "engine session" << (session ? "attached" : "detached");
    emit upstreamSessionChanged();
}

void SessionLifecycle::connectEngineSignals()
{
    if (!m_session) {
        return;
    }

    EngineSession* s = m_session;
    connect(s, &EngineSession::stageStarting, this, &SessionLifecycle::stageStarting);
    connect(s, &EngineSession::stageFailed, this, &SessionLifecycle::stageFailed);
    connect(s, &EngineSession::connectionStarted, this, &SessionLifecycle::connectionStarted);
    connect(s, &EngineSession::displayLaunchError, this, &SessionLifecycle::displayLaunchError);
    connect(s, &EngineSession::displayLaunchWarning, this, &SessionLifecycle::displayLaunchWarning);
    connect(s, &EngineSession::quitStarting, this, &SessionLifecycle::quitStarting);
    connect(s, &EngineSession::sessionFinished, this, &SessionLifecycle::sessionFinished);
    connect(s, &EngineSession::readyForDeletion, this, [this]() {
        // The engine has finished tearing the SDL window down; it is safe to release the
        // Session and to let the QML window come back (D-03). Upstream's QML does the
        // equivalent with `session = null; gc()`.
        emit readyForDeletion();

        // Detach only. The object stays alive: `SeatHubClient` owns it and destroys it after the
        // control-plane teardown it starts from this same signal has been asked for.
        disconnectEngineSignals();
        m_session = nullptr;
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

    if (!m_session) {
        // Fail closed. This is the branch that used to start the tracer: with no engine session
        // attached the client would have shown a stage sequence and a window for a session that
        // never existed. Nothing substitutes for a session here.
        qCWarning(seathubLifecycle) << "no engine session is attached; refusing to start";
        return false;
    }

    m_qtWindow = window;
    m_active = true;
    emit activeChanged();

    qCInfo(seathubLifecycle) << "session start requested;"
                             << "qt window visible =" << (m_qtWindow && m_qtWindow->isVisible());

    // The engine path: hand the engine the Qt window and let it run the session to completion.
    // `Session::exec()` blocks and pumps the Qt event loop itself, which is exactly how upstream's
    // StreamSegue.qml invokes it. A fake may return immediately instead; see the method's contract.
    m_session->run(window);
    return true;
}

void SessionLifecycle::interrupt()
{
    if (!m_active || !m_session) {
        return;
    }

    // The engine owns the quit keystroke and handles it as an ordinary session end; see
    // `EngineSession::interrupt()`.
    m_session->interrupt();
}

bool SessionLifecycle::publishOverlaySurface(SDL_Surface* surface)
{
    if (!m_session) {
        // No session means no stream and no swapchain to composite into. The caller releases the
        // surface, which is `EngineSession::publishOverlaySurface`'s contract for a refusal.
        return false;
    }

    return m_session->publishOverlaySurface(surface);
}
