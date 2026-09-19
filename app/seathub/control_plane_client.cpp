#include "control_plane_client.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QThread>
#include <QUrl>

namespace {

// Where a call lands when nothing else is known. One place names the control plane.
const char* kProductionBaseUrl = "https://api-sevenhills.damra.co";

// `docs/spec/openapi.yaml` `SessionState`. The four terminal states are what teardown waits
// for; the other six are in flight.
const char* const kTerminalStates[] = { "COMPLETED", "FAILED", "EXPIRED", "CANCELLED" };

// `docs/spec/openapi.yaml` `liveness.requestBody.state` - closed enum, 1.6.0.
const char* const kLivenessStates[] = { "streaming", "reconnecting", "ending" };

// ADR-0008 / `Error.reference`, `ReferenceCode`, `liveness.error_code`. The alphabet
// excludes I, L and O so a reference read off a screen is unambiguous.
const char* kReferencePattern = "^SH-[0-9A-HJ-KM-NP-TV-Z]{6}$";

} // namespace

// ---------------------------------------------------------------- payload parsing

bool SessionAuthorization::parse(const QJsonObject& body, SessionAuthorization* out)
{
    if (!out) {
        return false;
    }

    // `session_id` is the only required field a client cannot proceed without; the ports
    // object is required by the schema and is read defensively because a refusal is a
    // better outcome than an out-of-bounds port.
    if (!body.contains(QStringLiteral("session_id"))) {
        return false;
    }

    SessionAuthorization auth;
    auth.sessionId = body.value(QStringLiteral("session_id")).toString();
    // `pairing_pin` is `type: [string, "null"]` - explicitly nullable, and null means this
    // session has not asked to pair yet. Absent and null are the same thing here.
    auth.pairingPin = body.value(QStringLiteral("pairing_pin")).toString();
    auth.hostAddress = body.value(QStringLiteral("host_address")).toString();

    const QJsonObject ports = body.value(QStringLiteral("ports")).toObject();
    auth.httpsPort = ports.value(QStringLiteral("https")).toInt();
    auth.controlPort = ports.value(QStringLiteral("control")).toInt();
    auth.rtspPort = ports.value(QStringLiteral("rtsp")).toInt();

    auth.qualityProfile = body.value(QStringLiteral("quality_profile")).toString();

    const QJsonObject lease = body.value(QStringLiteral("lease")).toObject();
    auth.leaseId = lease.value(QStringLiteral("lease_id")).toString();
    auth.leaseSeq = lease.value(QStringLiteral("lease_seq")).toInt();
    auth.connectDeadlineAt = lease.value(QStringLiteral("connect_deadline_at")).toString();

    *out = auth;
    return true;
}

bool SessionInfo::isTerminal() const
{
    for (const char* state : kTerminalStates) {
        if (this->state == QLatin1String(state)) {
            return true;
        }
    }
    return false;
}

bool SessionInfo::parse(const QJsonObject& body, SessionInfo* out)
{
    if (!out || !body.contains(QStringLiteral("id"))) {
        return false;
    }

    SessionInfo info;
    info.id = body.value(QStringLiteral("id")).toString();
    info.state = body.value(QStringLiteral("state")).toString();
    info.hostId = body.value(QStringLiteral("host_id")).toString();
    info.qualityProfile = body.value(QStringLiteral("quality_profile")).toString();
    info.minutesBilled = body.value(QStringLiteral("minutes_billed")).toInt();
    info.reconnectCount = body.value(QStringLiteral("reconnect_count")).toInt();
    info.authorizedThrough = body.value(QStringLiteral("authorized_through")).toString();
    info.connectDeadlineAt = body.value(QStringLiteral("connect_deadline_at")).toString();
    info.saveDeadlineAt = body.value(QStringLiteral("save_deadline_at")).toString();
    info.billingStartedAt = body.value(QStringLiteral("billing_started_at")).toString();
    info.endReason = body.value(QStringLiteral("end_reason")).toString();

    *out = info;
    return true;
}

bool AuthTokenPair::parse(const QJsonObject& body, AuthTokenPair* out)
{
    if (!out || !body.contains(QStringLiteral("access_token"))
        || !body.contains(QStringLiteral("refresh_token"))) {
        return false;
    }

    AuthTokenPair pair;
    pair.accessToken = body.value(QStringLiteral("access_token")).toString();
    pair.refreshToken = body.value(QStringLiteral("refresh_token")).toString();
    pair.accessExpiresAt = body.value(QStringLiteral("access_expires_at")).toString();
    pair.refreshExpiresAt = body.value(QStringLiteral("refresh_expires_at")).toString();

    *out = pair;
    return true;
}

SeatHubFailure ControlPlaneResult::toFailure() const
{
    if (statusCode == 0) {
        // We never reached the control plane, so there is no control-plane sentence and no
        // control-plane reference. The reason is the sanctioned generic sentence (copy.md
        // has no control-plane-unreachable string and none is invented here); `kind`
        // carries the layer, which is what the UI picks its copy from.
        SeatHubFailure f = SeatHubFailure::network(SeatHubFailure::generic().error);
        f.diagnostic = error;
        return f;
    }

    if (statusCode == 401 || statusCode == 403) {
        SeatHubFailure f = SeatHubFailure::auth(error);
        f.statusCode = statusCode;
        f.reference = reference;
        return f;
    }

    return SeatHubFailure::api(statusCode, error, reference, failure);
}

// ---------------------------------------------------------------- construction

ControlPlaneClient::ControlPlaneClient(QObject* parent)
    : QObject(parent),
      m_baseUrl(QString::fromLatin1(kProductionBaseUrl))
{
    m_network = new QNetworkAccessManager(this);
    m_ownsNetwork = true;
}

ControlPlaneClient::~ControlPlaneClient()
{
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
}

QString ControlPlaneClient::defaultBaseUrl()
{
    return QString::fromLatin1(kProductionBaseUrl);
}

QString ControlPlaneClient::localBaseUrl()
{
    return QStringLiteral("http://127.0.0.1:8000");
}

bool ControlPlaneClient::isReferenceCode(const QString& reference)
{
    static const QRegularExpression re(QString::fromLatin1(kReferencePattern));
    return re.match(reference).hasMatch();
}

// ---------------------------------------------------------------- response classification

ControlPlaneResult ControlPlaneClient::classify(int httpStatus, const QByteArray& body)
{
    ControlPlaneResult result;
    result.statusCode = httpStatus;

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    const QJsonObject object = doc.isObject() ? doc.object() : QJsonObject();

    if (httpStatus >= 200 && httpStatus < 300) {
        // Pitfall 4, and the single most important line in this file. The control plane's
        // `Error` and `AllocationRefused` schemas both ship `status: false` underneath an
        // HTTP 200. A success schema (`Session`, `TokenPair`, `SessionAuthorization`) has no
        // `status` field at all, so absence is success and an explicit false is failure.
        if (object.contains(QStringLiteral("status"))
            && !object.value(QStringLiteral("status")).toBool(true)) {
            result.ok = false;
            result.error = object.value(QStringLiteral("error")).toString();
            result.reference = object.value(QStringLiteral("reference")).toString();
            result.failure = object.value(QStringLiteral("failure")).toString();
            if (result.error.isEmpty()) {
                result.error = SeatHubFailure::generic().error;
            }
            return result;
        }

        result.ok = true;
        result.body = object;
        return result;
    }

    // Non-2xx: the `Error` schema is the documented shape for every one of these, including
    // 402 (balance under the floor), 409 (no host / already a live session) and 429.
    result.ok = false;
    result.error = object.value(QStringLiteral("error")).toString();
    result.reference = object.value(QStringLiteral("reference")).toString();
    result.failure = object.value(QStringLiteral("failure")).toString();
    if (result.error.isEmpty()) {
        result.error = SeatHubFailure::generic().error;
    }
    return result;
}

// ---------------------------------------------------------------- request builders

QByteArray ControlPlaneClient::buildOtpRequest(const QString& phoneE164)
{
    QJsonObject object;
    object.insert(QStringLiteral("phone_e164"), phoneE164);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray ControlPlaneClient::buildOtpVerify(const QString& phoneE164, const QString& code)
{
    QJsonObject object;
    object.insert(QStringLiteral("phone_e164"), phoneE164);
    object.insert(QStringLiteral("code"), code);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray ControlPlaneClient::buildRefreshRequest(const QString& refreshToken)
{
    QJsonObject object;
    object.insert(QStringLiteral("refresh_token"), refreshToken);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray ControlPlaneClient::buildSessionCreate(const QString& qualityProfile)
{
    QJsonObject object;
    object.insert(QStringLiteral("quality_profile"), qualityProfile);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

bool ControlPlaneClient::isValidLivenessState(const QString& state)
{
    for (const char* candidate : kLivenessStates) {
        if (state == QLatin1String(candidate)) {
            return true;
        }
    }
    return false;
}

bool ControlPlaneClient::isValidErrorCode(const QString& errorCode)
{
    // `liveness.error_code` is the same ADR-0008 shape as every other reference. The client
    // never mints one; it only carries one it was already given (self-checked here so a
    // malformed code is dropped rather than sent).
    return isReferenceCode(errorCode);
}

QByteArray ControlPlaneClient::buildLiveness(const QString& state, const QString& errorCode)
{
    QJsonObject object;

    // ADR-0041 (1.6.0, D-34): the report carries `state` and `error_code` when each is valid,
    // so the control plane can tell "still streaming" from "reconnecting" and can record what
    // the client just failed with. Each is independently optional.
    if (!state.isEmpty() && isValidLivenessState(state)) {
        object.insert(QStringLiteral("state"), state);
    }
    if (!errorCode.isEmpty() && isValidErrorCode(errorCode)) {
        object.insert(QStringLiteral("error_code"), errorCode);
    }

    // Both fields are optional and additive (ADR-0041). Neither present is the pre-1.6.0
    // report: a caller that only wants to extend the deadline sends no body at all.
    if (object.isEmpty()) {
        return QByteArray();
    }
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

// ---------------------------------------------------------------- configuration

void ControlPlaneClient::setBaseUrl(const QString& baseUrl)
{
    m_baseUrl = baseUrl;
}

void ControlPlaneClient::setAccessToken(const QString& token)
{
    m_accessToken = token;
}

void ControlPlaneClient::setNetworkAccessManager(QNetworkAccessManager* manager)
{
    if (!manager || manager == m_network) {
        return;
    }
    m_network = manager;
    m_ownsNetwork = false;
}

void ControlPlaneClient::moveToOwnThread()
{
    if (m_thread) {
        return;
    }

    // Upstream `Session::exec()` hijacks the calling thread to be the SDL main thread and
    // suspends all Qt processing until the stream is over (`session.cpp:1965-1966`). A
    // QNetworkAccessManager on the Qt main thread completes nothing for the whole session,
    // so it is given a thread with its own running event loop for the duration of the
    // stream. `this` moves with it, so its access manager and every QTimer its orchestrators
    // own live on the same thread and can fire.
    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("seathub-control-plane"));
    moveToThread(m_thread);
    connect(m_thread, &QThread::finished, this, &QObject::deleteLater);
    m_thread->start();
}

void ControlPlaneClient::stopOwnedThread()
{
    if (m_thread == nullptr) {
        return;
    }

    // Join first. After this returns no reply signal can still be delivered, which is what makes
    // it safe for the owner to destroy this object and the facade it calls back into.
    m_thread->quit();
    m_thread->wait();

    // This object still lives on the thread that has just stopped, and Qt does not allow it to
    // be destroyed from a different one. Bring it home. The `finished -> deleteLater` above
    // cannot have run - its event loop is already gone - so this is the only destruction.
    moveToThread(QThread::currentThread());

    QThread* thread = m_thread;
    m_thread = nullptr;
    delete thread;
}

// ---------------------------------------------------------------- transport

void ControlPlaneClient::send(const QString& method, const QString& path, const QByteArray& body,
                             bool authenticated, Callback callback)
{
    // Every public call funnels through here, and here is the one place that knows the
    // caller may be on a different thread than this object. A queued invocation makes the
    // access to `m_network` happen on the owning thread, which is what
    // QNetworkAccessManager requires.
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this, method, path, body, authenticated, callback]() {
                send(method, path, body, authenticated, callback);
            },
            Qt::QueuedConnection);
        return;
    }

    QUrl url(m_baseUrl + path);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("SeatHub"));

    if (authenticated && !m_accessToken.isEmpty()) {
        request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + m_accessToken.toUtf8());
    }

    QNetworkReply* reply = nullptr;
    if (method == QLatin1String("GET")) {
        reply = m_network->get(request);
    }
    else {
        if (body.isEmpty()) {
            request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            reply = m_network->post(request, QByteArrayLiteral("{}"));
        }
        else {
            request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            reply = m_network->post(request, body);
        }
    }

    connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
        finishReply(reply, callback);
    });
}

void ControlPlaneClient::finishReply(QNetworkReply* reply, Callback callback)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkErrorText = reply->errorString();

    reply->deleteLater();

    ControlPlaneResult result = classify(status, body);

    // statusCode 0 means the transport never produced a response: an unreachable control
    // plane, a DNS failure, a timeout. This is the D-33 case, and it is reported as a
    // failure the caller can choose to treat as non-fatal - not as an exception and not as
    // a silent no-op.
    if (status == 0 && networkError != QNetworkReply::NoError) {
        result.ok = false;
        result.statusCode = 0;
        result.error = networkErrorText;
        result.reference.clear();
        result.failure.clear();
        emit transportFailed(networkErrorText);
    }

    if (callback) {
        callback(result);
    }
}

// ---------------------------------------------------------------- calls

void ControlPlaneClient::requestOtp(const QString& phoneE164, Callback callback)
{
    send(QStringLiteral("POST"), QStringLiteral("/api/auth/otp/request"),
         buildOtpRequest(phoneE164), false, callback);
}

void ControlPlaneClient::verifyOtp(const QString& phoneE164, const QString& code, Callback callback)
{
    send(QStringLiteral("POST"), QStringLiteral("/api/auth/otp/verify"),
         buildOtpVerify(phoneE164, code), false, callback);
}

void ControlPlaneClient::refresh(const QString& refreshToken, Callback callback)
{
    send(QStringLiteral("POST"), QStringLiteral("/api/auth/refresh"),
         buildRefreshRequest(refreshToken), false, callback);
}

void ControlPlaneClient::requestSession(const QString& qualityProfile, Callback callback)
{
    send(QStringLiteral("POST"), QStringLiteral("/api/sessions"),
         buildSessionCreate(qualityProfile), true, callback);
}

void ControlPlaneClient::fetchSession(const QString& sessionId, Callback callback)
{
    send(QStringLiteral("GET"), QStringLiteral("/api/sessions/") + sessionId,
         QByteArray(), true, callback);
}

void ControlPlaneClient::fetchSessionAuthorization(const QString& sessionId, Callback callback)
{
    // The session authorization: where to connect, what the CONNECT lease permits, and the
    // control-plane-issued pairing PIN (ADR-0034). Short-lived and scoped to this one
    // session; it never carries a Sunshine admin credential.
    send(QStringLiteral("GET"), QStringLiteral("/api/sessions/") + sessionId
             + QStringLiteral("/pairing"),
         QByteArray(), true, callback);
}

void ControlPlaneClient::postLiveness(const QString& sessionId, const QString& state,
                                     const QString& errorCode, Callback callback)
{
    send(QStringLiteral("POST"), QStringLiteral("/api/sessions/") + sessionId
             + QStringLiteral("/liveness"),
         buildLiveness(state, errorCode), true, callback);
}

void ControlPlaneClient::endSession(const QString& sessionId, Callback callback)
{
    send(QStringLiteral("POST"), QStringLiteral("/api/sessions/") + sessionId
             + QStringLiteral("/end"),
         QByteArray(), true, callback);
}
