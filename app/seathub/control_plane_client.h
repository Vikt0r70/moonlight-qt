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
// The routes this client does own (`docs/spec/openapi.yaml`, version 1.6.0):
//
//   POST /api/auth/otp/request                 sign-in step 1
//   POST /api/auth/otp/verify                  sign-in step 2            -> TokenPair
//   POST /api/auth/refresh                     rotate a refresh token    -> TokenPair
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

/// `TokenPair` - the bearer credentials sign-in and refresh return (ADR-0014).
struct AuthTokenPair
{
    QString accessToken;
    QString refreshToken;
    QString accessExpiresAt;
    QString refreshExpiresAt;

    static bool parse(const QJsonObject& body, AuthTokenPair* out);
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

    /// Normalises a phone number the way the sign-in field is allowed to be typed: strips the
    /// separators people actually type (`space ( ) - .`), turns a leading `00` into `+`, and
    /// returns an empty string when what is left is not E.164. Deciding here keeps `requestOtp`
    /// and `verifyOtp` from sending two spellings of the same number.
    static QString normalisePhoneE164(const QString& raw);

    /// Percent-encodes one path segment (a session id) so it cannot add structure to the route it
    /// is interpolated into.
    static QString encodedPathSegment(const QString& segment);

    // --- response classification (Pitfall 4). Static so the rule is testable without a server.

    /// Classifies one response. THE rule this class exists to hold: an HTTP 200 body whose
    /// `status` field is `false` is a FAILURE, not a success - and, since HR-02, so is any 2xx
    /// that carries no evidence of success (a non-JSON body, or a `status` that is not `true`).
    static ControlPlaneResult classify(int httpStatus, const QByteArray& body);

    // --- request builders. Static for the same reason: the documented payload shape is
    // --- asserted without a socket.

    static QByteArray buildOtpRequest(const QString& phoneE164);
    static QByteArray buildOtpVerify(const QString& phoneE164, const QString& code);
    static QByteArray buildRefreshRequest(const QString& refreshToken);
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
    /// request; it is never handed out (D-35).
    void setAccessToken(const QString& token);
    bool hasAccessToken() const { return !m_accessToken.isEmpty(); }

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
    void refresh(const QString& refreshToken, Callback callback);
    void requestSession(const QString& qualityProfile, Callback callback);
    void fetchSession(const QString& sessionId, Callback callback);
    void fetchSessionAuthorization(const QString& sessionId, Callback callback);

    /// The 10-second liveness report (D-31) carrying `state` and/or `error_code` (D-34,
    /// ADR-0041). Both are optional; both empty is the pre-1.6.0 deadline extension.
    void postLiveness(const QString& sessionId, const QString& state,
                      const QString& errorCode, Callback callback);

    /// `POST /api/sessions/{session_id}/end`. Idempotent server-side: ending an
    /// already-ending or terminal session returns it unchanged.
    void endSession(const QString& sessionId, Callback callback);

signals:
    /// The transport failed before any response existed. Distinct from a control-plane
    /// refusal so the UI can tell "we could not ask" from "the answer was no".
    void transportFailed(const QString& message);

private:
    void send(const QString& method, const QString& path, const QByteArray& body,
              bool authenticated, Callback callback);
    void finishReply(QNetworkReply* reply, Callback callback);

    QNetworkAccessManager* m_network = nullptr;
    bool m_ownsNetwork = false;
    QThread* m_thread = nullptr;
    QString m_baseUrl;
    QString m_accessToken;
};
