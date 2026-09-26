/*****************************************************************************
 * SeatHub fork - LogTee's re-entry contract (06.3.1 D-16, area 1 of the design-review trigger,
 * SEATHUB verdict F).
 *
 * WR-07, WR-10, WR-12, IN-11 and IN-12 (`06.3-REVIEW-FIXDIFF2-fork.md`) drew a review finding on
 * the same spot three rounds running, so per the root `CLAUDE.md` design-review rule the
 * contract changes once instead of a fourth patch: an `addSink()`/`removeSink()` call made from
 * inside a sink's own dispatch, on the thread dispatching it, is no longer deferred and applied
 * later (WR-10's fix) - it is a programming error, refused loudly. `Q_ASSERT_X` fires in a debug
 * build; in a release build (every suite in this tree, `CONFIG -= debug`) one reason line is
 * written through whatever handler was installed before `LogTee::install()` ran and to stderr,
 * then `std::abort()` ends the process. The in-dispatch flag is reset by an RAII guard
 * (`InDispatchGuard`, `log_tee.cpp`), and every sink call is wrapped in `try { } catch (...) { }`
 * so a throwing sink can never jam the tee or cross SDL's C callback (IN-11). WR-07's own
 * guarantee - "once `removeSink()` returns, the sink is not running and will not run, including
 * on another thread" - is unchanged and still covered here.
 *
 * This process runs in two roles, chosen by its own first argument:
 *   - `tst_log_tee --reentry-child add|remove`: installs the tee, registers a sink that calls
 *     `LogTee::addSink()` (mode `add`) or `LogTee::removeSink()` on itself (mode `remove`) the
 *     moment it sees the trigger line, then logs that trigger line. The illegal in-dispatch call
 *     aborts the process with a reason line on stderr; the parent's `QProcess` observes exit
 *     code 3 and reads the line back.
 *   - no matching argument: the normal QTest run (the RAII/no-throw test, the ordinary-dispatch
 *     test, WR-07's kept cross-thread test, and the two death tests that drive the role above).
 *
 * `_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT)` and
 * `SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS)` (both set in the reentry-child
 * role only, before `LogTee::install()` runs) are what make `std::abort()` exit with code 3 and
 * raise no WER dialog - the same disclosure `06.3.1-RESEARCH-SPIKE-CRASHPAD.md` Appendix A
 * records for the CRT's own default `abort()` path. This suite has no crashpad handler installed
 * at all (unlike `tst_telemetry.cpp`'s crash-child roles), so it is the CRT's plain behaviour
 * under test here, not crashpad's `SIGABRT` catch: with no signal(SIGABRT, ...) handler and
 * `_CALL_REPORTFAULT` cleared, `abort()` calls `_exit(3)` directly instead of raising the fault
 * exception `0x40000015` a crashpad-installed process would show instead (SPIKE T10/T10b).
 *****************************************************************************/

#include <QtTest>

#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>
#include <QScopedPointer>
#include <QSemaphore>
#include <QString>
#include <QStringList>
#include <QThread>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

// This file defines `main()` below. `<SDL.h>` otherwise `#define`s `main` to `SDL_main` (its own
// cross-platform entry-point redirection), which would rename the `main()` below out from under
// the linker - the same fix `tst_stream_stats.cpp`, `tst_overlay_injection.cpp` and
// `tst_hud_bitmap.cpp` already use for the same reason.
#define SDL_MAIN_HANDLED
#include <SDL.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "seathub/log_tee.h"

namespace {

const char* const kReentryTrigger = "log-tee-reentry-child-trigger";

// --- the reentry-child role ------------------------------------------------------------------

int runReentryChild(const QString& mode)
{
    // Neither flag changes what LogTee's own release-build refusal path does - it always calls
    // `std::abort()` directly, with no CRT hook of its own. They only stop the CRT's OWN default
    // reaction to that `abort()` from doing anything this test would otherwise have to work
    // around: `SEM_NOGPFAULTERRORBOX`/`SEM_FAILCRITICALERRORS` keep Windows' own "stopped
    // working" dialog off, and clearing `_WRITE_ABORT_MSG`/`_CALL_REPORTFAULT` keeps the CRT from
    // invoking Windows Error Reporting itself before the process exits - which is what turns the
    // exit code from the fault code `0x40000015` into the plain `_exit(3)` this suite checks for.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);

    LogTee::install();

    LogTee::SinkHandle handle = 0;
    handle = LogTee::addSink([&](LogLevel, int, int, const QString& text) {
        if (!text.contains(QLatin1String(kReentryTrigger))) {
            return;
        }
        if (mode == QStringLiteral("add")) {
            // The scenario itself: adding another sink from inside this sink's own dispatch, on
            // the thread dispatching it. Must abort, never return.
            LogTee::addSink([](LogLevel, int, int, const QString&) {});
        }
        else {
            // The scenario itself: removing this very sink, from inside its own dispatch, on the
            // thread dispatching it. Must abort, never return.
            LogTee::removeSink(handle);
        }
    });

    qWarning() << kReentryTrigger; // must abort before this returns
    return 0; // unreachable
}

} // namespace

class TstLogTee : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        m_appPath = QCoreApplication::applicationFilePath();
        QVERIFY(QFileInfo(m_appPath).isFile());

        // The parent test process needs the tee actually wired to Qt's message handler and
        // SDL's log output function before any test below can dispatch through it - the
        // reentry-child role installs it for itself (see runReentryChild()), but this process
        // never runs that role.
        LogTee::install();
    }

    void init() { LogTee::clearSinksForTests(); }
    void cleanup() { LogTee::clearSinksForTests(); }

    // --- an ordinary dispatch reaches every sink once -----------------------------------------

    void dispatch_ordinaryMessage_reachesEverySinkExactlyOnce()
    {
        int firstCalls = 0;
        int secondCalls = 0;
        const LogTee::SinkHandle first = LogTee::addSink(
            [&](LogLevel, int, int, const QString& text) {
                if (text.contains(QStringLiteral("ordinary-dispatch-trigger"))) {
                    ++firstCalls;
                }
            });
        const LogTee::SinkHandle second = LogTee::addSink(
            [&](LogLevel, int, int, const QString& text) {
                if (text.contains(QStringLiteral("ordinary-dispatch-trigger"))) {
                    ++secondCalls;
                }
            });

        qWarning() << "ordinary-dispatch-trigger";

        QCOMPARE(firstCalls, 1);
        QCOMPARE(secondCalls, 1);

        LogTee::removeSink(first);
        LogTee::removeSink(second);
    }

    // --- IN-11: a throwing sink cannot jam the tee --------------------------------------------
    //
    // The RAII in-dispatch guard resets `s_inDispatch` on unwind, and the per-sink `try`/`catch`
    // stops an exception from ever crossing SDL's C callback (undefined behaviour under `/EHsc`,
    // the review's own note). Proven here by a sink that throws on every call: the dispatch that
    // triggers it must still finish (a LATER sink for the SAME message still runs), a LATER
    // message must still reach every sink, and a LATER `addSink()`/`removeSink()` on this thread
    // must still work immediately - proof the flag was actually reset, not left set by the
    // exception (IN-11's own finding).
    void dispatch_throwingSink_doesNotJamLaterMessagesOrLaterMutation()
    {
        int throwingSinkCalls = 0;
        int otherSinkCalls = 0;

        const LogTee::SinkHandle throwing = LogTee::addSink(
            [&](LogLevel, int, int, const QString& text) {
                if (!text.contains(QStringLiteral("throwing-sink-trigger"))) {
                    return;
                }
                ++throwingSinkCalls;
                throw std::runtime_error("tst_log_tee: deliberate test exception");
            });
        const LogTee::SinkHandle other = LogTee::addSink(
            [&](LogLevel, int, int, const QString& text) {
                if (text.contains(QStringLiteral("throwing-sink-trigger"))) {
                    ++otherSinkCalls;
                }
            });

        qWarning() << "throwing-sink-trigger"; // must not crash or hang

        QCOMPARE(throwingSinkCalls, 1);
        QCOMPARE(otherSinkCalls, 1); // the same dispatch's later sink still ran

        qWarning() << "throwing-sink-trigger"; // a later message still reaches both sinks
        QCOMPARE(throwingSinkCalls, 2);
        QCOMPARE(otherSinkCalls, 2);

        // A later addSink()/removeSink() on this thread must work immediately, not be refused -
        // which would mean the flag was never reset after the throw above.
        int thirdCalls = 0;
        const LogTee::SinkHandle third =
            LogTee::addSink([&](LogLevel, int, int, const QString& text) {
                if (text.contains(QStringLiteral("post-throw-trigger"))) {
                    ++thirdCalls;
                }
            });
        qWarning() << "post-throw-trigger";
        QCOMPARE(thirdCalls, 1);

        LogTee::removeSink(throwing);
        LogTee::removeSink(other);
        LogTee::removeSink(third);
    }

    // --- WR-07: kept, unchanged guarantee ------------------------------------------------------
    //
    // Ported from `tst_stream_stats.cpp`'s own version of this test (06.3-15/06.3-review-fixes),
    // unchanged in substance: a sink is parked mid-call on one background thread, and
    // `removeSink()` - called on a second background thread, so the test thread itself is never
    // blocked - must not return before the parked sink does.
    void removeSink_waitsForADispatchAlreadyInFlightOnAnotherThread()
    {
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

        QScopedPointer<QThread> dispatcherThread(
            QThread::create([&]() { qWarning() << "wr07-trigger"; }));
        dispatcherThread->start();

        QVERIFY2(entered.tryAcquire(1, 5000), "the sink never started running");

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

    // --- refuse loudly: death tests, run as a child process -----------------------------------

    void addSink_calledFromInsideItsOwnSinksDispatch_abortsWithReasonLine()
    {
        runReentryDeathTest(QStringLiteral("add"), QStringLiteral("addSink"));
    }

    void removeSink_calledFromInsideItsOwnSinksDispatch_abortsWithReasonLine()
    {
        runReentryDeathTest(QStringLiteral("remove"), QStringLiteral("removeSink"));
    }

private:
    void runReentryDeathTest(const QString& mode, const QString& functionName)
    {
        QProcess child;
        child.setProgram(m_appPath);
        child.setArguments({ QStringLiteral("--reentry-child"), mode });
        child.start();
        QVERIFY2(child.waitForStarted(5000), "the reentry-child process must start");
        QVERIFY2(child.waitForFinished(10000), "the reentry-child process never finished");

        // `_set_abort_behavior`/`SetErrorMode` in `runReentryChild()` turn this into a plain,
        // clean-looking `_exit(3)` rather than an OS-level exception - so `NormalExit` with exit
        // code 3 is the abort, not evidence the child failed to reach it (see this file's own
        // header comment, and `06.3.1-RESEARCH-SPIKE-CRASHPAD.md` Appendix A).
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 3);

        const QByteArray stderrOutput = child.readAllStandardError();
        const QString expectedReason =
            QStringLiteral("LogTee: %1() called from inside a sink's dispatch").arg(functionName);
        QVERIFY2(stderrOutput.contains(expectedReason.toUtf8()),
                 qPrintable(QStringLiteral("stderr did not contain the reason line %1 - got: %2")
                                .arg(expectedReason, QString::fromUtf8(stderrOutput))));
    }

    QString m_appPath;
};

int main(int argc, char* argv[])
{
    if (argc > 1 && std::strcmp(argv[1], "--reentry-child") == 0) {
        const QString mode = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString();
        return runReentryChild(mode);
    }

    QCoreApplication app(argc, argv);
    TstLogTee testCase;
    return QTest::qExec(&testCase, argc, argv);
}

#include "tst_log_tee.moc"
