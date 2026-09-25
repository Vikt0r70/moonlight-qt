#include "stream_stats.h"

#include <QRegularExpression>
#include <QRegularExpressionMatch>

#include <SDL.h>

namespace {

// `FFmpegVideoDecoder::logVideoStats()`'s own title (`app/streaming/video/ffmpeg.cpp:296`),
// verbatim.
const char* const kTitle = "Global video stats";

// `FFmpegVideoDecoder::logVideoStats()`'s own dashes line (`app/streaming/video/ffmpeg.cpp:867`),
// verbatim, including the trailing newline the format string carries before `%s`.
const char* const kDashesPrefix = "----------------------------------------------------------\n";

// Every line `stringifyVideoStats()` can write (`app/streaming/video/ffmpeg.cpp:700-856`), each
// matched with `QRegularExpression::MultilineOption` so `^`/`$` bind to one line of the block
// rather than the whole string - the engine writes several of these lines, and each is
// independently present or absent.
const QRegularExpression& renderedFpsPattern()
{
    static const QRegularExpression re(
        QStringLiteral("^Video stream: \\d+x\\d+ (\\d+(?:\\.\\d+)?) FPS \\(Codec: .*\\)$"),
        QRegularExpression::MultilineOption);
    return re;
}

const QRegularExpression& networkDroppedPattern()
{
    static const QRegularExpression re(
        QStringLiteral("^Frames dropped by your network connection: (\\d+(?:\\.\\d+)?)%$"),
        QRegularExpression::MultilineOption);
    return re;
}

const QRegularExpression& jitterDroppedPattern()
{
    static const QRegularExpression re(
        QStringLiteral("^Frames dropped due to network jitter: (\\d+(?:\\.\\d+)?)%$"),
        QRegularExpression::MultilineOption);
    return re;
}

// "Average network latency: %u ms (variance: %u ms)" or "Average network latency: N/A".
const QRegularExpression& networkLatencyPattern()
{
    static const QRegularExpression re(
        QStringLiteral("^Average network latency: (?:(\\d+) ms \\(variance: (\\d+) ms\\)|N/A)$"),
        QRegularExpression::MultilineOption);
    return re;
}

const QRegularExpression& decodeTimePattern()
{
    static const QRegularExpression re(QStringLiteral("^Average decoding time: (\\d+(?:\\.\\d+)?) ms$"),
                                       QRegularExpression::MultilineOption);
    return re;
}

const QRegularExpression& queueTimePattern()
{
    static const QRegularExpression re(
        QStringLiteral("^Average frame queue delay: (\\d+(?:\\.\\d+)?) ms$"),
        QRegularExpression::MultilineOption);
    return re;
}

const QRegularExpression& renderTimePattern()
{
    static const QRegularExpression re(
        QStringLiteral(
            "^Average rendering time \\(including monitor V-sync latency\\): (\\d+(?:\\.\\d+)?) ms$"),
        QRegularExpression::MultilineOption);
    return re;
}

QJsonValue metricValue(const OptionalMetric& metric)
{
    return metric.present ? QJsonValue(metric.value) : QJsonValue(QJsonValue::Null);
}

} // namespace

bool parseVideoStatsBlock(const QString& block, VideoStats* out)
{
    if (out == nullptr) {
        return false;
    }
    *out = VideoStats();

    bool matchedAnyLine = false;

    QRegularExpressionMatch m = renderedFpsPattern().match(block);
    if (m.hasMatch()) {
        out->renderedFps = OptionalMetric::of(m.captured(1).toDouble());
        matchedAnyLine = true;
    }

    m = networkDroppedPattern().match(block);
    if (m.hasMatch()) {
        out->networkDroppedFramePct = OptionalMetric::of(m.captured(1).toDouble());
        matchedAnyLine = true;
    }

    m = jitterDroppedPattern().match(block);
    if (m.hasMatch()) {
        out->jitterDroppedFramePct = OptionalMetric::of(m.captured(1).toDouble());
        matchedAnyLine = true;
    }

    m = networkLatencyPattern().match(block);
    if (m.hasMatch()) {
        matchedAnyLine = true;
        if (!m.captured(1).isEmpty()) {
            out->rttMs = OptionalMetric::of(m.captured(1).toDouble());
            out->rttVarianceMs = OptionalMetric::of(m.captured(2).toDouble());
        }
        // Otherwise the line read "N/A" (`rttString` in `stringifyVideoStats()`): both metrics
        // stay absent, matched but not filled.
    }

    m = decodeTimePattern().match(block);
    if (m.hasMatch()) {
        out->decodeTimeMs = OptionalMetric::of(m.captured(1).toDouble());
        matchedAnyLine = true;
    }

    m = queueTimePattern().match(block);
    if (m.hasMatch()) {
        out->queueTimeMs = OptionalMetric::of(m.captured(1).toDouble());
        matchedAnyLine = true;
    }

    m = renderTimePattern().match(block);
    if (m.hasMatch()) {
        out->renderTimeMs = OptionalMetric::of(m.captured(1).toDouble());
        matchedAnyLine = true;
    }

    return matchedAnyLine;
}

QJsonObject toQualityReport(const VideoStats& stats)
{
    QJsonObject object;
    object.insert(QStringLiteral("rendered_fps"), metricValue(stats.renderedFps));
    object.insert(QStringLiteral("network_dropped_frame_pct"), metricValue(stats.networkDroppedFramePct));
    object.insert(QStringLiteral("jitter_dropped_frame_pct"), metricValue(stats.jitterDroppedFramePct));
    object.insert(QStringLiteral("rtt_ms"), metricValue(stats.rttMs));
    object.insert(QStringLiteral("rtt_variance_ms"), metricValue(stats.rttVarianceMs));
    object.insert(QStringLiteral("decode_time_ms"), metricValue(stats.decodeTimeMs));
    object.insert(QStringLiteral("queue_time_ms"), metricValue(stats.queueTimeMs));
    object.insert(QStringLiteral("render_time_ms"), metricValue(stats.renderTimeMs));
    return object;
}

StatsWatcher::StatsWatcher(QObject* parent) : QObject(parent) {}

LogTee::Sink StatsWatcher::sink()
{
    return [this](LogLevel level, int category, int priority, const QString& text) {
        handle(level, category, priority, text);
    };
}

void StatsWatcher::handle(LogLevel level, int category, int priority, const QString& text)
{
    Q_UNUSED(level);

    if (category != SDL_LOG_CATEGORY_APPLICATION || priority != SDL_LOG_PRIORITY_INFO) {
        // Not the category/priority `logVideoStats()` writes at all - ignored without touching
        // the title-then-block state machine.
        return;
    }

    if (text == QLatin1String(kTitle)) {
        m_previousWasTitle = true;
        return;
    }

    const bool wasTitle = m_previousWasTitle;
    m_previousWasTitle = false;

    if (!wasTitle || !text.startsWith(QLatin1String(kDashesPrefix))) {
        // Either this is not the message right after the title, or it does not start with the
        // dashes line at all - an unrelated APPLICATION/INFO message, or a dashes-prefixed one
        // that arrived with no title immediately before it.
        return;
    }

    const QString block = text.mid(static_cast<int>(qstrlen(kDashesPrefix)));
    VideoStats stats;
    if (parseVideoStatsBlock(block, &stats)) {
        emit videoStatsParsed(stats);
    }
}
