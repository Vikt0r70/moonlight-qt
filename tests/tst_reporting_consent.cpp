/*****************************************************************************
 * SeatHub fork - install-time reporting choice (ADR-0107, T021): the on-disk format the RentaPC
 * setup window writes and the app reads (`reporting_consent_store.h`), the one switch every covered
 * sender asks (`ReportingConsent::coveredUploadsAllowed()`), the silent-install rule, the
 * notice-version re-ask rule, and the proof that Sentry is never initialised without a recorded
 * "send". The senders that sit behind the switch have their own cases next to their own suites
 * (tst_log_shipper, tst_facade_wiring); this suite owns the store and the switch.
 *
 * Every test points the store at a private per-process registry key, never the real
 * `HKCU\Software\Seven Hills\SeatHub\Reporting`.
 *
 * Build recipe (nothing is on PATH machine-wide - Qt and MSVC are both absolute):
 *   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
 *   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;C:\Qt\Tools\Ninja;%PATH%
 *   cd tests && qmake tst_reporting_consent.pro && jom && tst_reporting_consent.exe -o tst_reporting_consent-out.txt,txt
 *****************************************************************************/

#include <QtTest>

#include <QCoreApplication>
#include <QFile>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "seathub/reporting_consent.h"
#include "seathub/telemetry.h"

#ifdef _WIN32
#include <windows.h>
#endif

using reporting_consent_store::Record;

namespace {

std::wstring testKeyParent()
{
    return L"Software\\Seven Hills\\SeatHubConsentTest-"
        + std::to_wstring(static_cast<unsigned long long>(QCoreApplication::applicationPid()));
}

} // namespace

class TstReportingConsent : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        reporting_consent_store::setKeyPathForTests(testKeyParent() + L"\\Reporting");
        reporting_consent_store::clear();
        ReportingConsent::setStateForTests(false, Record{});
    }

    void cleanupTestCase()
    {
        reporting_consent_store::clear();
#ifdef _WIN32
        RegDeleteTreeW(HKEY_CURRENT_USER, testKeyParent().c_str());
#endif
        ReportingConsent::setStateForTests(false, Record{});
    }

    // --- (g) the value in the format the setup window writes reads back exactly -----------------

    void aValueTheSetupWindowWritesReadsBackExactly()
    {
        QVERIFY(!reporting_consent_store::read().recorded);

        QVERIFY(reporting_consent_store::write(false, 7));
        Record record = reporting_consent_store::read();
        QVERIFY(record.recorded);
        QVERIFY(!record.send);
        QCOMPARE(record.noticeVersion, 7u);

        QVERIFY(reporting_consent_store::write(true, 1));
        record = reporting_consent_store::read();
        QVERIFY(record.recorded);
        QVERIFY(record.send);
        QCOMPARE(record.noticeVersion, 1u);
    }

    void aChoiceWithoutAKnownValueIsNoChoice()
    {
#ifdef _WIN32
        HKEY key = nullptr;
        QCOMPARE(RegCreateKeyExW(HKEY_CURRENT_USER, (testKeyParent() + L"\\Reporting").c_str(), 0,
                                 nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr),
                 static_cast<LONG>(ERROR_SUCCESS));
        const DWORD bogus = 9;
        RegSetValueExW(key, L"Choice", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&bogus),
                       sizeof(bogus));
        RegCloseKey(key);
        QVERIFY(!reporting_consent_store::read().recorded);
        ReportingConsent::loadFromStore();
        QVERIFY(!ReportingConsent::coveredUploadsAllowed());
#endif
    }

    // --- (h) a silent run never overwrites a recorded choice -----------------------------------

    void silentRunKeepsRecordedChoice()
    {
        // Recorded "do not send" at an older notice version: a silent install over it changes nothing.
        QVERIFY(reporting_consent_store::write(false, 3));
        QVERIFY(!reporting_consent_store::recordSilentDefaultIfNoChoice());
        Record record = reporting_consent_store::read();
        QVERIFY(record.recorded);
        QVERIFY(!record.send);
        QCOMPARE(record.noticeVersion, 3u);

        // Recorded "send" at an older notice version: its version is kept too, so the app still asks.
        QVERIFY(reporting_consent_store::write(true, 0));
        QVERIFY(!reporting_consent_store::recordSilentDefaultIfNoChoice());
        record = reporting_consent_store::read();
        QVERIFY(record.send);
        QCOMPARE(record.noticeVersion, 0u);

        // Only an empty store takes the silent default: send, at the current notice version.
        reporting_consent_store::clear();
        QVERIFY(reporting_consent_store::recordSilentDefaultIfNoChoice());
        record = reporting_consent_store::read();
        QVERIFY(record.recorded);
        QVERIFY(record.send);
        QCOMPARE(record.noticeVersion, reporting_consent_store::kCurrentNoticeVersion);
    }

    // --- (f) no recorded choice: nothing covered, and the app asks ------------------------------

    void noRecordedChoiceSendsNothingCoveredAndAsks()
    {
        ReportingConsent::loadFromStore();
        QVERIFY(!ReportingConsent::choiceRecorded());
        QVERIFY(!ReportingConsent::coveredUploadsAllowed());
        QVERIFY(ReportingConsent::needsAsk());
    }

    void aRecordedSendAllowsCoveredUploads()
    {
        QVERIFY(reporting_consent_store::write(true, reporting_consent_store::kCurrentNoticeVersion));
        ReportingConsent::loadFromStore();
        QVERIFY(ReportingConsent::coveredUploadsAllowed());
        QVERIFY(!ReportingConsent::needsAsk());
    }

    void aRecordedDoNotSendBlocksCoveredUploads()
    {
        QVERIFY(reporting_consent_store::write(false, reporting_consent_store::kCurrentNoticeVersion));
        ReportingConsent::loadFromStore();
        QVERIFY(!ReportingConsent::coveredUploadsAllowed());
        QVERIFY(!ReportingConsent::needsAsk());
    }

    // Suites that exercise a sender without main() never load; they keep behaving as before.
    void beforeTheStateIsLoadedTheSwitchDoesNotBlockLegacySuites()
    {
        QVERIFY(ReportingConsent::coveredUploadsAllowed());
        QVERIFY(!ReportingConsent::needsAsk());
    }

    // --- (b) an update keeps an unticked choice -------------------------------------------------

    void aNewProcessOverTheKeptValueKeepsOff()
    {
        QVERIFY(reporting_consent_store::write(false, reporting_consent_store::kCurrentNoticeVersion));
        ReportingConsent::loadFromStore();
        QVERIFY(!ReportingConsent::coveredUploadsAllowed());

        // "Update": the app folder is replaced and a new process starts; the per-user value stays.
        ReportingConsent::setStateForTests(false, Record{});
        ReportingConsent::loadFromStore();
        QVERIFY(!ReportingConsent::coveredUploadsAllowed());
        QVERIFY(!reporting_consent_store::read().send);
    }

    // --- (e) the notice version -----------------------------------------------------------------

    void aHigherNoticeVersionAsksAgainWithThePreviousChoiceInForce()
    {
        const unsigned stored = 1;
        const unsigned package = stored + 1;

        QVERIFY(reporting_consent_store::write(false, stored));
        ReportingConsent::loadFromStore();
        QVERIFY(ReportingConsent::needsAsk(package));
        QVERIFY(!ReportingConsent::coveredUploadsAllowed()); // unticked stays unticked until answered

        QVERIFY(reporting_consent_store::write(true, stored));
        ReportingConsent::loadFromStore();
        QVERIFY(ReportingConsent::needsAsk(package));
        QVERIFY(ReportingConsent::coveredUploadsAllowed()); // ticked stays ticked until answered

        // Answering records the answer against the new version; the same version never asks again.
        QVERIFY(ReportingConsent::recordChoice(false, package));
        QVERIFY(!ReportingConsent::needsAsk(package));
        QVERIFY(!ReportingConsent::coveredUploadsAllowed());
        QCOMPARE(reporting_consent_store::read().noticeVersion, package);
    }

    void theSameNoticeVersionNeverAsks()
    {
        QVERIFY(reporting_consent_store::write(true, 4));
        ReportingConsent::loadFromStore();
        QVERIFY(!ReportingConsent::needsAsk(4));
        QVERIFY(!ReportingConsent::needsAsk(3));
    }

    void aNoticeVersionAboveThePackageNeverAsks()
    {
        // A downgraded package (older notice) must not turn a recorded choice into "ask".
        QVERIFY(reporting_consent_store::write(false, 9));
        ReportingConsent::loadFromStore();
        QVERIFY(!ReportingConsent::needsAsk(8));
        QVERIFY(!ReportingConsent::coveredUploadsAllowed());
    }

    // --- the customer's own choice is the only thing that changes the switch ----------------------

    void onlyAnExplicitChoiceChangesTheSwitch()
    {
        QVERIFY(reporting_consent_store::write(false, reporting_consent_store::kCurrentNoticeVersion));
        ReportingConsent::loadFromStore();
        QVERIFY(!ReportingConsent::coveredUploadsAllowed());

        // A later load (sign-in, account change, update, reinstall all end here) cannot flip it.
        for (int i = 0; i < 3; ++i) {
            ReportingConsent::loadFromStore();
            QVERIFY(!ReportingConsent::coveredUploadsAllowed());
        }

        QVERIFY(ReportingConsent::recordChoice(true));
        QVERIFY(ReportingConsent::coveredUploadsAllowed());
        QVERIFY(reporting_consent_store::read().send);

        QVERIFY(ReportingConsent::recordChoice(false));
        QVERIFY(!ReportingConsent::coveredUploadsAllowed());
        QVERIFY(!reporting_consent_store::read().send);
    }

    // --- (d) Sentry is never initialised without a recorded "send" ------------------------------

    void sentryIsNotInitialisedWithoutARecordedSend_data()
    {
        QTest::addColumn<bool>("recorded");
        QTest::addColumn<bool>("send");
        QTest::newRow("no choice") << false << false;
        QTest::newRow("recorded off") << true << false;
    }

    void sentryIsNotInitialisedWithoutARecordedSend()
    {
        QFETCH(bool, recorded);
        QFETCH(bool, send);
        if (recorded) {
            QVERIFY(reporting_consent_store::write(send, reporting_consent_store::kCurrentNoticeVersion));
        }
        ReportingConsent::loadFromStore();
        QVERIFY(!ReportingConsent::coveredUploadsAllowed());

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        SeatHubTelemetry::Options options;
        options.databaseDir = dir.filePath(QStringLiteral("crash-db"));
        options.handlerPath = QCoreApplication::applicationFilePath(); // never launched: no init happens
        options.dsn = QStringLiteral("http://key@127.0.0.1:1/1");
        options.environment = QStringLiteral("test");
        options.release = QStringLiteral("seathub@test");

        const int initsBefore = SeatHubTelemetry::initCallCount();
        QVERIFY(!SeatHubTelemetry::startWith(options));
        QVERIFY(!SeatHubTelemetry::started());
        QCOMPARE(SeatHubTelemetry::initCallCount(), initsBefore);

        // A DSN handout arriving later (sign-in) must not start the SDK either, and the named
        // installer event is dropped rather than captured.
        SeatHubTelemetry::Handout handout;
        handout.dsn = QStringLiteral("http://key@127.0.0.1:1/1");
        handout.environment = QStringLiteral("test");
        handout.logs = true;
        SeatHubTelemetry::applyHandout(handout);
        QCOMPARE(SeatHubTelemetry::initCallCount(), initsBefore);
        QVERIFY(!SeatHubTelemetry::started());
        SeatHubTelemetry::deleteCache();
    }
};

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Seven Hills Consent Test"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_reporting_consent"));
    TstReportingConsent test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_reporting_consent.moc"
