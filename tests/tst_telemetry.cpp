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
#include <QProcess>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <cstring>
#include <memory>

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

private:
    QTemporaryDir m_handlerDir;
    QString m_handlerPath;
    QString m_appPath;
};

int main(int argc, char* argv[])
{
    if (argc > 1 && std::strcmp(argv[1], "--crash-child") == 0) {
        return runCrashChild(argc, argv);
    }
    if (argc > 1 && std::strcmp(argv[1], "--start-child") == 0) {
        return runStartChild(argc, argv);
    }

    QCoreApplication app(argc, argv);
    TstTelemetry testCase;
    return QTest::qExec(&testCase, argc, argv);
}

#include "tst_telemetry.moc"
