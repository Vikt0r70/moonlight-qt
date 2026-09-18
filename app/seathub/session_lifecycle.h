#pragma once

// Owns the upstream streaming-engine seam (`app/streaming/session.h`) for SeatHub.
//
// D-01/D-03/D-32: this is the single place that constructs, drives and destroys the
// engine's `Session`, and the single place the engine's lifecycle signals are observed.
// SeatHub's own signals carry the same names as the engine's, so `SessionSegue.qml`
// has exactly one set of handlers no matter which path is live:
//
//   attached path (Plan 03-03 and later)  engine Session -> connect -> re-emit below
//   tracer path  (Plan 03-02 Task 1)     no engine Session -> emit below directly
//
// Only one path is live at a time: `m_session != nullptr` selects the attached path, and
// the tracer sequence refuses to start while a Session is attached.
//
// The engine's files under `app/streaming/` are never modified to achieve this beyond
// ADR-0046's window-title literal - the seam used here (`Session::exec(QWindow*)` plus the
// engine's public lifecycle signals) is upstream's own, unaltered.

#include <QObject>
#include <QPointer>
#include <QString>
#include <QWindow>

class QTimer;
class Session;

class SessionLifecycle : public QObject
{
    Q_OBJECT

    /// The engine Session currently attached, or null in the tracer's stubbed path.
    Q_PROPERTY(QObject* upstreamSession READ upstreamSession NOTIFY upstreamSessionChanged)

    /// True from the moment a stream is driven until SDL destruction is complete.
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)

    /// Display name used for the stream window title. In the attached path the engine
    /// uses the real computer name; this is the tracer's stand-in.
    Q_PROPERTY(QString rigName READ rigName WRITE setRigName NOTIFY rigNameChanged)

    /// True while the tracer's stubbed sequence is driving the lifecycle instead of a
    /// real engine Session. Plan 03-03 attaches a real Session and this becomes false.
    Q_PROPERTY(bool stubbed READ stubbed NOTIFY stubbedChanged)

public:
    explicit SessionLifecycle(QObject* parent = nullptr);
    ~SessionLifecycle() override;

    QObject* upstreamSession() const;
    bool active() const { return m_active; }
    bool stubbed() const { return m_stubbed; }
    QString rigName() const { return m_rigName; }
    void setRigName(const QString& name);

    /// Hands this lifecycle an engine Session created elsewhere (the C++ owner keeps
    /// lifetime, exactly as upstream's AppModel/ComputerModel do). Connects the 7
    /// engine signals and re-emits them under this object.
    Q_INVOKABLE void attachSession(QObject* session);

    /// Owns the engine seam: drives the attached Session to completion, or runs the
    /// tracer's stubbed sequence when none is attached (Plan 03-02 Task 1).
    Q_INVOKABLE bool start(QWindow* window);

    /// D-02: ends the active stream immediately, without touching any engine file.
    /// Attached path: pushes the exact keystroke the engine already treats as "quit"
    /// (Ctrl+Alt+Shift+Q -> SdlInputHandler::KeyComboQuit -> SDL_QUIT), which the
    /// engine's own event loop then handles as a normal session end.
    /// Tracer path: jumps the stubbed sequence straight to its teardown leg.
    Q_INVOKABLE void interrupt();

signals:
    void upstreamSessionChanged();
    void activeChanged();
    void rigNameChanged();
    void stubbedChanged();

    // --- SeatHub's lifecycle signals. Same names as the engine's, so one QML wiring
    // --- serves both paths. See SessionSegue.qml for the visibility contract.
    void stageStarting(QString stage);
    void stageFailed(QString stage, int errorCode, QString failingPorts);
    void connectionStarted();
    void displayLaunchError(QString text);
    /// The engine's other public reporting seam: a setting it could not honour as saved
    /// (`Session::emitLaunchWarning`, e.g. "Your host PC doesn't support HDR streaming"). Like
    /// `displayLaunchError`, the text is engine wording and never reaches a screen; SeatHub's
    /// settings page turns it into its own sentence beside the saved value (D-14, D-51).
    void displayLaunchWarning(QString text);
    void quitStarting();
    void sessionFinished(int portTestResult);
    void readyForDeletion();

private slots:
    void runStubStage(int step);

private:
    void connectEngineSignals();
    void disconnectEngineSignals();

    /// True when the Qt window the engine (or the tracer) is streaming over is on screen.
    /// Used by the lifecycle's own diagnostics, which is how the D-01/D-03 visibility
    /// sequence is provable from a run's log rather than only from its source.
    bool qtWindowVisible() const;
    void beginStubSequence();
    void createStubStreamWindow();
    void destroyStubStreamWindow();
    void finishStubSequence();
    void stopStubSequence();

    Session* m_session = nullptr;
    QPointer<QObject> m_sessionGuard;

    QWindow* m_qtWindow = nullptr;
    bool m_active = false;
    bool m_stubbed = false;
    QString m_rigName;

    // Tracer-only state (Plan 03-02 Task 1; removed when Plan 03-03 attaches the engine).
    QTimer* m_stubTimer = nullptr;
    int m_stubStep = 0;
    bool m_stubOwnsVideoSubsystem = false;
    void* m_stubWindow = nullptr;   // SDL_Window*, kept void* so SDL.h stays out of the header
};
