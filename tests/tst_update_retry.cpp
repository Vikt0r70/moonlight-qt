#include <QtTest>
#include <QTemporaryDir>
#include "seathub/update_retry_state.h"

// ADR-0086 supersedes the pre-journal quiescence rule (V38) and the launch trace of the setup the client
// used to start; the bounded retry (V34, ADR-0077) is what these tests keep.

class TstUpdateRetry : public QObject
{
    Q_OBJECT
    const QDateTime clock = QDateTime::fromString("2026-09-30T12:00:00Z", Qt::ISODate);
private slots:
    void equalJitterMatchesV34()
    {
        QRandomGenerator rng(70);
        UpdateRetryState state; state.setRandomGenerator(&rng);
        QCOMPARE(state.nextDelayFor(1), 0);
        for (int i = 0; i < 100; ++i) {
            const auto second = state.nextDelayFor(2), third = state.nextDelayFor(3);
            QVERIFY(second >= 3600 && second <= 7200);
            QVERIFY(third >= 7200 && third <= 14400);
            QVERIFY(state.nextDelayFor(30) <= 86400);
        }
    }
    void failuresPersistAcrossLaunchAndStopAtFour()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto path = dir.filePath("retry-state.json");
        UpdateRetryState state; state.setClock([this]{ return clock; });
        QVERIFY(state.reset("0.2.0", "0.1.0"));
        QVERIFY(state.automaticAttemptAllowed(clock, false));
        state.launched = true; state.noteFailure("installer.locked_file");
        QCOMPARE(state.attemptsFailed, 1); QVERIFY(!state.launched);
        QVERIFY(!state.automaticAttemptAllowed(clock, false)); // first retry is NEXT launch.
        QVERIFY(state.save(path)); state = UpdateRetryState::load(path);
        QVERIFY(state.automaticAttemptAllowed(clock, false));
        QVERIFY(!state.automaticAttemptAllowed(clock, true));
        state.setClock([this]{ return clock; }); state.noteFailure("installer.locked_file");
        QVERIFY(!state.automaticAttemptAllowed(clock.addSecs(3599), false));
        QVERIFY(state.automaticAttemptAllowed(clock.addSecs(7200), false));
        state.noteFailure("installer.disk_space");
        QVERIFY(state.nextAllowedAt >= clock.addSecs(7200));
        QVERIFY(state.nextAllowedAt <= clock.addSecs(14400));
        state.noteFailure("installer.script_error");
        QVERIFY(state.exhausted()); QVERIFY(!state.automaticAttemptAllowed(clock.addDays(30), false));
        QVERIFY(state.save(path)); state = UpdateRetryState::load(path);
        QVERIFY(state.exhausted()); QVERIFY(!state.reset("0.2.0", "0.1.0"));
        QCOMPARE(state.attemptsFailed, 4);
    }
    void manualAndDeclinedAttemptsNeverConsumeTheBudget()
    {
        UpdateRetryState state; state.setClock([this]{ return clock; });
        state.reset("0.2.0", "0.1.0");
        state.noteFailure("installer.locked_file", false, true);
        QCOMPARE(state.attemptsFailed, 0);
        state.launched = true; state.noteDecline();
        QCOMPARE(state.attemptsFailed, 0); QCOMPARE(state.declines, 1); QVERIFY(!state.launched);
        QVERIFY(!state.automaticAttemptAllowed(clock.addDays(30), false));
        state.noteFailure("installer.verify_failed", true, true);
        state.noteDecline(); QVERIFY(state.exhausted()); QCOMPARE(state.attemptsFailed, 0);
    }
    void aStateFileFromTheStagedInstallerStillLoads()
    {
        // A file written before ADR-0086 carries the launch trace and the pending marker of a setup that no
        // longer exists. The budget it holds is honoured; the trace is ignored and never written back.
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto path = dir.filePath("retry-state.json");
        QFile old(path); QVERIFY(old.open(QIODevice::WriteOnly));
        old.write("{\"v\":1,\"target\":\"0.2.0\",\"attempts_failed\":2,\"last_class\":\"installer.locked_file\","
                  "\"launched\":true,\"launch_trace\":{\"pid\":4242,\"observations\":3}}");
        old.close();
        auto state = UpdateRetryState::load(path);
        QCOMPARE(state.target, QString("0.2.0")); QCOMPARE(state.attemptsFailed, 2);
        QVERIFY(state.launched);
        QVERIFY(state.save(path));
        QFile again(path); QVERIFY(again.open(QIODevice::ReadOnly));
        QVERIFY(!again.readAll().contains("launch_trace"));
    }
    void aNewerInstalledBuildCountsAsReached()
    {
        // ADR-0077 Amendment R5: an install equal to or newer than the target is current and is never moved
        // down. Only an older install converges, under the same four attempts.
        UpdateRetryState state; state.setClock([this]{ return clock; });
        QVERIFY(state.reset("0.1.27", "0.1.26"));            // older: the budget starts
        state.noteFailure("installer.locked_file"); QCOMPARE(state.attemptsFailed, 1);
        QVERIFY(!state.reset("0.1.27", "0.1.26"));           // still older: nothing resets
        QCOMPARE(state.attemptsFailed, 1);
        state.noteFailure("installer.locked_file"); state.noteFailure("installer.locked_file");
        state.noteFailure("installer.locked_file");
        QVERIFY(state.exhausted());
        QVERIFY(state.reset("0.1.27", "0.1.30"));            // newer is reached
        QVERIFY(!state.exhausted()); QCOMPARE(state.attemptsFailed, 0);
        state.noteFailure("installer.locked_file");
        QVERIFY(state.reset("0.1.27", "0.1.28"));            // so is the install the live test uses
        QCOMPARE(state.attemptsFailed, 0);
        state.noteFailure("installer.locked_file");
        QVERIFY(state.reset("0.1.27", "0.1.27"));            // and an equal one
        QCOMPARE(state.attemptsFailed, 0);
    }
    void persistentFailureBlocksWithReinstall()
    {
        // After the four automatic attempts the state is exhausted: automatic attempts stop for good, a
        // manual Try again stays free, and the one thing that ends it is the CURRENT release changing
        // or being reached, never time (ADR-0077 item 4; the feed-side reinstall is tst_update_feed).
        UpdateRetryState state; state.setClock([this]{ return clock; });
        QVERIFY(state.reset("0.2.0", "0.1.0"));
        for (int i = 0; i < UpdateRetryState::kMaxAttempts; ++i) state.noteFailure("installer.locked_file");
        QVERIFY(state.exhausted()); QCOMPARE(state.attemptsFailed, UpdateRetryState::kMaxAttempts);
        QVERIFY(!state.automaticAttemptAllowed(clock.addDays(365), false));
        state.noteFailure("installer.locked_file", false, true);           // a manual Try again
        state.noteDecline();                                              // a declined prompt
        QCOMPARE(state.attemptsFailed, UpdateRetryState::kMaxAttempts);
        QVERIFY(state.exhausted());
        QVERIFY(!state.reset("0.2.0", "0.1.0"));                          // the same target stays blocked
        QVERIFY(state.reset("0.3.0", "0.1.0"));                           // a newer current release starts fresh
        QVERIFY(!state.exhausted()); QCOMPARE(state.attemptsFailed, 0);
        // A permanent class (a checksum mismatch) blocks at once, without spending the budget.
        state.noteFailure("installer.verify_failed", true);
        QVERIFY(state.exhausted()); QCOMPARE(state.attemptsFailed, 0);
        QVERIFY(!state.automaticAttemptAllowed(clock.addDays(365), false));
    }
    void permanentIntegrityFailureExhaustsWithoutResettingOnTime()
    {
        UpdateRetryState state; state.setClock([this]{ return clock; });
        state.reset("0.2.0", "0.1.0"); state.noteFailure("installer.verify_failed", true);
        QVERIFY(state.exhausted()); QVERIFY(!state.automaticAttemptAllowed(clock.addDays(30), false));
        QVERIFY(!state.reset("0.2.0", "0.1.9")); QVERIFY(state.exhausted());
        QVERIFY(state.reset("0.3.0", "0.1.9")); QVERIFY(!state.exhausted());
        state.noteFailure("installer.verify_failed", true);
        QVERIFY(state.reset("0.3.0", "0.3.0")); QVERIFY(!state.exhausted());
        QCOMPARE(state.attemptsFailed, 0);
    }
};
QTEST_GUILESS_MAIN(TstUpdateRetry)
#include "tst_update_retry.moc"
