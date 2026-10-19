// Single-instance guard tests (T026, ADR-0099, SC20).
//
// Every test uses its own throwaway app name, so the lock and pipe it creates never collide with
// a real copy of the app or with another test. The primary and the second copy are two roles in
// this one process; the cross-integrity (normal / elevated) orders are proven live by
// scripts/sc20-combos.ps1, not here.

#include <QtTest>

#include <atomic>

#include "seathub/single_instance.h"

using namespace SingleInstance;

namespace {

std::wstring uniqueAppName()
{
    static std::atomic<int> counter{0};
    return L"TstSingleInstance" + std::to_wstring(GetCurrentProcessId()) + L"x" + std::to_wstring(++counter);
}

Names namesForCurrentUser(const std::wstring &app)
{
    std::wstring sid;
    Names names;
    if (!currentUserSid(&sid) || !namesFor(app, sid, &names)) {
        qFatal("cannot build single-instance names for the current user");
    }
    return names;
}

// Opens the pipe once as a bare client that speaks no protocol, retrying only while it is not yet
// listening or busy. Not a QTRY_ condition: that macro re-evaluates its expression after it first
// passes, and a second open of this one-instance pipe would find it taken.
HANDLE openRawClient(const Names &names, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    for (;;) {
        HANDLE pipe = CreateFileW(names.pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr);
        if (pipe != INVALID_HANDLE_VALUE || clock.elapsed() >= timeoutMs) {
            return pipe;
        }
        QTest::qWait(10);
    }
}

// A short-wait profile for the tests that need a peer to time out.
Timing quickTiming()
{
    Timing timing;
    timing.startupBudgetMs = 600;
    timing.pipeOpenWindowMs = 300;
    timing.lineTimeoutMs = 300;
    timing.startupRetryMs = 20;
    return timing;
}

}  // namespace

class tst_single_instance : public QObject
{
    Q_OBJECT

private slots:
    void sidValidation_acceptsOnlyPlainSids();
    void names_followTheFrozenShapeForTheCurrentUser();
    void lock_secondAcquireIsDeniedAndReleaseFreesIt();
    void pipe_secondCreatorOfTheSameNameFails();
    void focus_roundTripAnswersOnceWithBits();
    void focus_oversizedLineIsDroppedAndTheNextClientStillWorks();
    void focus_silentClientTimesOutAndTheNextClientStillWorks();
    void startup_secondCopyAsksFirstToFocusAndIsAnswered();
    void startup_noReachableFirstCopyGivesUpAtTheBudget();
    void startup_unreadableGuardStartsUnguardedWithOneWarning();
    void listener_pipeCreationFailureWarnsAndKeepsRunning();
    void exitCodes_areTheFrozenOnes();
};

void tst_single_instance::sidValidation_acceptsOnlyPlainSids()
{
    QVERIFY(isPlainSid(L"S-1-5-21-1004336348-1177238915-682003330-1001"));
    QVERIFY(isPlainSid(L"S-1-5-18"));
    QVERIFY(!isPlainSid(L""));
    QVERIFY(!isPlainSid(L"S-1-"));
    QVERIFY(!isPlainSid(L"S-2-5-18"));
    QVERIFY(!isPlainSid(L"S-1-5-18 "));
    // Anything that could close the ACE and open another one is refused before it is embedded.
    QVERIFY(!isPlainSid(L"S-1-5-18)(A;;GA;;;WD"));
    QVERIFY(!isPlainSid(L"S-1-5-18;"));

    Names names;
    QVERIFY(!namesFor(L"App", L"S-1-5-18)(A;;GA;;;WD", &names));
    QVERIFY(!namesFor(L"", L"S-1-5-18", &names));
}

void tst_single_instance::names_followTheFrozenShapeForTheCurrentUser()
{
    std::wstring sid;
    QVERIFY(currentUserSid(&sid));
    QVERIFY(isPlainSid(sid));

    Names names;
    QVERIFY(namesFor(L"RentaPC", sid, &names));
    QCOMPARE(QString::fromStdWString(names.lockName), QString::fromStdWString(L"Local\\RentaPC-" + sid));
    QCOMPARE(QString::fromStdWString(names.pipeName),
             QString::fromStdWString(L"\\\\.\\pipe\\RentaPC-signal-" + sid));
    QCOMPARE(QString::fromStdWString(names.lockSddl),
             QString::fromStdWString(L"D:P(A;;GA;;;" + sid + L")(A;;GA;;;SY)S:(ML;;NW;;;ME)"));
    QCOMPARE(QString::fromStdWString(names.pipeSddl),
             QString::fromStdWString(L"D:P(A;;GRGW;;;" + sid + L")(A;;GA;;;SY)S:(ML;;NW;;;ME)"));
}

void tst_single_instance::lock_secondAcquireIsDeniedAndReleaseFreesIt()
{
    const Names names = namesForCurrentUser(uniqueAppName());

    HANDLE first = nullptr;
    QCOMPARE(acquireLock(names, &first), LockResult::Primary);
    QVERIFY(first != nullptr);

    HANDLE second = nullptr;
    QCOMPARE(acquireLock(names, &second), LockResult::Existing);
    QVERIFY(second == nullptr);

    // The lock is never owned or waited on: closing the one handle is all it takes to free it.
    CloseHandle(first);
    HANDLE third = nullptr;
    QCOMPARE(acquireLock(names, &third), LockResult::Primary);
    CloseHandle(third);
}

void tst_single_instance::pipe_secondCreatorOfTheSameNameFails()
{
    const Names names = namesForCurrentUser(uniqueAppName());

    HANDLE first = createSignalPipe(names);
    QVERIFY(first != INVALID_HANDLE_VALUE);
    HANDLE second = createSignalPipe(names);
    QCOMPARE(second, INVALID_HANDLE_VALUE);
    QVERIFY(GetLastError() != ERROR_SUCCESS);
    CloseHandle(first);
}

void tst_single_instance::focus_roundTripAnswersOnceWithBits()
{
    const Names names = namesForCurrentUser(uniqueAppName());
    HANDLE pipe = createSignalPipe(names);
    QVERIFY(pipe != INVALID_HANDLE_VALUE);

    std::atomic<int> focusCalls{0};
    SignalServer server;
    server.start(names, pipe, Timing(), [&focusCalls] {
        ++focusCalls;
        return 3u;
    });

    unsigned bits = 0;
    QCOMPARE(signalExisting(names, Timing(), &bits), SignalResult::Answered);
    QCOMPARE(bits, 3u);
    // The ack is written after the handler ran, so the count is final once the ack is read.
    QCOMPARE(focusCalls.load(), 1);

    // A second ask reaches the same listener again (the instance is reusable after a disconnect).
    QCOMPARE(signalExisting(names, Timing(), &bits), SignalResult::Answered);
    QCOMPARE(focusCalls.load(), 2);
    server.stop();
}

void tst_single_instance::focus_oversizedLineIsDroppedAndTheNextClientStillWorks()
{
    const Names names = namesForCurrentUser(uniqueAppName());
    HANDLE pipe = createSignalPipe(names);
    QVERIFY(pipe != INVALID_HANDLE_VALUE);

    std::atomic<int> focusCalls{0};
    SignalServer server;
    server.start(names, pipe, quickTiming(), [&focusCalls] {
        ++focusCalls;
        return 1u;
    });

    HANDLE raw = openRawClient(names, 3000);
    QVERIFY(raw != INVALID_HANDLE_VALUE);

    // 70 bytes and no newline: over the 64-byte line limit.
    const std::string flood(70, 'F');
    DWORD written = 0;
    QVERIFY(WriteFile(raw, flood.data(), static_cast<DWORD>(flood.size()), &written, nullptr));
    CloseHandle(raw);

    unsigned bits = 0;
    Timing client = quickTiming();
    client.pipeOpenWindowMs = 3000;
    QCOMPARE(signalExisting(names, client, &bits), SignalResult::Answered);
    QCOMPARE(bits, 1u);
    QCOMPARE(focusCalls.load(), 1);  // only the well-formed client reached the handler
    server.stop();
}

void tst_single_instance::focus_silentClientTimesOutAndTheNextClientStillWorks()
{
    const Names names = namesForCurrentUser(uniqueAppName());
    HANDLE pipe = createSignalPipe(names);
    QVERIFY(pipe != INVALID_HANDLE_VALUE);

    std::atomic<int> focusCalls{0};
    SignalServer server;
    server.start(names, pipe, quickTiming(), [&focusCalls] {
        ++focusCalls;
        return 1u;
    });

    HANDLE silent = openRawClient(names, 3000);
    QVERIFY(silent != INVALID_HANDLE_VALUE);

    // The silent client holds the pipe open and says nothing. The listener must give up on it
    // by itself and serve the next client; it cannot be stuck in a blocking read.
    unsigned bits = 0;
    Timing client = quickTiming();
    client.pipeOpenWindowMs = 4000;
    QCOMPARE(signalExisting(names, client, &bits), SignalResult::Answered);
    QCOMPARE(focusCalls.load(), 1);
    CloseHandle(silent);
    server.stop();
}

void tst_single_instance::startup_secondCopyAsksFirstToFocusAndIsAnswered()
{
    const std::wstring app = uniqueAppName();

    StartupDecision first = decideStartup(app, quickTiming());
    QCOMPARE(first.kind, StartupDecision::Kind::Primary);
    QVERIFY(first.warnings.empty());

    std::atomic<int> focusCalls{0};
    Instance primary;
    primary.adopt(std::move(first));
    std::string warning;
    QVERIFY(primary.startListener(quickTiming(), [&focusCalls] {
        ++focusCalls;
        return 1u;
    }, &warning));
    QVERIFY(warning.empty());

    Timing secondTiming = quickTiming();
    secondTiming.startupBudgetMs = 5000;
    const StartupDecision second = decideStartup(app, secondTiming);
    QCOMPARE(second.kind, StartupDecision::Kind::AnsweredSecond);
    QVERIFY(second.lock == nullptr);  // the second copy never owns the lock
    QCOMPARE(focusCalls.load(), 1);

    primary.stop();
}

void tst_single_instance::startup_noReachableFirstCopyGivesUpAtTheBudget()
{
    const std::wstring app = uniqueAppName();
    const Names names = namesForCurrentUser(app);

    // Something holds the lock but nothing listens: a first copy that never opened its pipe.
    HANDLE holder = nullptr;
    QCOMPARE(acquireLock(names, &holder), LockResult::Primary);

    QElapsedTimer clock;
    clock.start();
    const StartupDecision decision = decideStartup(app, quickTiming());
    QCOMPARE(decision.kind, StartupDecision::Kind::NeverReached);
    QVERIFY(clock.elapsed() >= quickTiming().startupBudgetMs);
    QVERIFY(decision.lock == nullptr);

    CloseHandle(holder);
}

void tst_single_instance::startup_unreadableGuardStartsUnguardedWithOneWarning()
{
    // An empty app name cannot be turned into object names, which stands for "the guard cannot
    // be set up": the app starts anyway and says so exactly once.
    const StartupDecision decision = decideStartup(L"", quickTiming());
    QCOMPARE(decision.kind, StartupDecision::Kind::Unguarded);
    QCOMPARE(decision.warnings.size(), size_t(1));
    QVERIFY(decision.lock == nullptr);
}

void tst_single_instance::listener_pipeCreationFailureWarnsAndKeepsRunning()
{
    const std::wstring app = uniqueAppName();
    const Names names = namesForCurrentUser(app);

    // Someone already owns the pipe name, so this copy can win the lock but not the pipe.
    HANDLE squatter = createSignalPipe(names);
    QVERIFY(squatter != INVALID_HANDLE_VALUE);

    Instance instance;
    instance.adopt(decideStartup(app, quickTiming()));
    std::string warning;
    QVERIFY(!instance.startListener(quickTiming(), nullptr, &warning));
    QVERIFY2(warning.find("cannot be answered") != std::string::npos, warning.c_str());

    instance.stop();
    CloseHandle(squatter);
}

void tst_single_instance::exitCodes_areTheFrozenOnes()
{
    QCOMPARE(kExitAnswered, 0);
    QCOMPARE(kExitNeverReached, 3);
    QCOMPARE(kMaxLineBytes, size_t(64));
    const Timing frozen;
    QCOMPARE(frozen.startupBudgetMs, 10000);
    QCOMPARE(frozen.pipeOpenWindowMs, 500);
    QCOMPARE(frozen.lineTimeoutMs, 3000);
    QCOMPARE(frozen.startupRetryMs, 100);
}

QTEST_APPLESS_MAIN(tst_single_instance)

#include "tst_single_instance.moc"
