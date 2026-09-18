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

    // ---------------------------------------------------------------- the catalogue (D-11/D-48)

    /// The D-48 group order: Video, Audio, Input, Network, Advanced.
    Q_INVOKABLE QStringList groups() const;
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
    /// True when SeatHub manages the key internally and shows no control for it (D-47).
    Q_INVOKABLE bool isDropped(const QString& key) const;
    Q_INVOKABLE QString dropReason(const QString& key) const;
    Q_INVOKABLE QStringList droppedKeys() const;

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
    /// The setting an engine warning is about, or an empty string when it cannot be attributed.
    static QString warningKeyFor(const QString& engineText);

    StreamingPreferences* m_preferences = nullptr;

    bool m_streaming = false;
    bool m_connectionStarted = false;

    QHash<QString, QVariant> m_overrides;
    QHash<QString, QString> m_warningSentences;
    QHash<QString, QString> m_warningDiagnostics;
};
