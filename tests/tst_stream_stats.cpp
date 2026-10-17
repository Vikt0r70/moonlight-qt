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

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopedPointer>
#include <QSemaphore>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>

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
#include "seathub/stream_quality_sampler.h"
#include "seathub/telemetry.h"

// Unit capture; the real SDK envelope proof lives in tst_telemetry.
static QList<QJsonObject> rollupLogs;
static QList<QJsonObject> rollupMetrics;
struct QualityLog { QString body; LogLevel level; QJsonObject attrs; };
static QList<QualityLog> qualityLogs;
namespace SeatHubTelemetry {
void emitDiagnostic(const QString& body, LogLevel level, const QJsonObject& attrs)
{
    if (body == QLatin1String("stream.rollup")) rollupLogs.append(attrs);
    if (body.startsWith(QLatin1String("stream.quality_"))) qualityLogs.append({body, level, attrs});
}
void emitRollupMetrics(const QJsonObject& attrs) { rollupMetrics.append(attrs); }
QJsonObject rollupDeliveryHealth()
{ return {{"backlog_spool_lines", 0}, {"backlog_retry_files", 0}}; }
}

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

// --- ADR-0074 HI02/HI06 fixtures: delivered stream time ---------------------------------------

// The monotonic clock the delivered-time clock reads, driven by the test.
struct FakeClock
{
    qint64 now = 0;
    DeliveredStreamTime::Clock fn()
    {
        return [this]() { return now; };
    }
};

// One engine log line through the clock's own sink, exactly as `LogTee` hands it over.
void engineLine(DeliveredStreamTime& clock, int priority, const QString& text)
{
    clock.sink()(priority == SDL_LOG_PRIORITY_ERROR ? LogLevel::Error : LogLevel::Info,
                 SDL_LOG_CATEGORY_APPLICATION, priority, text);
}

const QString kQuitLine = QStringLiteral("Quit event received");
const QString kLostLine = QStringLiteral("Control stream received unexpected disconnect event");

void quitLine(DeliveredStreamTime& clock) { engineLine(clock, SDL_LOG_PRIORITY_INFO, kQuitLine); }
void lostLine(DeliveredStreamTime& clock) { engineLine(clock, SDL_LOG_PRIORITY_INFO, kLostLine); }
void terminatedLine(DeliveredStreamTime& clock, int code)
{
    engineLine(clock, SDL_LOG_PRIORITY_ERROR, QStringLiteral("Connection terminated: %1").arg(code));
}

// The segment `segmentNo` of a snapshot, or a default (segmentNo 0) when it is not there.
StreamSegmentReport segmentOf(const StreamTimeSnapshot& snapshot, int segmentNo)
{
    for (const StreamSegmentReport& segment : snapshot.segments) {
        if (segment.segmentNo == segmentNo) {
            return segment;
        }
    }
    return StreamSegmentReport();
}

// The control plane's rule for `POST /api/sessions/{id}/stream-time`, as the W04 text states it:
// 409 until the session has a `stream_started_at`; otherwise the larger of the stored and the
// reported `delivered_ms` for the (session, segment) key, never an addition. The total of a session
// is the sum over its segments.
struct FakeStreamTimeServer
{
    QHash<QString, bool> started;
    QMap<QPair<QString, int>, qint64> stored;
    QStringList calls;
    // Non-zero forces that answer for every call (0 forces a lost reply AFTER the report was applied).
    int forcedStatus = 200;
    bool applyThenLoseReply = false;

    qint64 total(const QString& sessionId) const
    {
        qint64 sum = 0;
        for (auto it = stored.constBegin(); it != stored.constEnd(); ++it) {
            if (it.key().first == sessionId) {
                sum += it.value();
            }
        }
        return sum;
    }

    StreamTimeOutbox::PostFn poster()
    {
        return [this](const QString& sessionId, const StreamSegmentReport& segment,
                      ControlPlaneClient::Callback callback) {
            calls.append(QStringLiteral("%1:%2:%3:%4")
                             .arg(sessionId).arg(segment.segmentNo).arg(segment.deliveredMs)
                             .arg(segment.closed ? 1 : 0));
            ControlPlaneResult result;
            if (applyThenLoseReply) {
                apply(sessionId, segment);
                result.statusCode = 0;
                callback(result);
                return;
            }
            if (forcedStatus != 200) {
                result.statusCode = forcedStatus;
                callback(result);
                return;
            }
            if (!started.value(sessionId, false)) {
                result.statusCode = 409;
                callback(result);
                return;
            }
            apply(sessionId, segment);
            result.ok = true;
            result.statusCode = 200;
            callback(result);
        };
    }

private:
    void apply(const QString& sessionId, const StreamSegmentReport& segment)
    {
        const QPair<QString, int> key(sessionId, segment.segmentNo);
        stored[key] = qMax(stored.value(key, 0), segment.deliveredMs);
    }
};

// A reply that produces one canned response as soon as the event loop turns.
class FakeReply : public QNetworkReply
{
    Q_OBJECT

public:
    FakeReply(int httpStatus, const QByteArray& body, QObject* parent) : QNetworkReply(parent)
    {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, QVariant(httpStatus));
        m_buffer.setData(body);
        m_buffer.open(QIODevice::ReadOnly);
        open(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this]() {
            setFinished(true);
            emit finished();
        });
    }

    void abort() override {}
    qint64 readData(char* data, qint64 maxSize) override { return m_buffer.read(data, maxSize); }
    qint64 bytesAvailable() const override { return m_buffer.size() + QNetworkReply::bytesAvailable(); }

private:
    QBuffer m_buffer;
};

// Records every path and body the client sends and answers each with HTTP 200 `{}`.
class RecordingNetworkAccessManager : public QNetworkAccessManager
{
    Q_OBJECT

public:
    QStringList paths;
    QList<QByteArray> bodies;

protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request,
                                 QIODevice* outgoingData = nullptr) override
    {
        paths.append(request.url().path(QUrl::FullyEncoded));
        bodies.append(outgoingData ? outgoingData->readAll() : QByteArray());
        return new FakeReply(200, QByteArrayLiteral("{}"), this);
    }
};

} // namespace

class TstStreamStats : public QObject
{
    Q_OBJECT

private slots:
    void aSimulatedHourStaysInsideTheDiagnosticBudget()
    {
        rollupLogs.clear(); rollupMetrics.clear(); qualityLogs.clear();
        qint64 now = 0;
        StreamQualitySampler sampler(nullptr, [&]() { return now; });
        sampler.start(QStringLiteral("hour"));
        VideoStats stats;
        stats.renderedFps = OptionalMetric::of(60);
        stats.rttMs = OptionalMetric::of(23);
        QTimer* timer = sampler.findChild<QTimer*>();
        QVERIFY(timer);
        for (int second = 1; second <= 3600; ++second) {
            now = second * 1000;
            sampler.feed(stats);
            // Frequent engine transitions stress suppression independently of rollup cadence.
            sampler.noteConnectionStatus(second % 2);
            if (second % 60 == 0) QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        }
        sampler.finish();
        QCOMPARE(rollupLogs.size(), 60);
        QCOMPARE(rollupMetrics.size(), 60);
        int bad = 0;
        for (const auto& log : qualityLogs) if (log.body == QLatin1String("stream.quality_bad")) ++bad;
        QCOMPARE(bad, 20);
        QVERIFY(rollupLogs.size() + qualityLogs.size() + 1 < 1000);
        QCOMPARE(sampler.rollups(), 60);
    }

    void poorStateEmitsQualityBadAtOnceAndOkOnRecovery()
    {
        qualityLogs.clear();
        qint64 now = 0;
        StreamQualitySampler sampler(nullptr, [&]() { return now; });
        sampler.start("quality");
        for (int i = 1; i <= 15; ++i) {
            VideoStats stats; stats.renderedFps = OptionalMetric::of(i);
            sampler.feed(stats);
        }
        sampler.takeWindow(false); // a minute boundary must not discard the last ten samples
        sampler.noteConnectionStatus(1); // Limelight.h: CONN_STATUS_POOR
        QCOMPARE(qualityLogs.size(), 1);
        QCOMPARE(qualityLogs.first().body, QStringLiteral("stream.quality_bad"));
        QCOMPARE(qualityLogs.first().level, LogLevel::Warning);
        QCOMPARE(qualityLogs.first().attrs.value("reason").toString(), QStringLiteral("engine_poor"));
        QCOMPARE(qualityLogs.first().attrs.value("n").toInt(), 10);
        QCOMPARE(qualityLogs.first().attrs.value("fps_avg").toDouble(), 10.5);
        sampler.noteConnectionStatus(1);
        QCOMPARE(qualityLogs.size(), 1);
        now = 12500;
        sampler.noteConnectionStatus(0);
        QCOMPARE(qualityLogs.size(), 2);
        QCOMPARE(qualityLogs.last().body, QStringLiteral("stream.quality_ok"));
        QCOMPARE(qualityLogs.last().level, LogLevel::Info);
        QCOMPARE(qualityLogs.last().attrs.value("bad_s").toDouble(), 12.5);
        QCOMPARE(sampler.badEpisodes(), 1);
        QCOMPARE(sampler.badSeconds(), 12.5);
        sampler.finish();
    }

    void theRttTriggerNeedsFiveInAndTenOut()
    {
        qualityLogs.clear();
        StreamQualitySampler sampler;
        sampler.start("rtt");
        VideoStats stats; stats.rttMs = OptionalMetric::of(80);
        const auto feed = [&]() { sampler.feed(stats); QCoreApplication::processEvents(); };
        for (int i = 0; i < 4; ++i) feed();
        QVERIFY(qualityLogs.isEmpty());
        feed();
        QCOMPARE(qualityLogs.size(), 1);
        QCOMPARE(qualityLogs.first().attrs.value("reason").toString(), QStringLiteral("rtt_high"));
        sampler.noteConnectionStatus(0); // engine OKAY cannot clear an independent RTT collapse
        QCOMPARE(qualityLogs.size(), 1);
        stats.rttMs = OptionalMetric::of(79.99);
        for (int i = 0; i < 9; ++i) feed();
        QCOMPARE(qualityLogs.size(), 1);
        feed();
        QCOMPARE(qualityLogs.size(), 2);
        QCOMPARE(qualityLogs.last().body, QStringLiteral("stream.quality_ok"));
        sampler.finish();
    }

    void badLinesAreCappedAt20PerPlayAndOnePer30Seconds()
    {
        qualityLogs.clear();
        qint64 now = 0;
        StreamQualitySampler sampler(nullptr, [&]() { return now; });
        sampler.start("caps");
        const auto cycle = [&]() { sampler.noteConnectionStatus(1); ++now; sampler.noteConnectionStatus(0); };
        const auto badCount = [&]() {
            int count = 0;
            for (const auto& log : qualityLogs) if (log.body == "stream.quality_bad") ++count;
            return count;
        };
        cycle();
        now = 29999; cycle();
        QCOMPARE(badCount(), 1);
        QCOMPARE(sampler.badEpisodes(), 2);
        for (int i = 1; i <= 20; ++i) { now = i * 30000; cycle(); }
        QCOMPARE(badCount(), 20);
        QCOMPARE(sampler.badEpisodes(), 22);
        QCOMPARE(sampler.badSeconds(), 0.022);
        sampler.finish();
        now += 30000;
        sampler.start("caps"); // a reconnect is still the same Play
        cycle();
        QCOMPARE(badCount(), 20);
        QCOMPARE(sampler.badEpisodes(), 23);
        sampler.finish();
        sampler.start("next-play");
        cycle();
        QCOMPARE(badCount(), 21);
        QCOMPARE(sampler.badEpisodes(), 1);
        sampler.finish();
    }

    void summaryAccumulatorsRetainMissingSamplingSecondsAndOpenBadTime()
    {
        qint64 now = 0;
        StreamQualitySampler sampler(nullptr, [&]() { return now; });
        sampler.start("gaps");
        VideoStats stats; stats.renderedFps = OptionalMetric::of(60);
        for (int i = 0; i < 45; ++i) { now = i * 1000; sampler.feed(stats); }
        now = 50000; sampler.noteConnectionStatus(1);
        now = 60000; sampler.finish();
        QCOMPARE(sampler.rollups(), 1);
        QCOMPARE(sampler.badEpisodes(), 1);
        QCOMPARE(sampler.badSeconds(), 10.0);
        QCOMPARE(sampler.samplingGapS(), 15.0);
        now = 90000;
        QCOMPARE(sampler.badSeconds(), 10.0);
        QCOMPARE(sampler.samplingGapS(), 15.0);
    }

    void absentRttBreaksTheStreakAndBothSourcesMustRecover()
    {
        qualityLogs.clear();
        StreamQualitySampler sampler;
        sampler.start("sources");
        VideoStats stats; stats.rttMs = OptionalMetric::of(80);
        const auto feed = [&]() { sampler.feed(stats); QCoreApplication::processEvents(); };
        for (int i = 0; i < 4; ++i) feed();
        stats.rttMs = OptionalMetric::none(); feed();
        stats.rttMs = OptionalMetric::of(80);
        for (int i = 0; i < 4; ++i) feed();
        QVERIFY(qualityLogs.isEmpty());
        feed();
        QCOMPARE(qualityLogs.size(), 1);
        sampler.noteConnectionStatus(1);
        stats.rttMs = OptionalMetric::of(79);
        for (int i = 0; i < 10; ++i) feed();
        QCOMPARE(qualityLogs.size(), 1);
        sampler.noteConnectionStatus(0);
        QCOMPARE(qualityLogs.size(), 2);
        sampler.finish();
    }

    void rollupNearestRankP95()
    {
        StreamQualitySampler sampler;
        sampler.start(QStringLiteral("math"));
        for (int count : {100, 7, 1}) {
            for (int i = count; i >= 1; --i) {
                VideoStats stats;
                stats.renderedFps = OptionalMetric::of(i);
                stats.rttMs = OptionalMetric::of(i);
                sampler.feed(stats);
            }
            const auto attrs = sampler.takeWindow(false);
            QCOMPARE(attrs.value("rtt_p95").toDouble(), count == 100 ? 95.0 : double(count));
            QCOMPARE(attrs.value("fps_avg").toDouble(), (count + 1) / 2.0);
            QCOMPARE(attrs.value("fps_min").toDouble(), 1.0);
        }
        sampler.finish();
    }

    void rollupKeepsAbsentFieldsAbsent()
    {
        StreamQualitySampler sampler;
        sampler.start(QStringLiteral("absent"));
        VideoStats stats;
        QVERIFY(parseVideoStatsBlock(productionStatsBlock(), &stats));
        sampler.feed(stats);
        QVERIFY(sampler.takeWindow(false).contains("rtt_avg"));
        QVERIFY(parseVideoStatsBlock(productionStatsBlockWithNoRtt(), &stats));
        sampler.feed(stats);
        const auto attrs = sampler.takeWindow(false);
        QVERIFY(!attrs.contains("rtt_avg"));
        QVERIFY(!attrs.contains("rtt_p95"));
        QVERIFY(sampler.takeWindow(false).isEmpty());
        rollupLogs.clear();
        sampler.finish();
        QVERIFY(rollupLogs.isEmpty());
    }

    void rollupCoverageCapAndUnits()
    {
        StreamQualitySampler sampler;
        sampler.start(QStringLiteral("coverage"));
        VideoStats stats;
        QVERIFY(parseVideoStatsBlock(QStringLiteral("Video stream: 1920x1080 60.00 FPS (Codec: H264)\n")
                                    + productionStatsBlock(), &stats));
        for (int i = 0; i < 45; ++i) sampler.feed(stats);
        auto attrs = sampler.takeWindow(false);
        QCOMPARE(attrs.value("n").toInt(), 45);
        QCOMPARE(attrs.value("coverage_pct").toDouble(), 75.0);
        QCOMPARE(attrs.value("res").toString(), QStringLiteral("1920x1080"));
        QCOMPARE(attrs.value("host_avg").toDouble(), 1.6);
        QCOMPARE(attrs.value("decode_avg").toDouble(), 3.21);
        QCOMPARE(attrs.value("fps_avg").toDouble(), 59.96);
        QCOMPARE(attrs.value("net_drop_avg").toDouble(), 0.42);
        QCOMPARE(attrs.value("partial").toBool(), false);
        for (int i = 0; i < 135; ++i) sampler.feed(stats);
        attrs = sampler.takeWindow(true);
        QCOMPARE(attrs.value("n").toInt(), 120);
        QCOMPARE(attrs.value("dropped_samples").toInt(), 15);
        QVERIFY(attrs.value("partial").toBool());
        sampler.finish();
    }

    void samplerPassivelyCarriesReceivedDecodedRatesAndRecentObservations()
    {
        rollupLogs.clear();
        StreamQualitySampler sampler;
        sampler.setWindowMs(40);
        sampler.start(QStringLiteral("sess-test-rates"));

        VideoStats stats;
        stats.renderedFps = OptionalMetric::of(59.9);
        stats.receivedFps = OptionalMetric::of(60.0);
        stats.decodedFps = OptionalMetric::of(59.8);

        // Feed 3 samples with known rates
        sampler.feed(stats);
        sampler.feed(stats);
        sampler.feed(stats);

        QTRY_COMPARE(sampler.rollups(), 1);
        QVERIFY(!rollupLogs.isEmpty());
        const QJsonObject log = rollupLogs.last();

        // Rates are passively aggregated
        QCOMPARE(log.value(QStringLiteral("fps_avg")).toDouble(), 59.9);
        QCOMPARE(log.value(QStringLiteral("incoming_fps_avg")).toDouble(), 60.0);
        QCOMPARE(log.value(QStringLiteral("decoded_fps_avg")).toDouble(), 59.8);

        // Recent observations are captured in bounded order (up to 10)
        QCOMPARE(log.value(QStringLiteral("recent_n")).toInt(), 3);
        QCOMPARE(log.value(QStringLiteral("recent_0_rendered_fps")).toDouble(), 59.9);
        QCOMPARE(log.value(QStringLiteral("recent_0_incoming_fps")).toDouble(), 60.0);
        QCOMPARE(log.value(QStringLiteral("recent_0_decoded_fps")).toDouble(), 59.8);
        QVERIFY(log.contains(QStringLiteral("recent_0_elapsed_ms")));
        QVERIFY(log.contains(QStringLiteral("elapsed_ms")));
        QVERIFY(log.contains(QStringLiteral("last_sample_age_ms")));

        // Empty / unpopulated rates are NOT fabricated as zero
        VideoStats emptyRatesStats;
        emptyRatesStats.rttMs = OptionalMetric::of(12.0); // only RTT, no FPS fields
        sampler.feed(emptyRatesStats);
        sampler.finish();

        QVERIFY(rollupLogs.size() >= 2);
        const QJsonObject finishLog = rollupLogs.last();
        // Since finishLog had recent samples with some missing rates, missing ones are omitted, not 0
        QVERIFY(finishLog.value(QStringLiteral("recent_n")).toInt() > 0);
        int lastIdx = finishLog.value(QStringLiteral("recent_n")).toInt() - 1;
        // Last sample has no rendered_fps, so recent_<lastIdx>_rendered_fps must be absent/omitted
        QVERIFY(!finishLog.contains(QStringLiteral("recent_%1_rendered_fps").arg(lastIdx)));
    }

    void theWindowIsAThreadSafeNonBlockingFeed()
    {
        StreamQualitySampler sampler;
        sampler.start(QStringLiteral("concurrent"));
        std::atomic<int> done{0};
        QList<QThread*> writers;
        for (int t = 0; t < 4; ++t) {
            auto writer = QThread::create([&]() {
                VideoStats stats; stats.renderedFps = OptionalMetric::of(60);
                for (int i = 0; i < 10000; ++i) sampler.feed(stats);
                ++done;
            });
            writers.append(writer); writer->start();
        }
        int accounted = 0;
        const auto drain = [&]() {
            const auto attrs = sampler.takeWindow(false);
            accounted += attrs.value("n").toInt() + attrs.value("dropped_samples").toInt();
        };
        while (done.load() < 4) { drain(); QThread::yieldCurrentThread(); }
        for (auto writer : writers) { writer->wait(); delete writer; }
        drain();
        QCOMPARE(accounted, 40000);
        sampler.finish();
    }

    void rollupTimerStopsAndEmitsOnlyNonemptyWindows()
    {
        rollupLogs.clear(); rollupMetrics.clear();
        StreamQualitySampler sampler;
        sampler.setWindowMs(40);
        sampler.start(QString()); // local streams have no control-plane session id
        VideoStats stats; stats.renderedFps = OptionalMetric::of(60);
        sampler.feed(stats);
        QTRY_COMPARE(sampler.rollups(), 1);
        QCOMPARE(rollupLogs.size(), 1);
        QCOMPARE(rollupMetrics.size(), 1);
        QTest::qWait(100);
        QCOMPARE(sampler.rollups(), 1);
        sampler.feed(stats); sampler.finish();
        QCOMPARE(sampler.rollups(), 2);
        QVERIFY(rollupLogs.last().value("partial").toBool());
        sampler.finish();
        QCOMPARE(sampler.rollups(), 2);
    }

    void rollupTimerRunsOnTheSamplerWorker()
    {
        QThread worker;
        auto sampler = new StreamQualitySampler;
        sampler->moveToThread(&worker);
        connect(&worker, &QThread::finished, sampler, &QObject::deleteLater);
        QSemaphore emitted;
        QThread* emissionThread = nullptr;
        connect(sampler, &StreamQualitySampler::rollupEmitted, sampler, [&]() {
            emissionThread = QThread::currentThread(); emitted.release();
        });
        worker.start();
        QMetaObject::invokeMethod(sampler, [sampler]() {
            sampler->setWindowMs(20); sampler->start("worker");
        }, Qt::BlockingQueuedConnection);
        VideoStats stats; stats.renderedFps = OptionalMetric::of(60);
        sampler->feed(stats);
        const bool received = emitted.tryAcquire(1, 5000);
        QMetaObject::invokeMethod(sampler, [sampler]() { sampler->finish(); }, Qt::BlockingQueuedConnection);
        worker.quit(); worker.wait();
        QVERIFY(received);
        QCOMPARE(emissionThread, &worker);
    }

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

    // D-16 (06.3.1, area 1 of the design-review trigger): WR-10's two deferral tests used to live
    // here (a self-removing sink and a sink that adds another sink, both from inside their own
    // dispatch). The contract they proved is gone - an in-dispatch addSink()/removeSink() is now
    // refused loudly (Q_ASSERT_X in debug, std::abort() in release) instead of deferred, closing
    // WR-12 and IN-12 along with it. `tests/tst_log_tee.cpp` proves the new contract, including
    // the death-test pair for exactly these two scenarios.

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
        stats.rttMs = OptionalMetric::of(23.4);
        stats.receivedFps = OptionalMetric::of(60.0);
        stats.decodedFps = OptionalMetric::of(59.8);
        stats.hostProcessingMinMs = OptionalMetric::of(1.1);
        stats.hostProcessingMaxMs = OptionalMetric::of(3.5);
        stats.hostProcessingAvgMs = OptionalMetric::of(2.2);
        // Every other field left absent.

        const QJsonObject report = toQualityReport(stats);

        QCOMPARE(report.value(QStringLiteral("rendered_fps")).toDouble(), 59.94);
        // ADR-0101: rtt_ms rounded half-up to int
        QCOMPARE(report.value(QStringLiteral("rtt_ms")).toInt(), 23);
        QCOMPARE(report.value(QStringLiteral("received_fps")).toDouble(), 60.0);
        QCOMPARE(report.value(QStringLiteral("decoded_fps")).toDouble(), 59.8);
        QCOMPARE(report.value(QStringLiteral("host_latency_min_ms")).toDouble(), 1.1);
        QCOMPARE(report.value(QStringLiteral("host_latency_max_ms")).toDouble(), 3.5);
        QCOMPARE(report.value(QStringLiteral("host_latency_avg_ms")).toDouble(), 2.2);
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

    // Plan 16 / ADR-0101: videoWidth/videoHeight remain parse-only; host latency min/max/avg
    // and received_fps/decoded_fps are sent in the quality report (contract 3.25.0's 13 fields).
    void toQualityReportSendsNoNewField()
    {
        VideoStats stats;
        stats.hostProcessingAvgMs = OptionalMetric::of(2.0);
        stats.videoWidth = OptionalMetric::of(1920.0);
        stats.videoHeight = OptionalMetric::of(1080.0);

        const QJsonObject report = toQualityReport(stats);

        QCOMPARE(report.size(), 13);
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

    void remove_deletesTheFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QualityOutbox outbox(dir.path());

        outbox.put(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"), sampleReport());
        QVERIFY(QFile::exists(dir.filePath(QString::fromLatin1(kSessionA) + QStringLiteral(".json"))));

        outbox.remove(QString::fromLatin1(kSessionA));
        QVERIFY(!QFile::exists(dir.filePath(QString::fromLatin1(kSessionA) + QStringLiteral(".json"))));
    }

    void killedStream_preservesWriteAheadReport_andDrainsOnceOnRelaunch()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        // Phase 1: active session writes report ahead before a simulated crash/kill.
        {
            QualityOutbox activeOutbox(dir.path());
            activeOutbox.put(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"), sampleReport());
            QVERIFY(QFile::exists(dir.filePath(QString::fromLatin1(kSessionA) + QStringLiteral(".json"))));
        }

        // Phase 2: process relaunches. Different account signs in first. Report must NOT be sent.
        {
            QualityOutbox relaunchOutbox(dir.path());
            int sentOther = 0;
            relaunchOutbox.drain(QStringLiteral("gen-other"), QStringLiteral("acct-OTHER"),
                                 [&](const QString&, const QJsonObject&, ControlPlaneClient::Callback) {
                                     ++sentOther;
                                 });
            QCOMPARE(sentOther, 0);
            QVERIFY(!QFile::exists(dir.filePath(QString::fromLatin1(kSessionA) + QStringLiteral(".json"))));
        }

        // Put a fresh write-ahead report for original account to verify drain once.
        {
            QualityOutbox activeOutbox(dir.path());
            activeOutbox.put(QString::fromLatin1(kSessionB), QStringLiteral("acct-1"), sampleReport());
            QVERIFY(QFile::exists(dir.filePath(QString::fromLatin1(kSessionB) + QStringLiteral(".json"))));
        }

        // Phase 3: original account signs in. Drains and delivers exactly once.
        {
            QualityOutbox relaunchOutbox(dir.path());
            int sentAcct1 = 0;
            relaunchOutbox.drain(QStringLiteral("gen-1"), QStringLiteral("acct-1"),
                                 [&](const QString& sessionId, const QJsonObject& report, ControlPlaneClient::Callback callback) {
                                     ++sentAcct1;
                                     QCOMPARE(sessionId, QString::fromLatin1(kSessionB));
                                     QCOMPARE(report.value(QStringLiteral("rendered_fps")).toDouble(), 59.94);
                                     callback(resultWithStatus(200, true));
                                 });
            QCOMPARE(sentAcct1, 1);
            QVERIFY(!QFile::exists(dir.filePath(QString::fromLatin1(kSessionB) + QStringLiteral(".json"))));

            // Subsequent drain does not deliver again.
            int sentAgain = 0;
            relaunchOutbox.drain(QStringLiteral("gen-2"), QStringLiteral("acct-1"),
                                 [&](const QString&, const QJsonObject&, ControlPlaneClient::Callback) {
                                     ++sentAgain;
                                 });
            QCOMPARE(sentAgain, 0);
        }
    }

    // --- ADR-0074 HI02 / HI06: delivered stream time, reported by the app ---------------------
    //
    // `DeliveredStreamTime` times each stream segment on a monotonic clock from the engine's own
    // signals, `StreamTimeOutbox` keeps the running time on disk, and `checkpointStreamTime()` sends
    // it. The tests drive a fake clock and a fake control plane that applies the W04 rule (the larger
    // of the stored and the reported value per session and segment, never an addition).

    void deliveredTimeIgnoresStatsHotkey()
    {
        // The same 200 s stream, three ways: the stats overlay on throughout, hidden throughout
        // (session 6aff8ade: the hotkey was pressed, sampling stopped, 222 s of stream produced 11
        // samples), and toggled off and on mid-stream. The overlay is the only thing that makes the
        // sampler's tick count differ; the delivered time reads none of it.
        struct Variant
        {
            const char* name;
            std::function<bool(qint64 seconds)> overlayShown;
        };
        const QVector<Variant> variants = {
            { "overlay on throughout", [](qint64) { return true; } },
            { "overlay hidden throughout", [](qint64) { return false; } },
            { "hotkey toggled mid-stream", [](qint64 s) { return s < 20 || s >= 180; } },
        };

        VideoStats stats;
        QVERIFY(parseVideoStatsBlock(productionStatsBlock(), &stats));

        QVector<int> samplerTicks;
        qint64 shownSecondsWhenHidden = -1;
        for (int index = 0; index < variants.size(); ++index) {
            const Variant& variant = variants.at(index);
            FakeClock clock;
            DeliveredStreamTime tracker(clock.fn());
            tracker.beginSession(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"));
            QCOMPARE(tracker.connectionStarted(), 1);
            // The ADR-0072 roll-up sampler, fed exactly when the overlay is up (it is the only
            // thing that feeds it): what T006 found undercounting and falling silent.
            StreamQualitySampler sampler;
            sampler.start(QStringLiteral("hotkey"));

            qint64 shownSeconds = 0;
            for (qint64 second = 1; second <= 200; ++second) {
                clock.now = second * 1000;
                if (variant.overlayShown(second)) {
                    ++shownSeconds;
                    sampler.feed(stats);
                    // What the engine logs while the overlay is up: not a segment line, ignored.
                    tracker.sink()(LogLevel::Info, SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO,
                                   productionStatsBlock());
                }
                // Mid-stream the running time is the wall time since the segment started.
                if (second == 100) {
                    QCOMPARE(segmentOf(tracker.pending(), 1).deliveredMs, qint64(100000));
                }
            }
            quitLine(tracker);

            const StreamTimeSnapshot snapshot = tracker.pending();
            QCOMPARE(snapshot.segments.size(), 1);
            QVERIFY2(snapshot.segments.first().closed, variant.name);
            QCOMPARE(snapshot.segments.first().deliveredMs, qint64(200000));
            QCOMPARE(tracker.totalMs(), qint64(200000));
            if (index == 1) {
                shownSecondsWhenHidden = shownSeconds;
            }
            samplerTicks.append(sampler.takeWindow(true).value("n").toInt());
            sampler.finish();
            rollupLogs.clear();
        }
        // The sampler saw the stream only while the overlay was up: a full window, none at all, and
        // a fraction - while every run above delivered the same 200000 ms.
        QVERIFY(samplerTicks.at(0) > samplerTicks.at(2));
        QVERIFY(samplerTicks.at(2) > 0);
        QCOMPARE(samplerTicks.at(1), 0);
        // The hidden run really did show the overlay for none of those seconds, and still delivered
        // all 200. (Nothing in the clock's API takes a statistics value: its only inputs are the
        // engine's connection-started signal and three log lines.)
        QCOMPARE(shownSecondsWhenHidden, qint64(0));
    }

    void deliveredTimeExcludesGrace()
    {
        // One Play with a connection loss in the middle (session 45eb926e's shape, but with the
        // reconnect that build could not do): 100 s of media, the loss noticed 10 s after it
        // happened, a minute of detection, pairing and grace, then 30 s of media again.
        FakeClock clock;
        DeliveredStreamTime tracker(clock.fn());
        tracker.beginSession(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"));

        clock.now = 5000;
        QCOMPARE(tracker.connectionStarted(), 1);
        // The wire went at 105 s; the engine's 10 s control-stream wait ends at 115 s.
        clock.now = 115000;
        lostLine(tracker);
        QCOMPARE(segmentOf(tracker.pending(), 1).deliveredMs, qint64(100000));
        QVERIFY(segmentOf(tracker.pending(), 1).closed);

        // The library reports the same loss as a termination right after; nothing is open, so it
        // changes nothing, and neither does the quit event the engine raises last.
        clock.now = 115002;
        terminatedLine(tracker, -1);
        clock.now = 115010;
        quitLine(tracker);
        QCOMPARE(segmentOf(tracker.pending(), 1).deliveredMs, qint64(100000));
        QVERIFY(!tracker.hasOpenSegment());

        // Sixty seconds of grace, re-pairing and reconnect attempts belong to no segment, at any
        // moment inside them.
        clock.now = 150000;
        QCOMPARE(tracker.totalMs(), qint64(100000));
        QCOMPARE(tracker.pending().segments.size(), 1);

        clock.now = 175000;
        QCOMPARE(tracker.connectionStarted(), 2);
        clock.now = 205000;
        quitLine(tracker);

        const StreamTimeSnapshot snapshot = tracker.pending();
        QCOMPARE(snapshot.segments.size(), 2);
        QCOMPARE(segmentOf(snapshot, 1).deliveredMs, qint64(100000));
        QCOMPARE(segmentOf(snapshot, 2).deliveredMs, qint64(30000));
        QVERIFY(segmentOf(snapshot, 2).closed);
        // 130 s delivered of 200 s of wall time between the first start and the last quit.
        QCOMPARE(tracker.totalMs(), qint64(130000));
    }

    void aSegmentEndsAtItsFirstEngineLineAndTheRestChangeNothing()
    {
        const QVector<QVector<int>> orders = {
            // each entry: 0 quit, 1 lost, 2 terminated(-1), 3 terminated(0)
            { 1, 2, 0 }, { 2, 0 }, { 2, 1, 0 }, { 0, 2 }, { 3, 0 },
        };
        const QVector<qint64> expected = { 40000, 40000, 40000, 50000, 50000 };
        for (int i = 0; i < orders.size(); ++i) {
            FakeClock clock;
            DeliveredStreamTime tracker(clock.fn());
            tracker.beginSession(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"));
            tracker.connectionStarted();
            clock.now = 50000;
            for (int line : orders.at(i)) {
                switch (line) {
                case 0: quitLine(tracker); break;
                case 1: lostLine(tracker); break;
                case 2: terminatedLine(tracker, -1); break;
                default: terminatedLine(tracker, 0); break;
                }
                clock.now += 5;
            }
            QCOMPARE(segmentOf(tracker.pending(), 1).deliveredMs, expected.at(i));
            QCOMPARE(tracker.pending().segments.size(), 1);
        }
    }

    void theTenSecondLossWaitIsTakenOffOnlyWhenTheConnectionWasLost_data()
    {
        QTest::addColumn<int>("kind"); // 0 quit, 1 unexpected disconnect, 2 terminated(code)
        QTest::addColumn<int>("code");
        QTest::addColumn<qint64>("runMs");
        QTest::addColumn<qint64>("expectedMs");

        // A lost connection (disconnect line, or any non-zero termination code) loses the wait.
        QTest::newRow("disconnect 60 s") << 1 << 0 << qint64(60000) << qint64(50000);
        QTest::newRow("terminated -1") << 2 << -1 << qint64(60000) << qint64(50000);
        QTest::newRow("terminated -100") << 2 << -100 << qint64(60000) << qint64(50000);
        QTest::newRow("terminated large negative code") << 2 << -2147483647 << qint64(60000) << qint64(50000);
        // A graceful engine end and the customer's own quit are not reduced.
        QTest::newRow("terminated 0 (graceful)") << 2 << 0 << qint64(60000) << qint64(60000);
        QTest::newRow("quit") << 0 << 0 << qint64(60000) << qint64(60000);
        // Never before the segment's own start: a loss inside the first 10 s delivers nothing.
        QTest::newRow("loss at 10000 ms exactly") << 1 << 0 << qint64(10000) << qint64(0);
        QTest::newRow("loss at 10001 ms") << 1 << 0 << qint64(10001) << qint64(1);
        QTest::newRow("loss at 9999 ms") << 1 << 0 << qint64(9999) << qint64(0);
        QTest::newRow("loss at once") << 2 << -1 << qint64(0) << qint64(0);
        QTest::newRow("graceful at 10000 ms") << 2 << 0 << qint64(10000) << qint64(10000);
        QTest::newRow("quit at 0 ms") << 0 << 0 << qint64(0) << qint64(0);
    }

    void theTenSecondLossWaitIsTakenOffOnlyWhenTheConnectionWasLost()
    {
        QFETCH(int, kind);
        QFETCH(int, code);
        QFETCH(qint64, runMs);
        QFETCH(qint64, expectedMs);

        FakeClock clock;
        clock.now = 123456; // a segment need not start at zero
        DeliveredStreamTime tracker(clock.fn());
        tracker.beginSession(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"));
        tracker.connectionStarted();
        clock.now += runMs;
        if (kind == 0) {
            quitLine(tracker);
        }
        else if (kind == 1) {
            lostLine(tracker);
        }
        else {
            terminatedLine(tracker, code);
        }
        QVERIFY(!tracker.hasOpenSegment());
        QCOMPARE(segmentOf(tracker.pending(), 1).deliveredMs, expectedMs);
        QVERIFY(segmentOf(tracker.pending(), 1).closed);
    }

    void aSegmentIsExactToTheMillisecondAtTheThirtyMinuteBoundary()
    {
        // The history rule needs 30:00.000 told from 30:00.001 (T006's AS-RESOLUTION: the stats-tick
        // count could not). Milliseconds, not seconds, and no rounding up.
        const QVector<qint64> runs = { 1799999, 1800000, 1800001 };
        for (qint64 run : runs) {
            FakeClock clock;
            DeliveredStreamTime tracker(clock.fn());
            tracker.beginSession(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"));
            tracker.connectionStarted();
            clock.now = run;
            QCOMPARE(segmentOf(tracker.pending(), 1).deliveredMs, run); // open: running time
            quitLine(tracker);
            QCOMPARE(segmentOf(tracker.pending(), 1).deliveredMs, run); // closed: the same
        }
    }

    void theDefaultClockIsMonotonicAndMeasuresRealTime()
    {
        DeliveredStreamTime tracker; // the QElapsedTimer clock
        tracker.beginSession(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"));
        tracker.connectionStarted();
        QTest::qWait(60);
        const qint64 first = segmentOf(tracker.pending(), 1).deliveredMs;
        QVERIFY2(first >= 50 && first < 5000, qPrintable(QString::number(first)));
        QTest::qWait(30);
        QVERIFY(segmentOf(tracker.pending(), 1).deliveredMs >= first);
        quitLine(tracker);
        QVERIFY(segmentOf(tracker.pending(), 1).deliveredMs >= first);
    }

    void connectionStartedWhileASegmentIsOpenClosesItUnreducedAndNumbersContinue()
    {
        FakeClock clock;
        DeliveredStreamTime tracker(clock.fn());
        tracker.beginSession(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"));
        QCOMPARE(tracker.connectionStarted(), 1);
        clock.now = 40000;
        // Its end line was never seen; the new connection proves it ended, and nothing says lost.
        QCOMPARE(tracker.connectionStarted(), 2);
        QCOMPARE(segmentOf(tracker.pending(), 1).deliveredMs, qint64(40000));
        QVERIFY(segmentOf(tracker.pending(), 1).closed);
        clock.now = 45000;
        QCOMPARE(segmentOf(tracker.pending(), 2).deliveredMs, qint64(5000));
        QVERIFY(!segmentOf(tracker.pending(), 2).closed);
    }

    void noSessionBoundMeansNothingIsRecorded()
    {
        FakeClock clock;
        DeliveredStreamTime tracker(clock.fn());
        QCOMPARE(tracker.connectionStarted(), 0);
        QVERIFY(!tracker.hasOpenSegment());
        QVERIFY(tracker.pending().segments.isEmpty());

        // A different session starts a fresh count; the same session again keeps counting.
        tracker.beginSession(QString::fromLatin1(kSessionA), QString());
        QCOMPARE(tracker.connectionStarted(), 1);
        tracker.closeOpenSegment();
        tracker.beginSession(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"));
        QCOMPARE(tracker.accountId(), QStringLiteral("acct-1")); // filled in later
        QCOMPARE(tracker.connectionStarted(), 2);
        tracker.setAccountId(QStringLiteral("acct-2"));
        QCOMPARE(tracker.accountId(), QStringLiteral("acct-1")); // never replaced
        tracker.beginSession(QString::fromLatin1(kSessionB), QStringLiteral("acct-1"));
        QVERIFY(tracker.pending().segments.isEmpty());
        QCOMPARE(tracker.connectionStarted(), 1);

        tracker.endSession();
        QCOMPARE(tracker.connectionStarted(), 0);
        QVERIFY(tracker.sessionId().isEmpty());
    }

    void onlyTheThreeEngineLinesAtTheRightCategoryAndPriorityEndASegment_data()
    {
        QTest::addColumn<int>("category");
        QTest::addColumn<int>("priority");
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("kind"); // DeliveredStreamTime::EngineLine
        QTest::addColumn<int>("code");

        const int none = static_cast<int>(DeliveredStreamTime::EngineLine::None);
        const int quit = static_cast<int>(DeliveredStreamTime::EngineLine::QuitEvent);
        const int lost = static_cast<int>(DeliveredStreamTime::EngineLine::UnexpectedDisconnect);
        const int term = static_cast<int>(DeliveredStreamTime::EngineLine::ConnectionTerminated);
        const int app = SDL_LOG_CATEGORY_APPLICATION;
        const int info = SDL_LOG_PRIORITY_INFO;
        const int error = SDL_LOG_PRIORITY_ERROR;

        QTest::newRow("quit") << app << info << QStringLiteral("Quit event received") << quit << 0;
        QTest::newRow("quit with a line break") << app << info << QStringLiteral("Quit event received\n") << quit << 0;
        QTest::newRow("lost") << app << info << kLostLine << lost << 0;
        QTest::newRow("lost with CRLF") << app << info << (kLostLine + QStringLiteral("\r\n")) << lost << 0;
        QTest::newRow("terminated 0") << app << error << QStringLiteral("Connection terminated: 0") << term << 0;
        QTest::newRow("terminated -1") << app << error << QStringLiteral("Connection terminated: -1") << term << -1;
        QTest::newRow("terminated -2147483648") << app << error << QStringLiteral("Connection terminated: -2147483648") << term << int(-2147483647 - 1);
        QTest::newRow("quit at error priority") << app << error << QStringLiteral("Quit event received") << none << 0;
        QTest::newRow("lost at error priority") << app << error << kLostLine << none << 0;
        QTest::newRow("terminated at info priority") << app << info << QStringLiteral("Connection terminated: -1") << none << 0;
        QTest::newRow("quit in another category") << int(SDL_LOG_CATEGORY_VIDEO) << info << QStringLiteral("Quit event received") << none << 0;
        QTest::newRow("quit, wrong case") << app << info << QStringLiteral("quit event received") << none << 0;
        QTest::newRow("quit with a suffix") << app << info << QStringLiteral("Quit event received twice") << none << 0;
        QTest::newRow("terminated, no digits") << app << error << QStringLiteral("Connection terminated: ") << none << 0;
        QTest::newRow("terminated, trailing text") << app << error << QStringLiteral("Connection terminated: 12x") << none << 0;
        QTest::newRow("terminated, overflow") << app << error << QStringLiteral("Connection terminated: 2147483648") << none << 0;
        QTest::newRow("terminated, a bare minus") << app << error << QStringLiteral("Connection terminated: -") << none << 0;
        QTest::newRow("terminated, a leading plus") << app << error << QStringLiteral("Connection terminated: +5") << none << 0;
        QTest::newRow("an unrelated line") << app << info << QStringLiteral("Connection status update: 1") << none << 0;
        QTest::newRow("the stats block") << app << info << productionStatsBlock() << none << 0;
        QTest::newRow("empty") << app << info << QString() << none << 0;
    }

    void onlyTheThreeEngineLinesAtTheRightCategoryAndPriorityEndASegment()
    {
        QFETCH(int, category);
        QFETCH(int, priority);
        QFETCH(QString, text);
        QFETCH(int, kind);
        QFETCH(int, code);

        int parsed = 12345;
        const DeliveredStreamTime::EngineLine line =
            DeliveredStreamTime::classifyEngineLine(category, priority, text, &parsed);
        QCOMPARE(static_cast<int>(line), kind);
        if (line == DeliveredStreamTime::EngineLine::ConnectionTerminated) {
            QCOMPARE(parsed, code);
        }
        else {
            QCOMPARE(parsed, 12345); // left untouched on a non-match
        }
    }

    void theSinkReadsOnlyEngineLinesAndNeverTheStatsOverlay()
    {
        FakeClock clock;
        DeliveredStreamTime tracker(clock.fn());
        tracker.beginSession(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"));
        tracker.connectionStarted();
        clock.now = 30000;
        // Lines that are not segment ends - including every kind of statistics line - change nothing.
        engineLine(tracker, SDL_LOG_PRIORITY_INFO, productionStatsBlock());
        engineLine(tracker, SDL_LOG_PRIORITY_INFO, QStringLiteral("Connection status update: 1"));
        engineLine(tracker, SDL_LOG_PRIORITY_ERROR, QStringLiteral("Some other error entirely"));
        QVERIFY(tracker.hasOpenSegment());
        QCOMPARE(segmentOf(tracker.pending(), 1).deliveredMs, qint64(30000));
    }

    void theClosedHookFiresOnceForEachSegmentThatCloses()
    {
        FakeClock clock;
        DeliveredStreamTime tracker(clock.fn());
        int closedHooks = 0;
        tracker.setSegmentClosedHook([&]() { ++closedHooks; });
        tracker.beginSession(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"));

        quitLine(tracker); // nothing open: no hook
        QCOMPARE(closedHooks, 0);
        tracker.connectionStarted();
        lostLine(tracker);
        terminatedLine(tracker, -1);
        quitLine(tracker);
        QCOMPARE(closedHooks, 1);
        tracker.connectionStarted();
        tracker.connectionStarted(); // closes the open one, then opens the next
        QCOMPARE(closedHooks, 2);
        tracker.closeOpenSegment();
        QCOMPARE(closedHooks, 3);
        tracker.closeOpenSegment();
        QCOMPARE(closedHooks, 3);
    }

    void killedStreamReplaysRecordedTime()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sessionA = QString::fromLatin1(kSessionA);
        FakeStreamTimeServer server;
        server.started.insert(sessionA, true);

        // ---- the run that is killed: a stream that was 25 s old when the process died ----------
        {
            FakeClock clock;
            auto tracker = std::make_shared<DeliveredStreamTime>(clock.fn());
            auto outbox = std::make_shared<StreamTimeOutbox>(dir.path());
            tracker->beginSession(sessionA, QStringLiteral("acct-1"));
            tracker->connectionStarted();

            // The first beat reaches the server and leaves the record.
            clock.now = 10000;
            checkpointStreamTime(tracker, outbox, server.poster());
            QCOMPARE(server.total(sessionA), qint64(10000));
            QVERIFY(outbox->entries(sessionA, QStringLiteral("acct-1")).isEmpty());

            // The second beat finds the network down: the record keeps it.
            clock.now = 20000;
            server.forcedStatus = 0;
            checkpointStreamTime(tracker, outbox, server.poster());
            QCOMPARE(server.total(sessionA), qint64(10000));
            const QVector<StreamSegmentReport> kept =
                outbox->entries(sessionA, QStringLiteral("acct-1"));
            QCOMPARE(kept.size(), 1);
            QCOMPARE(kept.first().segmentNo, 1);
            QCOMPARE(kept.first().deliveredMs, qint64(20000));
            QVERIFY(!kept.first().closed);

            // 5 s later the process is killed: no close line, no teardown, no further beat.
            clock.now = 25000;
        }

        // ---- someone else signs in on the relaunched app: nothing of the record is theirs ------
        {
            StreamTimeOutbox relaunched(dir.path());
            server.forcedStatus = 200;
            server.calls.clear();
            relaunched.drain(QStringLiteral("gen-other"), QStringLiteral("acct-2"), QString(),
                             server.poster());
            QVERIFY2(server.calls.isEmpty(), "a record is never sent under another account");
            QCOMPARE(server.total(sessionA), qint64(10000));
            // And it is not deleted either: its own account still has it.
            QCOMPARE(relaunched.entries(sessionA, QStringLiteral("acct-1")).size(), 1);
        }

        // ---- the same account relaunches: the record goes to the SAME session, once ------------
        {
            StreamTimeOutbox relaunched(dir.path());
            server.calls.clear();
            relaunched.drain(QStringLiteral("gen-1"), QStringLiteral("acct-1"), QString(),
                             server.poster());
            QCOMPARE(server.calls, QStringList{ sessionA + QStringLiteral(":1:20000:0") });
            // The kill lost the 5 s after the last checkpoint, and nothing else: no time was
            // invented for the killed stream and none was counted twice (10 s + 20 s would be 30 s).
            QCOMPARE(server.total(sessionA), qint64(20000));
            QVERIFY(relaunched.entries(sessionA, QStringLiteral("acct-1")).isEmpty());

            // A second drain finds nothing left to send.
            server.calls.clear();
            relaunched.drain(QStringLiteral("gen-1b"), QStringLiteral("acct-1"), QString(),
                             server.poster());
            QVERIFY(server.calls.isEmpty());
            QCOMPARE(server.total(sessionA), qint64(20000));
        }
    }

    void aRecordIsNeverAddedToADifferentSession()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sessionA = QString::fromLatin1(kSessionA);
        const QString sessionB = QString::fromLatin1(kSessionB);
        StreamTimeOutbox outbox(dir.path());
        outbox.put(sessionA, QStringLiteral("acct-1"), StreamSegmentReport{ 1, 20000, false });

        FakeStreamTimeServer server;
        server.started.insert(sessionA, true);
        server.started.insert(sessionB, true);
        outbox.drain(QStringLiteral("gen-1"), QStringLiteral("acct-1"), QString(), server.poster());

        // Only session A was ever named; session B, started later and also streaming, got nothing.
        QCOMPARE(server.calls.size(), 1);
        QVERIFY(server.calls.first().startsWith(sessionA));
        QCOMPARE(server.total(sessionB), qint64(0));
        QCOMPARE(server.total(sessionA), qint64(20000));
    }

    void theLiveSessionsOwnRecordBelongsToTheBeatNotToTheDrain()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StreamTimeOutbox outbox(dir.path());
        outbox.put(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"), StreamSegmentReport{ 1, 1000, false });
        outbox.put(QString::fromLatin1(kSessionB), QStringLiteral("acct-1"), StreamSegmentReport{ 1, 2000, true });

        FakeStreamTimeServer server;
        server.started.insert(QString::fromLatin1(kSessionA), true);
        server.started.insert(QString::fromLatin1(kSessionB), true);
        outbox.drain(QStringLiteral("gen-1"), QStringLiteral("acct-1"),
                     QString::fromLatin1(kSessionA), server.poster());

        QCOMPARE(server.calls.size(), 1);
        QVERIFY(server.calls.first().startsWith(QString::fromLatin1(kSessionB)));
        QCOMPARE(outbox.entries(QString::fromLatin1(kSessionA), QStringLiteral("acct-1")).size(), 1);
    }

    void streamReportReplayAddsNoTime()
    {
        const QString sessionA = QString::fromLatin1(kSessionA);
        const QString sessionB = QString::fromLatin1(kSessionB);
        FakeStreamTimeServer server;
        server.started.insert(sessionA, true);
        server.started.insert(sessionB, true);
        const StreamTimeOutbox::PostFn post = server.poster();
        const auto send = [&](const QString& session, int segment, qint64 ms, bool closed) {
            post(session, StreamSegmentReport{ segment, ms, closed }, [](const ControlPlaneResult&) {});
        };

        // A closed first segment, sent three times (a resend, a retry, a replay after a restart).
        send(sessionA, 1, 30000, true);
        send(sessionA, 1, 30000, true);
        send(sessionA, 1, 30000, true);
        QCOMPARE(server.total(sessionA), qint64(30000));

        // The same segment number replayed with an older, smaller value changes nothing.
        send(sessionA, 1, 20000, false);
        QCOMPARE(server.total(sessionA), qint64(30000));

        // A later, fuller report of the same segment raises it - to the new value, not by it.
        send(sessionA, 1, 35000, true);
        QCOMPARE(server.total(sessionA), qint64(35000));

        // A new segment number is new time; replaying it and the old ones adds nothing.
        send(sessionA, 2, 10000, true);
        QCOMPARE(server.total(sessionA), qint64(45000));
        send(sessionA, 2, 10000, true);
        send(sessionA, 1, 35000, true);
        send(sessionA, 2, 4000, false);
        QCOMPARE(server.total(sessionA), qint64(45000));

        // The same segment number in another session is another key.
        send(sessionB, 1, 7000, true);
        QCOMPARE(server.total(sessionB), qint64(7000));
        QCOMPARE(server.total(sessionA), qint64(45000));
    }

    void aLostReplyIsResentWithoutAddingTime()
    {
        // The report is applied but its reply is lost: the app cannot know, keeps the entry and
        // sends it again at the next beat. The server stores it once.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sessionA = QString::fromLatin1(kSessionA);
        FakeStreamTimeServer server;
        server.started.insert(sessionA, true);
        server.applyThenLoseReply = true;

        FakeClock clock;
        auto tracker = std::make_shared<DeliveredStreamTime>(clock.fn());
        auto outbox = std::make_shared<StreamTimeOutbox>(dir.path());
        tracker->beginSession(sessionA, QStringLiteral("acct-1"));
        tracker->connectionStarted();
        clock.now = 30000;
        quitLine(*tracker);

        checkpointStreamTime(tracker, outbox, server.poster());
        QCOMPARE(server.total(sessionA), qint64(30000));
        QCOMPARE(outbox->entries(sessionA, QStringLiteral("acct-1")).size(), 1);
        QCOMPARE(tracker->pending().segments.size(), 1); // not settled: the app saw no answer

        for (int beat = 0; beat < 3; ++beat) {
            checkpointStreamTime(tracker, outbox, server.poster());
        }
        QCOMPARE(server.total(sessionA), qint64(30000));
        QCOMPARE(server.calls.size(), 4);

        // The reply finally gets through: the entry leaves the record and the segment is settled.
        server.applyThenLoseReply = false;
        checkpointStreamTime(tracker, outbox, server.poster());
        QCOMPARE(server.total(sessionA), qint64(30000));
        QVERIFY(outbox->entries(sessionA, QStringLiteral("acct-1")).isEmpty());
        QVERIFY(tracker->pending().segments.isEmpty());

        // And a settled segment is not sent again.
        server.calls.clear();
        checkpointStreamTime(tracker, outbox, server.poster());
        QVERIFY(server.calls.isEmpty());
    }

    void theRecordKeepsTheEndOfASegmentEvenWhenItIsSmallerThanTheLastCheckpoint()
    {
        // A lost connection takes the 10 s wait off the end, so the closed value can be smaller
        // than the last open checkpoint. The record keeps the closed value; an open checkpoint
        // never reopens a closed segment; open over open keeps the larger.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StreamTimeOutbox outbox(dir.path());
        const QString session = QString::fromLatin1(kSessionA);
        const QString account = QStringLiteral("acct-1");

        outbox.put(session, account, StreamSegmentReport{ 1, 40000, false });
        outbox.put(session, account, StreamSegmentReport{ 1, 35000, false });
        QCOMPARE(outbox.entries(session, account).first().deliveredMs, qint64(40000));

        outbox.put(session, account, StreamSegmentReport{ 1, 32000, true });
        QCOMPARE(outbox.entries(session, account).first().deliveredMs, qint64(32000));
        QVERIFY(outbox.entries(session, account).first().closed);

        outbox.put(session, account, StreamSegmentReport{ 1, 50000, false });
        QCOMPARE(outbox.entries(session, account).first().deliveredMs, qint64(32000));
        QVERIFY(outbox.entries(session, account).first().closed);

        outbox.put(session, account, StreamSegmentReport{ 2, 1000, false });
        QCOMPARE(outbox.entries(session, account).size(), 2);
        QCOMPARE(outbox.entries(session, account).at(0).segmentNo, 1);
        QCOMPARE(outbox.entries(session, account).at(1).segmentNo, 2);
    }

    void aDeliveredEntryLeavesTheRecordOnlyWhileItStillHoldsWhatWasSent()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StreamTimeOutbox outbox(dir.path());
        const QString session = QString::fromLatin1(kSessionA);
        const QString account = QStringLiteral("acct-1");

        outbox.put(session, account, StreamSegmentReport{ 1, 10000, false });
        // A newer checkpoint is written while the older one is still in flight ...
        outbox.put(session, account, StreamSegmentReport{ 1, 20000, false });
        // ... and the older one's reply must not delete it.
        outbox.removeIfUnchanged(session, StreamSegmentReport{ 1, 10000, false });
        QCOMPARE(outbox.entries(session, account).size(), 1);
        QCOMPARE(outbox.entries(session, account).first().deliveredMs, qint64(20000));

        outbox.removeIfUnchanged(session, StreamSegmentReport{ 1, 20000, false });
        QVERIFY(outbox.entries(session, account).isEmpty());
        QVERIFY2(!QFile::exists(dir.filePath(session + QStringLiteral(".json"))),
                 "the last entry takes the file with it");
    }

    void putRefusesWhatCouldEscapeOrBelongToNobody()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StreamTimeOutbox outbox(dir.path());
        const StreamSegmentReport good{ 1, 1000, false };

        outbox.put(QStringLiteral("../escape"), QStringLiteral("acct-1"), good);
        outbox.put(QString::fromLatin1(kSessionA), QString(), good);                        // no account
        outbox.put(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"), StreamSegmentReport{ 0, 1000, false });
        outbox.put(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"), StreamSegmentReport{ 1, -1, false });
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 0);

        // A value beyond the server's integer is held at what it can store.
        outbox.put(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"),
                   StreamSegmentReport{ 1, qint64(1) << 40, true });
        QCOMPARE(outbox.entries(QString::fromLatin1(kSessionA), QStringLiteral("acct-1")).first().deliveredMs,
                 qint64(2147483647));
    }

    void drain_isJudgedByTheSameOutcomeTableAsTheQualityOutbox_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<bool>("ok");
        QTest::addColumn<bool>("kept");

        QTest::newRow("200 delivered") << 200 << true << false;
        QTest::newRow("404 not this account's session") << 404 << false << false;
        QTest::newRow("409 never streamed") << 409 << false << false;
        QTest::newRow("422 malformed") << 422 << false << false;
        QTest::newRow("400 is not a refusal W04 documents") << 400 << false << true;
        QTest::newRow("413 is not a refusal W04 documents") << 413 << false << true;
        QTest::newRow("401 kept for the next token") << 401 << false << true;
        QTest::newRow("403 kept") << 403 << false << true;
        QTest::newRow("500 retried") << 500 << false << true;
        QTest::newRow("503 retried") << 503 << false << true;
        QTest::newRow("429 retried") << 429 << false << true;
        QTest::newRow("no answer retried") << 0 << false << true;
    }

    void drain_isJudgedByTheSameOutcomeTableAsTheQualityOutbox()
    {
        QFETCH(int, status);
        QFETCH(bool, ok);
        QFETCH(bool, kept);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StreamTimeOutbox outbox(dir.path());
        const QString session = QString::fromLatin1(kSessionA);
        outbox.put(session, QStringLiteral("acct-1"), StreamSegmentReport{ 1, 12345, true });

        int sent = 0;
        outbox.drain(QStringLiteral("gen-1"), QStringLiteral("acct-1"), QString(),
                     [&](const QString& id, const StreamSegmentReport& segment,
                         ControlPlaneClient::Callback callback) {
                         QCOMPARE(id, session);
                         QCOMPARE(segment.deliveredMs, qint64(12345));
                         ++sent;
                         callback(resultWithStatus(status, ok));
                     });
        QCOMPARE(sent, 1);
        QCOMPARE(!outbox.entries(session, QStringLiteral("acct-1")).isEmpty(), kept);
    }

    void drain_authFailedStopsTheRestAndTheSameTokenIsNotRetried()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StreamTimeOutbox outbox(dir.path());
        outbox.put(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"), StreamSegmentReport{ 1, 1, true });
        outbox.put(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"), StreamSegmentReport{ 2, 2, true });
        outbox.put(QString::fromLatin1(kSessionB), QStringLiteral("acct-1"), StreamSegmentReport{ 1, 3, true });

        int sent = 0;
        const StreamTimeOutbox::PostFn refusing = [&](const QString&, const StreamSegmentReport&,
                                                      ControlPlaneClient::Callback callback) {
            ++sent;
            callback(resultWithStatus(401));
        };
        outbox.drain(QStringLiteral("gen-1"), QStringLiteral("acct-1"), QString(), refusing);
        QCOMPARE(sent, 1);
        outbox.drain(QStringLiteral("gen-1"), QStringLiteral("acct-1"), QString(), refusing);
        QCOMPARE(sent, 1);

        int sentWithNewToken = 0;
        outbox.drain(QStringLiteral("gen-2"), QStringLiteral("acct-1"), QString(),
                     [&](const QString&, const StreamSegmentReport&, ControlPlaneClient::Callback callback) {
                         ++sentWithNewToken;
                         callback(resultWithStatus(200, true));
                     });
        QCOMPARE(sentWithNewToken, 3);
        QVERIFY(!outbox.hasRecords());
    }

    void drain_abortedByASignOutStopsAndKeepsEverything()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StreamTimeOutbox outbox(dir.path());
        outbox.put(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"), StreamSegmentReport{ 1, 1, true });
        outbox.put(QString::fromLatin1(kSessionB), QStringLiteral("acct-1"), StreamSegmentReport{ 1, 2, true });

        int sent = 0;
        ControlPlaneClient::Callback pending;
        outbox.drain(QStringLiteral("gen-1"), QStringLiteral("acct-1"), QString(),
                     [&](const QString&, const StreamSegmentReport&, ControlPlaneClient::Callback callback) {
                         ++sent;
                         pending = callback;
                     });
        QCOMPARE(sent, 1);
        outbox.cancel(); // the account signs out mid-drain
        pending(ControlPlaneResult::aborted());
        QCOMPARE(sent, 1); // the loop did not carry on under the next account
        QVERIFY(outbox.hasRecords());

        // The next account's own drain is not swallowed by the abandoned one.
        int sentAfter = 0;
        outbox.drain(QStringLiteral("gen-2"), QStringLiteral("acct-1"), QString(),
                     [&](const QString&, const StreamSegmentReport&, ControlPlaneClient::Callback callback) {
                         ++sentAfter;
                         callback(resultWithStatus(200, true));
                     });
        QCOMPARE(sentAfter, 2);
    }

    void drain_withNoTokenOrNoAccountOrAForeignFileSendsNothing()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StreamTimeOutbox outbox(dir.path());
        outbox.put(QString::fromLatin1(kSessionA), QStringLiteral("acct-1"), StreamSegmentReport{ 1, 1, true });
        QFile junk(dir.filePath(QStringLiteral("not-a-record.json")));
        QVERIFY(junk.open(QIODevice::WriteOnly));
        junk.write("{ nonsense");
        junk.close();

        int sent = 0;
        const StreamTimeOutbox::PostFn post = [&](const QString&, const StreamSegmentReport&,
                                                  ControlPlaneClient::Callback) { ++sent; };
        outbox.drain(QString(), QStringLiteral("acct-1"), QString(), post);
        outbox.drain(QStringLiteral("gen-1"), QString(), QString(), post);
        QCOMPARE(sent, 0);
        QVERIFY(QFile::exists(junk.fileName())); // neither call looked at the folder

        // A drain for another account sends nothing, keeps the record, and removes only the junk.
        outbox.drain(QStringLiteral("gen-1"), QStringLiteral("acct-2"), QString(), post);
        QCOMPARE(sent, 0);
        QCOMPARE(outbox.entries(QString::fromLatin1(kSessionA), QStringLiteral("acct-1")).size(), 1);
        QVERIFY(!QFile::exists(junk.fileName()));
    }

    void theCheckpointStopsForASessionTheServerRefusesForGood()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sessionA = QString::fromLatin1(kSessionA);
        FakeStreamTimeServer server;
        server.started.insert(sessionA, true);
        server.forcedStatus = 404;

        FakeClock clock;
        auto tracker = std::make_shared<DeliveredStreamTime>(clock.fn());
        auto outbox = std::make_shared<StreamTimeOutbox>(dir.path());
        tracker->beginSession(sessionA, QStringLiteral("acct-1"));
        tracker->connectionStarted();
        clock.now = 10000;
        checkpointStreamTime(tracker, outbox, server.poster());
        QCOMPARE(server.calls.size(), 1);
        QVERIFY(outbox->entries(sessionA, QStringLiteral("acct-1")).isEmpty());

        // The next beat finds nothing to send for this session: it is not hammered every 10 s.
        clock.now = 20000;
        checkpointStreamTime(tracker, outbox, server.poster());
        QCOMPARE(server.calls.size(), 1);
        QVERIFY(tracker->pending().segments.isEmpty());
    }

    void aSessionNotYetActiveIsRetriedWhileOpenAndLetGoOnceClosed()
    {
        // 409 ("this session never streamed", `stream_started_at` null): the first seconds of a
        // stream, before the server's ACTIVE. An open segment is simply tried again at the next
        // beat, by which time the server has started it; a closed one means it never started.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sessionA = QString::fromLatin1(kSessionA);
        FakeStreamTimeServer server; // started not set: every report is a 409

        FakeClock clock;
        auto tracker = std::make_shared<DeliveredStreamTime>(clock.fn());
        auto outbox = std::make_shared<StreamTimeOutbox>(dir.path());
        tracker->beginSession(sessionA, QStringLiteral("acct-1"));
        tracker->connectionStarted();

        clock.now = 2000;
        checkpointStreamTime(tracker, outbox, server.poster());
        QVERIFY(outbox->entries(sessionA, QStringLiteral("acct-1")).isEmpty()); // dropped, as the 409 says
        QCOMPARE(tracker->pending().segments.size(), 1);

        // ACTIVE arrives: the next beat is taken, with the full running time since the start.
        server.started.insert(sessionA, true);
        clock.now = 12000;
        checkpointStreamTime(tracker, outbox, server.poster());
        QCOMPARE(server.total(sessionA), qint64(12000));

        // A segment that closes before the server ever started it is let go after one 409.
        const QString sessionB = QString::fromLatin1(kSessionB);
        tracker->beginSession(sessionB, QStringLiteral("acct-1"));
        clock.now = 20000;
        tracker->connectionStarted();
        clock.now = 23000;
        quitLine(*tracker);
        checkpointStreamTime(tracker, outbox, server.poster());
        QVERIFY(tracker->pending().segments.isEmpty());
        QCOMPARE(server.total(sessionB), qint64(0));
    }

    void anOpenSegmentIsCheckpointedAtEveryLivenessBeat()
    {
        // The existing 10-second beat is the cadence: `LivenessTimer::tick()` posts its report
        // through `ControlPlaneClient`, whose observer runs the checkpoint just before it leaves.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sessionA = QString::fromLatin1(kSessionA);

        auto* network = new RecordingNetworkAccessManager;
        ControlPlaneClient client;
        client.setNetworkAccessManager(network);
        client.setAccessToken(QStringLiteral("opaque-access-token"));

        FakeClock clock;
        auto tracker = std::make_shared<DeliveredStreamTime>(clock.fn());
        auto outbox = std::make_shared<StreamTimeOutbox>(dir.path());
        tracker->beginSession(sessionA, QStringLiteral("acct-1"));
        tracker->connectionStarted();
        client.setLivenessBeatObserver([&]() {
            checkpointStreamTime(tracker, outbox,
                                 [&](const QString& id, const StreamSegmentReport& segment,
                                     ControlPlaneClient::Callback callback) {
                                     client.postStreamTime(id, segment.segmentNo, segment.deliveredMs,
                                                           segment.closed, callback);
                                 });
        });

        LivenessTimer liveness;
        liveness.setControlPlane(&client);
        liveness.start(sessionA);          // the first beat
        clock.now = 10000;
        liveness.tick();                   // the second
        clock.now = 20000;
        liveness.tick();                   // the third
        liveness.stop();

        QStringList streamTimePaths;
        QList<qint64> reportedMs;
        for (int i = 0; i < network->paths.size(); ++i) {
            if (network->paths.at(i).endsWith(QLatin1String("/stream-time"))) {
                streamTimePaths.append(network->paths.at(i));
                reportedMs.append(QJsonDocument::fromJson(network->bodies.at(i))
                                      .object().value(QStringLiteral("delivered_ms")).toInteger());
            }
        }
        QCOMPARE(streamTimePaths.size(), 3);
        for (const QString& path : streamTimePaths) {
            QCOMPARE(path, QStringLiteral("/api/sessions/%1/stream-time").arg(sessionA));
        }
        QCOMPARE(reportedMs, (QList<qint64>{ 0, 10000, 20000 }));
        // Each beat also kept its liveness report: the checkpoint added to the beat, not replaced it.
        int liveness_reports = 0;
        for (const QString& path : network->paths) {
            liveness_reports += path.endsWith(QLatin1String("/liveness")) ? 1 : 0;
        }
        QCOMPARE(liveness_reports, 3);
        QTRY_VERIFY(outbox->entries(sessionA, QStringLiteral("acct-1")).isEmpty());
    }

};

QTEST_MAIN(TstStreamStats)

#include "tst_stream_stats.moc"
