#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

#include "control_plane_client.h"
#include "error_map.h"

class ControlPlaneClient;

// The liveness reporter (D-31, D-34, D-33, STREAM-09).
//
// What it does: `POST /api/sessions/{id}/liveness` every 10 seconds (the locked interval) with
// the ADR-0041 payload - `state` and `error_code` - so the control plane can tell "still
// streaming and healthy" from "still there but reconnecting because it just failed with
// SH-XXXXXX". The server's own grace is 30 s (`LIVENESS_GRACE_SECONDS`, `docs/spec/timing.md`),
// refreshed on every report.
//
// It also reads the wallet on the same tick (Phase 5, CUST-15, `docs/spec/timing.md` "Client's
// in-stream wallet read"): no timer of its own and no new interval - the balance rides the report
// the client already sends. The balance is announced through `walletRead` on this object's own
// thread, the control-plane thread, because that is the only one still running an event loop
// while a stream is up; whoever consumes it for the HUD must do so from there, not through the
// facade's (suspended) thread. A read that fails is only a `walletReadFailed`: nothing about the
// session or the balance changes, and it is never a liveness failure.
//
// What it deliberately does NOT do: end the session when a report fails.
//
// D-33 is the whole reason this class exists in this shape. If the control plane is unreachable
// mid-stream, the customer is still watching a working stream - the media path goes straight
// from the rig to this PC and never through the control plane. Killing the session because a
// heartbeat could not be delivered would turn a control-plane outage into a customer-visible
// one. So the timer keeps firing, and once the grace window has elapsed without a single
// successful report it raises a NON-FATAL warning and carries on retrying.
//
// The real stop is elsewhere: the session token's `authorized_through` horizon, enforced by
// `AuthorizedThroughTimer`. That is the billing-safety backstop, and it is the only locally
// enforced end (D-33).
//
// Threading: this class is driven by a QTimer, and upstream suspends Qt processing for the whole
// duration of a stream (`app/streaming/session.cpp:1965-1966` - "we want to suspend all Qt
// processing until the stream is over"), so a timer on the Qt main thread would never fire
// during exactly the interval liveness exists to cover. The facade moves this object onto the
// network thread `ControlPlaneClient` owns, where an event loop does run.
//
// Every entry point (`start`, `stop`, `tick`, `setReportedState`, `setErrorCode`) re-invokes
// itself queued when it is called from a thread other than the one that owns this object, so the
// QTimer is only ever started, stopped and read on its own thread. A `QTimer::start()` from a
// foreign thread is refused by Qt with a warning and nothing else, which is exactly the state
// that left one liveness report per session and an unreachable billing horizon (CR-02).
class LivenessTimer : public QObject
{
    Q_OBJECT

    /// One of: "idle" | "streaming" | "reconnecting" | "ending". Reported to the control plane
    /// as the ADR-0041 `state` field.
    Q_PROPERTY(QString reportedState READ reportedState NOTIFY reportedStateChanged)

public:
    /// D-31: the locked report interval.
    static const int kIntervalMs = 10000;
    /// D-31: the server's grace window, matched here so the warning is raised at the same moment
    /// the control plane stops counting this client as live.
    static const int kGraceMs = 30000;

    explicit LivenessTimer(QObject* parent = nullptr);

    QString reportedState() const { return m_reportedState; }
    /// D-11/3.1.0: SeatHub's own stage, one of the eight the contract names, or empty before
    /// `start()` has ever run.
    QString stage() const { return m_stage; }
    bool isRunning() const;
    int consecutiveFailures() const { return m_consecutiveFailures; }
    bool warnedSinceLastSuccess() const { return m_warned; }

    void setControlPlane(ControlPlaneClient* client);
    /// ADR-0041 `state`. "streaming" while the media path is up, "reconnecting" while the
    /// session channel is down, "ending" once teardown has begun. Unchanged since 1.6.0: only
    /// the three documented values are ever sent, and nothing here sets it on `start()` any more
    /// (D-11 moved `start()` to session begin, well before the media path exists) - the caller
    /// sets it exactly where it always did (`handleConnectionStarted`).
    void setReportedState(const QString& state);
    /// ADR-0041 `error_code`: the `SH-XXXXXX` reference of the failure the client just had, or
    /// empty. A malformed value is dropped rather than sent (ADR-0008's alphabet).
    void setErrorCode(const QString& reference);
    /// D-11/3.1.0: SeatHub's own stage - `preparing_rig`, `pairing`, `connecting`,
    /// `first_frame`, `streaming`, `reconnecting`, `ending` or `failed`. A value outside that
    /// list is ignored (never sent). A genuine change reports at once, ahead of the next tick,
    /// so the server sees a stage transition the moment it happens rather than up to
    /// `kIntervalMs` later.
    void setStage(const QString& stage);
    /// D-11: the engine's own stage name (`Limelight.h`'s `LiGetStageName()` family, e.g. "RTSP
    /// handshake"), diagnostic only and never shown (D-51). A free string, no enum: upstream's
    /// own names are "subject to change in future releases".
    void setEngineStage(const QString& engineStage);
    /// D-11: the stage-failure or termination error code. 0 is a real code
    /// (`ML_ERROR_GRACEFUL_TERMINATION`), not "unset" - `hasEngineError()` says whether one has
    /// ever been set on this timer.
    void setEngineError(int code);
    bool hasEngineError() const { return m_hasEngineError; }
    int engineError() const { return m_engineError; }
    /// D-11: the ports a stage failure named (`handleStageFailed`'s own string).
    void setFailingPorts(const QString& failingPorts);
    /// Test seams. The behaviour is identical at any interval; the real one is D-31's 10000.
    void setIntervalMs(int milliseconds);
    void setGraceMs(int milliseconds);

    /// Starts on session start (D-11: at `beginSession`, not at the stream's first frame) and
    /// reports until `stop()`. The first tick reports stage `preparing_rig` with no `state` key
    /// (the state stays whatever it already was - idle unless a caller set it first).
    void start(const QString& sessionId);
    void stop();

    /// D-11: a pre-engine failure with no engine code - a pairing timeout or refusal, or the
    /// `m_session->start()` refusal. Sets stage `failed`, `engine_stage` to `engineStage`, and
    /// sends no `engine_error` and no `failing_ports` (the contract reserves those for a real
    /// platform code). Posts once, synchronously on this timer's own thread, before any pending
    /// `stop()` takes effect.
    void reportFailure(const QString& engineStage);
    /// D-11: a stage failure, carrying the engine's own code and ports
    /// (`handleStageFailed(stage, errorCode, failingPorts)`). Same posting guarantee as the
    /// one-argument overload.
    void reportFailure(const QString& engineStage, int engineError, const QString& failingPorts);

    /// D-11/A-51: the engine's own termination code, as soon as it is known
    /// (`Session::clConnectionTerminated`, relayed through Plan 15's log-tee sink - no engine
    /// signal carries it directly). Sets `engine_error` and stage `ending` in one call and
    /// reports at once, with the same posting guarantee as `reportFailure`.
    void noteTermination(int code);

signals:
    void reportedStateChanged();
    void runningChanged();
    /// A report was accepted.
    void livenessReported();
    /// A report failed; `consecutiveFailures` is a count, not a verdict.
    void livenessFailed(int consecutiveFailures);
    /// The grace window elapsed with no success. NON-FATAL by contract: the stream keeps
    /// running and the timer keeps retrying (D-33).
    void livenessWarning();
    /// The state or error code changed and the next report will carry it.
    void payloadChanged();
    /// This tick's wallet read succeeded: the server's own balance in whole minutes. Emitted on
    /// this object's thread (see the class comment).
    void walletRead(qint64 balanceMinutes);
    /// This tick's wallet read did not produce a balance. Carries nothing on purpose: the caller
    /// keeps what it has.
    void walletReadFailed();

public slots:
    /// One report. Public so the interval and the grace window can be driven deterministically
    /// under test rather than in 10 s of real time.
    void tick();

private:
    void handleResult(const ControlPlaneResult& result);
    void handleWalletResult(const ControlPlaneResult& result);
    /// True when the calling thread is the one this object lives on (or when it lives on no
    /// thread at all, which only happens after its thread has been destroyed).
    bool onOwnThread() const;
    /// The current payload: `stage` (when set), `state` (when it is one of the three documented
    /// values), `error_code` (when valid), `engine_stage`, `engine_error` and `failing_ports`
    /// (each only when set). The one place that decides what a report carries.
    QJsonObject buildPayload() const;
    /// Posts `buildPayload()` at once, on this timer's own thread, with no wallet read. Used by
    /// `setStage`, `reportFailure` and `noteTermination` for their "posts at once" guarantee;
    /// `tick()` is the interval-driven path and reads the wallet alongside it. A no-op while no
    /// session is attached (`m_sessionId` empty), the same guard `tick()` already uses.
    void reportNow();

    ControlPlaneClient* m_client = nullptr;
    QTimer* m_timer = nullptr;

    QString m_sessionId;
    QString m_reportedState;
    QString m_errorCode;
    /// D-11/3.1.0: SeatHub's own stage. Set to `preparing_rig` by `start()`; empty only before
    /// the first `start()`.
    QString m_stage;
    QString m_engineStage;
    int m_engineError = 0;
    bool m_hasEngineError = false;
    QString m_failingPorts;
    int m_consecutiveFailures = 0;
    bool m_warned = false;
    int m_intervalMs = kIntervalMs;
    int m_graceMs = kGraceMs;
};
