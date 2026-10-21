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

// T094: the notice-version change-list test opens the real native notice window, the same way
// reporting_consent.cpp does, so this translation unit needs its definitions too.
#define RENTAPC_CONSENT_WINDOW 1

#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVector>

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

// ---- Bootstrapper visible-string audit (T083) -------------------------------------------------
// The setup file is a Win32 program that cannot link into this suite, so the audit reads its
// production sources as text. Source tests are not a readback of the built PE or of the installed
// window; docs/bootstrapper-visible-audit.md gives the T030 readback procedure for those.

struct AuditLiteral
{
    QString raw;       // spelling between the quotes, adjacent literals merged, escapes untouched
    bool wide = false;
};

struct AuditScan
{
    QString code;                  // comments and preprocessor lines removed, literals -> @LIT<n>@
    QVector<AuditLiteral> literals;
};

struct AuditRow
{
    QString id, kind, source, key, expected, authority;
};

bool isIdentChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_');
}

AuditScan scanCpp(const QString& src)
{
    AuditScan out;
    const int n = src.size();
    int i = 0;
    bool lineStart = true;

    const auto readString = [&](int& p) {
        QString raw;
        ++p; // opening quote
        while (p < n && src[p] != QLatin1Char('"')) {
            if (src[p] == QLatin1Char('\\') && p + 1 < n) {
                raw += src[p++];
            }
            raw += src[p++];
        }
        ++p; // closing quote
        return raw;
    };
    const auto skipSpace = [&](int p) {
        while (p < n && src[p].isSpace()) {
            ++p;
        }
        return p;
    };

    while (i < n) {
        const QChar c = src[i];
        if (c == QLatin1Char('\n')) {
            out.code += c;
            lineStart = true;
            ++i;
            continue;
        }
        if (lineStart && c.isSpace()) {
            out.code += c;
            ++i;
            continue;
        }
        if (lineStart && c == QLatin1Char('#')) {
            // Preprocessor line (with backslash continuations): never user-visible text.
            while (i < n && !(src[i] == QLatin1Char('\n') && src[i - 1] != QLatin1Char('\\'))) {
                ++i;
            }
            continue;
        }
        lineStart = false;
        if (c == QLatin1Char('/') && i + 1 < n && src[i + 1] == QLatin1Char('/')) {
            while (i < n && src[i] != QLatin1Char('\n')) {
                ++i;
            }
            continue;
        }
        if (c == QLatin1Char('/') && i + 1 < n && src[i + 1] == QLatin1Char('*')) {
            const int end = src.indexOf(QStringLiteral("*/"), i + 2);
            i = end < 0 ? n : end + 2;
            out.code += QLatin1Char(' ');
            continue;
        }
        if (c == QLatin1Char('\'')) {
            ++i;
            while (i < n && src[i] != QLatin1Char('\'')) {
                if (src[i] == QLatin1Char('\\')) {
                    ++i;
                }
                ++i;
            }
            ++i;
            out.code += QStringLiteral("''");
            continue;
        }
        const bool wide = c == QLatin1Char('L') && i + 1 < n && src[i + 1] == QLatin1Char('"')
            && (i == 0 || !isIdentChar(src[i - 1]));
        if (wide || c == QLatin1Char('"')) {
            AuditLiteral lit;
            lit.wide = wide;
            int p = wide ? i + 1 : i;
            lit.raw = readString(p);
            for (;;) {
                const int q = skipSpace(p);
                if (q + 1 < n && src[q] == QLatin1Char('L') && src[q + 1] == QLatin1Char('"')) {
                    p = q + 1;
                } else if (q < n && src[q] == QLatin1Char('"')) {
                    p = q;
                } else {
                    break;
                }
                lit.raw += readString(p);
            }
            out.code += QStringLiteral("@LIT%1@").arg(out.literals.size());
            out.literals.append(lit);
            i = p;
            continue;
        }
        out.code += c;
        ++i;
    }
    return out;
}

// A cell wrapped in backticks keeps its inner spaces (the separators are space-padded).
QString unwrapCell(const QString& cell)
{
    const QString t = cell.trimmed();
    if (t.size() >= 2 && t.startsWith(QLatin1Char('`')) && t.endsWith(QLatin1Char('`'))) {
        return t.mid(1, t.size() - 2);
    }
    return t;
}

QString forkRoot()
{
    QDir dir(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 8; ++i) {
        if (QFile::exists(dir.filePath(QStringLiteral("installer/bootstrapper/bootstrapper.rc")))) {
            return dir.absolutePath();
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return {};
}

bool readText(const QString& root, const QString& rel, QString* text)
{
    QFile f(QDir(root).filePath(rel));
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    *text = QString::fromUtf8(f.readAll());
    if (text->startsWith(QChar(0xFEFF))) {
        text->remove(0, 1);
    }
    text->replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    return true;
}

#ifdef _WIN32
// ---- Notice-window driver (T094) ----------------------------------------------------------------
// The notice window blocks in its own message loop, so the "customer" is a timer on that same
// thread: it fires inside the loop, reads what the real controls show, optionally flips the box,
// then presses OK or closes the window, exactly as the window's own handlers expect.

namespace window = reporting_consent_window;

struct NoticeDrive
{
    bool flipBox = false; // the customer clicks the box before answering
    bool close = false;   // the customer closes the window instead of pressing OK

    bool seen = false;
    bool headingDrawn = false;
    QString heading;
    QString items;
    bool boxBefore = false;
    bool boxAfter = false;
    bool warningAfter = false;
    int ticks = 0;
};

NoticeDrive* g_drive = nullptr;

BOOL CALLBACK findNotice(HWND hwnd, LPARAM out)
{
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"RentaPCReportingNotice") == 0) {
        *reinterpret_cast<HWND*>(out) = hwnd;
        return FALSE;
    }
    return TRUE;
}

QString childText(HWND parent, int id)
{
    HWND child = GetDlgItem(parent, id);
    wchar_t text[1024] = {};
    if (child) {
        GetWindowTextW(child, text, 1024);
    }
    return QString::fromWCharArray(text);
}

VOID CALLBACK driveTick(HWND, UINT, UINT_PTR, DWORD)
{
    NoticeDrive& d = *g_drive;
    if (d.seen) {
        return;
    }
    HWND notice = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), findNotice, reinterpret_cast<LPARAM>(&notice));
    if (!notice) {
        if (++d.ticks > 250) {
            PostQuitMessage(0); // never opened: end the loop instead of hanging the suite
        }
        return;
    }
    d.seen = true;
    namespace ids = window::detail;
    d.headingDrawn = GetDlgItem(notice, ids::kIdChangesHead) != nullptr;
    d.heading = childText(notice, ids::kIdChangesHead);
    d.items = childText(notice, ids::kIdChanges);
    HWND box = GetDlgItem(notice, ids::kIdCheck);
    d.boxBefore = SendMessageW(box, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (d.flipBox) {
        SendMessageW(box, BM_CLICK, 0, 0);
    }
    d.boxAfter = SendMessageW(box, BM_GETCHECK, 0, 0) == BST_CHECKED;
    d.warningAfter = IsWindowVisible(GetDlgItem(notice, ids::kIdWarnEn)) != FALSE;
    if (d.close) {
        SendMessageW(notice, WM_CLOSE, 0, 0);
    } else {
        SendMessageW(notice, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
    }
}

// Runs `show` (which opens the notice window on this thread) with the customer driver attached.
template <typename Show>
auto withCustomer(NoticeDrive* drive, Show show)
{
    g_drive = drive;
    const UINT_PTR timer = SetTimer(nullptr, 0, 20, driveTick);
    auto result = show();
    KillTimer(nullptr, timer);
    // show() can return before it reads the WM_QUIT its own WM_DESTROY posted; left in the queue it
    // would end the next window's loop at once, so each run drains it.
    MSG quit;
    while (PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {
    }
    g_drive = nullptr;
    return result;
}

// Test-local, clearly synthetic: the real package ships notice version one and no change list.
const wchar_t* const kSyntheticChanges[] = { L"Synthetic change one", L"Synthetic change two" };
constexpr int kSyntheticChangeCount = 2;

void checkRenderedChangeList(const NoticeDrive& drive)
{
    QVERIFY2(drive.seen, "the notice window never opened");
    QVERIFY2(drive.headingDrawn, "a nonempty change list must draw the What changed heading");
    QCOMPARE(drive.heading,
             QStringLiteral("What changed  /  ")
                 + QString::fromWCharArray(window::strings::baseSpec(true).changesHeading.ar));
    const QString bullet = QString(QChar(0x2022)) + QLatin1Char(' ');
    QCOMPARE(drive.items.split(QStringLiteral("\r\n"), Qt::SkipEmptyParts),
             (QStringList{ bullet + QStringLiteral("Synthetic change one"),
                           bullet + QStringLiteral("Synthetic change two") }));
}
#endif

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

    // T094: a nonempty "What changed" list really renders in the native window. The package only
    // ships notice version one, so version two and its change items are synthetic and test-local.
    void noticeVersionTwoRendersChangeListAndPreservesPriorChoice_data()
    {
        QTest::addColumn<bool>("priorSend");
        QTest::addColumn<bool>("flipBox");
        QTest::addColumn<bool>("close");
        QTest::addColumn<bool>("expectAnswered");
        QTest::addColumn<bool>("expectSend");
        QTest::newRow("off, left unticked, OK") << false << false << false << true << false;
        QTest::newRow("on, left ticked, OK") << true << false << false << true << true;
        QTest::newRow("off, ticked, OK") << false << true << false << true << true;
        QTest::newRow("on, unticked, OK") << true << true << false << true << false;
        QTest::newRow("off, window closed") << false << false << true << false << false;
        QTest::newRow("on, window closed") << true << false << true << false << true;
    }

    void noticeVersionTwoRendersChangeListAndPreservesPriorChoice()
    {
#ifdef _WIN32
        QFETCH(bool, priorSend);
        QFETCH(bool, flipBox);
        QFETCH(bool, close);
        QFETCH(bool, expectAnswered);
        QFETCH(bool, expectSend);

        const unsigned stored = reporting_consent_store::kCurrentNoticeVersion;
        const unsigned versionTwo = stored + 1;

        QVERIFY(reporting_consent_store::write(priorSend, stored));
        ReportingConsent::loadFromStore();
        QVERIFY(ReportingConsent::needsAsk(versionTwo));
        QCOMPARE(ReportingConsent::coveredUploadsAllowed(), priorSend); // in force until answered

        // The same spec the app's re-ask builds (reporting_consent.cpp), with the synthetic list.
        const Record previous = reporting_consent_store::read();
        const std::wstring notice = std::wstring(window::strings::kNoticeEn) + L"\r\n\r\n"
            + window::strings::kNoticeAr;
        window::Spec spec = window::strings::baseSpec(previous.recorded ? previous.send : true);
        spec.noticeText = notice.c_str();
        spec.changes = kSyntheticChanges;
        spec.changeCount = kSyntheticChangeCount;
        spec.primary = { L"OK", L"OK" };

        NoticeDrive drive;
        drive.flipBox = flipBox;
        drive.close = close;
        const window::Result result = withCustomer(&drive, [&] { return window::show(spec); });

        checkRenderedChangeList(drive);
        if (QTest::currentTestFailed()) {
            return;
        }
        QCOMPARE(drive.boxBefore, priorSend);          // the previous choice is pre-selected
        QCOMPARE(drive.boxAfter, expectSend);
        QCOMPARE(drive.warningAfter, !drive.boxAfter); // the red warning follows the box
        QCOMPARE(result.answered, expectAnswered);

        if (!expectAnswered) {
            // Closing answers nothing: nothing is recorded, the old choice stays, the app asks again.
            const Record kept = reporting_consent_store::read();
            QCOMPARE(kept.send, priorSend);
            QCOMPARE(kept.noticeVersion, stored);
            ReportingConsent::loadFromStore();
            QVERIFY(ReportingConsent::needsAsk(versionTwo));
            QCOMPARE(ReportingConsent::coveredUploadsAllowed(), priorSend);
            return;
        }

        QCOMPARE(result.send, expectSend);
        QVERIFY(ReportingConsent::recordChoice(result.send, versionTwo));
        QVERIFY(!ReportingConsent::needsAsk(versionTwo));
        QCOMPARE(ReportingConsent::coveredUploadsAllowed(), expectSend);

        const Record saved = reporting_consent_store::read();
        QVERIFY(saved.recorded);
        QCOMPARE(saved.send, expectSend);
        QCOMPARE(saved.noticeVersion, versionTwo);

        // A new process over the saved value does not ask a second time.
        ReportingConsent::setStateForTests(false, Record{});
        ReportingConsent::loadFromStore();
        QVERIFY(!ReportingConsent::needsAsk(versionTwo));
        QCOMPARE(ReportingConsent::coveredUploadsAllowed(), expectSend);
#else
        QSKIP("the notice window is Windows-only");
#endif
    }

    // The app's own re-ask entry point hands the change list to that same window.
    void askAtStartupRendersTheChangeListItIsGiven()
    {
#ifdef _WIN32
        QVERIFY(reporting_consent_store::write(false, 0));
        ReportingConsent::loadFromStore();

        NoticeDrive drive;
        const bool answered = withCustomer(
            &drive, [] { return ReportingConsent::askAtStartup(kSyntheticChanges, kSyntheticChangeCount); });

        checkRenderedChangeList(drive);
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(!drive.boxBefore); // the previous "do not send" is what the box starts at
        QVERIFY(answered);
        const Record saved = reporting_consent_store::read();
        QVERIFY(saved.recorded);
        QVERIFY(!saved.send);
        QCOMPARE(saved.noticeVersion, reporting_consent_store::kCurrentNoticeVersion);
#else
        QSKIP("the notice window is Windows-only");
#endif
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

    // T083: every user-visible string of the RentaPC setup file (version-resource fields, window
    // copy, error dialogs) matches the approved copy, and docs/bootstrapper-visible-audit.md lists
    // each one. A new or edited literal that the matrix does not know fails here.
    void bootstrapperVisibleStringsMatchApprovedInventoryAndCopy()
    {
        const QString root = forkRoot();
        QVERIFY2(!root.isEmpty(), "installer/bootstrapper/bootstrapper.rc not found above the test exe");

        const QString rcRel = QStringLiteral("installer/bootstrapper/bootstrapper.rc");
        const QString mainRel = QStringLiteral("installer/bootstrapper/main.cpp");
        const QString storeRel = QStringLiteral("app/seathub/reporting_consent_store.h");
        const QString noticeRel = QStringLiteral("installer/PRIVACY-NOTICE.txt");
        const QString packRel = QStringLiteral("scripts/pack-seathub.ps1");
        const QString matrixRel = QStringLiteral("docs/bootstrapper-visible-audit.md");

        QString rc, mainCpp, storeH, notice, versionTxt, versionH, packPs1, matrixDoc;
        QVERIFY2(readText(root, rcRel, &rc), "cannot read bootstrapper.rc");
        QVERIFY2(readText(root, mainRel, &mainCpp), "cannot read bootstrapper main.cpp");
        QVERIFY2(readText(root, storeRel, &storeH), "cannot read reporting_consent_store.h");
        QVERIFY2(readText(root, noticeRel, &notice), "cannot read PRIVACY-NOTICE.txt");
        QVERIFY2(readText(root, QStringLiteral("app/version.txt"), &versionTxt), "cannot read app/version.txt");
        QVERIFY2(readText(root, QStringLiteral("app/seathub/seathub_version.h"), &versionH),
                 "cannot read seathub_version.h");
        QVERIFY2(readText(root, packRel, &packPs1), "cannot read pack-seathub.ps1");
        QVERIFY2(readText(root, matrixRel, &matrixDoc), "docs/bootstrapper-visible-audit.md is missing");

        // ---- Parse the matrix: rows are table lines whose first cell is B<digits>. ----
        QVector<AuditRow> rows;
        QSet<QString> ids;
        static const QRegularExpression idPattern(QStringLiteral("^B\\d{2,}$"));
        for (const QString& line : matrixDoc.split(QLatin1Char('\n'))) {
            const QString t = line.trimmed();
            if (!t.startsWith(QStringLiteral("| B"))) {
                continue;
            }
            const QStringList cells = t.split(QLatin1Char('|'));
            QVERIFY2(cells.size() == 8, qPrintable(QStringLiteral("malformed matrix row: ") + t));
            AuditRow r;
            r.id = unwrapCell(cells[1]);
            r.kind = unwrapCell(cells[2]);
            r.source = unwrapCell(cells[3]);
            r.key = unwrapCell(cells[4]);
            r.expected = unwrapCell(cells[5]);
            r.authority = unwrapCell(cells[6]);
            QVERIFY2(idPattern.match(r.id).hasMatch(), qPrintable(QStringLiteral("bad matrix row id: ") + r.id));
            QVERIFY2(!ids.contains(r.id), qPrintable(QStringLiteral("duplicate matrix row id: ") + r.id));
            ids.insert(r.id);
            QVERIFY2(!r.authority.isEmpty(), qPrintable(QStringLiteral("matrix row has no authority: ") + r.id));
            QVERIFY2(!r.expected.isEmpty() || r.kind == QLatin1String("vendor"),
                     qPrintable(QStringLiteral("matrix row has no expected value: ") + r.id));
            rows.append(r);
        }
        QVERIFY2(!rows.isEmpty(), "the audit matrix has no B<nn> rows");
        QVERIFY2(matrixDoc.contains(QStringLiteral("T030")) && matrixDoc.contains(QStringLiteral("FileVersionInfo")),
                 "the audit must carry the T030 published-file readback procedure");

        const auto hasRow = [&](const char* kind, const QString& source, const QString& expected) {
            for (const AuditRow& r : rows) {
                if (r.kind == QLatin1String(kind) && r.source == source && r.expected == expected) {
                    return true;
                }
            }
            return false;
        };

        // ---- Version resource: every field, pinned here and listed in the matrix. ----
        const QStringList approvedRc = {
            QStringLiteral("101 RCDATA \"../PRIVACY-NOTICE.txt\""),
            QStringLiteral("102 RCDATA \"inner-setup.bin\""),
            QStringLiteral("FILEVERSION RENTAPC_VER_NUMBERS"),
            QStringLiteral("PRODUCTVERSION RENTAPC_VER_NUMBERS"),
            QStringLiteral("BLOCK \"StringFileInfo\""),
            QStringLiteral("BLOCK \"040904b0\""),
            QStringLiteral("VALUE \"CompanyName\", \"RentaPC\""),
            QStringLiteral("VALUE \"FileDescription\", \"RentaPC\""),
            QStringLiteral("VALUE \"FileVersion\", RENTAPC_VER_TEXT"),
            QStringLiteral("VALUE \"InternalName\", \"RentaPC-Setup\""),
            QStringLiteral("VALUE \"OriginalFilename\", \"RentaPC-Setup.exe\""),
            QStringLiteral("VALUE \"ProductName\", \"RentaPC\""),
            QStringLiteral("VALUE \"ProductVersion\", RENTAPC_VER_TEXT"),
            QStringLiteral("BLOCK \"VarFileInfo\""),
            QStringLiteral("VALUE \"Translation\", 0x409, 1200"),
        };
        QStringList rcLines;
        for (const QString& line : rc.split(QLatin1Char('\n'))) {
            rcLines.append(line.trimmed());
        }
        for (const QString& approved : approvedRc) {
            QVERIFY2(rcLines.contains(approved), qPrintable(QStringLiteral("bootstrapper.rc lacks approved line: ") + approved));
            QVERIFY2(hasRow("line", rcRel, approved), qPrintable(QStringLiteral("matrix lacks rc line: ") + approved));
        }
        static const QRegularExpression rcVisible(
            QStringLiteral("^(VALUE |BLOCK |FILEVERSION|PRODUCTVERSION|\\d+ RCDATA)"));
        for (const QString& line : rcLines) {
            if (rcVisible.match(line).hasMatch()) {
                QVERIFY2(hasRow("line", rcRel, line), qPrintable(QStringLiteral("rc line not in the matrix: ") + line));
            }
        }
        for (const AuditRow& r : rows) {
            if (r.kind == QLatin1String("line")) {
                QVERIFY2(r.source == rcRel && rcLines.contains(r.expected),
                         qPrintable(QStringLiteral("matrix line row not in bootstrapper.rc: ") + r.id));
            }
        }

        // ---- Numeric and text version macros come from the packer, from the single version file. ----
        const QString version = versionTxt.trimmed();
        static const QRegularExpression versionPattern(QStringLiteral("^(\\d+)\\.(\\d+)\\.(\\d+)$"));
        QVERIFY2(versionPattern.match(version).hasMatch(), "app/version.txt must be maj.min.patch");
        static const QRegularExpression headerVersion(QStringLiteral("#define\\s+SEATHUB_VERSION\\s+\"([^\"]+)\""));
        QCOMPARE(headerVersion.match(versionH).captured(1), version);
        const QStringList packPins = {
            QStringLiteral("$versionNumbers = \"$($Matches[1]),$($Matches[2]),$($Matches[3]),0\""),
            QStringLiteral("#define RENTAPC_VER_NUMBERS $versionNumbers"),
            QStringLiteral("#define RENTAPC_VER_TEXT \"\"$Version\"\"\""),
        };
        for (const QString& pin : packPins) {
            QVERIFY2(packPs1.contains(pin), qPrintable(QStringLiteral("pack-seathub.ps1 lacks: ") + pin));
            QVERIFY2(hasRow("text", packRel, pin), qPrintable(QStringLiteral("matrix lacks packer rule: ") + pin));
        }
        for (const AuditRow& r : rows) {
            if (r.kind == QLatin1String("text")) {
                QString body;
                QVERIFY2(readText(root, r.source, &body), qPrintable(QStringLiteral("cannot read ") + r.source));
                QVERIFY2(body.contains(r.expected), qPrintable(QStringLiteral("matrix text row not in source: ") + r.id));
            }
        }

        // ---- Census of every string literal in the two window sources. ----
        const AuditScan mainScan = scanCpp(mainCpp);
        const AuditScan storeScan = scanCpp(storeH);
        const QMap<QString, const AuditScan*> scans{ { mainRel, &mainScan }, { storeRel, &storeScan } };

        // Every MessageBoxW call: literal title and body, one matrix row each.
        static const QRegularExpression messageBox(
            QStringLiteral("MessageBoxW\\(\\s*nullptr\\s*,\\s*@LIT(\\d+)@\\s*,\\s*@LIT(\\d+)@\\s*,"));
        QStringList calls;
        auto it = messageBox.globalMatch(mainScan.code);
        while (it.hasNext()) {
            const auto m = it.next();
            calls.append(mainScan.literals[m.captured(2).toInt()].raw + QChar(0x1f)
                         + mainScan.literals[m.captured(1).toInt()].raw);
        }
        QCOMPARE(calls.size(), static_cast<int>(mainScan.code.count(QStringLiteral("MessageBoxW("))));
        QStringList boxRows;
        for (const AuditRow& r : rows) {
            if (r.kind == QLatin1String("messagebox")) {
                QCOMPARE(r.source, mainRel);
                boxRows.append(r.key + QChar(0x1f) + r.expected);
            }
        }
        calls.sort();
        boxRows.sort();
        QCOMPARE(boxRows, calls);
        const QStringList approvedBoxes = {
            QStringLiteral("RentaPC setup") + QChar(0x1f) + QStringLiteral("RentaPC setup could not unpack its installer."),
            QStringLiteral("RentaPC setup") + QChar(0x1f) + QStringLiteral("RentaPC setup could not start its installer."),
            QStringLiteral("RentaPC setup") + QChar(0x1f)
                + QStringLiteral("RentaPC setup could not save your choice, so it did not install."),
        };
        for (const QString& box : approvedBoxes) {
            QVERIFY2(calls.contains(box), qPrintable(QStringLiteral("approved error dialog missing from main.cpp: ") + box));
        }

        const QString classifiedKinds = QStringLiteral("literal classified");
        for (auto s = scans.constBegin(); s != scans.constEnd(); ++s) {
            QSet<QString> have;
            for (const AuditLiteral& lit : s.value()->literals) {
                QVERIFY2(lit.wide, qPrintable(s.key() + QStringLiteral(": narrow string literal: ") + lit.raw));
                if (!lit.raw.isEmpty()) { // empty defaults draw nothing
                    have.insert(lit.raw);
                }
            }
            QSet<QString> covered;
            for (const AuditRow& r : rows) {
                if (r.source != s.key()) {
                    continue;
                }
                if (classifiedKinds.split(QLatin1Char(' ')).contains(r.kind)) {
                    covered.insert(r.expected);
                    QVERIFY2(have.contains(r.expected),
                             qPrintable(QStringLiteral("matrix row ") + r.id + QStringLiteral(" is not a literal of ") + s.key()));
                } else if (r.kind == QLatin1String("messagebox")) {
                    covered.insert(r.key);
                    covered.insert(r.expected);
                }
            }
            for (const QString& literal : have) {
                QVERIFY2(covered.contains(literal),
                         qPrintable(s.key() + QStringLiteral(": literal missing from the audit matrix: ") + literal));
            }
        }

        // ---- Approved ASCII window copy, pinned here so the matrix is never the only authority. ----
        const QStringList approvedWindow = {
            QStringLiteral("send crash reports, logs and session evidence"),
            QStringLiteral("We can't give proper support for errors with this turned off."),
            QStringLiteral("What we collect"),
            QStringLiteral("What changed"),
            QStringLiteral("https://rentapc.online/data-collection"),
        };
        QSet<QString> storeHave;
        for (const AuditLiteral& lit : storeScan.literals) {
            storeHave.insert(lit.raw);
        }
        for (const QString& copy : approvedWindow) {
            QVERIFY2(storeHave.contains(copy), qPrintable(QStringLiteral("window copy missing from the store header: ") + copy));
            QVERIFY2(hasRow("literal", storeRel, copy), qPrintable(QStringLiteral("matrix lacks window copy: ") + copy));
        }
        bool titleFound = false;
        for (const QString& raw : storeHave) {
            titleFound = titleFound || raw.startsWith(QStringLiteral("RentaPC setup  /  "));
        }
        QVERIFY2(titleFound, "window title must start with \"RentaPC setup  /  \" (copy.md Setup window)");
        QSet<QString> mainHave;
        for (const AuditLiteral& lit : mainScan.literals) {
            mainHave.insert(lit.raw);
        }
        for (const QString& button : { QStringLiteral("Install"), QStringLiteral("Cancel") }) {
            QVERIFY2(mainHave.contains(button), qPrintable(QStringLiteral("main.cpp lacks the button: ") + button));
            QVERIFY2(hasRow("literal", mainRel, button), qPrintable(QStringLiteral("matrix lacks the button: ") + button));
        }

        // ---- The notice in the window is the first two paragraphs of PRIVACY-NOTICE.txt. ----
        const QStringList paragraphs = notice.split(QRegularExpression(QStringLiteral("\n\\s*\n")));
        QVERIFY2(paragraphs.size() >= 2, "PRIVACY-NOTICE.txt must have an English and an Arabic paragraph");
        const QString noticeEn = paragraphs[0].simplified();
        const QString noticeAr = paragraphs[1].simplified();
        bool enFound = false;
        bool arFound = false;
        for (const AuditLiteral& lit : storeScan.literals) {
            enFound = enFound || lit.raw.simplified() == noticeEn;
            arFound = arFound || lit.raw.simplified() == noticeAr;
        }
        QVERIFY2(enFound, "the English window notice differs from PRIVACY-NOTICE.txt paragraph 1");
        QVERIFY2(arFound, "the Arabic window notice differs from PRIVACY-NOTICE.txt paragraph 2");
        QVERIFY2(hasRow("literal", storeRel, noticeEn), "matrix lacks the English notice");
        QVERIFY2(notice.contains(QStringLiteral("https://rentapc.online/data-collection")),
                 "PRIVACY-NOTICE.txt lost the data-collection URL");
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
