/*****************************************************************************
 * SeatHub fork - the port-test result (ADR-0104, owner rulings (a) and (g) of 2026-10-17).
 *
 * `LiTestClientConnectivity`'s return value as a closed class, the wire `port_test` object and the
 * condition under which the existing firewall sentence may show (a conclusive failed port only).
 * Qt Core only: builds and runs on Linux through tests/linux/run-core-suites.sh.
 *
 * Build recipe: see tst_port_test_result.pro.
 *****************************************************************************/

#include <QtTest>

#include <QJsonDocument>

#include "seathub/port_test_result.h"

namespace {

// ML_TEST_RESULT_INCONCLUSIVE at the pinned moonlight-common-c commit (ADR-0104 section 2).
constexpr unsigned int kInconclusive = 0xFFFFFFFFu;
// A few ML_PORT_FLAG_* bits (Limelight.h): TCP 48010 0x0004, UDP 47999 0x0200, UDP 48000 0x0400.
constexpr unsigned int kFlagsRequested = 0x0004u | 0x0200u | 0x0400u;

} // namespace

class TstPortTestResult : public QObject
{
    Q_OBJECT

private slots:
    void noRequestedFlagsIsNotRunWhateverTheLibraryReturned_data()
    {
        QTest::addColumn<unsigned int>("result");
        QTest::newRow("zero") << 0u;
        QTest::newRow("a mask") << 0x0204u;
        QTest::newRow("the sentinel") << kInconclusive;
    }

    void noRequestedFlagsIsNotRunWhateverTheLibraryReturned()
    {
        QFETCH(unsigned int, result);
        const PortTestResult r = classifyPortTest(0u, result, kInconclusive);
        QCOMPARE(r.outcome, PortTestResult::Outcome::NotRun);
        QCOMPARE(r.failedMask, quint32(0));
        QVERIFY(portTestJson(r).isEmpty());
        QVERIFY(!showFirewallSentence(r));
    }

    void aZeroReturnForRequestedFlagsIsClean()
    {
        const PortTestResult r = classifyPortTest(kFlagsRequested, 0u, kInconclusive);
        QCOMPARE(r.outcome, PortTestResult::Outcome::Clean);
        QCOMPARE(r.failedMask, quint32(0));

        const QJsonObject json = portTestJson(r);
        QCOMPARE(json.size(), 1);
        QCOMPARE(json.value(QStringLiteral("outcome")).toString(), QStringLiteral("clean"));
        QVERIFY(!json.contains(QStringLiteral("failed_mask")));
        QVERIFY(!showFirewallSentence(r));
    }

    void theSentinelIsInconclusive()
    {
        const PortTestResult r = classifyPortTest(kFlagsRequested, kInconclusive, kInconclusive);
        QCOMPARE(r.outcome, PortTestResult::Outcome::Inconclusive);
        QCOMPARE(r.failedMask, quint32(0));

        const QJsonObject json = portTestJson(r);
        QCOMPARE(json.size(), 1);
        QCOMPARE(json.value(QStringLiteral("outcome")).toString(),
                 QStringLiteral("inconclusive"));
        QVERIFY(!json.contains(QStringLiteral("failed_mask")));
        QVERIFY(!showFirewallSentence(r));
    }

    void theSentinelIsAParameterNotAConstant()
    {
        // A different sentinel value makes 0xFFFFFFFF an ordinary (failed) mask: the header never
        // learns the library's value, it is told.
        const PortTestResult other = classifyPortTest(kFlagsRequested, 0x7u, 0x7u);
        QCOMPARE(other.outcome, PortTestResult::Outcome::Inconclusive);
        const PortTestResult failedAll = classifyPortTest(kFlagsRequested, kInconclusive, 0x7u);
        QCOMPARE(failedAll.outcome, PortTestResult::Outcome::Failed);
    }

    void aNonZeroMaskIsFailedAndKeepsTheMask()
    {
        const unsigned int mask = 0x0204u; // TCP 48010 + UDP 47999
        const PortTestResult r = classifyPortTest(kFlagsRequested, mask, kInconclusive);
        QCOMPARE(r.outcome, PortTestResult::Outcome::Failed);
        QCOMPARE(r.failedMask, quint32(mask));

        const QJsonObject json = portTestJson(r);
        QCOMPARE(json.size(), 2);
        QCOMPARE(json.value(QStringLiteral("outcome")).toString(), QStringLiteral("failed"));
        QVERIFY(json.value(QStringLiteral("failed_mask")).isDouble());
        QCOMPARE(json.value(QStringLiteral("failed_mask")).toInteger(), qint64(mask));
        QVERIFY(showFirewallSentence(r));
    }

    void theLargestMaskBelowTheSentinelSurvivesTheJsonRoundTrip()
    {
        const PortTestResult r = classifyPortTest(kFlagsRequested, 0xFFFFFFFEu, kInconclusive);
        QCOMPARE(r.outcome, PortTestResult::Outcome::Failed);
        const QByteArray text = QJsonDocument(portTestJson(r)).toJson(QJsonDocument::Compact);
        const QJsonObject back = QJsonDocument::fromJson(text).object();
        QCOMPARE(back.value(QStringLiteral("failed_mask")).toInteger(), qint64(4294967294LL));
    }

    void theFirewallSentenceShowsOnlyForAConclusiveFailure()
    {
        PortTestResult r;
        r.outcome = PortTestResult::Outcome::NotRun;
        QVERIFY(!showFirewallSentence(r));
        r.outcome = PortTestResult::Outcome::Clean;
        QVERIFY(!showFirewallSentence(r));
        r.outcome = PortTestResult::Outcome::Inconclusive;
        QVERIFY(!showFirewallSentence(r));
        r.outcome = PortTestResult::Outcome::Failed;
        r.failedMask = 0x4u;
        QVERIFY(showFirewallSentence(r));
    }

    void aDefaultResultIsNotRun()
    {
        const PortTestResult r;
        QCOMPARE(r.outcome, PortTestResult::Outcome::NotRun);
        QVERIFY(portTestJson(r).isEmpty());
        QVERIFY(!showFirewallSentence(r));
    }

    void theWireObjectCarriesNoFieldBeyondOutcomeAndMask()
    {
        // No address, no host name: an outcome and a bitmask of well-known port numbers only.
        const QJsonObject json = portTestJson(classifyPortTest(kFlagsRequested, 0x0204u, kInconclusive));
        QStringList keys = json.keys();
        keys.sort();
        QCOMPARE(keys, (QStringList{QStringLiteral("failed_mask"), QStringLiteral("outcome")}));
    }
};

QTEST_MAIN(TstPortTestResult)

#include "tst_port_test_result.moc"
