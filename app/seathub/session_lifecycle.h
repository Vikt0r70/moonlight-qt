#pragma once

// The one place a streaming engine's lifecycle becomes the client's view state.
//
// Plan 03-02 shipped this class with two paths: an "attached" one that drove a real engine
// `Session` and a "tracer" one that ran a timed sequence when no engine session was attached. No
// plan ever attached one, so the tracer - an expedient for proving the D-01/D-03 window sequence
// with no Sunshine host - was in practice the only path a customer would have got. Plan 03-06's gap
// closure removed the fallback: nothing attached means `start()` fails closed, and the tracer is
// now an explicitly injected `EngineSession` (`stub_engine_session.h`) rather than a default.
//
// What the class owns, and what it does not:
//
//   * It does NOT own the session. Whoever attaches one owns it and keeps it alive for at least as
//     long as the lifecycle holds it - the same contract `Session::exec()` comes with upstream,
//     where the allocating QML object is what releases it.
//   * It does NOT know the engine type. `EngineSession` (`engine_session.h`) is the whole surface,
//     which is what makes the signal wiring and the fail-closed start assertable in a test binary
//     with no engine, no SDL and no host in it.
//
// The D-01/D-03 window sequence is not implemented here - it is applied by `SessionSegue.qml`, on
// the signals this class re-emits, exactly as upstream's QML applies it on the engine's own.

#include <QObject>
#include <QPointer>
#include <QString>

#include "engine_session.h"

class QWindow;
struct SDL_Surface;

class SessionLifecycle : public QObject
{
    Q_OBJECT

    /// The attached session, or null. `QObject*` rather than `EngineSession*` so QML can name it
    /// without the seam type; `SessionSegue.qml` only ever connects to this object's signals.
    Q_PROPERTY(QObject* upstreamSession READ upstreamSession NOTIFY upstreamSessionChanged)
    /// True from `start()` until the session reports `readyForDeletion()`.
    ///
    /// Cleared *above* this class's re-emission of that signal, so a handler connected to
    /// `readyForDeletion` already sees false. The order is load-bearing:
    /// `SeatHubClient::releaseEngineSession()` refuses to release the engine object while this
    /// reports true, and it is called from a direct connection to that same signal.
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)

public:
    explicit SessionLifecycle(QObject* parent = nullptr);
    ~SessionLifecycle() override;

    QObject* upstreamSession() const;
    bool active() const { return m_active; }

    /// Attach the session `start()` will drive. Pass null to detach.
    ///
    /// Deliberately not `Q_INVOKABLE`: the only caller is `SeatHubClient`, and a QML-reachable
    /// setter for "the object that gets driven for the whole stream" is view-layer surface this
    /// client has no reason to expose.
    ///
    /// The caller keeps ownership. Attaching while a session is active is refused: replacing the
    /// object under a running `run()` would leave the old one driven by nothing.
    void attachSession(EngineSession* session);

    /// Hand the engine the Qt window and run the session.
    ///
    /// False when a session is already active, or when none is attached. The second case is the
    /// point of the Plan 03-06 fix: this used to start the tracer instead, which meant every
    /// customer got a fake stream. The caller reports a failure (which carries a reason, a retry
    /// and a support reference, D-51) rather than substituting one.
    Q_INVOKABLE bool start(QWindow* window);

    /// D-02: end the active stream now.
    Q_INVOKABLE void interrupt();

    /// ADR-0045: composite the HUD bitmap into the running stream's own swapchain. False when
    /// there is no session to composite into; see `EngineSession::publishOverlaySurface` for the
    /// ownership contract, which holds in every case.
    bool publishOverlaySurface(SDL_Surface* surface);

signals:
    // SeatHub's own copies of the engine's lifecycle signals, re-emitted so `SessionSegue.qml` has
    // one wiring whether the session is the engine's or a test's.
    void stageStarting(QString stage);
    void stageFailed(QString stage, int errorCode, QString failingPorts);
    void connectionStarted();
    void displayLaunchError(QString text);
    void displayLaunchWarning(QString text);
    void quitStarting();
    void sessionFinished(int portTestResult);
    void readyForDeletion();

    void activeChanged();
    void upstreamSessionChanged();

private:
    void connectEngineSignals();
    void disconnectEngineSignals();

    EngineSession* m_session = nullptr;
    /// `deleteLater()`d only to keep a queued connection from delivering into a destroyed object;
    /// it never deletes the session itself.
    QPointer<QObject> m_sessionGuard;
    QWindow* m_qtWindow = nullptr;
    bool m_active = false;
};
