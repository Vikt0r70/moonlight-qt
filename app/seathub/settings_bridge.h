#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

class StreamingPreferences;

// SeatHub's write-through bridge between the QML Settings page and upstream's
// `StreamingPreferences` singleton.
//
// The rules this class exists to enforce, and the decision each one comes from:
//
//   * There is exactly ONE preference store. Every read and every write goes to the upstream
//     `StreamingPreferences` object; this class caches nothing (D-12, STREAM-02).
//   * Nothing is written while a stream is running. `setValue()` returns false and the stored
//     value is untouched (Pitfall 6, T-03-15). Settings changes apply to the NEXT stream (D-13).
//   * A value the control plane dictates for one launch is an in-memory override. Overrides are
//     never written to `StreamingPreferences` and never reach disk (D-37, WR-05).
//   * An engine warning about a setting it could not honour never rewrites the preference. It
//     becomes SeatHub-worded copy the Settings page shows beside the saved value (D-14). The
//     engine's own sentence is kept for diagnostics only and is never rendered (D-51, T-03-05).
//   * Every `[streamsettings]` key upstream serializes is listed here, including the ones
//     SeatHub deliberately manages itself, so nothing is hidden (D-11, CUST-05).
//   * The page is the engine's own settings page, section for section and row for row (D-24). The
//     only differences from upstream's own v6.1.0 page are the ones D-25/D-25a name, plus the
//     brand substitution and the tooltip-to-description move that D-24 itself requires. A key
//     D-25 removes is not rendered at all - not disabled, not hidden behind a disclosure - and its
//     value is corrected back to the fixed one on every load, not only the first (D-25, T-05-45).
//
// The catalogue (keys, groups, labels, enum vocabularies) lives in this class rather than in
// QML so there is one place to check against `settings-audit.md`.
class SettingsBridge : public QObject
{
    Q_OBJECT

    // False while a stream is active - the Settings page renders read-only (Pitfall 6).
    Q_PROPERTY(bool writable READ writable NOTIFY writableChanged)
    Q_PROPERTY(bool sessionActive READ sessionActive NOTIFY sessionActiveChanged)
    Q_PROPERTY(bool hasSessionOverrides READ hasSessionOverrides NOTIFY sessionOverridesChanged)

    // Setting key -> SeatHub-worded fallback warning. Empty until the engine reports one.
    Q_PROPERTY(QVariantMap negotiationWarnings READ negotiationWarnings NOTIFY negotiationChanged)

public:
    // How a value crosses the QML boundary. Enumerated settings cross as the SeatHub string
    // listed in `enumOptions()`; `KindFixed` settings are managed by SeatHub and read-only.
    enum ValueKind {
        KindBool,
        KindInt,
        KindEnum,
        KindFixed,
    };

    explicit SettingsBridge(QObject* parent = nullptr);

    // ---------------------------------------------------------------- the catalogue (D-11/D-24)

    /// The seven upstream sections, in upstream's own order (D-24, superseding Phase 3's D-48).
    Q_INVOKABLE QStringList groups() const;
    /// The upstream section title for a group id (`"Basic Settings"`, and so on).
    Q_INVOKABLE QString groupTitle(const QString& group) const;
    /// Every `[streamsettings]` key upstream serializes, in catalogue order.
    Q_INVOKABLE QStringList keys() const;
    Q_INVOKABLE QStringList keysInGroup(const QString& group) const;
    Q_INVOKABLE QString groupOf(const QString& key) const;
    Q_INVOKABLE QString labelOf(const QString& key) const;
    /// The SeatHub strings an enum setting accepts, in the order the dropdown shows them.
    Q_INVOKABLE QStringList enumOptions(const QString& key) const;
    /// True when this key has no control of its own because a SeatHub control merges it (D-11).
    Q_INVOKABLE bool isMerged(const QString& key) const;
    /// The legacy keys folded into the merged control, for the audit trail.
    Q_INVOKABLE QStringList mergedSources(const QString& key) const;
    /// True when this row is not rendered on the page: either D-25 removed it and forced its
    /// value, or it was never part of upstream's own settings page to begin with (D-47).
    Q_INVOKABLE bool isDropped(const QString& key) const;
    Q_INVOKABLE QString dropReason(const QString& key) const;
    Q_INVOKABLE QStringList droppedKeys() const;
    /// True for the five D-25(b)/(c) keys corrected back to their fixed value on every load -
    /// distinct from a key that is merely not rendered (`packetsize`, `defaultver`, OD-06),
    /// whose stored value is left exactly as it is.
    Q_INVOKABLE bool isForced(const QString& key) const;

    // ------------------------------------------------------------- read and write (STREAM-02)

    /// The value in force right now: the session override if one exists, else the saved value.
    Q_INVOKABLE QVariant getValue(const QString& key) const;
    /// The value persisted in `StreamingPreferences`, ignoring session overrides.
    Q_INVOKABLE QVariant getSavedValue(const QString& key) const;
    /// Write-through. Returns false - and writes nothing - while a stream is active, for an
    /// unknown key, or for a value the setting does not accept.
    Q_INVOKABLE bool setValue(const QString& key, const QVariant& value);
    Q_INVOKABLE QVariantMap effectiveValues() const;

    // --------------------------------------------------------------- merged controls (D-11)

    /// Resolution is one control over `width` + `height`; "custom" means the pair is neither
    /// of the four upstream presets (1280x720, 1920x1080, 2560x1440, 3840x2160).
    Q_INVOKABLE QString resolutionPreset() const;
    Q_INVOKABLE bool setResolutionPreset(const QString& preset);
    Q_INVOKABLE QStringList resolutionPresets() const;

    /// Frame rate is one control over `fps`; "Custom" means the value is neither of the two
    /// upstream-fixed presets (30, 60), matching upstream's own "30 FPS / 60 FPS / Custom" set.
    Q_INVOKABLE QString frameRatePreset() const;
    Q_INVOKABLE bool setFrameRatePreset(const QString& preset);
    Q_INVOKABLE QStringList frameRatePresets() const;

    /// Display mode is one control over `windowmode` + its legacy predecessor `fullscreen`.
    Q_INVOKABLE QString displayMode() const;
    Q_INVOKABLE bool setDisplayMode(const QString& mode);
    Q_INVOKABLE QStringList displayModes() const;

    /// The launcher window's own display mode is one control over `uidisplaymode` + its legacy
    /// predecessor `startwindowed`.
    Q_INVOKABLE QString launchDisplayMode() const;
    Q_INVOKABLE bool setLaunchDisplayMode(const QString& mode);
    Q_INVOKABLE QStringList launchDisplayModes() const;

    /// ADR-0042: the checkbox yields "never" when unchecked, and the dropdown yields
    /// "fullscreen" or "always" when it is checked.
    Q_INVOKABLE QString captureSysKeysMode() const;
    Q_INVOKABLE bool setCaptureSysKeysMode(const QString& mode);
    Q_INVOKABLE QStringList captureSysKeysModes() const;

    /// The bound the bitrate slider/field accepts right now: upstream widens it from 150000 to
    /// 500000 Kbps only while `unlockbitrate` is on (`streamingpreferences.h`, D-26).
    Q_INVOKABLE int bitrateMaximum() const;

    // ------------------------------------------------------- performance stats toggles (CUST-17)

    /// One boolean per line Moonlight's own `stringifyVideoStats()` writes (D-23, D-26), in the
    /// order `copy.md` § Settings lists them. Stored under their own SeatHub keys in the same
    /// preference store (D-12) - not `StreamingPreferences` members, since that file is upstream's.
    Q_INVOKABLE QStringList statsToggleKeys() const;
    /// The line's own verbatim label (`copy.md` § Settings), with no number in it (OD-03).
    Q_INVOKABLE QString statsToggleLabel(const QString& statsKey) const;
    Q_INVOKABLE bool getStatsToggle(const QString& statsKey) const;
    /// Writes the toggle and recomputes the derived `showperfoverlay` value: on when any toggle
    /// is on, off when none is (CUST-17). Refused while streaming, like any other write.
    Q_INVOKABLE bool setStatsToggle(const QString& statsKey, bool value);
    /// The label text of every stats toggle currently on, in the engine's own line order (D-26) -
    /// what Plan 12's overlay filter (`OverlayManager::setDebugLineFilter()`, the D-28 exception)
    /// draws. This is the one place both this bridge and that filter read the label catalogue
    /// from; the filter itself carries no copy. Empty when every toggle is off.
    Q_INVOKABLE QStringList enabledStatsLabels() const;

    // ------------------------------------------------- negotiated results and warnings (D-14)

    /// True once the engine has reported `connectionStarted` for the current session.
    Q_INVOKABLE bool hasNegotiatedResults() const;
    /// What the session actually used for this setting. Invalid (empty) before a connection
    /// starts; the effective value once it has, unless the engine reported a fallback, in which
    /// case this is the value the fallback settled on.
    Q_INVOKABLE QVariant getNegotiatedValue(const QString& key) const;
    Q_INVOKABLE QVariantMap getNegotiatedValues() const;
    Q_INVOKABLE QVariantMap negotiationWarnings() const;
    Q_INVOKABLE bool hasNegotiationWarning(const QString& key) const;
    Q_INVOKABLE QString negotiationWarning(const QString& key) const;

    // ------------------------------------------------------ session overrides (D-37 / WR-05)

    /// Apply the session authorization's `quality_profile` for this launch only. The value is
    /// the OpenAPI `QualityProfile` enum (`1080p60 | 1080p75 | 1080p120`, `openapi.yaml`
    /// §QualityProfile) - it names a resolution and a frame rate and nothing else, so those are
    /// the only fields it overrides. The customer's saved bitrate, codec and HDR selections are
    /// left alone. Nothing is persisted.
    Q_INVOKABLE void applySessionOverride(const QString& qualityProfile);
    /// Drop every override. Called when the session finishes; the saved values were never
    /// touched, so the Settings page simply goes back to showing them.
    Q_INVOKABLE void clearSessionOverrides();

    // ------------------------------------------- lifecycle hooks, called by SeatHubClient

    /// Pitfall 6: while this is true every write is refused.
    void setStreamingActive(bool active);
    /// D-25/T-05-45: called by `SeatHubClient` at the start of every connect attempt, before the
    /// engine is started (`beginSession()`/`beginLocalAttempt()`) and therefore before
    /// `app/streaming/session.cpp` reads `StreamingPreferences` to build the connection. A value
    /// edited by hand in the store between sessions is corrected here, on every load - not only
    /// the first - so the engine never reads a forced key's stale value.
    void prepareForSession();
    /// D-14: from here on a negotiated value can be reported.
    void noteConnectionStarted();
    /// D-14/D-37: the session is over - negotiated results go stale and overrides are dropped.
    void noteSessionFinished();
    /// The engine's public `displayLaunchWarning(QString)` seam. The text is matched to the
    /// setting it concerns and kept for diagnostics only; what the customer reads is SeatHub's
    /// own sentence.
    void noteLaunchWarning(const QString& engineText);

    bool writable() const;
    bool sessionActive() const;
    bool hasSessionOverrides() const;

signals:
    void writableChanged();
    void sessionActiveChanged();
    void sessionOverridesChanged();
    void negotiationChanged();
    void valueChanged(const QString& key);

private:
    /// The one place a preference is actually written, after the guard and the value check.
    bool writeThrough(const QString& key, const QVariant& value);
    /// SeatHub's own captured values, applied once on first run where ADR-0042 freezes a
    /// default that differs from upstream's (capture-system-keys: checked + "In fullscreen").
    void applySeatHubDefaults();
    /// D-25/D-25a: `language`, `richpresence`, `mdns`, `detectnetblocking` and `quitAppAfter` are
    /// corrected back to their fixed value and saved, on every load - not only the first
    /// (T-05-45). `hostaudio` is deliberately excluded (D-25a amendment): it stays user-editable.
    void applyForcedValues();
    /// D-23/CUST-17: `showperfoverlay` follows the eleven stats toggles - on when any is on, off
    /// when none is - recomputed on every load and on every toggle write.
    void recomputeShowPerfOverlay();
    /// The setting an engine warning is about, or an empty string when it cannot be attributed.
    static QString warningKeyFor(const QString& engineText);

    StreamingPreferences* m_preferences = nullptr;

    bool m_streaming = false;
    bool m_connectionStarted = false;

    QHash<QString, QVariant> m_overrides;
    QHash<QString, QString> m_warningSentences;
    QHash<QString, QString> m_warningDiagnostics;
};
