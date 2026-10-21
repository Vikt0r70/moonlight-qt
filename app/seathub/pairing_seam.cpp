#include "pairing_seam.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QRunnable>
#include <QSslCertificate>
#include <QThreadPool>

#include <atomic>
#include <memory>

#include "pairing_key.h"
#include "pairing_recovery.h"
#include "pairing_retry.h"

Q_LOGGING_CATEGORY(seathubPairingSeam, "seathub.pairing.seam")

/// T074: the worker publishes here, under the lock, what it has settled so far - the first attempt's
/// closed outcome (before the pending-slot clear and the retry can block) and the actual retry
/// decision (before the retry handshake blocks). The seam's deadline reads a snapshot to speak for a
/// worker that has not answered. Plain closed tokens and flags only: no PIN, no salt, no hash, no
/// diagnostic text.
struct PairingProgress
{
    struct Snapshot
    {
        bool firstPublished = false;
        bool firstSucceeded = false;
        QString firstStep;
        QString firstClass;
        bool retried = false;
        QString retryReason;
    };

    void publishFirst(bool succeeded, const QString& step, const QString& failureClass)
    {
        QMutexLocker locker(&m_lock);
        m_state.firstPublished = true;
        m_state.firstSucceeded = succeeded;
        m_state.firstStep = succeeded ? QString() : step;
        m_state.firstClass = succeeded ? QString() : failureClass;
    }

    void publishRetry(const QString& reason)
    {
        QMutexLocker locker(&m_lock);
        m_state.retried = true;
        m_state.retryReason = reason;
    }

    Snapshot snapshot() const
    {
        QMutexLocker locker(&m_lock);
        return m_state;
    }

private:
    mutable QMutex m_lock;
    Snapshot m_state;
};

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
    HandshakeTask(PairingHandshake handshake, WarmReturnCheck warmCheck,
                  PairingCancelRequest cancelRequest,
                  PairingTarget target, quint64 generation,
                  std::shared_ptr<std::atomic<quint64>> currentGeneration,
                  QElapsedTimer clock, int deadlineMs,
                  std::shared_ptr<PairingProgress> progress)
        : m_handshake(std::move(handshake)),
          m_warmCheck(std::move(warmCheck)),
          m_cancelRequest(std::move(cancelRequest)),
          m_target(std::move(target)),
          m_generation(generation),
          m_currentGeneration(std::move(currentGeneration)),
          m_clock(clock),
          m_deadlineMs(deadlineMs),
          m_progress(std::move(progress))
    {
    }

    void run() override
    {
        // The target - including the PIN - is captured by value and destroyed with this task. The
        // generation travels with the result so a `pair()` call that has since superseded this one
        // (T-06.6-53) can be told apart from the current one - upstream's own blocking call cannot
        // be aborted, so this task keeps running to completion regardless; only its result's fate
        // changes. Nothing copies the target anywhere else, and nothing logs it.
        PairingHandshakeResult result;

        // ADR-0074 W03 / L07, the warm return: the PIN handshake is skipped only when the control
        // plane said so (`warmReturn`: a NEW session on the caller's own free return hold, or an
        // ACTIVE session reconnecting inside its disconnect grace), this app run still holds the
        // host certificate it pinned earlier, and the check - the rig's HTTPS serverinfo read with
        // that pin - answers paired. Anything else (the field absent or false, nothing held after a
        // restart or a sign-out, a failed or refused check) falls through to the normal PIN
        // handshake below, untouched: a PIN is always issued, so a normal pairing is always
        // possible, and the rig's own certificate check is what admits the stream either way. The
        // check and the handshake run one after the other on this one worker, so a failed warm
        // attempt never leaves two handshakes in flight.
        bool warmReturned = false;
        if (m_target.warmReturn && m_warmCheck) {
            const QByteArray held = HeldRigTrust::held();
            if (!held.isEmpty()) {
                PairingHandshakeResult warm = m_warmCheck(m_target, held);
                if (warm.ok && !warm.clientIdentity.isEmpty() && warm.host) {
                    result = warm;
                    result.warm = true;
                    warmReturned = true;
                }
            }
        }
        if (!warmReturned) {
            // ADR-0109: this real pair attempt's key record is scoped to this session and attempt
            // on this worker thread only, and the scope ends with the call.
            seathub::PairingKeyLogScope keyScope(m_target.sessionId, m_generation);
            result = m_handshake(m_target);
        }

        // SC18: the first attempt stays on the record whatever the retry does.
        const PairingHandshakeResult first = result;
        const bool firstSucceeded = first.ok && !first.clientIdentity.isEmpty();
        // T074: on the record before the pending-slot clear or the retry can block, so a deadline
        // that fires meanwhile still reports the first attempt.
        m_progress->publishFirst(firstSucceeded, first.attemptStep, first.stepClass);

        // The ONE automatic retry a Play may take (T-06.2-12-02: one retry, never a loop; SC18).
        // The rig's 409 / ALREADY_IN_PROGRESS clear (G-06.2-2) and the new retry of any other
        // non-PAIRED result draw from the same `PairingRetryBudget`, so a conflict clear followed
        // by a failure is not a second retry. On a failure the pending-pairing slot is cleared
        // first (the clear must COMPLETE before the retry, so the rig's answer is seen first);
        // then the handshake runs exactly once more with the same session and PIN, and whatever
        // that produced is reported.
        //
        // The generation gate makes the whole block a no-op for a superseded run: a fresh
        // `pair()` has since stored its own generation in the shared atomic, this run's number no
        // longer matches, and the stale run sends no clear and no retry - a clear aimed by a dead
        // run could cancel the very session that superseded it. The atomic is read and written
        // without touching `this`, which the worker outlives. It is read again AFTER the clear
        // returns, because the clear blocks and a newer `pair()` or a cancel can land meanwhile.
        PairingRetryBudget budget;
        if (!result.ok && m_generation == m_currentGeneration->load()) {
            m_cancelRequest(m_target.hostAddress, m_target.controlPort);

            PairingRetryBudget::Inputs in;
            in.ok = result.ok;
            in.pairingConflict = result.pairingConflict;
            in.generationCurrent = m_generation == m_currentGeneration->load();
            in.reconnecting = m_target.reconnecting;
            // No stream can exist before pairing completes.
            in.streamExists = false;
            in.elapsedMs = m_clock.elapsed();
            in.deadlineMs = m_deadlineMs;
            const std::optional<PairingRetryBudget::Reason> reason = budget.decide(in);
            if (reason) {
                const QString token = PairingRetryBudget::token(*reason);
                // T074: the actual retry decision is on the record before the retry blocks.
                m_progress->publishRetry(token);
                // Neither the session, the rig address nor the PIN is part of this line.
                qCInfo(seathubPairingSeam).noquote()
                    << QStringLiteral("pairing handshake retried once reason=%1").arg(token);
                {
                    // ADR-0109: the retry is a separate real pair attempt with its own scope.
                    seathub::PairingKeyLogScope keyScope(m_target.sessionId, m_generation);
                    result = m_handshake(m_target);
                }
                // A second try of the same step in this Play is attempt 2 (ADR-0072 item 2).
                result.attempts = 2;
                result.retried = true;
                result.retryReason = token;
                if (result.ok && *reason == PairingRetryBudget::Reason::Conflict) {
                    // The recovered record is FROZEN by the ADR (item 2) and by
                    // `docs/spec/client.md` "Play diagnostics - Rules": `step=pair_handshake`,
                    // `failure_class=in_progress`, `attempt=2`. The conflict that was cleared was
                    // a pairing-already-in-progress whichever surface named it - the rig's HTTP
                    // 409 on getservercert, whose own mapper would say `pair_server_info` /
                    // `http_4xx`, or `ALREADY_IN_PROGRESS` out of the handshake - so the record
                    // names the step that was retried and the class that was cleared, never the
                    // mapper's interim answer for the failure the retry just retired. The
                    // diagnostic text the first run carried stays where it was: out of it.
                    result.attemptStep = QStringLiteral("pair_handshake");
                    result.stepClass = QStringLiteral("in_progress");
                } else if (result.ok) {
                    // A failure-retry success keeps the FIRST attempt's step and class, never
                    // relabelled `in_progress` (SC18): the record says what went wrong first.
                    result.attemptStep = first.attemptStep;
                    result.stepClass = first.stepClass;
                }
                // A retry that fails again keeps ITS OWN class at attempt 2 (same item): the
                // second run computed it from its own failure, and nothing here overwrites it.
            }
        }

        result.firstOutcome = firstSucceeded ? QStringLiteral("success") : QStringLiteral("failure");
        if (!firstSucceeded) {
            result.firstAttemptStep = first.attemptStep;
            result.firstStepClass = first.stepClass;
        }
        result.pinShapeOk = PairingRetryBudget::pinShapeOk(m_target.pairingPin);
        result.handshakeMs = m_clock.elapsed();

        emit finished(m_generation, result);
    }

signals:
    void finished(quint64 generation, PairingHandshakeResult result);

private:
    PairingHandshake m_handshake;
    WarmReturnCheck m_warmCheck;
    PairingCancelRequest m_cancelRequest;
    PairingTarget m_target;
    quint64 m_generation;
    std::shared_ptr<std::atomic<quint64>> m_currentGeneration;
    /// A copy of the clock `pair()` started with the 90 s deadline: the retry draws on what that
    /// one deadline has left and never restarts it.
    QElapsedTimer m_clock;
    int m_deadlineMs;
    /// This `pair()` call's progress record, shared with the seam (T074).
    std::shared_ptr<PairingProgress> m_progress;
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

namespace {

// The held certificate lives here and nowhere else: process memory, one lock, no file. Written by
// the handshake's pool thread after a pairing, read by the next handshake's, cleared on the
// facade's thread at sign-out.
QMutex& heldRigTrustMutex()
{
    static QMutex mutex;
    return mutex;
}

QByteArray& heldRigTrustBytes()
{
    static QByteArray bytes;
    return bytes;
}

// What a Play settled before its worker blocked, shared by the deadline and a cancel. The final
// attempt has no class of its own here: it did not answer.
PairingHandshakeResult progressRecord(const PairingProgress::Snapshot& progress, bool pinShapeOk,
                                      qint64 elapsedMs)
{
    PairingHandshakeResult record;
    record.firstOutcome = (progress.firstPublished && progress.firstSucceeded)
                              ? QStringLiteral("success")
                              : QStringLiteral("failure");
    record.firstAttemptStep = progress.firstStep;
    record.firstStepClass = progress.firstClass;
    if (progress.retried) {
        // A second try of the same step in this Play is attempt 2 (ADR-0072 item 2).
        record.attempts = 2;
        record.retried = true;
        record.retryReason = progress.retryReason;
    }
    record.pinShapeOk = pinShapeOk;
    record.handshakeMs = elapsedMs;
    return record;
}

} // namespace

void HeldRigTrust::remember(const QByteArray& hostCertificatePem)
{
    QMutexLocker locker(&heldRigTrustMutex());
    heldRigTrustBytes() = hostCertificatePem;
}

QByteArray HeldRigTrust::held()
{
    QMutexLocker locker(&heldRigTrustMutex());
    return heldRigTrustBytes();
}

void HeldRigTrust::clear()
{
    QMutexLocker locker(&heldRigTrustMutex());
    heldRigTrustBytes().clear();
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

        // T074: speak for what the worker had settled before it blocked. `m_progress` belongs to
        // the current `pair()` call, so a superseded generation's first failure or retry never
        // reaches this result.
        const PairingProgress::Snapshot progress = m_progress ? m_progress->snapshot()
                                                               : PairingProgress::Snapshot();
        PairingHandshakeResult timedOut = progressRecord(progress, m_pinShapeOk, m_clock.elapsed());
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

void ProductionPairingSeam::setWarmReturnCheck(WarmReturnCheck check)
{
    m_warmCheck = std::move(check);
}

void ProductionPairingSeam::setDeadlineMs(int milliseconds)
{
    m_deadlineMs = qMax(1, milliseconds);
}

void ProductionPairingSeam::setCancelRequest(PairingCancelRequest cancel)
{
    m_cancelRequest = std::move(cancel);
}

void ProductionPairingSeam::setAttemptSink(AttemptSink sink)
{
    m_attemptSink = std::move(sink);
}

void ProductionPairingSeam::pair(const PairingTarget& target,
                                 std::function<void(bool, const QString&, const QString&)> done)
{
    if (target.hostAddress.isEmpty() || target.pairingPin.isEmpty() || target.controlPort <= 0) {
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
    m_clock.start();
    m_pinShapeOk = PairingRetryBudget::pinShapeOk(target.pairingPin);
    // One progress record per call: the worker below publishes to it, the deadline reads it, and a
    // later `pair()` replaces it, so an older generation's worker only ever writes to a record
    // nobody reads any more.
    m_progress = std::make_shared<PairingProgress>();

    qCInfo(seathubPairingSeam).noquote()
        << QStringLiteral("pairing_instrumentation writer=client stage=start session_id=%1 client_attempt=%2")
            .arg(target.sessionId)
            .arg(generation);

    HandshakeTask* task = new HandshakeTask(m_handshake, m_warmCheck, m_cancelRequest, target,
                                            generation, m_currentGeneration, m_clock, m_deadlineMs,
                                            m_progress);
    connect(task, &HandshakeTask::finished, this, &ProductionPairingSeam::handleHandshakeResult);
    QThreadPool::globalInstance()->start(task);
}

void ProductionPairingSeam::clearPendingPairing(const PairingTarget& target)
{
    if (target.hostAddress.isEmpty()) {
        return;
    }

    // The caller cancelled the attempt this clear belongs to (`PairingController::cancel()`). If its
    // handshake is still pending, it is over: nobody is waiting for its result, so it must not
    // report one later (a late classification or host would reach whatever attempt is current by
    // then), and its worker must not clear or retry against the rig (PF15-b). A new generation
    // does both at once: `finish()` drops the old one's result and the worker's own gate no longer
    // matches. The deadline belongs to the abandoned handshake, so it stops with it.
    //
    // T093 (SC18): a first failure the customer already caused is still recorded, once, for the
    // abandoned Play. Only the attempt sink hears it - the facade's evidence - and never the
    // classification, the host, `done`, the instrumentation line or `lastResult()`, so the public
    // cancellation stays neutral. A Play cancelled before its first attempt failed records nothing.
    if (m_pending && m_sessionId == target.sessionId) {
        m_pending = false;
        m_deadline->stop();
        m_done = nullptr;
        ++m_generation;
        m_currentGeneration->store(m_generation);

        const PairingProgress::Snapshot progress = m_progress ? m_progress->snapshot()
                                                               : PairingProgress::Snapshot();
        if (m_attemptSink && progress.firstPublished && !progress.firstSucceeded) {
            PairingHandshakeResult cancelled =
                progressRecord(progress, m_pinShapeOk, m_clock.elapsed());
            if (!cancelled.retried) {
                // Cancelled before the retry began: the first attempt is the final one.
                cancelled.attemptStep = progress.firstStep;
                cancelled.stepClass = progress.firstClass;
            }
            m_attemptSink(cancelled);
        }
    }

    // Fire-and-forget: one clear on the pool thread, so the caller (the UI thread calling
    // `cancel()`) never waits on the rig. Best-effort by construction - `sendPairingCancelRequest`
    // reports its outcome in the log and returns a result nobody reads here. The task dies with
    // its own copy of the request and the target, exactly like `HandshakeTask` above.
    //
    // PF15-b: the clear belongs to the attempt that asked for it, named by the generation read
    // here. It runs later on a pool thread, and by then a newer `pair()` may have started its own
    // handshake against the same rig: the rig's pending-pairing session is one shared slot
    // (`target.controlPort`, one uniqueid), so a clear aimed by the older attempt would cancel the
    // newer one's pairing. The same atomic the handshake worker's recovery loop reads gates it: a
    // clear whose generation is no longer current is dropped without touching the rig.
    const quint64 generation = m_generation;
    QThreadPool::globalInstance()->start(
        [cancelRequest = m_cancelRequest, target, generation,
         currentGeneration = m_currentGeneration]() mutable {
            if (generation != currentGeneration->load()) {
                qCInfo(seathubPairingSeam)
                    << "dropping a pairing clear that belongs to a superseded attempt";
                return;
            }
            cancelRequest(target.hostAddress, target.controlPort);
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

    const bool effectiveSuccess = result.ok && !result.clientIdentity.isEmpty();
    QString record = QStringLiteral("pairing_instrumentation writer=client stage=result session_id=%1 client_attempt=%2 outcome=%3 warm=%4 attempts=%5")
        .arg(m_sessionId)
        .arg(generation)
        .arg(effectiveSuccess ? QStringLiteral("success") : QStringLiteral("failure"))
        .arg(result.warm ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(result.attempts);
    if (!result.attemptStep.isEmpty()) {
        record += QStringLiteral(" engine_step=%1").arg(result.attemptStep);
    }
    if (!result.stepClass.isEmpty()) {
        record += QStringLiteral(" engine_class=%1").arg(result.stepClass);
    }
    // SC18: the first attempt, the retry and the timing, appended after the fields above. The
    // first attempt's step and class appear only for a first failure that was classified.
    record += QStringLiteral(" first_outcome=%1")
                  .arg(result.firstOutcome.isEmpty() ? QStringLiteral("failure") : result.firstOutcome);
    if (result.firstOutcome != QLatin1String("success")) {
        if (!result.firstAttemptStep.isEmpty()) {
            record += QStringLiteral(" first_engine_step=%1").arg(result.firstAttemptStep);
        }
        if (!result.firstStepClass.isEmpty()) {
            record += QStringLiteral(" first_engine_class=%1").arg(result.firstStepClass);
        }
    }
    record += QStringLiteral(" retried=%1").arg(result.retried ? QStringLiteral("true") : QStringLiteral("false"));
    if (result.retried) {
        record += QStringLiteral(" retry_reason=%1").arg(result.retryReason);
    }
    record += QStringLiteral(" pin_shape_ok=%1 handshake_ms=%2")
                  .arg(result.pinShapeOk ? QStringLiteral("true") : QStringLiteral("false"))
                  .arg(result.handshakeMs);
    qCInfo(seathubPairingSeam).noquote() << record;
    if (m_attemptSink) {
        m_attemptSink(result);
    }

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
        emit handshakeClassified(result);
        done(false, QString(), result.engineError);
        return;
    }

    if (result.clientIdentity.isEmpty()) {
        // Nothing paired with an empty identity is a successful pairing: without an identity
        // there is nothing to verify a teardown against, and the whole reason this value is
        // reported is that verification (fail closed, Pitfall 3).
        qCWarning(seathubPairingSeam) << "upstream reported success with no client identity";
        m_lastResult = result;
        emit handshakeClassified(result);
        done(false, QString(), result.engineError);
        return;
    }

    qCInfo(seathubPairingSeam) << "upstream pairing handshake completed";

    // The result's classification leaves on the same thread BEFORE the result itself, exactly as
    // it does on the failure paths above - so a queued connection to the facade holds it by the
    // time `hostResolved` and the controller's `pairingCompleted` arrive. An ordinary success
    // carries no step and no class (attempts 1) and the facade emits nothing for it; a pairing
    // that recovered through the clear-and-retry carries the frozen record (ADR-0072 item 2),
    // which is the one this seam previously dropped on the floor here - the success branch
    // reported only the host, and the classification the whole recovery produced never reached an
    // emitter at all.
    m_lastResult = result;
    emit handshakeClassified(result);

    // The host travels first: the engine session is built from it, and a listener that acted on
    // `done` before this arrived would be building a session from nothing. Tagged with the session
    // this handshake paired for (06.6-18/T-06.6-52), so a listener whose attached session has since
    // moved on can tell a live result from a stale one.
    emit hostResolved(m_sessionId, result.host);

    done(true, result.clientIdentity, QString());
}

#include "pairing_seam.moc"
