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
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

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
};

// Declared for the same reason as `SeatHubFailure` and `TeardownStage`: the handshake result
// crosses the thread-pool boundary as a queued connection, which needs a registered metatype.
Q_DECLARE_METATYPE(PairingHandshakeResult)

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

/// Runs one pairing handshake against `target.hostAddress` with `target.pairingPin`.
///
/// Blocks - it is called on a worker thread - and reports what upstream reported. It must not
/// touch any UI, and it must not call the control plane.
using PairingHandshake = std::function<PairingHandshakeResult(const PairingTarget&)>;

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
    /// Test seam. Same clock either way.
    void setDeadlineMs(int milliseconds);

    /// Start one handshake. `done` is called exactly once, on this object's thread, with
    /// `(ok, clientIdentity, engineError)`. See `pairing_controller.h` for what the identity is
    /// and is not.
    void pair(const PairingTarget& target,
              std::function<void(bool ok, const QString& clientUuid,
                                 const QString& engineError)> done) override;

signals:
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
    void handleHandshakeResult(const PairingHandshakeResult& result);

private:
    void finish(const PairingHandshakeResult& result);

    PairingHandshake m_handshake;
    std::function<void(bool, const QString&, const QString&)> m_done;
    QTimer* m_deadline = nullptr;
    int m_deadlineMs = kDeadlineMs;
    bool m_pending = false;
    /// The session id of the `pair()` call this object is currently running or last ran
    /// (06.6-18/T-06.6-52). Set at the top of `pair()`, read back in `finish()` to tag `hostResolved`.
    QString m_sessionId;
};
