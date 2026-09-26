#include "pairing_seam.h"

#include <QCryptographicHash>
#include <QLoggingCategory>
#include <QRunnable>
#include <QSslCertificate>
#include <QThreadPool>

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
    HandshakeTask(PairingHandshake handshake, PairingTarget target, quint64 generation)
        : m_handshake(std::move(handshake)), m_target(std::move(target)), m_generation(generation)
    {
    }

    void run() override
    {
        // The target - including the PIN - is captured by value and destroyed with this task. The
        // generation travels with the result so a `pair()` call that has since superseded this one
        // (T-06.6-53) can be told apart from the current one - upstream's own blocking call cannot
        // be aborted, so this task keeps running to completion regardless; only its result's fate
        // changes. Nothing copies the target anywhere else, and nothing logs it.
        emit finished(m_generation, m_handshake(m_target));
    }

signals:
    void finished(quint64 generation, PairingHandshakeResult result);

private:
    PairingHandshake m_handshake;
    PairingTarget m_target;
    quint64 m_generation;
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
      m_deadline(new QTimer(this))
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

    m_pending = true;
    // 06.6-18/T-06.6-52: recorded before the handshake starts, so a late `hostResolved` always
    // carries the session it actually paired for, whatever this object's caller does meanwhile.
    m_sessionId = target.sessionId;
    m_done = std::move(done);
    // Restarts the timer if one was already running (for the handshake this call just superseded) -
    // it now counts down 90 s from THIS `pair()` call, not from whatever remained of the old one.
    m_deadline->start(m_deadlineMs);

    HandshakeTask* task = new HandshakeTask(m_handshake, target, generation);
    connect(task, &HandshakeTask::finished, this, &ProductionPairingSeam::handleHandshakeResult);
    QThreadPool::globalInstance()->start(task);
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
        done(false, QString(), result.engineError);
        return;
    }

    if (result.clientIdentity.isEmpty()) {
        // Nothing paired with an empty identity is a successful pairing: without an identity
        // there is nothing to verify a teardown against, and the whole reason this value is
        // reported is that verification (fail closed, Pitfall 3).
        qCWarning(seathubPairingSeam) << "upstream reported success with no client identity";
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
