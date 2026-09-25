#include "liveness_timer.h"

#include <QLoggingCategory>
#include <QThread>

Q_LOGGING_CATEGORY(seathubLiveness, "seathub.liveness")

namespace {

const char* kStateIdle = "idle";

// D-11/3.1.0: SeatHub's own stage, one per contract 3.1.0's `LivenessStage` (`docs/spec/openapi.yaml`,
// Plan 02). `first_frame` is in the enum but no engine signal reports the first decoded frame
// without an engine edit (ADR-0044, ADR-0060 item 20), so this plan never sends it - `setStage`
// still accepts it, since a later plan may.
const char* const kSeatHubStages[] = { "preparing_rig", "pairing",      "connecting", "first_frame",
                                       "streaming",     "reconnecting", "ending",     "failed" };
const char* kStagePreparingRig = "preparing_rig";
const char* kStageFailed = "failed";
const char* kStageEnding = "ending";

bool isValidSeatHubStage(const QString& stage)
{
    for (const char* candidate : kSeatHubStages) {
        if (stage == QLatin1String(candidate)) {
            return true;
        }
    }
    return false;
}

} // namespace

LivenessTimer::LivenessTimer(QObject* parent)
    : QObject(parent),
      m_timer(new QTimer(this)),
      m_reportedState(QString::fromLatin1(kStateIdle))
{
    m_timer->setInterval(m_intervalMs);
    connect(m_timer, &QTimer::timeout, this, &LivenessTimer::tick);
}

bool LivenessTimer::onOwnThread() const
{
    // `thread()` goes null only when the QThread that owned this object has been destroyed, in
    // which case there is no queue left to hand work to and running here is the only option.
    QThread* owner = thread();
    return owner == nullptr || owner == QThread::currentThread();
}

void LivenessTimer::setControlPlane(ControlPlaneClient* client)
{
    m_client = client;
}

void LivenessTimer::setIntervalMs(int milliseconds)
{
    m_intervalMs = qMax(1, milliseconds);
    m_timer->setInterval(m_intervalMs);
}

void LivenessTimer::setGraceMs(int milliseconds)
{
    m_graceMs = qMax(1, milliseconds);
}

void LivenessTimer::setReportedState(const QString& state)
{
    if (!onOwnThread()) {
        QMetaObject::invokeMethod(this, [this, state]() { setReportedState(state); },
                                  Qt::QueuedConnection);
        return;
    }
    if (m_reportedState == state) {
        return;
    }
    m_reportedState = state;
    emit reportedStateChanged();
    emit payloadChanged();
}

void LivenessTimer::setErrorCode(const QString& reference)
{
    if (!onOwnThread()) {
        QMetaObject::invokeMethod(this, [this, reference]() { setErrorCode(reference); },
                                  Qt::QueuedConnection);
        return;
    }
    // Dropped rather than sent when it does not match ADR-0008's shape, so the control plane
    // never has to reject a body this client built.
    const QString sanitised = ControlPlaneClient::isValidErrorCode(reference) ? reference
                                                                             : QString();
    if (m_errorCode == sanitised) {
        return;
    }
    m_errorCode = sanitised;
    emit payloadChanged();
}

bool LivenessTimer::isRunning() const
{
    return m_timer->isActive();
}

void LivenessTimer::start(const QString& sessionId)
{
    if (sessionId.isEmpty()) {
        return;
    }

    if (!onOwnThread()) {
        // Hand the start to the thread that owns the QTimer. Qt refuses `QTimer::start()` from a
        // foreign thread - "Timers cannot be started from another thread" is a warning followed by
        // a no-op, not a failure - so the main-thread caller used to get exactly one report (the
        // immediate `tick()` below) and then silence for the rest of the stream.
        QMetaObject::invokeMethod(this, [this, sessionId]() { start(sessionId); },
                                  Qt::QueuedConnection);
        return;
    }

    m_sessionId = sessionId;
    m_consecutiveFailures = 0;
    m_warned = false;
    // D-11: `start()` now runs at session begin, well before the media path exists, so it no
    // longer forces `state` to "streaming" the way it used to when this was called from
    // `handleConnectionStarted`. `state` stays whatever it already was (idle, unless a caller set
    // it first) - `handleConnectionStarted` is the one place that still sets it to "streaming",
    // at exactly the same real-world moment as before (ADR-0041's meaning is unchanged). The new
    // stage, in contrast, does start here: every session begins at `preparing_rig`.
    m_stage = QString::fromLatin1(kStagePreparingRig);
    m_engineStage.clear();
    m_hasEngineError = false;
    m_engineError = 0;
    m_failingPorts.clear();

    if (!m_timer->isActive()) {
        m_timer->start();
        emit runningChanged();
    }

    qCInfo(seathubLiveness) << "liveness reporting started for session" << sessionId;

    // Report immediately as well as on the interval: the server stamps its deadline at ACTIVE
    // entry, and a first report that waits a whole interval spends part of the grace window
    // before the client has said anything at all.
    tick();
}

void LivenessTimer::stop()
{
    if (!onOwnThread()) {
        QMetaObject::invokeMethod(this, [this]() { stop(); }, Qt::QueuedConnection);
        return;
    }

    if (m_timer->isActive()) {
        m_timer->stop();
        emit runningChanged();
    }
    m_sessionId.clear();
    m_consecutiveFailures = 0;
    m_warned = false;
    setReportedState(QString::fromLatin1(kStateIdle));
}

void LivenessTimer::tick()
{
    if (!onOwnThread()) {
        QMetaObject::invokeMethod(this, [this]() { tick(); }, Qt::QueuedConnection);
        return;
    }

    if (m_sessionId.isEmpty() || m_client == nullptr) {
        return;
    }

    // The wallet first, so the report stays the last request of a tick. Its outcome is independent
    // of the report's: neither can fail the other.
    m_client->fetchWallet([this](const ControlPlaneResult& result) { handleWalletResult(result); });

    m_client->postLiveness(m_sessionId, buildPayload(),
                           [this](const ControlPlaneResult& result) { handleResult(result); });
}

QJsonObject LivenessTimer::buildPayload() const
{
    QJsonObject object;

    // `stage` is always present once a session has been started (D-11): every session begins
    // reporting from `preparing_rig`.
    if (!m_stage.isEmpty()) {
        object.insert(QStringLiteral("stage"), m_stage);
    }
    // ADR-0041 (1.6.0), unchanged: only the three documented values are ever sent - "idle" (and
    // anything else `setReportedState` was handed) is left out rather than sent as an invalid
    // enum value.
    if (ControlPlaneClient::isValidLivenessState(m_reportedState)) {
        object.insert(QStringLiteral("state"), m_reportedState);
    }
    if (!m_errorCode.isEmpty()) {
        object.insert(QStringLiteral("error_code"), m_errorCode);
    }
    if (!m_engineStage.isEmpty()) {
        object.insert(QStringLiteral("engine_stage"), m_engineStage);
    }
    if (m_hasEngineError) {
        object.insert(QStringLiteral("engine_error"), m_engineError);
    }
    if (!m_failingPorts.isEmpty()) {
        object.insert(QStringLiteral("failing_ports"), m_failingPorts);
    }
    return object;
}

void LivenessTimer::reportNow()
{
    if (m_sessionId.isEmpty() || m_client == nullptr) {
        return;
    }
    m_client->postLiveness(m_sessionId, buildPayload(),
                           [this](const ControlPlaneResult& result) { handleResult(result); });
}

void LivenessTimer::setStage(const QString& stage)
{
    if (!onOwnThread()) {
        QMetaObject::invokeMethod(this, [this, stage]() { setStage(stage); }, Qt::QueuedConnection);
        return;
    }
    if (!isValidSeatHubStage(stage)) {
        return;
    }
    if (m_stage == stage) {
        return;
    }
    m_stage = stage;
    emit payloadChanged();
    // A stage change is reported immediately, ahead of the next scheduled tick, so the server
    // sees the transition the moment it happens.
    reportNow();
}

void LivenessTimer::setEngineStage(const QString& engineStage)
{
    if (!onOwnThread()) {
        QMetaObject::invokeMethod(this, [this, engineStage]() { setEngineStage(engineStage); },
                                  Qt::QueuedConnection);
        return;
    }
    if (m_engineStage == engineStage) {
        return;
    }
    m_engineStage = engineStage;
    emit payloadChanged();
}

void LivenessTimer::setEngineError(int code)
{
    if (!onOwnThread()) {
        QMetaObject::invokeMethod(this, [this, code]() { setEngineError(code); },
                                  Qt::QueuedConnection);
        return;
    }
    if (m_hasEngineError && m_engineError == code) {
        return;
    }
    m_hasEngineError = true;
    m_engineError = code;
    emit payloadChanged();
}

void LivenessTimer::setFailingPorts(const QString& failingPorts)
{
    if (!onOwnThread()) {
        QMetaObject::invokeMethod(this, [this, failingPorts]() { setFailingPorts(failingPorts); },
                                  Qt::QueuedConnection);
        return;
    }
    if (m_failingPorts == failingPorts) {
        return;
    }
    m_failingPorts = failingPorts;
    emit payloadChanged();
}

void LivenessTimer::reportFailure(const QString& engineStage)
{
    if (!onOwnThread()) {
        QMetaObject::invokeMethod(this, [this, engineStage]() { reportFailure(engineStage); },
                                  Qt::QueuedConnection);
        return;
    }

    // No code: a pairing timeout or refusal, or the `m_session->start()` refusal. The 3.1.0
    // contract reserves `engine_error` for a real platform code, so neither it nor
    // `failing_ports` is sent here - any stale value from an earlier stage of this same session
    // is dropped, not carried into a failure that never had one.
    m_stage = QString::fromLatin1(kStageFailed);
    m_engineStage = engineStage;
    m_hasEngineError = false;
    m_engineError = 0;
    m_failingPorts.clear();
    emit payloadChanged();
    reportNow();
}

void LivenessTimer::reportFailure(const QString& engineStage, int engineError,
                                  const QString& failingPorts)
{
    if (!onOwnThread()) {
        QMetaObject::invokeMethod(
            this, [this, engineStage, engineError, failingPorts]() {
                reportFailure(engineStage, engineError, failingPorts);
            },
            Qt::QueuedConnection);
        return;
    }

    // A stage failure, with the engine's own code and ports (`handleStageFailed`).
    m_stage = QString::fromLatin1(kStageFailed);
    m_engineStage = engineStage;
    m_hasEngineError = true;
    m_engineError = engineError;
    m_failingPorts = failingPorts;
    emit payloadChanged();
    reportNow();
}

void LivenessTimer::noteTermination(int code)
{
    if (!onOwnThread()) {
        QMetaObject::invokeMethod(this, [this, code]() { noteTermination(code); },
                                  Qt::QueuedConnection);
        return;
    }

    // D-11/A-51: the engine's own termination code, the moment it is known. `stage` moves to
    // `ending` in the same call - this is a stage change too, and reports at once for the same
    // reason `setStage` does.
    m_hasEngineError = true;
    m_engineError = code;
    m_stage = QString::fromLatin1(kStageEnding);
    emit payloadChanged();
    reportNow();
}

void LivenessTimer::handleWalletResult(const ControlPlaneResult& result)
{
    // The tick was stopped while the read was on its way: the session it belonged to is over.
    if (m_sessionId.isEmpty()) {
        return;
    }

    // A body that is not a balance is not a balance of zero (`WalletInfo::parse`): reporting zero
    // for "we could not read it" would put a two-minute warning in front of a customer with credit.
    WalletInfo wallet;
    if (!result.ok || !WalletInfo::parse(result.body, &wallet)) {
        qCInfo(seathubLiveness) << "wallet read failed; keeping the last balance; status"
                                << result.statusCode;
        emit walletReadFailed();
        return;
    }

    emit walletRead(wallet.balanceMinutes);
}

void LivenessTimer::handleResult(const ControlPlaneResult& result)
{
    if (m_sessionId.isEmpty()) {
        return;
    }

    if (result.ok) {
        m_consecutiveFailures = 0;
        m_warned = false;
        emit livenessReported();
        return;
    }

    ++m_consecutiveFailures;
    qCWarning(seathubLiveness) << "liveness report failed; consecutive failures"
                               << m_consecutiveFailures
                               << "status" << result.statusCode;
    emit livenessFailed(m_consecutiveFailures);

    // D-33: this is where a session would be ended if the timer were written the obvious way,
    // and it is the wrong thing to do. The stream is independent of the control plane, so an
    // undelivered heartbeat is reported and retried, never turned into a session-ending error
    // and never routed through `error_map.h`.
    if (!m_warned && m_consecutiveFailures * m_intervalMs >= m_graceMs) {
        m_warned = true;
        qCWarning(seathubLiveness)
            << "liveness grace window elapsed with no successful report; warning only, session "
               "continues and retries (D-33)";
        emit livenessWarning();
    }

    // Deliberately no `m_timer->stop()` and no failure raised: the timer keeps firing.
}
