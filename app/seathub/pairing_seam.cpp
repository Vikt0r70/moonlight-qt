#include "pairing_seam.h"

#include <QCryptographicHash>
#include <QLoggingCategory>
#include <QRunnable>
#include <QSslCertificate>
#include <QThreadPool>

#include <atomic>
#include <memory>

#include "pairing_recovery.h"

Q_LOGGING_CATEGORY(seathubPairingSeam, "seathub.pairing.seam")

namespace {

/// The blocking half: upstream's handshake runs here, never on the caller's thread.
///
/// A pool thread - and not `QtConcurrent::run` - because this is exactly the shape upstream
/// already ships for the same job: `ComputerManager::pairHost()` hands a `QRunnable` to
/// `QThreadPool::globalInstance()` (`app/backend/computermanager.cpp:636-642`) and the result
/// comes back as a signal. The pool deletes this task when `run()` returns, which is safe with a
/// queued emission: the same precedent, in the same repository, with the same Qt version.
///
/// Why a worker at all: every upstream pairing request blocks on a nested `QEventLoop`
/// (`app/backend/nvhttp.cpp:510-511` arms the escape timer), and the first one blocks with no
/// timeout until the host side answers (`app/backend/nvpairingmanager.cpp:217-221`).
/// `ControlPlaneClient` documented why that cannot happen on a Qt thread that also
/// has timers to run: upstream's own `Session::exec()` hijacks the calling thread as the SDL main
/// thread for the whole stream (`app/streaming/session.cpp:1965-1966`).
class HandshakeTask : public QObject, public QRunnable
{
    Q_OBJECT

public:
    HandshakeTask(PairingHandshake handshake, PairingCancelRequest cancelRequest,
                  PairingTarget target, quint64 generation,
                  std::shared_ptr<std::atomic<quint64>> currentGeneration)
        : m_handshake(std::move(handshake)),
          m_cancelRequest(std::move(cancelRequest)),
          m_target(std::move(target)),
          m_generation(generation),
          m_currentGeneration(std::move(currentGeneration))
    {
    }

    void run() override
    {
        // The target - including the PIN - is captured by value and destroyed with this task. The
        // generation travels with the result so a `pair()` call that has since superseded this one
        // (T-06.6-53) can be told apart from the current one - upstream's own blocking call cannot
        // be aborted, so this task keeps running to completion regardless; only its result's fate
        // changes. Nothing copies the target anywhere else, and nothing logs it.
        PairingHandshakeResult result = m_handshake(m_target);

        // G-06.2-2 recovery, once (T-06.2-12-02: one retry, never a loop). A conflicting failure -
        // the rig's 409 on getservercert, or ALREADY_IN_PROGRESS out of the handshake - means a
        // half-open pending pairing session holds the shared upstream uniqueid on the rig. Clear
        // it (the clear must COMPLETE before the retry, so the rig's answer is seen first), then
        // run the handshake exactly once more and report whatever that produced, as-is.
        //
        // The generation gate makes the whole block a no-op for a superseded run: a fresh
        // `pair()` has since stored its own generation in the shared atomic, this run's number no
        // longer matches, and the stale run sends no clear and no retry - a clear aimed by a dead
        // run could cancel the very session that superseded it. The atomic is read and written
        // without touching `this`, which the worker outlives.
        if (!result.ok && m_generation == m_currentGeneration->load()) {
            // The classification the clear is about to remove, kept for the recovered record
            // (ADR-0072 item 2): a pairing that succeeds only after the clear reports the class
            // that was cleared, at attempt 2.
            const QString clearedStep = result.attemptStep;
            const QString clearedClass = result.stepClass;
            m_cancelRequest(m_target.hostAddress, kPairingControlPort);
            if (result.pairingConflict && m_generation == m_currentGeneration->load()) {
                result = m_handshake(m_target);
                // A second try of the same step in this Play is attempt 2, whether the retry
                // succeeds (with the cleared class) or fails (with the class of its own last
                // failure, which the second run computed for itself).
                result.attempts = 2;
                if (result.ok) {
                    result.attemptStep = clearedStep;
                    result.stepClass = clearedClass;
                }
            }
        }

        emit finished(m_generation, result);
    }

signals:
    void finished(quint64 generation, PairingHandshakeResult result);

private:
    PairingHandshake m_handshake;
    PairingCancelRequest m_cancelRequest;
    PairingTarget m_target;
    quint64 m_generation;
    std::shared_ptr<std::atomic<quint64>> m_currentGeneration;
};

} // namespace

QString clientCertificateFingerprint(const QByteArray& pem)
{
    if (pem.isEmpty()) {
        return QString();
    }

    QSslCertificate certificate(pem, QSsl::Pem);
    if (certificate.isNull()) {
        return QString();
    }

    // SHA-256, not the `QSslCertificate::digest()` default of MD5: this value is an identity, and
    // MD5 is not one (collisions are cheap).
    return QString::fromLatin1(certificate.digest(QCryptographicHash::Sha256).toHex());
}

ProductionPairingSeam::ProductionPairingSeam(QObject* parent)
    : QObject(parent),
      m_deadline(new QTimer(this)),
      m_currentGeneration(std::make_shared<std::atomic<quint64>>(0)),
      m_cancelRequest([](const QString& hostAddress, int port) {
          return sendPairingCancelRequest(hostAddress, port);
      })
{
    // Both arrive from the handshake's thread as queued connections.
    qRegisterMetaType<PairingHandshakeResult>("PairingHandshakeResult");
    qRegisterMetaType<PairedHostPtr>("PairedHostPtr");

    m_deadline->setSingleShot(true);
    connect(m_deadline, &QTimer::timeout, this, [this]() {
        qCWarning(seathubPairingSeam)
            << "pairing handshake did not finish inside" << m_deadlineMs << "ms";

        PairingHandshakeResult timedOut;
        timedOut.engineError = QStringLiteral("the pairing handshake did not finish inside %1 ms")
                                   .arg(m_deadlineMs);
        // 06.6-18/T-06.6-53: this timer is restarted by every `pair()` call, so whenever it fires
        // it is timing out whichever handshake is current - `m_generation` always names that one.
        finish(m_generation, timedOut);
    });
}

ProductionPairingSeam::~ProductionPairingSeam() = default;

void ProductionPairingSeam::setHandshake(PairingHandshake handshake)
{
    m_handshake = std::move(handshake);
}

void ProductionPairingSeam::setDeadlineMs(int milliseconds)
{
    m_deadlineMs = qMax(1, milliseconds);
}

void ProductionPairingSeam::setCancelRequest(PairingCancelRequest cancel)
{
    m_cancelRequest = std::move(cancel);
}

void ProductionPairingSeam::pair(const PairingTarget& target,
                                 std::function<void(bool, const QString&, const QString&)> done)
{
    if (target.hostAddress.isEmpty() || target.pairingPin.isEmpty()) {
        // Nothing to pair against, or nothing to pair with. The controller already refuses an
        // empty PIN before it calls this seam, so this is a boundary check on a public entry
        // point, not a second copy of that rule.
        done(false, QString(), QStringLiteral("the pairing target is incomplete"));
        return;
    }

    if (!m_handshake) {
        // No handshake installed. Fail closed rather than report a pairing that never ran.
        done(false, QString(), QStringLiteral("no pairing handshake is installed"));
        return;
    }

    // 06.6-18/T-06.6-53: a `pair()` call while an earlier one is still in flight is a fresh Play
    // or Try again for a different session - it is abandoned, not refused. Refusing here would
    // leave that customer stuck behind a handshake that can never resolve into their new session
    // anyway. Upstream's own blocking call cannot be aborted, so the old one keeps its pool thread
    // until it returns and finds nothing waiting for it: the generation bump below is what makes
    // that safe.
    ++m_generation;
    const quint64 generation = m_generation;
    m_currentGeneration->store(generation);

    m_pending = true;
    // 06.6-18/T-06.6-52: recorded before the handshake starts, so a late `hostResolved` always
    // carries the session it actually paired for, whatever this object's caller does meanwhile.
    m_sessionId = target.sessionId;
    m_done = std::move(done);
    // Restarts the timer if one was already running (for the handshake this call just superseded) -
    // it now counts down 90 s from THIS `pair()` call, not from whatever remained of the old one.
    m_deadline->start(m_deadlineMs);

    HandshakeTask* task = new HandshakeTask(m_handshake, m_cancelRequest, target, generation,
                                            m_currentGeneration);
    connect(task, &HandshakeTask::finished, this, &ProductionPairingSeam::handleHandshakeResult);
    QThreadPool::globalInstance()->start(task);
}

void ProductionPairingSeam::clearPendingPairing(const PairingTarget& target)
{
    if (target.hostAddress.isEmpty()) {
        return;
    }

    // Fire-and-forget: one clear on the pool thread, so the caller (the UI thread calling
    // `cancel()`) never waits on the rig. Best-effort by construction - `sendPairingCancelRequest`
    // reports its outcome in the log and returns a result nobody reads here. The task dies with
    // its own copy of the request and the target, exactly like `HandshakeTask` above.
    QThreadPool::globalInstance()->start(
        [cancelRequest = m_cancelRequest, target]() mutable {
            cancelRequest(target.hostAddress, kPairingControlPort);
        });
}

void ProductionPairingSeam::handleHandshakeResult(quint64 generation, const PairingHandshakeResult& result)
{
    finish(generation, result);
}

void ProductionPairingSeam::finish(quint64 generation, const PairingHandshakeResult& result)
{
    if (!m_pending || generation != m_generation) {
        // Either the deadline already spoke for the current generation, or this generation is
        // stale: a later `pair()` call has superseded the handshake it belongs to (T-06.6-53), or
        // this is a very late result from a handshake whose lease was revoked. `done` is called
        // exactly once per `pair()`, which is the whole contract - a stale generation gets nothing.
        return;
    }

    m_pending = false;
    m_deadline->stop();

    std::function<void(bool, const QString&, const QString&)> done = std::move(m_done);
    m_done = nullptr;

    if (!result.ok) {
        // Upstream's own sentence is diagnostic only and must never reach the customer (D-51);
        // `PairingController` composes the SeatHub sentence and the `SH-` reference from the
        // `ok == false` it receives here. The identity is empty on every failure path. Nothing is
        // logged here: the deadline and the missing identity each log their own reason above, and
        // a handshake that failed logged its own.
        //
        // The classification leaves ahead of `done`, on the same thread, so a queued connection
        // to the facade receives it before the controller's own `pairingFailed` for this run
        // (ADR-0072, Plan 09): `handlePairingFailed()` then has the step and the class while it
        // is still handling this failure.
        m_lastResult = result;
        emit failureClassified(result);
        done(false, QString(), result.engineError);
        return;
    }

    if (result.clientIdentity.isEmpty()) {
        // Nothing paired with an empty identity is a successful pairing: without an identity
        // there is nothing to verify a teardown against, and the whole reason this value is
        // reported is that verification (fail closed, Pitfall 3).
        qCWarning(seathubPairingSeam) << "upstream reported success with no client identity";
        m_lastResult = result;
        emit failureClassified(result);
        done(false, QString(), result.engineError);
        return;
    }

    qCInfo(seathubPairingSeam) << "upstream pairing handshake completed";

    // The host travels first: the engine session is built from it, and a listener that acted on
    // `done` before this arrived would be building a session from nothing. Tagged with the session
    // this handshake paired for (06.6-18/T-06.6-52), so a listener whose attached session has since
    // moved on can tell a live result from a stale one.
    emit hostResolved(m_sessionId, result.host);

    done(true, result.clientIdentity, QString());
}

#include "pairing_seam.moc"
