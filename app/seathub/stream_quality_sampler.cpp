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

StreamQualitySampler::StreamQualitySampler(QObject* parent, Clock clock)
    : QObject(parent), m_clock(std::move(clock)), m_timer(new QTimer(this))
{
    m_buffer.reserve(kMaxSamplesPerWindow);
    m_recent.reserve(kRecentSamples);
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
void StreamQualitySampler::start(const QString& playId)
{
    if (!onOwnThread()) {
        QMetaObject::invokeMethod(this, [this, playId]() { start(playId); }, Qt::QueuedConnection);
        return;
    }
    if (m_running.load()) return;
    {
        QMutexLocker lock(&m_mutex);
        m_buffer.clear(); m_recent.clear(); m_dropped.store(0);
        m_rttIn = m_rttOut = 0; m_feedRttPoor = false;
    }
    if (!m_hasPlay || m_playId != playId) {
        m_hasPlay = true; m_playId = playId;
        m_rollups.store(0); m_badEpisodes.store(0); m_badSeconds.store(0);
        m_samples.store(0); m_finishedMs.store(0);
        m_badLines = 0; m_lastBadLineMs = -kBadSpacingMs;
        m_playClock.start();
        m_clockOrigin = m_clock ? m_clock() : 0;
    }
    m_badSinceMs.store(-1); m_generation.fetch_add(1);
    m_enginePoor = m_rttPoor = false;
    m_badReason.clear();
    m_running.store(true);
    m_windowStartMs.store(nowMs());
    m_timer->start(m_windowMs);
}
void StreamQualitySampler::finish()
{
    if (!onOwnThread()) {
        QMetaObject::invokeMethod(this, [this]() { finish(); }, Qt::QueuedConnection);
        return;
    }
    if (!m_running.load()) return;
    m_finishedMs.store(nowMs());
    m_running.store(false);
    m_timer->stop();
    closeBadEpisode();
    emitWindow(true);
}
void StreamQualitySampler::feed(const VideoStats& stats)
{
    if (!m_running.load()) return;
    // The render thread never waits for window extraction or another producer.
    if (!m_mutex.tryLock()) { m_dropped.fetch_add(1); return; }
    if (m_buffer.size() < kMaxSamplesPerWindow) { m_buffer.append(stats); m_samples.fetch_add(1); }
    else m_dropped.fetch_add(1);
    if (m_recent.size() == kRecentSamples) m_recent.removeFirst();
    m_recent.append(stats);
    bool changed = false;
    if (!stats.rttMs.present) {
        m_rttIn = m_rttOut = 0;
    } else if (stats.rttMs.value >= kRttThresholdMs) {
        m_rttOut = 0;
        m_rttIn = std::min(kRttInCount, m_rttIn + 1);
        if (!m_feedRttPoor && m_rttIn == kRttInCount) { m_feedRttPoor = true; changed = true; }
    } else {
        m_rttIn = 0;
        m_rttOut = std::min(kRttOutCount, m_rttOut + 1);
        if (m_feedRttPoor && m_rttOut == kRttOutCount) { m_feedRttPoor = false; changed = true; }
    }
    const bool poor = m_feedRttPoor;
    const auto generation = m_generation.load();
    m_mutex.unlock();
    if (changed) {
        // No diagnostics, timers or network calls on the producer thread.
        QMetaObject::invokeMethod(this, [this, poor, generation]() {
            if (generation == m_generation.load()) noteRttState(poor);
        }, Qt::QueuedConnection);
    }
}
qint64 StreamQualitySampler::nowMs() const
{
    if (!m_running.load()) return m_finishedMs.load();
    return m_clock ? m_clock() - m_clockOrigin : m_playClock.elapsed();
}
double StreamQualitySampler::badSeconds() const
{
    const auto since = m_badSinceMs.load();
    return m_badSeconds.load() + (since < 0 ? 0.0 : (nowMs() - since) / 1000.0);
}
double StreamQualitySampler::samplingGapS() const
{ return std::max(0.0, nowMs() / 1000.0 - m_samples.load()); }
void StreamQualitySampler::noteConnectionStatus(int status)
{
    if (!onOwnThread()) {
        QMetaObject::invokeMethod(this, [this, status]() { noteConnectionStatus(status); }, Qt::QueuedConnection);
        return;
    }
    if (!m_running.load()) return;
    m_enginePoor = status == 1; // Limelight.h: OKAY=0, POOR=1
    updateQuality(QStringLiteral("engine_poor"));
}
void StreamQualitySampler::noteRttState(bool poor)
{
    if (!m_running.load()) return;
    m_rttPoor = poor;
    updateQuality(QStringLiteral("rtt_high"));
}
void StreamQualitySampler::closeBadEpisode()
{
    const auto since = m_badSinceMs.exchange(-1);
    if (since >= 0) m_badSeconds.store(m_badSeconds.load() + (nowMs() - since) / 1000.0);
}
void StreamQualitySampler::updateQuality(const QString& reason)
{
    const bool poor = m_enginePoor || m_rttPoor;
    if (poor && m_badSinceMs.load() < 0) {
        const auto now = nowMs();
        m_badSinceMs.store(now); m_badEpisodes.fetch_add(1); m_badReason = reason;
        if (m_badLines >= kBadCapPerPlay || now - m_lastBadLineMs < kBadSpacingMs) {
            m_badReason.clear();
            return;
        }
        QVector<VideoStats> recent;
        { QMutexLocker lock(&m_mutex); recent = m_recent; }
        auto attrs = aggregate(recent, 0, true, kRecentSamples);
        attrs.insert("reason", reason);
        SeatHubTelemetry::emitDiagnostic(QStringLiteral("stream.quality_bad"), LogLevel::Warning, attrs);
        ++m_badLines; m_lastBadLineMs = now;
    } else if (!poor && m_badSinceMs.load() >= 0) {
        // A silently suppressed episode has no shipped bad record to recover. Keep its
        // counters, but do not flood the run budget with orphan recovery lines.
        const bool shippedEpisode = !m_badReason.isEmpty();
        const double seconds = (nowMs() - m_badSinceMs.load()) / 1000.0;
        closeBadEpisode();
        if (shippedEpisode) {
            SeatHubTelemetry::emitDiagnostic(QStringLiteral("stream.quality_ok"), LogLevel::Info,
                                            {{"reason", m_badReason}, {"bad_s", rounded(seconds, 2)}});
        }
    }
}
QJsonObject StreamQualitySampler::takeWindow(bool partial)
{
    QVector<VideoStats> samples;
    int dropped;
    { QMutexLocker lock(&m_mutex); samples.swap(m_buffer); dropped = m_dropped.exchange(0); }
    const double seconds = partial ? std::max(0.001, (nowMs() - m_windowStartMs.load()) / 1000.0)
                                   : m_windowMs / 1000.0;
    return aggregate(samples, dropped, partial, seconds);
}
void StreamQualitySampler::tick()
{
    if (!m_running.load()) return;
    emitWindow(false);
    m_windowStartMs.store(nowMs());
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
