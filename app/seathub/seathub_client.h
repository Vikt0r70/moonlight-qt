#pragma once

// The single object QML talks to (D-35).
//
// Ported from the retired Tauri client's `src-tauri/src/commands.rs` (the `#[tauri::command]`
// facade) and `state.rs`. The rules that carried over verbatim:
//
//   * One facade owns every layer below it; QML never reaches past it.
//   * States are typed properties, not JSON. QML never parses a payload (D-35).
//   * No credential ever crosses into the view layer - not a token, not a header.
//
// Plan 03-02 is the tracer: the control-plane calls are stubbed, so this class proves the
// UI lifecycle and the branding, not network auth. Plan 03-03 replaces the stubbed
// `requestOtp`/`verifyOtp` bodies with the real `ControlPlaneClient`.

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QWindow>

#include "error_map.h"
#include "session_lifecycle.h"
// Included rather than forward-declared: moc needs complete types for the `SettingsBridge*` and
// `UpdateFeedClient*` properties below (a bare forward declaration fails the pointer-metatype
// static_assert in Qt's meta-object code).
#include "settings_bridge.h"
#include "update_feed_client.h"

class SeatHubClient : public QObject
{
    Q_OBJECT

    /// One of: "signed_out" | "home" | "connecting" | "streaming" | "error" (D-35).
    Q_PROPERTY(QString appState READ appState NOTIFY appStateChanged)

    /// The engine lifecycle this client drives.
    Q_PROPERTY(SessionLifecycle* session READ session CONSTANT)

    /// The write-through settings bridge. The Settings page reads and writes through this and
    /// nothing else (STREAM-02, D-11, D-13).
    Q_PROPERTY(SettingsBridge* settings READ settings CONSTANT)

    /// The release-feed client behind the forced-update modal (D-38, D-41).
    Q_PROPERTY(UpdateFeedClient* updates READ updates CONSTANT)

    /// True while the Settings page is showing. Settings are a view inside the home state, not
    /// an appState of their own: a session can end while the page is open and the page must
    /// still be the right view when it does.
    Q_PROPERTY(bool inSettings READ inSettings NOTIFY inSettingsChanged)

    /// Customer-facing connecting line from `docs/spec/copy.md` §Play flow. Never the
    /// engine's own stage name.
    Q_PROPERTY(QString stageText READ stageText NOTIFY stageTextChanged)

    /// The mapped failure, or an empty map. `diagnostic` is never present here (D-51).
    Q_PROPERTY(QVariantMap failure READ failure NOTIFY failureChanged)

    /// `SH-XXXXXX` for the current failure, or empty (ADR-0008).
    Q_PROPERTY(QString reference READ reference NOTIFY failureChanged)

    /// The signed-in identity (the phone number). Never a credential.
    Q_PROPERTY(QString identity READ identity NOTIFY identityChanged)

public:
    explicit SeatHubClient(QObject* parent = nullptr);

    QString appState() const { return m_appState; }
    SessionLifecycle* session() const { return m_session; }
    SettingsBridge* settings() const { return m_settings; }
    UpdateFeedClient* updates() const { return m_updates; }
    bool inSettings() const { return m_inSettings; }
    QString stageText() const { return m_stageText; }
    QVariantMap failure() const { return m_failure; }
    QString reference() const;
    QString identity() const { return m_identity; }

    /// The Qt window the visibility sequence hides and restores. Called once by main.qml.
    Q_INVOKABLE void setHostWindow(QWindow* window);

    /// Play (D-35). Moves appState home -> connecting and drives the engine lifecycle.
    Q_INVOKABLE void start();

    /// D-02: end the active stream immediately.
    Q_INVOKABLE void interrupt();

    /// Sign-in step 1 (D-55 shell, stubbed in Plan 03-02).
    Q_INVOKABLE void requestOtp(const QString& phoneE164);

    /// Sign-in step 2 (D-55 shell, stubbed in Plan 03-02).
    Q_INVOKABLE void verifyOtp(const QString& phoneE164, const QString& code);

    /// Returns to the signed-out view and forgets the in-memory identity.
    Q_INVOKABLE void signOut();

    /// Leaves the error state for the home view.
    Q_INVOKABLE void dismissError();

    /// Shows the Settings page (a view inside the home state).
    Q_INVOKABLE void openSettings();

    /// Returns to the home view.
    Q_INVOKABLE void closeSettings();

signals:
    void appStateChanged();
    void stageTextChanged();
    void failureChanged();
    void identityChanged();
    void inSettingsChanged();

    /// Step 1 succeeded - the view should show the code field.
    void otpRequested(const QString& phoneE164);

    /// Step 2 rejected the code. `message` is SeatHub copy, never engine text.
    void otpRejected(const QString& message, const QString& reference);

    /// Step 2 accepted the code.
    void otpAccepted();

private slots:
    void handleStageStarting(const QString& stage);
    void handleStageFailed(const QString& stage, int errorCode, const QString& failingPorts);
    void handleConnectionStarted();
    void handleDisplayLaunchError(const QString& text);
    void handleDisplayLaunchWarning(const QString& text);
    void handleQuitStarting();
    void handleSessionFinished(int portTestResult);
    void handleReadyForDeletion();

private:
    void setAppState(const QString& state);
    void setStageText(const QString& text);
    void raiseFailure(const SeatHubFailure& failure);
    void clearFailure();
    void setInSettings(bool inSettings);

    QString m_appState;
    QString m_stageText;
    QVariantMap m_failure;
    QString m_identity;
    QWindow* m_hostWindow = nullptr;
    SessionLifecycle* m_session = nullptr;
    SettingsBridge* m_settings = nullptr;
    UpdateFeedClient* m_updates = nullptr;
    bool m_inSettings = false;
};
