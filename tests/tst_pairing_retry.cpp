/*****************************************************************************
 * SeatHub fork - the pairing retry budget (INTENT SC18/N18 correction 2026-10-17, ADR-0072 item 2
 * amendment, ADR-0098 amendment).
 *
 * One automatic retry per Play, shared by the 409 / ALREADY_IN_PROGRESS clear and by any other
 * non-PAIRED handshake result; never after success, a generation change, the `reconnecting`
 * flow, an existing stream or the single 90 s seam deadline. Qt Core only: this suite builds and
 * runs on Linux through tests/linux/run-core-suites.sh.
 *
 * Build recipe: see tst_pairing_retry.pro.
 *****************************************************************************/

#include <QtTest>

#include "seathub/pairing_retry.h"

namespace {

constexpr qint64 kDeadlineMs = 90000;

PairingRetryBudget::Inputs failed(bool conflict = false)
{
    PairingRetryBudget::Inputs in;
    in.ok = false;
    in.pairingConflict = conflict;
    in.generationCurrent = true;
    in.reconnecting = false;
    in.streamExists = false;
    in.elapsedMs = 1000;
    in.deadlineMs = kDeadlineMs;
    return in;
}

} // namespace

class TstPairingRetry : public QObject
{
    Q_OBJECT

private slots:
    void aFreshBudgetIsUnspent()
    {
        PairingRetryBudget budget;
        QVERIFY(!budget.spent());
    }

    void aConflictGrantsTheConflictReasonOnce()
    {
        PairingRetryBudget budget;
        const auto first = budget.decide(failed(true));
        QVERIFY(first.has_value());
        QCOMPARE(*first, PairingRetryBudget::Reason::Conflict);
        QVERIFY(budget.spent());
        QVERIFY(!budget.decide(failed(true)).has_value());
    }

    void anyOtherFailureGrantsTheFailureReasonOnce()
    {
        PairingRetryBudget budget;
        const auto first = budget.decide(failed(false));
        QVERIFY(first.has_value());
        QCOMPARE(*first, PairingRetryBudget::Reason::Failure);
        QVERIFY(budget.spent());
        QVERIFY(!budget.decide(failed(false)).has_value());
    }

    void aConflictClearFollowedByAFailureIsNotASecondRetry()
    {
        PairingRetryBudget budget;
        QVERIFY(budget.decide(failed(true)).has_value());
        // The retried handshake failed again with a plain failure: the budget is shared.
        QVERIFY(!budget.decide(failed(false)).has_value());
    }

    void aFailureFollowedByAConflictIsNotASecondRetry()
    {
        PairingRetryBudget budget;
        QVERIFY(budget.decide(failed(false)).has_value());
        QVERIFY(!budget.decide(failed(true)).has_value());
    }

    void noRetryAfterSuccess()
    {
        PairingRetryBudget budget;
        PairingRetryBudget::Inputs in = failed(true);
        in.ok = true;
        QVERIFY(!budget.decide(in).has_value());
        QVERIFY(!budget.spent());
    }

    void noRetryAfterAGenerationChange()
    {
        PairingRetryBudget budget;
        PairingRetryBudget::Inputs in = failed(true);
        in.generationCurrent = false;
        QVERIFY(!budget.decide(in).has_value());
        QVERIFY(!budget.spent());
    }

    void noRetryInTheReconnectingFlow()
    {
        PairingRetryBudget budget;
        PairingRetryBudget::Inputs in = failed(false);
        in.reconnecting = true;
        QVERIFY(!budget.decide(in).has_value());
        QVERIFY(!budget.spent());
    }

    void noRetryOnceAStreamExists()
    {
        PairingRetryBudget budget;
        PairingRetryBudget::Inputs in = failed(false);
        in.streamExists = true;
        QVERIFY(!budget.decide(in).has_value());
        QVERIFY(!budget.spent());
    }

    void theDeadlineIsExclusive()
    {
        PairingRetryBudget budget;
        PairingRetryBudget::Inputs in = failed(false);
        in.elapsedMs = kDeadlineMs;
        QVERIFY(!budget.decide(in).has_value());
        in.elapsedMs = kDeadlineMs + 1;
        QVERIFY(!budget.decide(in).has_value());
        QVERIFY(!budget.spent());
        in.elapsedMs = kDeadlineMs - 1;
        QVERIFY(budget.decide(in).has_value());
    }

    void aBlockedDecisionDoesNotSpendTheBudget()
    {
        PairingRetryBudget budget;
        PairingRetryBudget::Inputs blocked = failed(false);
        blocked.streamExists = true;
        QVERIFY(!budget.decide(blocked).has_value());
        QVERIFY(!budget.spent());
        // The conditions later allow it (a fresh decision): the one retry is still available.
        QVERIFY(budget.decide(failed(false)).has_value());
    }

    void aZeroDeadlineGrantsNothing()
    {
        PairingRetryBudget budget;
        PairingRetryBudget::Inputs in = failed(false);
        in.elapsedMs = 0;
        in.deadlineMs = 0;
        QVERIFY(!budget.decide(in).has_value());
    }

    void eachInstanceHasItsOwnBudget()
    {
        PairingRetryBudget one;
        PairingRetryBudget two;
        QVERIFY(one.decide(failed(false)).has_value());
        QVERIFY(two.decide(failed(false)).has_value());
    }

    void reasonTokensAreTheClosedWireValues()
    {
        QCOMPARE(PairingRetryBudget::token(PairingRetryBudget::Reason::Conflict),
                 QStringLiteral("conflict"));
        QCOMPARE(PairingRetryBudget::token(PairingRetryBudget::Reason::Failure),
                 QStringLiteral("failure"));
    }

    void pinShapeOkIsExactlyFourAsciiDigits_data()
    {
        QTest::addColumn<QString>("pin");
        QTest::addColumn<bool>("expected");
        QTest::newRow("four digits") << QStringLiteral("0427") << true;
        QTest::newRow("all zeros") << QStringLiteral("0000") << true;
        QTest::newRow("empty") << QString() << false;
        QTest::newRow("three digits") << QStringLiteral("123") << false;
        QTest::newRow("five digits") << QStringLiteral("12345") << false;
        QTest::newRow("a letter") << QStringLiteral("12a4") << false;
        QTest::newRow("a space") << QStringLiteral("12 4") << false;
        QTest::newRow("a trailing newline") << QStringLiteral("1234\n") << false;
        QTest::newRow("a sign") << QStringLiteral("+123") << false;
        // Arabic-Indic digits are digits for QChar::isDigit but not ASCII.
        QTest::newRow("arabic-indic digits")
            << QString::fromUtf16(u"١٢٣٤") << false;
        QTest::newRow("fullwidth digits")
            << QString::fromUtf16(u"１２３４") << false;
    }

    void pinShapeOkIsExactlyFourAsciiDigits()
    {
        QFETCH(QString, pin);
        QFETCH(bool, expected);
        QCOMPARE(PairingRetryBudget::pinShapeOk(pin), expected);
    }
};

QTEST_MAIN(TstPairingRetry)

#include "tst_pairing_retry.moc"
