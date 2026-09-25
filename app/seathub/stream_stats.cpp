#include "stream_stats.h"

#include <QRegularExpression>
#include <QRegularExpressionMatch>

#include <SDL.h>

namespace {

// `FFmpegVideoDecoder::logVideoStats()`'s own dashes line (`app/streaming/video/ffmpeg.cpp:867`),
// verbatim, including the trailing newline the format string carries before `%s`. The one
// content `StatsWatcher::handle()` matches the block on (WR-05, `stream_stats.h`'s own header
// comment).
const char* const kDashesPrefix = "----------------------------------------------------------\n";

// Every line `stringifyVideoStats()` can write (`app/streaming/video/ffmpeg.cpp:700-856`), each
// matched with `QRegularExpression::MultilineOption` so `^`/`$` bind to one line of the block
// rather than the whole string - the engine writes several of these lines, and each is
// independently present or absent.
//
// CR-01 (code review 06.3-REVIEW-fork.md): `rendered_fps` used to be parsed from the `Video
// stream: ...` line (ffmpeg.cpp:778-792), which is gated on `m_VideoDecoderCtx != nullptr` and is
// never printed in production - `logVideoStats()` runs at ffmpeg.cpp:296, strictly after
// `avcodec_free_context(&m_VideoDecoderCtx)` at ffmpeg.cpp:280, which FFmpeg documents writes
// `nullptr` to the pointer it is given. That line's own number was also the wrong one on its own
// terms: `stats.totalFps`, not the rendered rate. `Rendering frame rate: %.2f FPS` (ffmpeg.cpp:
// 798, 801) is the line that actually carries the rendered rate, and - unlike `Video stream:` -
// it is printed whenever `stats.receivedFps > 0` (ffmpeg.cpp:777), with no decoder-context gate.
const QRegularExpression& renderedFpsPattern()
{
    static const QRegularExpression re(
        QStringLiteral("^Rendering frame rate: (\\d+(?:\\.\\d+)?) FPS$"),
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
        // Not the category/priority `logVideoStats()` writes at all.
        return;
    }

    if (!text.startsWith(QLatin1String(kDashesPrefix))) {
        // Not the block (WR-05: content alone decides now, not adjacency to a title message).
        return;
    }

    const QString block = text.mid(static_cast<int>(qstrlen(kDashesPrefix)));
    VideoStats stats;
    if (parseVideoStatsBlock(block, &stats)) {
        emit videoStatsParsed(stats);
    }
}

// ---------------------------------------------------------------- VideoStatsAggregator (WR-04)

void VideoStatsAggregator::start()
{
    m_segments.clear();
    m_timer.start();
}

void VideoStatsAggregator::addSegment(const VideoStats& stats)
{
    const qint64 durationMs = m_timer.isValid() ? m_timer.restart() : 0;
    m_segments.push_back(Segment{ stats, durationMs });
}

void VideoStatsAggregator::addSegmentForTesting(const VideoStats& stats, qint64 durationMs)
{
    m_segments.push_back(Segment{ stats, durationMs });
}

namespace {

// Duration-weighted average of one metric across every segment that held a value for it. A
// segment with no duration (the clock never started, or restarted at 0) contributes nothing
// rather than a divide-by-zero; a metric absent everywhere stays absent.
OptionalMetric weightedAverage(const std::vector<VideoStatsAggregator::Segment>& segments,
                                OptionalMetric VideoStats::*field)
{
    double weightedSum = 0.0;
    double weightTotal = 0.0;
    for (const VideoStatsAggregator::Segment& segment : segments) {
        const OptionalMetric& metric = segment.stats.*field;
        if (!metric.present || segment.durationMs <= 0) {
            continue;
        }
        weightedSum += metric.value * static_cast<double>(segment.durationMs);
        weightTotal += static_cast<double>(segment.durationMs);
    }
    if (weightTotal <= 0.0) {
        return OptionalMetric::none();
    }
    return OptionalMetric::of(weightedSum / weightTotal);
}

} // namespace

VideoStats VideoStatsAggregator::aggregate() const
{
    VideoStats out;
    if (m_segments.empty()) {
        return out;
    }
    if (m_segments.size() == 1) {
        // Trivial case, and also the common one (most sessions have exactly one decoder
        // instance): the single segment's own numbers, unchanged - equal to what the weighted
        // average below would compute anyway, whatever its duration is.
        return m_segments.front().stats;
    }

    out.renderedFps = weightedAverage(m_segments, &VideoStats::renderedFps);
    out.networkDroppedFramePct = weightedAverage(m_segments, &VideoStats::networkDroppedFramePct);
    out.jitterDroppedFramePct = weightedAverage(m_segments, &VideoStats::jitterDroppedFramePct);
    out.decodeTimeMs = weightedAverage(m_segments, &VideoStats::decodeTimeMs);
    out.queueTimeMs = weightedAverage(m_segments, &VideoStats::queueTimeMs);
    out.renderTimeMs = weightedAverage(m_segments, &VideoStats::renderTimeMs);

    // rtt_ms / rtt_variance_ms: a point sample, not a real average (see `aggregate()`'s own
    // header comment) - taken from the longest segment that actually held one, not combined. A
    // segment whose latency line read "N/A" (`rttMs.present == false`) is not a candidate at all,
    // the same "absent, never defaulted" rule every other metric here follows - the longest
    // segment overall may be exactly the one with no RTT sample.
    const Segment* longestWithRtt = nullptr;
    for (const Segment& segment : m_segments) {
        if (!segment.stats.rttMs.present) {
            continue;
        }
        if (longestWithRtt == nullptr || segment.durationMs > longestWithRtt->durationMs) {
            longestWithRtt = &segment;
        }
    }
    if (longestWithRtt != nullptr) {
        out.rttMs = longestWithRtt->stats.rttMs;
        out.rttVarianceMs = longestWithRtt->stats.rttVarianceMs;
    }

    return out;
}
