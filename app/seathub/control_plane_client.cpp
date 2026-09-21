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
    // ADR-0050 D-10: sessions are permanent. The control plane returns a single, non-expiring
    // access_token and NO refresh_token (and no expiry fields). Require only access_token; the
    // refresh/expiry fields are optional and absent under the permanent-token model. Requiring
    // refresh_token here is what silently rejected every valid sign-in after Phase 4.
    if (!out || !body.contains(QStringLiteral("access_token"))) {
        return false;
    }

    AuthTokenPair pair;
    pair.accessToken = body.value(QStringLiteral("access_token")).toString();
    pair.refreshToken = body.value(QStringLiteral("refresh_token")).toString();
    pair.accessExpiresAt = body.value(QStringLiteral("access_expires_at")).toString();
    pair.refreshExpiresAt = body.value(QStringLiteral("refresh_expires_at")).toString();

    if (pair.accessToken.isEmpty()) {
        return false;
    }

    *out = pair;
    return true;
}

bool AccountInfo::parse(const QJsonObject& body, AccountInfo* out)
{
    // Tolerant on purpose: `id` is the only member a signed-in client cannot do without, and the
    // account schema grows new members over time. A missing username, email or phone is a normal
    // account (phone-only sign-up has no email), not a malformed body.
    if (!out || body.value(QStringLiteral("id")).toString().isEmpty()) {
        return false;
    }

    AccountInfo account;
    account.id = body.value(QStringLiteral("id")).toString();
    account.displayName = body.value(QStringLiteral("display_name")).toString();
    account.username = body.value(QStringLiteral("username")).toString();
    account.email = body.value(QStringLiteral("email")).toString();
    account.phoneE164 = body.value(QStringLiteral("phone_e164")).toString();
    account.emailVerified = body.value(QStringLiteral("email_verified")).toBool(false);

    *out = account;
    return true;
}

bool WalletInfo::parse(const QJsonObject& body, WalletInfo* out)
{
    // `balance_minutes` is required by the `Wallet` schema and is an integer. A body without one is
    // not a balance of zero: reporting zero for "we could not read it" would tell a customer with
    // credit that they have none.
    const QJsonValue balance = body.value(QStringLiteral("balance_minutes"));
    if (!out || !balance.isDouble()) {
        return false;
    }

    WalletInfo wallet;
    wallet.balanceMinutes = static_cast<qint64>(balance.toDouble());
    if (wallet.balanceMinutes < 0) {
        return false;
    }
    wallet.updatedAt = body.value(QStringLiteral("updated_at")).toString();

    *out = wallet;
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
    // Safety net only: the documented route is `stopOwnedThread()`, which has already joined the
    // thread and cleared `m_thread` by the time this runs. A direct `delete` of an object that
    // still owns its thread would otherwise leave the thread running with a destroyed owner.
    if (m_thread) {
        QThread* thread = m_thread;
        m_thread = nullptr;
        // `finished -> deleteLater` would otherwise re-enter this destructor when the join below
        // flushes the worker's deferred-delete queue.
        disconnect(thread, nullptr, this, nullptr);
        thread->quit();
        thread->wait();
        delete thread;
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

bool ControlPlaneClient::isPhoneE164(const QString& phoneE164)
{
    static const QRegularExpression re(QStringLiteral("^\\+[1-9][0-9]{7,14}$"));
    return re.match(phoneE164).hasMatch();
}

QString ControlPlaneClient::normalisePhoneE164(const QString& raw, const QString& dialCode)
{
    // The normalisation rule (ME-03), stated once, in one place, and tested. It is the website's
    // own (`seathub-web/src/lib/auth-validation.ts` `toE164`), so a number typed the same way in
    // either surface reaches the server as the same E.164:
    //   1. digits typed as Arabic-Indic (U+0660..0669) or Persian (U+06F0..06F9) numerals are read
    //      as the Latin digits they are;
    //   2. drop the separators a person types between digits: space, '(', ')', '-' and '.';
    //   3. a number already written with a leading '+' is believed as it stands, and so is one
    //      written with the international prefix `00` (mapped to '+') - what the customer typed
    //      wins over the country the tag shows;
    //   4. otherwise the number is national: `dialCode` (the chosen country's, `+962`) is put in
    //      front of it after one leading trunk '0' is dropped (`0790000000` in Jordan). With no
    //      `dialCode` nothing is guessed - the spec defines no default - and the number is refused;
    //   5. whatever results must match `^\+[1-9][0-9]{7,14}$` exactly, the pattern
    //      `docs/spec/openapi.yaml` puts on `phone_e164`. Anything else returns an empty string and
    //      the caller rejects locally instead of spending a round trip. The server validates again.
    // No phone-parsing library is used: these are the same minimal rules the website applies.
    static const QRegularExpression separators(QStringLiteral("[\\s()\\-.]+"));

    QString digits;
    digits.reserve(raw.size());
    for (const QChar ch : raw) {
        const ushort u = ch.unicode();
        if (u >= 0x0660 && u <= 0x0669) {
            digits.append(QChar(u - 0x0660 + u'0'));
        }
        else if (u >= 0x06F0 && u <= 0x06F9) {
            digits.append(QChar(u - 0x06F0 + u'0'));
        }
        else {
            digits.append(ch);
        }
    }
    digits.remove(separators);

    if (digits.startsWith(QLatin1String("00"))) {
        digits = QStringLiteral("+") + digits.mid(2);
    }
    else if (!digits.startsWith(QLatin1Char('+')) && !dialCode.isEmpty()) {
        if (digits.startsWith(QLatin1Char('0'))) {
            digits.remove(0, 1);
        }
        digits = dialCode + digits;
    }

    return isPhoneE164(digits) ? digits : QString();
}

QString ControlPlaneClient::encodedPathSegment(const QString& segment)
{
    // ME-04: session ids are interpolated into request paths, and `/`, `?`, `#` and `%` are
    // *structure* inside a URL path - a corrupt or hostile id could retarget or truncate the route
    // it was pasted into. `toPercentEncoding` escapes exactly those (and everything else outside
    // the unreserved set) while leaving the contract's UUIDs, and the `-_.~` set, untouched.
    return QString::fromLatin1(QUrl::toPercentEncoding(segment));
}

// ---------------------------------------------------------------- response classification

ControlPlaneResult ControlPlaneClient::classify(int httpStatus, const QByteArray& body)
{
    ControlPlaneResult result;
    result.statusCode = httpStatus;

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    const QJsonObject object = doc.isObject() ? doc.object() : QJsonObject();

    // 204 No Content is the one 2xx that legitimately has no body (`POST /api/auth/logout`).
    // Absence of a body is the documented success there, so it is not held to the "evidence of
    // success" rule below - which is about a 200 that should have carried a JSON object.
    if (httpStatus == 204) {
        result.ok = true;
        return result;
    }

    if (httpStatus >= 200 && httpStatus < 300) {
        // Pitfall 4, and the single most important rule in this file. The control plane's
        // `Error` and `AllocationRefused` schemas both ship `status: false` underneath an
        // HTTP 200. A success schema (`Session`, `TokenPair`, `SessionAuthorization`) has no
        // `status` field at all, so absence is success and an explicit false is failure.
        //
        // HR-02: absence is the *only* shape that means success. A 2xx whose body is not a JSON
        // object carries no evidence of success at all - a captive portal or a proxy interstitial
        // answering 200 with a sign-in page is exactly that, and reporting "code sent" for it is a
        // lie. A `status` that is present but is not the boolean `true` (`"false"`, `0`, `null`)
        // is not a success either; `toBool(true)` used to read every one of those as true.
        if (!doc.isObject()) {
            result.ok = false;
            result.error = SeatHubFailure::generic().error;
            return result;
        }

        const QJsonValue status = object.value(QStringLiteral("status"));
        if (!status.isUndefined() && (!status.isBool() || !status.toBool())) {
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

QByteArray ControlPlaneClient::buildLogin(const QString& identifier, const QString& password)
{
    QJsonObject object;
    object.insert(QStringLiteral("identifier"), identifier);
    object.insert(QStringLiteral("password"), password);
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
    //
    // Deliberately *not* parented to `this`: `this` is destroyed during `stopOwnedThread()`'s
    // join, and a parented thread would be destroyed by that deletion and then deleted again by
    // the join. The thread is deleted by `stopOwnedThread()`, or by the destructor when nothing
    // called it.
    m_thread = new QThread;
    m_thread->setObjectName(QStringLiteral("seathub-control-plane"));
    moveToThread(m_thread);
    connect(m_thread, &QThread::finished, this, &QObject::deleteLater);
    m_thread->start();
}

void ControlPlaneClient::stopOwnedThread()
{
    // Callers check `onOwnThread()` first: false means this object was never moved and still
    // belongs to the calling thread, which is then the thread that has to delete it.
    QThread* thread = m_thread;
    if (thread == nullptr) {
        return;
    }

    // Cleared *before* the thread is stopped: `this` is destroyed during `wait()` below (see the
    // header), so no member may be read or written after the join, and the destructor must not
    // try to join a thread it no longer owns.
    m_thread = nullptr;

    // `moveToOwnThread()` installed `finished -> deleteLater`. `this` lives on that thread, so the
    // connection is direct and the deferred delete is posted to the worker's own queue; Qt then
    // flushes that queue as the thread unwinds (`QThreadPrivate::finish()`), on the worker thread
    // and inside the call below. That is the one and only destruction of this object, and it is
    // what makes lines that used to follow this join (a `moveToThread()`, a `delete`) illegal:
    // after `wait()` returns, `this` is gone. Nothing below touches it.
    thread->quit();
    thread->wait();
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

void ControlPlaneClient::login(const QString& identifier, const QString& password,
                               Callback callback)
{
    send(QStringLiteral("POST"), QStringLiteral("/api/auth/login"),
         buildLogin(identifier, password), false, callback);
}

void ControlPlaneClient::logout(Callback callback)
{
    send(QStringLiteral("POST"), QStringLiteral("/api/auth/logout"), QByteArray(), true, callback);
}

void ControlPlaneClient::fetchMe(Callback callback)
{
    send(QStringLiteral("GET"), QStringLiteral("/api/me"), QByteArray(), true, callback);
}

void ControlPlaneClient::fetchWallet(Callback callback)
{
    send(QStringLiteral("GET"), QStringLiteral("/api/wallet"), QByteArray(), true, callback);
}

void ControlPlaneClient::requestSession(const QString& qualityProfile, Callback callback)
{
    send(QStringLiteral("POST"), QStringLiteral("/api/sessions"),
         buildSessionCreate(qualityProfile), true, callback);
}

void ControlPlaneClient::fetchSession(const QString& sessionId, Callback callback)
{
    send(QStringLiteral("GET"), QStringLiteral("/api/sessions/") + encodedPathSegment(sessionId),
         QByteArray(), true, callback);
}

void ControlPlaneClient::fetchSessionAuthorization(const QString& sessionId, Callback callback)
{
    // The session authorization: where to connect, what the CONNECT lease permits, and the
    // control-plane-issued pairing PIN (ADR-0034). Short-lived and scoped to this one
    // session; it never carries a Sunshine admin credential.
    send(QStringLiteral("GET"), QStringLiteral("/api/sessions/") + encodedPathSegment(sessionId)
             + QStringLiteral("/pairing"),
         QByteArray(), true, callback);
}

void ControlPlaneClient::postLiveness(const QString& sessionId, const QString& state,
                                     const QString& errorCode, Callback callback)
{
    send(QStringLiteral("POST"), QStringLiteral("/api/sessions/") + encodedPathSegment(sessionId)
             + QStringLiteral("/liveness"),
         buildLiveness(state, errorCode), true, callback);
}

void ControlPlaneClient::endSession(const QString& sessionId, Callback callback)
{
    send(QStringLiteral("POST"), QStringLiteral("/api/sessions/") + encodedPathSegment(sessionId)
             + QStringLiteral("/end"),
         QByteArray(), true, callback);
}
