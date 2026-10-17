#pragma once

// stream_stats (D-17, Plan 15 Task 1): parses Moonlight's own end-of-stream "Global video stats"
// block into `SessionQualityReport`'s eight fields (`docs/spec/openapi.yaml`, contract 3.1.0).
//
// The block is two `SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, ...)` calls
// `FFmpegVideoDecoder::logVideoStats()` makes as the decoder is destroyed at the end of a stream
// (`app/streaming/video/ffmpeg.cpp:858-870`, read-only, called once from `~FFmpegVideoDecoder()`
// at line 296 with the fixed title `"Global video stats"`): the title, then a message starting
// with a fixed dashes line followed by `stringifyVideoStats()`'s own text
// (`app/streaming/video/ffmpeg.cpp:700-856`, read-only). No engine file is read at runtime and
// none is edited by this plan - `StatsWatcher` is a `LogTee` sink, and `parseVideoStatsBlock()` is
// a pure text parser matched against those exact, unedited line formats.

#include <QElapsedTimer>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>
#include <vector>

#include "log_tee.h"

/// One field the contract's `SessionQualityReport` may or may not have a value for
/// (`docs/spec/openapi.yaml`: "every metric is nullable"). A hand-rolled nullable `double` in
/// place of `std::optional<double>`: this fork's `app/app.pro` declares `CONFIG += c++11`
/// (unmodified upstream, and this plan's own `FORK-CHANGES.md` promise for `app.pro` is
/// "additions only"), and MSVC's `<optional>` refuses to compile without an explicit
/// `/std:c++17` project-wide change - `#error <optional> requires C++17 or later` in
/// `<yvals_core.h>` gates every C++17-only STL header the same way, and nothing else in
/// `app/seathub/` uses one. `present == false` carries the same "this metric is absent" meaning
/// `std::optional`'s empty state would.
struct OptionalMetric
{
    bool present = false;
    double value = 0.0;

    static OptionalMetric of(double v) { return OptionalMetric{ true, v }; }
    static OptionalMetric none() { return OptionalMetric{}; }

    bool operator==(const OptionalMetric& other) const
    {
        return present == other.present && (!present || value == other.value);
    }
};

/// Every field `SessionQualityReport` names, in the contract's own order
/// (`docs/spec/openapi.yaml`). Each is independently absent.
struct VideoStats
{
    OptionalMetric renderedFps;
    OptionalMetric networkDroppedFramePct;
    OptionalMetric jitterDroppedFramePct;
    OptionalMetric rttMs;
    OptionalMetric rttVarianceMs;
    OptionalMetric decodeTimeMs;
    OptionalMetric queueTimeMs;
    OptionalMetric renderTimeMs;

    /// WR-09 (code review 06.3-REVIEW-fork.md, second review pass): `Incoming frame rate from
    /// network:`/`Decoding frame rate:` (`app/streaming/video/ffmpeg.cpp:791-796`, read-only),
    /// parsed for `VideoStatsAggregator::aggregate()`'s own frame-weighting ONLY - neither field
    /// is one of `SessionQualityReport`'s eight and neither is sent on the wire
    /// (`toQualityReport()` does not reference either). Printed in the same conditional block as
    /// `renderedFps` (gated on `stats.receivedFps > 0`), so a segment that carries one carries
    /// all three.
    OptionalMetric receivedFps;
    OptionalMetric decodedFps;

    /// D-10 (Plan 16, `06.6-16-PLAN.md`): parse-only fields for the in-stream stats overlay's
    /// HOST row and total-latency sum, and the RES row's resolution.
    /// ADR-0101 (T024/T025): host latency min, max, and avg are also sent in SessionQualityReport.
    OptionalMetric hostProcessingMinMs;
    OptionalMetric hostProcessingMaxMs;
    OptionalMetric hostProcessingAvgMs;
    OptionalMetric videoWidth;
    OptionalMetric videoHeight;
};

/// Parses the block that follows the dashes line - `stringifyVideoStats()`'s own output, with
/// the "Global video stats" title and the leading dashes line already stripped by the caller
/// (`StatsWatcher` does this before calling in). Returns `false` when the text matches none of
/// the known stats lines at all (an unrelated message the watcher should never have forwarded);
/// a block with some of the engine's own conditional lines absent still returns `true`, with each
/// missing metric left absent rather than defaulted to zero.
bool parseVideoStatsBlock(const QString& block, VideoStats* out);

/// D-10 (Plan 16): the stats overlay's total latency - the sum of every measured part Moonlight
/// prints (the network round trip, the host processing average when present, decode, queue and
/// render), rounded to whole ms (`docs/spec/screens.md` §25, the owner: "sum it and add it").
/// Absent whenever `stats.rttMs` is absent ("With no network figure there is no total") - a
/// missing host-processing line (a Sunshine host that does not report it) is simply omitted from
/// the sum, never treated as zero.
OptionalMetric totalLatencyMs(const VideoStats& stats);

/// `VideoStats` as the `SessionQualityReport` request body (`docs/spec/openapi.yaml`): each
/// present metric as a JSON number, each absent one as JSON `null` (the schema's own
/// `type: [number, "null"]`).
QJsonObject toQualityReport(const VideoStats& stats);

/// The `LogTee` sink that recognises the "Global video stats" block (WR-05, code review
/// 06.3-REVIEW-fork.md): a message of `SDL_LOG_CATEGORY_APPLICATION`/`SDL_LOG_PRIORITY_INFO`
/// whose text starts with `stringifyVideoStats()`'s own dashes line. `logVideoStats()`
/// (`app/streaming/video/ffmpeg.cpp:858-870`, read-only) is the only place in the whole engine
/// tree that ever prints that exact 60-dash line (confirmed by grep against
/// `app/streaming/`), so the content alone identifies the block - `parseVideoStatsBlock()`'s own
/// `matchedAnyLine` result additionally refuses anything that is not at least one of the known
/// stats lines.
///
/// An earlier revision instead required the dashes-prefixed message to be the message
/// *immediately following* the block's own title (`"Global video stats"`, logged one call
/// earlier at ffmpeg.cpp:864-865), tracked with one `bool` member. That state was racy: upstream
/// deletes the decoder - and so logs this block - while moonlight-common-c's own connection,
/// audio and input threads are still running (`session.cpp:2319-2320`'s own comment: "This must
/// happen before `LiStopConnection()`"), and any one of them logging an
/// `SDL_LOG_CATEGORY_APPLICATION`/`SDL_LOG_PRIORITY_INFO` message between the title and the body
/// - two separate `SDL_LogInfo()` calls, on whichever thread the decoder is destroyed on - reset
/// the flag and silently dropped the whole block. Matching on the block's own content removes the
/// race instead of hardening the state machine that caused it: there is no cross-message state
/// left to race on.
class StatsWatcher : public QObject
{
    Q_OBJECT

public:
    explicit StatsWatcher(QObject* parent = nullptr);

    /// The `LogTee::Sink` to register (`LogTee::addSink`). Bound to this instance; the caller is
    /// responsible for `LogTee::removeSink()` before this object is destroyed
    /// (`LogTee::addSink()`'s own header comment).
    LogTee::Sink sink();

signals:
    void videoStatsParsed(VideoStats stats);

private:
    void handle(LogLevel level, int category, int priority, const QString& text);
};

/// WR-04 (code review 06.3-REVIEW-fork.md, ruling: fixed now): upstream recreates the video
/// decoder mid-stream - on a display move or resize, on `SDL_RENDER_DEVICE_RESET`/
/// `SDL_RENDER_TARGETS_RESET`, and on every fullscreen toggle on Windows
/// (`app/streaming/session.cpp:2156-2174`, `:1432-1446`, read-only) - and each `FFmpegVideoDecoder`
/// instance logs its own "Global video stats" block when it is destroyed. Without this, the
/// report posted at teardown describes only the last decoder instance, not the session (a
/// fullscreen toggle one minute before the end would report only that last minute).
///
/// This collects every segment's block for the current session and combines them into one
/// `VideoStats` at teardown.
///
/// WR-09 (code review 06.3-REVIEW-fork.md, second review pass): the ruling's own words are
/// "average times weighted by frames", and each averaged metric in the engine's own text IS a
/// ratio over a frame count, not a plain rate over wall-clock time - `decodeTimeMs` is
/// `totalDecodeTime / decodedFrames`, `queueTimeMs`/`renderTimeMs` are `/ renderedFrames`,
/// `networkDroppedFramePct` is `/ totalFrames` (tracked here by `receivedFps`, the printed rate
/// closest to it - `stringifyVideoStats()` prints no `totalFps` line at all) and
/// `jitterDroppedFramePct` is `/ decodedFrames` (`app/streaming/video/ffmpeg.cpp:836-847`,
/// read-only). The printed block carries no frame COUNT of its own, but it does carry the
/// matching RATE for every one of those (`Incoming frame rate from network`/`Decoding frame
/// rate`/`Rendering frame rate`, `:791-796`) - `VideoStats::receivedFps`/`decodedFps` above, and
/// `renderedFps` itself - and `rate x durationMs` recovers the count a segment's own weight
/// should be. `renderedFps` is the one metric here that is ITSELF a rate over wall-clock time,
/// not a ratio over a frame count, so it keeps plain duration weighting - the two coincide
/// exactly on a single segment either way. `rttMs`/`rttVarianceMs` are excluded from all of this
/// and reported from the longest segment instead (`aggregate()`'s own comment says why).
class VideoStatsAggregator
{
public:
    /// One segment's own parsed block and its measured duration. Public so `aggregate()`'s own
    /// duration-weighting helper (`stream_stats.cpp`, an anonymous-namespace free function) can
    /// read it without being a member.
    struct Segment
    {
        VideoStats stats;
        qint64 durationMs = 0;
    };

    /// Forgets every segment held so far and (re)starts the duration clock. Called once per
    /// session, at `SeatHubClient::handleConnectionStarted()` - the moment the stream truly
    /// begins, not `beginSession()`, which can run well before the first frame while pairing and
    /// connecting happen and would otherwise overstate the first segment's own duration.
    void start();

    /// One decoder segment's own end-of-stream block, as `StatsWatcher` parsed it. Measures the
    /// segment's duration as the wall-clock time since `start()` or the previous `addSegment()`
    /// call.
    void addSegment(const VideoStats& stats);

    /// Test seam: appends a segment with an explicit duration instead of the real elapsed-time
    /// clock, so the weighting can be asserted deterministically. Production never calls this -
    /// see `addSegment()`.
    void addSegmentForTesting(const VideoStats& stats, qint64 durationMs);

    bool hasSegments() const { return !m_segments.empty(); }

    /// One `VideoStats` describing the whole session so far: with a single segment, that
    /// segment's own numbers, unchanged. With more than one (WR-09): `renderedFps` is a
    /// duration-weighted average; `decodeTimeMs`/`jitterDroppedFramePct` are weighted by
    /// `decodedFps x durationMs`, `networkDroppedFramePct` by `receivedFps x durationMs`, and
    /// `queueTimeMs`/`renderTimeMs` by `renderedFps x durationMs` - each the frame count the
    /// engine's own averaged metric is really a ratio over (this class's own header comment says
    /// which is which and why). `rttMs`/`rttVarianceMs` are excluded from all of this -
    /// `stringifyVideoStats()` prints those from `LiGetEstimatedRttInfo()`'s own instantaneous
    /// read at the moment the segment ended (`app/streaming/video/ffmpeg.cpp:674`), a point
    /// sample despite the printed "Average" label, so summing or weighting several of them would
    /// not describe anything real; they are reported from the longest (by duration) segment
    /// instead. A metric absent from every segment that held a weight stays absent, never
    /// defaulted to zero.
    VideoStats aggregate() const;

private:
    QElapsedTimer m_timer;
    std::vector<Segment> m_segments;
};

// ---------------------------------------------------------------- delivered stream time (HI02)

/// One stream segment as the reporter sends it (`StreamTimeReport`, ADR-0074 W04): the key
/// `segmentNo` (the first segment of a Play is 1, each reconnect starts the next number), the
/// milliseconds the app measured for it so far, and whether it has ended.
struct StreamSegmentReport
{
    int segmentNo = 0;
    qint64 deliveredMs = 0;
    bool closed = false;
};

/// What `DeliveredStreamTime::pending()` hands the reporter: the session and account the clock is
/// bound to, and every segment whose last state the server has not yet confirmed - an open one at
/// its running time, a closed one at its final value.
struct StreamTimeSnapshot
{
    QString sessionId;
    QString accountId;
    QVector<StreamSegmentReport> segments;
};

/// The delivered-stream-time clock (ADR-0074 item 6, `docs/spec/client.md` § Stream Liveness and
/// Reconnect, "Delivered stream time", HI02).
///
/// For the history rule only (distinct-rig history qualifies on streamed time strictly greater
/// than 30 minutes), the app measures the time its own stream actually ran. It reads no stats
/// overlay and no statistics value - there is no member here that takes a `VideoStats` - so showing
/// or hiding the overlay (Moonlight's stats hotkey) cannot change it, and it never uses billed
/// minutes, the wall-clock `stream_s` or the Node Agent's Busy minutes.
///
/// A segment starts when the engine reports its connection started (`connectionStarted()`) and ends
/// at the first of three engine log lines, which `sink()` reads from the one `LogTee`:
///
///   * `Quit event received` (`app/streaming/session.cpp`, the customer quitting or the end of any
///     stream),
///   * `Connection terminated: <code>` (`Session::clConnectionTerminated`), or
///   * `Control stream received unexpected disconnect event` (`moonlight-common-c`
///     `ControlStream.c`, after which the library reports a termination of -1).
///
/// When the segment ends on either of the last two with a termination code other than 0, the
/// engine's 10-second control-stream wait (`enet_peer_timeout(peer, 2, 10000, 10000)` in
/// `ControlStream.c`) is taken off the end - the connection was already gone that long ago - but a
/// segment never ends before its own start. A graceful engine end (code 0) and the customer's own
/// quit are not reduced. The first line closes the segment; the lines that follow it (the engine
/// reports one loss as a disconnect line, then a termination line, then the quit event) find no open
/// segment and change nothing.
///
/// Each segment is timed on the monotonic clock in whole milliseconds. A reconnect starts a new
/// segment with the next number; the seconds between segments (noticing the loss, reconnect
/// attempts, grace, pairing) belong to no segment. Everything is read from signals and log lines
/// SeatHub already receives; no engine file changes.
///
/// Thread safety: `sink()` runs on whichever thread logs (the engine's connection and control
/// threads, the SDL main thread), `pending()` and `settle()` on the network thread, and
/// `connectionStarted()` on the facade's. One mutex guards the state; the segment-closed hook is
/// called with it released.
class DeliveredStreamTime
{
public:
    /// `moonlight-common-c` `ControlStream.c`: `enet_peer_timeout(peer, 2, 10000, 10000)`.
    static constexpr qint64 kControlStreamWaitMs = 10000;

    /// A monotonic millisecond clock. The default is `QElapsedTimer`; a test supplies its own.
    using Clock = std::function<qint64()>;

    /// How a recognised engine line ends a segment (`classifyEngineLine()`).
    enum class EngineLine
    {
        None,
        QuitEvent,
        UnexpectedDisconnect,
        ConnectionTerminated,
    };

    explicit DeliveredStreamTime(Clock clock = {});

    /// Binds the clock to the app's own current session and the signed-in account. The same
    /// session again changes nothing (segment numbers keep counting); a different one starts a
    /// fresh count and forgets the previous session's segments (they were persisted when they were
    /// written, `StreamTimeOutbox`). An empty `accountId` is filled in later by `setAccountId()`.
    void beginSession(const QString& sessionId, const QString& accountId);
    /// Fills the account only when none is set yet; never replaces one.
    void setAccountId(const QString& accountId);
    /// Forgets the session and its segments (sign-out, or the session being detached).
    void endSession();
    QString sessionId() const;
    QString accountId() const;

    /// The engine reported its connection started: opens the next segment and returns its number.
    /// A segment still open (its end line was missed) is closed first, at this instant and
    /// unreduced. Returns 0 and records nothing when no session is bound (a local attempt).
    int connectionStarted();

    /// Closes the open segment at this instant, reduced by the control-stream wait when
    /// `connectionLost` is true. Returns whether a segment was open.
    bool closeOpenSegment(bool connectionLost = false);
    bool hasOpenSegment() const;

    /// What one engine line means for a segment, or `None`. `code` receives the termination code
    /// for `ConnectionTerminated`. Trailing line breaks are ignored.
    static EngineLine classifyEngineLine(int category, int priority, const QString& text, int* code);

    /// The `LogTee::Sink` to register (`LogTee::addSink`); the caller must `LogTee::removeSink()`
    /// before this object is destroyed.
    LogTee::Sink sink();

    /// Called (on the thread that closed the segment, with no lock held) after a segment closes, so
    /// the owner can send its final value at once instead of waiting for the next beat. Must not
    /// block.
    void setSegmentClosedHook(std::function<void()> hook);

    /// Every segment the server has not yet confirmed (see `StreamTimeSnapshot`). Empty after
    /// `refuse()`.
    StreamTimeSnapshot pending() const;
    /// A closed segment's final value reached the server (or was refused for good): it is not sent
    /// again. Ignored for a different session than the one bound now, for an open segment, and for
    /// a value that is not the segment's final one.
    void settle(const QString& sessionId, int segmentNo, qint64 deliveredMs);
    /// The server refused this session's reports for good (404 or 422): nothing more is reported
    /// for it. Ignored for a different session than the one bound now.
    void refuse(const QString& sessionId);

    /// The sum of every segment of the bound session, an open one at its running time. For tests
    /// and diagnostics; the server computes its own total from the reports.
    qint64 totalMs() const;

private:
    struct Segment
    {
        int no = 0;
        qint64 startMs = 0;
        qint64 deliveredMs = 0;
        bool closed = false;
        bool settled = false;
    };

    qint64 nowMs() const;
    bool closeLocked(qint64 reduceMs);
    void fireClosed();
    bool handleEngineLine(int category, int priority, const QString& text);

    Clock m_clock;
    QElapsedTimer m_origin;
    mutable QMutex m_mutex;
    QString m_sessionId;
    QString m_accountId;
    QVector<Segment> m_segments;
    int m_open = -1;
    int m_nextNo = 1;
    bool m_refused = false;
    std::function<void()> m_closedHook;
};
