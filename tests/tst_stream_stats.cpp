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

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>

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
#include "seathub/stream_stats.h"

namespace {

// A full, normal-session "Global video stats" block: every conditional line present. Field
// values are chosen so no two are equal, so a test that reads the wrong field fails loudly.
QString fullStatsBlock()
{
    return QStringLiteral(
        "----------------------------------------------------------\n"
        "Video stream: 1920x1080 59.94 FPS (Codec: H.264)\n"
        "Incoming frame rate from network: 60.00 FPS\n"
        "Decoding frame rate: 59.98 FPS\n"
        "Rendering frame rate: 59.96 FPS\n"
        "Frames dropped by your network connection: 0.42%\n"
        "Frames dropped due to network jitter: 0.10%\n"
        "Average network latency: 23 ms (variance: 4 ms)\n"
        "Average decoding time: 3.21 ms\n"
        "Average frame queue delay: 1.05 ms\n"
        "Average rendering time (including monitor V-sync latency): 2.77 ms\n");
}

// The same block with `m_VideoDecoderCtx == nullptr` (no "Video stream:" line) and the network
// latency line reading "N/A" - `stringifyVideoStats()`'s own fallback when `stats.lastRtt == 0`.
QString blockWithMissingLinesAndNoRtt()
{
    return QStringLiteral(
        "----------------------------------------------------------\n"
        "Incoming frame rate from network: 60.00 FPS\n"
        "Decoding frame rate: 59.98 FPS\n"
        "Rendering frame rate: 59.96 FPS\n"
        "Frames dropped by your network connection: 0.42%\n"
        "Frames dropped due to network jitter: 0.10%\n"
        "Average network latency: N/A\n"
        "Average decoding time: 3.21 ms\n"
        "Average frame queue delay: 1.05 ms\n"
        "Average rendering time (including monitor V-sync latency): 2.77 ms\n");
}

const char* const kDashesPrefix = "----------------------------------------------------------\n";

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

    // --- parseVideoStatsBlock / toQualityReport --------------------------------------------

    void parseVideoStatsBlock_fullBlock_fillsEveryField()
    {
        VideoStats stats;
        const bool matched = parseVideoStatsBlock(fullStatsBlock(), &stats);

        QVERIFY(matched);
        QVERIFY(stats.renderedFps.present);
        QCOMPARE(stats.renderedFps.value, 59.94);
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

    void parseVideoStatsBlock_naLatencyAndMissingVideoStreamLine_leavesOnlyThoseAbsent()
    {
        VideoStats stats;
        const bool matched = parseVideoStatsBlock(blockWithMissingLinesAndNoRtt(), &stats);

        QVERIFY(matched);
        QVERIFY2(!stats.renderedFps.present, "no 'Video stream:' line in this block");
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
             QString::fromLatin1(kDashesPrefix) + fullStatsBlock().mid(qstrlen(kDashesPrefix)));

        QCOMPARE(emitCount, 1);
        QVERIFY(captured.renderedFps.present);
        QCOMPARE(captured.renderedFps.value, 59.94);
    }

    void statsWatcher_dashesLineWithNoPrecedingTitle_isIgnored()
    {
        StatsWatcher watcher;
        int emitCount = 0;
        connect(&watcher, &StatsWatcher::videoStatsParsed, &watcher,
                [&](VideoStats) { ++emitCount; });

        const LogTee::Sink sink = watcher.sink();
        // No title message first - an unrelated line that happens to start with the same dashes.
        sink(LogLevel::Info, SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO,
             QString::fromLatin1(kDashesPrefix) + QStringLiteral("Frames dropped by your network connection: 0.00%\n"));

        QCOMPARE(emitCount, 0);
    }

    void statsWatcher_anotherApplicationInfoMessageBetweenTitleAndBlock_resetsTheState()
    {
        StatsWatcher watcher;
        int emitCount = 0;
        connect(&watcher, &StatsWatcher::videoStatsParsed, &watcher,
                [&](VideoStats) { ++emitCount; });

        const LogTee::Sink sink = watcher.sink();
        sink(LogLevel::Info, SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO,
             QStringLiteral("Global video stats"));
        // Some other APPLICATION/INFO line in between - the block is no longer "the next one".
        sink(LogLevel::Info, SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO,
             QStringLiteral("an unrelated application/info line"));
        sink(LogLevel::Info, SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO,
             QString::fromLatin1(kDashesPrefix) + fullStatsBlock().mid(qstrlen(kDashesPrefix)));

        QCOMPARE(emitCount, 0);
    }

    void statsWatcher_unrelatedCategoryOrPriorityBetweenTitleAndBlock_isIgnoredNotReset()
    {
        StatsWatcher watcher;
        int emitCount = 0;
        connect(&watcher, &StatsWatcher::videoStatsParsed, &watcher,
                [&](VideoStats) { ++emitCount; });

        const LogTee::Sink sink = watcher.sink();
        sink(LogLevel::Info, SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO,
             QStringLiteral("Global video stats"));
        // A different SDL category, and a Qt-origin message (category/priority -1) - neither is
        // "the next SDL info message", so neither should disturb the state.
        sink(LogLevel::Warning, SDL_LOG_CATEGORY_VIDEO, SDL_LOG_PRIORITY_WARN,
             QStringLiteral("an unrelated video-category warning"));
        sink(LogLevel::Info, -1, -1, QStringLiteral("an unrelated Qt info line"));
        sink(LogLevel::Info, SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO,
             QString::fromLatin1(kDashesPrefix) + fullStatsBlock().mid(qstrlen(kDashesPrefix)));

        QCOMPARE(emitCount, 1);
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
};

QTEST_MAIN(TstStreamStats)

#include "tst_stream_stats.moc"
