#pragma once

#include <QElapsedTimer>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QTimer>

#include "control_plane_client.h"
#include "error_map.h"

class ControlPlaneClient;

/// Teardown's stages, in the order D-10 and STREAM-10 require.
///
/// The order is the safety property, so it is an enum the code advances through rather than a
/// sequence of statements a later edit can reorder without noticing. `isOrdered()` is the rule
/// `advanceTo()` enforces.
enum class TeardownStage {
    Idle = 0,
    /// Step 1: the client is disabled on the rig - a live stream must stop before its
    /// certificate is removed (Pitfall 5).
    Disable = 1,
    /// Step 2: the pairing record is removed from the rig.
    Unpair = 2,
    /// Step 3: the removal is verified.
    Verify = 3,
    /// Step 4: every local artifact is gone (STREAM-10).
    Clear = 4,
    Done = 5,
    Failed = 6,
};

// Declared for the same reason as `SeatHubFailure`: `stageEntered` crosses the network-thread
// boundary as a queued connection, which needs a registered metatype.
Q_DECLARE_METATYPE(TeardownStage)

// The client half of teardown: disable, then remove, then verify, then leave nothing behind.
//
// STREAM-10's second half is the part this client can be held to directly - "the client keeps no
// stored rig, address or pairing of its own; every session starts fresh and the only record
// lives on the server" - so teardown does not report success while anything local remains.
//
// The first half (`disable`, then `remove`, then verify the removal) happens on the rig. This
// client holds no Sunshine admin credential and must never call a Sunshine admin route
// (`03-RESEARCH.md` §Teardown order), so it does not perform those steps. What it does is wait
// for the control plane to report the result, because a session reaches a terminal state only
// after the rig side has finished - a teardown still `ENDING` past `TEARDOWN_GRACE_SECONDS`
// (30 s, `docs/spec/timing.md`) is `TEARDOWN_TIMEOUT` rather than a normal end.
//
// Threading (HR-01): `m_verifyTimer` is a child of this object, so this object must live on the
// thread that runs the polling - the network thread `ControlPlaneClient` owns, because every
// control-plane callback arrives there and because upstream suspends Qt processing on the main
// thread for the whole stream (`app/streaming/session.cpp:1965-1966`). The facade therefore moves
// it there before any session starts, and `teardown()`, `cancel()` and `verifySession()` hand
// themselves over queued when they are called from anywhere else - a `QTimer` started from a
// foreign thread is refused by Qt with a warning and nothing else, which is what used to stop
// teardown before `Clear` and `teardownCompleted()` was never emitted.
//
// Teardown does not touch the customer's stored sign-in credential: that is removed by sign-out
// alone (CUST-08, Phase 5 plan 02). STREAM-10's "keeps no stored rig, address or pairing" holds
// because the client never writes any of the three to disk.
class TeardownController : public QObject
{
    Q_OBJECT

    /// One of: "idle" | "disabling" | "unpairing" | "verifying" | "clearing" | "done" | "failed".
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)

public:
    /// `TEARDOWN_GRACE_SECONDS` (`docs/spec/timing.md`, D-13/D-16): how long a session may stay
    /// `ENDING` before the rig-side teardown is treated as failed.
    static const int kTeardownGraceMs = 30000;
    /// How often the control plane is asked whether the session is over yet.
    static const int kVerifyIntervalMs = 500;

    explicit TeardownController(QObject* parent = nullptr);

    QString state() const { return m_state; }
    TeardownStage stage() const { return m_stage; }

    void setControlPlane(ControlPlaneClient* client);
    /// Test seam: the grace window is measured with the same clock either way.
    void setTeardownGraceMs(int milliseconds);
    void setVerifyIntervalMs(int milliseconds);

    /// The order rule, as a predicate. False for any move to an earlier or equal stage, which is
    /// what makes "disable before remove" something code can be checked against rather than
    /// something a comment asks for.
    static bool isOrdered(TeardownStage from, TeardownStage to);

    /// Run the client half of teardown for `sessionId`. `clientUuid` is the exact Sunshine
    /// client UUID pairing returned; it is not used to address Sunshine, only to identify which
    /// pairing this teardown is about in the log and in the session record.
    Q_INVOKABLE void teardown(const QString& sessionId, const QString& clientUuid);
    /// Abandon a teardown in flight (a second End press, or sign-out).
    Q_INVOKABLE void cancel();

signals:
    void stateChanged();
    /// The session reached a terminal state and nothing local remains. `finalSession` is the terminal
    /// read the teardown ended on: the session's own `end_reason` and `minutes_billed`, which is where
    /// Home's end-reason line comes from (CUST-15) now that no socket carries them.
    void teardownCompleted(const SessionInfo& finalSession);
    /// Teardown failed. Always a SeatHub failure; a rig-side timeout carries
    /// `failure = "TEARDOWN_TIMEOUT"` so support can tell it apart from a transport failure.
    void teardownFailed(const SeatHubFailure& failure);
    /// Emitted once per stage entry, so the order is observable in a log and by a test.
    void stageEntered(TeardownStage stage);

public slots:
    /// One verification poll. Public so the grace window can be driven deterministically in a
    /// test rather than by waiting 30 s.
    void verifySession();

private slots:
    void handleEndResult(const ControlPlaneResult& result);
    void handleVerifyResult(const ControlPlaneResult& result);

private:
    void setState(const QString& state);
    /// Every stage transition goes through here, which is where the order rule is enforced.
    bool advanceTo(TeardownStage stage);
    void fail(const SeatHubFailure& failure, const QString& failureCode = QString());
    void scheduleVerify();
    /// True when the calling thread is the one this object lives on (or when it lives on no
    /// thread at all, which only happens after its thread has been destroyed).
    bool onOwnThread() const;

    ControlPlaneClient* m_client = nullptr;

    QString m_state;
    TeardownStage m_stage = TeardownStage::Idle;
    QString m_sessionId;
    QString m_clientUuid;

    QElapsedTimer m_clock;
    QTimer* m_verifyTimer = nullptr;

    int m_teardownGraceMs = kTeardownGraceMs;
    int m_verifyIntervalMs = kVerifyIntervalMs;
    bool m_finished = false;
    bool m_cleared = false;
};
