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
// Plan 03-02 shipped this against a stubbed control plane and a tracer in place of a stream.
// Both are gone: `requestOtp`/`verifyOtp` run the real `ControlPlaneClient`, and the stream is a
// real engine session the pairing handshake's resolved host is attached to. The tracer survives as
// an explicitly injected fake (`stub_engine_session.h`), never as a fallback.

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QWindow>

#include <functional>

#include "authorized_through_timer.h"
#include "control_plane_client.h"
#include "customer_lists.h"
#include "engine_session.h"
#include "error_map.h"
#include "hud_overlay.h"
#include "liveness_timer.h"
#include "log_tee.h"
#include "moonlight_engine_session.h"
#include "pairing_controller.h"
#include "pairing_seam.h"
#include "session_lifecycle.h"
#include "session_websocket.h"
#include "stream_stats.h"
// Included rather than forward-declared: moc needs complete types for the `SettingsBridge*` and
// `UpdateFeedClient*` properties below (a bare forward declaration fails the pointer-metatype
// static_assert in Qt's meta-object code).
#include "settings_bridge.h"
#include "teardown_controller.h"
#include "teardown_guard.h"
#include "token_store.h"
#include "update_feed_client.h"

class SeatHubClient : public QObject
{
    Q_OBJECT

    /// One of: "restoring" | "signed_out" | "home" | "connecting" | "streaming" | "error" (D-35).
    /// "restoring" is the state the client is constructed in: the stored credential is being read
    /// and confirmed (`restoreSession()`), and the view shows the restore splash - never the
    /// sign-in form - until that resolves to "home" or "signed_out".
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

    /// The control plane's live session channel, `/ws/session/{session_id}` (D-29). The server
    /// serves no such route, so this client no longer opens it (`ADR-0055`): the class stays
    /// declared, and the handlers below still accept what it would deliver, but the session is read
    /// on the pairing poll instead (`handleSessionState`).
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

    /// Customer-facing connecting line from `docs/spec/copy.md` §Play flow: the line of the highest
    /// stage reached, or empty before anything has been read. Never the engine's own stage name.
    Q_PROPERTY(QString stageText READ stageText NOTIFY stageTextChanged)

    /// The highest connecting stage reached, 0 to 3 (`screens.md` §24): 1 `Preparing the rig`, 2
    /// `Preparing the stream`, 3 `Streaming`. 0 until the first answer about the session has come
    /// back, and then no stage is done (the first is simply where a session that exists begins).
    /// Every step is a real transition: the session's own state as the control plane reports it, the
    /// engine's own stages (inside stage 2) and the stream starting. It only ever moves forward; no
    /// timer moves it.
    Q_PROPERTY(int connectStage READ connectStage NOTIFY connectStageChanged)

    /// True when connecting stopped and the view is showing where. The stage that was active stays
    /// the highest one reached (it is what the view marks failed); `failure` and `reference` hold
    /// what the server or the client said, and `stalledStepText` and `stalledReasonText` are what
    /// the view prints (CUST-13). The app state stays "connecting" throughout: `Try again` (`retry()`)
    /// and `Back to home` (`dismissError()`) are the two ways out, and neither offers another rig.
    Q_PROPERTY(bool connectFailed READ connectFailed NOTIFY connectFailedChanged)

    /// `Stopped at: {stage}` (`copy.md` §Play flow, C5), naming the stage that was active. Empty
    /// unless `connectFailed`.
    Q_PROPERTY(QString stalledStepText READ stalledStepText NOTIFY connectFailedChanged)

    /// The sentence that goes with it, as styled text: the copy deck's end-reason sentence for what the
    /// server decided (its minute count in the mono family), or - when the server gave no reason - the
    /// failure's own sentence, escaped. Empty unless `connectFailed`.
    Q_PROPERTY(QString stalledReasonText READ stalledReasonText NOTIFY connectFailedChanged)

    /// The mapped failure, or an empty map. `diagnostic` is never present here (D-51).
    Q_PROPERTY(QVariantMap failure READ failure NOTIFY failureChanged)

    /// `SH-XXXXXX` for the current failure, or empty (ADR-0008).
    Q_PROPERTY(QString reference READ reference NOTIFY failureChanged)

    /// The home screen's own state: "ready" | "checking" | "busy" | "offline" | "refused"
    /// (audit F1, `screens.md` §23). Separate from `appState` because all five are the home view:
    /// "checking" while Play's allocation request is in flight, "busy" when the control plane refused
    /// it with `NO_HOST_AVAILABLE` (`copy.md` §Play flow, "No rig"), "offline" when the control plane
    /// could not be reached at all (`copy.md` §Support & errors, "Offline"), "refused" when the
    /// control plane answered and said no in its own words (the balance floor, or a session this
    /// customer already has) - `failure` and `reference` then hold that sentence and its code, shown
    /// as they are - and "ready" for the populated state `screens.md` §23 draws. QML turns each into
    /// a sentence; none of them is a failure, so none of them raises the error screen.
    Q_PROPERTY(QString homeStatus READ homeStatus NOTIFY homeStatusChanged)

    /// True while a control-plane session is attached and the server has not reported it over: Home
    /// then reads `Resume session` in place of `Play`, and `start()` resumes that session instead of
    /// asking for a new one. It is derived from what this client itself knows (an attached session
    /// that has not ended), not from a server read: nothing discovers a live session at launch, so
    /// after a restart it is false until a session is attached again (`.planning/WINDOWS.md`, the
    /// resume-after-restart row). The connecting reads of `handleSessionState` do not set it.
    Q_PROPERTY(bool liveSession READ liveSession NOTIFY liveSessionChanged)

    /// The last session's end reason as the sentence `docs/spec/copy.md` §Session end reasons
    /// gives it, or empty. Styled text: the minute count in it is wrapped in the mono family,
    /// because every number with a unit is mono (copy.md §5). The bare enum never reaches QML.
    Q_PROPERTY(QString endReasonText READ endReasonText NOTIFY endReasonTextChanged)

    /// The signed-in identity: the first of phone, email, username and display name the account
    /// has. Never a credential.
    Q_PROPERTY(QString identity READ identity NOTIFY identityChanged)

    /// What `GET /api/me` said about the signed-in account, as a map of `display_name`,
    /// `username`, `email`, `phone` and `email_verified`. Empty until a restore or a sign-in has
    /// read it. Never a credential.
    Q_PROPERTY(QVariantMap account READ account NOTIFY identityChanged)

    /// The customer's balance in whole minutes as `GET /api/wallet` last reported it, or -1 when
    /// no read has ever succeeded. The client does no arithmetic on it (CUST-06,
    /// `docs/spec/client.md` §Wallet authority); the view compares it to the warning thresholds
    /// and nothing else.
    Q_PROPERTY(qint64 balanceMinutes READ balanceMinutes NOTIFY balanceChanged)

    /// `balanceMinutes` in the hours-and-minutes format (`duration_text.h`), or empty when no read
    /// has ever succeeded.
    Q_PROPERTY(QString balanceText READ balanceText NOTIFY balanceChanged)

    /// True when the most recent wallet read failed. The value then on screen is the last known
    /// one, and the view says so; a failed read never blanks it and never signs anybody out.
    Q_PROPERTY(bool balanceStale READ balanceStale NOTIFY balanceChanged)

    /// The bundled country list the sign-in field's tag and picker read: `{iso, name, dial}` rows,
    /// name-sorted, read from the binary (`countries.h`). Nothing is fetched.
    Q_PROPERTY(QVariantList countries READ countries CONSTANT)

    /// The ISO code the country tag starts on: the machine's own region, then the locale's
    /// territory, then Jordan (`region.h`). Always a row in `countries`.
    Q_PROPERTY(QString defaultCountryCode READ defaultCountryCode CONSTANT)

    /// False when Windows' "Animation effects" setting is off, so the country tag appears and hides
    /// without its transition (`screens.md` §22). True everywhere else.
    Q_PROPERTY(bool animationEffects READ animationEffects CONSTANT)

    /// True from a confirmed sign-in (or an offline restore, where the stored credential is kept and
    /// trusted) until sign-out. The shell puts the header, and the balance in it, on a view only
    /// while this holds: there is no balance before sign-in (CUST-06, `screens.md` Common shell).
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY signedInChanged)

    /// True while the profile is showing. Like Settings it is a view inside the home state, not an
    /// appState of its own, so it keeps the signed-in header and a session that ends while it is open
    /// still lands on the right view (`screens.md` §27).
    Q_PROPERTY(bool inProfile READ inProfile NOTIFY inProfileChanged)

    /// The profile's three lists (CUST-14, D-14). Constant objects the view binds to; each is
    /// independent of the others and of the totals and the identity rows. QML never sees raw data:
    /// every row is already the text the screen draws.
    Q_PROPERTY(SessionListModel* sessionHistory READ sessionHistory CONSTANT)
    Q_PROPERTY(CreditHistoryModel* creditHistory READ creditHistory CONSTANT)
    Q_PROPERTY(TopupListModel* topupHistory READ topupHistory CONSTANT)

    /// The profile's two totals, both the server's own numbers (`GET /api/usage`) in the hours-and-
    /// minutes format (`duration_text.h`): `Hours played` (the sum of session-debit minutes, OD-09)
    /// and `Credit left`. Empty until the read has succeeded. `totalsStatus` is "idle" | "loading" |
    /// "ready" | "error"; on "error" `totalsError` is the server's sentence (or the deck's offline or
    /// generic one) and `totalsErrorReference` its ADR-0008 code, when it gave one.
    Q_PROPERTY(QString hoursPlayedText READ hoursPlayedText NOTIFY totalsChanged)
    Q_PROPERTY(QString creditLeftText READ creditLeftText NOTIFY totalsChanged)
    Q_PROPERTY(QString totalsStatus READ totalsStatus NOTIFY totalsChanged)
    Q_PROPERTY(QString totalsError READ totalsError NOTIFY totalsChanged)
    Q_PROPERTY(QString totalsErrorReference READ totalsErrorReference NOTIFY totalsChanged)

    /// Whether the identity rows (`account`) can be drawn: "ready" once `GET /api/me` has said who
    /// the customer is, "loading" while it has not (and no failure is recorded), "error" when the
    /// read failed and there is nothing to show. `accountError` and `accountErrorReference` are the
    /// sentence and code for that failure.
    Q_PROPERTY(QString accountStatus READ accountStatus NOTIFY accountStatusChanged)
    Q_PROPERTY(QString accountError READ accountError NOTIFY accountStatusChanged)
    Q_PROPERTY(QString accountErrorReference READ accountErrorReference NOTIFY accountStatusChanged)

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
    /// The in-stream HUD and the liveness reporter that feeds it a balance. Exposed the way
    /// `session()` and `teardown()` are: so a test can drive the real objects through the real
    /// facade. Nothing in the app reads them through here.
    HudOverlay* hud() { return &m_hud; }
    LivenessTimer* liveness() const { return m_liveness; }
    QVariantMap billing() const { return m_billing; }
    QString sessionWarning() const { return m_sessionWarning; }
    bool inSettings() const { return m_inSettings; }
    QString stageText() const { return m_stageText; }
    int connectStage() const { return m_connectStage; }
    bool connectFailed() const { return m_connectFailed; }
    QString stalledStepText() const { return m_stalledStepText; }
    QString stalledReasonText() const { return m_stalledReasonText; }
    QVariantMap failure() const { return m_failure; }
    QString reference() const;
    QString identity() const { return m_identity; }
    QVariantMap account() const { return m_account; }
    qint64 balanceMinutes() const { return m_balanceMinutes; }
    QString balanceText() const { return m_balanceText; }
    bool balanceStale() const { return m_balanceStale; }
    QString homeStatus() const { return m_homeStatus; }
    QString endReasonText() const { return m_endReasonText; }
    QVariantList countries() const { return m_countries; }
    QString defaultCountryCode() const { return m_defaultCountryCode; }
    bool animationEffects() const { return m_animationEffects; }
    bool signedIn() const { return m_signedIn; }
    bool liveSession() const { return m_liveSession; }
    bool inProfile() const { return m_inProfile; }
    SessionListModel* sessionHistory() const { return m_sessionHistory; }
    CreditHistoryModel* creditHistory() const { return m_creditHistory; }
    TopupListModel* topupHistory() const { return m_topupHistory; }
    QString hoursPlayedText() const { return m_hoursPlayedText; }
    QString creditLeftText() const { return m_creditLeftText; }
    QString totalsStatus() const { return m_totalsStatus; }
    QString totalsError() const { return m_totalsError; }
    QString totalsErrorReference() const { return m_totalsErrorReference; }
    QString accountStatus() const;
    QString accountError() const { return m_accountError; }
    QString accountErrorReference() const { return m_accountErrorReference; }

    /// The Qt window the visibility sequence hides and restores. Called once by main.qml.
    Q_INVOKABLE void setHostWindow(QWindow* window);

    /// Play (D-35). Asks the control plane for a session (`POST /api/sessions`) when there is an
    /// access token to ask with, then drives the engine lifecycle from the allocation. While a
    /// session is live (`liveSession`) it resumes that session instead of asking for a new one.
    /// Without a token - the documented Plan 03-02 tracer path - it drives the engine lifecycle
    /// directly.
    Q_INVOKABLE void start();

    /// D-02: end the active stream immediately.
    Q_INVOKABLE void interrupt();

    /// Sign-in step 1 (D-55 shell, stubbed in Plan 03-02).
    Q_INVOKABLE void requestOtp(const QString& phoneE164);

    /// Sign-in step 2 (D-55 shell, stubbed in Plan 03-02).
    Q_INVOKABLE void verifyOtp(const QString& phoneE164, const QString& code);

    /// What a phone number typed into the sign-in field becomes as E.164, or an empty string when
    /// it is not one. `dialCode` is the chosen country's (`+962`); a number already written with a
    /// leading `+` or `00` is believed as it stands. The screen asks before it sends anything, so a
    /// mis-typed number is named on the field and never spends a round trip.
    Q_INVOKABLE QString toE164(const QString& typed, const QString& dialCode) const;

    /// Sign in with an email and a password (`POST /api/auth/login`, the email step of the
    /// sign-in screen). The identifier is sent as typed (only surrounding space is dropped) and the
    /// server decides what it is. On success the credential is stored exactly as the code path
    /// stores it and the client lands on Home; on refusal the server's own sentence and reference
    /// come back through `passwordSignInRejected`. The password lives for the length of the
    /// request: it is not stored, not kept by this object and not logged.
    Q_INVOKABLE void signInWithPassword(const QString& identifier, const QString& password);

    /// The website address for `target` (`signup`, `reset`, `topup` or `home`, the origin itself that
    /// the profile's `Open the website` link opens), or an empty string for anything else. QML never builds an address itself; these come from `web_origin.h`, carry no
    /// parameters and so can never carry a credential.
    Q_INVOKABLE QString websiteUrl(const QString& target) const;

    /// Opens `websiteUrl(target)` in the customer's browser. False (and nothing opened) for an
    /// unknown target.
    Q_INVOKABLE bool openWebsite(const QString& target);

    /// Opens the website's top-up page (`/topup`) in the customer's browser: the menu's `Top up` and
    /// Home's quiet top-up control (CUST-07). A named call so no view has to spell a target, and no
    /// view ever holds an address; the page carries nothing about the customer (D-07, D-08).
    Q_INVOKABLE bool openTopUp();

    /// Replaces what opens an address (the default is the desktop's browser). A test uses this so
    /// it can see what would have been opened without launching one.
    void setUrlOpener(std::function<bool(const QUrl&)> opener) { m_urlOpener = std::move(opener); }

    /// Launch (CUST-08, D-06): reads the stored credential, confirms it with `GET /api/me`, and
    /// opens on Home. Called once by main.qml; further calls are ignored.
    ///
    ///   * no credential on disk               -> signed_out
    ///   * the control plane says 401          -> the store is cleared, signed_out
    ///   * the control plane cannot be reached -> the customer stays signed in: Home, in its
    ///                                            offline state. A lost network never signs anyone
    ///                                            out.
    ///   * confirmed                           -> the identity is filled from the account, Home,
    ///                                            and the balance is read.
    ///
    /// Before it reads anything it runs `TokenStore::recoverAtStartup()`, so a credential written by
    /// 0.1.4 or parked by an update is in the access slot by then (WINDOWS #22). The log records
    /// that a credential was found, never its bytes.
    Q_INVOKABLE void restoreSession();

    /// Reads `GET /api/wallet` and updates `balanceMinutes` / `balanceText` / `balanceStale`. Runs
    /// itself on every return to Home; exposed so a view can refresh it too.
    Q_INVOKABLE void refreshBalance();

    /// Revokes the credential on the server (`POST /api/auth/logout`), deletes it locally
    /// whatever that reply is, and returns to the signed-out view. The local deletion never waits
    /// on the server: an offline sign-out still leaves nothing on this machine.
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

    /// Shows the profile (a view inside the home state, like Settings). Reads the identity and the two
    /// totals afresh and forgets whatever the three lists held, so each opens on its first page and
    /// nothing from an earlier visit is shown as current. The lists themselves are not asked for here:
    /// the view asks for the one it shows and the others wait until their tab is opened.
    Q_INVOKABLE void openProfile();

    /// Returns to the home view, and forgets the lists (they are the customer's own history and are
    /// not kept while nobody is looking at it).
    Q_INVOKABLE void closeProfile();

    /// `list` is "sessions", "credit" or "topups". Each of the four calls below is a no-op for any
    /// other name.
    ///
    /// Asks for the list's first page, once: a list already asked for, loading, loaded or failed is
    /// left as it is (`reloadList` is how a failed one starts again).
    Q_INVOKABLE void loadFirstPage(const QString& list);

    /// Asks for the list's next page when the server said there is one and no request is in flight;
    /// otherwise does nothing (the view calls this every time it reaches the end of the list).
    Q_INVOKABLE void loadNextPage(const QString& list);

    /// Forgets the list and asks for its first page again: the list's `Try again`.
    Q_INVOKABLE void reloadList(const QString& list);

    /// `Try again` on the totals and on the identity rows: each is read again on its own.
    Q_INVOKABLE void reloadTotals();
    Q_INVOKABLE void reloadAccount();

    /// Attach a control-plane session to this client: the real path, as opposed to the tracer's
    /// stubbed stage sequence. Starts the session channel, runs silent pairing, and hands the
    /// session authorization's `quality_profile` to the settings bridge as an in-memory override
    /// for this launch only (D-37, WR-05).
    ///
    /// The access token must already have been set on the control-plane client by a successful
    /// sign-in. Nothing here reads it, returns it, or logs it.
    Q_INVOKABLE void beginSession(const QString& sessionId);

    /// Reads the host agent's config file the customer picked (the file the Node Agent writes,
    /// `%ProgramData%\SeatHub\node-agent.json`) and reports what may be shown: the file's absolute
    /// path and the agent token in masked form.
    ///
    /// The token itself never crosses this boundary - see `agent_config.h`. A file that is missing,
    /// unreadable or carries no token is reported as an ordinary value (`ok: false`) and logged for
    /// support; it is never raised as a session failure, because the customer chose the file and
    /// nothing about the session depends on it.
    Q_INVOKABLE QVariantMap readAgentConfigFile(const QUrl& fileUrl);

    /// The DPAPI-backed credential store. Exposed for teardown's benefit and for `signOut()`;
    /// its contents are never a property.
    TokenStore* credentialStore() const { return m_tokenStore; }

signals:
    void appStateChanged();
    void stageTextChanged();
    void connectStageChanged();
    void connectFailedChanged();
    void failureChanged();
    void identityChanged();
    void inSettingsChanged();
    void billingChanged();
    void sessionWarningChanged();
    void homeStatusChanged();
    void endReasonTextChanged();
    void balanceChanged();
    void signedInChanged();
    void liveSessionChanged();
    void inProfileChanged();
    void totalsChanged();
    void accountStatusChanged();

    /// Step 1 succeeded - the view should show the code field.
    void otpRequested(const QString& phoneE164);

    /// Step 2 rejected the code. `message` is SeatHub copy, never engine text.
    void otpRejected(const QString& message, const QString& reference);

    /// Step 2 accepted the code.
    void otpAccepted();

    /// The email-and-password sign-in was refused, locally or by the server. `message` is the
    /// sentence to show (the server's own, verbatim, or the offline or field-level one from the
    /// copy deck); `reference` is the ADR-0008 code when the server gave one.
    void passwordSignInRejected(const QString& message, const QString& reference);

    /// The email-and-password sign-in succeeded; the credential is stored and Home is next.
    void passwordSignInAccepted();

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

    /// D-17/Plan 15: the tee's `StatsWatcher` parsed the end-of-stream "Global video stats"
    /// block. Held here with the session it belongs to (`m_sessionId`, still the ending
    /// session's at this point - Pitfall 7) until `handleTeardownCompleted()` posts it once.
    void handleVideoStatsParsed(VideoStats stats);

    // D-33: the only locally enforced end. Liveness failure is not one (see `onLivenessWarning`).
    void handleHorizonReached();
    void onLivenessWarning();
    /// `finalSession` is the terminal read teardown ended on: the source of the end reason Home shows.
    void handleTeardownCompleted(const SessionInfo& finalSession);
    void handleTeardownFailed(const SeatHubFailure& failure);
    void handlePairingCompleted(const QString& clientUuid);
    void handlePairingFailed(const SeatHubFailure& failure);

    /// The control plane authorized the session, and with which quality profile. Applies the
    /// profile to the settings bridge as an in-memory override for this launch only (D-37, WR-05).
    void handleAuthorizationGranted(const QString& qualityProfile);

    /// The host the handshake paired with, emitted by `ProductionPairingSeam` on its success path
    /// only. Builds the engine session from it and attaches it, so that by the time
    /// `handlePairingCompleted` runs there is something for `start()` to drive.
    ///
    /// Queued, not direct: the seam lives on the network thread and this slot builds a Qt object
    /// tree that belongs to the facade's thread.
    void handleHostResolved(const PairedHostPtr& host);

    /// One of the profile's reads was answered 401: the credential is no longer valid. Cleared, and
    /// the customer lands on sign-in (`screens.md` §27), exactly as a sign-out that could not reach
    /// the server does.
    void handleCredentialRefused();

private:
    void setAppState(const QString& state);
    void setStageText(const QString& text);
    /// Moves the highest connecting stage forward to `stage` (1 to 3). A stage at or below the one
    /// already reached, or any move once connecting has stopped, changes nothing and says nothing:
    /// a late reply carrying an earlier state must not move the stepper back, and two states seen in
    /// one tick land on the later stage without the earlier one being shown twice.
    void advanceConnectStage(int stage);
    /// Forgets the stages: a session is starting.
    void resetConnecting();
    /// Forgets a stall (the customer left the stalled view, or a session is starting). Says so only
    /// when there was one.
    void clearStall();
    /// Connecting stopped. Keeps the app state, keeps the stage that was active as the one to mark
    /// failed, and fills the two texts the view prints. `endReason` is the server's, when it gave one;
    /// `minutesBilled` is the server's own count for it. A failure the server gave no reason for
    /// falls back to `failure`'s own sentence, and to the generic one when that is empty too.
    void raiseConnectFailure(const SeatHubFailure& failure, const QString& endReason = QString(),
                             int minutesBilled = 0);
    /// True while the customer is looking at the connecting view for a control-plane session: the
    /// only time a failure becomes a stall rather than the error view.
    bool connectingSession() const;
    void setHomeStatus(const QString& status);
    void setEndReasonText(const QString& text);
    void raiseFailure(const SeatHubFailure& failure);
    void clearFailure();
    void setInSettings(bool inSettings);
    void setInProfile(bool inProfile);
    /// The list named `list` ("sessions", "credit", "topups"), or null for any other name.
    CustomerListModel* listNamed(const QString& list) const;
    /// Forgets the three lists and the totals. The account itself stays: it is the signed-in
    /// customer's identity, and only sign-out clears it.
    void resetProfileData();
    void readTotals();
    void readAccount();
    /// The one writer of `m_signedIn`, so `signedInChanged()` can never be missed.
    void setSignedIn(bool signedIn);
    /// The one writer of `m_sessionId`, and of whether that session has ended, so `liveSession` can
    /// never be left stale by a path that forgot to say so.
    void setAttachedSession(const QString& sessionId);
    void setAttachedSessionEnded(bool ended);
    /// Recomputes `m_liveSession` from the two and says so when it changed.
    void updateLiveSession();
    /// Applies the answer to the launch-time `GET /api/me` (see `restoreSession()`).
    void applyRestoreResult(const ControlPlaneResult& result);
    /// Applies an answer to `GET /api/wallet`. `epoch` is the credential generation the read was
    /// issued under; an answer that arrives after a sign-out (or a different sign-in) is dropped.
    void applyWalletResult(quint64 epoch, const ControlPlaneResult& result);
    /// Takes a balance the liveness report's own wallet read produced during a stream (CUST-15).
    /// Delivered queued to this thread, so it lands whenever the stream is over; the HUD does not
    /// wait for it (`LivenessTimer::walletRead` reaches the HUD directly, on the network thread).
    void applyLiveBalance(qint64 minutes);
    /// Sets the three balance properties from a server balance and says so.
    void setBalance(qint64 minutes);
    /// Forgets the balance. Called at sign-out so the next customer never sees this one's.
    void resetBalance();
    /// Fills `m_identity` and `m_account` from a confirmed account.
    void setAccount(const AccountInfo& account);
    /// The success tail both sign-in routes share: the credential goes to the access slot as a DPAPI
    /// blob, whatever 0.1.x left in its own slot is dropped, the control-plane client takes it, and
    /// the facade becomes signed in as `identity`. False (with `failureText` set) when this PC would
    /// not save the credential; nothing is then signed in.
    bool adoptSignIn(const AuthTokenPair& pair, const QString& identity, QString* failureText);
    /// The sentence a failed sign-in call shows: the copy deck's offline sentence when the request
    /// never reached the control plane, otherwise the server's own words, verbatim.
    static QString signInFailureText(const ControlPlaneResult& result, QString* reference);
    /// True once `beginSession()` has attached a real control-plane session.
    bool inControlPlaneSession() const;
    /// Play with no access token: there is nothing to allocate a session with, so the engine
    /// lifecycle is asked to run and - with no host attached to stream from - fails closed.
    void beginLocalAttempt();
    /// Drop the engine session the previous launch attached, if any. Called at the end of a
    /// session (`handleReadyForDeletion()`) and at the start of the next one
    /// (`handleHostResolved()`). Refuses while a session is running: `run()` hijacks the calling
    /// thread for the whole stream, and the object cannot be destroyed under it. The end-of-session
    /// call site depends on the lifecycle clearing its own state *above* the emission that reaches
    /// it - see the ordering comment in `session_lifecycle.cpp`.
    void releaseEngineSession();
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
    int m_connectStage = 0;
    bool m_connectFailed = false;
    QString m_stalledStepText;
    QString m_stalledReasonText;
    QString m_homeStatus;
    QString m_endReasonText;
    QVariantMap m_failure;
    QString m_identity;
    QVariantMap m_account;

    // The profile (Phase 5 plan 09). The three lists and the totals are each independent: one failing
    // never blanks another, and none is computed here - every number is the server's own answer.
    bool m_inProfile = false;
    SessionListModel* m_sessionHistory = nullptr;
    CreditHistoryModel* m_creditHistory = nullptr;
    TopupListModel* m_topupHistory = nullptr;
    QString m_totalsStatus = QStringLiteral("idle");
    QString m_hoursPlayedText;
    QString m_creditLeftText;
    QString m_totalsError;
    QString m_totalsErrorReference;
    bool m_accountFailed = false;
    QString m_accountError;
    QString m_accountErrorReference;
    /// Bumped by every totals or identity read, and by every reset, so a reply that was asked for
    /// before the profile was left, reloaded or signed out of is dropped.
    quint64 m_totalsRead = 0;
    quint64 m_accountRead = 0;

    QVariantList m_countries;
    QString m_defaultCountryCode;
    bool m_animationEffects = true;
    std::function<bool(const QUrl&)> m_urlOpener;

    /// True from a confirmed sign-in (or a restore that could not reach the control plane, where
    /// the credential is kept and trusted) until sign-out. This - not `m_identity`, which an
    /// offline restore cannot fill - is what "signed in" means: the screens a session ends on
    /// (Home or sign-in) follow it.
    bool m_signedIn = false;
    bool m_restoreStarted = false;
    /// Bumped by every sign-in, restore result and sign-out. A reply issued under an older value
    /// (a wallet read, a sign-out's own revoke) must not touch what a newer one set up.
    quint64 m_authEpoch = 0;

    qint64 m_balanceMinutes = -1;
    QString m_balanceText;
    bool m_balanceStale = false;

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

    // D-17/A-51, Plan 15: SeatHub's one log tee and its two sinks (`log_tee.h`'s own header
    // comment on why there is exactly one installation point). Both handles are unregistered at
    // the top of `~SeatHubClient()`, before anything the two sink lambdas capture - `this` and
    // `m_liveness` - is destroyed: `LogTee`'s sink list is process-global and outlives any one
    // `SeatHubClient`, which is exactly what a test that constructs and destroys many of them
    // (`tst_facade_wiring.cpp`) would otherwise turn into a dangling-lambda call on every later
    // log line.
    StatsWatcher* m_statsWatcher = nullptr;
    LogTee::SinkHandle m_statsSinkHandle = 0;
    LogTee::SinkHandle m_terminationSinkHandle = 0;
    /// The stats block parsed before this session's teardown began (Pitfall 7), held until
    /// `handleTeardownCompleted()` posts it once.
    QJsonObject m_pendingQualityReport;
    bool m_hasPendingQualityReport = false;

    /// The engine session this launch attached, or null. Owned here: the lifecycle drives it and
    /// deliberately does not destroy it (it cannot know whether the attacher has other uses for
    /// it). Released in `handleReadyForDeletion()`.
    MoonlightEngineSession* m_engineSession = nullptr;
    /// Per-session teardown claim. Reset by `beginSession()` and released once a teardown has run
    /// for that session, so the second and every later session tears down exactly like the first
    /// (defect F-9; `teardown_guard.h` has the whole story).
    SessionTeardownGuard m_teardownGuard;

    /// The session the real control-plane path is running, or empty when no session is attached.
    QString m_sessionId;
    /// True once the control plane has reported `m_sessionId` terminal. A session that is over is not
    /// one Home offers to resume, even while its teardown has not finished.
    bool m_sessionEnded = false;
    bool m_liveSession = false;
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
