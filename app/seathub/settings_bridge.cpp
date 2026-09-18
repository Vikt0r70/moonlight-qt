#include "settings_bridge.h"

#include "settings/streamingpreferences.h"

#include <QLoggingCategory>
#include <QSettings>

Q_LOGGING_CATEGORY(seathubSettings, "seathub.settings")

namespace {

// ---------------------------------------------------------------------------------------------
// The catalogue (D-11, D-47, CUST-05).
//
// Every key upstream's `StreamingPreferences::save()` writes is listed exactly once, in the
// order `settings-audit.md` audits them. `group` is the D-48 SeatHub grouping, which is
// SeatHub's own arrangement - upstream's launcher has no such split.
//
// A key is one of three things and the table says which:
//   * its own control           (mergedInto == nullptr, dropReason == nullptr)
//   * a legacy key folded into another control (mergedInto set - `windowmode` and `fullscreen`
//     are one "Display mode" dropdown, `startwindowed` and `uidisplaymode` are one launcher
//     display dropdown, `width`/`height` are one resolution dropdown plus its custom fields)
//   * deliberately managed by SeatHub with no control at all (dropReason set - D-47's
//     "intentionally dropped" status, which the Settings page discloses rather than hides)
// ---------------------------------------------------------------------------------------------

enum Kind {
    KindBool,
    KindInt,
    KindEnum,
    // Read-only in SeatHub's own vocabulary: the value upstream stores is an internal marker
    // (an enum index, a version number) that has no honest customer-facing control.
    KindFixed,
};

struct Setting {
    const char* key;
    const char* group;
    const char* label;
    Kind kind;
    // KindEnum only: the SeatHub strings the setting accepts, in dropdown order.
    const char* const* options;
    int optionCount;
    // Non-null when another control owns this key.
    const char* mergedInto;
    // Non-null when SeatHub manages the key itself and shows no control.
    const char* dropReason;
};

const char* const kDisplayModes[] = { "Fullscreen", "Borderless windowed", "Windowed" };
const char* const kLaunchDisplayModes[] = { "Windowed", "Maximized", "Fullscreen" };
const char* const kAudioConfigs[] = { "Stereo", "5.1 surround sound", "7.1 surround sound" };
const char* const kVideoCodecs[] = { "Automatic (Recommended)", "H.264", "HEVC (H.265)",
                                     "AV1 (Experimental)" };
const char* const kVideoDecoders[] = { "Automatic (Recommended)", "Force hardware decoding",
                                       "Force software decoding" };
const char* const kCaptureSysKeys[] = { "never", "fullscreen", "always" };
// `LANG_*` in ascending order, which is the order `streamingpreferences.h` documents these in.
const char* const kLanguages[] = {
    "Automatic", "English", "French", "Chinese (Simplified)", "German", "Norwegian Bokmål",
    "Russian", "Spanish", "Japanese", "Vietnamese", "Thai", "Korean", "Hungarian", "Dutch",
    "Swedish", "Turkish", "Ukrainian", "Chinese (Traditional)", "Portuguese",
    "Portuguese (Brazil)", "Greek", "Italian", "Hindi", "Polish", "Czech", "Hebrew",
    "Kurdish (Sorani)", "Lithuanian", "Estonian",
};

#define COUNT_OF(a) int(sizeof(a) / sizeof((a)[0]))

const Setting kSettings[] = {
    // ---- Video ------------------------------------------------------------------------------
    { "width", "video", "Resolution", KindInt, nullptr, 0, "resolutionPreset", nullptr },
    { "height", "video", "Resolution height", KindInt, nullptr, 0, "resolutionPreset", nullptr },
    { "fps", "video", "Frame rate", KindInt, nullptr, 0, nullptr, nullptr },
    { "bitrate", "video", "Video bitrate (Kbps)", KindInt, nullptr, 0, nullptr, nullptr },
    { "unlockbitrate", "video", "Unlock bitrate limit", KindBool, nullptr, 0, nullptr, nullptr },
    { "fullscreen", "video", "Display mode", KindEnum, kDisplayModes, COUNT_OF(kDisplayModes),
      "displayMode", nullptr },
    { "windowmode", "video", "Display mode", KindEnum, kDisplayModes, COUNT_OF(kDisplayModes),
      "displayMode", nullptr },
    { "vsync", "video", "V-Sync", KindBool, nullptr, 0, nullptr, nullptr },
    { "framepacing", "video", "Frame pacing", KindBool, nullptr, 0, nullptr, nullptr },
    { "hdr", "video", "HDR", KindBool, nullptr, 0, nullptr, nullptr },
    { "yuv444", "video", "YUV 4:4:4", KindBool, nullptr, 0, nullptr, nullptr },
    { "videocfg", "video", "Video codec", KindEnum, kVideoCodecs, COUNT_OF(kVideoCodecs),
      nullptr, nullptr },
    { "videodec", "video", "Video decoder", KindEnum, kVideoDecoders, COUNT_OF(kVideoDecoders),
      nullptr, nullptr },
    { "showperfoverlay", "video", "Show performance stats while streaming", KindBool, nullptr, 0,
      nullptr, nullptr },

    // ---- Audio ------------------------------------------------------------------------------
    { "audiocfg", "audio", "Audio configuration", KindEnum, kAudioConfigs, COUNT_OF(kAudioConfigs),
      nullptr, nullptr },
    { "hostaudio", "audio", "Mute host PC speakers while streaming", KindBool, nullptr, 0, nullptr,
      nullptr },
    { "muteonfocusloss", "audio", "Mute audio when SeatHub is not the active window", KindBool,
      nullptr, 0, nullptr, nullptr },

    // ---- Input ------------------------------------------------------------------------------
    { "gameopts", "input", "Optimize game settings for streaming", KindBool, nullptr, 0, nullptr,
      nullptr },
    { "multicontroller", "input", "Force gamepad #1 always connected", KindBool, nullptr, 0,
      nullptr, nullptr },
    { "gamepadmouse", "input", "Mouse control with gamepads (hold Start)", KindBool, nullptr, 0,
      nullptr, nullptr },
    { "backgroundgamepad", "input", "Process gamepad input in the background", KindBool, nullptr,
      0, nullptr, nullptr },
    { "swapfacebuttons", "input", "Swap A/B and X/Y gamepad buttons", KindBool, nullptr, 0,
      nullptr, nullptr },
    { "mouseacceleration", "input", "Optimize mouse for remote desktop instead of games",
      KindBool, nullptr, 0, nullptr, nullptr },
    { "abstouchmode", "input", "Use touchscreen as a virtual trackpad", KindBool, nullptr, 0,
      nullptr, nullptr },
    { "swapmousebuttons", "input", "Swap left and right mouse buttons", KindBool, nullptr, 0,
      nullptr, nullptr },
    { "reversescroll", "input", "Reverse mouse scrolling direction", KindBool, nullptr, 0, nullptr,
      nullptr },

    // ---- Network ----------------------------------------------------------------------------
    { "packetsize", "network", "Packet size (bytes)", KindInt, nullptr, 0, nullptr, nullptr },
    { "connwarnings", "network", "Show connection quality warnings", KindBool, nullptr, 0, nullptr,
      nullptr },
    { "detectnetblocking", "network", "Automatically detect blocked connections", KindBool,
      nullptr, 0, nullptr, nullptr },
    { "mdns", "network", "Find PCs on the local network", KindFixed, nullptr, 0, nullptr,
      "SeatHub reaches the host through the session the control plane allocated, so local "
      "discovery is not part of the session path. Left at upstream's setting." },

    // ---- Advanced ---------------------------------------------------------------------------
    { "startwindowed", "advanced", "Launcher window display mode", KindEnum, kLaunchDisplayModes,
      COUNT_OF(kLaunchDisplayModes), "launchDisplayMode", nullptr },
    { "uidisplaymode", "advanced", "Launcher window display mode", KindEnum, kLaunchDisplayModes,
      COUNT_OF(kLaunchDisplayModes), "launchDisplayMode", nullptr },
    { "capturesyskeys", "advanced", "Capture system keyboard shortcuts", KindEnum, kCaptureSysKeys,
      COUNT_OF(kCaptureSysKeys), nullptr, nullptr },
    { "keepawake", "advanced", "Keep the display awake while streaming", KindBool, nullptr, 0,
      nullptr, nullptr },
    { "language", "advanced", "Interface language", KindEnum, kLanguages, COUNT_OF(kLanguages),
      nullptr, nullptr },
    { "quitAppAfter", "advanced", "Quit app on host PC after ending stream", KindFixed, nullptr, 0,
      nullptr,
      "SeatHub owns one session per launch and closes the host app as part of that teardown, so "
      "this is not a customer-facing choice." },
    { "richpresence", "advanced", "Discord Rich Presence integration", KindFixed, nullptr, 0,
      nullptr, "SeatHub has no external presence surface; the setting has nothing to act on." },
    { "defaultver", "advanced", "Preference-format version", KindFixed, nullptr, 0, nullptr,
      "Internal upstream migration marker. SeatHub preserves the migration behavior and never "
      "presents a version-control setting." },
};

const Setting* findSetting(const QString& key)
{
    for (const Setting& s : kSettings) {
        if (key == QLatin1String(s.key)) {
            return &s;
        }
    }
    return nullptr;
}

int optionIndex(const Setting& s, const QString& value)
{
    for (int i = 0; i < s.optionCount; i++) {
        if (value == QLatin1String(s.options[i])) {
            return i;
        }
    }
    return -1;
}

// The four resolution presets upstream's CLI and settings page share
// (`commandlineparser.cpp:344-348`), plus "Custom" for anything else.
struct ResolutionPreset {
    const char* name;
    int width;
    int height;
};

const ResolutionPreset kResolutionPresets[] = {
    { "720p", 1280, 720 },
    { "1080p", 1920, 1080 },
    { "1440p", 2560, 1440 },
    { "4K", 3840, 2160 },
};

// SeatHub's sentences for a setting the engine could not honour as saved. Composed in
// `copy.md`'s voice (plain, factual); the engine's own sentence never reaches a screen
// (D-51, T-03-05) and is kept in `m_warningDiagnostics` for the support bundle only.
QString warningSentenceFor(const QString& key, const QString& label)
{
    if (key == QLatin1String("width") || key == QLatin1String("height")
        || key == QLatin1String("resolutionPreset")) {
        return QStringLiteral("This host can't use the resolution you saved, so the stream is "
                              "running at a different one. Your saved resolution is unchanged.");
    }
    if (key == QLatin1String("hdr")) {
        return QStringLiteral("HDR isn't available for this session, so the stream is running "
                              "without it. Your saved setting is unchanged.");
    }
    if (key == QLatin1String("yuv444")) {
        return QStringLiteral("YUV 4:4:4 isn't available for this session, so the stream is "
                              "running without it. Your saved setting is unchanged.");
    }
    if (key == QLatin1String("videocfg")) {
        return QStringLiteral("This host can't encode the video codec you saved, so the stream "
                              "is using another one. Your saved codec is unchanged.");
    }
    if (key == QLatin1String("videodec")) {
        return QStringLiteral("This PC can't decode with the option you saved, so the stream is "
                              "using another decoder. Your saved choice is unchanged.");
    }
    if (key == QLatin1String("audiocfg")) {
        return QStringLiteral("This audio configuration isn't available for this session. Your "
                              "saved setting is unchanged.");
    }
    if (key == QLatin1String("mouseacceleration")) {
        return QStringLiteral("Remote desktop mouse mode can cause problems in games. Your saved "
                              "setting is unchanged.");
    }
    if (key == QLatin1String("multicontroller")) {
        return QStringLiteral("A connected gamepad has no mapping, so it won't work this "
                              "session. Your saved setting is unchanged.");
    }
    return QStringLiteral("This host couldn't use your saved %1 setting for this session, so the "
                          "stream is using something else. Your saved value is unchanged.")
        .arg(label.toLower());
}

// Longest, most specific phrases first: "…YUV 4:4:4 streaming for selected video codec" must
// not be caught by the shorter YUV phrase's key before its own is tested.
struct WarningPattern {
    const char* needle;
    const char* key;
};

const WarningPattern kWarningPatterns[] = {
    { "doesn't support yuv 4:4:4 streaming for selected video codec", "yuv444" },
    { "yuv 4:4:4 decoding for selected video codec", "yuv444" },
    { "doesn't support yuv 4:4:4 streaming", "yuv444" },
    { "force yuv 4:4:4 without gpu support", "videodec" },
    { "geforce experience 3.0 or higher is required for 4k streaming", "width" },
    { "10-bit hevc or av1 decoding for hdr", "hdr" },
    { "av1 main10 decoding for hdr", "hdr" },
    { "hevc main10 decoding for hdr", "hdr" },
    { "hdr is not supported using the h.264 codec", "hdr" },
    { "force hdr without gpu support", "videodec" },
    { "doesn't support hdr streaming", "hdr" },
    { "don't support the same hdr video codecs", "hdr" },
    { "force av1 without gpu support", "videodec" },
    { "force hevc without gpu support", "videodec" },
    { "force software decoding", "videodec" },
    { "doesn't support encoding av1", "videocfg" },
    { "doesn't support encoding hevc", "videocfg" },
    { "don't support the same video codecs", "videocfg" },
    { "doesn't support h.264 decoding", "videodec" },
    { "surround sound setting is not supported", "audiocfg" },
    { "failed to open audio device", "audiocfg" },
    { "gamepad has no mapping", "multicontroller" },
    { "remote desktop mouse mode", "mouseacceleration" },
};

} // namespace

SettingsBridge::SettingsBridge(QObject* parent)
    : QObject(parent)
{
    m_preferences = StreamingPreferences::get();
    applySeatHubDefaults();
}

// ---------------------------------------------------------------------------- catalogue

QStringList SettingsBridge::groups() const
{
    // D-48's order, and the order the Settings page renders.
    return { QStringLiteral("video"), QStringLiteral("audio"), QStringLiteral("input"),
             QStringLiteral("network"), QStringLiteral("advanced") };
}

QStringList SettingsBridge::keys() const
{
    QStringList out;
    for (const Setting& s : kSettings) {
        out.append(QString::fromLatin1(s.key));
    }
    return out;
}

QStringList SettingsBridge::keysInGroup(const QString& group) const
{
    QStringList out;
    for (const Setting& s : kSettings) {
        if (group == QLatin1String(s.group)) {
            out.append(QString::fromLatin1(s.key));
        }
    }
    return out;
}

QString SettingsBridge::groupOf(const QString& key) const
{
    const Setting* s = findSetting(key);
    return s ? QString::fromLatin1(s->group) : QString();
}

QString SettingsBridge::labelOf(const QString& key) const
{
    const Setting* s = findSetting(key);
    return s ? QString::fromLatin1(s->label) : QString();
}

QStringList SettingsBridge::enumOptions(const QString& key) const
{
    const Setting* s = findSetting(key);
    if (!s || s->kind != KindEnum) {
        return {};
    }
    QStringList out;
    for (int i = 0; i < s->optionCount; i++) {
        out.append(QString::fromLatin1(s->options[i]));
    }
    return out;
}

bool SettingsBridge::isMerged(const QString& key) const
{
    const Setting* s = findSetting(key);
    return s && s->mergedInto != nullptr;
}

QStringList SettingsBridge::mergedSources(const QString& key) const
{
    QStringList out;
    for (const Setting& s : kSettings) {
        if (s.mergedInto != nullptr && key == QLatin1String(s.mergedInto)) {
            out.append(QString::fromLatin1(s.key));
        }
    }
    return out;
}

bool SettingsBridge::isDropped(const QString& key) const
{
    const Setting* s = findSetting(key);
    return s && s->dropReason != nullptr;
}

QString SettingsBridge::dropReason(const QString& key) const
{
    const Setting* s = findSetting(key);
    return (s && s->dropReason) ? QString::fromLatin1(s->dropReason) : QString();
}

QStringList SettingsBridge::droppedKeys() const
{
    QStringList out;
    for (const Setting& s : kSettings) {
        if (s.dropReason != nullptr) {
            out.append(QString::fromLatin1(s.key));
        }
    }
    return out;
}

// ------------------------------------------------------------------------ read / write

QVariant SettingsBridge::getSavedValue(const QString& key) const
{
    const Setting* s = findSetting(key);
    if (!s) {
        return {};
    }

    if (key == QLatin1String("width")) {
        return m_preferences->width;
    }
    if (key == QLatin1String("height")) {
        return m_preferences->height;
    }
    if (key == QLatin1String("fps")) {
        return m_preferences->fps;
    }
    if (key == QLatin1String("bitrate")) {
        return m_preferences->bitrateKbps;
    }
    if (key == QLatin1String("unlockbitrate")) {
        return m_preferences->unlockBitrate;
    }
    if (key == QLatin1String("vsync")) {
        return m_preferences->enableVsync;
    }
    if (key == QLatin1String("framepacing")) {
        return m_preferences->framePacing;
    }
    if (key == QLatin1String("hdr")) {
        return m_preferences->enableHdr;
    }
    if (key == QLatin1String("yuv444")) {
        return m_preferences->enableYUV444;
    }
    if (key == QLatin1String("showperfoverlay")) {
        return m_preferences->showPerformanceOverlay;
    }
    if (key == QLatin1String("hostaudio")) {
        return m_preferences->playAudioOnHost;
    }
    if (key == QLatin1String("muteonfocusloss")) {
        return m_preferences->muteOnFocusLoss;
    }
    if (key == QLatin1String("gameopts")) {
        return m_preferences->gameOptimizations;
    }
    if (key == QLatin1String("multicontroller")) {
        return m_preferences->multiController;
    }
    if (key == QLatin1String("gamepadmouse")) {
        return m_preferences->gamepadMouse;
    }
    if (key == QLatin1String("backgroundgamepad")) {
        return m_preferences->backgroundGamepad;
    }
    if (key == QLatin1String("swapfacebuttons")) {
        return m_preferences->swapFaceButtons;
    }
    if (key == QLatin1String("mouseacceleration")) {
        return m_preferences->absoluteMouseMode;
    }
    if (key == QLatin1String("abstouchmode")) {
        return m_preferences->absoluteTouchMode;
    }
    if (key == QLatin1String("swapmousebuttons")) {
        return m_preferences->swapMouseButtons;
    }
    if (key == QLatin1String("reversescroll")) {
        return m_preferences->reverseScrollDirection;
    }
    if (key == QLatin1String("packetsize")) {
        return m_preferences->packetSize;
    }
    if (key == QLatin1String("connwarnings")) {
        return m_preferences->connectionWarnings;
    }
    if (key == QLatin1String("detectnetblocking")) {
        return m_preferences->detectNetworkBlocking;
    }
    if (key == QLatin1String("mdns")) {
        return m_preferences->enableMdns;
    }
    if (key == QLatin1String("keepawake")) {
        return m_preferences->keepAwake;
    }
    if (key == QLatin1String("quitAppAfter")) {
        return m_preferences->quitAppAfter;
    }
    if (key == QLatin1String("richpresence")) {
        return m_preferences->richPresence;
    }
    if (key == QLatin1String("defaultver")) {
        // Not a member: `reload()` keeps it local. Read the same store rather than a second one.
        return QSettings().value(QStringLiteral("defaultver"), 0).toInt();
    }
    if (key == QLatin1String("audiocfg")) {
        return QString::fromLatin1(kAudioConfigs[int(m_preferences->audioConfig)]);
    }
    if (key == QLatin1String("videocfg")) {
        // VCC_FORCE_HEVC_HDR_DEPRECATED is read as VCC_AUTO by `reload()`, so it is never a
        // value this bridge reports or writes back.
        if (m_preferences->videoCodecConfig == StreamingPreferences::VCC_FORCE_HEVC_HDR_DEPRECATED) {
            return QString::fromLatin1(kVideoCodecs[0]);
        }
        if (m_preferences->videoCodecConfig == StreamingPreferences::VCC_FORCE_AV1) {
            return QString::fromLatin1(kVideoCodecs[3]);
        }
        return QString::fromLatin1(kVideoCodecs[int(m_preferences->videoCodecConfig)]);
    }
    if (key == QLatin1String("videodec")) {
        return QString::fromLatin1(kVideoDecoders[int(m_preferences->videoDecoderSelection)]);
    }
    if (key == QLatin1String("capturesyskeys")) {
        return QString::fromLatin1(kCaptureSysKeys[int(m_preferences->captureSysKeysMode)]);
    }
    if (key == QLatin1String("language")) {
        const int index = int(m_preferences->language);
        if (index < 0 || index >= COUNT_OF(kLanguages)) {
            return QString::fromLatin1(kLanguages[0]);
        }
        return QString::fromLatin1(kLanguages[index]);
    }
    if (key == QLatin1String("windowmode") || key == QLatin1String("displayMode")
        || key == QLatin1String("fullscreen")) {
        return QString::fromLatin1(kDisplayModes[int(m_preferences->windowMode)]);
    }
    if (key == QLatin1String("uidisplaymode") || key == QLatin1String("launchDisplayMode")
        || key == QLatin1String("startwindowed")) {
        return QString::fromLatin1(kLaunchDisplayModes[int(m_preferences->uiDisplayMode)]);
    }
    return {};
}

QVariant SettingsBridge::getValue(const QString& key) const
{
    const auto it = m_overrides.constFind(key);
    if (it != m_overrides.constEnd()) {
        return it.value();
    }
    return getSavedValue(key);
}

bool SettingsBridge::setValue(const QString& key, const QVariant& value)
{
    if (m_streaming) {
        // Pitfall 6 / T-03-15: settings apply to the next stream, never the running one.
        qCInfo(seathubSettings) << "refused a settings write during an active stream:" << key;
        return false;
    }

    const Setting* s = findSetting(key);
    if (!s) {
        qCWarning(seathubSettings) << "unknown settings key:" << key;
        return false;
    }
    if (s->dropReason != nullptr) {
        qCWarning(seathubSettings) << "key is managed by SeatHub and not writable:" << key;
        return false;
    }

    return writeThrough(key, value);
}

QVariantMap SettingsBridge::effectiveValues() const
{
    QVariantMap out;
    for (const Setting& s : kSettings) {
        out.insert(QString::fromLatin1(s.key), getValue(QString::fromLatin1(s.key)));
    }
    return out;
}

bool SettingsBridge::writeThrough(const QString& key, const QVariant& value)
{
    // Every accepted value is written to the one upstream preference object and saved
    // immediately (D-13). The merged keys converge on the same field, so a write through any
    // of a merge's members leaves one stored value - there is no second field to disagree.
    const Setting* s = findSetting(key);

    bool ok = true;

    if (key == QLatin1String("width")) {
        m_preferences->width = value.toInt();
    }
    else if (key == QLatin1String("height")) {
        m_preferences->height = value.toInt();
    }
    else if (key == QLatin1String("fps")) {
        const int fps = value.toInt();
        if (fps <= 0) {
            return false;
        }
        m_preferences->fps = fps;
    }
    else if (key == QLatin1String("bitrate")) {
        const int bitrate = value.toInt();
        if (bitrate <= 0) {
            return false;
        }
        m_preferences->bitrateKbps = bitrate;
    }
    else if (key == QLatin1String("packetsize")) {
        const int packetSize = value.toInt();
        // Upstream's own rule, from the CLI parser: 0 means "let the engine decide" and anything
        // else must be at least 1024 bytes (`commandlineparser.cpp:433-437`). No new bound.
        if (packetSize < 0 || (packetSize > 0 && packetSize < 1024)) {
            qCWarning(seathubSettings) << "refused a packet size outside upstream's rule:"
                                       << packetSize;
            return false;
        }
        m_preferences->packetSize = packetSize;
    }
    else if (key == QLatin1String("unlockbitrate")) {
        m_preferences->unlockBitrate = value.toBool();
    }
    else if (key == QLatin1String("vsync")) {
        m_preferences->enableVsync = value.toBool();
    }
    else if (key == QLatin1String("framepacing")) {
        m_preferences->framePacing = value.toBool();
    }
    else if (key == QLatin1String("hdr")) {
        m_preferences->enableHdr = value.toBool();
    }
    else if (key == QLatin1String("yuv444")) {
        m_preferences->enableYUV444 = value.toBool();
    }
    else if (key == QLatin1String("showperfoverlay")) {
        m_preferences->showPerformanceOverlay = value.toBool();
    }
    else if (key == QLatin1String("hostaudio")) {
        m_preferences->playAudioOnHost = value.toBool();
    }
    else if (key == QLatin1String("muteonfocusloss")) {
        m_preferences->muteOnFocusLoss = value.toBool();
    }
    else if (key == QLatin1String("gameopts")) {
        m_preferences->gameOptimizations = value.toBool();
    }
    else if (key == QLatin1String("multicontroller")) {
        m_preferences->multiController = value.toBool();
    }
    else if (key == QLatin1String("gamepadmouse")) {
        m_preferences->gamepadMouse = value.toBool();
    }
    else if (key == QLatin1String("backgroundgamepad")) {
        m_preferences->backgroundGamepad = value.toBool();
    }
    else if (key == QLatin1String("swapfacebuttons")) {
        m_preferences->swapFaceButtons = value.toBool();
    }
    else if (key == QLatin1String("mouseacceleration")) {
        m_preferences->absoluteMouseMode = value.toBool();
    }
    else if (key == QLatin1String("abstouchmode")) {
        m_preferences->absoluteTouchMode = value.toBool();
    }
    else if (key == QLatin1String("swapmousebuttons")) {
        m_preferences->swapMouseButtons = value.toBool();
    }
    else if (key == QLatin1String("reversescroll")) {
        m_preferences->reverseScrollDirection = value.toBool();
    }
    else if (key == QLatin1String("connwarnings")) {
        m_preferences->connectionWarnings = value.toBool();
    }
    else if (key == QLatin1String("detectnetblocking")) {
        m_preferences->detectNetworkBlocking = value.toBool();
    }
    else if (key == QLatin1String("keepawake")) {
        m_preferences->keepAwake = value.toBool();
    }
    else if (key == QLatin1String("audiocfg")) {
        const int index = optionIndex(*findSetting(key), value.toString());
        if (index < 0) {
            return false;
        }
        m_preferences->audioConfig = StreamingPreferences::AudioConfig(index);
    }
    else if (key == QLatin1String("videocfg")) {
        const int index = optionIndex(*findSetting(key), value.toString());
        if (index < 0) {
            return false;
        }
        // "AV1 (Experimental)" is option 3 in SeatHub's list and VCC_FORCE_AV1 (4) upstream,
        // because index 3 is the deprecated HDR variant `reload()` folds into VCC_AUTO.
        static const StreamingPreferences::VideoCodecConfig kCodecMap[] = {
            StreamingPreferences::VCC_AUTO,
            StreamingPreferences::VCC_FORCE_H264,
            StreamingPreferences::VCC_FORCE_HEVC,
            StreamingPreferences::VCC_FORCE_AV1,
        };
        m_preferences->videoCodecConfig = kCodecMap[index];
    }
    else if (key == QLatin1String("videodec")) {
        const int index = optionIndex(*findSetting(key), value.toString());
        if (index < 0) {
            return false;
        }
        m_preferences->videoDecoderSelection =
            StreamingPreferences::VideoDecoderSelection(index);
    }
    else if (key == QLatin1String("capturesyskeys")) {
        const int index = optionIndex(*findSetting(key), value.toString());
        if (index < 0) {
            return false;
        }
        m_preferences->captureSysKeysMode = StreamingPreferences::CaptureSysKeysMode(index);
    }
    else if (key == QLatin1String("language")) {
        const int index = optionIndex(*findSetting(key), value.toString());
        if (index < 0) {
            return false;
        }
        m_preferences->language = StreamingPreferences::Language(index);
    }
    else if (key == QLatin1String("windowmode") || key == QLatin1String("displayMode")
             || key == QLatin1String("fullscreen")) {
        const int index = optionIndex(*findSetting(QStringLiteral("windowmode")),
                                      value.toString());
        if (index < 0) {
            return false;
        }
        m_preferences->windowMode = StreamingPreferences::WindowMode(index);
    }
    else if (key == QLatin1String("uidisplaymode") || key == QLatin1String("launchDisplayMode")
             || key == QLatin1String("startwindowed")) {
        const int index = optionIndex(*findSetting(QStringLiteral("uidisplaymode")),
                                      value.toString());
        if (index < 0) {
            return false;
        }
        m_preferences->uiDisplayMode = StreamingPreferences::UIDisplayMode(index);
    }
    else {
        qCWarning(seathubSettings) << "no write-through path for settings key:" << key;
        ok = false;
    }

    if (!ok) {
        return false;
    }

    m_preferences->save();
    emit valueChanged(key);

    // A write through one member of a merge is a write to the merged control.
    if (s && s->mergedInto != nullptr) {
        emit valueChanged(QString::fromLatin1(s->mergedInto));
    }

    return true;
}

// ------------------------------------------------------------------------ merged controls

QStringList SettingsBridge::resolutionPresets() const
{
    QStringList out;
    for (const ResolutionPreset& p : kResolutionPresets) {
        out.append(QString::fromLatin1(p.name));
    }
    out.append(QStringLiteral("Custom"));
    return out;
}

QString SettingsBridge::resolutionPreset() const
{
    for (const ResolutionPreset& p : kResolutionPresets) {
        if (m_preferences->width == p.width && m_preferences->height == p.height) {
            return QString::fromLatin1(p.name);
        }
    }
    return QStringLiteral("Custom");
}

bool SettingsBridge::setResolutionPreset(const QString& preset)
{
    for (const ResolutionPreset& p : kResolutionPresets) {
        if (preset == QLatin1String(p.name)) {
            // One control, one stored pair: preset names and custom values cannot disagree.
            const bool ok = setValue(QStringLiteral("width"), p.width);
            return setValue(QStringLiteral("height"), p.height) && ok;
        }
    }
    // "Custom" is a state of the pair, not a value to store - the width and height fields
    // carry it. Anything else is not a preset this build knows.
    return preset == QLatin1String("Custom") && resolutionPreset() == QLatin1String("Custom");
}

QStringList SettingsBridge::displayModes() const
{
    return enumOptions(QStringLiteral("windowmode"));
}

QString SettingsBridge::displayMode() const
{
    return getSavedValue(QStringLiteral("windowmode")).toString();
}

bool SettingsBridge::setDisplayMode(const QString& mode)
{
    return setValue(QStringLiteral("windowmode"), mode);
}

QStringList SettingsBridge::launchDisplayModes() const
{
    return enumOptions(QStringLiteral("uidisplaymode"));
}

QString SettingsBridge::launchDisplayMode() const
{
    return getSavedValue(QStringLiteral("uidisplaymode")).toString();
}

bool SettingsBridge::setLaunchDisplayMode(const QString& mode)
{
    return setValue(QStringLiteral("uidisplaymode"), mode);
}

QStringList SettingsBridge::captureSysKeysModes() const
{
    // ADR-0042: the checkbox yields "never"; the dropdown offers exactly these two.
    return { QStringLiteral("fullscreen"), QStringLiteral("always") };
}

QString SettingsBridge::captureSysKeysMode() const
{
    return getSavedValue(QStringLiteral("capturesyskeys")).toString();
}

bool SettingsBridge::setCaptureSysKeysMode(const QString& mode)
{
    return setValue(QStringLiteral("capturesyskeys"), mode);
}

// ------------------------------------------------------------------ negotiated / D-14

bool SettingsBridge::hasNegotiatedResults() const
{
    return m_connectionStarted;
}

QVariant SettingsBridge::getNegotiatedValue(const QString& key) const
{
    if (!m_connectionStarted) {
        // Nothing has been negotiated yet; an empty value is the honest answer (IN-04).
        return {};
    }
    return getValue(key);
}

QVariantMap SettingsBridge::getNegotiatedValues() const
{
    if (!m_connectionStarted) {
        return {};
    }
    QVariantMap out = effectiveValues();
    for (auto it = m_warningSentences.constBegin(); it != m_warningSentences.constEnd(); ++it) {
        // The fallback the engine actually settled on is not reachable through a public engine
        // seam (see this class's header); the warning is what the engine does publish, so it is
        // reported alongside the effective value rather than invented.
        out.insert(it.key() + QStringLiteral("Warning"), it.value());
    }
    return out;
}

QVariantMap SettingsBridge::negotiationWarnings() const
{
    QVariantMap out;
    for (auto it = m_warningSentences.constBegin(); it != m_warningSentences.constEnd(); ++it) {
        out.insert(it.key(), it.value());
    }
    return out;
}

bool SettingsBridge::hasNegotiationWarning(const QString& key) const
{
    return m_warningSentences.contains(key);
}

QString SettingsBridge::negotiationWarning(const QString& key) const
{
    return m_warningSentences.value(key);
}

QString SettingsBridge::warningKeyFor(const QString& engineText)
{
    const QString haystack = engineText.toLower();
    for (const WarningPattern& p : kWarningPatterns) {
        if (haystack.contains(QLatin1String(p.needle))) {
            return QString::fromLatin1(p.key);
        }
    }
    return {};
}

// ------------------------------------------------------------------ session overrides

void SettingsBridge::applySessionOverride(const QString& qualityProfile)
{
    m_overrides.clear();

    // `openapi.yaml` §QualityProfile is a closed enum of three identifiers, matched by equality
    // (ADR-0011). Each names a resolution and a frame rate; the profile says nothing about
    // bitrate, codec, HDR or audio, so nothing else is overridden - the customer's saved
    // choices stand for those.
    int width = 0;
    int height = 0;
    int fps = 0;
    if (qualityProfile == QLatin1String("1080p60")) {
        width = 1920;
        height = 1080;
        fps = 60;
    }
    else if (qualityProfile == QLatin1String("1080p75")) {
        width = 1920;
        height = 1080;
        fps = 75;
    }
    else if (qualityProfile == QLatin1String("1080p120")) {
        width = 1920;
        height = 1080;
        fps = 120;
    }
    else {
        qCWarning(seathubSettings) << "unrecognized quality profile, no overrides applied:"
                                   << qualityProfile;
        emit sessionOverridesChanged();
        return;
    }

    m_overrides.insert(QStringLiteral("width"), width);
    m_overrides.insert(QStringLiteral("height"), height);
    m_overrides.insert(QStringLiteral("fps"), fps);

    qCInfo(seathubSettings) << "applied in-memory overrides for" << qualityProfile;
    emit sessionOverridesChanged();
}

void SettingsBridge::clearSessionOverrides()
{
    if (m_overrides.isEmpty()) {
        return;
    }
    m_overrides.clear();
    emit sessionOverridesChanged();
}

// ------------------------------------------------------------------ lifecycle hooks

bool SettingsBridge::writable() const
{
    return !m_streaming;
}

bool SettingsBridge::sessionActive() const
{
    return m_streaming;
}

bool SettingsBridge::hasSessionOverrides() const
{
    return !m_overrides.isEmpty();
}

void SettingsBridge::setStreamingActive(bool active)
{
    if (m_streaming == active) {
        return;
    }
    m_streaming = active;
    emit writableChanged();
    emit sessionActiveChanged();
}

void SettingsBridge::noteConnectionStarted()
{
    if (m_connectionStarted) {
        return;
    }
    m_connectionStarted = true;
    emit negotiationChanged();
}

void SettingsBridge::noteSessionFinished()
{
    // D-37: the overrides belonged to that launch. The saved values were never touched, so
    // clearing them restores exactly what the customer had.
    clearSessionOverrides();

    if (m_connectionStarted || !m_warningSentences.isEmpty()) {
        m_connectionStarted = false;
        m_warningSentences.clear();
        m_warningDiagnostics.clear();
        emit negotiationChanged();
    }
}

void SettingsBridge::noteLaunchWarning(const QString& engineText)
{
    const QString key = warningKeyFor(engineText);
    if (key.isEmpty() || !findSetting(key)) {
        // Not attributable to a setting we expose; nothing to show the customer. The sentence
        // is still engine text, so it never becomes customer-facing copy (D-51).
        qCInfo(seathubSettings) << "engine launch warning (not attributable to an exposed setting)";
        return;
    }

    if (!m_warningDiagnostics.contains(key)) {
        m_warningDiagnostics.insert(key, engineText);
    }
    m_warningSentences.insert(key, warningSentenceFor(key, labelOf(key)));

    // D-14: the saved preference is never rewritten by a fallback.
    qCInfo(seathubSettings) << "engine reported a fallback for" << key
                            << "- saved preference left unchanged";
    emit negotiationChanged();
}

void SettingsBridge::applySeatHubDefaults()
{
    // ADR-0042 freezes a SeatHub default that differs from upstream's: capture-system-keys
    // ships checked with "In fullscreen", where upstream's own default is never. Applied once,
    // through the same preference object everything else uses, so the stored value and the
    // control agree from the first launch onwards.
    QSettings settings;
    if (settings.contains(QStringLiteral("capturesyskeys"))) {
        return;
    }

    m_preferences->captureSysKeysMode = StreamingPreferences::CSK_FULLSCREEN;
    m_preferences->save();
    qCInfo(seathubSettings) << "applied SeatHub's capture-system-keys default (ADR-0042)";
}
