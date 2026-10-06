#include <QtTest>
#include <QTemporaryDir>
#include <type_traits>
#include "seathub/update_retry_state.h"

// ADR-0077 item 7 / V38: the quiescence rule is a pure function of observed state. It takes no clock, no
// duration and no journal age, so there is nothing to encode and nothing that elapsed time could unlock.
static_assert(std::is_same_v<decltype(&admitInstallerLaunch), Admission (*)(const InstallerObservation&)>,
              "admission must not take a time parameter");

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
    void admissionIsAStateRuleWithNoDuration()
    {
        using Owner = InstallerObservation::Owner; using Scan = InstallerObservation::Scan; using Boot = InstallerObservation::Boot;
        // Every combination: only a proven absence of a writer admits a new installer.
        for (const auto owner : {Owner::None, Owner::Alive, Owner::Gone, Owner::Unknown})
            for (const auto scan : {Scan::Clear, Scan::Present, Scan::Unknown})
                for (const auto boot : {Boot::Same, Boot::Changed, Boot::Unknown}) {
                    InstallerObservation o; o.owner = owner; o.scan = scan; o.boot = boot;
                    const bool expected = owner != Owner::Alive && scan == Scan::Clear
                        && (owner != Owner::Unknown || boot == Boot::Changed);
                    QVERIFY2((admitInstallerLaunch(o) == Admission::Quiescent) == expected,
                             qPrintable(QStringLiteral("%1 %2 %3").arg(ownerName(owner), scanName(scan), bootName(boot))));
                }
        // Journal age, retry timing and the clock are not inputs: the answer cannot change with time.
        UpdateRetryState state; state.setClock([this]{ return clock; });
        InstallerObservation held; held.owner = Owner::Alive; held.scan = Scan::Clear;
        state.noteFailure("installer.locked_file");
        state.setClock([this]{ return clock.addDays(365); });
        QCOMPARE(admitInstallerLaunch(held), Admission::Blocked);
    }
    void launchTraceSurvivesRestartAndStaysBoundedByConstruction()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto path = dir.filePath("retry-state.json");
        UpdateRetryState state; state.target = "0.2.0";
        state.trace.requestedAt = clock; state.trace.returnedAt = clock.addMSecs(81234);
        state.trace.shellError = 1223; state.trace.pid = 4242; state.trace.pidCreated = "133000000000000001";
        state.trace.launcherPid = 77; state.trace.launcherSession = 1; state.trace.installerSession = 2;
        state.trace.bootUptimeMs = 123456789012; state.trace.observations = 3;
        state.trace.lastOwner = "gone"; state.trace.lastScan = "clear"; state.trace.lastBoot = "changed";
        state.trace.ownerAccessDenied = true; state.trace.observerChanged = true;
        state.trace.journalStartPresent = true; state.trace.stagePresent = true; state.trace.progressChanged = true;
        QVERIFY(state.save(path));
        const auto back = UpdateRetryState::load(path).trace;
        QCOMPARE(back.requestedAt, clock); QCOMPARE(back.returnedAt, clock.addMSecs(81234));
        QCOMPARE(back.shellError, qint64(1223)); QCOMPARE(back.pid, qint64(4242));
        QCOMPARE(back.pidCreated, QString("133000000000000001")); QCOMPARE(back.launcherPid, qint64(77));
        QCOMPARE(back.launcherSession, qint64(1)); QCOMPARE(back.installerSession, qint64(2));
        QCOMPARE(back.bootUptimeMs, qint64(123456789012)); QCOMPARE(back.observations, 3);
        QCOMPARE(back.lastOwner, QString("gone")); QCOMPARE(back.lastBoot, QString("changed"));
        QVERIFY(back.ownerAccessDenied && back.observerChanged && back.journalStartPresent
                && back.stagePresent && back.progressChanged);
        // The next launch replaces it: one record, so the file cannot grow with the number of launches.
        const auto first = QFileInfo(path).size();
        for (int i = 0; i < 20; ++i) { state.trace.observations = 3; QVERIFY(state.save(path)); }
        QCOMPARE(QFileInfo(path).size(), first);
        // A state written before the observer existed has an empty trace, not an error.
        QFile old(path); QVERIFY(old.open(QIODevice::WriteOnly)); old.write("{\"v\":1,\"target\":\"0.2.0\"}"); old.close();
        const auto empty = UpdateRetryState::load(path).trace;
        QCOMPARE(empty.pid, qint64(0)); QVERIFY(empty.pidCreated.isEmpty()); QCOMPARE(empty.launcherSession, qint64(-1));
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
