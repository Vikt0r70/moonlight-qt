/*****************************************************************************
 * SeatHub fork - sentry-native crash reporting tests (06.3.1 D-01, D-02, D-05).
 *
 * This suite exercises the real sentry-native + crashpad stack, not a mock of it: a child
 * process crashes for real (a null write through a volatile pointer), and the parent's own fake
 * Sentry endpoint (a plain QTcpServer) receives the minidump POST crashpad's out-of-process
 * handler sends. The two things this suite substitutes for the real thing are the ingest
 * endpoint (a loopback listener instead of sentry.io) and the handler folder (a temp copy
 * holding crashpad_handler.exe alone, so these tests never register a WER helper-module value on
 * the machine that runs them - see theTestHandlerFolderHasNoWerModule() below).
 *
 * This process runs in two different roles, chosen by its own first argument:
 *   - `tst_telemetry --crash-child <dsn> <db> <handler>`: calls SeatHubTelemetry::startWith()
 *     with those exact values, then crashes with a null write. Used for the "a real crash reaches
 *     the listener" and "a no-DSN crash stays pending" halves of the behaviour list.
 *   - `tst_telemetry --start-child <dsn> <db> <handler>`: calls startWith() with a DSN against an
 *     EXISTING database directory (one a `--crash-child` run with no DSN already crashed into),
 *     then stays alive for a few seconds before exiting normally - long enough for the pending
 *     report's upload to complete before its own handler exits with it (crashpad_handler.exe does
 *     not outlive its app, SPIKE T8a).
 *   - no matching argument: the normal QTest run.
 *
 * Neither child branch constructs a QCoreApplication, matching the production call site
 * (app/main.cpp calls SeatHubTelemetry::start() before QCoreApplication exists). Neither child
 * branch, nor the parent test process, ever calls SeatHubTelemetry::start() itself - only
 * startWith(), with explicit test-only Options - so the real %LOCALAPPDATA%\Seven Hills\SeatHub\
 * crash-db and the real %TEMP%\SeatHub-*.dmp legacy dumps are never touched by this suite.
 *
 * Build recipe (nothing is on PATH machine-wide - Qt and MSVC are both absolute):
 *   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
 *   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
 *   cd tests && qmake tst_telemetry.pro && jom && tst_telemetry.exe -o tst_telemetry-out.txt,txt
 *****************************************************************************/

#include <QtTest>

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <cstdio>
#include <cstring>
#include <memory>

#include "seathub/token_store.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "seathub/telemetry.h"

#ifndef SEATHUB_SENTRY_BIN_DIR
#error "SEATHUB_SENTRY_BIN_DIR must be defined by the .pro file (the sentry-native install's bin folder)"
#endif
#ifndef FORK_ROOT
#error "FORK_ROOT must be defined by the .pro file (the fork's repository root)"
#endif

namespace {

// --- the two child roles ------------------------------------------------------------------

/// Builds the Options a child call uses from its own argv, so both child roles share one parser.
SeatHubTelemetry::Options optionsFromChildArgs(int argc, char* argv[])
{
    SeatHubTelemetry::Options options;
    // argv[0] = exe, argv[1] = "--crash-child" or "--start-child",
    // argv[2] = dsn (may be empty), argv[3] = db dir, argv[4] = handler path.
    options.dsn = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString();
    options.databaseDir = argc > 3 ? QString::fromLocal8Bit(argv[3]) : QString();
    options.handlerPath = argc > 4 ? QString::fromLocal8Bit(argv[4]) : QString();
    options.environment = QStringLiteral("test");
    options.release = QStringLiteral("tst_telemetry@0.0.0");
    return options;
}

int runCrashChild(int argc, char* argv[])
{
    // A child whose handler failed to launch must never raise a WER dialog that hangs the test
    // runner - crashpad's own filter still catches the crash regardless of this flag.
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);

    const SeatHubTelemetry::Options options = optionsFromChildArgs(argc, argv);
    SeatHubTelemetry::startWith(options);

    volatile int* crashPointer = nullptr;
    *crashPointer = 42;
    return 0; // unreachable
}

int runStartChild(int argc, char* argv[])
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);

    const SeatHubTelemetry::Options options = optionsFromChildArgs(argc, argv);
    SeatHubTelemetry::startWith(options);

    // The handler does not outlive its app (SPIKE T8a: about 1 s after the app dies). Stay alive
    // long enough for the pending report's upload to finish first.
    ::Sleep(8000);
    return 0;
}

// --- Plan 15's own two child roles ---------------------------------------------------------
//
// Both share `optionsFromChildArgs()`'s db/handler positions, but always start with an EMPTY dsn
// (the scenario both behaviours are about: "a process started without a DSN"), and drive the DSN
// in only through `SeatHubTelemetry::applyHandout()`, exactly as `SeatHubClient` will in Task 2.

/// `tst_telemetry --handout-child <db> <handler> <dsn> <environment>`: starts with no DSN, applies
/// one handout (the one re-init this process ever performs), then crashes. Proves the tracer's
/// whole chain: no-DSN start -> handout -> re-init -> cache written -> crash uploads through the
/// new handler.
int runHandoutChild(int argc, char* argv[])
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);

    SeatHubTelemetry::Options options;
    options.databaseDir = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString();
    options.handlerPath = argc > 3 ? QString::fromLocal8Bit(argv[3]) : QString();
    options.environment = QStringLiteral("test");
    options.release = QStringLiteral("tst_telemetry@0.0.0");
    options.dsn = QString(); // the scenario: this process starts with no DSN at all.
    SeatHubTelemetry::startWith(options);

    SeatHubTelemetry::Handout handout;
    handout.dsn = argc > 4 ? QString::fromLocal8Bit(argv[4]) : QString();
    handout.environment = argc > 5 ? QString::fromLocal8Bit(argv[5]) : QStringLiteral("test");
    handout.logs = true;
    SeatHubTelemetry::applyHandout(handout);

    volatile int* crashPointer = nullptr;
    *crashPointer = 42;
    return 0; // unreachable
}

/// `tst_telemetry --rotation-child <db> <handler> <dsn1> <dsn2>`: starts with no DSN, adopts
/// `dsn1` (the one re-init), then applies `dsn2` (a rotation - only the cache changes), prints
/// `INIT_COUNT=<n> CLOSE_COUNT=<n>` to stdout, then crashes. Proves there is no second re-init and
/// no second `sentry_close()` for a later handout in the same run (C.3 rule 2, prohibition 1).
int runRotationChild(int argc, char* argv[])
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);

    SeatHubTelemetry::Options options;
    options.databaseDir = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString();
    options.handlerPath = argc > 3 ? QString::fromLocal8Bit(argv[3]) : QString();
    options.environment = QStringLiteral("test");
    options.release = QStringLiteral("tst_telemetry@0.0.0");
    options.dsn = QString();
    SeatHubTelemetry::startWith(options);

    SeatHubTelemetry::Handout first;
    first.dsn = argc > 4 ? QString::fromLocal8Bit(argv[4]) : QString();
    first.environment = QStringLiteral("test");
    first.logs = true;
    SeatHubTelemetry::applyHandout(first);

    SeatHubTelemetry::Handout second;
    second.dsn = argc > 5 ? QString::fromLocal8Bit(argv[5]) : QString();
    second.environment = QStringLiteral("test");
    second.logs = true;
    SeatHubTelemetry::applyHandout(second);

    std::fprintf(stdout, "INIT_COUNT=%d CLOSE_COUNT=%d\n", SeatHubTelemetry::initCallCount(),
                 SeatHubTelemetry::closeCallCount());
    std::fflush(stdout);

    volatile int* crashPointer = nullptr;
    *crashPointer = 42;
    return 0; // unreachable
}

// --- the fake Sentry endpoint --------------------------------------------------------------

/// Wires `server` to answer every request with `200 {}` and record the request line's path in
/// `receivedPaths`. Only the request line is parsed - crashpad's real POST bodies are chunked and
/// gzip-encoded envelopes/multipart, and this suite only needs to know which endpoint the handler
/// reached, not to decode what it sent.
void wireFakeSentryListener(QTcpServer* server, QStringList* receivedPaths)
{
    QObject::connect(server, &QTcpServer::newConnection, server, [server, receivedPaths]() {
        while (QTcpSocket* socket = server->nextPendingConnection()) {
            auto buffer = std::make_shared<QByteArray>();
            auto responded = std::make_shared<bool>(false);
            QObject::connect(socket, &QTcpSocket::readyRead, socket,
                              [socket, buffer, responded, receivedPaths]() {
                *buffer += socket->readAll();
                if (*responded) {
                    return;
                }
                const int headerEnd = buffer->indexOf("\r\n\r\n");
                if (headerEnd < 0) {
                    return;
                }
                const int lineEnd = buffer->indexOf("\r\n");
                const QString requestLine = QString::fromLatin1(buffer->left(lineEnd));
                const QStringList parts = requestLine.split(QLatin1Char(' '));
                if (parts.size() >= 2) {
                    receivedPaths->append(parts.at(1));
                }
                *responded = true;
                static const QByteArray body = QByteArrayLiteral("{}");
                QByteArray response = QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                                         "Content-Length: ") + QByteArray::number(body.size())
                    + QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + body;
                socket->write(response);
                socket->flush();
                socket->disconnectFromHost();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
        }
    });
}

bool anyStartsWith(const QStringList& list, const QString& prefix)
{
    for (const QString& entry : list) {
        if (entry.startsWith(prefix)) {
            return true;
        }
    }
    return false;
}

bool hasAnyDumpFile(const QString& databaseDir)
{
    QDir reportsDir(databaseDir + QStringLiteral("/reports"));
    return !reportsDir.entryList(QStringList(QStringLiteral("*.dmp")), QDir::Files).isEmpty();
}

} // namespace

class TstTelemetry : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // Behaviour 4: the handler folder these tests use is a temp copy holding
        // crashpad_handler.exe ALONE - crashpad_wer.dll never sits beside it, so sentry_init
        // never registers a WER helper-module value in this machine's HKCU
        // (RuntimeExceptionHelperModules) while these tests run.
        QVERIFY(m_handlerDir.isValid());
        const QString sourceHandler =
            QStringLiteral(SEATHUB_SENTRY_BIN_DIR "/crashpad_handler.exe");
        QVERIFY2(QFileInfo(sourceHandler).isFile(),
                 qPrintable(QStringLiteral("sentry-native install not found: %1 "
                                           "(build scripts/build-seathub.ps1 first)")
                                .arg(sourceHandler)));
        m_handlerPath = m_handlerDir.filePath(QStringLiteral("crashpad_handler.exe"));
        QVERIFY(QFile::copy(sourceHandler, m_handlerPath));

        m_appPath = QCoreApplication::applicationFilePath();
        QVERIFY(QFileInfo(m_appPath).isFile());
    }

    // Plan 15's cache tests use the REAL `SeatHubTelemetry::cachePath()` /
    // `TokenStore::defaultDirectory()` (there is no override for either - see `cachePath()`'s own
    // comment), which `main()` redirects into a `qttest`-sandboxed folder before this test binary
    // does anything else (`QStandardPaths::setTestModeEnabled(true)`, set beside the org/app names
    // `token_store.cpp` documents - both roles and this parent process need the SAME two names to
    // resolve the SAME sandboxed directory). That sandbox persists across runs, so every test that
    // touches it starts and ends with nothing left over.
    void init()
    {
        SeatHubTelemetry::deleteCache();
    }

    void cleanup()
    {
        SeatHubTelemetry::deleteCache();
    }

    // --- behaviour 4: the test handler folder carries no WER module ---------------------------

    void theTestHandlerFolderHasNoWerModule()
    {
        const QStringList entries = QDir(m_handlerDir.path()).entryList(QDir::Files);
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first(), QStringLiteral("crashpad_handler.exe"));
        QVERIFY(!QFileInfo(m_handlerDir.filePath(QStringLiteral("crashpad_wer.dll"))).exists());
    }

    // --- behaviour 1: a real crash reaches the fake Sentry endpoint ---------------------------

    void crashChildReachesTheMinidumpEndpoint()
    {
        QTemporaryDir dbDir;
        QVERIFY(dbDir.isValid());

        QTcpServer server;
        QStringList receivedPaths;
        wireFakeSentryListener(&server, &receivedPaths);
        QVERIFY2(server.listen(QHostAddress::LocalHost, 0), "the fake Sentry listener must bind 127.0.0.1:0");

        const QString dsn = QStringLiteral("http://publickey@127.0.0.1:%1/1").arg(server.serverPort());

        QProcess child;
        child.setProgram(m_appPath);
        child.setArguments({ QStringLiteral("--crash-child"), dsn, dbDir.path(), m_handlerPath });
        child.start();
        QVERIFY2(child.waitForStarted(5000), "the crash-child process must start");

        QTRY_VERIFY_WITH_TIMEOUT(anyStartsWith(receivedPaths, QStringLiteral("/api/1/minidump/")), 15000);

        // QProcess::waitForFinished() returns false for a process that terminated abnormally
        // (its own documented behaviour: a crash is reported through the Crashed QProcess::
        // ProcessError, and that error is exactly what makes waitForFinished() report failure
        // here) - so its return value is not the crash signal to check. QProcess::NotRunning
        // plus QProcess::CrashExit together are.
        child.waitForFinished(10000);
        QTRY_COMPARE_WITH_TIMEOUT(child.state(), QProcess::NotRunning, 10000);
        QCOMPARE(child.exitStatus(), QProcess::CrashExit);

        // Give crashpad_handler.exe (a separate process) a moment to exit before the temp
        // directories are torn down (SPIKE T8a: it exits about 1 s after the app it serves dies).
        QTest::qWait(1500);
    }

    // --- behaviour 2: a no-DSN crash stays pending; a later DSN init uploads it ----------------

    void pendingReportUploadsAtTheNextDsnInit()
    {
        QTemporaryDir dbDir;
        QVERIFY(dbDir.isValid());

        // Step 1: a no-DSN crash. sentry_init is not fatal with an empty DSN (SPIKE C.1 fact 1);
        // the handler starts, and the crash is written to the crashpad database but stays
        // pending - there is no URL to upload it to yet.
        QProcess firstChild;
        firstChild.setProgram(m_appPath);
        firstChild.setArguments({ QStringLiteral("--crash-child"), QString(), dbDir.path(), m_handlerPath });
        firstChild.start();
        QVERIFY2(firstChild.waitForStarted(5000), "the first (no-DSN) crash-child process must start");
        QVERIFY(firstChild.waitForFinished(5000));

        QTRY_VERIFY_WITH_TIMEOUT(hasAnyDumpFile(dbDir.path()), 10000);

        // Step 2: a later process, on the SAME database, with a DSN. It does not crash - it just
        // starts, which is enough for crashpad's upload thread to scan pending reports and send
        // the one step 1 left behind (SPIKE T5a/T5b).
        QTcpServer server;
        QStringList receivedPaths;
        wireFakeSentryListener(&server, &receivedPaths);
        QVERIFY2(server.listen(QHostAddress::LocalHost, 0), "the fake Sentry listener must bind 127.0.0.1:0");
        const QString dsn = QStringLiteral("http://publickey@127.0.0.1:%1/1").arg(server.serverPort());

        QProcess secondChild;
        secondChild.setProgram(m_appPath);
        secondChild.setArguments({ QStringLiteral("--start-child"), dsn, dbDir.path(), m_handlerPath });
        secondChild.start();
        QVERIFY2(secondChild.waitForStarted(5000), "the second (DSN) start-child process must start");

        QTRY_VERIFY_WITH_TIMEOUT(anyStartsWith(receivedPaths, QStringLiteral("/api/1/minidump/")), 15000);

        QVERIFY(secondChild.waitForFinished(15000));
        QCOMPARE(secondChild.exitCode(), 0);

        QTest::qWait(1500);
    }

    // --- behaviour 3: removeLegacyDumps ---------------------------------------------------------

    void removeLegacyDumpsDeletesOnlyTheDmpFiles()
    {
        QTemporaryDir logDir;
        QVERIFY(logDir.isValid());

        const QString dmp1 = logDir.filePath(QStringLiteral("SeatHub-1.dmp"));
        const QString dmp2 = logDir.filePath(QStringLiteral("SeatHub-2.dmp"));
        const QString keptLog = logDir.filePath(QStringLiteral("SeatHub-1.log"));
        for (const QString& path : { dmp1, dmp2, keptLog }) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("x");
        }

        SeatHubTelemetry::removeLegacyDumps(logDir.path());

        QVERIFY(!QFileInfo(dmp1).exists());
        QVERIFY(!QFileInfo(dmp2).exists());
        QVERIFY(QFileInfo(keptLog).exists());
    }

    // --- Task 3: the hook order is guarded by a test, not just by the code review -------------

    void hookOrderPutsSeatHubTelemetryAfterUpstreamsFilterWithAFallback()
    {
        QFile file(QStringLiteral(FORK_ROOT "/app/main.cpp"));
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), "app/main.cpp must be readable");
        const QString text = QString::fromUtf8(file.readAll());

        const int blockStart =
            text.indexOf(QStringLiteral("#ifdef Q_OS_WIN32\n    // Create a crash dump"));
        QVERIFY2(blockStart >= 0, "the Q_OS_WIN32 crash-dump block was not found in app/main.cpp");
        const int blockEnd = text.indexOf(QStringLiteral("#endif"), blockStart);
        QVERIFY2(blockEnd > blockStart, "no matching #endif found for the Q_OS_WIN32 crash-dump block");
        const QString block = text.mid(blockStart, blockEnd - blockStart);

        const int upstreamIndex =
            block.indexOf(QStringLiteral("SetUnhandledExceptionFilter(UnhandledExceptionHandler);"));
        QVERIFY2(upstreamIndex >= 0, "upstream's SetUnhandledExceptionFilter call was not found");

        const int seatHubIndex = block.indexOf(QStringLiteral("SeatHubTelemetry::start()"), upstreamIndex);
        QVERIFY2(seatHubIndex > upstreamIndex,
                 "SeatHubTelemetry::start() must come after upstream's SetUnhandledExceptionFilter "
                 "call - the reverse order loses every SEH crash to crashpad (SPIKE T2 vs T3)");

        // The fallback: a second SetUnhandledExceptionFilter(UnhandledExceptionHandler) call,
        // after SeatHubTelemetry::start(), inside the guarded if - SPIKE T14: a handler file that
        // exists but cannot launch otherwise leaves crashpad's filter in place and every crash of
        // that session self-terminates with no dump anywhere.
        const int fallbackIndex = block.indexOf(
            QStringLiteral("SetUnhandledExceptionFilter(UnhandledExceptionHandler);"), seatHubIndex);
        QVERIFY2(fallbackIndex > seatHubIndex,
                 "no fallback SetUnhandledExceptionFilter(UnhandledExceptionHandler) re-install "
                 "found after SeatHubTelemetry::start() (SPIKE T14)");
    }

    // --- Plan 15: parseHandout ------------------------------------------------------------------

    void parseHandoutReadsNullDsnAsEmpty()
    {
        QJsonObject body;
        body.insert(QStringLiteral("dsn"), QJsonValue());
        body.insert(QStringLiteral("environment"), QStringLiteral("production"));
        body.insert(QStringLiteral("logs"), true);

        const std::optional<SeatHubTelemetry::Handout> handout = SeatHubTelemetry::parseHandout(body);
        QVERIFY(handout.has_value());
        QVERIFY(handout->dsn.isEmpty());
        QCOMPARE(handout->environment, QStringLiteral("production"));
        QVERIFY(handout->logs);
    }

    void parseHandoutRejectsABodyMissingLogs()
    {
        QJsonObject body;
        body.insert(QStringLiteral("dsn"), QJsonValue());
        body.insert(QStringLiteral("environment"), QStringLiteral("production"));
        // "logs" deliberately absent - a malformed handout, never a reason to change anything.

        QVERIFY(!SeatHubTelemetry::parseHandout(body).has_value());
    }

    // --- Plan 15: acceptDsn ----------------------------------------------------------------------

    void acceptDsnChecksSchemeAndHost()
    {
        QVERIFY(!SeatHubTelemetry::acceptDsn(QStringLiteral("http://k@o1.ingest.de.sentry.io/1")));
        QVERIFY(!SeatHubTelemetry::acceptDsn(QStringLiteral("https://k@evil.example/1")));
        QVERIFY(SeatHubTelemetry::acceptDsn(QStringLiteral("https://k@o1.ingest.de.sentry.io/2")));
    }

    // --- Plan 15: applyHandout's cache and logsEnabled(), all in-process -------------------------
    //
    // SeatHubTelemetry::started() is false for the whole life of this parent test process (only
    // the child roles above ever call start()/startWith()), so applyHandout() never reaches the
    // SDK here - only the cache file and logsEnabled() change, which is exactly what these prove.

    void applyHandoutCachesAnAcceptedDsnAndTogglesLogsEnabled()
    {
        QVERIFY(!SeatHubTelemetry::started());

        SeatHubTelemetry::Handout on;
        on.dsn = QStringLiteral("https://k@o1.ingest.de.sentry.io/2");
        on.environment = QStringLiteral("development");
        on.logs = true;
        SeatHubTelemetry::applyHandout(on);

        QVERIFY(QFileInfo(SeatHubTelemetry::cachePath()).exists());
        const std::optional<SeatHubTelemetry::Handout> cached = SeatHubTelemetry::readCache();
        QVERIFY(cached.has_value());
        QCOMPARE(cached->dsn, on.dsn);
        QCOMPARE(cached->environment, on.environment);
        QVERIFY(cached->logs);
        QVERIFY(SeatHubTelemetry::logsEnabled());

        const int closesBefore = SeatHubTelemetry::closeCallCount();

        SeatHubTelemetry::Handout off;
        off.dsn = QString();
        off.environment = QStringLiteral("development");
        off.logs = true;
        SeatHubTelemetry::applyHandout(off);

        QVERIFY(!QFileInfo(SeatHubTelemetry::cachePath()).exists());
        QVERIFY(!SeatHubTelemetry::logsEnabled());
        // Prohibition 1: an "off" handout never calls sentry_close().
        QCOMPARE(SeatHubTelemetry::closeCallCount(), closesBefore);
    }

    void applyHandoutIgnoresARejectedDsnAndLeavesTheCacheAlone()
    {
        SeatHubTelemetry::Handout baseline;
        baseline.dsn = QStringLiteral("https://k@o1.ingest.de.sentry.io/2");
        baseline.environment = QStringLiteral("production");
        baseline.logs = true;
        SeatHubTelemetry::applyHandout(baseline);
        QVERIFY(SeatHubTelemetry::logsEnabled());

        SeatHubTelemetry::Handout rejected;
        rejected.dsn = QStringLiteral("https://k@evil.example/1");
        rejected.environment = QStringLiteral("production");
        rejected.logs = false;
        SeatHubTelemetry::applyHandout(rejected);

        const std::optional<SeatHubTelemetry::Handout> cached = SeatHubTelemetry::readCache();
        QVERIFY(cached.has_value());
        QCOMPARE(cached->dsn, baseline.dsn);
        QVERIFY(SeatHubTelemetry::logsEnabled()); // untouched - still whatever the baseline set.
    }

    void aRejectedDsnIsNeverWrittenToALogLine()
    {
        static QByteArray s_capturedMessages;
        s_capturedMessages.clear();
        const QtMessageHandler previous = qInstallMessageHandler(
            [](QtMsgType, const QMessageLogContext&, const QString& message) {
                s_capturedMessages += message.toUtf8();
                s_capturedMessages += '\n';
            });

        const QString secretKey = QStringLiteral("SECRET-MARKER-4F7K9Q");
        SeatHubTelemetry::Handout rejected;
        rejected.dsn = QStringLiteral("https://%1@evil.example/1").arg(secretKey);
        rejected.environment = QStringLiteral("production");
        rejected.logs = true;
        SeatHubTelemetry::applyHandout(rejected);

        qInstallMessageHandler(previous);

        QVERIFY2(!s_capturedMessages.contains(secretKey.toUtf8()),
                 "a rejected DSN's own text must never reach a log line (prohibition 2)");
    }

    // --- Plan 15: TokenStore::clearAll() leaves telemetry.json in place --------------------------

    void tokenStoreClearAllLeavesTelemetryJsonInPlace()
    {
        TokenStore store; // default directory - the SAME directory cachePath() resolves to.
        QVERIFY(store.storeToken(TokenStore::accessTokenName(), QStringLiteral("a-credential")));

        SeatHubTelemetry::Handout handout;
        handout.dsn = QStringLiteral("https://k@o1.ingest.de.sentry.io/2");
        handout.environment = QStringLiteral("production");
        handout.logs = true;
        SeatHubTelemetry::writeCache(handout);
        QVERIFY(QFileInfo(SeatHubTelemetry::cachePath()).exists());

        QVERIFY(store.clearAll());

        QVERIFY(!store.hasToken(TokenStore::accessTokenName()));
        QVERIFY2(QFileInfo(SeatHubTelemetry::cachePath()).exists(),
                 "TokenStore::clearAll() must never sweep telemetry.json - it only sweeps *.dpapi");

        store.clearToken(TokenStore::accessTokenName());
    }

    // --- Task 1 tracer: no-DSN start -> handout -> re-init -> cache -> crash uploads -------------

    void handoutChildReInitsOnceCachesTheHandoutAndUploadsAfterACrash()
    {
        QTemporaryDir dbDir;
        QVERIFY(dbDir.isValid());

        QTcpServer server;
        QStringList receivedPaths;
        wireFakeSentryListener(&server, &receivedPaths);
        QVERIFY2(server.listen(QHostAddress::LocalHost, 0),
                 "the fake Sentry listener must bind 127.0.0.1:0");
        const QString dsn = QStringLiteral("http://publickey@127.0.0.1:%1/1").arg(server.serverPort());
        const QString environment = QStringLiteral("development");

        QProcess child;
        child.setProgram(m_appPath);
        child.setArguments({ QStringLiteral("--handout-child"), dbDir.path(), m_handlerPath, dsn,
                            environment });
        child.start();
        QVERIFY2(child.waitForStarted(5000), "the handout-child process must start");

        QTRY_VERIFY_WITH_TIMEOUT(anyStartsWith(receivedPaths, QStringLiteral("/api/1/minidump/")), 15000);

        child.waitForFinished(10000);
        QTRY_COMPARE_WITH_TIMEOUT(child.state(), QProcess::NotRunning, 10000);
        QCOMPARE(child.exitStatus(), QProcess::CrashExit);

        QTest::qWait(1500);

        const std::optional<SeatHubTelemetry::Handout> cached = SeatHubTelemetry::readCache();
        QVERIFY2(cached.has_value(),
                 "the handout-child's cache write must land at the SAME cachePath() this parent "
                 "resolves (same org/app name, same QStandardPaths test mode)");
        QCOMPARE(cached->dsn, dsn);
        QCOMPARE(cached->environment, environment);
        QVERIFY(cached->logs);
    }

    // --- rotation: no second re-init, no second sentry_close(), only the first DSN uploads -------

    void rotationChildNeverReInitsTwiceAndOnlyTheFirstDsnEverUploads()
    {
        QTemporaryDir dbDir;
        QVERIFY(dbDir.isValid());

        QTcpServer serverA;
        QStringList receivedA;
        wireFakeSentryListener(&serverA, &receivedA);
        QVERIFY2(serverA.listen(QHostAddress::LocalHost, 0), "fake Sentry listener A must bind");
        const QString dsnA = QStringLiteral("http://publickey@127.0.0.1:%1/1").arg(serverA.serverPort());

        QTcpServer serverB;
        QStringList receivedB;
        wireFakeSentryListener(&serverB, &receivedB);
        QVERIFY2(serverB.listen(QHostAddress::LocalHost, 0), "fake Sentry listener B must bind");
        const QString dsnB = QStringLiteral("http://publickey@127.0.0.1:%1/1").arg(serverB.serverPort());

        QProcess child;
        child.setProgram(m_appPath);
        child.setArguments({ QStringLiteral("--rotation-child"), dbDir.path(), m_handlerPath, dsnA,
                            dsnB });
        child.start();
        QVERIFY2(child.waitForStarted(5000), "the rotation-child process must start");

        QTRY_VERIFY_WITH_TIMEOUT(anyStartsWith(receivedA, QStringLiteral("/api/1/minidump/")), 15000);

        child.waitForFinished(10000);
        QTRY_COMPARE_WITH_TIMEOUT(child.state(), QProcess::NotRunning, 10000);
        QCOMPARE(child.exitStatus(), QProcess::CrashExit);

        const QByteArray stdoutText = child.readAllStandardOutput();
        QVERIFY2(stdoutText.contains("INIT_COUNT=2 CLOSE_COUNT=1"),
                 qPrintable(QStringLiteral("expected exactly one re-init (INIT_COUNT=2 "
                                          "CLOSE_COUNT=1), got: %1")
                                .arg(QString::fromUtf8(stdoutText))));

        // dsnB was only ever cached - never adopted in-process - so its own listener sees nothing.
        QTest::qWait(2000);
        QVERIFY2(receivedB.isEmpty(),
                 "a rotation never re-inits mid-run - the second DSN's listener must see nothing");

        const std::optional<SeatHubTelemetry::Handout> cached = SeatHubTelemetry::readCache();
        QVERIFY(cached.has_value());
        QCOMPARE(cached->dsn, dsnB); // the cache always holds the LATEST handout, adopted or not.
    }

private:
    QTemporaryDir m_handlerDir;
    QString m_handlerPath;
    QString m_appPath;
};

int main(int argc, char* argv[])
{
    // Plan 15: `SeatHubTelemetry::cachePath()`/`TokenStore::defaultDirectory()` have no override -
    // production relies on them resolving the real per-user profile directory, so tests instead
    // redirect QStandardPaths itself, before anything in this process (either child role, or the
    // parent QTest role below) can call either one. `setOrganizationName`/`setApplicationName` are
    // static and work with no QCoreApplication instance - required here, because the crash-child
    // roles never construct one (matching the production call site, which runs before
    // QCoreApplication exists too) - and both names must match `app/main.cpp`'s real ones exactly,
    // so every role in this binary (and the parent test process) resolves the SAME sandboxed
    // directory.
    QCoreApplication::setOrganizationName(QStringLiteral("Seven Hills"));
    QCoreApplication::setApplicationName(QStringLiteral("SeatHub"));
    QStandardPaths::setTestModeEnabled(true);

    if (argc > 1 && std::strcmp(argv[1], "--crash-child") == 0) {
        return runCrashChild(argc, argv);
    }
    if (argc > 1 && std::strcmp(argv[1], "--start-child") == 0) {
        return runStartChild(argc, argv);
    }
    if (argc > 1 && std::strcmp(argv[1], "--handout-child") == 0) {
        return runHandoutChild(argc, argv);
    }
    if (argc > 1 && std::strcmp(argv[1], "--rotation-child") == 0) {
        return runRotationChild(argc, argv);
    }

    QCoreApplication app(argc, argv);
    TstTelemetry testCase;
    return QTest::qExec(&testCase, argc, argv);
}

#include "tst_telemetry.moc"
