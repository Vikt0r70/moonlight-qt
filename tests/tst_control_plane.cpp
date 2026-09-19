/*****************************************************************************
 * SeatHub fork - unit tests for the control-plane HTTP client (Plan 03-03 Task 1).
 *
 * The client is the only thing in the fork that talks to the control plane, so the two rules
 * worth pinning down are pinned here rather than in a build/run cycle:
 *
 *   1. Pitfall 4 - an HTTP 200 whose body says `status:false` is a FAILURE. The control
 *      plane's `Error` and `AllocationRefused` schemas both look exactly like that.
 *   2. Every route is the one `docs/spec/openapi.yaml` documents. The Sunshine admin routes
 *      (`/api/pin`, `/api/clients/*`) are Node Agent-only (`COVERAGE.md`) and must never
 *      appear on this client's wire.
 *
 * No server is involved and none is needed: the responses are injected through a fake
 * QNetworkAccessManager, which is the same seam production uses.
 *****************************************************************************/

#include <QtTest>
#include <QBuffer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include "seathub/control_plane_client.h"

namespace {

// A reply that produces one canned response as soon as the event loop turns. Emitting
// `finished()` from the constructor would fire before the client connected to it.
class FakeReply : public QNetworkReply
{
    Q_OBJECT

public:
    FakeReply(int httpStatus, const QByteArray& body, QObject* parent)
        : QNetworkReply(parent)
    {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, httpStatus == 0
                         ? QVariant()
                         : QVariant(httpStatus));
        setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        m_buffer.setData(body);
        m_buffer.open(QIODevice::ReadOnly);
        open(QIODevice::ReadOnly);

        if (httpStatus == 0) {
            // The transport never produced a response: an unreachable control plane.
            setError(QNetworkReply::HostNotFoundError, QStringLiteral("no route to control plane"));
        }

        QTimer::singleShot(0, this, [this]() {
            setFinished(true);
            emit finished();
        });
    }

    void abort() override {}

    qint64 readData(char* data, qint64 maxSize) override
    {
        return m_buffer.read(data, maxSize);
    }

    qint64 bytesAvailable() const override
    {
        return m_buffer.size() + QNetworkReply::bytesAvailable();
    }

private:
    QBuffer m_buffer;
};

// Returns whatever the test put in it, and records what the client asked for.
class FakeNetworkAccessManager : public QNetworkAccessManager
{
    Q_OBJECT

public:
    int status = 200;
    QByteArray body;
    QString lastPath;
    QByteArray lastMethod;
    QNetworkRequest lastRequest;

protected:
    QNetworkReply* createRequest(Operation op, const QNetworkRequest& request,
                                 QIODevice* outgoingData = nullptr) override
    {
        Q_UNUSED(op);
        lastPath = request.url().path();
        lastRequest = request;
        lastMethod = outgoingData ? QByteArrayLiteral("POST") : QByteArrayLiteral("GET");
        if (outgoingData) {
            // Drain the body so the caller's write completes; the content is asserted through
            // the static builders instead.
            outgoingData->readAll();
        }
        return new FakeReply(status, body, this);
    }
};

QByteArray okBody(const QString& extra = QString())
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), QStringLiteral("6f1c6f5e-3a1e-4b1e-9f2e-0f1a2b3c4d5e"));
    object.insert(QStringLiteral("state"), QStringLiteral("ACTIVE"));
    object.insert(QStringLiteral("quality_profile"), QStringLiteral("1080p60"));
    object.insert(QStringLiteral("minutes_billed"), 3);
    object.insert(QStringLiteral("reconnect_count"), 0);
    object.insert(QStringLiteral("requested_at"), QStringLiteral("2026-09-19T00:00:00Z"));
    if (!extra.isEmpty()) {
        const QJsonObject extraObject = QJsonDocument::fromJson(extra.toUtf8()).object();
        for (auto it = extraObject.begin(); it != extraObject.end(); ++it) {
            object.insert(it.key(), it.value());
        }
    }
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

// `Error` / `AllocationRefused` - valid JSON, HTTP 200, and still a refusal.
QByteArray refusedBody(const QString& error, const QString& reference,
                       const QString& failure = QString())
{
    QJsonObject object;
    object.insert(QStringLiteral("status_code"), 200);
    object.insert(QStringLiteral("status"), false);
    object.insert(QStringLiteral("error"), error);
    object.insert(QStringLiteral("reference"), reference);
    if (!failure.isEmpty()) {
        object.insert(QStringLiteral("failure"), failure);
    }
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

} // namespace

class TstControlPlane : public QObject
{
    Q_OBJECT

private slots:
    // --- Pitfall 4 -------------------------------------------------------------------------

    void classify_http200WithStatusFalse_isFailure()
    {
        const ControlPlaneResult result = ControlPlaneClient::classify(
            200, refusedBody(QStringLiteral("No rig is free right now."), QStringLiteral("SH-4F7KQ2")));

        QVERIFY2(!result.ok, "HTTP 200 with status:false must be a failure (Pitfall 4)");
        QCOMPARE(result.statusCode, 200);
        QCOMPARE(result.error, QStringLiteral("No rig is free right now."));
        QCOMPARE(result.reference, QStringLiteral("SH-4F7KQ2"));
    }

    void classify_http200WithAllocationFailure_carriesTheFailureCode()
    {
        const ControlPlaneResult result = ControlPlaneClient::classify(
            409, refusedBody(QStringLiteral("All rigs are busy."), QStringLiteral("SH-7QK2M4"),
                             QStringLiteral("NO_HOST_AVAILABLE")));

        QVERIFY(!result.ok);
        QCOMPARE(result.failure, QStringLiteral("NO_HOST_AVAILABLE"));

        const SeatHubFailure failure = result.toFailure();
        QCOMPARE(failure.kind, FailureKind::Api);
        QCOMPARE(failure.failure, QStringLiteral("NO_HOST_AVAILABLE"));
        QCOMPARE(failure.reference, QStringLiteral("SH-7QK2M4"));
        QCOMPARE(failure.statusCode, 409);
    }

    void classify_http200WithoutStatusField_isSuccess()
    {
        // `Session`, `TokenPair` and `SessionAuthorization` carry no `status` field at all;
        // absence of the field is success, not an unhandled case.
        const ControlPlaneResult result = ControlPlaneClient::classify(200, okBody());
        QVERIFY2(result.ok, "a 200 body without a status field is a success");
        QCOMPARE(result.body.value(QStringLiteral("state")).toString(), QStringLiteral("ACTIVE"));
    }

    void classify_transportFailure_reportsStatusZero()
    {
        const ControlPlaneResult result = ControlPlaneClient::classify(0, QByteArray());
        QVERIFY(!result.ok);
        QCOMPARE(result.statusCode, 0);
    }

    void classify_401_mapsToAuthFailure()
    {
        const ControlPlaneResult result = ControlPlaneClient::classify(
            401, refusedBody(QStringLiteral("Sign in again."), QStringLiteral("SH-3K2XQ1")));
        QCOMPARE(result.toFailure().kind, FailureKind::Auth);
    }

    // --- the routes themselves -------------------------------------------------------------

    void authorization_isFetchedFromTheDocumentedRoute()
    {
        ControlPlaneClient client;
        auto* fake = new FakeNetworkAccessManager;
        client.setNetworkAccessManager(fake);

        QJsonObject ports;
        ports.insert(QStringLiteral("https"), 47984);
        ports.insert(QStringLiteral("control"), 47989);
        ports.insert(QStringLiteral("rtsp"), 48010);

        QJsonObject lease;
        lease.insert(QStringLiteral("lease_id"), QStringLiteral("11111111-2222-3333-4444-555555555555"));
        lease.insert(QStringLiteral("lease_seq"), 1);
        lease.insert(QStringLiteral("kind"), QStringLiteral("connect"));
        lease.insert(QStringLiteral("connect_deadline_at"), QStringLiteral("2026-09-19T00:10:00Z"));

        QJsonObject body;
        body.insert(QStringLiteral("session_id"), QStringLiteral("aaaabbbb-cccc-dddd-eeee-ffff00001111"));
        body.insert(QStringLiteral("pairing_pin"), QStringLiteral("4821"));
        body.insert(QStringLiteral("host_address"), QStringLiteral("203.0.113.7"));
        body.insert(QStringLiteral("ports"), ports);
        body.insert(QStringLiteral("quality_profile"), QStringLiteral("1080p60"));
        body.insert(QStringLiteral("lease"), lease);

        fake->status = 200;
        fake->body = QJsonDocument(body).toJson(QJsonDocument::Compact);

        ControlPlaneResult captured;
        bool called = false;
        client.fetchSessionAuthorization(QStringLiteral("aaaabbbb-cccc-dddd-eeee-ffff00001111"),
                                        [&](const ControlPlaneResult& result) {
                                            captured = result;
                                            called = true;
                                        });

        QTRY_VERIFY(called);

        // The documented route, and not a Sunshine route.
        QCOMPARE(fake->lastPath, QStringLiteral("/api/sessions/aaaabbbb-cccc-dddd-eeee-ffff00001111/pairing"));
        QVERIFY2(!fake->lastPath.contains(QStringLiteral("/api/pin")),
                 "the client must never call Sunshine's pairing routes (COVERAGE.md)");
        QVERIFY(!fake->lastPath.contains(QStringLiteral("/api/clients")));

        QVERIFY(captured.ok);
        SessionAuthorization auth;
        QVERIFY(SessionAuthorization::parse(captured.body, &auth));
        QCOMPARE(auth.pairingPin, QStringLiteral("4821"));
        QCOMPARE(auth.hostAddress, QStringLiteral("203.0.113.7"));
        QCOMPARE(auth.httpsPort, 47984);
        QCOMPARE(auth.controlPort, 47989);
        QCOMPARE(auth.rtspPort, 48010);
        QCOMPARE(auth.qualityProfile, QStringLiteral("1080p60"));
        QCOMPARE(auth.leaseSeq, 1);
    }

    void authorization_toleratesNullPairingPin()
    {
        // `pairing_pin` is `type: [string, "null"]`: null means the session has not asked to
        // pair yet, and the client must not read that as an empty PIN it should submit.
        QJsonObject body;
        body.insert(QStringLiteral("session_id"), QStringLiteral("aaaabbbb-cccc-dddd-eeee-ffff00001111"));
        body.insert(QStringLiteral("pairing_pin"), QJsonValue::Null);
        body.insert(QStringLiteral("host_address"), QStringLiteral("203.0.113.7"));

        SessionAuthorization auth;
        QVERIFY(SessionAuthorization::parse(body, &auth));
        QVERIFY2(auth.pairingPin.isEmpty(), "a null pairing_pin is not a PIN");
    }

    void liveness_postsToTheDocumentedRouteWithStateAndErrorCode()
    {
        ControlPlaneClient client;
        auto* fake = new FakeNetworkAccessManager;
        client.setNetworkAccessManager(fake);
        fake->status = 200;
        fake->body = okBody();

        bool called = false;
        client.postLiveness(QStringLiteral("aaaabbbb-cccc-dddd-eeee-ffff00001111"),
                            QStringLiteral("streaming"), QStringLiteral("SH-4F7KQ2"),
                            [&](const ControlPlaneResult& result) {
                                QVERIFY(result.ok);
                                called = true;
                            });
        QTRY_VERIFY(called);

        QCOMPARE(fake->lastPath,
                 QStringLiteral("/api/sessions/aaaabbbb-cccc-dddd-eeee-ffff00001111/liveness"));
        QCOMPARE(fake->lastMethod, QByteArrayLiteral("POST"));
    }

    void endSession_postsToTheDocumentedRoute()
    {
        ControlPlaneClient client;
        auto* fake = new FakeNetworkAccessManager;
        client.setNetworkAccessManager(fake);
        fake->status = 202;
        fake->body = okBody();

        bool called = false;
        client.endSession(QStringLiteral("aaaabbbb-cccc-dddd-eeee-ffff00001111"),
                          [&](const ControlPlaneResult& result) {
                              QVERIFY(result.ok);
                              called = true;
                          });
        QTRY_VERIFY(called);

        QCOMPARE(fake->lastPath,
                 QStringLiteral("/api/sessions/aaaabbbb-cccc-dddd-eeee-ffff00001111/end"));
    }

    void sessionFetch_parsesTheSessionSchema()
    {
        ControlPlaneClient client;
        auto* fake = new FakeNetworkAccessManager;
        client.setNetworkAccessManager(fake);
        fake->status = 200;
        fake->body = okBody(QStringLiteral(
            "{\"authorized_through\":\"2026-09-19T01:00:00Z\",\"minutes_billed\":7}"));

        ControlPlaneResult captured;
        bool called = false;
        client.fetchSession(QStringLiteral("aaaabbbb-cccc-dddd-eeee-ffff00001111"),
                            [&](const ControlPlaneResult& result) {
                                captured = result;
                                called = true;
                            });
        QTRY_VERIFY(called);

        SessionInfo info;
        QVERIFY(SessionInfo::parse(captured.body, &info));
        QCOMPARE(info.state, QStringLiteral("ACTIVE"));
        QCOMPARE(info.minutesBilled, 7);
        QCOMPARE(info.authorizedThrough, QStringLiteral("2026-09-19T01:00:00Z"));
        QVERIFY2(!info.isTerminal(), "ACTIVE is not a terminal SessionState");
    }

    void sessionInfo_terminalStatesAreTheFourFromTheSpec()
    {
        for (const char* state : { "COMPLETED", "FAILED", "EXPIRED", "CANCELLED" }) {
            QJsonObject body;
            body.insert(QStringLiteral("id"), QStringLiteral("x"));
            body.insert(QStringLiteral("state"), QLatin1String(state));
            SessionInfo info;
            QVERIFY(SessionInfo::parse(body, &info));
            QVERIFY2(info.isTerminal(), state);
        }

        for (const char* state : { "REQUESTED", "ALLOCATED", "PREPARING", "READY", "ACTIVE", "ENDING" }) {
            QJsonObject body;
            body.insert(QStringLiteral("id"), QStringLiteral("x"));
            body.insert(QStringLiteral("state"), QLatin1String(state));
            SessionInfo info;
            QVERIFY(SessionInfo::parse(body, &info));
            QVERIFY2(!info.isTerminal(), state);
        }
    }

    void bearerToken_isAttachedAndNeverExposed()
    {
        ControlPlaneClient client;
        auto* fake = new FakeNetworkAccessManager;
        client.setNetworkAccessManager(fake);
        client.setAccessToken(QStringLiteral("opaque-access-token"));
        fake->status = 200;
        fake->body = okBody();

        bool called = false;
        client.fetchSession(QStringLiteral("s"), [&](const ControlPlaneResult&) { called = true; });
        QTRY_VERIFY(called);

        QCOMPARE(fake->lastRequest.rawHeader("Authorization"),
                 QByteArrayLiteral("Bearer opaque-access-token"));
    }

    // --- liveness payload (D-34, ADR-0041) --------------------------------------------------

    void livenessPayload_carriesStateAndErrorCode()
    {
        const QByteArray body = ControlPlaneClient::buildLiveness(QStringLiteral("streaming"),
                                                                QStringLiteral("SH-4F7KQ2"));
        const QJsonObject object = QJsonDocument::fromJson(body).object();
        QCOMPARE(object.value(QStringLiteral("state")).toString(), QStringLiteral("streaming"));
        QCOMPARE(object.value(QStringLiteral("error_code")).toString(), QStringLiteral("SH-4F7KQ2"));
    }

    void livenessPayload_onlyTheThreeDocumentedStates()
    {
        QVERIFY(ControlPlaneClient::isValidLivenessState(QStringLiteral("streaming")));
        QVERIFY(ControlPlaneClient::isValidLivenessState(QStringLiteral("reconnecting")));
        QVERIFY(ControlPlaneClient::isValidLivenessState(QStringLiteral("ending")));
        QVERIFY(!ControlPlaneClient::isValidLivenessState(QStringLiteral("ACTIVE")));
        QVERIFY(!ControlPlaneClient::isValidLivenessState(QString()));

        // A state outside the closed enum is dropped rather than sent, so the server never has
        // to reject a body this client built.
        const QByteArray body = ControlPlaneClient::buildLiveness(QStringLiteral("nonsense"),
                                                                QString());
        QVERIFY2(body.isEmpty(), "an out-of-enum state produces the pre-1.6.0 empty body");
    }

    void livenessPayload_emptyMeansThePre16Report()
    {
        QVERIFY(ControlPlaneClient::buildLiveness(QString(), QString()).isEmpty());
    }

    void errorCode_mustMatchTheAdr0008Shape()
    {
        QVERIFY(ControlPlaneClient::isValidErrorCode(QStringLiteral("SH-4F7KQ2")));
        QVERIFY(ControlPlaneClient::isReferenceCode(QStringLiteral("SH-4F7KQ2")));

        // `SH-9K2XQ1` is `docs/spec/copy.md`'s *sample* reference, and it is well-formed -
        // which is exactly why printing it as a fallback would be worse than printing nothing:
        // it looks resolvable to support and is not (the 03-02 decision that produced
        // `SH-GENERR`). The shape check cannot tell the two apart and must not try.
        QVERIFY(ControlPlaneClient::isReferenceCode(QStringLiteral("SH-9K2XQ1")));

        // The alphabet excludes I, L, O and U so a code read off a screen is unambiguous.
        for (const char* excluded : { "SH-4F7KQI", "SH-4F7KQL", "SH-4F7KQO", "SH-4F7KQU" }) {
            QVERIFY2(!ControlPlaneClient::isReferenceCode(QLatin1String(excluded)), excluded);
        }

        // Shape violations.
        QVERIFY(!ControlPlaneClient::isReferenceCode(QStringLiteral("SH-4F7KQ")));
        QVERIFY(!ControlPlaneClient::isReferenceCode(QStringLiteral("sh-4f7kq2")));
        QVERIFY(!ControlPlaneClient::isReferenceCode(QStringLiteral("XX-4F7KQ2")));
        QVERIFY(!ControlPlaneClient::isReferenceCode(QString()));
    }

    void errorCode_aMalformedCodeIsNeverSent()
    {
        const QByteArray body = ControlPlaneClient::buildLiveness(QString(),
                                                                 QStringLiteral("not-a-code"));
        QVERIFY2(body.isEmpty(), "a malformed error_code must be dropped, not forwarded");
    }

    // --- builders ---------------------------------------------------------------------------

    void builders_matchTheDocumentedRequestBodies()
    {
        const QJsonObject otp = QJsonDocument::fromJson(
            ControlPlaneClient::buildOtpRequest(QStringLiteral("+962790000000"))).object();
        QCOMPARE(otp.value(QStringLiteral("phone_e164")).toString(), QStringLiteral("+962790000000"));

        const QJsonObject verify = QJsonDocument::fromJson(
            ControlPlaneClient::buildOtpVerify(QStringLiteral("+962790000000"),
                                               QStringLiteral("123456"))).object();
        QCOMPARE(verify.value(QStringLiteral("code")).toString(), QStringLiteral("123456"));

        const QJsonObject create = QJsonDocument::fromJson(
            ControlPlaneClient::buildSessionCreate(QStringLiteral("1080p120"))).object();
        QCOMPARE(create.value(QStringLiteral("quality_profile")).toString(),
                 QStringLiteral("1080p120"));

        const QJsonObject refresh = QJsonDocument::fromJson(
            ControlPlaneClient::buildRefreshRequest(QStringLiteral("r"))).object();
        QCOMPARE(refresh.value(QStringLiteral("refresh_token")).toString(), QStringLiteral("r"));
    }

    void tokenPair_parsesTheSignInResponse()
    {
        QJsonObject body;
        body.insert(QStringLiteral("access_token"), QStringLiteral("a"));
        body.insert(QStringLiteral("refresh_token"), QStringLiteral("r"));
        body.insert(QStringLiteral("access_expires_at"), QStringLiteral("2026-09-19T01:00:00Z"));
        body.insert(QStringLiteral("refresh_expires_at"), QStringLiteral("2026-10-19T01:00:00Z"));

        AuthTokenPair pair;
        QVERIFY(AuthTokenPair::parse(body, &pair));
        QCOMPARE(pair.accessToken, QStringLiteral("a"));
        QCOMPARE(pair.refreshToken, QStringLiteral("r"));
        QVERIFY(pair.accessExpiresAt.startsWith(QStringLiteral("2026-09-19")));
    }

    void baseUrl_isTheSameHostTheReleaseFeedUses()
    {
        QCOMPARE(ControlPlaneClient::defaultBaseUrl(),
                 QStringLiteral("https://api-sevenhills.damra.co"));
        QCOMPARE(ControlPlaneClient::localBaseUrl(), QStringLiteral("http://127.0.0.1:8000"));
    }
};

QTEST_MAIN(TstControlPlane)

#include "tst_control_plane.moc"
