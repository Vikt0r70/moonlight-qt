/*****************************************************************************
 * SeatHub fork - LogShipper/LogSpool (06.3.1 D-14, D-09, SC-5, Plan 21): SeatHub's own log
 * shipper - a bounded in-memory queue, a bounded on-disk spool that holds lines until the SDK
 * may take them, and a scrub pass, sitting behind LogTee. A stub HandOff stands in for
 * telemetry.cpp's real one throughout - this suite never links sentry-native (that is
 * tst_telemetry.pro's job).
 *
 * This process runs in two roles, chosen by its own first argument:
 *   - `tst_log_shipper --leftover-child <spool-dir>`: constructs a real LogSpool pointed at
 *     <spool-dir>, appends one line, then `_exit(0)`s WITHOUT running destructors - leaving the
 *     spool file's companion QLockFile on disk with this process's own (now-dead) pid, exactly
 *     what a real crash would leave. Used by the "leftover from a dead process" test below.
 *   - no matching argument: the normal QTest run.
 *
 * Build recipe (nothing is on PATH machine-wide - Qt and MSVC are both absolute):
 *   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
 *   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;C:\Qt\Tools\Ninja;%PATH%
 *   cd tests && qmake tst_log_shipper.pro && jom && tst_log_shipper.exe -o tst_log_shipper-out.txt,txt
 *****************************************************************************/

#include <QtTest>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>
#include <QVector>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

// This file defines `main()` below. `<SDL.h>` otherwise `#define`s `main` to `SDL_main` - the
// same fix `tst_log_tee.cpp` uses for the same reason (log_tee.cpp needs SDL2).
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "seathub/log_shipper.h"
#include "seathub/log_tee.h"

namespace {

/// Captures everything a stub `HandOff` receives, thread-safely (the worker thread calls it).
class StubHandOff
{
public:
    /// `answer` is returned for every line; `false` leaves the caller's own line in the spool.
    explicit StubHandOff(bool answer = true)
        : m_answer(answer)
    {
    }

    bool operator()(const ShippedLine& line)
    {
        QMutexLocker locker(&m_mutex);
        m_received.append(line);
        return m_answer;
    }

    QVector<ShippedLine> received() const
    {
        QMutexLocker locker(&m_mutex);
        return m_received;
    }

    int countMatching(const QString& marker) const
    {
        QMutexLocker locker(&m_mutex);
        int count = 0;
        for (const ShippedLine& line : m_received) {
            if (line.body.contains(marker)) {
                ++count;
            }
        }
        return count;
    }

    void setAnswer(bool answer)
    {
        QMutexLocker locker(&m_mutex);
        m_answer = answer;
    }

private:
    mutable QMutex m_mutex;
    QVector<ShippedLine> m_received;
    bool m_answer;
};

/// Every `HandOff` a test below builds captures `&stub` (and every other test-function local) BY
/// REFERENCE, and the worker thread keeps calling it until `stop()` joins that thread. Declaring
/// one of these as the LAST local in a test function makes it destruct FIRST on the way out
/// (reverse declaration order) - on EVERY exit path, including a `QVERIFY`/`QCOMPARE` failure's
/// own early `return` - so the worker is always stopped before `stub`/`dir` go away, never after.
struct StopShipperOnScopeExit
{
    ~StopShipperOnScopeExit() { LogShipper::instance().stop(); }
};

double epochSecondsNow()
{
    return static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
}

/// `spool-<ownpid>.jsonl` in `directory` - the exact naming `log_shipper.cpp` uses, so a test can
/// inject a line directly onto disk (the 7-day age-out test) without a public API for it.
QString ownSpoolFilePath(const QString& directory)
{
    return QDir(directory).filePath(
        QStringLiteral("spool-%1.jsonl").arg(QCoreApplication::applicationPid()));
}

void appendRawSpoolLine(const QString& directory, const QString& body, double loggedAt)
{
    QDir().mkpath(directory);
    QJsonObject obj;
    obj.insert(QStringLiteral("level"), static_cast<int>(LogLevel::Info));
    obj.insert(QStringLiteral("body"), body);
    obj.insert(QStringLiteral("logged_at"), loggedAt);
    obj.insert(QStringLiteral("session_id"), QString());
    obj.insert(QStringLiteral("host_id"), QString());
    obj.insert(QStringLiteral("trace_id"), QString());

    QFile file(ownSpoolFilePath(directory));
    file.open(QIODevice::Append | QIODevice::Text);
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    file.write("\n");
}

// --- the leftover-child role -------------------------------------------------------------------

int runLeftoverChild(const QString& directory)
{
    // Constructed on the stack so its destructor WOULD run on a normal return - but `_exit()`
    // below skips that deliberately, leaving the spool file and its companion `QLockFile` behind
    // with this (about-to-be-dead) process's own pid, exactly like a real crash would.
    LogSpool spool(directory);
    ShippedLine line;
    line.level = LogLevel::Info;
    line.body = QStringLiteral("leftover-child-line");
    line.loggedAt = epochSecondsNow();
    spool.append(line);

    fflush(nullptr);
    _exit(0); // unreachable after this
}

} // namespace

class TstLogShipper : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        m_appPath = QCoreApplication::applicationFilePath();
        QVERIFY(QFileInfo(m_appPath).isFile());
        LogTee::install();
    }

    void cleanup()
    {
        // Resets every per-run counter and the queue; the sink registration itself is left alone
        // (LogShipper never removes its own sink - D-16's contract, mirrored in log_shipper.cpp).
        LogShipper::instance().stop();
        // CR-02: a test that shortened pauseHandOff()'s bounded wait must not leak that override
        // into a later, unrelated test.
        LogShipper::instance().setPauseTimeoutForTests(-1);
    }

    // --- the tracer: hold, then drain in order with the original time and session -------------

    void linesWaitInTheSpoolUntilSignedInThenShipInOrderWithOriginalTimeAndSession()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StubHandOff stub;
        StopShipperOnScopeExit stopGuard;
        const QString marker = QStringLiteral("tracer-marker");

        LogShipper::instance().setSpoolDirectoryForTests(dir.path());
        LogShipper::instance().start([&stub](const ShippedLine& line) { return stub(line); });
        LogShipper::instance().publishIds(QStringLiteral("session-tracer"), QString(), QString());
        LogShipper::instance().setCanShip(false);

        const double beforeLog = epochSecondsNow();
        for (int i = 0; i < 3; ++i) {
            qInfo() << marker << i;
        }

        // Give the worker a moment to make its decision (spool, since canShip is false) before
        // asserting nothing reached the stub yet.
        QTest::qWait(300);
        QCOMPARE(stub.countMatching(marker), 0);
        QVERIFY(QFileInfo::exists(ownSpoolFilePath(dir.path())));

        LogShipper::instance().setCanShip(true);
        QTRY_COMPARE_WITH_TIMEOUT(stub.countMatching(marker), 3, 5000);

        const QVector<ShippedLine> received = stub.received();
        int seen = 0;
        for (const ShippedLine& line : received) {
            if (!line.body.contains(marker)) {
                continue;
            }
            QVERIFY2(line.body.contains(QString::number(seen)),
                     "lines did not arrive in the order they were logged");
            QCOMPARE(line.sessionId, QStringLiteral("session-tracer"));
            QVERIFY2(line.loggedAt >= beforeLog - 1.0 && line.loggedAt <= beforeLog + 5.0,
                     "loggedAt was not the original capture time");
            ++seen;
        }
        QCOMPARE(seen, 3);
    }

    // --- a debug line never reaches the queue or the spool -------------------------------------

    void aDebugLineNeverReachesTheQueueOrSpool()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StubHandOff stub;
        StopShipperOnScopeExit stopGuard;
        const QString marker = QStringLiteral("debug-filter-marker");

        LogShipper::instance().setSpoolDirectoryForTests(dir.path());
        LogShipper::instance().start([&stub](const ShippedLine& line) { return stub(line); });
        LogShipper::instance().setCanShip(true);

        qDebug() << marker;
        qInfo() << marker << "info-sentinel";

        QTRY_COMPARE_WITH_TIMEOUT(stub.countMatching(QStringLiteral("info-sentinel")), 1, 5000);
        QCOMPARE(stub.countMatching(marker), 1); // only the info line, not the debug one
    }

    // --- the queue keeps the newest kQueueLines while held --------------------------------------

    void theQueueKeepsTheNewestLinesWhenMoreArriveWhileHeld()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StubHandOff stub;
        StopShipperOnScopeExit stopGuard;
        const QString marker = QStringLiteral("burst-marker");

        LogShipper::instance().setSpoolDirectoryForTests(dir.path());
        LogShipper::instance().start([&stub](const ShippedLine& line) { return stub(line); });
        LogShipper::instance().setCanShip(true);
        LogShipper::instance().holdWorkerForTests(true);

        const int total = 1200;
        for (int i = 0; i < total; ++i) {
            qInfo().noquote() << QStringLiteral("%1 %2").arg(marker).arg(i);
        }

        LogShipper::instance().holdWorkerForTests(false);
        QTRY_COMPARE_WITH_TIMEOUT(stub.countMatching(marker), LogShipper::kQueueLines, 10000);

        // The survivors are the NEWEST kQueueLines - indexes [total - kQueueLines, total).
        bool sawOldest = false;
        int lowestSurviving = total;
        for (const ShippedLine& line : stub.received()) {
            if (!line.body.contains(marker)) {
                continue;
            }
            const QStringList parts = line.body.split(QLatin1Char(' '));
            const int index = parts.last().toInt();
            lowestSurviving = std::min(lowestSurviving, index);
            if (index < total - LogShipper::kQueueLines) {
                sawOldest = true;
            }
        }
        QVERIFY2(!sawOldest, "an old line that should have been dropped survived");
        QCOMPARE(lowestSurviving, total - LogShipper::kQueueLines);
    }

    // --- the spool file never exceeds its cap; oldest first -------------------------------------

    void theSpoolFileNeverExceedsItsCapAndDropsTheOldestFirst()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StubHandOff stub;
        StopShipperOnScopeExit stopGuard;

        // Seed ~2.5 MiB directly on disk (bypassing the queue/worker entirely - no race with a
        // burst still being appended one line at a time) - past the 2 MiB cap before this
        // process's own `LogSpool` ever appends a single line of its own.
        const QString padding(100 * 1024, QLatin1Char('x')); // ~100 KiB per line
        const int seeded = 25; // ~2.5 MiB
        const double now = epochSecondsNow();
        for (int i = 0; i < seeded; ++i) {
            appendRawSpoolLine(dir.path(), QStringLiteral("seed-%1-%2").arg(i).arg(padding), now);
        }

        LogShipper::instance().setSpoolDirectoryForTests(dir.path());
        LogShipper::instance().start([&stub](const ShippedLine& line) { return stub(line); });
        LogShipper::instance().setCanShip(false); // this one new line also spools

        qInfo().noquote() << "prune-trigger";
        // Waits for exactly this ONE queued line to be processed - deterministic, since nothing
        // here can be dropped by the queue's own 1000-line cap.
        LogShipper::instance().drainBeforeSignOut();

        const QString path = ownSpoolFilePath(dir.path());
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(QFileInfo(path).size() <= LogShipper::kSpoolBytes);
        QVERIFY(QFileInfo(path).size() > 0);

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        bool sawSeedZero = false;
        bool sawPruneTrigger = false;
        int survivingSeedLines = 0;
        while (!file.atEnd()) {
            const QByteArray raw = file.readLine();
            if (raw.trimmed().isEmpty()) {
                continue;
            }
            const QJsonObject obj = QJsonDocument::fromJson(raw).object();
            const QString body = obj.value(QStringLiteral("body")).toString();
            if (body.startsWith(QStringLiteral("seed-0-"))) {
                sawSeedZero = true;
            }
            if (body.startsWith(QStringLiteral("seed-"))) {
                ++survivingSeedLines;
            }
            if (body == QStringLiteral("prune-trigger")) {
                sawPruneTrigger = true;
            }
        }
        QVERIFY2(!sawSeedZero, "the oldest seeded line survived the cap");
        QVERIFY2(survivingSeedLines < seeded, "no seeded lines were dropped despite exceeding the cap");
        QVERIFY2(sawPruneTrigger, "the newest line - the one that triggered pruning - must survive");
    }

    // --- a spooled line older than kSpoolMaxAgeDays is dropped at drain -------------------------

    void aSpooledLineOlderThanSevenDaysIsDroppedAtDrain()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StubHandOff stub;
        StopShipperOnScopeExit stopGuard;

        const double old = epochSecondsNow() - (LogShipper::kSpoolMaxAgeDays + 1) * 24.0 * 3600.0;
        appendRawSpoolLine(dir.path(), QStringLiteral("stale-spooled-line"), old);

        LogShipper::instance().setSpoolDirectoryForTests(dir.path());
        LogShipper::instance().start([&stub](const ShippedLine& line) { return stub(line); });
        LogShipper::instance().setCanShip(true);

        qInfo() << "fresh-drain-trigger";
        QTRY_COMPARE_WITH_TIMEOUT(stub.countMatching(QStringLiteral("fresh-drain-trigger")), 1, 5000);

        QCOMPARE(stub.countMatching(QStringLiteral("stale-spooled-line")), 0);
    }

    // --- a stub answering false keeps the line in the spool --------------------------------------

    void aStubHandOffAnsweringFalseKeepsTheLineInTheSpool()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StubHandOff stub(false);
        StopShipperOnScopeExit stopGuard;
        const QString marker = QStringLiteral("refused-marker");

        LogShipper::instance().setSpoolDirectoryForTests(dir.path());
        LogShipper::instance().start([&stub](const ShippedLine& line) { return stub(line); });
        LogShipper::instance().setCanShip(true);

        qInfo() << marker;
        QTRY_VERIFY_WITH_TIMEOUT(stub.countMatching(marker) >= 1, 5000);

        QTest::qWait(200);
        QVERIFY2(QFileInfo::exists(ownSpoolFilePath(dir.path())),
                 "a line the stub refused never landed back in the spool");
    }

    // --- drainBeforeSignOut: everything captured before it ships before it returns --------------

    void drainBeforeSignOutHandsOverEveryCapturedLineBeforeReturning()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StubHandOff stub;
        StopShipperOnScopeExit stopGuard;
        const QString marker = QStringLiteral("signout-marker");

        LogShipper::instance().setSpoolDirectoryForTests(dir.path());
        LogShipper::instance().start([&stub](const ShippedLine& line) { return stub(line); });
        LogShipper::instance().setCanShip(true);

        for (int i = 0; i < 5; ++i) {
            qInfo() << marker << i;
        }
        LogShipper::instance().drainBeforeSignOut();

        QCOMPARE(stub.countMatching(marker), 5);
    }

    // --- pauseHandOff spools until resumeHandOff --------------------------------------------------

    void pauseHandOffSpoolsUntilResumeHandOff()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StubHandOff stub;
        StopShipperOnScopeExit stopGuard;
        const QString marker = QStringLiteral("pause-marker");

        LogShipper::instance().setSpoolDirectoryForTests(dir.path());
        LogShipper::instance().start([&stub](const ShippedLine& line) { return stub(line); });
        LogShipper::instance().setCanShip(true);

        LogShipper::instance().pauseHandOff();
        qInfo() << marker << "while-paused";
        QTest::qWait(300);
        QCOMPARE(stub.countMatching(marker), 0);

        LogShipper::instance().resumeHandOff();
        QTRY_COMPARE_WITH_TIMEOUT(stub.countMatching(marker), 1, 5000);
    }

    // --- CR-02 regression: pauseHandOff()'s notify no longer races a lost wakeup -----------------

    void pauseHandOffReturnsPromptlyOnceAnInFlightHandOffFinishes()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        std::mutex blockMutex;
        std::condition_variable blockCv;
        bool release = false;
        std::atomic<bool> entered{ false };
        auto handOff = [&](const ShippedLine&) {
            entered.store(true);
            std::unique_lock<std::mutex> lock(blockMutex);
            blockCv.wait(lock, [&] { return release; });
            return true;
        };

        // Declared LAST (see the comment on StopShipperOnScopeExit above the class) so its own
        // destructor - which joins the worker - runs FIRST on every exit path, before the
        // blockMutex/blockCv/release/entered locals the worker's captured lambda references go
        // away.
        StopShipperOnScopeExit stopGuard;

        LogShipper::instance().setSpoolDirectoryForTests(dir.path());
        LogShipper::instance().start(handOff);
        LogShipper::instance().setCanShip(true);

        qInfo() << "pause-race-marker";
        QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 5000);

        QElapsedTimer timer;
        timer.start();
        std::thread pauser([] { LogShipper::instance().pauseHandOff(); });

        // Give pauseHandOff() a moment to actually reach its own wait before the in-flight
        // hand-off finishes - the exact interleaving CR-02 is about (the notify racing the
        // waiter's own lock-check-block sequence).
        QTest::qWait(200);
        {
            std::lock_guard<std::mutex> lock(blockMutex);
            release = true;
        }
        blockCv.notify_all();

        pauser.join();
        QVERIFY2(timer.elapsed() < 2000,
                 "pauseHandOff() did not return promptly once the in-flight hand-off finished - "
                 "this is CR-02's lost-wakeup regression, or its bounded-timeout fallback firing "
                 "when it should not have to");

        LogShipper::instance().resumeHandOff();
    }

    void pauseHandOffProceedsAfterItsBoundedTimeoutWhenAHandOffNeverFinishes()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        std::mutex blockMutex;
        std::condition_variable blockCv;
        bool release = false;
        std::atomic<bool> entered{ false };
        auto handOff = [&](const ShippedLine&) {
            entered.store(true);
            // "Wedged": only unblocks once the test itself releases it, well after
            // pauseHandOff()'s own bounded wait below has already timed out and returned.
            std::unique_lock<std::mutex> lock(blockMutex);
            blockCv.wait(lock, [&] { return release; });
            return true;
        };

        StopShipperOnScopeExit stopGuard; // see the comment above - destructs FIRST.

        // CR-02: shortens pauseHandOff()'s bounded wait (production: 5000ms) so this test does
        // not have to wait the full 5s to prove the fallback fires.
        LogShipper::instance().setPauseTimeoutForTests(300);
        LogShipper::instance().setSpoolDirectoryForTests(dir.path());
        LogShipper::instance().start(handOff);
        LogShipper::instance().setCanShip(true);

        qInfo() << "pause-timeout-marker";
        QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 5000);

        QElapsedTimer timer;
        timer.start();
        LogShipper::instance().pauseHandOff(); // the hand-off above is still wedged right now.
        const qint64 elapsed = timer.elapsed();
        QVERIFY2(elapsed >= 250,
                 "pauseHandOff() returned before its own bounded timeout could have fired");
        QVERIFY2(elapsed < 2000,
                 "pauseHandOff() did not proceed after its bounded timeout - CR-02's fallback "
                 "did not fire and the calling thread was left hanging");

        // Let the wedged hand-off finish so the worker (and StopShipperOnScopeExit's stop() call)
        // can actually join cleanly.
        {
            std::lock_guard<std::mutex> lock(blockMutex);
            release = true;
        }
        blockCv.notify_all();
        LogShipper::instance().resumeHandOff();
    }

    // --- a leftover spool file from a dead process is adopted and drained -----------------------

    void aLeftoverSpoolFileFromADeadProcessIsAdoptedAndDrained()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QProcess child;
        child.setProgram(m_appPath);
        child.setArguments({ QStringLiteral("--leftover-child"), dir.path() });
        child.start();
        QVERIFY2(child.waitForStarted(5000), "the leftover-child process must start");
        QVERIFY2(child.waitForFinished(10000), "the leftover-child process never finished");

        StubHandOff stub;
        StopShipperOnScopeExit stopGuard;
        LogShipper::instance().setSpoolDirectoryForTests(dir.path());
        LogShipper::instance().start([&stub](const ShippedLine& line) { return stub(line); });
        LogShipper::instance().setCanShip(true);

        qInfo() << "adopt-drain-trigger";
        QTRY_COMPARE_WITH_TIMEOUT(stub.countMatching(QStringLiteral("leftover-child-line")), 1, 5000);
    }

    // --- scrub -----------------------------------------------------------------------------------

    void scrubRemovesSecretsAndKeepsTheIpv4Address()
    {
        const QString scrubbed = LogShipper::scrub(QStringLiteral(
            "GET /launch?&rikey=abc123&rikeyid=45-67 from 203.0.113.7 "
            "Authorization: Bearer sk-real-token-value "
            "pin: 4321 "
            "pair?devicename=roth&updateState=1&salt=aa11&clientcert=deadbeef"
            "&clientchallenge=beadfeed&serverchallengeresp=cafef00d&clientpairingsecret=f00dcafe "
            "-----BEGIN CERTIFICATE-----\nMIIBabc\n-----END CERTIFICATE-----"));

        QVERIFY(!scrubbed.contains(QStringLiteral("abc123")));
        QVERIFY(!scrubbed.contains(QStringLiteral("45-67")));
        QVERIFY(!scrubbed.contains(QStringLiteral("sk-real-token-value")));
        QVERIFY(!scrubbed.contains(QStringLiteral("4321")));
        QVERIFY(!scrubbed.contains(QStringLiteral("aa11")));
        QVERIFY(!scrubbed.contains(QStringLiteral("deadbeef")));
        QVERIFY(!scrubbed.contains(QStringLiteral("beadfeed")));
        QVERIFY(!scrubbed.contains(QStringLiteral("cafef00d")));
        QVERIFY(!scrubbed.contains(QStringLiteral("f00dcafe")));
        QVERIFY(!scrubbed.contains(QStringLiteral("MIIBabc")));
        QVERIFY2(scrubbed.contains(QStringLiteral("203.0.113.7")),
                 "the customer's public IP must survive scrubbing");
    }

    // --- the per-run shipped-bytes cap ------------------------------------------------------------

    void afterTheShippedBytesCapOneCapReachedLineShipsAndNothingMore()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StubHandOff stub;
        StopShipperOnScopeExit stopGuard;

        LogShipper::instance().setSpoolDirectoryForTests(dir.path());
        LogShipper::instance().start([&stub](const ShippedLine& line) { return stub(line); });
        LogShipper::instance().setCanShip(true);

        // ~500 KB per line; 17 lines is ~8.5 MB, comfortably past the 8 MiB cap without ever
        // approaching the 1000-line queue cap.
        const QString bigBody(500 * 1024, QLatin1Char('y'));
        const int lineCount = 17;
        for (int i = 0; i < lineCount; ++i) {
            qInfo().noquote() << QStringLiteral("big-line-%1 %2").arg(i).arg(bigBody);
        }
        qInfo() << "after-cap-marker";

        QTRY_VERIFY_WITH_TIMEOUT(stub.countMatching(QStringLiteral("log cap reached")) >= 1, 10000);
        QTest::qWait(500); // let anything still in flight settle

        QCOMPARE(stub.countMatching(QStringLiteral("log cap reached")), 1);
        QCOMPARE(stub.countMatching(QStringLiteral("after-cap-marker")), 0);
        QVERIFY(LogShipper::instance().shippedBytesForTests() <= LogShipper::kShippedBytesPerRun);
    }

private:
    QString m_appPath;
};

int main(int argc, char* argv[])
{
    if (argc > 2 && std::strcmp(argv[1], "--leftover-child") == 0) {
        return runLeftoverChild(QString::fromLocal8Bit(argv[2]));
    }

    QCoreApplication app(argc, argv);
    TstLogShipper testCase;
    return QTest::qExec(&testCase, argc, argv);
}

#include "tst_log_shipper.moc"
