#pragma once

// The production `PairingSeam` (D-21, D-22, D-08, STREAM-03).
//
// 03-03 shipped the interface and no implementation, so `SeatHubClient::beginSession()`
// authorized and stopped. This is the implementation: it takes the host address the control plane
// put in the session authorization and the PIN it issued, runs upstream's own pairing handshake,
// and reports the outcome exactly once.
//
// Three properties this class exists to hold:
//
//   * The PIN never leaves the handshake. It arrives in `PairingTarget`, is copied into the
//     worker that runs the handshake, and is destroyed with it. It is not a property, it is on no
//     signal, it is returned by no accessor, and it never appears in a diagnostic string
//     (STREAM-03, D-30).
//   * The handshake is bounded. Upstream issues its first pairing request with no client-side
//     timeout at all (`app/backend/nvpairingmanager.cpp:217-221` passes `0` for that request's
//     timeout, and `app/backend/nvhttp.cpp:510-511` only arms a timer when it is non-zero), so
//     Sunshine holds that request open until the Node Agent submits the PIN or the pending
//     session expires. Without a deadline here the customer watches the connecting view forever.
//     D-08's 90 s horizon is that deadline, measured from `pair()`.
//   * Nothing is persisted. The pinned server certificate and the pairing target live in memory
//     for the duration of the handshake and are released with it: "the client keeps no stored
//     rig, address or pairing of its own; every session starts fresh" (STREAM-10).
//
// The handshake itself is injected rather than called directly, for the same reason the seam is an
// interface in the first place: the deadline, the fail-closed branches and the exactly-once
// contract are assertable with no host, no Sunshine and no engine. `pairing_handshake.cpp` is the
// production handshake and, with `moonlight_engine_session.cpp`, one of the only two files that
// include `app/backend/`.

#include <QByteArray>
#include <QElapsedTimer>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>

#include "engine_session.h"
#include "pairing_controller.h"

/// One production pairing handshake's outcome.
struct PairingHandshakeResult
{
    /// True only when upstream reported `NvPairingManager::PAIRED`.
    bool ok = false;
    /// The client identity reported on success - `clientCertificateFingerprint()` of this
    /// client's own certificate. Empty on every failure path (fail closed).
    QString clientIdentity;
    /// The host this handshake paired with, or null on every failure path.
    ///
    /// This exists because the engine has to stream from *the same host the handshake paired with*:
    /// upstream's `NvComputer` carries the pinned certificate the handshake produced, plus the
    /// HTTPS port and app version the host reported. A `NvComputer` local to the handshake is
    /// destroyed when the handshake returns, and a second one built afterwards pins nothing - which
    /// is what left the engine with no host to stream from before Plan 03-06's gap closure.
    PairedHostPtr host;
    /// Diagnostics only: raw upstream text for the log and for support. Never rendered (D-51),
    /// and never contains the PIN.
    QString engineError;
    /// True when the attempt hit the rig's already-pairing surface: the HTTP 409 the rig answers
    /// the getservercert step with when a half-open pending session holds the shared uniqueid
    /// (Sunshine `src/nvhttp.cpp:975-997`), or `NvPairingManager::ALREADY_IN_PROGRESS` from the
    /// handshake's own five-phase flow. Recovery plumbing for the seam worker's clear-and-retry
    /// loop (G-06.2-2, Plan 06.2-12 Task 1) - never rendered, never logged, never leaves this
    /// process on a signal (D-51).
    bool pairingConflict = false;
    /// The frozen attempt step this failure belongs to (ADR-0072 item 1, `attempt_vocab.h`):
    /// `pair_server_info` for anything before `pair()`, `pair_handshake` for the five-phase
    /// handshake and its recovery. Empty until the handshake classifies it; never derived from
    /// `engineError`.
    QString attemptStep;
    /// The closed failure class for this step (ADR-0072 item 2): one vocabulary token computed
    /// from an enum integer or an HTTP bucket only. Empty until classified. This is a token for
    /// Sentry - `engineError` stays the local diagnostic and never enters a telemetry field.
    QString stepClass;
    /// How many tries of that step this Play made (ADR-0072 item 2): 1 normally, 2 after the
    /// G-06.2-2 clear-and-retry.
    int attempts = 1;
    /// True when this result came from the warm return (the PIN handshake was skipped), false for
    /// every PIN handshake - including the one a failed warm attempt falls back to (L07). Lets the
    /// caller tell a reconnect attempt that streamed on held trust from one that streamed on a
    /// fresh pairing; it carries no credential.
    bool warm = false;

    // SC18 (INTENT N18 amendment 2026-10-17): the one logged retry. The fields above describe the
    // FINAL attempt; the fields below keep the FIRST attempt on the record. None carries the PIN,
    // the salt or any hash of either.
    /// `success` or `failure`: the first handshake attempt's own outcome. Set by the worker on
    /// every result (and by the deadline for a result it speaks for); empty only on a
    /// default-constructed value.
    QString firstOutcome;
    /// The first attempt's frozen step and closed failure class, only when it failed and the
    /// token is nonempty. A failure-retry success keeps these as its `attemptStep`/`stepClass`.
    QString firstAttemptStep;
    QString firstStepClass;
    /// True when the worker made the Play's one automatic retry.
    bool retried = false;
    /// `conflict` or `failure` (`PairingRetryBudget::token`), only when `retried`.
    QString retryReason;
    /// True when the target's PIN is exactly four ASCII digits. A shape, not the PIN.
    bool pinShapeOk = false;
    /// Milliseconds from `pair()` to this result, every attempt included.
    qint64 handshakeMs = 0;
};

// Declared for the same reason as `SeatHubFailure` and `TeardownStage`: the handshake result
// crosses the thread-pool boundary as a queued connection, which needs a registered metatype.
Q_DECLARE_METATYPE(PairingHandshakeResult)

/// What one `pair()` call's worker has settled so far (T074): the first attempt's closed outcome and
/// the actual retry decision, published BEFORE the retry blocks so a deadline that speaks for the
/// blocked worker still reports them. One object per `pair()` call, shared by that call's worker and
/// the seam, so it is generation-bound by construction. Holds no PIN, salt or hash. Defined in
/// `pairing_seam.cpp`.
struct PairingProgress;

/// The SHA-256 fingerprint of a certificate in PEM form, lowercase hex, or empty when the PEM is
/// empty or cannot be parsed.
///
/// This is the client-side identity, and it is deliberately not "the Sunshine client UUID":
/// Sunshine mints that UUID server-side and returns it to no one (see `pairing_seam.cpp`). What
/// the host does key on is the certificate: a pairing record is
/// `{name, uuid, cert, enabled}` (Sunshine `src/nvhttp.cpp:171-176`) and TLS admission compares
/// the presented certificate with the stored PEM byte for byte (`is_client_enabled`,
/// Sunshine `src/nvhttp.cpp:334-347`). The fingerprint is therefore the only client-side value
/// that a host-side reader of `GET /api/clients/list` can match to this client's record.
QString clientCertificateFingerprint(const QByteArray& pem);

/// The host certificate this app run itself pinned, held in memory for the warm return
/// (ADR-0074 W03, `docs/spec/client.md` "Warm return"). The app stores no pairing of its own
/// (STREAM-10) and persists nothing for this: the bytes live in this process and nowhere else, so a
/// restart, a crash or a kill leaves nothing to reuse and the next Play pairs normally. It is
/// cleared at sign-out, and replaced by every successful pairing.
///
/// This is the certificate of the RIG, which upstream's pairing pins to authenticate the host's
/// HTTPS server. It is not a credential for anything the rig would admit by itself: the rig's own
/// check of the client certificate is what admits a stream.
class HeldRigTrust
{
public:
    /// Remembers the PEM the pairing handshake pinned. An empty value forgets what was held.
    static void remember(const QByteArray& hostCertificatePem);
    /// The held PEM, or empty when this run has pinned none (or it was cleared).
    static QByteArray held();
    static void clear();
};

/// The warm-return check: with the host certificate this run pinned earlier (`heldHostCertificatePem`),
/// asks the rig's HTTPS `serverinfo` - read with that pin - whether this client is still paired, and
/// answers like a handshake does: `ok` with the client identity and the host record on a yes, `ok`
/// false on every other outcome (a refused pin, an unreachable rig, an answer of not paired). Blocks
/// on a worker thread, never throws, and must not call the control plane. The production one is
/// `runUpstreamWarmReturnCheck` (`pairing_handshake.h`).
using WarmReturnCheck =
    std::function<PairingHandshakeResult(const PairingTarget&, const QByteArray& heldHostCertificatePem)>;

/// Runs one pairing handshake against `target.hostAddress` with `target.pairingPin`.
///
/// Blocks - it is called on a worker thread - and reports what upstream reported. It must not
/// touch any UI, and it must not call the control plane.
using PairingHandshake = std::function<PairingHandshakeResult(const PairingTarget&)>;

/// One best-effort clear of the rig's pending pairing session for `hostAddress`. Blocking on the
/// calling thread, bounded, best-effort: the production default is `sendPairingCancelRequest`
/// (`pairing_recovery.h`); tests inject a counting fake. `port` is the plain-HTTP control port.
using PairingCancelRequest = std::function<bool(const QString& hostAddress, int port)>;

class ProductionPairingSeam : public QObject, public PairingSeam
{
    Q_OBJECT

public:
    /// Test seam: the deadline in milliseconds. Defaults to D-08's locked 90 s horizon, the same
    /// value `PairingController::kDeadlineMs` holds - measured here from `pair()` rather than from
    /// `start()`, because the interval this deadline has to cover is the handshake, which the
    /// controller's own window cannot bound (it is not running during the blocking request).
    static const int kDeadlineMs = 90000;

    explicit ProductionPairingSeam(QObject* parent = nullptr);
    ~ProductionPairingSeam() override;

    /// The handshake to run. Without one, every `pair()` fails closed: reporting success without
    /// having paired would tell the caller a rig was paired when nothing was.
    void setHandshake(PairingHandshake handshake);
    /// The warm-return check (ADR-0074 W03, L07). Without one, a target that carries `warmReturn`
    /// pairs normally, exactly like one that does not.
    ///
    /// The PIN handshake is skipped only when ALL of these hold: the target's `warmReturn` is true
    /// (the control plane said so - for a NEW session on the caller's own free return hold, or for
    /// an `ACTIVE` session reconnecting inside its disconnect grace), this app run holds the host
    /// certificate it pinned earlier (`HeldRigTrust`), and the check answers `ok`. In every other
    /// case - the field absent or false, nothing held (a restart, a sign-out), a check that fails
    /// or is refused - the normal PIN handshake runs, with its own recovery, exactly as before. A
    /// PIN is always issued, so a normal pairing is always possible, and the warm check and the
    /// handshake that follows it run one after the other on one worker: never two at once.
    void setWarmReturnCheck(WarmReturnCheck check);
    /// Test seam. Same clock either way.
    void setDeadlineMs(int milliseconds);
    /// Test seam: the clear request the recovery loop (and `clearPendingPairing()`) runs.
    /// Defaults to `sendPairingCancelRequest` (`pairing_recovery.h`).
    void setCancelRequest(PairingCancelRequest cancel);
    /// T063: a narrow, read-only sink called once per reported result, on this object's thread, right
    /// after the `pairing_instrumentation` line - the facade journals the attempt from it. T093: also
    /// once for a Play cancelled after its first attempt failed (no instrumentation line). It takes
    /// the result by const reference and cannot change the seam's retry, guards or callbacks.
    using AttemptSink = std::function<void(const PairingHandshakeResult& result)>;
    void setAttemptSink(AttemptSink sink);

    /// Start one handshake. `done` is called exactly once, on this object's thread, with
    /// `(ok, clientIdentity, engineError)`. See `pairing_controller.h` for what the identity is
    /// and is not.
    void pair(const PairingTarget& target,
              std::function<void(bool ok, const QString& clientUuid,
                                 const QString& engineError)> done) override;

    /// Best-effort clear of the rig's pending pairing session the target's handshake may have
    /// left half-open (G-06.2-2): one `sendPairingCancelRequest`, fire-and-forget on the pool
    /// thread, so a cancel() on the UI thread never waits on the rig. Never called from
    /// `pair()`'s own path - the worker's recovery loop clears inline, where completion is
    /// sequenced before the retry.
    ///
    /// The clear is fenced to the attempt that asked for it (PF15-b): if a newer `pair()` has
    /// started by the time the pool thread runs it, it is dropped, because the rig has one pending
    /// pairing slot and the older attempt's clear would cancel the newer attempt's pairing. A
    /// handshake of `target`'s session that is still pending is abandoned by the same call: no
    /// result, classification or host is ever reported for it, and its worker neither clears nor
    /// retries. If its first attempt had already failed, the attempt sink (below) gets one
    /// neutral record of that failure and the retry, and nothing else does.
    void clearPendingPairing(const PairingTarget& target) override;

    /// The classification this seam last reported, or a default-constructed result before any
    /// result was reported. The failure path the facade reads: `handlePairingFailed()` is
    /// reached through `pairingFailed`, which carries no classification of its own (the
    /// controller's callback and signal signatures are not this plan's to change), so the step
    /// and the class the facade needs are read from here and from `handshakeClassified`.
    const PairingHandshakeResult& lastResult() const { return m_lastResult; }

signals:
    /// The result this seam just reported, with whatever classification it carries, handed to the
    /// facade as one value whose `attemptStep`, `stepClass` and `attempts` are separate members -
    /// never `engineError` (ADR-0072 item 2, D-12). Emitted immediately BEFORE `done()` on every
    /// path that reports a result - a failure, and also a pairing that succeeded after the
    /// clear-and-retry, whose frozen record (`pair_handshake` / `in_progress` / attempt 2) the
    /// facade turns into its ONE INFO `play.step` (ADR-0072 item 2). Because it always leads the
    /// result, a queued connection to the facade always lands before the controller's own
    /// `pairingFailed` / `pairingCompleted` for the same run, and before `hostResolved`.
    void handshakeClassified(const PairingHandshakeResult& result);

    /// The host the handshake resolved, tagged with the session id of the `pair()` call it answers
    /// (06.6-18/T-06.6-52), emitted immediately before `done(true, ...)` on the one success path and
    /// never on a failure.
    ///
    /// The tag is what lets a listener refuse a host that resolved for a session that is no longer
    /// attached - cancelled, or superseded by a fresh Play or Try again - rather than attaching an
    /// engine (and starting a stream) for the wrong session.
    ///
    /// A signal rather than a getter because the two ends live on different threads: this object is
    /// moved to the network thread (`SeatHubClient::startNetworkThreads()`) while the object that
    /// builds the engine session belongs to the Qt main thread. `PairedHostPtr` is a
    /// `shared_ptr`, so the queued connection that carries it keeps the record alive on both sides
    /// until both are done with it.
    void hostResolved(const QString& sessionId, const PairedHostPtr& host);

private slots:
    void handleHandshakeResult(quint64 generation, const PairingHandshakeResult& result);

private:
    void finish(quint64 generation, const PairingHandshakeResult& result);

    PairingHandshake m_handshake;
    WarmReturnCheck m_warmCheck;
    PairingCancelRequest m_cancelRequest;
    AttemptSink m_attemptSink;
    std::function<void(bool, const QString&, const QString&)> m_done;
    /// The generation token the pool thread reads without touching this object: bumped and
    /// stored here by every `pair()` call, captured by the worker as a `shared_ptr` copy, and
    /// consulted by the worker's recovery loop (a superseded run sends no clear and no retry -
    /// G-06.2-2). An atomic the worker outlives the object with, never a raw `this` (the plan's
    /// T-06.2-12-02 mitigation for the loop-spin and lifetime hazards).
    std::shared_ptr<std::atomic<quint64>> m_currentGeneration;
    /// The current `pair()` call's progress (T074): replaced by every `pair()`, so the deadline and
    /// `finish()` never read a superseded generation's record.
    std::shared_ptr<PairingProgress> m_progress;
    QTimer* m_deadline = nullptr;
    int m_deadlineMs = kDeadlineMs;
    /// Started by `pair()` beside the deadline: `handshakeMs` and the retry's remaining budget.
    QElapsedTimer m_clock;
    /// `PairingRetryBudget::pinShapeOk` of the current `pair()` target, so the deadline can speak
    /// for a handshake whose worker has not answered. A bool; the PIN itself is never kept here.
    bool m_pinShapeOk = false;
    bool m_pending = false;
    /// The session id of the `pair()` call this object is currently running or last ran
    /// (06.6-18/T-06.6-52). Set at the top of `pair()`, read back in `finish()` to tag `hostResolved`.
    QString m_sessionId;
    /// Bumped by every `pair()` call (06.6-18/T-06.6-53). A handshake's completion carries the
    /// generation it started with; `finish()` drops one whose generation no longer matches
    /// `m_generation` - it belongs to a handshake a later `pair()` call has since superseded, and
    /// upstream's own blocking call cannot be aborted, so the old one is left to simply run out on
    /// its pool thread. The deadline timer needs no generation of its own: `pair()` restarts the
    /// same `QTimer`, so it is always counting down for whichever handshake is current.
    quint64 m_generation = 0;
    /// What the last reported failure carried (ADR-0072, Plan 09): written on the same thread
    /// that reports it, read by the facade after the queued `pairingFailed` for that run has
    /// already delivered the `handshakeClassified` copy.
    PairingHandshakeResult m_lastResult;
};
