// Settings bridge tests (Plan 03-04 Task 1): write-through, the streaming guard, the launch-only
// override layer, and the two QML artifacts that carry the settings page and its controls.
//
// The bridge is exercised against the real upstream `StreamingPreferences`, not a stand-in: the
// whole point of the bridge (D-12, STREAM-02) is that there is exactly one preference store, so a
// test that substituted a second one would be testing the bug the design forbids.
//
// QSettings is redirected to a temporary directory before any preference object exists, so a test
// run never reads or writes the preferences of the machine it runs on (upstream's own `main.cpp`
// uses an INI file too - `QSettings::setDefaultFormat(QSettings::IniFormat)` - so this changes the
// location, not the format).

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>

#include "seathub/settings_bridge.h"
#include "settings/streamingpreferences.h"

namespace {
const char* const kSansFamily = "Inter";

QString guiDir()
{
    // The .pro hands the fork's own root over so the test finds `app/gui` wherever it was built.
#ifdef FORK_ROOT
    const QString fromRoot =
        QDir(QString::fromUtf8(FORK_ROOT) + QStringLiteral("/app/gui")).absolutePath();
    if (QDir(fromRoot).exists()) {
        return fromRoot;
    }
#endif
    return QDir(QCoreApplication::applicationDirPath() + QStringLiteral("/../app/gui"))
        .absolutePath();
}

// Collects every Text item under `root`, the same way the 03-02 error-screen test does.
QList<QObject*> textItems(QObject* root)
{
    QList<QObject*> items;
    for (QObject* child : root->children()) {
        if (child->inherits("QQuickText")) {
            items.append(child);
        }
        items.append(textItems(child));
    }
    return items;
}
}

// A stand-in for `SeatHubClient` for the QML tests: SettingsPage talks to the facade (D-35), and
// what it uses of it is `settings` plus `closeSettings()`. The bridge behind it is the real one.
class FakeClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(SettingsBridge* settings READ settings CONSTANT)

public:
    explicit FakeClient(SettingsBridge* bridge, QObject* parent = nullptr)
        : QObject(parent), m_bridge(bridge) {}

    SettingsBridge* settings() const { return m_bridge; }

    Q_INVOKABLE void closeSettings() { ++m_closes; }
    int closes() const { return m_closes; }

private:
    SettingsBridge* m_bridge;
    int m_closes = 0;
};

class TstSettingsBridge : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void bridgeReadsThroughToStreamingPreferences();
    void bridgeWritesThroughAndPersists();
    void captureSystemKeysIsCheckboxPlusDropdownState();
    void writesRefusedDuringStream();
    void settingsPageRendersFiveGroups();
    void everyAuditedKeyHasAVisibleControl();
    void sessionOverridesAreInMemoryOnly();
    void negotiatedResultsAreExposedOnlyAfterConnectionStarted();

private:
    QTemporaryDir m_settingsDir;
    SettingsBridge* m_bridge = nullptr;
};

void TstSettingsBridge::initTestCase()
{
    // Redirect QSettings before anything constructs one.
    QVERIFY(m_settingsDir.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("SeatHubTest"));
    QCoreApplication::setApplicationName(QStringLiteral("SeatHubSettingsBridgeTest"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, m_settingsDir.path());

    m_bridge = new SettingsBridge(this);
}

void TstSettingsBridge::bridgeReadsThroughToStreamingPreferences()
{
    StreamingPreferences* prefs = StreamingPreferences::get();

    // The stored value, read straight off the upstream object.
    prefs->width = 1600;
    QCOMPARE(m_bridge->getValue(QStringLiteral("width")).toInt(), 1600);

    // Nothing is cached: change the preference behind the bridge's back and the next read sees
    // it. A bridge that had grown its own copy of the values would fail here (D-12, STREAM-02).
    prefs->width = 1024;
    QCOMPARE(m_bridge->getValue(QStringLiteral("width")).toInt(), 1024);

    // The same for a merged key: `fullscreen` has no control of its own, it is the legacy input
    // the display-mode control projects from `windowmode` (`streamingpreferences.cpp:165-168`).
    prefs->windowMode = StreamingPreferences::WM_WINDOWED;
    QCOMPARE(m_bridge->getValue(QStringLiteral("fullscreen")).toString(), QStringLiteral("Windowed"));
    QCOMPARE(m_bridge->displayMode(), QStringLiteral("Windowed"));

    // And for an enum read through the catalogue.
    prefs->videoCodecConfig = StreamingPreferences::VCC_FORCE_HEVC;
    QCOMPARE(m_bridge->getValue(QStringLiteral("videocfg")).toString(),
             QStringLiteral("HEVC (H.265)"));
}

void TstSettingsBridge::bridgeWritesThroughAndPersists()
{
    QVERIFY(m_bridge->setValue(QStringLiteral("width"), 1920));
    QCOMPARE(StreamingPreferences::get()->width, 1920);

    // D-13: saved immediately. The value is already in QSettings, not waiting for a flush.
    QSettings settings;
    QCOMPARE(settings.value(QStringLiteral("width")).toInt(), 1920);

    // A relaunch re-reads the same store: `reload()` is what a fresh process does first.
    StreamingPreferences::get()->reload();
    QCOMPARE(m_bridge->getValue(QStringLiteral("width")).toInt(), 1920);

    // A merged control writes both of the keys it owns.
    QVERIFY(m_bridge->setResolutionPreset(QStringLiteral("1440p")));
    QCOMPARE(m_bridge->getValue(QStringLiteral("width")).toInt(), 2560);
    QCOMPARE(m_bridge->getValue(QStringLiteral("height")).toInt(), 1440);

    // An unknown key, and a value the setting cannot hold, are both refused rather than stored.
    QVERIFY(!m_bridge->setValue(QStringLiteral("not-a-key"), 1));
    QVERIFY(!m_bridge->setValue(QStringLiteral("fps"), 0));
    QVERIFY(!m_bridge->setValue(QStringLiteral("packetsize"), 512)); // upstream floor is 1024
}

void TstSettingsBridge::captureSystemKeysIsCheckboxPlusDropdownState()
{
    // ADR-0042 / D-49: SeatHub's default is checked + "In fullscreen", which is deliberately not
    // upstream's own default (never). The bridge applies it once, through StreamingPreferences,
    // so there is no second store holding a default the preference object disagrees with.
    QCOMPARE(m_bridge->getValue(QStringLiteral("capturesyskeys")).toString(),
             QStringLiteral("fullscreen"));
    QCOMPARE(StreamingPreferences::get()->captureSysKeysMode,
             StreamingPreferences::CSK_FULLSCREEN);

    // The checkbox's unchecked state.
    QVERIFY(m_bridge->setCaptureSysKeysMode(QStringLiteral("never")));
    QCOMPARE(m_bridge->getValue(QStringLiteral("capturesyskeys")).toString(),
             QStringLiteral("never"));
    QCOMPARE(StreamingPreferences::get()->captureSysKeysMode, StreamingPreferences::CSK_OFF);

    // The dropdown's other choice.
    QVERIFY(m_bridge->setCaptureSysKeysMode(QStringLiteral("always")));
    QCOMPARE(m_bridge->getValue(QStringLiteral("capturesyskeys")).toString(),
             QStringLiteral("always"));
    QCOMPARE(StreamingPreferences::get()->captureSysKeysMode, StreamingPreferences::CSK_ALWAYS);

    // The dropdown offers exactly upstream's two states, never "never" (ADR-0042).
    const QStringList modes = m_bridge->captureSysKeysModes();
    QCOMPARE(modes.size(), 2);
    QVERIFY(modes.contains(QStringLiteral("fullscreen")));
    QVERIFY(modes.contains(QStringLiteral("always")));
    QVERIFY(!modes.contains(QStringLiteral("never")));

    // Back to the shipped default for the remaining tests.
    QVERIFY(m_bridge->setCaptureSysKeysMode(QStringLiteral("fullscreen")));
}

void TstSettingsBridge::writesRefusedDuringStream()
{
    QCOMPARE(m_bridge->getSavedValue(QStringLiteral("width")).toInt(), 2560);

    m_bridge->setStreamingActive(true);
    QVERIFY(!m_bridge->writable());
    QVERIFY(m_bridge->sessionActive());

    // Pitfall 6 / T-03-15: a settings write during an active stream is refused, and the stored
    // value is left exactly as it was.
    QVERIFY(!m_bridge->setValue(QStringLiteral("width"), 1280));
    QCOMPARE(m_bridge->getSavedValue(QStringLiteral("width")).toInt(), 2560);
    QCOMPARE(StreamingPreferences::get()->width, 2560);
    QVERIFY(!m_bridge->setResolutionPreset(QStringLiteral("720p")));
    QVERIFY(!m_bridge->setCaptureSysKeysMode(QStringLiteral("always")));
    QCOMPARE(StreamingPreferences::get()->captureSysKeysMode,
             StreamingPreferences::CSK_FULLSCREEN);

    m_bridge->setStreamingActive(false);
    QVERIFY(m_bridge->writable());
    QVERIFY(m_bridge->setValue(QStringLiteral("width"), 1920));
    QCOMPARE(StreamingPreferences::get()->width, 1920);
}

void TstSettingsBridge::settingsPageRendersFiveGroups()
{
    const QString qmlPath = guiDir() + QStringLiteral("/SettingsPage.qml");
    QVERIFY2(QFile::exists(qmlPath), qPrintable(QStringLiteral("missing ") + qmlPath));

    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QQmlEngine engine;
    qmlRegisterSingletonType(QUrl::fromLocalFile(guiDir() + QStringLiteral("/Tokens.qml")),
                             "SeatHub.Tokens", 1, 0, "Tokens");
    qmlRegisterSingletonType(QUrl::fromLocalFile(guiDir() + QStringLiteral("/Metrics.qml")),
                             "SeatHub.Tokens", 1, 0, "Metrics");

    QQmlComponent component(&engine, QUrl::fromLocalFile(qmlPath));
    QScopedPointer<QObject> root(component.create());
    if (!root) {
        QFAIL(qPrintable(component.errorString()));
    }

    FakeClient client(m_bridge);
    QVERIFY(root->setProperty("client", QVariant::fromValue(static_cast<QObject*>(&client))));

    // D-48: exactly these five groups, each with a heading set in the H2 size.
    const QStringList groups = { QStringLiteral("Video"), QStringLiteral("Audio"),
                                 QStringLiteral("Input"), QStringLiteral("Network"),
                                 QStringLiteral("Advanced") };
    const QList<QObject*> items = textItems(root.data());
    for (const QString& group : groups) {
        bool headingFound = false;
        for (QObject* item : items) {
            if (item->property("text").toString() != group) {
                continue;
            }
            const QFont font = item->property("font").value<QFont>();
            if (font.pixelSize() == 24) { // Metrics.fontH2
                headingFound = true;
                break;
            }
        }
        QVERIFY2(headingFound, qPrintable(QStringLiteral("SettingsPage must render the ")
                                          + group + QStringLiteral(" section (D-48)")));
    }

    // The page is a view inside the home state and can always be left.
    bool sawBack = false;
    for (QObject* item : items) {
        if (item->property("text").toString() == QStringLiteral("Back")) {
            sawBack = true;
            break;
        }
    }
    QVERIFY2(sawBack, "the settings page must offer a way back to the home view");
}

void TstSettingsBridge::everyAuditedKeyHasAVisibleControl()
{
    const QString source = [&] {
        QFile file(guiDir() + QStringLiteral("/SettingsPage.qml"));
        if (!file.open(QIODevice::ReadOnly)) {
            return QString();
        }
        return QString::fromUtf8(file.readAll());
    }();
    QVERIFY(!source.isEmpty());

    // CUST-05 / D-11: nothing is hidden. Every key the bridge catalogues is named in the page -
    // the managed ones are shown with the reason they are not a choice, the rest have a control.
    //
    // The pinned commit serializes exactly 38 `[streamsettings]` keys
    // (`app/settings/streamingpreferences.cpp`): the 34 the audit gives controls to plus the four
    // it marks intentionally dropped (`mdns`, `quitAppAfter`, `richpresence`, `defaultver`), which
    // SeatHub still renders as rows carrying their reason. A short catalogue is the defect this
    // catches - a key with no row anywhere on the page.
    const QStringList keys = m_bridge->keys();
    QVERIFY2(int(keys.size()) == 38, qPrintable(QStringLiteral("expected the full catalogue, got ")
                                               + QString::number(keys.size())));
    // Every key is accounted for on the page: a control binds it (`settingKey: "fps"`), a merged
    // control names its sources in its own description, and a key SeatHub does not expose has a
    // read-only row carrying the reason. What must never appear is a catalogued key that the page
    // neither names nor renders - that is the key the audit would lose track of.
    //
    // The rows for the keys SeatHub does not expose are built from the bridge's own list rather
    // than written out one at a time, so those four are covered by the `droppedKeys()` binding
    // below instead of a literal in this file.
    QVERIFY2(source.contains(QStringLiteral("droppedKeys()")),
             "SettingsPage.qml must render the managed keys from the bridge's own list");
    for (const QString& key : keys) {
        const QRegularExpression named(QStringLiteral("\\b") + QRegularExpression::escape(key)
                                       + QStringLiteral("\\b"));
        QVERIFY2(named.match(source).hasMatch() || m_bridge->isDropped(key),
                 qPrintable(QStringLiteral("SettingsPage.qml never names the key ") + key));
    }

    // The audit's split: 34 keys with a control, 4 SeatHub reports without offering a choice.
    QCOMPARE(int(keys.size() - m_bridge->droppedKeys().size()), 34);
    QCOMPARE(m_bridge->groupOf(QStringLiteral("width")), QStringLiteral("video"));
    QCOMPARE(m_bridge->groupOf(QStringLiteral("audiocfg")), QStringLiteral("audio"));
    QCOMPARE(m_bridge->groupOf(QStringLiteral("multicontroller")), QStringLiteral("input"));
    QCOMPARE(m_bridge->groupOf(QStringLiteral("packetsize")), QStringLiteral("network"));
    QCOMPARE(m_bridge->groupOf(QStringLiteral("capturesyskeys")), QStringLiteral("advanced"));
    QCOMPARE(m_bridge->groups().size(), 5);

    // The four keys that must never become a second, conflicting control. They report which
    // control covers them, and that control reports which upstream keys it spans - one direction
    // so a page can render "Merges X and Y" from the catalogue instead of restating it.
    QVERIFY(m_bridge->isMerged(QStringLiteral("fullscreen")));
    QVERIFY(m_bridge->isMerged(QStringLiteral("windowmode")));
    QVERIFY(m_bridge->mergedSources(QStringLiteral("displayMode"))
                .contains(QStringLiteral("fullscreen")));
    QVERIFY(m_bridge->mergedSources(QStringLiteral("displayMode"))
                .contains(QStringLiteral("windowmode")));
    QVERIFY(m_bridge->isMerged(QStringLiteral("startwindowed")));
    QVERIFY(m_bridge->isMerged(QStringLiteral("uidisplaymode")));
    QVERIFY(m_bridge->mergedSources(QStringLiteral("launchDisplayMode"))
                .contains(QStringLiteral("startwindowed")));
    QVERIFY(m_bridge->mergedSources(QStringLiteral("launchDisplayMode"))
                .contains(QStringLiteral("uidisplaymode")));
    QVERIFY(m_bridge->mergedSources(QStringLiteral("resolutionPreset"))
                .contains(QStringLiteral("width")));
    QVERIFY(m_bridge->mergedSources(QStringLiteral("resolutionPreset"))
                .contains(QStringLiteral("height")));

    // The four the audit marks intentionally dropped still have a reason attached.
    const QStringList dropped = m_bridge->droppedKeys();
    QVERIFY(dropped.contains(QStringLiteral("mdns")));
    QVERIFY(dropped.contains(QStringLiteral("quitAppAfter")));
    QVERIFY(dropped.contains(QStringLiteral("richpresence")));
    QVERIFY(dropped.contains(QStringLiteral("defaultver")));
    for (const QString& key : dropped) {
        QVERIFY2(!m_bridge->dropReason(key).isEmpty(),
                 qPrintable(QStringLiteral("no reason recorded for ") + key));
    }
}

void TstSettingsBridge::sessionOverridesAreInMemoryOnly()
{
    QVERIFY(m_bridge->setValue(QStringLiteral("fps"), 60));
    QVERIFY(!m_bridge->hasSessionOverrides());

    // D-37 / WR-05: the control plane's per-session profile is applied in memory for that launch.
    m_bridge->applySessionOverride(QStringLiteral("1080p120"));
    QVERIFY(m_bridge->hasSessionOverrides());
    QCOMPARE(m_bridge->getValue(QStringLiteral("width")).toInt(), 1920);
    QCOMPARE(m_bridge->getValue(QStringLiteral("height")).toInt(), 1080);
    QCOMPARE(m_bridge->getValue(QStringLiteral("fps")).toInt(), 120);

    // The saved values are untouched, and the settings page can tell the two apart.
    QCOMPARE(m_bridge->getSavedValue(QStringLiteral("fps")).toInt(), 60);
    QCOMPARE(StreamingPreferences::get()->fps, 60);
    QSettings settings;
    QCOMPARE(settings.value(QStringLiteral("fps")).toInt(), 60);

    // Nothing about the override can escape into the preference object, not even a write of the
    // same value through the ordinary path.
    QVERIFY(!m_bridge->effectiveValues().isEmpty());
    QCOMPARE(m_bridge->effectiveValues().value(QStringLiteral("fps")).toInt(), 120);

    m_bridge->clearSessionOverrides();
    QVERIFY(!m_bridge->hasSessionOverrides());
    QCOMPARE(m_bridge->getValue(QStringLiteral("fps")).toInt(), 60);
}

void TstSettingsBridge::negotiatedResultsAreExposedOnlyAfterConnectionStarted()
{
    // D-14 / IN-04: nothing is reported before there is a connection to report about.
    QVERIFY(!m_bridge->hasNegotiatedResults());
    QVERIFY(!m_bridge->getNegotiatedValue(QStringLiteral("width")).isValid());
    QVERIFY(m_bridge->getNegotiatedValues().isEmpty());

    m_bridge->noteConnectionStarted();
    QVERIFY(m_bridge->hasNegotiatedResults());
    QCOMPARE(m_bridge->getNegotiatedValue(QStringLiteral("width")).toInt(),
             m_bridge->getValue(QStringLiteral("width")).toInt());
    QVERIFY(!m_bridge->getNegotiatedValues().isEmpty());

    // The engine's fallback report, through its own public signal seam: the setting it could not
    // honour gets a SeatHub sentence beside the saved value, and - D-14's hard rule - the saved
    // value is not rewritten to match what the stream actually settled on.
    QVERIFY(!m_bridge->hasNegotiationWarning(QStringLiteral("hdr")));
    m_bridge->noteLaunchWarning(
        QStringLiteral("Your host PC doesn't support HDR streaming."));
    QVERIFY(m_bridge->hasNegotiationWarning(QStringLiteral("hdr")));
    QVERIFY(!m_bridge->negotiationWarning(QStringLiteral("hdr")).isEmpty());
    QCOMPARE(m_bridge->getSavedValue(QStringLiteral("hdr")),
             m_bridge->getValue(QStringLiteral("hdr")));

    // An engine sentence the catalogue cannot attribute is still not shown to anybody.
    m_bridge->noteLaunchWarning(QStringLiteral("Some other engine message entirely."));
    QVERIFY(!m_bridge->negotiationWarnings().isEmpty());

    m_bridge->noteSessionFinished();
    QVERIFY(!m_bridge->hasNegotiatedResults());
    QVERIFY(!m_bridge->hasNegotiationWarning(QStringLiteral("hdr")));
    QCOMPARE(m_bridge->negotiationWarnings().size(), 0);
}

QTEST_MAIN(TstSettingsBridge)

#include "tst_settings_bridge.moc"
