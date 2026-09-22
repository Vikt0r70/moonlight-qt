#include "settings_bridge.h"

#include "settings/streamingpreferences.h"

#include <QLoggingCategory>
#include <QSettings>

Q_LOGGING_CATEGORY(seathubSettings, "seathub.settings")

namespace {

// ---------------------------------------------------------------------------------------------
// The catalogue (D-11, D-24, D-25, D-25a, D-47, CUST-17).
//
// Every key upstream's `StreamingPreferences::save()` writes is listed exactly once, in the
// order `settings-audit.md` audits them. `group` is one of the seven sections upstream's own
// `SettingsView.qml` renders, in upstream's own order (D-24, superseding Phase 3's D-48
// SeatHub-invented grouping).
//
// A key is one of four things and the table says which:
//   * its own control, rendered                (mergedInto == nullptr, forced == false,
//                                                dropReason == nullptr)
//   * a legacy key folded into another control  (mergedInto set - `windowmode` and `fullscreen`
//     are one "Display mode" dropdown, `startwindowed` and `uidisplaymode` are one "GUI display
//     mode" dropdown, `width`/`height` are the "Resolution" control, `fps` is the "Frame rate"
//     control)
//   * corrected to a fixed value on every load, not rendered (forced == true, D-25(b)/(c),
//     T-05-45)
//   * not rendered and not corrected - its stored value keeps working exactly as it is, because
//     it was never on upstream's own page, or (`showperfoverlay`) because SeatHub's page shows
//     its per-value toggles instead (forced == false, dropReason set)
// ---------------------------------------------------------------------------------------------

enum Kind {
    KindBool,
    KindInt,
    KindEnum,
    // Read-only in SeatHub's own vocabulary: the value upstream stores is an internal marker
    // (an enum index, a version number) that has no honest customer-facing control, or a value
    // D-25 forces to one fixed setting.
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
    // True when the displayed/written boolean is the logical NOT of the stored one. Three
    // upstream checkboxes read this way (`hostaudio`, `abstouchmode`, `multicontroller`) - the
    // catalogue carries the inversion so no QML row has to know about it.
    bool inverted;
    // D-25(b)/(c): corrected back to this fixed value on every load (T-05-45). Never true at the
    // same time as a non-null `dropReason` being merely informational - a forced key's
    // `dropReason` explains *why* it is forced.
    bool forced;
    // Non-null when the row is not rendered, whether forced or simply not on upstream's page.
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

// D-24 order: Basic, Audio, Host, UI (column 1), Input, Gamepad, Advanced (column 2) -
// upstream's own two-column layout (`SettingsView.qml`, RESEARCH Q4).
const Setting kSettings[] = {
    // ---- Basic Settings ----------------------------------------------------------------------
    // Upstream's one row "Resolution and FPS" holds two combo boxes; SeatHub renders them as two
    // rows with the SeatHub select control, each carrying its own half of upstream's description.
    { "width", "basic", "Resolution", KindInt, nullptr, 0, "resolutionPreset", false, false,
      nullptr },
    { "height", "basic", "Resolution height", KindInt, nullptr, 0, "resolutionPreset", false,
      false, nullptr },
    { "fps", "basic", "Frame rate", KindInt, nullptr, 0, "frameRatePreset", false, false,
      nullptr },
    { "bitrate", "basic", "Video bitrate", KindInt, nullptr, 0, nullptr, false, false, nullptr },
    { "fullscreen", "basic", "Display mode", KindEnum, kDisplayModes, COUNT_OF(kDisplayModes),
      "displayMode", false, false, nullptr },
    { "windowmode", "basic", "Display mode", KindEnum, kDisplayModes, COUNT_OF(kDisplayModes),
      "displayMode", false, false, nullptr },
    { "vsync", "basic", "V-Sync", KindBool, nullptr, 0, nullptr, false, false, nullptr },
    { "framepacing", "basic", "Frame pacing", KindBool, nullptr, 0, nullptr, false, false,
      nullptr },

    // ---- Audio Settings -----------------------------------------------------------------------
    { "audiocfg", "audio", "Audio configuration", KindEnum, kAudioConfigs, COUNT_OF(kAudioConfigs),
      nullptr, false, false, nullptr },
    // D-25a: NOT forced. Stays visible and user-editable at upstream's own default
    // (`playAudioOnHost=false`), which the checkbox shows as checked (host muted) because the
    // checkbox is the inverse of the stored key, exactly as upstream renders it.
    { "hostaudio", "audio", "Mute host PC speakers while streaming", KindBool, nullptr, 0,
      nullptr, true, false, nullptr },
    { "muteonfocusloss", "audio", "Mute audio stream when SeatHub is not the active window",
      KindBool, nullptr, 0, nullptr, false, false, nullptr },

    // ---- Host Settings ------------------------------------------------------------------------
    { "gameopts", "host", "Optimize game settings for streaming", KindBool, nullptr, 0, nullptr,
      false, false, nullptr },
    // D-25(c): forced off, which is already upstream's own default - SeatHub owns the one
    // session per launch and closes the host app as part of its own teardown.
    { "quitAppAfter", "host", "Quit app on host PC after ending stream", KindFixed, nullptr, 0,
      nullptr, false, true,
      "SeatHub owns one session per launch and closes the host app as part of that teardown, so "
      "this is not a customer-facing choice. Forced off on every load (D-25(c))." },

    // ---- UI Settings --------------------------------------------------------------------------
    // D-25(b): forced English on every load. ADR-0043 also retires every language toggle on
    // every surface - Arabic wording renders inside the same layout, with no switcher.
    { "language", "ui", "Language", KindFixed, nullptr, 0, nullptr, false, true,
      "English-only milestone (ADR-0043). Forced on every load so a value edited by hand in the "
      "store cannot reach the engine (D-25(b))." },
    { "startwindowed", "ui", "GUI display mode", KindEnum, kLaunchDisplayModes,
      COUNT_OF(kLaunchDisplayModes), "launchDisplayMode", false, false, nullptr },
    { "uidisplaymode", "ui", "GUI display mode", KindEnum, kLaunchDisplayModes,
      COUNT_OF(kLaunchDisplayModes), "launchDisplayMode", false, false, nullptr },
    // OD-05: stays visible with upstream's own default (true). SeatHub's overlay re-asserts
    // itself after the engine writes status text so a low-balance warning is never blanked.
    { "connwarnings", "ui", "Show connection quality warnings", KindBool, nullptr, 0, nullptr,
      false, false, nullptr },
    // D-25(b): forced off. SeatHub has no external presence surface for this to act on.
    { "richpresence", "ui", "Discord Rich Presence integration", KindFixed, nullptr, 0, nullptr,
      false, true,
      "SeatHub has no external presence surface; the setting has nothing to act on. Forced off "
      "on every load (D-25(b))." },
    { "keepawake", "ui", "Keep the display awake while streaming", KindBool, nullptr, 0, nullptr,
      false, false, nullptr },

    // ---- Input Settings -----------------------------------------------------------------------
    { "mouseacceleration", "input", "Optimize mouse for remote desktop instead of games",
      KindBool, nullptr, 0, nullptr, false, false, nullptr },
    // ADR-0042/D-49, kept exactly as Phase 3 decided (D-26): SeatHub's own default (checked,
    // "In fullscreen") differs from upstream's own default (unchecked/never) - a third
    // authorized deviation from upstream, alongside the brand substitution and the
    // tooltip-to-description move, recorded in the re-issued audit.
    { "capturesyskeys", "input", "Capture system keyboard shortcuts", KindEnum, kCaptureSysKeys,
      COUNT_OF(kCaptureSysKeys), nullptr, false, false, nullptr },
    // Upstream's checkbox is the inverse of the stored key (`absoluteTouchMode=true` by default,
    // so the box ships unchecked).
    { "abstouchmode", "input", "Use touchscreen as a virtual trackpad", KindBool, nullptr, 0,
      nullptr, true, false, nullptr },
    { "swapmousebuttons", "input", "Swap left and right mouse buttons", KindBool, nullptr, 0,
      nullptr, false, false, nullptr },
    { "reversescroll", "input", "Reverse mouse scrolling direction", KindBool, nullptr, 0, nullptr,
      false, false, nullptr },

    // ---- Gamepad Settings ---------------------------------------------------------------------
    { "swapfacebuttons", "gamepad", "Swap A/B and X/Y gamepad buttons", KindBool, nullptr, 0,
      nullptr, false, false, nullptr },
    // Upstream's checkbox is the inverse of the stored key (`multiController=true` by default,
    // so the box ships unchecked).
    { "multicontroller", "gamepad", "Force gamepad #1 always connected", KindBool, nullptr, 0,
      nullptr, true, false, nullptr },
    { "gamepadmouse", "gamepad", "Enable mouse control with gamepads by holding the 'Start' "
      "button", KindBool, nullptr, 0, nullptr, false, false, nullptr },
    { "backgroundgamepad", "gamepad", "Process gamepad input when SeatHub is in the background",
      KindBool, nullptr, 0, nullptr, false, false, nullptr },

    // ---- Advanced Settings --------------------------------------------------------------------
    { "videodec", "advanced", "Video decoder", KindEnum, kVideoDecoders, COUNT_OF(kVideoDecoders),
      nullptr, false, false, nullptr },
    { "videocfg", "advanced", "Video codec", KindEnum, kVideoCodecs, COUNT_OF(kVideoCodecs),
      nullptr, false, false, nullptr },
    { "hdr", "advanced", "Enable HDR (Experimental)", KindBool, nullptr, 0, nullptr, false, false,
      nullptr },
    { "yuv444", "advanced", "Enable YUV 4:4:4 (Experimental)", KindBool, nullptr, 0, nullptr,
      false, false, nullptr },
    { "unlockbitrate", "advanced", "Unlock bitrate limit (Experimental)", KindBool, nullptr, 0,
      nullptr, false, false, nullptr },
    // D-25(b): forced off. SeatHub's session path is the control plane's own allocation, not
    // local discovery.
    { "mdns", "advanced", "Automatically find PCs on the local network (Recommended)", KindFixed,
      nullptr, 0, nullptr, false, true,
      "SeatHub reaches the host through the session the control plane allocated, so local "
      "discovery is not part of the session path. Forced off on every load (D-25(b))." },
    // D-25(c): forced off - it raises the engine's own dialogs, which never reach a customer.
    { "detectnetblocking", "advanced", "Automatically detect blocked connections (Recommended)",
      KindFixed, nullptr, 0, nullptr, false, true,
      "This check raises the engine's own dialogs, which SeatHub never shows a customer. Forced "
      "off on every load (D-25(c))." },
    // D-23/CUST-17: not a customer control any more. The eleven `statsToggle*` keys below are
    // the customer's actual choice; this value is derived from them (recomputeShowPerfOverlay).
    { "showperfoverlay", "advanced", "Show performance stats while streaming", KindFixed, nullptr,
      0, nullptr, false, false,
      "Replaced by the eleven performance-stats toggles below (D-23); its value follows them "
      "automatically (on when any is on, off when none is) rather than being a customer choice "
      "of its own." },

    // ---- Not on the engine's own settings page (completeness audit only, D-47) -----------------
    // OD-06: the owner dropped this row because the engine's own stock page does not show it.
    // Not forced - the stored value is untouched and keeps working, exactly as OD-06 asks.
    { "packetsize", "advanced", "Packet size (bytes)", KindInt, nullptr, 0, nullptr, false, false,
      "Not on the engine's own stock settings page (`moonlight stream` CLI only). The owner "
      "dropped this row (OD-06); the stored value is untouched and keeps working." },
    { "defaultver", "advanced", "Preference-format version", KindFixed, nullptr, 0, nullptr,
      false, false,
      "Internal upstream migration marker, never on the engine's own settings page. SeatHub "
      "preserves the migration behavior and never presents a version-control setting." },
};

// The eleven lines Moonlight 6.1.0's own `stringifyVideoStats()` writes (RESEARCH Q3,
// `ffmpeg.cpp:700-856`), each labelled with that line's own text minus its numbers (OD-03), in
// its own output order. `copy.md` § Settings carries the same eleven labels verbatim - this is
// the one place both this bridge and Plan 12's overlay filter read them from (D-26).
struct StatsToggle {
    const char* key;
    const char* label;
};

const StatsToggle kStatsToggles[] = {
    { "statsVideoStream", "Video stream" },
    { "statsIncomingFrameRate", "Incoming frame rate from network" },
    { "statsDecodingFrameRate", "Decoding frame rate" },
    { "statsRenderingFrameRate", "Rendering frame rate" },
    { "statsHostProcessingLatency", "Host processing latency min/max/average" },
    { "statsNetworkDroppedFrames", "Frames dropped by your network connection" },
    { "statsJitterDroppedFrames", "Frames dropped due to network jitter" },
    { "statsNetworkLatency", "Average network latency" },
    { "statsDecodingTime", "Average decoding time" },
    { "statsFrameQueueDelay", "Average frame queue delay" },
    { "statsRenderingTime", "Average rendering time (including monitor V-sync latency)" },
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

const StatsToggle* findStatsToggle(const QString& key)
{
    for (const StatsToggle& t : kStatsToggles) {
        if (key == QLatin1String(t.key)) {
            return &t;
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

// Upstream always offers 30 and 60 FPS plus a per-display detected refresh rate plus Custom
// (RESEARCH Q4). SeatHub's page offers the two fixed presets every install has, plus Custom for
// anything else, including a display's own native rate - a proper subset of upstream's option
// set, never a value upstream would refuse, and recorded as a deviation in the settings audit.
const int kFrameRatePresets[] = { 30, 60 };

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
    // T-05-45: a load is a load, including the very first one - a store hand-edited before the
    // process ever started must not survive past this constructor.
    applyForcedValues();
    recomputeShowPerfOverlay();
}

// ---------------------------------------------------------------------------- catalogue

QStringList SettingsBridge::groups() const
{
    // D-24's order: the two upstream columns, read top to bottom then left to right.
    return { QStringLiteral("basic"), QStringLiteral("audio"), QStringLiteral("host"),
             QStringLiteral("ui"), QStringLiteral("input"), QStringLiteral("gamepad"),
             QStringLiteral("advanced") };
}

QString SettingsBridge::groupTitle(const QString& group) const
{
    if (group == QLatin1String("basic")) return QStringLiteral("Basic Settings");
    if (group == QLatin1String("audio")) return QStringLiteral("Audio Settings");
    if (group == QLatin1String("host")) return QStringLiteral("Host Settings");
    if (group == QLatin1String("ui")) return QStringLiteral("UI Settings");
    if (group == QLatin1String("input")) return QStringLiteral("Input Settings");
    if (group == QLatin1String("gamepad")) return QStringLiteral("Gamepad Settings");
    if (group == QLatin1String("advanced")) return QStringLiteral("Advanced Settings");
    return QString();
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

bool SettingsBridge::isForced(const QString& key) const
{
    const Setting* s = findSetting(key);
    return s && s->forced;
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
        // Inverted (D-25a): the row shows "Mute host PC speakers", the stored key is
        // "play audio on host". Default false -> shown checked, exactly like upstream.
        return !m_preferences->playAudioOnHost;
    }
    if (key == QLatin1String("muteonfocusloss")) {
        return m_preferences->muteOnFocusLoss;
    }
    if (key == QLatin1String("gameopts")) {
        return m_preferences->gameOptimizations;
    }
    if (key == QLatin1String("multicontroller")) {
        // Inverted: default true -> shown unchecked, exactly like upstream.
        return !m_preferences->multiController;
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
        // Inverted: default true -> shown unchecked, exactly like upstream.
        return !m_preferences->absoluteTouchMode;
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
    if (s->forced) {
        qCWarning(seathubSettings) << "key is forced by SeatHub and not writable:" << key;
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
    else if (key == QLatin1String("hostaudio")) {
        // Inverted write: the row's "checked" means muted, i.e. `playAudioOnHost = false`.
        m_preferences->playAudioOnHost = !value.toBool();
    }
    else if (key == QLatin1String("muteonfocusloss")) {
        m_preferences->muteOnFocusLoss = value.toBool();
    }
    else if (key == QLatin1String("gameopts")) {
        m_preferences->gameOptimizations = value.toBool();
    }
    else if (key == QLatin1String("multicontroller")) {
        // Inverted write: the row's "checked" means "force gamepad #1 always connected", i.e.
        // `multiController = false`.
        m_preferences->multiController = !value.toBool();
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
        // Inverted write: the row's "checked" means "use as a virtual trackpad", i.e.
        // `absoluteTouchMode = false`.
        m_preferences->absoluteTouchMode = !value.toBool();
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
    // Widening the bitrate ceiling can leave a saved value that used to be clamped; nothing to
    // do here beyond notifying, since the QML row re-reads the bound on every refresh.
    if (key == QLatin1String("unlockbitrate")) {
        emit valueChanged(QStringLiteral("bitrate"));
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

QStringList SettingsBridge::frameRatePresets() const
{
    QStringList out;
    for (int fps : kFrameRatePresets) {
        out.append(QString::number(fps) + QStringLiteral(" FPS"));
    }
    out.append(QStringLiteral("Custom"));
    return out;
}

QString SettingsBridge::frameRatePreset() const
{
    for (int fps : kFrameRatePresets) {
        if (m_preferences->fps == fps) {
            return QString::number(fps) + QStringLiteral(" FPS");
        }
    }
    return QStringLiteral("Custom");
}

bool SettingsBridge::setFrameRatePreset(const QString& preset)
{
    for (int fps : kFrameRatePresets) {
        if (preset == QString::number(fps) + QStringLiteral(" FPS")) {
            return setValue(QStringLiteral("fps"), fps);
        }
    }
    return preset == QLatin1String("Custom") && frameRatePreset() == QLatin1String("Custom");
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

int SettingsBridge::bitrateMaximum() const
{
    // Upstream widens the ceiling from 150000 to 500000 Kbps only while unlockbitrate is on
    // (RESEARCH Q4, `SettingsView.qml`'s bitrate Slider). No new bound is invented here.
    return m_preferences->unlockBitrate ? 500000 : 150000;
}

// ------------------------------------------------------------------ performance stats (CUST-17)

QStringList SettingsBridge::statsToggleKeys() const
{
    QStringList out;
    for (const StatsToggle& t : kStatsToggles) {
        out.append(QString::fromLatin1(t.key));
    }
    return out;
}

QString SettingsBridge::statsToggleLabel(const QString& statsKey) const
{
    const StatsToggle* t = findStatsToggle(statsKey);
    return t ? QString::fromLatin1(t->label) : QString();
}

bool SettingsBridge::getStatsToggle(const QString& statsKey) const
{
    if (!findStatsToggle(statsKey)) {
        return false;
    }
    // Same preference store as everything else (D-12); a SeatHub-only key with no upstream
    // `[streamsettings]` counterpart, so it is read directly rather than through
    // `StreamingPreferences`, which upstream owns.
    return QSettings().value(statsKey, false).toBool();
}

bool SettingsBridge::setStatsToggle(const QString& statsKey, bool value)
{
    if (m_streaming) {
        qCInfo(seathubSettings) << "refused a stats-toggle write during an active stream:"
                                << statsKey;
        return false;
    }
    if (!findStatsToggle(statsKey)) {
        qCWarning(seathubSettings) << "unknown stats toggle key:" << statsKey;
        return false;
    }

    QSettings settings;
    settings.setValue(statsKey, value);
    settings.sync();

    recomputeShowPerfOverlay();
    emit valueChanged(statsKey);
    return true;
}

QStringList SettingsBridge::enabledStatsLabels() const
{
    // D-26: this is the one place both this bridge and Plan 12's overlay filter read the label
    // catalogue from - `kStatsToggles` above is defined once, here, and the filter itself
    // (`OverlayManager::setDebugLineFilter()`) carries no copy of it.
    QStringList out;
    for (const StatsToggle& t : kStatsToggles) {
        if (getStatsToggle(QString::fromLatin1(t.key))) {
            out.append(QString::fromLatin1(t.label));
        }
    }
    return out;
}

void SettingsBridge::recomputeShowPerfOverlay()
{
    QSettings settings;
    bool anyOn = false;
    for (const StatsToggle& t : kStatsToggles) {
        if (settings.value(QString::fromLatin1(t.key), false).toBool()) {
            anyOn = true;
            break;
        }
    }
    if (m_preferences->showPerformanceOverlay != anyOn) {
        m_preferences->showPerformanceOverlay = anyOn;
        m_preferences->save();
        emit valueChanged(QStringLiteral("showperfoverlay"));
    }
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

void SettingsBridge::prepareForSession()
{
    // RESEARCH Q4: forced values are re-applied "after reload() and before each stream start".
    // `SeatHubClient` calls this at the top of `beginSession()`/`beginLocalAttempt()`, before
    // `setAppState(kStateConnecting)` and therefore well before the engine constructs anything
    // that reads `StreamingPreferences` (T-05-45).
    applyForcedValues();
    recomputeShowPerfOverlay();
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

void SettingsBridge::applyForcedValues()
{
    // D-25(b)/(c), T-05-45: re-applied on every load, not only the first, so a value edited by
    // hand in the store between sessions is corrected before the engine reads it. `hostaudio` is
    // deliberately absent (D-25a amendment) - it stays user-editable at upstream's own default.
    bool changed = false;

    if (m_preferences->language != StreamingPreferences::LANG_EN) {
        m_preferences->language = StreamingPreferences::LANG_EN;
        changed = true;
    }
    if (m_preferences->richPresence) {
        m_preferences->richPresence = false;
        changed = true;
    }
    if (m_preferences->enableMdns) {
        m_preferences->enableMdns = false;
        changed = true;
    }
    if (m_preferences->detectNetworkBlocking) {
        m_preferences->detectNetworkBlocking = false;
        changed = true;
    }
    if (m_preferences->quitAppAfter) {
        m_preferences->quitAppAfter = false;
        changed = true;
    }

    if (changed) {
        m_preferences->save();
        qCInfo(seathubSettings) << "corrected forced settings back to their fixed value (D-25)";
    }
}
