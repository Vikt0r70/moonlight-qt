#include "stream_quality_sampler.h"
#include "telemetry.h"
#include <QMutexLocker>
#include <QThread>
#include <algorithm>
#include <cmath>

namespace {
double rounded(double value, int decimals)
{ const double scale = std::pow(10.0, decimals); return std::round(value * scale) / scale; }

QJsonObject aggregate(const QVector<VideoStats>& samples, int dropped, bool partial,
                      double windowSeconds)
{
    if (samples.isEmpty() && dropped == 0) return {};
    QJsonObject attrs;
    attrs.insert("n", samples.size());
    attrs.insert("coverage_pct", rounded(std::min(100.0, samples.size() * 100.0 / windowSeconds), 2));
    attrs.insert("dropped_samples", dropped);
    attrs.insert("partial", partial);
    for (auto it = samples.crbegin(); it != samples.crend(); ++it) {
        if (it->videoWidth.present && it->videoHeight.present) {
            attrs.insert("res", QStringLiteral("%1x%2").arg(int(it->videoWidth.value)).arg(int(it->videoHeight.value)));
            break;
        }
    }
    struct Field { const char* name; OptionalMetric VideoStats::* member; int decimals; };
    const Field fields[] = {
        {"fps", &VideoStats::renderedFps, 2},
        {"net_drop", &VideoStats::networkDroppedFramePct, 2},
        {"jitter_drop", &VideoStats::jitterDroppedFramePct, 2},
        {"rtt", &VideoStats::rttMs, 2}, {"decode", &VideoStats::decodeTimeMs, 2},
        {"queue", &VideoStats::queueTimeMs, 2}, {"render", &VideoStats::renderTimeMs, 2},
        {"host", &VideoStats::hostProcessingAvgMs, 1},
    };
    for (const auto& field : fields) {
        QVector<double> values;
        double sum = 0;
        for (const auto& sample : samples) {
            const auto value = sample.*field.member;
            if (value.present) { values.append(value.value); sum += value.value; }
        }
        if (values.isEmpty()) continue;
        std::sort(values.begin(), values.end());
        const QString prefix = QString::fromLatin1(field.name);
        attrs.insert(prefix + "_avg", rounded(sum / values.size(), field.decimals));
        const bool fps = prefix == QLatin1String("fps");
        const int index = fps ? 0 : int(std::ceil(0.95 * values.size())) - 1;
        attrs.insert(prefix + (fps ? "_min" : "_p95"), rounded(values.at(index), field.decimals));
    }
    return attrs;
}
}

StreamQualitySampler::StreamQualitySampler(QObject* parent, Clock)
    : QObject(parent), m_timer(new QTimer(this))
{
    m_buffer.reserve(kMaxSamplesPerWindow);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &StreamQualitySampler::tick);
}
bool StreamQualitySampler::onOwnThread() const
{ return thread() == nullptr || thread() == QThread::currentThread(); }
void StreamQualitySampler::setWindowMs(int ms)
{
    if (!onOwnThread()) {
        QMetaObject::invokeMethod(this, [this, ms]() { setWindowMs(ms); }, Qt::QueuedConnection);
        return;
    }
    Q_ASSERT(!m_running.load() && ms > 0);
    m_windowMs = ms;
}
void StreamQualitySampler::start(const QString& sessionId)
{
    if (!onOwnThread()) {
        QMetaObject::invokeMethod(this, [this, sessionId]() { start(sessionId); }, Qt::QueuedConnection);
        return;
    }
    if (m_running.load()) return;
    { QMutexLocker lock(&m_mutex); m_buffer.clear(); m_dropped.store(0); }
    m_rollups.store(0);
    m_windowClock.start();
    m_running.store(true);
    m_timer->start(m_windowMs);
}
void StreamQualitySampler::finish()
{
    if (!onOwnThread()) {
        QMetaObject::invokeMethod(this, [this]() { finish(); }, Qt::QueuedConnection);
        return;
    }
    if (!m_running.exchange(false)) return;
    m_timer->stop();
    emitWindow(true);
}
void StreamQualitySampler::feed(const VideoStats& stats)
{
    if (!m_running.load()) return;
    // The render thread never waits for window extraction or another producer.
    if (!m_mutex.tryLock()) { m_dropped.fetch_add(1); return; }
    if (m_buffer.size() < kMaxSamplesPerWindow) m_buffer.append(stats);
    else m_dropped.fetch_add(1);
    m_mutex.unlock();
}
void StreamQualitySampler::noteConnectionStatus(int) {}
QJsonObject StreamQualitySampler::takeWindow(bool partial)
{
    QVector<VideoStats> samples;
    int dropped;
    { QMutexLocker lock(&m_mutex); samples.swap(m_buffer); dropped = m_dropped.exchange(0); }
    const double seconds = partial ? std::max(0.001, m_windowClock.elapsed() / 1000.0)
                                   : m_windowMs / 1000.0;
    return aggregate(samples, dropped, partial, seconds);
}
void StreamQualitySampler::tick()
{
    if (!m_running.load()) return;
    emitWindow(false);
    m_windowClock.restart();
}
void StreamQualitySampler::emitWindow(bool partial)
{
    auto attrs = takeWindow(partial);
    if (attrs.value("n").toInt() == 0) return;
    const auto health = SeatHubTelemetry::rollupDeliveryHealth();
    for (auto it = health.begin(); it != health.end(); ++it) attrs.insert(it.key(), it.value());
    SeatHubTelemetry::emitDiagnostic(QStringLiteral("stream.rollup"), LogLevel::Info, attrs);
    SeatHubTelemetry::emitRollupMetrics(attrs);
    m_rollups.fetch_add(1);
    emit rollupEmitted();
}
