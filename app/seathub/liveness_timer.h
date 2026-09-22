#pragma once

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
    bool isRunning() const;
    int consecutiveFailures() const { return m_consecutiveFailures; }
    bool warnedSinceLastSuccess() const { return m_warned; }

    void setControlPlane(ControlPlaneClient* client);
    /// ADR-0041 `state`. "streaming" while the media path is up, "reconnecting" while the
    /// session channel is down, "ending" once teardown has begun.
    void setReportedState(const QString& state);
    /// ADR-0041 `error_code`: the `SH-XXXXXX` reference of the failure the client just had, or
    /// empty. A malformed value is dropped rather than sent (ADR-0008's alphabet).
    void setErrorCode(const QString& reference);
    /// Test seams. The behaviour is identical at any interval; the real one is D-31's 10000.
    void setIntervalMs(int milliseconds);
    void setGraceMs(int milliseconds);

    /// Starts on session start and reports until `stop()`.
    void start(const QString& sessionId);
    void stop();

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

    ControlPlaneClient* m_client = nullptr;
    QTimer* m_timer = nullptr;

    QString m_sessionId;
    QString m_reportedState;
    QString m_errorCode;
    int m_consecutiveFailures = 0;
    bool m_warned = false;
    int m_intervalMs = kIntervalMs;
    int m_graceMs = kGraceMs;
};
