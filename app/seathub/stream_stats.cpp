#include "stream_stats.h"

#include <QMutexLocker>
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
// D-10 (Plan 16): "Video stream: %dx%d %.2f FPS (Codec: %s)" (`ffmpeg.cpp:778, 792`) - only the
// resolution is captured; the rendered rate and codec are read from elsewhere (CR-01: this line
// is gated on `m_VideoDecoderCtx != nullptr` and never printed in the end-of-stream block, but it
// IS printed in the live `OverlayDebug` text this compositor parses, since the decoder context is
// still valid there).
const QRegularExpression& videoStreamPattern()
{
    static const QRegularExpression re(
        QStringLiteral("^Video stream: (\\d+)x(\\d+) .*$"), QRegularExpression::MultilineOption);
    return re;
}

const QRegularExpression& renderedFpsPattern()
{
    static const QRegularExpression re(
        QStringLiteral("^Rendering frame rate: (\\d+(?:\\.\\d+)?) FPS$"),
        QRegularExpression::MultilineOption);
    return re;
}

// WR-09 (code review 06.3-REVIEW-fork.md, second review pass): printed in the same conditional
// block as `Rendering frame rate:` above (ffmpeg.cpp:791-796, gated on `stats.receivedFps > 0`) -
// parsed for `VideoStatsAggregator::aggregate()`'s own frame-weighting only; neither line is one
// of `SessionQualityReport`'s fields and neither is sent on the wire (`toQualityReport()` does
// not reference `VideoStats::receivedFps`/`decodedFps` at all).
const QRegularExpression& receivedFpsPattern()
{
    static const QRegularExpression re(
        QStringLiteral("^Incoming frame rate from network: (\\d+(?:\\.\\d+)?) FPS$"),
        QRegularExpression::MultilineOption);
    return re;
}

const QRegularExpression& decodedFpsPattern()
{
    static const QRegularExpression re(
        QStringLiteral("^Decoding frame rate: (\\d+(?:\\.\\d+)?) FPS$"),
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

// D-10 (Plan 16), ADR-0101 (T024/T025): "Host processing latency min/max/average: %.1f/%.1f/%.1f ms"
// (`ffmpeg.cpp:811-819`, gated on `stats.framesWithHostProcessingLatency > 0`). Captures min, max, avg.
const QRegularExpression& hostProcessingLatencyPattern()
{
    static const QRegularExpression re(
        QStringLiteral("^Host processing latency min/max/average: (\\d+(?:\\.\\d+)?)/(\\d+(?:\\.\\d+)?)/"
                       "(\\d+(?:\\.\\d+)?) ms$"),
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

    QRegularExpressionMatch m = videoStreamPattern().match(block);
    if (m.hasMatch()) {
        out->videoWidth = OptionalMetric::of(m.captured(1).toDouble());
        out->videoHeight = OptionalMetric::of(m.captured(2).toDouble());
        matchedAnyLine = true;
    }

    m = renderedFpsPattern().match(block);
    if (m.hasMatch()) {
        out->renderedFps = OptionalMetric::of(m.captured(1).toDouble());
        matchedAnyLine = true;
    }

    // WR-09: not part of the report body (`toQualityReport()` never reads either), parsed only
    // so `VideoStatsAggregator::aggregate()` can weight this segment's other metrics by the
    // matching frame count.
    m = receivedFpsPattern().match(block);
    if (m.hasMatch()) {
        out->receivedFps = OptionalMetric::of(m.captured(1).toDouble());
        matchedAnyLine = true;
    }

    m = decodedFpsPattern().match(block);
    if (m.hasMatch()) {
        out->decodedFps = OptionalMetric::of(m.captured(1).toDouble());
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

    m = hostProcessingLatencyPattern().match(block);
    if (m.hasMatch()) {
        out->hostProcessingMinMs = OptionalMetric::of(m.captured(1).toDouble());
        out->hostProcessingMaxMs = OptionalMetric::of(m.captured(2).toDouble());
        out->hostProcessingAvgMs = OptionalMetric::of(m.captured(3).toDouble());
        matchedAnyLine = true;
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
    // ADR-0101 / T024: rtt_ms is integer, rounded half-up
    object.insert(QStringLiteral("rtt_ms"), stats.rttMs.present
                                                ? QJsonValue(static_cast<qint64>(qRound(stats.rttMs.value)))
                                                : QJsonValue(QJsonValue::Null));
    object.insert(QStringLiteral("rtt_variance_ms"), metricValue(stats.rttVarianceMs));
    object.insert(QStringLiteral("decode_time_ms"), metricValue(stats.decodeTimeMs));
    object.insert(QStringLiteral("queue_time_ms"), metricValue(stats.queueTimeMs));
    object.insert(QStringLiteral("render_time_ms"), metricValue(stats.renderTimeMs));
    object.insert(QStringLiteral("received_fps"), metricValue(stats.receivedFps));
    object.insert(QStringLiteral("decoded_fps"), metricValue(stats.decodedFps));
    object.insert(QStringLiteral("host_latency_min_ms"), metricValue(stats.hostProcessingMinMs));
    object.insert(QStringLiteral("host_latency_max_ms"), metricValue(stats.hostProcessingMaxMs));
    object.insert(QStringLiteral("host_latency_avg_ms"), metricValue(stats.hostProcessingAvgMs));
    return object;
}

OptionalMetric totalLatencyMs(const VideoStats& stats)
{
    if (!stats.rttMs.present) {
        // `docs/spec/screens.md` §25: "With no network figure there is no total."
        return OptionalMetric::none();
    }

    double total = stats.rttMs.value;
    if (stats.hostProcessingAvgMs.present) {
        total += stats.hostProcessingAvgMs.value;
    }
    if (stats.decodeTimeMs.present) {
        total += stats.decodeTimeMs.value;
    }
    if (stats.queueTimeMs.present) {
        total += stats.queueTimeMs.value;
    }
    if (stats.renderTimeMs.present) {
        total += stats.renderTimeMs.value;
    }
    return OptionalMetric::of(qRound(total));
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
// rather than a divide-by-zero; a metric absent everywhere stays absent. Correct only for a
// metric that is itself a rate over wall-clock time (`renderedFps` - see `frameWeightedAverage()`
// below for every other averaged metric, WR-09).
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

// WR-09 (code review 06.3-REVIEW-fork.md, second review pass): `field` is a ratio over a frame
// count in the engine's own text (`stream_stats.h`'s own class comment names which count for
// which field); `fpsField` is the matching rate `stringifyVideoStats()` also prints for that same
// segment, so `fpsField.value * durationMs` recovers that count. A segment missing either value,
// or with no duration, contributes nothing - the same "absent, never defaulted to zero" rule
// `weightedAverage()` follows, extended to a missing weight as well as a missing metric (a
// segment cannot be weighted by a frame count it never printed).
OptionalMetric frameWeightedAverage(const std::vector<VideoStatsAggregator::Segment>& segments,
                                     OptionalMetric VideoStats::*field,
                                     OptionalMetric VideoStats::*fpsField)
{
    double weightedSum = 0.0;
    double weightTotal = 0.0;
    for (const VideoStatsAggregator::Segment& segment : segments) {
        const OptionalMetric& metric = segment.stats.*field;
        const OptionalMetric& fps = segment.stats.*fpsField;
        if (!metric.present || !fps.present || segment.durationMs <= 0) {
            continue;
        }
        const double weight = fps.value * static_cast<double>(segment.durationMs);
        if (weight <= 0.0) {
            continue;
        }
        weightedSum += metric.value * weight;
        weightTotal += weight;
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

    // WR-09: `renderedFps` is itself a rate over wall-clock time, so it keeps plain duration
    // weighting. Every other metric here is a ratio over a frame count in the engine's own text
    // (`stream_stats.h`'s own class comment names which count for which), so each is weighted by
    // the matching printed rate x this segment's duration instead - see
    // `frameWeightedAverage()`'s own comment.
    out.renderedFps = weightedAverage(m_segments, &VideoStats::renderedFps);
    out.receivedFps = weightedAverage(m_segments, &VideoStats::receivedFps);
    out.decodedFps = weightedAverage(m_segments, &VideoStats::decodedFps);
    out.networkDroppedFramePct =
        frameWeightedAverage(m_segments, &VideoStats::networkDroppedFramePct, &VideoStats::receivedFps);
    out.jitterDroppedFramePct =
        frameWeightedAverage(m_segments, &VideoStats::jitterDroppedFramePct, &VideoStats::decodedFps);
    out.decodeTimeMs =
        frameWeightedAverage(m_segments, &VideoStats::decodeTimeMs, &VideoStats::decodedFps);
    out.queueTimeMs =
        frameWeightedAverage(m_segments, &VideoStats::queueTimeMs, &VideoStats::renderedFps);
    out.renderTimeMs =
        frameWeightedAverage(m_segments, &VideoStats::renderTimeMs, &VideoStats::renderedFps);

    // Host processing latency (min/max/avg) across segments
    out.hostProcessingAvgMs =
        frameWeightedAverage(m_segments, &VideoStats::hostProcessingAvgMs, &VideoStats::decodedFps);
    bool hasHostMin = false;
    bool hasHostMax = false;
    double minHost = 0.0;
    double maxHost = 0.0;
    for (const Segment& segment : m_segments) {
        if (segment.stats.hostProcessingMinMs.present) {
            if (!hasHostMin || segment.stats.hostProcessingMinMs.value < minHost) {
                minHost = segment.stats.hostProcessingMinMs.value;
                hasHostMin = true;
            }
        }
        if (segment.stats.hostProcessingMaxMs.present) {
            if (!hasHostMax || segment.stats.hostProcessingMaxMs.value > maxHost) {
                maxHost = segment.stats.hostProcessingMaxMs.value;
                hasHostMax = true;
            }
        }
    }
    if (hasHostMin) {
        out.hostProcessingMinMs = OptionalMetric::of(minHost);
    }
    if (hasHostMax) {
        out.hostProcessingMaxMs = OptionalMetric::of(maxHost);
    }

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

// ---------------------------------------------------------------- DeliveredStreamTime (HI02)

namespace {

// The three engine lines that end a segment. Each is the exact text its source writes, up to the
// format argument: `app/streaming/session.cpp:2003` (`SDL_LogInfo ... "Quit event received"`),
// `session.cpp:144-146` (`SDL_LogError ... "Connection terminated: %d"`) and
// `moonlight-common-c/src/ControlStream.c:1302`
// (`Limelog("Control stream received unexpected disconnect event\n")`, which `Session::clLogMessage`
// forwards as `SDL_LOG_PRIORITY_INFO`; SDL chops the final line break, and a trailing one is ignored
// here in case a build keeps it). All three are read-only upstream text.
const QLatin1String kQuitEventLine("Quit event received");
const QLatin1String kUnexpectedDisconnectLine("Control stream received unexpected disconnect event");
const QLatin1String kTerminatedPrefix("Connection terminated: ");

QString withoutTrailingLineBreaks(const QString& text)
{
    int end = text.size();
    while (end > 0 && (text.at(end - 1) == QLatin1Char('\n') || text.at(end - 1) == QLatin1Char('\r'))) {
        --end;
    }
    return text.left(end);
}

constexpr qint64 kMaxSegmentMs = 2147483647;

} // namespace

DeliveredStreamTime::DeliveredStreamTime(Clock clock) : m_clock(std::move(clock))
{
    m_origin.start();
}

qint64 DeliveredStreamTime::nowMs() const
{
    if (m_clock) {
        return m_clock();
    }
    // Whole milliseconds on the monotonic clock, truncated from nanoseconds so the measure is
    // exact to the millisecond and never rounds a segment up.
    return m_origin.nsecsElapsed() / 1000000;
}

void DeliveredStreamTime::beginSession(const QString& sessionId, const QString& accountId)
{
    QMutexLocker locker(&m_mutex);
    if (sessionId == m_sessionId) {
        // The same session again (a repeated begin): segment numbering keeps counting, so a
        // replayed call can never reuse a number for different time.
        if (m_accountId.isEmpty()) {
            m_accountId = accountId;
        }
        return;
    }
    m_sessionId = sessionId;
    m_accountId = accountId;
    m_segments.clear();
    m_open = -1;
    m_nextNo = 1;
    m_refused = false;
}

void DeliveredStreamTime::setAccountId(const QString& accountId)
{
    QMutexLocker locker(&m_mutex);
    if (m_accountId.isEmpty()) {
        m_accountId = accountId;
    }
}

void DeliveredStreamTime::endSession()
{
    QMutexLocker locker(&m_mutex);
    m_sessionId.clear();
    m_accountId.clear();
    m_segments.clear();
    m_open = -1;
    m_nextNo = 1;
    m_refused = false;
}

QString DeliveredStreamTime::sessionId() const
{
    QMutexLocker locker(&m_mutex);
    return m_sessionId;
}

QString DeliveredStreamTime::accountId() const
{
    QMutexLocker locker(&m_mutex);
    return m_accountId;
}

void DeliveredStreamTime::setSegmentClosedHook(std::function<void()> hook)
{
    QMutexLocker locker(&m_mutex);
    m_closedHook = std::move(hook);
}

void DeliveredStreamTime::fireClosed()
{
    std::function<void()> hook;
    {
        QMutexLocker locker(&m_mutex);
        hook = m_closedHook;
    }
    if (hook) {
        hook();
    }
}

int DeliveredStreamTime::connectionStarted()
{
    bool closedPrevious = false;
    int number = 0;
    {
        QMutexLocker locker(&m_mutex);
        if (m_sessionId.isEmpty()) {
            return 0;
        }
        // A segment still open means its end line never arrived. The new connection proves it
        // ended; it is closed at this instant and not reduced (nothing says it was lost).
        closedPrevious = closeLocked(0);
        Segment segment;
        segment.no = m_nextNo++;
        segment.startMs = nowMs();
        m_segments.push_back(segment);
        m_open = m_segments.size() - 1;
        number = segment.no;
    }
    if (closedPrevious) {
        fireClosed();
    }
    return number;
}

bool DeliveredStreamTime::closeLocked(qint64 reduceMs)
{
    if (m_open < 0) {
        return false;
    }
    Segment& segment = m_segments[m_open];
    // The reduction can never take a segment to before its own start.
    const qint64 ran = qMax<qint64>(0, nowMs() - segment.startMs - reduceMs);
    segment.deliveredMs = qMin(ran, kMaxSegmentMs);
    segment.closed = true;
    m_open = -1;
    return true;
}

bool DeliveredStreamTime::closeOpenSegment(bool connectionLost)
{
    bool closed = false;
    {
        QMutexLocker locker(&m_mutex);
        closed = closeLocked(connectionLost ? kControlStreamWaitMs : 0);
    }
    if (closed) {
        fireClosed();
    }
    return closed;
}

bool DeliveredStreamTime::hasOpenSegment() const
{
    QMutexLocker locker(&m_mutex);
    return m_open >= 0;
}

DeliveredStreamTime::EngineLine DeliveredStreamTime::classifyEngineLine(int category, int priority,
                                                                        const QString& text,
                                                                        int* code)
{
    if (category != SDL_LOG_CATEGORY_APPLICATION) {
        return EngineLine::None;
    }
    const QString line = withoutTrailingLineBreaks(text);

    if (priority == SDL_LOG_PRIORITY_INFO) {
        if (line == kQuitEventLine) {
            return EngineLine::QuitEvent;
        }
        if (line == kUnexpectedDisconnectLine) {
            return EngineLine::UnexpectedDisconnect;
        }
        return EngineLine::None;
    }

    if (priority == SDL_LOG_PRIORITY_ERROR && line.startsWith(kTerminatedPrefix)) {
        // `%d`: an optional minus sign and decimal digits, to the end of the line, in `int`.
        static const QRegularExpression number(QStringLiteral("\\A-?[0-9]+\\z"));
        const QString digits = line.mid(kTerminatedPrefix.size());
        bool ok = false;
        const int value = digits.toInt(&ok);
        if (ok && number.match(digits).hasMatch()) {
            if (code != nullptr) {
                *code = value;
            }
            return EngineLine::ConnectionTerminated;
        }
    }
    return EngineLine::None;
}

bool DeliveredStreamTime::handleEngineLine(int category, int priority, const QString& text)
{
    int code = 0;
    switch (classifyEngineLine(category, priority, text, &code)) {
    case EngineLine::None:
        return false;
    case EngineLine::QuitEvent:
        // The customer quitting, or the end of any stream: the stream ran until now.
        return closeOpenSegment(false);
    case EngineLine::UnexpectedDisconnect:
        // The library reports this loss as a termination of -1 right after: a lost connection.
        return closeOpenSegment(true);
    case EngineLine::ConnectionTerminated:
        // 0 is `ML_ERROR_GRACEFUL_TERMINATION`, a real code and a clean end: not reduced.
        return closeOpenSegment(code != 0);
    }
    return false;
}

LogTee::Sink DeliveredStreamTime::sink()
{
    // A sink never adds or removes sinks, never blocks and never logs (`log_tee.h`): this only
    // classifies the line and, for a match, takes the segment's end under the mutex.
    return [this](LogLevel level, int category, int priority, const QString& text) {
        Q_UNUSED(level);
        handleEngineLine(category, priority, text);
    };
}

StreamTimeSnapshot DeliveredStreamTime::pending() const
{
    QMutexLocker locker(&m_mutex);
    StreamTimeSnapshot snapshot;
    snapshot.sessionId = m_sessionId;
    snapshot.accountId = m_accountId;
    if (m_sessionId.isEmpty() || m_refused) {
        return snapshot;
    }
    const qint64 now = nowMs();
    for (int i = 0; i < m_segments.size(); ++i) {
        const Segment& segment = m_segments.at(i);
        if (segment.settled) {
            continue;
        }
        StreamSegmentReport report;
        report.segmentNo = segment.no;
        report.closed = segment.closed;
        report.deliveredMs = segment.closed
            ? segment.deliveredMs
            : qMin(qMax<qint64>(0, now - segment.startMs), kMaxSegmentMs);
        snapshot.segments.push_back(report);
    }
    return snapshot;
}

void DeliveredStreamTime::settle(const QString& sessionId, int segmentNo, qint64 deliveredMs)
{
    QMutexLocker locker(&m_mutex);
    if (sessionId.isEmpty() || sessionId != m_sessionId) {
        return;
    }
    for (Segment& segment : m_segments) {
        if (segment.no == segmentNo && segment.closed && segment.deliveredMs == deliveredMs) {
            segment.settled = true;
            return;
        }
    }
}

void DeliveredStreamTime::refuse(const QString& sessionId)
{
    QMutexLocker locker(&m_mutex);
    if (!sessionId.isEmpty() && sessionId == m_sessionId) {
        m_refused = true;
    }
}

qint64 DeliveredStreamTime::totalMs() const
{
    QMutexLocker locker(&m_mutex);
    const qint64 now = nowMs();
    qint64 total = 0;
    for (const Segment& segment : m_segments) {
        total += segment.closed ? segment.deliveredMs : qMax<qint64>(0, now - segment.startMs);
    }
    return total;
}
