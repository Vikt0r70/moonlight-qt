/*****************************************************************************
 * SeatHub fork - unit tests for the one log tee, the end-of-stream video-stats parser, and the
 * engine termination matcher (Plan 15, D-17/A-51, ADR-0044).
 *
 * Three things are pinned down here:
 *
 *   1. `LogTee` is the ONE place that installs the Qt message handler and the SDL log output
 *      function: it chains whatever was installed before it, and a same-thread re-entry (a
 *      sink's own logging) is stopped from recursing back into the sink list.
 *   2. `parseVideoStatsBlock()` matches every line `FFmpegVideoDecoder::stringifyVideoStats()`
 *      can write (`app/streaming/video/ffmpeg.cpp:700-856`, read-only), including the "N/A"
 *      network-latency case and a block with a conditional line missing; `StatsWatcher` only
 *      recognises the block as the SDL info message immediately after the exact title.
 *   3. `parseConnectionTerminated()` matches upstream's own `"Connection terminated: %d"` line
 *      (`app/streaming/session.cpp:144-146`, read-only) exactly - category, priority, prefix
 *      case, and every malformed-number case - and, wired the way `SeatHubClient`'s constructor
 *      wires it, reaches `LivenessTimer::noteTermination()` once per matching line.
 *****************************************************************************/

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopedPointer>
#include <QSemaphore>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>

// This file defines `main()` via `QTEST_MAIN` below. `<SDL.h>` otherwise `#define`s `main` to
// `SDL_main` (its own cross-platform entry-point redirection), which would rename QTest's
// generated `main()` out from under the linker - the same fix `tst_overlay_injection.cpp` and
// `tst_hud_bitmap.cpp` already use for the same reason.
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "seathub/control_plane_client.h"
#include "seathub/engine_termination.h"
#include "seathub/liveness_timer.h"
#include "seathub/log_tee.h"
#include "seathub/quality_outbox.h"
#include "seathub/stream_stats.h"

namespace {

// The real end-of-stream block `FFmpegVideoDecoder::logVideoStats()` prints in production
// (`app/streaming/video/ffmpeg.cpp:279-296`, read-only, this file's own literal strings copied
// from :794-801, :811-819 and :835-848): every conditional line the engine can print with
// `m_VideoDecoderCtx` already freed. There is NO "Video stream: ..." line - that line is gated on
// `m_VideoDecoderCtx != nullptr` (:778) and `logVideoStats()` runs strictly after
// `avcodec_free_context()` has nulled it (:280, :296) - which is exactly CR-01's finding: the
// earlier fixture here (`fullStatsBlock()`) carried a "Video stream:" line production can never
// emit, which is how a wrong parse source went untested. Field values are chosen so no two are
// equal, so a test that reads the wrong field fails loudly.
//
// IN-08 (code review 06.3-REVIEW-fork.md, second review pass): the "Host processing latency
// min/max/average: ... ms" line (:811-819, gated on `stats.framesWithHostProcessingLatency > 0` -
// a Sunshine host reporting its own encode latency) is between `Rendering frame rate:` and the
// drop-percentage lines here because that is where `stringifyVideoStats()` actually prints it.
// `parseVideoStatsBlock()` has no regex for it and never will (it is not one of
// `SessionQualityReport`'s eight fields) - it is here purely so this fixture matches a Sunshine
// host's real output; the earlier revision of this comment claimed the fixture carried "every
// conditional line the engine can print" while missing this one.
QString productionStatsBlock()
{
    return QStringLiteral(
        "----------------------------------------------------------\n"
        "Incoming frame rate from network: 60.00 FPS\n"
        "Decoding frame rate: 59.98 FPS\n"
        "Rendering frame rate: 59.96 FPS\n"
        "Host processing latency min/max/average: 0.8/3.4/1.6 ms\n"
        "Frames dropped by your network connection: 0.42%\n"
        "Frames dropped due to network jitter: 0.10%\n"
        "Average network latency: 23 ms (variance: 4 ms)\n"
        "Average decoding time: 3.21 ms\n"
        "Average frame queue delay: 1.05 ms\n"
        "Average rendering time (including monitor V-sync latency): 2.77 ms\n");
}

// `productionStatsBlock()` with the network latency line reading "N/A" instead -
// `stringifyVideoStats()`'s own fallback when `stats.lastRtt == 0` (ffmpeg.cpp:828-833).
QString productionStatsBlockWithNoRtt()
{
    return QStringLiteral(
        "----------------------------------------------------------\n"
        "Incoming frame rate from network: 60.00 FPS\n"
        "Decoding frame rate: 59.98 FPS\n"
        "Rendering frame rate: 59.96 FPS\n"
        "Host processing latency min/max/average: 0.8/3.4/1.6 ms\n"
        "Frames dropped by your network connection: 0.42%\n"
        "Frames dropped due to network jitter: 0.10%\n"
        "Average network latency: N/A\n"
        "Average decoding time: 3.21 ms\n"
        "Average frame queue delay: 1.05 ms\n"
        "Average rendering time (including monitor V-sync latency): 2.77 ms\n");
}

const char* const kDashesPrefix = "----------------------------------------------------------\n";

// --- QualityOutbox fixtures ---------------------------------------------------------------

const char* const kSessionA = "11111111-1111-1111-1111-111111111111";
const char* const kSessionB = "22222222-2222-2222-2222-222222222222";

ControlPlaneResult resultWithStatus(int statusCode, bool ok = false)
{
    ControlPlaneResult result;
    result.statusCode = statusCode;
    result.ok = ok;
    return result;
}

QJsonObject sampleReport()
{
    QJsonObject report;
    report.insert(QStringLiteral("rendered_fps"), 59.94);
    return report;
}

int g_previousQtHandlerCalls = 0;
void previousQtHandler(QtMsgType, const QMessageLogContext&, const QString&)
{
    ++g_previousQtHandlerCalls;
}

int g_previousSdlHandlerCalls = 0;
void previousSdlHandler(void*, int, SDL_LogPriority, const char*)
{
    ++g_previousSdlHandlerCalls;
}

} // namespace

class TstStreamStats : public QObject
{
    Q_OBJECT

private slots:
    // --- LogTee. This must run first: `LogTee::install()` only ever captures "the previous
    // handler" on its FIRST call for the whole process (this test binary); every later test in
    // this file that logs anything already runs through the tee installed here. -----------------

    void logTee_install_isIdempotentAndChainsThePreviousHandlerFirst()
    {
        qInstallMessageHandler(previousQtHandler);
        SDL_LogSetOutputFunction(previousSdlHandler, nullptr);

        LogTee::install();
        LogTee::install(); // second call is a documented no-op

        int sinkCalls = 0;
        const LogTee::SinkHandle handle =
            LogTee::addSink([&](LogLevel, int, int, const QString&) { ++sinkCalls; });

        const int qtBefore = g_previousQtHandlerCalls;
        const int sdlBefore = g_previousSdlHandlerCalls;

        qWarning() << "logTee chaining test - qt";
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "logTee chaining test - sdl");

        QVERIFY2(g_previousQtHandlerCalls > qtBefore,
                 "install() must call the handler that was installed before it, unchanged");
        QVERIFY2(g_previousSdlHandlerCalls > sdlBefore,
                 "install() must call the SDL function that was installed before it, unchanged");
        QVERIFY2(sinkCalls >= 2, "both messages must have reached the registered sink");

        LogTee::removeSink(handle);
    }

    void logTee_recursionGuard_stopsASinksOwnLoggingFromReenteringTheSinkList()
    {
        LogTee::clearSinksForTests();

        int callCount = 0;
        const LogTee::SinkHandle handle =
            LogTee::addSink([&](LogLevel, int, int, const QString& text) {
                if (text.contains(QStringLiteral("trigger-recursion-guard-test"))) {
                    ++callCount;
                    // This sink's own logging, while it is running. Without the guard this would
                    // re-enter LogTee's dispatch on this same thread and call this very sink a
                    // second time for the line below.
                    qWarning() << "trigger-recursion-guard-test (re-entrant line)";
                }
            });

        qWarning() << "trigger-recursion-guard-test";

        QCOMPARE(callCount, 1);
        LogTee::removeSink(handle);
    }

    void logTee_removeSink_stopsFurtherDelivery()
    {
        LogTee::clearSinksForTests();

        int callCount = 0;
        const LogTee::SinkHandle handle =
            LogTee::addSink([&](LogLevel, int, int, const QString&) { ++callCount; });

        qWarning() << "before removal";
        const int afterFirst = callCount;
        QVERIFY(afterFirst >= 1);

        LogTee::removeSink(handle);
        qWarning() << "after removal";
        QCOMPARE(callCount, afterFirst);
    }

    // WR-07 (code review 06.3-REVIEW-fork.md): `removeSink()` used to erase the handle from the
    // sink list and return immediately, even while a `dispatch()` already under way on another
    // thread had already copied the list and was still calling this very sink - the header's own
    // promise ("a `removeSink()` call before destruction is sufficient") did not hold. A sink is
    // parked mid-call on a background thread here (having already announced it is running), and
    // `removeSink()` - called on a second background thread, so the test thread itself is never
    // blocked - must not return before the parked sink does.
    void logTee_removeSink_waitsForADispatchAlreadyInFlightOnAnotherThread()
    {
        LogTee::clearSinksForTests();

        QSemaphore entered(0);
        QSemaphore releaseGate(0);
        std::atomic<bool> sinkDone{ false };
        std::atomic<bool> removeReturned{ false };

        const LogTee::SinkHandle handle = LogTee::addSink(
            [&](LogLevel, int, int, const QString& text) {
                if (!text.contains(QStringLiteral("wr07-trigger"))) {
                    return;
                }
                entered.release();
                releaseGate.acquire(); // parked here until the test releases it, below
                sinkDone = true;
            });

        // Thread A: logs the trigger line, which dispatch()es into the sink above and parks
        // there. `QThread::create()` runs the callable directly as the thread's own entry point -
        // no event loop is needed or running on either side, unlike a `QThread::started`
        // connection (which would queue back onto this thread's own, absent, event loop).
        QScopedPointer<QThread> dispatcherThread(QThread::create([&]() { qWarning() << "wr07-trigger"; }));
        dispatcherThread->start();

        QVERIFY2(entered.tryAcquire(1, 5000), "the sink never started running");

        // Thread B: calls removeSink() for the parked sink's handle. Its own thread, not the test
        // thread, precisely so the test can observe whether it has returned yet.
        QScopedPointer<QThread> removerThread(QThread::create([&]() {
            LogTee::removeSink(handle);
            removeReturned = true;
        }));
        removerThread->start();

        // The sink is still parked on `releaseGate`, so `removeSink()` must still be blocked.
        QThread::msleep(200);
        QVERIFY2(!removeReturned,
                 "removeSink() returned before the in-flight dispatch on another thread finished");

        releaseGate.release(); // lets the parked sink, and therefore dispatch(), finish

        QVERIFY(dispatcherThread->wait(5000));
        QVERIFY(removerThread->wait(5000));
        QVERIFY(sinkDone);
        QVERIFY(removeReturned);
    }

    // WR-10 (code review 06.3-REVIEW-fork.md, second review pass): a sink that calls
    // `removeSink()` on itself, from inside its own dispatch, on the SAME thread that is
    // dispatching it. Before this fix, `removeSink()` unconditionally took the same write lock
    // `dispatch()` holds for reading across the whole sink-calling loop - `QReadWriteLock` is
    // non-recursive (Qt's own documentation), so this thread would deadlock against its own read
    // lock and this test would hang forever rather than fail loudly. The fix defers the removal
    // instead: this call returns immediately, and the removal takes effect the moment `dispatch()`
    // (still running, further up this same thread's call stack) returns.
    void logTee_removeSink_calledFromInsideItsOwnDispatch_deferredNotDeadlocked()
    {
        LogTee::clearSinksForTests();

        int callCount = 0;
        LogTee::SinkHandle handle = 0;
        handle = LogTee::addSink([&](LogLevel, int, int, const QString& text) {
            if (!text.contains(QStringLiteral("wr10-self-remove-trigger"))) {
                return;
            }
            ++callCount;
            // The scenario itself: removing this very sink, on this thread, from inside its own
            // call. Must return at once (never wait on itself) and must not corrupt the sink list
            // dispatch() is still iterating over (`dispatch()`'s own copy-then-call ordering
            // means this is safe either way, but the removal itself must not run here).
            LogTee::removeSink(handle);
        });

        qWarning() << "wr10-self-remove-trigger"; // must return promptly, not hang

        QCOMPARE(callCount, 1);

        // The deferred removal must have actually applied once dispatch() returned - not been
        // silently dropped: a second trigger line must not reach the sink again.
        qWarning() << "wr10-self-remove-trigger";
        QCOMPARE(callCount, 1);
    }

    // WR-10: the same guarantee for `addSink()` - a sink that registers ANOTHER sink from inside
    // its own dispatch must not deadlock either, and the new sink must actually be registered
    // once dispatch() returns (not lost, and not called for the very message that triggered its
    // own registration - it was not on the list yet when this dispatch copied it).
    void logTee_addSink_calledFromInsideAnotherSinksDispatch_deferredNotDeadlocked()
    {
        LogTee::clearSinksForTests();

        int outerCalls = 0;
        int innerCalls = 0;
        LogTee::SinkHandle innerHandle = 0;
        const LogTee::SinkHandle outerHandle = LogTee::addSink(
            [&](LogLevel, int, int, const QString& text) {
                if (!text.contains(QStringLiteral("wr10-add-from-dispatch-trigger"))) {
                    return;
                }
                ++outerCalls;
                if (innerHandle == 0) {
                    innerHandle = LogTee::addSink(
                        [&](LogLevel, int, int, const QString&) { ++innerCalls; });
                }
            });

        qWarning() << "wr10-add-from-dispatch-trigger"; // registers the inner sink, must not hang
        QCOMPARE(outerCalls, 1);
        QCOMPARE(innerCalls, 0); // not registered yet when this dispatch copied the sink list

        qWarning() << "wr10-add-from-dispatch-trigger"; // a later message reaches both sinks
        QCOMPARE(outerCalls, 2);
        QCOMPARE(innerCalls, 1);

        LogTee::removeSink(outerHandle);
        LogTee::removeSink(innerHandle);
    }

    // --- parseVideoStatsBlock / toQualityReport --------------------------------------------

    // CR-01: `rendered_fps` is parsed from `Rendering frame rate: ...` (the line the engine
    // actually prints, whatever the decoder context's state is at teardown - `stream_stats.cpp`'s
    // own header comment), not `Video stream: ...` (which production never prints - see
    // `productionStatsBlock()`'s own comment). This is the block production actually emits.
    void parseVideoStatsBlock_productionBlock_fillsEveryField()
    {
        VideoStats stats;
        const bool matched = parseVideoStatsBlock(productionStatsBlock(), &stats);

        QVERIFY(matched);
        QVERIFY(stats.renderedFps.present);
        QCOMPARE(stats.renderedFps.value, 59.96);
        QVERIFY(stats.networkDroppedFramePct.present);
        QCOMPARE(stats.networkDroppedFramePct.value, 0.42);
        QVERIFY(stats.jitterDroppedFramePct.present);
        QCOMPARE(stats.jitterDroppedFramePct.value, 0.10);
        QVERIFY(stats.rttMs.present);
        QCOMPARE(stats.rttMs.value, 23.0);
        QVERIFY(stats.rttVarianceMs.present);
        QCOMPARE(stats.rttVarianceMs.value, 4.0);
        QVERIFY(stats.decodeTimeMs.present);
        QCOMPARE(stats.decodeTimeMs.value, 3.21);
        QVERIFY(stats.queueTimeMs.present);
        QCOMPARE(stats.queueTimeMs.value, 1.05);
        QVERIFY(stats.renderTimeMs.present);
        QCOMPARE(stats.renderTimeMs.value, 2.77);
    }

    void parseVideoStatsBlock_naLatency_leavesOnlyRttAbsentAndStillFillsRenderedFps()
    {
        VideoStats stats;
        const bool matched = parseVideoStatsBlock(productionStatsBlockWithNoRtt(), &stats);

        QVERIFY(matched);
        QVERIFY2(stats.renderedFps.present,
                 "'Rendering frame rate:' is in every real block with receivedFps > 0 (CR-01)");
        QCOMPARE(stats.renderedFps.value, 59.96);
        QVERIFY2(!stats.rttMs.present, "N/A must leave rtt_ms absent");
        QVERIFY2(!stats.rttVarianceMs.present, "N/A must leave rtt_variance_ms absent");
        QVERIFY(stats.networkDroppedFramePct.present);
        QVERIFY(stats.jitterDroppedFramePct.present);
        QVERIFY(stats.decodeTimeMs.present);
        QVERIFY(stats.queueTimeMs.present);
        QVERIFY(stats.renderTimeMs.present);
    }

    void parseVideoStatsBlock_unrelatedText_returnsFalse()
    {
        VideoStats stats;
        const bool matched =
            parseVideoStatsBlock(QStringLiteral("nothing here matches any known line\n"), &stats);
        QVERIFY(!matched);
        QVERIFY(!stats.renderedFps.present);
    }

    void toQualityReport_presentFieldsAreNumbersAbsentFieldsAreNull()
    {
        VideoStats stats;
        stats.renderedFps = OptionalMetric::of(59.94);
        stats.rttMs = OptionalMetric::of(23.0);
        // Every other field left absent.

        const QJsonObject report = toQualityReport(stats);

        QCOMPARE(report.value(QStringLiteral("rendered_fps")).toDouble(), 59.94);
        QCOMPARE(report.value(QStringLiteral("rtt_ms")).toDouble(), 23.0);
        QVERIFY(report.value(QStringLiteral("network_dropped_frame_pct")).isNull());
        QVERIFY(report.value(QStringLiteral("jitter_dropped_frame_pct")).isNull());
        QVERIFY(report.value(QStringLiteral("rtt_variance_ms")).isNull());
        QVERIFY(report.value(QStringLiteral("decode_time_ms")).isNull());
        QVERIFY(report.value(QStringLiteral("queue_time_ms")).isNull());
        QVERIFY(report.value(QStringLiteral("render_time_ms")).isNull());
    }

    // --- Plan 16 Task 2: hostProcessingAvgMs, videoWidth/videoHeight, totalLatencyMs ----------
    // Pinned against `ffmpeg.cpp`'s exact own formats (RESEARCH-FORK.md §5): "Host processing
    // latency min/max/average: %.1f/%.1f/%.1f ms" and "Video stream: %dx%d %.2f FPS (Codec: %s)".

    void parsesHostLatencyAndResolution()
    {
        VideoStats stats;
        const bool matched = parseVideoStatsBlock(
            QStringLiteral("Video stream: 1920x1080 59.94 FPS (Codec: H264)\n"
                           "Host processing latency min/max/average: 1.0/3.0/2.0 ms\n"),
            &stats);

        QVERIFY(matched);
        QVERIFY(stats.hostProcessingAvgMs.present);
        QCOMPARE(stats.hostProcessingAvgMs.value, 2.0);
        QVERIFY(stats.videoWidth.present);
        QCOMPARE(stats.videoWidth.value, 1920.0);
        QVERIFY(stats.videoHeight.present);
        QCOMPARE(stats.videoHeight.value, 1080.0);
    }

    // The owner: "sum it and add it" - the total is the sum of every part Moonlight measures and
    // prints (`docs/spec/screens.md` §25): the network round trip, the host processing average
    // (when present), decode, queue and render, rounded to whole ms.
    void totalLatencyIsTheSumOfMeasuredParts()
    {
        VideoStats stats;
        stats.rttMs = OptionalMetric::of(12.0);
        stats.hostProcessingAvgMs = OptionalMetric::of(2.0);
        stats.decodeTimeMs = OptionalMetric::of(1.4);
        stats.queueTimeMs = OptionalMetric::of(0.6);
        stats.renderTimeMs = OptionalMetric::of(3.1);

        const OptionalMetric total = totalLatencyMs(stats);
        QVERIFY(total.present);
        QCOMPARE(total.value, 19.0);
    }

    // A Sunshine host that does not report its own processing latency simply has that part
    // omitted from the sum - never treated as zero, never blocking the other four parts.
    void totalOmitsHostWhenAbsent()
    {
        VideoStats stats;
        stats.rttMs = OptionalMetric::of(12.0);
        stats.decodeTimeMs = OptionalMetric::of(1.4);
        stats.queueTimeMs = OptionalMetric::of(0.6);
        stats.renderTimeMs = OptionalMetric::of(3.1);

        const OptionalMetric total = totalLatencyMs(stats);
        QVERIFY(total.present);
        QCOMPARE(total.value, 17.0);
    }

    // "Average network latency: N/A": with no round trip there is nothing to add the other parts
    // to, so there is no total at all (`docs/spec/screens.md` §25: "With no network figure there
    // is no total").
    void noTotalWithoutNetwork()
    {
        VideoStats stats;
        stats.hostProcessingAvgMs = OptionalMetric::of(2.0);
        stats.decodeTimeMs = OptionalMetric::of(1.4);
        stats.queueTimeMs = OptionalMetric::of(0.6);
        stats.renderTimeMs = OptionalMetric::of(3.1);
        // stats.rttMs left absent.

        QVERIFY(!totalLatencyMs(stats).present);
    }

    // The three new parse-only fields are parse-only in fact, not just in name: the quality
    // report the client sends to the control plane is byte-for-byte unchanged (contract 3.1.0's
    // eight fields, nothing more).
    void toQualityReportSendsNoNewField()
    {
        VideoStats stats;
        stats.hostProcessingAvgMs = OptionalMetric::of(2.0);
        stats.videoWidth = OptionalMetric::of(1920.0);
        stats.videoHeight = OptionalMetric::of(1080.0);

        const QJsonObject report = toQualityReport(stats);

        QCOMPARE(report.size(), 8);
        QVERIFY(!report.contains(QStringLiteral("host_processing_avg_ms")));
        QVERIFY(!report.contains(QStringLiteral("video_width")));
        QVERIFY(!report.contains(QStringLiteral("video_height")));
    }

    // --- StatsWatcher ------------------------------------------------------------------------

    void statsWatcher_titleThenBlock_emitsParsedStats()
    {
        StatsWatcher watcher;
        VideoStats captured;
        int emitCount = 0;
        connect(&watcher, &StatsWatcher::videoStatsParsed, &watcher,
                [&](VideoStats stats) { captured = stats; ++emitCount; });

        const LogTee::Sink sink = watcher.sink();
        sink(LogLevel::Info, SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO,
             QStringLiteral("Global video stats"));
        sink(LogLevel::Info, SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO,
             QString::fromLatin1(kDashesPrefix) + productionStatsBlock().mid(qstrlen(kDashesPrefix)));

        QCOMPARE(emitCount, 1);
        QVERIFY(captured.renderedFps.present);
        QCOMPARE(captured.renderedFps.value, 59.96);
    }

    // WR-05 (code review 06.3-REVIEW-fork.md): the watcher used to require the dashes-prefixed
    // message to be the one immediately following the title, tracked with one `bool` member -
    // and reset that state on ANY other APPLICATION/INFO message in between. Upstream deletes the
    // decoder (and so logs this block) while moonlight-common-c's own connection/audio/input
    // threads are still running (`session.cpp:2319-2320`), so a message from one of them landing
    // between the title and the body silently dropped the whole session's quality report. The fix
    // matches on the block's own content instead (the dashes line, which only this one call site
    // in the whole engine prints - `stream_stats.h`'s own header comment), so an intervening
    // message from another thread no longer matters. RED on the pre-fix code (which required
    // exact adjacency to a preceding title and had no defence against this at all): this was
    // untested before this plan - `06.3-REVIEW-fork.md`'s own finding.
    void statsWatcher_anotherApplicationInfoMessageBetweenTitleAndBlock_stillEmits()
    {
        StatsWatcher watcher;
        VideoStats captured;
        int emitCount = 0;
        connect(&watcher, &StatsWatcher::videoStatsParsed, &watcher,
                [&](VideoStats stats) { captured = stats; ++emitCount; });

        const LogTee::Sink sink = watcher.sink();
        sink(LogLevel::Info, SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO,
             QStringLiteral("Global video stats"));
        // A moonlight-common-c thread logging while the main thread is between its own two
        // SDL_LogInfo() calls (session.cpp:2319-2320's own ordering comment).
        sink(LogLevel::Info, SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO,
             QStringLiteral("an unrelated application/info line from another thread"));
        sink(LogLevel::Info, SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO,
             QString::fromLatin1(kDashesPrefix) + productionStatsBlock().mid(qstrlen(kDashesPrefix)));

        QCOMPARE(emitCount, 1);
        QVERIFY(captured.renderedFps.present);
        QCOMPARE(captured.renderedFps.value, 59.96);
    }

    // The block is recognised by its own content: no title message is required at all (a title
    // is always logged in production, one call earlier, but the watcher no longer depends on
    // seeing it).
    void statsWatcher_dashesLineWithNoPrecedingTitle_stillEmits()
    {
        StatsWatcher watcher;
        int emitCount = 0;
        connect(&watcher, &StatsWatcher::videoStatsParsed, &watcher,
                [&](VideoStats) { ++emitCount; });

        const LogTee::Sink sink = watcher.sink();
        sink(LogLevel::Info, SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO,
             QString::fromLatin1(kDashesPrefix) + productionStatsBlock().mid(qstrlen(kDashesPrefix)));

        QCOMPARE(emitCount, 1);
    }

    void statsWatcher_messageWithoutTheDashesPrefix_isIgnored()
    {
        StatsWatcher watcher;
        int emitCount = 0;
        connect(&watcher, &StatsWatcher::videoStatsParsed, &watcher,
                [&](VideoStats) { ++emitCount; });

        const LogTee::Sink sink = watcher.sink();
        sink(LogLevel::Info, SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO,
             QStringLiteral("Global video stats"));
        sink(LogLevel::Info, SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO,
             QStringLiteral("an unrelated application/info line, not the stats block"));

        QCOMPARE(emitCount, 0);
    }

    void statsWatcher_unrelatedCategoryOrPriority_isIgnored()
    {
        StatsWatcher watcher;
        int emitCount = 0;
        connect(&watcher, &StatsWatcher::videoStatsParsed, &watcher,
                [&](VideoStats) { ++emitCount; });

        const LogTee::Sink sink = watcher.sink();
        // A different SDL category, and a Qt-origin message (category/priority -1), each
        // carrying the exact dashes-prefixed block: neither is `SDL_LOG_CATEGORY_APPLICATION`/
        // `SDL_LOG_PRIORITY_INFO`, so neither is ever treated as the block.
        const QString dashesBlock =
            QString::fromLatin1(kDashesPrefix) + productionStatsBlock().mid(qstrlen(kDashesPrefix));
        sink(LogLevel::Warning, SDL_LOG_CATEGORY_VIDEO, SDL_LOG_PRIORITY_WARN, dashesBlock);
        sink(LogLevel::Info, -1, -1, dashesBlock);

        QCOMPARE(emitCount, 0);
    }

    // --- VideoStatsAggregator (WR-04) ---------------------------------------------------------

    void aggregator_noSegments_aggregatesToEverythingAbsent()
    {
        VideoStatsAggregator aggregator;
        const VideoStats aggregate = aggregator.aggregate();

        QVERIFY(!aggregate.renderedFps.present);
        QVERIFY(!aggregate.rttMs.present);
    }

    // One decoder instance for the whole session (the common case) reports exactly its own
    // numbers, unchanged - a session that never recreates its decoder must see no aggregation
    // effect at all.
    void aggregator_oneSegment_reportsItUnchanged()
    {
        VideoStatsAggregator aggregator;
        VideoStats segment;
        parseVideoStatsBlock(productionStatsBlock(), &segment);

        aggregator.addSegmentForTesting(segment, 60000);
        const VideoStats aggregate = aggregator.aggregate();

        QCOMPARE(aggregate.renderedFps.value, segment.renderedFps.value);
        QCOMPARE(aggregate.networkDroppedFramePct.value, segment.networkDroppedFramePct.value);
        QCOMPARE(aggregate.rttMs.value, segment.rttMs.value);
        QCOMPARE(aggregate.rttVarianceMs.value, segment.rttVarianceMs.value);
    }

    // WR-04's own scenario: a fullscreen toggle recreates the decoder partway through the
    // session. Two segments of equal duration average their rates and percentages exactly
    // halfway between the two.
    void aggregator_twoEqualDurationSegments_averagesRatesAndPercentagesEqually()
    {
        VideoStatsAggregator aggregator;

        VideoStats first;
        first.renderedFps = OptionalMetric::of(30.0);
        first.networkDroppedFramePct = OptionalMetric::of(1.0);
        // WR-09: both segments print the SAME `receivedFps`, so `networkDroppedFramePct`'s own
        // weight (`receivedFps x durationMs`) reduces to plain duration weighting here, exactly
        // like `renderedFps` above - this test is about equal-duration weighting, not about
        // unequal frame rates (`aggregator_frameRateWeighting...` below covers that).
        first.receivedFps = OptionalMetric::of(60.0);
        first.rttMs = OptionalMetric::of(10.0);
        first.rttVarianceMs = OptionalMetric::of(1.0);

        VideoStats second;
        second.renderedFps = OptionalMetric::of(60.0);
        second.networkDroppedFramePct = OptionalMetric::of(3.0);
        second.receivedFps = OptionalMetric::of(60.0);
        second.rttMs = OptionalMetric::of(50.0);
        second.rttVarianceMs = OptionalMetric::of(5.0);

        aggregator.addSegmentForTesting(first, 30000);
        aggregator.addSegmentForTesting(second, 30000);
        const VideoStats aggregate = aggregator.aggregate();

        QVERIFY(aggregate.renderedFps.present);
        QCOMPARE(aggregate.renderedFps.value, 45.0);
        QVERIFY(aggregate.networkDroppedFramePct.present);
        QCOMPARE(aggregate.networkDroppedFramePct.value, 2.0);
    }

    // WR-09 (code review 06.3-REVIEW-fork.md, second review pass): the reviewer's own worked
    // example. Duration weighting alone (the pre-fix code) would average the two decode times
    // as (3 + 12) / 2 = 7.5 ms; frame weighting - the ruling's own words, "average times weighted
    // by frames" - gives (36000 x 3 + 12000 x 12) / 48000 = 5.25 ms instead. Equal DURATIONS on
    // purpose: this isolates the frame-rate weighting itself from the duration weighting
    // `aggregator_twoUnequalDurationSegments_weightsTheLongerOneMore` above already covers.
    void aggregator_unequalFrameRatesAcrossEqualDurationSegments_weightsByFramesNotDuration()
    {
        VideoStatsAggregator aggregator;

        // Segment 1: 10 minutes at 60 fps, 3 ms decode time.
        VideoStats fast;
        fast.decodedFps = OptionalMetric::of(60.0);
        fast.decodeTimeMs = OptionalMetric::of(3.0);

        // Segment 2: 10 minutes at 20 fps (a bad network), 12 ms decode time.
        VideoStats slow;
        slow.decodedFps = OptionalMetric::of(20.0);
        slow.decodeTimeMs = OptionalMetric::of(12.0);

        aggregator.addSegmentForTesting(fast, 600000);
        aggregator.addSegmentForTesting(slow, 600000);
        const VideoStats aggregate = aggregator.aggregate();

        QVERIFY(aggregate.decodeTimeMs.present);
        QCOMPARE(aggregate.decodeTimeMs.value, 5.25);
        QVERIFY2(qAbs(aggregate.decodeTimeMs.value - 7.5) > 0.01,
                 "must not fall back to the plain duration-weighted 7.5 ms");
    }

    // The single-segment case is unaffected by frame-weighting: with only one segment,
    // `aggregate()` returns it unchanged before any weighting helper ever runs (see the trivial
    // case at the top of `aggregate()`).
    void aggregator_oneSegmentWithUnequalFrameRateFields_reportsItUnchanged()
    {
        VideoStatsAggregator aggregator;

        VideoStats only;
        only.decodedFps = OptionalMetric::of(20.0);
        only.decodeTimeMs = OptionalMetric::of(12.0);
        only.receivedFps = OptionalMetric::of(18.0);
        only.networkDroppedFramePct = OptionalMetric::of(2.5);

        aggregator.addSegmentForTesting(only, 600000);
        const VideoStats aggregate = aggregator.aggregate();

        QVERIFY(aggregate.decodeTimeMs.present);
        QCOMPARE(aggregate.decodeTimeMs.value, 12.0);
        QVERIFY(aggregate.networkDroppedFramePct.present);
        QCOMPARE(aggregate.networkDroppedFramePct.value, 2.5);
    }

    // A one-minute segment must not count as much as a one-hour one: the longer segment's rate
    // dominates the combined figure.
    void aggregator_twoUnequalDurationSegments_weightsTheLongerOneMore()
    {
        VideoStatsAggregator aggregator;

        VideoStats short_;
        short_.renderedFps = OptionalMetric::of(30.0);

        VideoStats long_;
        long_.renderedFps = OptionalMetric::of(60.0);

        aggregator.addSegmentForTesting(short_, 60000);   // 1 minute
        aggregator.addSegmentForTesting(long_, 3600000);  // 1 hour
        const VideoStats aggregate = aggregator.aggregate();

        QVERIFY(aggregate.renderedFps.present);
        // (30 * 60000 + 60 * 3600000) / (60000 + 3600000) ~= 59.51
        QVERIFY2(aggregate.renderedFps.value > 59.0 && aggregate.renderedFps.value < 60.0,
                 qPrintable(QString::number(aggregate.renderedFps.value)));
    }

    // `rtt_ms`/`rtt_variance_ms` are a point sample (`ffmpeg.cpp:674`), not a true average despite
    // the printed "Average" label - averaging or summing several of them describes nothing real,
    // so they come from the longest segment instead (the ruling's own words: "any metric the
    // block's own numbers cannot combine correctly is reported from the longest segment").
    void aggregator_rttIsTakenFromTheLongestSegmentNotAveraged()
    {
        VideoStatsAggregator aggregator;

        VideoStats shortSegment;
        shortSegment.rttMs = OptionalMetric::of(999.0);
        shortSegment.rttVarianceMs = OptionalMetric::of(999.0);

        VideoStats longSegment;
        longSegment.rttMs = OptionalMetric::of(15.0);
        longSegment.rttVarianceMs = OptionalMetric::of(2.0);

        aggregator.addSegmentForTesting(shortSegment, 1000);
        aggregator.addSegmentForTesting(longSegment, 600000);
        const VideoStats aggregate = aggregator.aggregate();

        QVERIFY(aggregate.rttMs.present);
        QCOMPARE(aggregate.rttMs.value, 15.0);
        QVERIFY(aggregate.rttVarianceMs.present);
        QCOMPARE(aggregate.rttVarianceMs.value, 2.0);
    }

    // A metric a segment never printed (a weighted-average field like `network_dropped_frame_pct`)
    // must not drag the combined average toward zero: only segments that held a value contribute
    // to the weighted sum and its weight.
    void aggregator_weightedFieldAbsentInOneSegment_isIgnoredNotTreatedAsZero()
    {
        VideoStatsAggregator aggregator;

        VideoStats withValue;
        withValue.networkDroppedFramePct = OptionalMetric::of(4.0);
        // WR-09: `networkDroppedFramePct` is now frame-weighted (by `receivedFps`), not
        // duration-weighted - the segment that carries the metric also carries its weight, since
        // both are printed in `stringifyVideoStats()`'s own text together or not at all.
        withValue.receivedFps = OptionalMetric::of(60.0);

        VideoStats withoutValue; // this decoder segment never printed the line at all

        aggregator.addSegmentForTesting(withValue, 30000);
        aggregator.addSegmentForTesting(withoutValue, 30000);
        const VideoStats aggregate = aggregator.aggregate();

        // If the absent segment counted as 0 the answer would be 2.0 (4.0 averaged with 0 over
        // equal durations); it must stay 4.0, the one segment that actually measured anything.
        QVERIFY(aggregate.networkDroppedFramePct.present);
        QCOMPARE(aggregate.networkDroppedFramePct.value, 4.0);
    }

    // The longest segment by duration is not automatically the RTT candidate: it may be exactly
    // the segment whose latency line read "N/A". The longest segment that actually held a sample
    // must win instead - never a fallback to zero or to the wrong segment's absence.
    void aggregator_rttFromLongestSegmentThatActuallyHeldASample()
    {
        VideoStatsAggregator aggregator;

        VideoStats shortWithRtt;
        shortWithRtt.rttMs = OptionalMetric::of(20.0);
        shortWithRtt.rttVarianceMs = OptionalMetric::of(3.0);

        VideoStats longWithoutRtt; // "Average network latency: N/A", but the longer segment

        aggregator.addSegmentForTesting(shortWithRtt, 30000);
        aggregator.addSegmentForTesting(longWithoutRtt, 600000);
        const VideoStats aggregate = aggregator.aggregate();

        QVERIFY(aggregate.rttMs.present);
        QCOMPARE(aggregate.rttMs.value, 20.0);
        QVERIFY(aggregate.rttVarianceMs.present);
        QCOMPARE(aggregate.rttVarianceMs.value, 3.0);
    }

    void aggregator_rttAbsentInEverySegment_aggregatesToAbsent()
    {
        VideoStatsAggregator aggregator;

        VideoStats first;  // "Average network latency: N/A"
        VideoStats second; // also N/A

        aggregator.addSegmentForTesting(first, 30000);
        aggregator.addSegmentForTesting(second, 30000);
        const VideoStats aggregate = aggregator.aggregate();

        QVERIFY(!aggregate.rttMs.present);
        QVERIFY(!aggregate.rttVarianceMs.present);
    }

    // --- ControlPlaneClient::postSessionQuality ---------------------------------------------

    void postSessionQuality_sendsToTheQualityRouteWithTheBearerAndTheReportBody()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const quint16 port = server.serverPort();

        QByteArray requestBytes;
        QTcpSocket* socket = nullptr;
        connect(&server, &QTcpServer::newConnection, &server, [&]() {
            socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, &server, [&]() {
                requestBytes += socket->readAll();
                if (requestBytes.contains("\r\n\r\n")) {
                    const QByteArray body = QByteArrayLiteral("{}");
                    QByteArray response = QByteArrayLiteral(
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n"
                        "Content-Length: ");
                    response += QByteArray::number(body.size());
                    response += QByteArrayLiteral("\r\n\r\n");
                    response += body;
                    socket->write(response);
                    socket->flush();
                    socket->disconnectFromHost();
                }
            });
        });

        ControlPlaneClient client;
        client.setBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(port));
        client.setAccessToken(QStringLiteral("opaque-access-token"));

        VideoStats stats;
        stats.renderedFps = OptionalMetric::of(59.94);
        stats.rttMs = OptionalMetric::of(23.0);
        stats.rttVarianceMs = OptionalMetric::of(4.0);
        const QJsonObject report = toQualityReport(stats);

        bool called = false;
        ControlPlaneResult received;
        client.postSessionQuality(QStringLiteral("6f1c6f5e-3a1e-4b1e-9f2e-0f1a2b3c4d5e"), report,
                                  [&](const ControlPlaneResult& result) {
                                      received = result;
                                      called = true;
                                  });

        QTRY_VERIFY(called);

        QVERIFY2(requestBytes.startsWith(
                     "POST /api/sessions/6f1c6f5e-3a1e-4b1e-9f2e-0f1a2b3c4d5e/quality"),
                 requestBytes.constData());
        QVERIFY2(requestBytes.contains("Authorization: Bearer opaque-access-token"),
                 requestBytes.constData());
        QVERIFY2(requestBytes.contains("\"rendered_fps\":59.94"), requestBytes.constData());
        QVERIFY(received.ok);
    }

    // --- parseConnectionTerminated -----------------------------------------------------------

    void parseConnectionTerminated_matchesEveryDocumentedCase_data()
    {
        QTest::addColumn<int>("category");
        QTest::addColumn<int>("priority");
        QTest::addColumn<QString>("message");
        QTest::addColumn<bool>("expectedMatch");
        QTest::addColumn<int>("expectedCode");

        QTest::newRow("graceful termination, code 0 (a real code)")
            << static_cast<int>(SDL_LOG_CATEGORY_APPLICATION) << static_cast<int>(SDL_LOG_PRIORITY_ERROR)
            << QStringLiteral("Connection terminated: 0") << true << 0;
        QTest::newRow("no video traffic, -100")
            << static_cast<int>(SDL_LOG_CATEGORY_APPLICATION) << static_cast<int>(SDL_LOG_PRIORITY_ERROR)
            << QStringLiteral("Connection terminated: -100") << true << -100;
        QTest::newRow("frame conversion, -104")
            << static_cast<int>(SDL_LOG_CATEGORY_APPLICATION) << static_cast<int>(SDL_LOG_PRIORITY_ERROR)
            << QStringLiteral("Connection terminated: -104") << true << -104;
        QTest::newRow("wrong category")
            << static_cast<int>(SDL_LOG_CATEGORY_VIDEO) << static_cast<int>(SDL_LOG_PRIORITY_ERROR)
            << QStringLiteral("Connection terminated: -100") << false << 0;
        QTest::newRow("wrong priority (INFO)")
            << static_cast<int>(SDL_LOG_CATEGORY_APPLICATION) << static_cast<int>(SDL_LOG_PRIORITY_INFO)
            << QStringLiteral("Connection terminated: -100") << false << 0;
        QTest::newRow("trailing garbage after the number")
            << static_cast<int>(SDL_LOG_CATEGORY_APPLICATION) << static_cast<int>(SDL_LOG_PRIORITY_ERROR)
            << QStringLiteral("Connection terminated: 12x") << false << 0;
        QTest::newRow("no number at all")
            << static_cast<int>(SDL_LOG_CATEGORY_APPLICATION) << static_cast<int>(SDL_LOG_PRIORITY_ERROR)
            << QStringLiteral("Connection terminated: ") << false << 0;
        QTest::newRow("wrong case")
            << static_cast<int>(SDL_LOG_CATEGORY_APPLICATION) << static_cast<int>(SDL_LOG_PRIORITY_ERROR)
            << QStringLiteral("connection terminated: -100") << false << 0;
        QTest::newRow("overflows int")
            << static_cast<int>(SDL_LOG_CATEGORY_APPLICATION) << static_cast<int>(SDL_LOG_PRIORITY_ERROR)
            << QStringLiteral("Connection terminated: 99999999999") << false << 0;
    }

    void parseConnectionTerminated_matchesEveryDocumentedCase()
    {
        QFETCH(int, category);
        QFETCH(int, priority);
        QFETCH(QString, message);
        QFETCH(bool, expectedMatch);
        QFETCH(int, expectedCode);

        int code = -12345;
        const QByteArray utf8 = message.toUtf8();
        const bool matched = parseConnectionTerminated(category, priority, utf8.constData(), &code);

        QCOMPARE(matched, expectedMatch);
        if (expectedMatch) {
            QCOMPARE(code, expectedCode);
        }
    }

    // --- the sink wired the way SeatHubClient's constructor wires it, reaching noteTermination ---

    void engineTerminationSink_matchingLine_callsNoteTerminationOnce()
    {
        LivenessTimer liveness;
        // No control plane attached: `reportNow()` no-ops on the POST itself, but
        // `noteTermination()` still sets its observable state before calling it - the same
        // accessors the `06.3-15-PLAN.md` behaviour line calls "a test double" here.
        const auto sink = [&liveness](LogLevel level, int category, int priority,
                                      const QString& text) {
            Q_UNUSED(level);
            int code = 0;
            const QByteArray utf8 = text.toUtf8();
            if (parseConnectionTerminated(category, priority, utf8.constData(), &code)) {
                liveness.noteTermination(code);
            }
        };

        QVERIFY(!liveness.hasEngineError());
        sink(LogLevel::Error, SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_ERROR,
             QStringLiteral("Connection terminated: -100"));

        QVERIFY(liveness.hasEngineError());
        QCOMPARE(liveness.engineError(), -100);
    }

    void engineTerminationSink_unrelatedErrorLine_callsNoteTerminationNever()
    {
        LivenessTimer liveness;
        const auto sink = [&liveness](LogLevel level, int category, int priority,
                                      const QString& text) {
            Q_UNUSED(level);
            int code = 0;
            const QByteArray utf8 = text.toUtf8();
            if (parseConnectionTerminated(category, priority, utf8.constData(), &code)) {
                liveness.noteTermination(code);
            }
        };

        sink(LogLevel::Error, SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_ERROR,
             QStringLiteral("Some other error entirely"));

        QVERIFY(!liveness.hasEngineError());
    }

    // --- QualityOutbox (Plan 30, D-17) --------------------------------------------------------

    void put_writesOneFileNamedForTheSessionId()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QualityOutbox outbox(dir.path());

        outbox.put(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"), sampleReport());

        QVERIFY(QFile::exists(dir.filePath(QString::fromLatin1(kSessionA) + QStringLiteral(".json"))));
    }

    void put_withANonUuidSessionId_writesNothing()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QualityOutbox outbox(dir.path());

        outbox.put(QStringLiteral("../escape"), QStringLiteral("acct-1"), sampleReport());

        QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 0);
    }

    void drain_deliveredOutcome_sendsAndDeletesTheFile_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<bool>("ok");

        QTest::newRow("200 ok") << 200 << true;
        // 409 ("this session never streamed") is grouped with the successes per this plan's own
        // truth line: no retry ever changes that answer either.
        QTest::newRow("409 never streamed") << 409 << false;
    }

    void drain_deliveredOutcome_sendsAndDeletesTheFile()
    {
        QFETCH(int, status);
        QFETCH(bool, ok);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QualityOutbox outbox(dir.path());
        outbox.put(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"), sampleReport());

        int sendCount = 0;
        outbox.drain(QStringLiteral("gen-1"), QStringLiteral("acct-1"),
                     [&](const QString& sessionId, const QJsonObject&,
                         ControlPlaneClient::Callback callback) {
                         QCOMPARE(sessionId, QString::fromLatin1(kSessionA));
                         ++sendCount;
                         callback(resultWithStatus(status, ok));
                     });

        QCOMPARE(sendCount, 1);
        QVERIFY(!QFile::exists(dir.filePath(QString::fromLatin1(kSessionA) + QStringLiteral(".json"))));
    }

    void drain_refusedOutcome_deletesTheFileAndSendsNothingAgain_data()
    {
        QTest::addColumn<int>("status");

        QTest::newRow("400") << 400;
        QTest::newRow("404") << 404;
        QTest::newRow("413") << 413;
        QTest::newRow("422") << 422;
    }

    void drain_refusedOutcome_deletesTheFileAndSendsNothingAgain()
    {
        QFETCH(int, status);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QualityOutbox outbox(dir.path());
        outbox.put(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"), sampleReport());

        int sendCount = 0;
        outbox.drain(QStringLiteral("gen-1"), QStringLiteral("acct-1"),
                     [&](const QString&, const QJsonObject&, ControlPlaneClient::Callback callback) {
                         ++sendCount;
                         callback(resultWithStatus(status));
                     });

        QCOMPARE(sendCount, 1);
        QVERIFY(!QFile::exists(dir.filePath(QString::fromLatin1(kSessionA) + QStringLiteral(".json"))));
    }

    void drain_authFailedOutcome_keepsTheFileAndStopsTheRestOfTheLoop_data()
    {
        QTest::addColumn<int>("status");

        QTest::newRow("401") << 401;
        QTest::newRow("403") << 403;
    }

    void drain_authFailedOutcome_keepsTheFileAndStopsTheRestOfTheLoop()
    {
        QFETCH(int, status);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QualityOutbox outbox(dir.path());
        outbox.put(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"), sampleReport());
        outbox.put(QString::fromLatin1(kSessionB), QStringLiteral("acct-1"), sampleReport());

        int sendCount = 0;
        outbox.drain(QStringLiteral("gen-1"), QStringLiteral("acct-1"),
                     [&](const QString&, const QJsonObject&, ControlPlaneClient::Callback callback) {
                         ++sendCount;
                         callback(resultWithStatus(status));
                     });

        // Only the first (name-sorted) file was attempted; a 401/403 stops the loop rather than
        // trying the second file with a token that just failed.
        QCOMPARE(sendCount, 1);
        QVERIFY(QFile::exists(dir.filePath(QString::fromLatin1(kSessionA) + QStringLiteral(".json"))));
        QVERIFY(QFile::exists(dir.filePath(QString::fromLatin1(kSessionB) + QStringLiteral(".json"))));
    }

    void drain_retryableOutcome_keepsTheFileAndStillAttemptsTheNextOne_data()
    {
        QTest::addColumn<int>("status");

        QTest::newRow("transport failure") << 0;
        QTest::newRow("500") << 500;
        QTest::newRow("503") << 503;
        QTest::newRow("408") << 408;
        QTest::newRow("429") << 429;
    }

    void drain_retryableOutcome_keepsTheFileAndStillAttemptsTheNextOne()
    {
        QFETCH(int, status);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QualityOutbox outbox(dir.path());
        outbox.put(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"), sampleReport());
        outbox.put(QString::fromLatin1(kSessionB), QStringLiteral("acct-1"), sampleReport());

        int sendCount = 0;
        outbox.drain(QStringLiteral("gen-1"), QStringLiteral("acct-1"),
                     [&](const QString&, const QJsonObject&, ControlPlaneClient::Callback callback) {
                         ++sendCount;
                         callback(resultWithStatus(status));
                     });

        // Retryable does not stop the loop - both files are attempted and both are kept.
        QCOMPARE(sendCount, 2);
        QVERIFY(QFile::exists(dir.filePath(QString::fromLatin1(kSessionA) + QStringLiteral(".json"))));
        QVERIFY(QFile::exists(dir.filePath(QString::fromLatin1(kSessionB) + QStringLiteral(".json"))));
    }

    void drain_anotherAccountsReport_isDeletedWithOneWarnAndNeverSent()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QualityOutbox outbox(dir.path());
        outbox.put(QString::fromLatin1(kSessionA), QStringLiteral("acct-OTHER"), sampleReport());

        int sendCount = 0;
        outbox.drain(QStringLiteral("gen-1"), QStringLiteral("acct-1"),
                     [&](const QString&, const QJsonObject&, ControlPlaneClient::Callback) {
                         ++sendCount;
                     });

        QCOMPARE(sendCount, 0);
        QVERIFY(!QFile::exists(dir.filePath(QString::fromLatin1(kSessionA) + QStringLiteral(".json"))));
    }

    void drain_withNoTokenOrNoAccount_sendsNothing()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QualityOutbox outbox(dir.path());
        outbox.put(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"), sampleReport());

        int sendCount = 0;
        auto postFn = [&](const QString&, const QJsonObject&, ControlPlaneClient::Callback) {
            ++sendCount;
        };

        outbox.drain(QString(), QStringLiteral("acct-1"), postFn);
        outbox.drain(QStringLiteral("gen-1"), QString(), postFn);

        QCOMPARE(sendCount, 0);
        QVERIFY(QFile::exists(dir.filePath(QString::fromLatin1(kSessionA) + QStringLiteral(".json"))));
    }

    void drain_sameGenerationAfterAuthFailed_sendsNothingUntilANewGeneration()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QualityOutbox outbox(dir.path());
        outbox.put(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"), sampleReport());

        int sendCount = 0;
        auto refusing = [&](const QString&, const QJsonObject&, ControlPlaneClient::Callback callback) {
            ++sendCount;
            callback(resultWithStatus(401));
        };
        outbox.drain(QStringLiteral("gen-1"), QStringLiteral("acct-1"), refusing);
        QCOMPARE(sendCount, 1);

        // The same generation is not retried at all.
        outbox.drain(QStringLiteral("gen-1"), QStringLiteral("acct-1"), refusing);
        QCOMPARE(sendCount, 1);

        // A new generation (a new token) is attempted again.
        int newGenerationSendCount = 0;
        outbox.drain(QStringLiteral("gen-2"), QStringLiteral("acct-1"),
                     [&](const QString&, const QJsonObject&, ControlPlaneClient::Callback callback) {
                         ++newGenerationSendCount;
                         callback(resultWithStatus(200, true));
                     });
        QCOMPARE(newGenerationSendCount, 1);
        QVERIFY(!QFile::exists(dir.filePath(QString::fromLatin1(kSessionA) + QStringLiteral(".json"))));
    }

    void drain_whileAlreadyDraining_doesNotSendTheSameFileTwice()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QualityOutbox outbox(dir.path());
        outbox.put(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"), sampleReport());

        int sendCount = 0;
        ControlPlaneClient::Callback pendingCallback;
        outbox.drain(QStringLiteral("gen-1"), QStringLiteral("acct-1"),
                     [&](const QString&, const QJsonObject&, ControlPlaneClient::Callback callback) {
                         ++sendCount;
                         // Held, not answered yet: this call is still "in flight".
                         pendingCallback = callback;
                     });
        QCOMPARE(sendCount, 1);
        QVERIFY(pendingCallback);

        // A second drain call while the first is still in flight must not re-send the same file.
        outbox.drain(QStringLiteral("gen-1"), QStringLiteral("acct-1"),
                     [&](const QString&, const QJsonObject&, ControlPlaneClient::Callback) {
                         ++sendCount;
                     });
        QCOMPARE(sendCount, 1);

        pendingCallback(resultWithStatus(200, true));
        QVERIFY(!QFile::exists(dir.filePath(QString::fromLatin1(kSessionA) + QStringLiteral(".json"))));
    }

    void hasQueuedReports_emptyDirectory_isFalse()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QualityOutbox outbox(dir.path());

        QVERIFY(!outbox.hasQueuedReports());

        outbox.put(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"), sampleReport());
        QVERIFY(outbox.hasQueuedReports());
    }
};

QTEST_MAIN(TstStreamStats)

#include "tst_stream_stats.moc"
