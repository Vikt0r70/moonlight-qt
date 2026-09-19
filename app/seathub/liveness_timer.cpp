#include "liveness_timer.h"

#include <QLoggingCategory>
#include <QThread>

Q_LOGGING_CATEGORY(seathubLiveness, "seathub.liveness")

namespace {

const char* kStateIdle = "idle";
const char* kStateStreaming = "streaming";

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
    setReportedState(QString::fromLatin1(kStateStreaming));

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

    m_client->postLiveness(m_sessionId, m_reportedState, m_errorCode,
                           [this](const ControlPlaneResult& result) { handleResult(result); });
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
