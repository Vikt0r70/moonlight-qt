#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

#include "control_plane_client.h"
#include "error_map.h"

class ControlPlaneClient;

// What the engine's pairing call needs, and nothing more.
//
// `pairingPin` is the control-plane-issued PIN for this launch (ADR-0034). It travels from the
// control plane to the engine seam and nowhere else: it is not a Q_PROPERTY, it is not emitted
// on a signal, and no view can reach it. That is STREAM-03's whole requirement - "Customer
// connects to a rig without typing a PIN" - and the strongest form of it is that the client has
// no PIN surface at all.
struct PairingTarget {
    QString sessionId;
    QString hostAddress;
    int httpsPort = 0;
    /// Dies at the end of the launch. Never persisted (D-30, D-37).
    QString pairingPin;
};

// The engine's pairing entry point, as this controller needs it.
//
// It is an interface rather than a call into `app/backend/` so the controller can be tested with
// no engine, no host and no Sunshine: the protocol, the deadline and the fail-closed rules are
// what this plan delivers, and all three are assertable against a fake.
//
// Upstream owns the cryptography. `03-RESEARCH.md` §Pairing sequence step 3: "reuse upstream
// Moonlight pairing crypto; do not reimplement salt/certificate/AES handshake logic in SeatHub".
//
// The production implementation is `ProductionPairingSeam` (`app/seathub/pairing_seam.h`), which
// drives upstream's `NvPairingManager` against the address the control plane supplied
// (`app/seathub/pairing_handshake.cpp`). 03-03 expected this interface to be filled by
// `ComputerManager::pairHost()`, and it cannot be: that path reports its result as
// `pairingCompleted(NvComputer*, QString error)` (`app/backend/computermanager.h:251`) with no
// client identity anywhere in the signal, so it cannot satisfy the fail-closed rule the
// contract below is built on. Both `pairHost` and `NvPairingManager` are public upstream API -
// no upstream file is edited either way.
class PairingSeam
{
public:
    virtual ~PairingSeam() = default;

    /// Start upstream's pairing handshake against `target.hostAddress` using `target.pairingPin`.
    ///
    /// Calls `done` exactly once: `ok` true with the identity the client reports about itself -
    /// see `pairing_seam.h` for why that is this client's certificate fingerprint and not the
    /// Sunshine-assigned UUID, which no route available to this process returns - or `ok` false
    /// with the engine's own failure text, which is diagnostic only and is never rendered (D-51).
    virtual void pair(const PairingTarget& target,
                      std::function<void(bool ok, const QString& clientUuid,
                                         const QString& engineError)> done) = 0;

    /// Best-effort clear of the rig's pending pairing session the target's handshake may have
    /// left half-open (G-06.2-2). Called by `PairingController::cancel()` for the last target a
    /// handshake was started for, and by nothing else: the in-handshake recovery path clears
    /// inside the seam worker, where the clear is sequenced before the one retry. Fire-and-forget
    /// from the caller's point of view - a cancel() on the UI thread never waits on the rig.
    virtual void clearPendingPairing(const PairingTarget& target) = 0;
};

// The client half of silent pairing (D-21, D-22, D-08, STREAM-03).
//
// The division of labour, which is the part that is easy to get wrong:
//
//   * The control plane issues the session-scoped PIN (`GET /api/sessions/{id}/pairing` ->
//     `SessionAuthorization.pairing_pin`, ADR-0034). This client asks for it.
//   * The Node Agent independently polls Sunshine's `GET /api/pin` every 250 ms and submits
//     `POST /api/pin` (D-08). Those are Sunshine admin routes and this client never calls them
//     (`COVERAGE.md`; `03-RESEARCH.md` §Teardown order makes the same point for `clients/*`).
//   * This client hands the same PIN to upstream's pairing flow and waits for the handshake.
//     The two halves meet at the PIN.
//
// The deadline is 30 s (06.1's ADR item R1), counted from `start()` or from the last waiting
// answer - a 409, or a 200 whose `state` is REQUESTED, ALLOCATED or PREPARING (J-16) -
// whichever is later: the rig's preparation is bounded by the server's own readiness deadline,
// not by this one (2026-09-23). This clock now bounds only a pairing resolving once the rig is
// READY; the server's own CONNECT lease deadline (60 s from READY) is sized around it, giving a
// pairing that starts the moment READY arrives room to finish well inside the lease. At expiry
// the controller fails closed with a SeatHub error, which the error screen renders with a
// reason, a retry and an `SH-` reference - never a Moonlight dialog (ADR-0008, D-51).
class PairingController : public QObject
{
    Q_OBJECT

    /// One of: "idle" | "authorizing" | "pairing" | "ready" | "failed".
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)

public:
    /// J-16: the client's authorization poll backs off from D-08's original 250 ms to 1 s -
    /// waiting is no longer urgent now that the rig-preparing wait arrives as its own 200
    /// answer (`kWaitingStates`, `pairing_controller.cpp`) rather than sharing this clock.
    static const int kPollIntervalMs = 1000;
    /// 06.1's ADR item R1: 30 s, counted from READY, for a pairing to resolve - replacing D-08's
    /// 90 s now that the rig getting ready no longer spends this clock at all (see the class
    /// comment above).
    static const int kDeadlineMs = 30000;

    explicit PairingController(QObject* parent = nullptr);

    QString state() const { return m_state; }
    QString sessionId() const { return m_sessionId; }
    /// Test accessor: what a fresh controller actually holds, before any override below runs.
    int pollIntervalMs() const { return m_pollIntervalMs; }
    /// Test accessor: see `pollIntervalMs()`.
    int deadlineMs() const { return m_deadlineMs; }

    void setControlPlane(ControlPlaneClient* client);
    void setSeam(PairingSeam* seam);
    /// Test seam. The deadline is measured from `start()` with the same clock either way.
    void setPollIntervalMs(int milliseconds);
    void setDeadlineMs(int milliseconds);

    /// Begin the silent pairing for `sessionId`. No prompt, no dialog, no PIN on screen.
    Q_INVOKABLE void start(const QString& sessionId);
    /// Stop polling and forget the session. Called from teardown and from `signOut()`.
    Q_INVOKABLE void cancel();
    /// 06.4/ADR-0067: runs the next authorization read at once rather than waiting out
    /// `pollIntervalMs()` - the account push channel's own `session.state` frame wakes this so
    /// connecting speeds up on push. A no-op while idle (no session) or once pairing has already
    /// resolved (`pollAuthorization()`'s own guard). Restarts the interval timer: the answer's own
    /// `handleAuthorization()` reschedules the next poll from here, exactly as an ordinary tick
    /// would, so this cannot make the poll fire twice in quick succession.
    Q_INVOKABLE void pollNow();

    /// True once the deadline has passed without a result.
    bool deadlineExceeded() const;

signals:
    void stateChanged();
    /// The control plane granted the session. Carries no argument: the authorization's own
    /// `quality_profile` (`ADR-0011`) is parsed but nothing reads it any more (A-68, D-06
    /// reversal - the customer's saved Settings are the only decider of stream quality). Nothing
    /// from `SessionAuthorization` leaves this class - least of all `pairing_pin`, which is on no
    /// signal, in no property and behind no accessor (STREAM-03).
    void authorizationGranted();
    /// Pairing finished; `clientUuid` is the identity this client reports about itself - the
    /// SHA-256 of its own certificate, the value a host-side reader of Sunshine's client list can
    /// match to this client's pairing record. It is the only thing that ever identifies this
    /// client (Pitfall 3, D-07); see `pairing_seam.h` for the full statement.
    void pairingCompleted(const QString& clientUuid);
    /// Pairing failed closed. Always a SeatHub failure, never engine text in `error`.
    void pairingFailed(const SeatHubFailure& failure);
    /// Closed attempt classification for authorization failures; never carries response text.
    void stepFailed(const QString& step, const QString& failureClass, qint64 elapsedMs);
    /// The session exactly as `GET /api/sessions/{id}` reported it on one poll tick. It rides the
    /// tick this controller already runs - the same interval, no timer of its own - and is what the
    /// connecting stages are read from (CUST-12, `ADR-0055`). A read that failed, or that arrived
    /// after pairing finished or for another session, is dropped: a missing answer is not a state.
    void sessionRead(const SessionInfo& session);

public slots:
    /// One authorization poll. Public so the deadline and the fail-closed rules can be driven
    /// deterministically under test instead of in 250 ms real time.
    void pollAuthorization();

private slots:
    void handleAuthorization(const ControlPlaneResult& result);
    void handleSessionRead(const QString& polledSession, const ControlPlaneResult& result);
    void handleSeamResult(bool ok, const QString& clientUuid, const QString& engineError);

private:
    void setState(const QString& state);
    void fail(const SeatHubFailure& failure, const QString& attemptFailureClass = QString());
    void scheduleNextPoll();

    ControlPlaneClient* m_client = nullptr;
    PairingSeam* m_seam = nullptr;

    QString m_state;
    QString m_sessionId;
    QString m_clientUuid;

    QElapsedTimer m_clock;
    QTimer* m_pollTimer = nullptr;

    int m_pollIntervalMs = kPollIntervalMs;
    int m_deadlineMs = kDeadlineMs;
    int m_polls = 0;
    int m_conflictPolls = 0;
    bool m_finished = false;
    bool m_transportFailureSeen = false;

    // G-06.2-2 state (Plan 06.2-12 Task 1).
    //
    // Single-flight: a stray poll tick or a push-driven pollNow() while this session's handshake
    // is still running must not start a second handshake for the same session - the customer's
    // rig saw three back-to-back getservercert sequences in one second, two of them 409s, because
    // nothing stopped the re-fire. `m_inFlightSessionId` tags which session the in-flight
    // handshake belongs to: a fresh Play for a DIFFERENT session still supersedes through the
    // seam's own generation gate, and is not blocked here.
    bool m_handshakeInFlight = false;
    QString m_inFlightSessionId;
    // Teardown recovery: the last target a handshake was actually started for, so cancel() can
    // ask the seam for the rig-side clear. The PIN inside dies with this controller, as it always
    // did (D-30, D-37) - it is copied into no log, no signal and no property.
    PairingTarget m_lastTarget;
    bool m_handshakeStarted = false;
};
