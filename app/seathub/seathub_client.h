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

#include "authorized_through_timer.h"
#include "control_plane_client.h"
#include "error_map.h"
#include "hud_overlay.h"
#include "liveness_timer.h"
#include "pairing_controller.h"
#include "pairing_seam.h"
#include "session_lifecycle.h"
#include "session_websocket.h"
// Included rather than forward-declared: moc needs complete types for the `SettingsBridge*` and
// `UpdateFeedClient*` properties below (a bare forward declaration fails the pointer-metatype
// static_assert in Qt's meta-object code).
#include "settings_bridge.h"
#include "teardown_controller.h"
#include "token_store.h"
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

    /// The control-plane bridge (D-29). Holds the session's opaque access token; the token is
    /// never a property and never crosses into a view (D-35).
    Q_PROPERTY(ControlPlaneClient* controlPlane READ controlPlane CONSTANT)

    /// The control plane's live session channel, `/ws/session/{session_id}` (D-29). The UI
    /// reads billing and warning state from here via the signals below, never by parsing a
    /// frame itself.
    Q_PROPERTY(SessionWebSocket* sessionChannel READ sessionChannel CONSTANT)

    /// Silent pairing (D-21, D-22, STREAM-03). Exposed so the connecting view can report
    /// progress; it carries no PIN and no token.
    Q_PROPERTY(PairingController* pairing READ pairing CONSTANT)

    /// Disable -> remove -> verify, then leave nothing behind (D-10, STREAM-10).
    Q_PROPERTY(TeardownController* teardown READ teardown CONSTANT)

    /// The most recent `session.billing` frame: `minutes_billed`, `balance_minutes`,
    /// `minute_index`. Server-provided facts the client displays; it never computes, adjusts or
    /// anticipates them (`docs/spec/client.md` §Wallet authority).
    Q_PROPERTY(QVariantMap billing READ billing NOTIFY billingChanged)

    /// The most recent `session.warning` enum value (`LOW_BALANCE | SAVE_NOW | DISCONNECTED |
    /// RECONNECT_LIMIT_NEAR | OWNER_RESERVATION_NEAR`), or empty. The sentence the customer
    /// reads for it comes from `docs/spec/copy.md` and is chosen in QML.
    Q_PROPERTY(QString sessionWarning READ sessionWarning NOTIFY sessionWarningChanged)

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

    /// The home screen's own state: "ready" | "checking" | "busy" | "offline" (audit F1).
    /// Separate from `appState` because all four are the home view: "checking" while Play's
    /// allocation request is in flight, "busy" when the control plane refused it with
    /// `NO_HOST_AVAILABLE` (`copy.md` §Play flow, "No rig"), "offline" when the control plane
    /// could not be reached at all (`copy.md` §Support & errors, "Offline"), and "ready" for the
    /// populated state `screens.md` §23 draws. QML turns each into a sentence; none of them is a
    /// failure, so none of them raises the error screen.
    Q_PROPERTY(QString homeStatus READ homeStatus NOTIFY homeStatusChanged)

    /// The last session's end reason as the sentence `docs/spec/copy.md` §Session end reasons
    /// gives it, or empty. Styled text: the minute count in it is wrapped in the mono family,
    /// because every number with a unit is mono (copy.md §5). The bare enum never reaches QML.
    Q_PROPERTY(QString endReasonText READ endReasonText NOTIFY endReasonTextChanged)

    /// The signed-in identity (the phone number). Never a credential.
    Q_PROPERTY(QString identity READ identity NOTIFY identityChanged)

public:
    explicit SeatHubClient(QObject* parent = nullptr);
    /// Stops the control-plane thread and joins it before anything that could still be running
    /// on it is destroyed. A reply in flight during shutdown would otherwise call back into a
    /// half-destroyed facade.
    ~SeatHubClient() override;

    QString appState() const { return m_appState; }
    SessionLifecycle* session() const { return m_session; }
    SettingsBridge* settings() const { return m_settings; }
    UpdateFeedClient* updates() const { return m_updates; }
    ControlPlaneClient* controlPlane() const { return m_controlPlane; }
    SessionWebSocket* sessionChannel() const { return m_sessionChannel; }
    PairingController* pairing() const { return m_pairing; }
    TeardownController* teardown() const { return m_teardown; }
    QVariantMap billing() const { return m_billing; }
    QString sessionWarning() const { return m_sessionWarning; }
    bool inSettings() const { return m_inSettings; }
    QString stageText() const { return m_stageText; }
    QVariantMap failure() const { return m_failure; }
    QString reference() const;
    QString identity() const { return m_identity; }
    QString homeStatus() const { return m_homeStatus; }
    QString endReasonText() const { return m_endReasonText; }

    /// The Qt window the visibility sequence hides and restores. Called once by main.qml.
    Q_INVOKABLE void setHostWindow(QWindow* window);

    /// Play (D-35). Asks the control plane for a session (`POST /api/sessions`) when there is an
    /// access token to ask with, then drives the engine lifecycle from the allocation. Without a
    /// token - the documented Plan 03-02 tracer path - it drives the engine lifecycle directly.
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

    /// ErrorScreen's retry (audit F21). Clears the failure and re-runs the step that failed:
    /// Play when there is an identity to play with, the sign-in screen when there is not.
    Q_INVOKABLE void retry();

    /// Shows the Settings page (a view inside the home state).
    Q_INVOKABLE void openSettings();

    /// Returns to the home view.
    Q_INVOKABLE void closeSettings();

    /// Attach a control-plane session to this client: the real path, as opposed to the tracer's
    /// stubbed stage sequence. Starts the session channel, runs silent pairing, and hands the
    /// session authorization's `quality_profile` to the settings bridge as an in-memory override
    /// for this launch only (D-37, WR-05).
    ///
    /// The access token must already have been set on the control-plane client by a successful
    /// sign-in. Nothing here reads it, returns it, or logs it.
    Q_INVOKABLE void beginSession(const QString& sessionId);

    /// The DPAPI-backed credential store. Exposed for teardown's benefit and for `signOut()`;
    /// its contents are never a property.
    TokenStore* credentialStore() const { return m_tokenStore; }

signals:
    void appStateChanged();
    void stageTextChanged();
    void failureChanged();
    void identityChanged();
    void inSettingsChanged();
    void billingChanged();
    void sessionWarningChanged();
    void homeStatusChanged();
    void endReasonTextChanged();

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

    // The control plane's session channel (`/ws/session/{session_id}`).
    void handleSessionState(const SessionInfo& session);
    void handleSessionBilling(const QString& sessionId, int minutesBilled, int balanceMinutes,
                              int minuteIndex);
    void handleSessionWarning(const QString& sessionId, const QString& warning,
                              const QString& deadlineAt);

    // D-33: the only locally enforced end. Liveness failure is not one (see `onLivenessWarning`).
    void handleHorizonReached();
    void onLivenessWarning();
    void handleTeardownCompleted();
    void handleTeardownFailed(const SeatHubFailure& failure);
    void handlePairingCompleted(const QString& clientUuid);
    void handlePairingFailed(const SeatHubFailure& failure);

private:
    void setAppState(const QString& state);
    void setStageText(const QString& text);
    void setHomeStatus(const QString& status);
    void setEndReasonText(const QString& text);
    void raiseFailure(const SeatHubFailure& failure);
    void clearFailure();
    void setInSettings(bool inSettings);
    /// True once `beginSession()` has attached a real control-plane session.
    bool inControlPlaneSession() const;
    /// The tracer path: no control-plane session, so the engine lifecycle runs on its own
    /// (Plan 03-02's documented interim, kept for the no-token case).
    void beginLocalAttempt();
    /// Play against the control plane: `POST /api/sessions`, then `beginSession()`.
    void beginPlayRequest();
    /// Turns an allocation result into a home state: a refusal is the empty state, an
    /// unreachable control plane is the offline state, and anything else is a real failure.
    void applyPlayFailure(const ControlPlaneResult& result);
    /// Move the network objects onto a thread with a running event loop. Upstream suspends Qt
    /// processing for the whole stream (`session.cpp:1965-1966`), so a timer or socket left on
    /// the main thread would be silent for exactly the interval it exists to cover.
    void startNetworkThreads();

    QString m_appState;
    QString m_stageText;
    QString m_homeStatus;
    QString m_endReasonText;
    QVariantMap m_failure;
    QString m_identity;
    QWindow* m_hostWindow = nullptr;
    SessionLifecycle* m_session = nullptr;
    SettingsBridge* m_settings = nullptr;
    UpdateFeedClient* m_updates = nullptr;

    // The control-plane bridge (Plan 03-03). `m_tokenStore` is the DPAPI store and the only
    // place a credential is ever at rest (D-30); `m_sessionChannel` is the control plane's
    // `/ws/session/{id}`; `m_pairing` and `m_teardown` are the two safety-critical sequences.
    ControlPlaneClient* m_controlPlane = nullptr;
    TokenStore* m_tokenStore = nullptr;
    SessionWebSocket* m_sessionChannel = nullptr;
    PairingController* m_pairing = nullptr;
    /// The production pairing seam (the gap 03-03 left open). Owned here, moved to the network
    /// thread with the controller it serves, and never exposed: it is the only object in the
    /// process that ever holds the PIN, and it holds it only for the length of one handshake.
    ProductionPairingSeam* m_pairingSeam = nullptr;
    TeardownController* m_teardown = nullptr;

    // D-31/D-34 and D-33. Both must outlive the stream and neither is a Q_PROPERTY: the UI has
    // no business starting or stopping either one.
    LivenessTimer* m_liveness = nullptr;
    AuthorizedThroughTimer* m_horizon = nullptr;

    /// The session the real control-plane path is running, or empty on the tracer path.
    QString m_sessionId;
    /// What pairing returned about this client: the SHA-256 fingerprint of its own certificate,
    /// which is the identity a host-side reader of Sunshine's client list can match to this
    /// client's record. It is the only thing that identifies this client - never the rig's name,
    /// address or index (Pitfall 3, D-07). The Sunshine-assigned UUID is a different value and
    /// this process cannot read it (see `pairing_seam.h`).
    QString m_clientUuid;
    QVariantMap m_billing;
    QString m_sessionWarning;

    // The D-56 in-session HUD: a duration timer and the End session affordance, composited into
    // the stream's own swapchain (ADR-0045). It is deliberately not a Q_PROPERTY - no QML view
    // reads it, because the HUD is not QML on this tier; the lifecycle drives it and the
    // publisher composites it.
    HudOverlay m_hud;
    bool m_inSettings = false;
};
