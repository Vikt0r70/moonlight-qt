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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
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

// Plan 21: `telemetry.h` now includes `log_shipper.h`, which includes `log_tee.h`, which includes
// <SDL.h> - and <SDL.h> otherwise `#define`s `main` to `SDL_main` on Windows, which would rename
// this file's own `main()` (below) out from under the linker (the same fix `tst_log_tee.cpp` and
// `tst_log_shipper.cpp` use for the same reason - neither of THOSE files pulled SDL in through
// telemetry.h before this plan, so this file never needed the fix until now).
#define SDL_MAIN_HANDLED

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

/// `tst_telemetry --identity-reinit-child <db> <handler> <dsn> <environment>`: starts with no DSN,
/// sets every identity value (user/session/host/trace) BEFORE applying the handout - CR-01's exact
/// scenario (a customer's first Play on a fresh install, where setUser()/setSession()/setTrace()
/// all ran before the first accepted DSN handout triggers `adoptFirstDsn()`'s
/// `sentry_close()`/`sentry_init()`) - then captures a plain test message event (never a crash: a
/// crash's own minidump upload is gzipped/msgpack-encoded by crashpad's OUT-OF-PROCESS handler,
/// which this suite cannot decode; a plain `sentry_capture_event()` goes out through the SDK's own
/// envelope transport instead, uncompressed in this fork's pinned build, exactly like the log
/// envelope tests above).
int runIdentityReinitChild(int argc, char* argv[])
{
    SeatHubTelemetry::Options options;
    options.databaseDir = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString();
    options.handlerPath = argc > 3 ? QString::fromLocal8Bit(argv[3]) : QString();
    options.environment = QStringLiteral("test");
    options.release = QStringLiteral("tst_telemetry@0.0.0");
    options.dsn = QString(); // the scenario: this process starts with no DSN at all.
    SeatHubTelemetry::startWith(options);

    SeatHubTelemetry::setUser(QStringLiteral("acct-reinit-test"));
    SeatHubTelemetry::setSession(QStringLiteral("session-reinit-test"),
                                 QStringLiteral("host-reinit-test"));
    SeatHubTelemetry::setTrace(QStringLiteral("trace-reinit-test"));

    SeatHubTelemetry::Handout handout;
    handout.dsn = argc > 4 ? QString::fromLocal8Bit(argv[4]) : QString();
    handout.environment = argc > 5 ? QString::fromLocal8Bit(argv[5]) : QStringLiteral("test");
    handout.logs = true;
    SeatHubTelemetry::applyHandout(handout); // triggers the one re-init (adoptFirstDsn()).

    SeatHubTelemetry::captureTestMessageForTests(QStringLiteral("identity-reinit-marker"));

    // There is no sentry_flush() available outside telemetry.cpp (the one TU with sentry.h) - stay
    // alive long enough for the SDK's own background transport to send the envelope before exit.
    ::Sleep(3000);
    return 0;
}

/// `tst_telemetry --test-crash-child <db> <handler> <dsn>`: starts with `dsn` already in place
/// (Task 3 is about the switch, not the handout path Task 1 already covers), then calls
/// `SeatHubTelemetry::maybeTestCrash()` exactly once. `SEATHUB_TEST_CRASH` is read from this
/// process's own environment - the parent sets or removes it explicitly before starting this
/// child (`QProcess::setProcessEnvironment()`), so a developer's own shell can never make this
/// test flaky. Returns 0 when the switch does not crash this process (unset, empty, or any value
/// other than `1`/`fastfail`/`qfatal`).
int runTestCrashChild(int argc, char* argv[])
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);

    SeatHubTelemetry::Options options;
    options.databaseDir = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString();
    options.handlerPath = argc > 3 ? QString::fromLocal8Bit(argv[3]) : QString();
    options.dsn = argc > 4 ? QString::fromLocal8Bit(argv[4]) : QString();
    options.environment = QStringLiteral("test");
    options.release = QStringLiteral("tst_telemetry@0.0.0");
    SeatHubTelemetry::startWith(options);

    SeatHubTelemetry::maybeTestCrash();
    return 0; // reached whenever the switch does not crash this process
}

// --- Plan 21 (D-14): the real LogShipper hand-off, against a loopback DSN -------------------

/// `tst_telemetry --logs-child <dsn> <db> <handler> <spool-dir> <gate>`: wires the REAL
/// production hand-off (`SeatHubTelemetry::logShipperHandOff()`) onto a `LogShipper` this child
/// drives itself - `startWith()`, unlike `start()`, never touches `LogShipper`
/// (`logShipperHandOff()`'s own header comment), so this role does that wiring by hand, against a
/// temp spool directory the parent's `QTemporaryDir` owns. `gate` is `"signed-in"` (calls
/// `setUser()` before the handout, so `canShip` - `signedIn() && logsEnabled()` - goes true) or
/// `"signed-out"` (never signs in, so `canShip` stays false even though the handout's `dsn` and
/// `logs: true` are otherwise identical) - proving the SAME gating `SeatHubClient` relies on,
/// through the real `setUser()`/`applyHandout()` calls, not a direct `LogShipper::setCanShip()`
/// bypass. Stays alive past the logs batcher's own 5000ms flush interval
/// (`sentry_logs.c`'s `SENTRY_BATCHER_FLUSH_INTERVAL_MS`) only for the signed-in gate - there is
/// no `sentry_flush()` call available outside `telemetry.cpp`, the one translation unit that
/// includes `sentry.h`.
int runLogsChild(int argc, char* argv[])
{
    const SeatHubTelemetry::Options options = optionsFromChildArgs(argc, argv);
    const QString spoolDir = argc > 5 ? QString::fromLocal8Bit(argv[5]) : QString();
    const QString gate = argc > 6 ? QString::fromLocal8Bit(argv[6]) : QStringLiteral("signed-in");

    LogTee::install();
    LogShipper::instance().setSpoolDirectoryForTests(spoolDir);
    LogShipper::instance().start(SeatHubTelemetry::logShipperHandOff());
    SeatHubTelemetry::startWith(options);
    SeatHubTelemetry::setSession(QStringLiteral("session-logs-child"), QString());

    if (gate == QStringLiteral("signed-in")) {
        SeatHubTelemetry::setUser(QStringLiteral("acct-logs-child"));
    }
    // Either way: a DSN already in use (via `startWith()` above) and a `logs: true` handout - the
    // gate's only variable is whether `setUser()` ran.
    SeatHubTelemetry::Handout handout;
    handout.dsn = options.dsn;
    handout.environment = QStringLiteral("test");
    handout.logs = true;
    SeatHubTelemetry::applyHandout(handout);

    qInfo() << "logs-child-line-marker";

    LogShipper::instance().drainBeforeSignOut();
    if (gate == QStringLiteral("signed-in")) {
        ::Sleep(6000);
    }
    LogShipper::instance().stop();
    return 0;
}

// --- Plan 09 (ADR-0072): the diagnostic lane's own child role -----------------------------

/// `tst_telemetry --diag-child <scenario> <dsn> <db> <handler> <spool-dir> <run-state-dir>`:
/// one fresh process per scenario, so each assertion below is about a new SDK, a new shipper and
/// a new gate - the state the three kill switches and the launch record are actually about.
///
/// Scenarios: `play-step` (one structured `play.step`, with the PIN sentinel present ONLY in a
/// local `engineError`-shaped string the emission never receives), `step-metric` (one
/// `seathub.play.step_result` + `seathub.play.step_duration` while signed in with a session, a
/// host and a trace), `flush` (one line, then a timed `flush(2000)` reported on stdout),
/// `launch` (the `client.telemetry` launch record), and the three `kill-*` scenarios, which emit
/// a line AND a metric that must produce no envelope at all.
int runDiagChild(int argc, char* argv[])
{
    const QString scenario = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString();
    const QString dsn = argc > 3 ? QString::fromLocal8Bit(argv[3]) : QString();
    const QString dbDir = argc > 4 ? QString::fromLocal8Bit(argv[4]) : QString();
    const QString handler = argc > 5 ? QString::fromLocal8Bit(argv[5]) : QString();
    const QString spoolDir = argc > 6 ? QString::fromLocal8Bit(argv[6]) : QString();
    const QString runStateDir = argc > 7 ? QString::fromLocal8Bit(argv[7]) : QString();

    SeatHubTelemetry::Options options;
    options.databaseDir = dbDir;
    options.handlerPath = handler;
    options.environment = QStringLiteral("test");
    options.release = QStringLiteral("seathub@0.0.0-diag");
    // The `kill-no-dsn` scenario is the control plane's handout turning telemetry OFF: an empty
    // DSN from the start, which is what an off handout looks like to `startWith()`.
    options.dsn = scenario == QLatin1String("kill-no-dsn") ? QString() : dsn;

    LogTee::install();
    LogShipper::instance().setSpoolDirectoryForTests(spoolDir);
    LogShipper::instance().start(SeatHubTelemetry::logShipperHandOff());
    SeatHubTelemetry::setRunStateDirectoryForTests(runStateDir);
    SeatHubTelemetry::startWith(options);
    SeatHubTelemetry::setSession(QStringLiteral("session-diag-child"),
                                 QStringLiteral("host-diag-child"));

    SeatHubTelemetry::Handout handout;
    handout.dsn = options.dsn;
    handout.environment = QStringLiteral("test");
    handout.logs = scenario != QLatin1String("kill-logs-false");
    SeatHubTelemetry::applyHandout(handout);

    if (scenario != QLatin1String("kill-signed-out")) {
        SeatHubTelemetry::setUser(QStringLiteral("acct-diag-child"));
    }

    QJsonObject stepAttributes;
    stepAttributes.insert(QStringLiteral("step"), QStringLiteral("pair_handshake"));
    stepAttributes.insert(QStringLiteral("outcome"), QStringLiteral("failed"));
    stepAttributes.insert(QStringLiteral("failure_class"), QStringLiteral("pin_rejected"));
    stepAttributes.insert(QStringLiteral("attempt"), 1);

    if (scenario == QLatin1String("play-step")) {
        // ADR-0072 item 2: the local diagnostic a real failure would carry, PIN and all. It is
        // built HERE and handed to nothing - the emission below receives only vocabulary tokens
        // and numbers, which is exactly what the assertion on the other side proves.
        const QString engineError = QStringLiteral("the rig rejected the pairing PIN 4821");
        QJsonObject attributes = stepAttributes;
        attributes.insert(QStringLiteral("elapsed_ms"), 812);
        SeatHubTelemetry::emitDiagnostic(QStringLiteral("play.step"), LogLevel::Warning,
                                         attributes);
        if (engineError.isEmpty()) { // never true: keeps the sentinel in scope, not in the emission
            return 1;
        }
    }
    else if (scenario == QLatin1String("step-metric")) {
        SeatHubTelemetry::setTrace(QStringLiteral("trace-diag-child"));
        SeatHubTelemetry::emitStepMetric(QStringLiteral("pair_handshake"),
                                         QStringLiteral("failed"),
                                         QStringLiteral("pin_rejected"), 812.0);
    }
    else if (scenario == QLatin1String("flush")) {
        SeatHubTelemetry::emitDiagnostic(QStringLiteral("play.step"), LogLevel::Warning,
                                         stepAttributes);
    }
    else if (scenario == QLatin1String("launch")) {
        SeatHubTelemetry::noteLaunch();
    }
    else if (scenario.startsWith(QLatin1String("kill-"))) {
        // Both lanes at once: the kill switch has to reach the diagnostic line (the shipper's
        // own gate) and the metric (`before_send_metric`'s atomic gate) or one of them would
        // still report while telemetry is off.
        SeatHubTelemetry::emitDiagnostic(QStringLiteral("play.step"), LogLevel::Warning,
                                         stepAttributes);
        SeatHubTelemetry::emitStepMetric(QStringLiteral("pair_handshake"),
                                         QStringLiteral("failed"),
                                         QStringLiteral("pin_rejected"), 812.0);
    }

    LogShipper::instance().drainBeforeSignOut();
    const qint64 flushStartedAt = QDateTime::currentMSecsSinceEpoch();
    const bool flushed = SeatHubTelemetry::flush(2000);
    const qint64 flushElapsed = QDateTime::currentMSecsSinceEpoch() - flushStartedAt;
    if (scenario == QLatin1String("flush")) {
        // The parent reads this: the call must return inside its 2000 ms budget.
        fprintf(stdout, "FLUSH_MS=%lld FLUSH_OK=%d\n", static_cast<long long>(flushElapsed),
                flushed ? 1 : 0);
        fflush(stdout);
    }
    LogShipper::instance().stop();
    // A marker after the one call that can block on another thread, so a parent that reports a
    // child "still running" can tell where it stopped rather than guessing.
    fprintf(stdout, "STOPPED=1\n");
    fflush(stdout);
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

/// Wires `server` to answer every request `200 {}` only once the FULL body (per its own
/// `Content-Length` header) has arrived, and stores it in `*envelopeBody` -
/// `wireFakeSentryListener()` above answers as soon as the headers end, which would truncate an
/// envelope this suite actually needs to parse. Safe to parse directly, never gzipped: this
/// fork's pinned sentry-native build has `SENTRY_TRANSPORT_COMPRESSION:BOOL=OFF`
/// (`build/sentry-native-0.17.1/build/CMakeCache.txt`).
void wireLogEnvelopeListener(QTcpServer* server, QByteArray* envelopeBody)
{
    QObject::connect(server, &QTcpServer::newConnection, server, [server, envelopeBody]() {
        while (QTcpSocket* socket = server->nextPendingConnection()) {
            auto buffer = std::make_shared<QByteArray>();
            auto responded = std::make_shared<bool>(false);
            QObject::connect(socket, &QTcpSocket::readyRead, socket,
                              [socket, buffer, responded, envelopeBody]() {
                *buffer += socket->readAll();
                if (*responded) {
                    return;
                }
                const int headerEnd = buffer->indexOf("\r\n\r\n");
                if (headerEnd < 0) {
                    return;
                }
                qint64 contentLength = 0;
                for (const QByteArray& line : buffer->left(headerEnd).split('\n')) {
                    const QByteArray trimmed = line.trimmed();
                    if (trimmed.toLower().startsWith("content-length:")) {
                        contentLength = trimmed.mid(trimmed.indexOf(':') + 1).trimmed().toLongLong();
                        break;
                    }
                }
                const int bodyStart = headerEnd + 4;
                if (buffer->size() - bodyStart < contentLength) {
                    return; // more body still to arrive
                }
                *envelopeBody = buffer->mid(bodyStart, static_cast<int>(contentLength));
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

/// One "log" envelope item's first entry, per `sentry__envelope_add_logs()`
/// (`sentry_envelope.c`/`sentry_logs.c`): the envelope is `{envelope headers}\n{item
/// headers}\n{item payload}` (repeated per item, `SENTRY_TRANSPORT_COMPRESSION` off means never
/// gzipped in this build), the item header's own `length` field is exactly the payload's byte
/// count, and the payload itself is `{"items":[{...one log object per sentry_log() call batched
/// into this envelope...}]}`.
struct ParsedLogItem
{
    bool found = false;
    QString body;
    QJsonObject attributes;
};

ParsedLogItem parseFirstLogItem(const QByteArray& envelopeBody)
{
    ParsedLogItem result;
    int pos = envelopeBody.indexOf('\n');
    if (pos < 0) {
        return result;
    }
    pos += 1; // past the envelope's own header line

    while (pos < envelopeBody.size()) {
        const int headerLineEnd = envelopeBody.indexOf('\n', pos);
        if (headerLineEnd < 0) {
            break;
        }
        QJsonParseError error;
        const QJsonObject itemHeader =
            QJsonDocument::fromJson(envelopeBody.mid(pos, headerLineEnd - pos), &error).object();
        if (error.error != QJsonParseError::NoError) {
            break;
        }
        const qint64 length = itemHeader.value(QStringLiteral("length")).toVariant().toLongLong();
        const QString type = itemHeader.value(QStringLiteral("type")).toString();
        pos = headerLineEnd + 1;
        const QByteArray payload = envelopeBody.mid(pos, static_cast<int>(length));
        pos += static_cast<int>(length);
        if (pos < envelopeBody.size() && envelopeBody.at(pos) == '\n') {
            ++pos; // the next item's own leading newline
        }

        if (type == QStringLiteral("log")) {
            const QJsonObject logsObject = QJsonDocument::fromJson(payload).object();
            const QJsonArray items = logsObject.value(QStringLiteral("items")).toArray();
            if (!items.isEmpty()) {
                const QJsonObject firstLog = items.first().toObject();
                result.found = true;
                result.body = firstLog.value(QStringLiteral("body")).toString();
                result.attributes = firstLog.value(QStringLiteral("attributes")).toObject();
                return result;
            }
        }
    }
    return result;
}

/// Like `wireLogEnvelopeListener()` but keeps EVERY envelope instead of the last one written:
/// plan 09's runs send more than one (the launch record, the `play.step` and the metrics can
/// each arrive as their own request), and a test that has to prove "zero envelopes" or find one
/// item among several cannot do it through a single overwritten buffer. Same body-complete rule,
/// same `Connection: close` answer - one request per connection, so nothing is ever concatenated.
void wireEnvelopeCollector(QTcpServer* server, QList<QByteArray>* envelopes)
{
    QObject::connect(server, &QTcpServer::newConnection, server, [server, envelopes]() {
        while (QTcpSocket* socket = server->nextPendingConnection()) {
            auto buffer = std::make_shared<QByteArray>();
            auto responded = std::make_shared<bool>(false);
            QObject::connect(socket, &QTcpSocket::readyRead, socket,
                             [socket, buffer, responded, envelopes]() {
                                 *buffer += socket->readAll();
                                 if (*responded) {
                                     return;
                                 }
                                 const int headerEnd = buffer->indexOf("\r\n\r\n");
                                 if (headerEnd < 0) {
                                     return;
                                 }
                                 qint64 contentLength = 0;
                                 for (const QByteArray& line : buffer->left(headerEnd).split('\n')) {
                                     const QByteArray trimmed = line.trimmed();
                                     if (trimmed.toLower().startsWith("content-length:")) {
                                         contentLength = trimmed.mid(trimmed.indexOf(':') + 1)
                                                             .trimmed()
                                                             .toLongLong();
                                         break;
                                     }
                                 }
                                 const int bodyStart = headerEnd + 4;
                                 if (buffer->size() - bodyStart < contentLength) {
                                     return;
                                 }
                                 envelopes->append(
                                     buffer->mid(bodyStart, static_cast<int>(contentLength)));
                                 *responded = true;
                                 static const QByteArray body = QByteArrayLiteral("{}");
                                 QByteArray response
                                     = QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                                        "Content-Length: ")
                                     + QByteArray::number(body.size())
                                     + QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + body;
                                 socket->write(response);
                                 socket->flush();
                                 socket->disconnectFromHost();
                             });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
        }
    });
}

/// Every `log` item in one envelope - the same walk `parseFirstLogItem()` does, kept going past
/// the first item so a run whose envelope batches several lines can still be searched by body.
QList<ParsedLogItem> parseLogItems(const QByteArray& envelopeBody)
{
    QList<ParsedLogItem> items;
    int pos = envelopeBody.indexOf('\n');
    if (pos < 0) {
        return items;
    }
    pos += 1;
    while (pos < envelopeBody.size()) {
        const int headerLineEnd = envelopeBody.indexOf('\n', pos);
        if (headerLineEnd < 0) {
            break;
        }
        QJsonParseError error;
        const QJsonObject itemHeader =
            QJsonDocument::fromJson(envelopeBody.mid(pos, headerLineEnd - pos), &error).object();
        if (error.error != QJsonParseError::NoError) {
            break;
        }
        const qint64 length = itemHeader.value(QStringLiteral("length")).toVariant().toLongLong();
        const QString type = itemHeader.value(QStringLiteral("type")).toString();
        pos = headerLineEnd + 1;
        const QByteArray payload = envelopeBody.mid(pos, static_cast<int>(length));
        pos += static_cast<int>(length);
        if (pos < envelopeBody.size() && envelopeBody.at(pos) == '\n') {
            ++pos;
        }
        if (type != QLatin1String("log")) {
            continue;
        }
        const QJsonArray entries =
            QJsonDocument::fromJson(payload).object().value(QStringLiteral("items")).toArray();
        for (const QJsonValue& entry : entries) {
            const QJsonObject log = entry.toObject();
            ParsedLogItem parsed;
            parsed.found = true;
            parsed.body = log.value(QStringLiteral("body")).toString();
            parsed.attributes = log.value(QStringLiteral("attributes")).toObject();
            items.append(parsed);
        }
    }
    return items;
}

/// One `trace_metric` entry (ADR-0072 item 6): the item type is `trace_metric`, its payload is
/// the same `{items: [...]}` shape the log item uses (`sentry__envelope_add_metrics` ->
/// `add_telemetry()` in the pinned `sentry_envelope.c`), and each entry carries `name`, `type`,
/// `value`, its `attributes` object and - when the Play's trace is live - a top-level `trace_id`
/// (P-3: a field of the metric, never one of its attributes).
struct ParsedMetric
{
    QString name;
    QString type;
    QJsonObject attributes;
    QString traceId;
};

QList<ParsedMetric> parseMetricItems(const QByteArray& envelopeBody)
{
    QList<ParsedMetric> metrics;
    int pos = envelopeBody.indexOf('\n');
    if (pos < 0) {
        return metrics;
    }
    pos += 1;
    while (pos < envelopeBody.size()) {
        const int headerLineEnd = envelopeBody.indexOf('\n', pos);
        if (headerLineEnd < 0) {
            break;
        }
        QJsonParseError error;
        const QJsonObject itemHeader =
            QJsonDocument::fromJson(envelopeBody.mid(pos, headerLineEnd - pos), &error).object();
        if (error.error != QJsonParseError::NoError) {
            break;
        }
        const qint64 length = itemHeader.value(QStringLiteral("length")).toVariant().toLongLong();
        const QString type = itemHeader.value(QStringLiteral("type")).toString();
        pos = headerLineEnd + 1;
        const QByteArray payload = envelopeBody.mid(pos, static_cast<int>(length));
        pos += static_cast<int>(length);
        if (pos < envelopeBody.size() && envelopeBody.at(pos) == '\n') {
            ++pos;
        }
        if (type != QLatin1String("trace_metric")) {
            continue;
        }
        const QJsonArray entries =
            QJsonDocument::fromJson(payload).object().value(QStringLiteral("items")).toArray();
        for (const QJsonValue& entry : entries) {
            const QJsonObject metric = entry.toObject();
            ParsedMetric parsed;
            parsed.name = metric.value(QStringLiteral("name")).toString();
            parsed.type = metric.value(QStringLiteral("type")).toString();
            parsed.attributes = metric.value(QStringLiteral("attributes")).toObject();
            parsed.traceId = metric.value(QStringLiteral("trace_id")).toString();
            metrics.append(parsed);
        }
    }
    return metrics;
}

/// Everything a run sent, flattened across every captured envelope.
struct EnvelopeScan
{
    QList<ParsedLogItem> logs;
    QList<ParsedMetric> metrics;
};

EnvelopeScan scanEnvelopes(const QList<QByteArray>& envelopes)
{
    EnvelopeScan scan;
    for (const QByteArray& envelope : envelopes) {
        scan.logs.append(parseLogItems(envelope));
        scan.metrics.append(parseMetricItems(envelope));
    }
    return scan;
}

/// The first log whose body is exactly `body`, or a default-constructed (not found) item.
ParsedLogItem findLog(const EnvelopeScan& scan, const QString& body)
{
    for (const ParsedLogItem& item : scan.logs) {
        if (item.body == body) {
            return item;
        }
    }
    return ParsedLogItem();
}

/// The first metric with this name, or `nullptr`.
const ParsedMetric* findMetric(const EnvelopeScan& scan, const QString& name)
{
    for (const ParsedMetric& metric : scan.metrics) {
        if (metric.name == name) {
            return &metric;
        }
    }
    return nullptr;
}

/// Convenience predicates over the whole capture - a `QTRY_` on these also gives the event loop
/// the time it needs to read an envelope that the child sent just before it exited (the listener
/// only ever reads on the event loop, and `QProcess::waitForFinished()` blocks it).
bool hasLog(const QList<QByteArray>& envelopes, const QString& body)
{
    return findLog(scanEnvelopes(envelopes), body).found;
}

bool hasMetric(const QList<QByteArray>& envelopes, const QString& name)
{
    return findMetric(scanEnvelopes(envelopes), name) != nullptr;
}

/// One "event" envelope item (CR-01 regression: `captureTestMessageForTests()`'s plain message
/// event, never a crash). Same envelope shape `parseFirstLogItem()` above already parses - only
/// the item `type` and the payload's own top-level keys differ: a message event's body lives at
/// `message.formatted` (`sentry_value_new_message_event_n()`'s own shape, verified against the
/// vendored `sentry_value.c`), and the scope's live user/tags/trace are merged onto the event's
/// own `user`/`tags`/`contexts.trace.trace_id` keys (verified against the vendored
/// `sentry_scope.c`'s `sentry__scope_apply_to_event()`).
struct ParsedEventItem
{
    bool found = false;
    QString body;
    QJsonObject tags;
    QJsonObject user;
    QString traceId;
};

ParsedEventItem parseFirstEventItem(const QByteArray& envelopeBody)
{
    ParsedEventItem result;
    int pos = envelopeBody.indexOf('\n');
    if (pos < 0) {
        return result;
    }
    pos += 1; // past the envelope's own header line

    while (pos < envelopeBody.size()) {
        const int headerLineEnd = envelopeBody.indexOf('\n', pos);
        if (headerLineEnd < 0) {
            break;
        }
        QJsonParseError error;
        const QJsonObject itemHeader =
            QJsonDocument::fromJson(envelopeBody.mid(pos, headerLineEnd - pos), &error).object();
        if (error.error != QJsonParseError::NoError) {
            break;
        }
        const qint64 length = itemHeader.value(QStringLiteral("length")).toVariant().toLongLong();
        const QString type = itemHeader.value(QStringLiteral("type")).toString();
        pos = headerLineEnd + 1;
        const QByteArray payload = envelopeBody.mid(pos, static_cast<int>(length));
        pos += static_cast<int>(length);
        if (pos < envelopeBody.size() && envelopeBody.at(pos) == '\n') {
            ++pos; // the next item's own leading newline
        }

        if (type == QStringLiteral("event")) {
            const QJsonObject event = QJsonDocument::fromJson(payload).object();
            result.found = true;
            result.tags = event.value(QStringLiteral("tags")).toObject();
            result.user = event.value(QStringLiteral("user")).toObject();
            result.body = event.value(QStringLiteral("message")).toObject()
                              .value(QStringLiteral("formatted")).toString();
            result.traceId = event.value(QStringLiteral("contexts")).toObject()
                                  .value(QStringLiteral("trace")).toObject()
                                  .value(QStringLiteral("trace_id")).toString();
            return result;
        }
    }
    return result;
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

    // --- Plan 21 (D-14): the real LogShipper hand-off -----------------------------------------

    void signedInAndLogsTrueSendsAnEnvelopeCarryingTheLineWithSessionAndOriginalTime()
    {
        QTemporaryDir dbDir;
        QVERIFY(dbDir.isValid());
        QTemporaryDir spoolDir;
        QVERIFY(spoolDir.isValid());

        QTcpServer server;
        QList<QByteArray> envelopes;
        wireEnvelopeCollector(&server, &envelopes);
        QVERIFY2(server.listen(QHostAddress::LocalHost, 0),
                 "the fake Sentry listener must bind 127.0.0.1:0");
        const QString dsn = QStringLiteral("http://publickey@127.0.0.1:%1/1").arg(server.serverPort());

        const double beforeLog =
            static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;

        QProcess child;
        child.setProgram(m_appPath);
        child.setArguments({ QStringLiteral("--logs-child"), dsn, dbDir.path(), m_handlerPath,
                             spoolDir.path(), QStringLiteral("signed-in") });
        child.start();
        QVERIFY2(child.waitForStarted(5000), "the logs-child process must start");

        // Plan 09: this run also emits `seathub.client.launch`, whose `trace_metric` envelope can
        // arrive before (and in any case separately from) the log envelope - so wait for the
        // envelope that actually carries the marker line, across every envelope captured.
        QTRY_VERIFY_WITH_TIMEOUT(
            findLog(scanEnvelopes(envelopes), QStringLiteral("logs-child-line-marker")).found,
            15000);

        QVERIFY(child.waitForFinished(10000));
        QCOMPARE(child.exitCode(), 0);

        const ParsedLogItem item =
            findLog(scanEnvelopes(envelopes), QStringLiteral("logs-child-line-marker"));
        QVERIFY2(item.found, "no log item found in the captured envelope");
        QVERIFY2(item.body.contains(QStringLiteral("logs-child-line-marker")),
                 qPrintable(QStringLiteral("unexpected log body: %1").arg(item.body)));

        QVERIFY2(item.attributes.contains(QStringLiteral("session_id")),
                 "the log's attributes must carry session_id");
        QCOMPARE(item.attributes.value(QStringLiteral("session_id")).toObject()
                     .value(QStringLiteral("value")).toString(),
                 QStringLiteral("session-logs-child"));

        QVERIFY2(item.attributes.contains(QStringLiteral("seathub.logged_at")),
                 "the log's attributes must carry seathub.logged_at");
        const double loggedAt = item.attributes.value(QStringLiteral("seathub.logged_at"))
                                     .toObject().value(QStringLiteral("value")).toDouble();
        QVERIFY2(loggedAt >= beforeLog - 2.0 && loggedAt <= beforeLog + 10.0,
                 "seathub.logged_at was not close to when the line was actually logged");
    }

    void withNoSignInSetCanShipStaysFalseAndNothingReachesTheSdk()
    {
        QTemporaryDir dbDir;
        QVERIFY(dbDir.isValid());
        QTemporaryDir spoolDir;
        QVERIFY(spoolDir.isValid());

        QTcpServer server;
        QByteArray envelopeBody;
        wireLogEnvelopeListener(&server, &envelopeBody);
        QVERIFY2(server.listen(QHostAddress::LocalHost, 0),
                 "the fake Sentry listener must bind 127.0.0.1:0");
        const QString dsn = QStringLiteral("http://publickey@127.0.0.1:%1/1").arg(server.serverPort());

        QProcess child;
        child.setProgram(m_appPath);
        // Same DSN, same `logs: true` handout as the signed-in test above - the only difference
        // is this role never calls `setUser()` (SeatHubTelemetry::signedIn() stays false), which
        // must be enough on its own to keep `canShip` - and therefore every hand-off - false.
        child.setArguments({ QStringLiteral("--logs-child"), dsn, dbDir.path(), m_handlerPath,
                             spoolDir.path(), QStringLiteral("signed-out") });
        child.start();
        QVERIFY2(child.waitForStarted(5000), "the logs-child process must start");
        QVERIFY(child.waitForFinished(10000));
        QCOMPARE(child.exitCode(), 0);

        QVERIFY2(envelopeBody.isEmpty(),
                 "a line reached the SDK despite no sign-in - canShip must have been true");
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

    // --- CR-01 regression: identity survives adoptFirstDsn()'s one re-init ----------------------

    void identitySurvivesTheOneFirstDsnReInit()
    {
        QTemporaryDir dbDir;
        QVERIFY(dbDir.isValid());

        QTcpServer server;
        QByteArray envelopeBody;
        wireLogEnvelopeListener(&server, &envelopeBody);
        QVERIFY2(server.listen(QHostAddress::LocalHost, 0),
                 "the fake Sentry listener must bind 127.0.0.1:0");
        const QString dsn = QStringLiteral("http://publickey@127.0.0.1:%1/1").arg(server.serverPort());

        QProcess child;
        child.setProgram(m_appPath);
        child.setArguments({ QStringLiteral("--identity-reinit-child"), dbDir.path(), m_handlerPath,
                             dsn, QStringLiteral("test") });
        child.start();
        QVERIFY2(child.waitForStarted(5000), "the identity-reinit-child process must start");

        QTRY_VERIFY_WITH_TIMEOUT(!envelopeBody.isEmpty(), 15000);

        QVERIFY(child.waitForFinished(10000));
        QCOMPARE(child.exitCode(), 0);

        const ParsedEventItem item = parseFirstEventItem(envelopeBody);
        QVERIFY2(item.found, "no event item found in the captured envelope");
        QVERIFY2(item.body.contains(QStringLiteral("identity-reinit-marker")),
                 qPrintable(QStringLiteral("unexpected event body: %1").arg(item.body)));

        // CR-01: `adoptFirstDsn()`'s `sentry_close()`/`sentry_init()` wipes the whole scope.
        // Without the fix, NONE of these survive the re-init `applyHandout()` just performed -
        // even though the C++-side statics (setUser()/setSession()/setTrace() all ran BEFORE the
        // handout, exactly the "first Play on a fresh install" scenario) still report the right
        // values (the review's own point: the bug is invisible from the C++ side alone).
        QVERIFY2(item.user.contains(QStringLiteral("id")), "the event must carry a user id");
        QCOMPARE(item.user.value(QStringLiteral("id")).toString(), QStringLiteral("acct-reinit-test"));

        QCOMPARE(item.tags.value(QStringLiteral("signed_in")).toString(), QStringLiteral("true"));
        QCOMPARE(item.tags.value(QStringLiteral("session_id")).toString(),
                 QStringLiteral("session-reinit-test"));
        QCOMPARE(item.tags.value(QStringLiteral("host_id")).toString(),
                 QStringLiteral("host-reinit-test"));

        QCOMPARE(item.traceId, QStringLiteral("trace-reinit-test"));
    }

    // --- Task 3: the test crash switch -----------------------------------------------------------

    void testCrashChildCrashesWithSeatHubTestCrash1AndUploadsToTheListener()
    {
        QTemporaryDir dbDir;
        QVERIFY(dbDir.isValid());

        QTcpServer server;
        QStringList receivedPaths;
        wireFakeSentryListener(&server, &receivedPaths);
        QVERIFY2(server.listen(QHostAddress::LocalHost, 0),
                 "the fake Sentry listener must bind 127.0.0.1:0");
        const QString dsn = QStringLiteral("http://publickey@127.0.0.1:%1/1").arg(server.serverPort());

        QProcess child;
        child.setProgram(m_appPath);
        child.setArguments({ QStringLiteral("--test-crash-child"), dbDir.path(), m_handlerPath, dsn });
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("SEATHUB_TEST_CRASH"), QStringLiteral("1"));
        child.setProcessEnvironment(env);
        child.start();
        QVERIFY2(child.waitForStarted(5000), "the test-crash-child process must start");

        QTRY_VERIFY_WITH_TIMEOUT(anyStartsWith(receivedPaths, QStringLiteral("/api/1/minidump/")), 15000);

        child.waitForFinished(10000);
        QTRY_COMPARE_WITH_TIMEOUT(child.state(), QProcess::NotRunning, 10000);
        QCOMPARE(child.exitStatus(), QProcess::CrashExit);

        QTest::qWait(1500);
    }

    void testCrashChildExitsZeroWhenTheSwitchIsUnsetEmptyOrUnknown()
    {
        struct Case
        {
            QString label;
            bool setValue;
            QString value;
        };
        const QList<Case> cases = {
            { QStringLiteral("unset"), false, QString() },
            { QStringLiteral("empty"), true, QString() },
            { QStringLiteral("unknown"), true, QStringLiteral("banana") },
        };

        for (const Case& testCase : cases) {
            QTemporaryDir dbDir;
            QVERIFY(dbDir.isValid());

            QProcess child;
            child.setProgram(m_appPath);
            child.setArguments(
                { QStringLiteral("--test-crash-child"), dbDir.path(), m_handlerPath, QString() });
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            if (testCase.setValue) {
                env.insert(QStringLiteral("SEATHUB_TEST_CRASH"), testCase.value);
            }
            else {
                env.remove(QStringLiteral("SEATHUB_TEST_CRASH"));
            }
            child.setProcessEnvironment(env);
            child.start();
            QVERIFY2(child.waitForStarted(5000),
                     qPrintable(QStringLiteral("the %1 case must start").arg(testCase.label)));
            QVERIFY2(child.waitForFinished(10000),
                     qPrintable(QStringLiteral("the %1 case must exit on its own").arg(testCase.label)));
            QCOMPARE(child.exitStatus(), QProcess::NormalExit);
            QCOMPARE(child.exitCode(), 0);
        }
    }

    void maybeTestCrashReadsTheSwitchOnlyOnceInThisProcess()
    {
        // Primed with a value that never crashes, so the FIRST call is the one that consumes the
        // one-shot check - exactly like a real process reading its own real environment once.
        qputenv("SEATHUB_TEST_CRASH", QByteArrayLiteral("banana"));
        SeatHubTelemetry::maybeTestCrash();

        // A second call, even with the variable now changed to a value that DOES crash, must be a
        // no-op: the switch was already read once in this process. Reaching the QVERIFY below at
        // all is the proof - "1" would otherwise have crashed this very test process.
        qputenv("SEATHUB_TEST_CRASH", QByteArrayLiteral("1"));
        SeatHubTelemetry::maybeTestCrash();

        QVERIFY(true);

        qunsetenv("SEATHUB_TEST_CRASH");
    }

    // --- Plan 09 (ADR-0072): the diagnostic lane end to end ---------------------------------

    void metricsCarryNoSessionHostOrUserIdAndKeepTheTraceId()
    {
        QTemporaryDir dbDir;
        QVERIFY(dbDir.isValid());
        QTemporaryDir spoolDir;
        QVERIFY(spoolDir.isValid());
        QTemporaryDir runStateDir;
        QVERIFY(runStateDir.isValid());

        QTcpServer server;
        QList<QByteArray> envelopes;
        wireEnvelopeCollector(&server, &envelopes);
        QVERIFY2(server.listen(QHostAddress::LocalHost, 0),
                 "the fake Sentry listener must bind 127.0.0.1:0");
        const QString dsn =
            QStringLiteral("http://publickey@127.0.0.1:%1/1").arg(server.serverPort());

        QProcess child;
        child.setProgram(m_appPath);
        child.setArguments({ QStringLiteral("--diag-child"), QStringLiteral("step-metric"), dsn,
                             dbDir.path(), m_handlerPath, spoolDir.path(), runStateDir.path() });
        child.start();
        QVERIFY2(child.waitForStarted(5000), "the diag-child process must start");
        QTRY_VERIFY_WITH_TIMEOUT(!envelopes.isEmpty(), 20000);
        // QTRY, never `waitForFinished()`: the latter blocks this process's event loop, and the
        // fake Sentry listener only ever reads ON that loop. A child waiting for its envelope to
        // be acknowledged would then be waiting for a parent that is waiting for it - the two
        // deadlocking until the timeout below. `state()` flips from the process's own notifier,
        // which the same event loop delivers, so this wait also lets every envelope in.
        QTRY_VERIFY_WITH_TIMEOUT(child.state() != QProcess::Running, 20000);
        QCOMPARE(child.exitCode(), 0);

        // The listener only reads on the event loop, which `waitForFinished()` just blocked -
        // give an envelope the child sent on its way out its turn to be read.
        QTRY_VERIFY_WITH_TIMEOUT(
            hasMetric(envelopes, QStringLiteral("seathub.play.step_result")), 10000);

        const EnvelopeScan scan = scanEnvelopes(envelopes);
        const ParsedMetric* result =
            findMetric(scan, QStringLiteral("seathub.play.step_result"));
        QVERIFY2(result != nullptr, "no seathub.play.step_result metric reached the listener");

        // Exactly `step`, `outcome`, `failure_class` plus the SDK's own `sentry.*` / `os.*`
        // (ADR-0072 item 6) - and above all none of the per-session identities the scope attaches
        // to every metric (item 7, T-06.7-36).
        const QStringList attributeKeys = result->attributes.keys();
        QVERIFY2(!attributeKeys.isEmpty(), "the metric carries no attributes at all");
        for (const QString& key : attributeKeys) {
            const bool named = key == QLatin1String("step") || key == QLatin1String("outcome")
                || key == QLatin1String("failure_class");
            const bool sdk = key.startsWith(QLatin1String("sentry."))
                || key.startsWith(QLatin1String("os."));
            QVERIFY2(named || sdk, qPrintable(QStringLiteral("metric attribute outside the allow-list: %1").arg(key)));
        }
        QVERIFY(result->attributes.contains(QStringLiteral("step")));
        QVERIFY(result->attributes.contains(QStringLiteral("outcome")));
        QVERIFY(result->attributes.contains(QStringLiteral("failure_class")));
        QVERIFY2(!result->attributes.contains(QStringLiteral("session_id")),
                 "session_id must never be a metric dimension");
        QVERIFY2(!result->attributes.contains(QStringLiteral("host_id")),
                 "host_id must never be a metric dimension");
        QVERIFY2(!result->attributes.contains(QStringLiteral("user.id")),
                 "user.id must never be a metric dimension");
        QVERIFY2(!result->traceId.isEmpty(),
                 "the top-level trace_id field is what links a fleet metric to its Play (P-3)");

        const ParsedMetric* duration =
            findMetric(scan, QStringLiteral("seathub.play.step_duration"));
        QVERIFY2(duration != nullptr, "no seathub.play.step_duration metric reached the listener");
        QCOMPARE(duration->type, QStringLiteral("distribution"));
        QVERIFY(duration->attributes.contains(QStringLiteral("step")));
        QVERIFY(duration->attributes.contains(QStringLiteral("outcome")));
        QVERIFY2(!duration->attributes.contains(QStringLiteral("failure_class")),
                 "the duration distribution is dimensioned by step and outcome only");
    }

    void theKillSwitchesAlsoStopMetricsAndDiagnosticLines()
    {
        const QStringList scenarios = { QStringLiteral("kill-no-dsn"),
                                         QStringLiteral("kill-logs-false"),
                                         QStringLiteral("kill-signed-out") };
        for (const QString& scenario : scenarios) {
            QTemporaryDir dbDir;
            QVERIFY(dbDir.isValid());
            QTemporaryDir spoolDir;
            QVERIFY(spoolDir.isValid());
            QTemporaryDir runStateDir;
            QVERIFY(runStateDir.isValid());

            QTcpServer server;
            QList<QByteArray> envelopes;
            wireEnvelopeCollector(&server, &envelopes);
            QVERIFY2(server.listen(QHostAddress::LocalHost, 0),
                     "the fake Sentry listener must bind 127.0.0.1:0");
            const QString dsn =
                QStringLiteral("http://publickey@127.0.0.1:%1/1").arg(server.serverPort());

            QProcess child;
            child.setProgram(m_appPath);
            child.setArguments({ QStringLiteral("--diag-child"), scenario, dsn, dbDir.path(),
                                 m_handlerPath, spoolDir.path(), runStateDir.path() });
            child.start();
            QVERIFY2(child.waitForStarted(5000), qPrintable(scenario + " must start"));
            // Event-loop wait for the same reason as everywhere else in this file: the listener
            // only reads on the loop (and these children send nothing to read, but a blocked loop
            // would still be the wrong way to wait for one).
            QTRY_VERIFY_WITH_TIMEOUT(child.state() != QProcess::Running, 30000);
            QCOMPARE(child.exitCode(), 0);

            // Anything the SDK was still going to send would be on the wire by the time the
            // child exits (its own `flush(2000)` runs before it does); a moment longer only
            // makes the negative claim harder to satisfy, never easier.
            QTest::qWait(1500);
            QVERIFY2(envelopes.isEmpty(),
                     qPrintable(QStringLiteral("%1 still shipped %2 envelope(s)")
                                    .arg(scenario)
                                    .arg(envelopes.size())));
        }
    }

    void flushDeliversTheLastLineWithinBudget()
    {
        QTemporaryDir dbDir;
        QVERIFY(dbDir.isValid());
        QTemporaryDir spoolDir;
        QVERIFY(spoolDir.isValid());
        QTemporaryDir runStateDir;
        QVERIFY(runStateDir.isValid());

        QTcpServer server;
        QList<QByteArray> envelopes;
        wireEnvelopeCollector(&server, &envelopes);
        QVERIFY2(server.listen(QHostAddress::LocalHost, 0),
                 "the fake Sentry listener must bind 127.0.0.1:0");
        const QString dsn =
            QStringLiteral("http://publickey@127.0.0.1:%1/1").arg(server.serverPort());

        QProcess child;
        child.setProgram(m_appPath);
        child.setArguments({ QStringLiteral("--diag-child"), QStringLiteral("flush"), dsn,
                             dbDir.path(), m_handlerPath, spoolDir.path(), runStateDir.path() });
        child.start();
        QVERIFY2(child.waitForStarted(5000), "the diag-child process must start");

        // The child exits immediately after its own `flush(2000)`, so an envelope can only be
        // here if the flush put it on the wire - the SDK's own 5 s batcher interval would fire
        // long after the process is gone (ADR-0072 item 9, V31).
        QTRY_VERIFY_WITH_TIMEOUT(!envelopes.isEmpty(), 20000);
        // QTRY, never `waitForFinished()`: the latter blocks this process's event loop, and the
        // fake Sentry listener only ever reads ON that loop. A child waiting for its envelope to
        // be acknowledged would then be waiting for a parent that is waiting for it - the two
        // deadlocking until the timeout below. `state()` flips from the process's own notifier,
        // which the same event loop delivers, so this wait also lets every envelope in.
        QTRY_VERIFY_WITH_TIMEOUT(child.state() != QProcess::Running, 20000);
        QCOMPARE(child.exitCode(), 0);

        const QString report = QString::fromUtf8(child.readAllStandardOutput());
        const QRegularExpressionMatch match =
            QRegularExpression(QStringLiteral("FLUSH_MS=(\\d+)")).match(report);
        QVERIFY2(match.hasMatch(), qPrintable(QStringLiteral("no flush timing in: %1").arg(report)));
        const qint64 flushMs = match.captured(1).toLongLong();
        QVERIFY2(flushMs <= 2500,
                 qPrintable(QStringLiteral("flush(2000) took %1 ms - outside its budget").arg(flushMs)));

        QTRY_VERIFY_WITH_TIMEOUT(hasLog(envelopes, QStringLiteral("play.step")), 10000);

        const EnvelopeScan scan = scanEnvelopes(envelopes);
        const ParsedLogItem item = findLog(scan, QStringLiteral("play.step"));
        QVERIFY2(item.found, "the line emitted before the flush never reached the listener");
    }

    void theLaunchRecordCarriesTheDeliveryHealthAttributes()
    {
        QTemporaryDir dbDir;
        QVERIFY(dbDir.isValid());
        QTemporaryDir spoolDir;
        QVERIFY(spoolDir.isValid());
        QTemporaryDir runStateDir;
        QVERIFY(runStateDir.isValid());

        QTcpServer server;
        QList<QByteArray> envelopes;
        wireEnvelopeCollector(&server, &envelopes);
        QVERIFY2(server.listen(QHostAddress::LocalHost, 0),
                 "the fake Sentry listener must bind 127.0.0.1:0");
        const QString dsn =
            QStringLiteral("http://publickey@127.0.0.1:%1/1").arg(server.serverPort());

        QProcess child;
        child.setProgram(m_appPath);
        child.setArguments({ QStringLiteral("--diag-child"), QStringLiteral("launch"), dsn,
                             dbDir.path(), m_handlerPath, spoolDir.path(), runStateDir.path() });
        child.start();
        QVERIFY2(child.waitForStarted(5000), "the diag-child process must start");
        QTRY_VERIFY_WITH_TIMEOUT(!envelopes.isEmpty(), 20000);
        // QTRY, never `waitForFinished()`: the latter blocks this process's event loop, and the
        // fake Sentry listener only ever reads ON that loop. A child waiting for its envelope to
        // be acknowledged would then be waiting for a parent that is waiting for it - the two
        // deadlocking until the timeout below. `state()` flips from the process's own notifier,
        // which the same event loop delivers, so this wait also lets every envelope in.
        QTRY_VERIFY_WITH_TIMEOUT(child.state() != QProcess::Running, 20000);
        QCOMPARE(child.exitCode(), 0);

        QTRY_VERIFY_WITH_TIMEOUT(hasLog(envelopes, QStringLiteral("client.telemetry")), 10000);

        const EnvelopeScan scan = scanEnvelopes(envelopes);
        const ParsedLogItem item = findLog(scan, QStringLiteral("client.telemetry"));
        QVERIFY2(item.found, "the launch record never reached the listener");

        int records = 0;
        for (const ParsedLogItem& log : scan.logs) {
            if (log.body == QLatin1String("client.telemetry")) {
                ++records;
            }
        }
        QVERIFY2(records == 1, qPrintable(QStringLiteral("expected one launch record, found %1").arg(records)));

        const QStringList expected = {
            QStringLiteral("dsn_source"),           QStringLiteral("logs_enabled"),
            QStringLiteral("signed_in"),            QStringLiteral("crashed_last_run"),
            QStringLiteral("prev_exit"),            QStringLiteral("spool_lines_adopted"),
            QStringLiteral("retry_files"),          QStringLiteral("dropped_queue"),
            QStringLiteral("dropped_spool_age"),    QStringLiteral("cap_reached"),
        };
        for (const QString& key : expected) {
            QVERIFY2(item.attributes.contains(key),
                     qPrintable(QStringLiteral("the launch record is missing %1").arg(key)));
        }

        const auto value = [&item](const QString& key) {
            return item.attributes.value(key).toObject().value(QStringLiteral("value"));
        };
        const QString dsnSource = value(QStringLiteral("dsn_source")).toString();
        QVERIFY2(dsnSource == QLatin1String("cache") || dsnSource == QLatin1String("handout")
                     || dsnSource == QLatin1String("none"),
                 qPrintable(QStringLiteral("dsn_source is not one of the three: %1").arg(dsnSource)));
        QCOMPARE(value(QStringLiteral("prev_exit")).toString(), QStringLiteral("clean"));
        QVERIFY(value(QStringLiteral("logs_enabled")).toBool());
        QVERIFY(value(QStringLiteral("signed_in")).toBool());
        QVERIFY(!value(QStringLiteral("crashed_last_run")).toBool());
        QVERIFY(value(QStringLiteral("spool_lines_adopted")).toInt() >= 0);
        QVERIFY(value(QStringLiteral("retry_files")).toInt() >= 0);
        QVERIFY(value(QStringLiteral("dropped_queue")).toInt() >= 0);
        QVERIFY(value(QStringLiteral("dropped_spool_age")).toInt() >= 0);
        QVERIFY(value(QStringLiteral("cap_reached")).toInt() >= 0);
    }

    void thePlayStepCarriesOnlyAllowListedAttributesAndNeverThePin()
    {
        QTemporaryDir dbDir;
        QVERIFY(dbDir.isValid());
        QTemporaryDir spoolDir;
        QVERIFY(spoolDir.isValid());
        QTemporaryDir runStateDir;
        QVERIFY(runStateDir.isValid());

        QTcpServer server;
        QList<QByteArray> envelopes;
        wireEnvelopeCollector(&server, &envelopes);
        QVERIFY2(server.listen(QHostAddress::LocalHost, 0),
                 "the fake Sentry listener must bind 127.0.0.1:0");
        const QString dsn =
            QStringLiteral("http://publickey@127.0.0.1:%1/1").arg(server.serverPort());

        QProcess child;
        child.setProgram(m_appPath);
        child.setArguments({ QStringLiteral("--diag-child"), QStringLiteral("play-step"), dsn,
                             dbDir.path(), m_handlerPath, spoolDir.path(), runStateDir.path() });
        child.start();
        QVERIFY2(child.waitForStarted(5000), "the diag-child process must start");
        QTRY_VERIFY_WITH_TIMEOUT(!envelopes.isEmpty(), 20000);
        // QTRY, never `waitForFinished()`: the latter blocks this process's event loop, and the
        // fake Sentry listener only ever reads ON that loop. A child waiting for its envelope to
        // be acknowledged would then be waiting for a parent that is waiting for it - the two
        // deadlocking until the timeout below. `state()` flips from the process's own notifier,
        // which the same event loop delivers, so this wait also lets every envelope in.
        QTRY_VERIFY_WITH_TIMEOUT(child.state() != QProcess::Running, 20000);
        QCOMPARE(child.exitCode(), 0);

        QTRY_VERIFY_WITH_TIMEOUT(hasLog(envelopes, QStringLiteral("play.step")), 10000);

        const EnvelopeScan scan = scanEnvelopes(envelopes);
        const ParsedLogItem item = findLog(scan, QStringLiteral("play.step"));
        QVERIFY2(item.found, "the one failed pair_handshake step never reached the listener");

        int steps = 0;
        for (const ParsedLogItem& log : scan.logs) {
            if (log.body == QLatin1String("play.step")) {
                ++steps;
            }
        }
        QVERIFY2(steps == 1, qPrintable(QStringLiteral("expected one play.step, found %1").arg(steps)));

        // Every key within the ADR-0072 allow-list (item 1/7): SeatHub's own, the keys the
        // shipper and the SDK attach, and nothing else.
        const QSet<QString> allowed = {
            QStringLiteral("step"),           QStringLiteral("outcome"),
            QStringLiteral("failure_class"),  QStringLiteral("http_status"),
            QStringLiteral("net_error"),      QStringLiteral("engine_error"),
            QStringLiteral("engine_stage"),   QStringLiteral("failing_ports"),
            QStringLiteral("elapsed_ms"),     QStringLiteral("attempt"),
            QStringLiteral("end_reason"),     QStringLiteral("exit_path"),
            QStringLiteral("last_step"),      QStringLiteral("reason"),
            QStringLiteral("session_id"),     QStringLiteral("host_id"),
            QStringLiteral("seathub.trace_id"), QStringLiteral("seathub.logged_at"),
            QStringLiteral("user.id"),
        };
        for (const QString& key : item.attributes.keys()) {
            QVERIFY2(allowed.contains(key) || key.startsWith(QLatin1String("sentry."))
                         || key.startsWith(QLatin1String("os.")),
                     qPrintable(QStringLiteral("play.step attribute outside the allow-list: %1").arg(key)));
        }
        QCOMPARE(item.attributes.value(QStringLiteral("step")).toObject()
                     .value(QStringLiteral("value")).toString(),
                 QStringLiteral("pair_handshake"));
        QCOMPARE(item.attributes.value(QStringLiteral("failure_class")).toObject()
                     .value(QStringLiteral("value")).toString(),
                 QStringLiteral("pin_rejected"));

        // The child built an `engineError` holding the PIN sentinel `4821`; the emission never
        // receives it, so not one byte of it may appear anywhere in what was sent
        // (ADR-0072 item 2, T-06.7-35). The wider hostile fixture over every emitter is plan 22.
        for (const QByteArray& envelope : envelopes) {
            QVERIFY2(!envelope.contains("4821"),
                     "the PIN sentinel reached the envelope bytes");
        }
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
    if (argc > 1 && std::strcmp(argv[1], "--identity-reinit-child") == 0) {
        return runIdentityReinitChild(argc, argv);
    }
    if (argc > 1 && std::strcmp(argv[1], "--test-crash-child") == 0) {
        return runTestCrashChild(argc, argv);
    }
    if (argc > 1 && std::strcmp(argv[1], "--logs-child") == 0) {
        return runLogsChild(argc, argv);
    }
    if (argc > 1 && std::strcmp(argv[1], "--diag-child") == 0) {
        return runDiagChild(argc, argv);
    }

    QCoreApplication app(argc, argv);
    TstTelemetry testCase;
    return QTest::qExec(&testCase, argc, argv);
}

#include "tst_telemetry.moc"
