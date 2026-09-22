// Settings bridge tests (Plan 05-11): the upstream-shaped catalogue (D-24), the forced values
// re-applied on every load (D-25/D-25a, T-05-45), the write-through and streaming guard carried
// over from Plan 03-04, and the per-value performance-stats toggles (D-23, CUST-17).
//
// The bridge is exercised against the real upstream `StreamingPreferences`, not a stand-in: the
// whole point of the bridge (D-12, STREAM-02) is that there is exactly one preference store, so a
// test that substituted a second one would be testing the bug the design forbids.
//
// QSettings is redirected to a temporary directory before any preference object exists, so a test
// run never reads or writes the preferences of the machine it runs on (upstream's own `main.cpp`
// uses an INI file too - `QSettings::setDefaultFormat(QSettings::IniFormat)` - so this changes the
// location, not the format).
//
// The QML page itself (`SettingsPage.qml`) is exercised in `tst_ui_screens.cpp` (Plan 05-11 Task
// 2) rather than here: this file is the bridge's own contract, checkable without a rebuilt page.

#include <QtTest>
#include <QSettings>
#include <QTemporaryDir>

#include "seathub/settings_bridge.h"
#include "settings/streamingpreferences.h"

class TstSettingsBridge : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void catalogueMatchesUpstreamInventoryRowForRow();
    void bridgeReadsThroughToStreamingPreferences();
    void bridgeWritesThroughAndPersists();
    void invertedCheckboxesReadAndWriteTheOppositeOfTheStoredKey();
    void captureSystemKeysIsCheckboxPlusDropdownState();
    void writesRefusedDuringStream();
    void forcedValuesAreCorrectedOnEveryLoadNotOnlyTheFirst();
    void hostSpeakerRowStaysEditableAtUpstreamsDefault();
    void statsTogglesDefaultOffAndTheDerivedOptionFollowsInBothDirections();
    void enabledStatsLabelsListsOnlyTheOnesTurnedOnInTheEnginesOwnOrder();
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
}

void TstSettingsBridge::init()
{
    // A fresh bridge (and a fresh constructor pass over applySeatHubDefaults/applyForcedValues/
    // recomputeShowPerfOverlay) for every test, so one test's writes cannot leak into another's
    // "on every load" assertion.
    delete m_bridge;
    m_bridge = new SettingsBridge(this);
}

void TstSettingsBridge::catalogueMatchesUpstreamInventoryRowForRow()
{
    // D-24: the seven upstream sections, in upstream's own two-column order.
    const QStringList groups = m_bridge->groups();
    QCOMPARE(groups, (QStringList{ QStringLiteral("basic"), QStringLiteral("audio"),
                                   QStringLiteral("host"), QStringLiteral("ui"),
                                   QStringLiteral("input"), QStringLiteral("gamepad"),
                                   QStringLiteral("advanced") }));
    QCOMPARE(m_bridge->groupTitle(QStringLiteral("basic")), QStringLiteral("Basic Settings"));
    QCOMPARE(m_bridge->groupTitle(QStringLiteral("audio")), QStringLiteral("Audio Settings"));
    QCOMPARE(m_bridge->groupTitle(QStringLiteral("host")), QStringLiteral("Host Settings"));
    QCOMPARE(m_bridge->groupTitle(QStringLiteral("ui")), QStringLiteral("UI Settings"));
    QCOMPARE(m_bridge->groupTitle(QStringLiteral("input")), QStringLiteral("Input Settings"));
    QCOMPARE(m_bridge->groupTitle(QStringLiteral("gamepad")), QStringLiteral("Gamepad Settings"));
    QCOMPARE(m_bridge->groupTitle(QStringLiteral("advanced")),
             QStringLiteral("Advanced Settings"));

    // D-11/D-47: the full 38-key completeness baseline is unchanged by the re-grouping - nothing
    // was added or removed from the catalogue, only re-sectioned, re-labelled and re-flagged.
    QCOMPARE(m_bridge->keys().size(), 38);

    // Sampled labels, matching RESEARCH Q4's inventory verbatim (with "Moonlight" read as
    // "SeatHub" where upstream's own tooltip names it).
    QCOMPARE(m_bridge->labelOf(QStringLiteral("hostaudio")),
             QStringLiteral("Mute host PC speakers while streaming"));
    QCOMPARE(m_bridge->labelOf(QStringLiteral("muteonfocusloss")),
             QStringLiteral("Mute audio stream when SeatHub is not the active window"));
    QCOMPARE(m_bridge->labelOf(QStringLiteral("uidisplaymode")),
             QStringLiteral("GUI display mode"));
    QCOMPARE(m_bridge->labelOf(QStringLiteral("backgroundgamepad")),
             QStringLiteral("Process gamepad input when SeatHub is in the background"));
    QCOMPARE(m_bridge->labelOf(QStringLiteral("hdr")),
             QStringLiteral("Enable HDR (Experimental)"));

    // Sections moved to match upstream, not Phase 3's own D-48 grouping: capture-system-keys is
    // Input, keep-awake is UI (RESEARCH Q4).
    QCOMPARE(m_bridge->groupOf(QStringLiteral("capturesyskeys")), QStringLiteral("input"));
    QCOMPARE(m_bridge->groupOf(QStringLiteral("keepawake")), QStringLiteral("ui"));
    QCOMPARE(m_bridge->groupOf(QStringLiteral("hostaudio")), QStringLiteral("audio"));
    QCOMPARE(m_bridge->groupOf(QStringLiteral("quitAppAfter")), QStringLiteral("host"));

    // Merged controls: the resolution and frame-rate presets, same pattern as the display-mode
    // and launcher-display-mode merges already covered below.
    QVERIFY(m_bridge->isMerged(QStringLiteral("fps")));
    QVERIFY(m_bridge->mergedSources(QStringLiteral("frameRatePreset"))
                .contains(QStringLiteral("fps")));
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

    // Same pattern for the new frame-rate preset (D-26: derived from upstream's own two fixed
    // presets, 30 and 60, plus Custom for anything else).
    QCOMPARE(m_bridge->frameRatePresets(),
             (QStringList{ QStringLiteral("30 FPS"), QStringLiteral("60 FPS"),
                           QStringLiteral("Custom") }));
    QVERIFY(m_bridge->setFrameRatePreset(QStringLiteral("30 FPS")));
    QCOMPARE(m_bridge->getValue(QStringLiteral("fps")).toInt(), 30);
    QCOMPARE(m_bridge->frameRatePreset(), QStringLiteral("30 FPS"));
    QVERIFY(m_bridge->setValue(QStringLiteral("fps"), 45));
    QCOMPARE(m_bridge->frameRatePreset(), QStringLiteral("Custom"));

    // Bitrate's ceiling widens only while unlockbitrate is on (D-26; upstream's own Slider
    // bound), not a new value this bridge invents.
    QCOMPARE(m_bridge->bitrateMaximum(), 150000);
    QVERIFY(m_bridge->setValue(QStringLiteral("unlockbitrate"), true));
    QCOMPARE(m_bridge->bitrateMaximum(), 500000);
    QVERIFY(m_bridge->setValue(QStringLiteral("unlockbitrate"), false));
    QCOMPARE(m_bridge->bitrateMaximum(), 150000);

    // An unknown key, and a value the setting cannot hold, are both refused rather than stored.
    QVERIFY(!m_bridge->setValue(QStringLiteral("not-a-key"), 1));
    QVERIFY(!m_bridge->setValue(QStringLiteral("fps"), 0));
    QVERIFY(!m_bridge->setValue(QStringLiteral("packetsize"), 512)); // upstream floor is 1024
}

void TstSettingsBridge::invertedCheckboxesReadAndWriteTheOppositeOfTheStoredKey()
{
    // RESEARCH Q4: three upstream checkboxes are the logical NOT of the key they write. The
    // catalogue carries the inversion (advisor point 4) so no QML row has to know about it.
    StreamingPreferences* prefs = StreamingPreferences::get();

    prefs->playAudioOnHost = false;
    QCOMPARE(m_bridge->getValue(QStringLiteral("hostaudio")).toBool(), true); // shown checked
    QVERIFY(m_bridge->setValue(QStringLiteral("hostaudio"), false));           // unchecked
    QCOMPARE(prefs->playAudioOnHost, true);
    QVERIFY(m_bridge->setValue(QStringLiteral("hostaudio"), true));            // back to checked
    QCOMPARE(prefs->playAudioOnHost, false);

    prefs->absoluteTouchMode = true;
    QCOMPARE(m_bridge->getValue(QStringLiteral("abstouchmode")).toBool(), false);
    QVERIFY(m_bridge->setValue(QStringLiteral("abstouchmode"), true));
    QCOMPARE(prefs->absoluteTouchMode, false);

    prefs->multiController = true;
    QCOMPARE(m_bridge->getValue(QStringLiteral("multicontroller")).toBool(), false);
    QVERIFY(m_bridge->setValue(QStringLiteral("multicontroller"), true));
    QCOMPARE(prefs->multiController, false);
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
}

void TstSettingsBridge::writesRefusedDuringStream()
{
    QVERIFY(m_bridge->setValue(QStringLiteral("width"), 2560));

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

    m_bridge->setStreamingActive(false);
    QVERIFY(m_bridge->writable());
    QVERIFY(m_bridge->setValue(QStringLiteral("width"), 1920));
    QCOMPARE(StreamingPreferences::get()->width, 1920);
}

void TstSettingsBridge::forcedValuesAreCorrectedOnEveryLoadNotOnlyTheFirst()
{
    // D-25(b)/(c), T-05-45: exactly these five keys are forced, matching D-25a's amendment that
    // excludes `hostaudio`.
    QVERIFY(m_bridge->isForced(QStringLiteral("language")));
    QVERIFY(m_bridge->isForced(QStringLiteral("richpresence")));
    QVERIFY(m_bridge->isForced(QStringLiteral("mdns")));
    QVERIFY(m_bridge->isForced(QStringLiteral("detectnetblocking")));
    QVERIFY(m_bridge->isForced(QStringLiteral("quitAppAfter")));
    QVERIFY(!m_bridge->isForced(QStringLiteral("hostaudio")));
    QVERIFY(!m_bridge->isForced(QStringLiteral("packetsize")));
    QVERIFY(!m_bridge->isForced(QStringLiteral("defaultver")));
    QVERIFY(!m_bridge->isForced(QStringLiteral("showperfoverlay")));

    // A forced key refuses a direct write, unlike a merely-dropped one.
    QVERIFY(!m_bridge->setValue(QStringLiteral("language"), QStringLiteral("French")));

    // The construction pass already forced everything once; edit the store BY HAND, behind the
    // bridge's back (`StreamingPreferences` directly, the way a stray INI edit or an upstream
    // code path would), and prove a fresh "load" corrects it - this is the exact scenario T-05-45
    // names, and the exact call `SeatHubClient` makes before every connect attempt.
    StreamingPreferences* prefs = StreamingPreferences::get();
    prefs->language = StreamingPreferences::LANG_AUTO;
    prefs->richPresence = true;
    prefs->enableMdns = true;
    prefs->detectNetworkBlocking = true;
    prefs->quitAppAfter = true;
    prefs->save();

    QCOMPARE(prefs->language, StreamingPreferences::LANG_AUTO);

    m_bridge->prepareForSession();

    QCOMPARE(m_bridge->getValue(QStringLiteral("language")).toString(),
             QStringLiteral("English"));
    QCOMPARE(prefs->language, StreamingPreferences::LANG_EN);
    QCOMPARE(m_bridge->getValue(QStringLiteral("richpresence")).toBool(), false);
    QCOMPARE(m_bridge->getValue(QStringLiteral("mdns")).toBool(), false);
    QCOMPARE(m_bridge->getValue(QStringLiteral("detectnetblocking")).toBool(), false);
    QCOMPARE(m_bridge->getValue(QStringLiteral("quitAppAfter")).toBool(), false);

    // And the correction is actually persisted, not just held in memory - a fresh reload from
    // disk still reads the fixed value.
    prefs->reload();
    QCOMPARE(prefs->language, StreamingPreferences::LANG_EN);
    QCOMPARE(prefs->richPresence, false);
    QCOMPARE(prefs->enableMdns, false);
    QCOMPARE(prefs->detectNetworkBlocking, false);
    QCOMPARE(prefs->quitAppAfter, false);
}

void TstSettingsBridge::hostSpeakerRowStaysEditableAtUpstreamsDefault()
{
    // D-25a: NOT forced, and a hand-edit survives a fresh load exactly because it is not one of
    // the five corrected keys - the opposite of the previous test, on purpose.
    StreamingPreferences* prefs = StreamingPreferences::get();
    prefs->playAudioOnHost = false; // upstream default -> the row shows checked (muted)
    prefs->save();

    QCOMPARE(m_bridge->getValue(QStringLiteral("hostaudio")).toBool(), true);

    m_bridge->prepareForSession(); // a "load" moment - must NOT touch hostaudio
    QCOMPARE(prefs->playAudioOnHost, false);
    QCOMPARE(m_bridge->getValue(QStringLiteral("hostaudio")).toBool(), true);

    // The customer unticks it (unmutes the host), and that choice survives a "load" too.
    QVERIFY(m_bridge->setValue(QStringLiteral("hostaudio"), false));
    QCOMPARE(prefs->playAudioOnHost, true);
    m_bridge->prepareForSession();
    QCOMPARE(prefs->playAudioOnHost, true);
    QCOMPARE(m_bridge->getValue(QStringLiteral("hostaudio")).toBool(), false);
}

void TstSettingsBridge::statsTogglesDefaultOffAndTheDerivedOptionFollowsInBothDirections()
{
    // D-23/CUST-17/OD-03: one toggle per line, eleven of them, in `copy.md`'s own order.
    const QStringList keys = m_bridge->statsToggleKeys();
    QCOMPARE(keys.size(), 11);

    QCOMPARE(m_bridge->statsToggleLabel(keys.first()), QStringLiteral("Video stream"));
    QCOMPARE(m_bridge->statsToggleLabel(keys.last()),
             QStringLiteral("Average rendering time (including monitor V-sync latency)"));
    QVERIFY(keys.contains(QStringLiteral("statsNetworkLatency")));
    QCOMPARE(m_bridge->statsToggleLabel(QStringLiteral("statsNetworkLatency")),
             QStringLiteral("Average network latency"));

    // OD-03/OD-04: all off by default, and all-off is a valid state - the master option follows.
    for (const QString& key : keys) {
        QVERIFY2(!m_bridge->getStatsToggle(key), qPrintable(key));
    }
    QCOMPARE(m_bridge->getValue(QStringLiteral("showperfoverlay")).toBool(), false);

    // Turning any one on turns the derived master on (CUST-17: "on when any toggle is on").
    QVERIFY(m_bridge->setStatsToggle(QStringLiteral("statsNetworkLatency"), true));
    QCOMPARE(m_bridge->getStatsToggle(QStringLiteral("statsNetworkLatency")), true);
    QCOMPARE(m_bridge->getValue(QStringLiteral("showperfoverlay")).toBool(), true);

    // A second one on, master stays on.
    QVERIFY(m_bridge->setStatsToggle(QStringLiteral("statsDecodingFrameRate"), true));
    QCOMPARE(m_bridge->getValue(QStringLiteral("showperfoverlay")).toBool(), true);

    // Turning the first back off, with the second still on, leaves the master on.
    QVERIFY(m_bridge->setStatsToggle(QStringLiteral("statsNetworkLatency"), false));
    QCOMPARE(m_bridge->getValue(QStringLiteral("showperfoverlay")).toBool(), true);

    // Turning the last one off too, master goes off ("off when none is").
    QVERIFY(m_bridge->setStatsToggle(QStringLiteral("statsDecodingFrameRate"), false));
    QCOMPARE(m_bridge->getValue(QStringLiteral("showperfoverlay")).toBool(), false);

    // The master itself is not a customer control any more: a direct write is refused.
    QVERIFY(!m_bridge->setValue(QStringLiteral("showperfoverlay"), true));

    // Refused during a stream, exactly like any other write.
    m_bridge->setStreamingActive(true);
    QVERIFY(!m_bridge->setStatsToggle(QStringLiteral("statsVideoStream"), true));
    m_bridge->setStreamingActive(false);
}

void TstSettingsBridge::enabledStatsLabelsListsOnlyTheOnesTurnedOnInTheEnginesOwnOrder()
{
    // Phase 5 plan 12 (D-26): the one place `moonlight_engine_session.cpp`'s overlay filter
    // wiring reads the customer's choice from.
    QVERIFY(m_bridge->enabledStatsLabels().isEmpty());

    // Turned on out of the engine's own order - "Rendering frame rate" before "Video stream" -
    // the result must still read in the engine's order.
    QVERIFY(m_bridge->setStatsToggle(QStringLiteral("statsRenderingFrameRate"), true));
    QVERIFY(m_bridge->setStatsToggle(QStringLiteral("statsVideoStream"), true));

    QCOMPARE(m_bridge->enabledStatsLabels(),
             QStringList({ QStringLiteral("Video stream"), QStringLiteral("Rendering frame rate") }));

    QVERIFY(m_bridge->setStatsToggle(QStringLiteral("statsVideoStream"), false));
    QCOMPARE(m_bridge->enabledStatsLabels(), QStringList({ QStringLiteral("Rendering frame rate") }));

    QVERIFY(m_bridge->setStatsToggle(QStringLiteral("statsRenderingFrameRate"), false));
    QVERIFY(m_bridge->enabledStatsLabels().isEmpty());
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
