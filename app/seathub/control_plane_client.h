#pragma once

// The control-plane HTTP client (D-29: Qt Networking).
//
// Every network call this client makes goes to the control plane at
// `docs/spec/openapi.yaml` `servers[0].url` - the same host the release feed reads
// (ADR-0018). It never calls Sunshine, and it never holds a Sunshine admin credential:
// the seven Sunshine admin routes (`GET /api/pin`, `POST /api/pin`,
// `POST /api/clients/update`, `POST /api/clients/unpair`, `GET /api/clients/list`,
// `POST /api/clients/unpair-all`, `GET /serverinfo`) belong to the Node Agent and are not
// on the control-plane contract at all. `COVERAGE.md` is explicit: "The client (fork)
// itself never calls any of these endpoints directly - all seven are Node Agent-only."
//
// The routes this client does own (`docs/spec/openapi.yaml`, version 1.8.0):
//
//   POST /api/auth/otp/request                 sign-in step 1
//   POST /api/auth/otp/verify                  sign-in step 2            -> TokenPair
//   POST /api/auth/login                       email/phone + password    -> TokenPair
//   POST /api/auth/logout                      revoke the credential     -> 204
//   GET  /api/me                               who the credential is     -> Account
//   GET  /api/wallet                           the balance in minutes    -> Wallet
//   GET  /api/sessions                         finished sessions, paged  -> CustomerSessionPage
//   GET  /api/wallet/history                   the ledger, paged         -> LedgerPage
//   GET  /api/topup-notices                    the top-up notices, paged -> TopupNoticePage
//   GET  /api/usage                            hours played + balance    -> Usage
//   POST /api/sessions                         Play (D-35)               -> Session
//   GET  /api/sessions/{session_id}            state + billing facts     -> Session
//   GET  /api/sessions/{session_id}/pairing    the session authorization -> SessionAuthorization
//   POST /api/sessions/{session_id}/liveness   10s liveness (D-31, D-34)
//   POST /api/sessions/{session_id}/end        End session (idempotent)
//
// Two rules the shape of this class exists to hold:
//
//   * Pitfall 4: an HTTP 200 whose JSON body says `status:false` is a FAILURE. The control
//     plane's `Error` and `AllocationRefused` schemas are both exactly that. Transport
//     success is never operation success, and `ok` is only ever set from the body.
//   * D-35: nothing here hands a token, a header or a JSON payload to QML. Callers get
//     typed structs; the bearer token never leaves this object.
//
// Threading (see `moveToOwnThread`): upstream `Session::exec()` hijacks the calling thread
// to be the SDL main thread and suspends all Qt processing for the whole stream
// (`app/streaming/session.cpp:1965-1966`). A QNetworkAccessManager - and a QTimer, and a
// QWebSocket - on the Qt main thread therefore cannot complete a single request while a
// stream is running, which is precisely the interval liveness exists to cover. This object
// is moved onto a thread with its own running event loop for the duration of a stream; the
// accessors below are safe to call from any thread.

#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <functional>

#include "error_map.h"

class QNetworkAccessManager;
class QNetworkReply;
class QThread;

/// The outcome of one control-plane call. `ok` is authoritative and is derived from the
/// response *body*, never from the HTTP status alone (Pitfall 4).
struct ControlPlaneResult
{
    bool ok = false;
    /// The HTTP status, or 0 when the transport never produced one.
    int statusCode = 0;
    /// `Error.error` - customer-facing text from the control plane - or a SeatHub sentence
    /// composed locally when there was no usable response.
    QString error;
    /// `Error.reference`, when the control plane named one. Empty for a local failure.
    QString reference;
    /// `AllocationRefused.failure`, e.g. `NO_HOST_AVAILABLE`. Empty when absent.
    QString failure;
    /// The parsed response object. Empty on failure.
    QJsonObject body;

    /// CR-03 (code review 06.3-REVIEW-fork.md): true when this "result" never touched the wire at
    /// all - `QualityOutbox::drain()`'s caller decided, before or after issuing the real request,
    /// that the account it would have been sent for is no longer the signed-in one. Not a real
    /// transport or server outcome, so it is never mistaken for one: `QualityOutbox::classify()`
    /// checks this field first, ahead of `ok`/`statusCode`.
    bool wasAborted = false;

    /// A result that never reached the network (`wasAborted == true`, everything else at its
    /// default). See the field's own comment.
    static ControlPlaneResult aborted() { ControlPlaneResult r; r.wasAborted = true; return r; }

    SeatHubFailure toFailure() const;
};

/// `GET /api/sessions/{session_id}/pairing` (`SessionAuthorization`). Short-lived and scoped
/// to one session; never contains Sunshine admin credentials.
struct SessionAuthorization
{
    QString sessionId;
    /// The control-plane-issued pairing PIN (ADR-0034), or empty when the session has not
    /// asked to pair yet. It is handed to the engine's own pairing flow and is never shown
    /// to the customer and never written to disk (STREAM-03, D-30).
    QString pairingPin;
    /// The verified public address to pair against.
    QString hostAddress;
    int httpsPort = 0;
    int controlPort = 0;
    int rtspPort = 0;
    /// `1080p60` | `1080p75` | `1080p120` (ADR-0011, matched by equality).
    QString qualityProfile;
    QString leaseId;
    int leaseSeq = 0;
    /// The CONNECT lease deadline (RFC 3339 UTC). Empty when absent.
    QString connectDeadlineAt;

    static bool parse(const QJsonObject& body, SessionAuthorization* out);
};

/// `Session` - request, status, liveness and end all return this shape.
struct SessionInfo
{
    QString id;
    /// One of SessionState: REQUESTED ALLOCATED PREPARING READY ACTIVE ENDING COMPLETED
    /// FAILED EXPIRED CANCELLED.
    QString state;
    QString hostId;
    QString qualityProfile;
    int minutesBilled = 0;
    int reconnectCount = 0;
    /// The effective lease horizon (D-33). Advisory for display; the accrued budget binds.
    QString authorizedThrough;
    /// The customer-facing 10-minute connect timeout, computed at READY.
    QString connectDeadlineAt;
    /// One minute before the horizon. Drives the save countdown.
    QString saveDeadlineAt;
    QString billingStartedAt;
    QString endReason;

    /// True for COMPLETED, FAILED, EXPIRED, CANCELLED - the four terminal SessionStates
    /// (`docs/spec/state-machines.md`). Teardown waits for one of these (STREAM-10: the
    /// session is only marked over after the host side has verified removal).
    bool isTerminal() const;

    static bool parse(const QJsonObject& body, SessionInfo* out);
};
// Crosses threads as a signal argument (the pairing poll and the teardown read both run on the
// network thread and hand the session to the facade), so it has to be a registered metatype.
Q_DECLARE_METATYPE(SessionInfo)

/// `TokenPair` - the bearer credential sign-in returns. ADR-0050 made sessions permanent, so the
/// control plane sends one non-expiring `access_token` and nothing else; the refresh and expiry
/// members below stay optional only so a body from an older server still parses.
struct AuthTokenPair
{
    QString accessToken;
    QString refreshToken;
    QString accessExpiresAt;
    QString refreshExpiresAt;

    static bool parse(const QJsonObject& body, AuthTokenPair* out);
};

/// `GET /api/me` (`Account`) - who the stored credential belongs to. Read at launch to confirm
/// the credential is still valid and to fill the signed-in identity. Only `id` is required; every
/// other member is carried when present and empty when not (an account may have no username, no
/// email or no phone).
struct AccountInfo
{
    QString id;
    QString displayName;
    QString username;
    QString email;
    QString phoneE164;
    bool emailVerified = false;

    static bool parse(const QJsonObject& body, AccountInfo* out);
};

/// `GET /api/wallet` (`Wallet`) - the customer's balance. The number is the server's; the client
/// formats it (`duration_text.h`) and does no arithmetic on it (`docs/spec/client.md` §Wallet
/// authority).
struct WalletInfo
{
    /// Whole minutes, never negative on the wire.
    qint64 balanceMinutes = 0;
    QString updatedAt;

    static bool parse(const QJsonObject& body, WalletInfo* out);
};

// --- the profile's three lists and its usage totals (Phase 5 plan 09, contract 1.8.0) -------------
//
// Each list page is a small array plus the server's own opaque cursor. The cursor is carried
// unchanged and sent back unchanged: the client never builds one, reads one or does arithmetic on one
// (T-05-38). Every parse is tolerant in the way `AccountInfo::parse` is - members the client does not
// draw are ignored - but a row that has no `id`, or a page that has no array at all, is a contract
// violation and fails the whole page, so a row is never silently dropped from a history.
//
// None of these structs has a rig member of any kind. The contract's `CustomerSessionRow` carries
// none (CUST-01), and this client would not read one if it did.

/// One row of `GET /api/sessions` (`CustomerSessionRow`): a finished session as the customer sees it.
struct CustomerSessionRow
{
    QString id;
    QString state;
    /// The server's own billed-minutes count for the session.
    int minutesBilled = 0;
    /// RFC 3339 UTC; the position the list is ordered by.
    QString requestedAt;
    /// `EndReason`, or empty when the server gave none.
    QString endReason;

    static bool parse(const QJsonObject& body, CustomerSessionRow* out);
};

/// `CustomerSessionPage`.
struct CustomerSessionPage
{
    QList<CustomerSessionRow> rows;
    /// Empty on the last page (`next_cursor` is null there).
    QString nextCursor;

    static bool parse(const QJsonObject& body, CustomerSessionPage* out);
};

/// One row of `GET /api/wallet/history` (`LedgerEntry`).
struct LedgerRow
{
    QString id;
    /// `LedgerKind`: topup_credit, first_bonus, session_debit, refund, adjustment, shortfall.
    QString kind;
    /// Positive for a credit, negative for a debit; the server's own signed number.
    qint64 amountMinutes = 0;
    QString createdAt;

    static bool parse(const QJsonObject& body, LedgerRow* out);
};

/// `LedgerPage`.
struct LedgerPage
{
    QList<LedgerRow> rows;
    QString nextCursor;

    static bool parse(const QJsonObject& body, LedgerPage* out);
};

/// One row of `GET /api/topup-notices` (`TopupNotice`): a statement that a transfer was sent.
struct TopupNoticeRow
{
    QString id;
    QString sentAt;
    /// Empty while the notice is open (waiting).
    QString creditedAt;
    /// The whole minutes the operator's credit added, or -1 while the notice is open.
    qint64 creditedMinutes = -1;

    static bool parse(const QJsonObject& body, TopupNoticeRow* out);
};

/// `TopupNoticePage`.
struct TopupNoticePage
{
    QList<TopupNoticeRow> rows;
    QString nextCursor;

    static bool parse(const QJsonObject& body, TopupNoticePage* out);
};

/// `GET /api/usage` (`Usage`): the two totals the profile shows. Both are the server's numbers; the
/// client formats them and adds nothing to them.
struct UsageInfo
{
    /// The sum of the customer's session-debit ledger minutes (OD-09: a refund is not played).
    qint64 minutesPlayed = 0;
    qint64 balanceMinutes = 0;

    static bool parse(const QJsonObject& body, UsageInfo* out);
};

class ControlPlaneClient : public QObject
{
    Q_OBJECT

public:
    explicit ControlPlaneClient(QObject* parent = nullptr);
    ~ControlPlaneClient() override;

    /// The control plane's public API host: `docs/spec/openapi.yaml` `servers[0].url`,
    /// frozen by ADR-0015 and moved to this host by ADR-0018. Same value the release feed
    /// uses, so there is one place that names the control plane.
    static QString defaultBaseUrl();

    /// `docs/spec/openapi.yaml` `servers[1].url`, for a local control plane.
    static QString localBaseUrl();

    /// The ADR-0008 reference-code shape every control-plane reference must match:
    /// `^SH-[0-9A-HJ-KM-NP-TV-Z]{6}$`.
    static bool isReferenceCode(const QString& reference);

    /// The E.164 shape `docs/spec/openapi.yaml` puts on `phone_e164`
    /// (`^\+[1-9][0-9]{7,14}$`).
    static bool isPhoneE164(const QString& phoneE164);

    /// Normalises a phone number the way the sign-in field is allowed to be typed, into E.164:
    /// Arabic-Indic and Persian digits become Latin ones, the separators people actually type
    /// (`space ( ) - .`) are dropped, a leading `+` or `00` is believed as already international,
    /// and otherwise `dialCode` (the chosen country's, e.g. `+962`) is applied after one leading
    /// trunk `0` is removed. With no `dialCode` a national number is refused rather than guessed.
    /// Returns an empty string when what is left is not E.164. Deciding here keeps `requestOtp`
    /// and `verifyOtp` from sending two spellings of the same number, and matches the website's
    /// own `toE164`.
    static QString normalisePhoneE164(const QString& raw, const QString& dialCode = QString());

    /// Percent-encodes one path segment (a session id) so it cannot add structure to the route it
    /// is interpolated into.
    static QString encodedPathSegment(const QString& segment);

    /// `path` with the page size and, when there is one, the server's cursor as its query. The cursor
    /// is opaque: it is percent-encoded so nothing in it can add structure to the query, and is
    /// otherwise passed through exactly as the server gave it.
    static QString listPath(const QString& path, int limit, const QString& cursor);

    // --- response classification (Pitfall 4). Static so the rule is testable without a server.

    /// Classifies one response. THE rule this class exists to hold: an HTTP 200 body whose
    /// `status` field is `false` is a FAILURE, not a success - and, since HR-02, so is any 2xx
    /// that carries no evidence of success (a non-JSON body, or a `status` that is not `true`).
    static ControlPlaneResult classify(int httpStatus, const QByteArray& body);

    // --- request builders. Static for the same reason: the documented payload shape is
    // --- asserted without a socket.

    static QByteArray buildOtpRequest(const QString& phoneE164);
    static QByteArray buildOtpVerify(const QString& phoneE164, const QString& code);
    /// `LoginRequest`: `{identifier, password}`. The identifier is what the customer typed (an
    /// email or an E.164 phone number); the control plane decides which it is.
    static QByteArray buildLogin(const QString& identifier, const QString& password);
    static QByteArray buildSessionCreate(const QString& qualityProfile);
    /// The liveness body is optional and additive (ADR-0041, D-34). An empty `state` and an
    /// empty `error_code` produce the pre-1.6.0 empty body, which is still a valid report.
    static QByteArray buildLiveness(const QString& state, const QString& errorCode);
    static bool isValidLivenessState(const QString& state);
    static bool isValidErrorCode(const QString& errorCode);

    // --- configuration

    void setBaseUrl(const QString& baseUrl);
    QString baseUrl() const { return m_baseUrl; }

    /// The bearer access token. Held here and attached as an `Authorization` header on every
    /// request; it is never handed out (D-35). Guarded by `m_credentialMutex` (WR-01): written
    /// from whichever thread signs in or signs out, read at `send()`'s call time from whichever
    /// thread calls a public method - the same mutex `setTraceId`/`clearTraceId` use.
    void setAccessToken(const QString& token);
    bool hasAccessToken() const;

    /// D-27: the W3C trace id this client stamps as `traceparent` on every request until
    /// `clearTraceId()`. Must be 32 lowercase hex characters and not all zeros (W3C); a malformed
    /// value clears the id instead of being silently ignored (WR-01: an invalid mint must not
    /// leave the previous Play's id in place). Guarded by `m_credentialMutex`, the same as
    /// `setAccessToken` (WR-01: both are written from one thread and read from another with no
    /// synchronisation before this fix - a data race on a non-atomic `QString`, and `clear()`
    /// could free the buffer a concurrent read was using).
    void setTraceId(const QString& traceId);
    /// Stops sending `traceparent`.
    void clearTraceId();
    /// The id currently set, or empty. Read under the same lock, so a caller that wants to clear
    /// it conditionally (`clearTraceIdIfEquals`) can capture "the id I minted for this Play"
    /// without racing a concurrent `setTraceId`.
    QString traceId() const;
    /// WR-01: clears the id only if it still equals `expected` - a no-op for an empty `expected`
    /// (never means "clear whatever is there"). Exists for a caller whose own clear has to be
    /// marshalled onto this object's thread to run after an already-queued send that must still
    /// carry the id (`SeatHubClient::handlePairingFailed()`'s own comment has the whole story): a
    /// PLAIN queued clear, with no comparison, would otherwise wipe a *different*, newer Play's id
    /// if the customer retried before the queued clear ran.
    void clearTraceIdIfEquals(const QString& expected);

    /// Replaces the access manager. Exists so a test can drive every response path without a
    /// server; production never calls it.
    void setNetworkAccessManager(QNetworkAccessManager* manager);

    QNetworkAccessManager* networkAccessManager() const { return m_network; }

    // --- threading (see the header comment)

    /// Moves this object - and therefore its QNetworkAccessManager - onto a thread with its
    /// own running event loop, so requests complete while upstream's SDL loop has suspended
    /// Qt processing (`session.cpp:1965-1966`). Idempotent. The thread is owned and stopped
    /// by this object.
    void moveToOwnThread();
    /// True once `moveToOwnThread()` has run. `false` means requests only complete while the
    /// owning thread's event loop is running - which, during a stream, it is not.
    bool onOwnThread() const { return m_thread != nullptr; }

    /// Stop the thread `moveToOwnThread()` created, join it, and destroy this object **on that
    /// thread**. Callers must therefore check `onOwnThread()` first:
    ///
    ///   * `onOwnThread()` true  -> `stopOwnedThread()` destroys the object; the caller must not
    ///     touch it again, and must not delete it (that would be the double free this method's
    ///     shape exists to prevent).
    ///   * `onOwnThread()` false -> nothing happened and nothing was destroyed; the object still
    ///     lives on the calling thread and the caller still owns it.
    ///
    /// Why it has to be this way: once moved, `this` and its `QNetworkAccessManager` live on the
    /// worker thread, and Qt refuses both a cross-thread `delete` and a cross-thread
    /// `moveToThread()` (each is a warning, not an error - the object is simply left behind).
    /// The one deletion that is both thread-correct and observable is the deferred delete
    /// `moveToOwnThread()` arms on `QThread::finished`: Qt flushes that thread's deferred-delete
    /// queue as the thread unwinds, i.e. *inside* `wait()` below, on the thread that owns the
    /// object. So the join is also the destruction, and no line after it may read `this`.
    ///
    /// Idempotent in the only sense that is meaningful: a second call on a destroyed object is
    /// not a call anyone can make.
    void stopOwnedThread();

    // --- calls. Each reports exactly once, on the caller's thread.
    //
    // A `ControlPlaneResult` is always delivered, including for transport failures, so a
    // caller never has to distinguish "no answer" from "no callback".

    using Callback = std::function<void(const ControlPlaneResult&)>;

    void requestOtp(const QString& phoneE164, Callback callback);
    void verifyOtp(const QString& phoneE164, const QString& code, Callback callback);

    /// `POST /api/auth/login`. Unauthenticated: no credential is attached. Returns `TokenPair`.
    /// Called by the sign-in screen's password step; wired and tested here so that screen has a
    /// real method to call.
    void login(const QString& identifier, const QString& password, Callback callback);

    /// `POST /api/auth/logout`. Authenticated, empty body; the server revokes the presented
    /// credential and answers 204, which is success (it has no body to classify).
    void logout(Callback callback);

    /// `GET /api/me`. Authenticated. `Account`.
    void fetchMe(Callback callback);

    /// `GET /api/me/telemetry` (06.3.1 D-01, contract 3.2.0). Authenticated. `TelemetryConfig`:
    /// the Sentry DSN this account's SeatHub should use, or null when telemetry is off. A 401
    /// answers like any other authenticated call - `SeatHubTelemetry::applyHandout` is never
    /// reached, so the caller's cached DSN stays exactly as it was (D-18 SV-C3).
    void fetchTelemetry(Callback callback);

    /// `GET /api/wallet`. Authenticated. `Wallet`.
    void fetchWallet(Callback callback);

    /// The profile's reads (contract 1.8.0, `ADR-0055`). Each is authenticated, takes the page size and
    /// the server's cursor of the page wanted (empty for the first) and answers the page shape named
    /// beside it; `fetchUsage` takes neither.
    ///
    /// `GET /api/sessions` - `CustomerSessionPage`, the customer's own finished sessions.
    void fetchSessionList(const QString& cursor, int limit, Callback callback);
    /// `GET /api/wallet/history` - `LedgerPage`.
    void fetchWalletHistory(const QString& cursor, int limit, Callback callback);
    /// `GET /api/topup-notices` - `TopupNoticePage`.
    void fetchTopupNotices(const QString& cursor, int limit, Callback callback);
    /// `GET /api/usage` - `Usage`.
    void fetchUsage(Callback callback);

    void requestSession(const QString& qualityProfile, Callback callback);
    void fetchSession(const QString& sessionId, Callback callback);
    void fetchSessionAuthorization(const QString& sessionId, Callback callback);

    /// The 10-second liveness report (D-31) carrying `state` and/or `error_code` (D-34,
    /// ADR-0041). Both are optional; both empty is the pre-1.6.0 deadline extension. Kept for the
    /// callers this exact shape still has (tests exercising the class directly); production
    /// callers use the `QJsonObject` overload below.
    void postLiveness(const QString& sessionId, const QString& state,
                      const QString& errorCode, Callback callback);

    /// Contract 3.1.0 (D-11): the full liveness body, built by the caller (`LivenessTimer::buildPayload`)
    /// and posted verbatim - `stage`, `state`, `error_code`, `engine_stage`, `engine_error`,
    /// `failing_ports`, each present only when the caller set it. An empty object is the same
    /// pre-1.6.0 deadline extension the string overload sends for two empty strings.
    void postLiveness(const QString& sessionId, const QJsonObject& payload, Callback callback);

    /// `POST /api/sessions/{session_id}/end`. Idempotent server-side: ending an
    /// already-ending or terminal session returns it unchanged.
    void endSession(const QString& sessionId, Callback callback);

    /// `POST /api/sessions/{session_id}/quality` (3.1.0, D-17, Plan 15): the parsed end-of-stream
    /// video-stats block (`stream_stats.h`'s `toQualityReport()`), posted once per session. The
    /// server's own first-report-wins rule (`docs/spec/openapi.yaml`'s `SessionQuality`
    /// description) makes a second POST for the same session harmless, so this client never needs
    /// to suppress a retry itself; a failed POST is logged by the caller and not retried here -
    /// Plan 30 adds the outbox that keeps it.
    void postSessionQuality(const QString& sessionId, const QJsonObject& report, Callback callback);

signals:
    /// The transport failed before any response existed. Distinct from a control-plane
    /// refusal so the UI can tell "we could not ask" from "the answer was no".
    void transportFailed(const QString& message);

private:
    void send(const QString& method, const QString& path, const QByteArray& body,
              bool authenticated, Callback callback);
    /// The rest of what `send()` used to do after its thread marshal, now taking the access token
    /// and trace id as parameters captured at `send()`'s own call time (WR-01) instead of reading
    /// `m_accessToken`/`m_traceId` again here, on whichever thread this runs on.
    void sendOnOwningThread(const QString& method, const QString& path, const QByteArray& body,
                            bool authenticated, const QString& accessToken, const QString& traceId,
                            Callback callback);
    void finishReply(QNetworkReply* reply, Callback callback);

    QNetworkAccessManager* m_network = nullptr;
    bool m_ownsNetwork = false;
    QThread* m_thread = nullptr;
    QString m_baseUrl;

    /// WR-01: guards `m_accessToken` and `m_traceId`, the two fields a setter writes from one
    /// thread while `send()` reads from another. `mutable` so `hasAccessToken() const` can lock
    /// it too.
    mutable QMutex m_credentialMutex;
    QString m_accessToken;
    QString m_traceId;
};
