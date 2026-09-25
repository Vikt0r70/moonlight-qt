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

#include <QJsonObject>
#include <QObject>
#include <QString>

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
};

/// Parses the block that follows the dashes line - `stringifyVideoStats()`'s own output, with
/// the "Global video stats" title and the leading dashes line already stripped by the caller
/// (`StatsWatcher` does this before calling in). Returns `false` when the text matches none of
/// the known stats lines at all (an unrelated message the watcher should never have forwarded);
/// a block with some of the engine's own conditional lines absent still returns `true`, with each
/// missing metric left absent rather than defaulted to zero.
bool parseVideoStatsBlock(const QString& block, VideoStats* out);

/// `VideoStats` as the `SessionQualityReport` request body (`docs/spec/openapi.yaml`): each
/// present metric as a JSON number, each absent one as JSON `null` (the schema's own
/// `type: [number, "null"]`).
QJsonObject toQualityReport(const VideoStats& stats);

/// The `LogTee` sink that recognises the two-message "Global video stats" block: it remembers
/// whether the immediately preceding `SDL_LOG_CATEGORY_APPLICATION`/`SDL_LOG_PRIORITY_INFO`
/// message equalled the title exactly, and on the next message of that same category/priority
/// starting with `stringifyVideoStats()`'s own dashes line, parses the remainder and emits
/// `videoStatsParsed`. A message of a different category or priority is ignored without
/// disturbing the "previous was the title" state (Qt lines, other SDL categories and other SDL
/// priorities of the SAME category are frequent during a session); any OTHER
/// `SDL_LOG_CATEGORY_APPLICATION`/`SDL_LOG_PRIORITY_INFO` message - including one that itself
/// happens to start with the dashes line, with no title immediately before it - resets the state
/// and is never treated as the block.
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

    bool m_previousWasTitle = false;
};
