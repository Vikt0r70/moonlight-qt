#include <QtTest>
#include <QTemporaryDir>
#include "seathub/update_retry_state.h"

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
