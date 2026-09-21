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
#include <QDir>
#include <QFile>
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
    /// Every path asked for, in order - for the assertion that a route was never built.
    QStringList paths;

protected:
    QNetworkReply* createRequest(Operation op, const QNetworkRequest& request,
                                 QIODevice* outgoingData = nullptr) override
    {
        Q_UNUSED(op);
        lastPath = request.url().path(QUrl::FullyEncoded);
        paths.append(lastPath);
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

    // --- HR-02: an unexpected 200 fails closed, it is never read as success ------------------

    void classify_http200WithAnHtmlBody_isFailure()
    {
        // The captive portal / proxy interstitial: HTTP 200 and a sign-in page instead of JSON.
        const QByteArray html =
            QByteArrayLiteral("<!DOCTYPE html><html><head><title>Sign in to the network</title>"
                              "</head><body>Wi-Fi authentication required</body></html>");
        const ControlPlaneResult result = ControlPlaneClient::classify(200, html);

        QVERIFY2(!result.ok, "a 2xx that is not a JSON object carries no evidence of success");
        QCOMPARE(result.statusCode, 200);
        QVERIFY(!result.error.isEmpty());
        QVERIFY(!result.body.contains(QStringLiteral("state")));
    }

    void classify_http200WithAnEmptyBody_isFailure()
    {
        const ControlPlaneResult result = ControlPlaneClient::classify(200, QByteArray());
        QVERIFY2(!result.ok, "an empty 200 body is not a success either");
        QVERIFY(!result.error.isEmpty());
    }

    void classify_http200WithANonBooleanStatus_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<bool>("expectedOk");

        // A present `status` that is not the boolean `true` is not success. `toBool(true)` used to
        // read every one of these as true (the default applies when the value has the wrong type).
        QTest::newRow("string-false") << QByteArrayLiteral("{\"status\":\"false\"}") << false;
        QTest::newRow("numeric-zero") << QByteArrayLiteral("{\"status\":0}") << false;
        QTest::newRow("null") << QByteArrayLiteral("{\"status\":null}") << false;
        QTest::newRow("string-true") << QByteArrayLiteral("{\"status\":\"true\"}") << false;
        QTest::newRow("array") << QByteArrayLiteral("{\"status\":[]}") << false;
        QTest::newRow("boolean-false") << QByteArrayLiteral("{\"status\":false}") << false;
        QTest::newRow("boolean-true") << QByteArrayLiteral("{\"status\":true}") << true;
    }

    void classify_http200WithANonBooleanStatus()
    {
        QFETCH(QByteArray, body);
        QFETCH(bool, expectedOk);

        const ControlPlaneResult result = ControlPlaneClient::classify(200, body);
        QCOMPARE(result.ok, expectedOk);
        if (!expectedOk) {
            QVERIFY(!result.error.isEmpty());
        }
    }

    void requestOtp_aCaptivePortalAnswering200_doesNotReportSuccess()
    {
        // The scenario the review named: a proxy answers the OTP request with a login page and
        // HTTP 200. Nothing was sent, so nothing may be reported as sent.
        ControlPlaneClient client;
        auto* fake = new FakeNetworkAccessManager;
        client.setNetworkAccessManager(fake);
        fake->status = 200;
        fake->body = QByteArrayLiteral("<html><body>Sign in to continue</body></html>");

        ControlPlaneResult captured;
        bool called = false;
        client.requestOtp(QStringLiteral("+962790000000"), [&](const ControlPlaneResult& result) {
            captured = result;
            called = true;
        });

        QTRY_VERIFY(called);
        QVERIFY2(!captured.ok, "an HTML 200 must not be read as 'code sent'");
        QCOMPARE(captured.statusCode, 200);
        QVERIFY(!captured.error.isEmpty());
        QCOMPARE(fake->lastPath, QStringLiteral("/api/auth/otp/request"));
    }

    // --- ME-03: one phone normalisation rule ------------------------------------------------

    void phoneNormalisation_data()
    {
        QTest::addColumn<QString>("raw");
        QTest::addColumn<QString>("expected");

        // Already E.164: unchanged.
        QTest::newRow("e164") << QStringLiteral("+962790000000")
                              << QStringLiteral("+962790000000");
        // The separators people actually type: spaces, parentheses, dashes and dots. `+962 7 0000
        // 0000` is the placeholder the sign-in screen shows, and it has to normalise to the same
        // number as `+962700000000`.
        QTest::newRow("spaces") << QStringLiteral("+962 7 0000 0000")
                                << QStringLiteral("+962700000000");
        QTest::newRow("parentheses-and-dashes") << QStringLiteral("+962 (79) 000-0000")
                                                << QStringLiteral("+962790000000");
        QTest::newRow("dots") << QStringLiteral("+962.79.000.0000")
                              << QStringLiteral("+962790000000");
        QTest::newRow("surrounding-space") << QStringLiteral("  +962790000000  ")
                                           << QStringLiteral("+962790000000");
        // The international prefix as people write it.
        QTest::newRow("double-zero") << QStringLiteral("00962790000000")
                                     << QStringLiteral("+962790000000");
        QTest::newRow("double-zero-with-spaces") << QStringLiteral("00 962 79 000 0000")
                                                 << QStringLiteral("+962790000000");
        // Rejected: no country code is invented for these, because the spec defines none.
        QTest::newRow("no-plus") << QStringLiteral("0962790000000") << QString();
        QTest::newRow("national") << QStringLiteral("0790000000") << QString();
        QTest::newRow("leading-zero-after-plus") << QStringLiteral("+0962790000000") << QString();
        QTest::newRow("too-short") << QStringLiteral("+962790") << QString();
        QTest::newRow("too-long") << QStringLiteral("+9627900000000000") << QString();
        QTest::newRow("letters") << QStringLiteral("+96279ABC000") << QString();
        QTest::newRow("empty") << QString() << QString();
    }

    void phoneNormalisation()
    {
        QFETCH(QString, raw);
        QFETCH(QString, expected);

        QCOMPARE(ControlPlaneClient::normalisePhoneE164(raw), expected);
    }

    // --- ME-04: ids cannot add structure to the route they are pasted into -------------------

    void encodedPathSegment_data()
    {
        QTest::addColumn<QString>("segment");
        QTest::addColumn<QString>("expected");

        QTest::newRow("uuid") << QStringLiteral("aaaabbbb-cccc-dddd-eeee-ffff00001111")
                              << QStringLiteral("aaaabbbb-cccc-dddd-eeee-ffff00001111");
        QTest::newRow("slash") << QStringLiteral("aa/bb") << QStringLiteral("aa%2Fbb");
        QTest::newRow("query") << QStringLiteral("aa?bb") << QStringLiteral("aa%3Fbb");
        QTest::newRow("fragment") << QStringLiteral("aa#bb") << QStringLiteral("aa%23bb");
        QTest::newRow("percent") << QStringLiteral("aa%2Fbb") << QStringLiteral("aa%252Fbb");
        QTest::newRow("spaces") << QStringLiteral("aa bb") << QStringLiteral("aa%20bb");
    }

    void encodedPathSegment()
    {
        QFETCH(QString, segment);
        QFETCH(QString, expected);

        QCOMPARE(ControlPlaneClient::encodedPathSegment(segment), expected);
    }

    void fetchSession_anIdWithASlash_cannotRetargetTheRoute()
    {
        ControlPlaneClient client;
        auto* fake = new FakeNetworkAccessManager;
        client.setNetworkAccessManager(fake);
        fake->status = 200;
        fake->body = okBody();

        bool called = false;
        client.fetchSession(QStringLiteral("aa/../../admin"), [&](const ControlPlaneResult&) {
            called = true;
        });

        QTRY_VERIFY(called);
        // One segment, still under `/api/sessions/`, and no `/../` left to walk.
        QCOMPARE(fake->lastPath, QStringLiteral("/api/sessions/aa%2F..%2F..%2Fadmin"));
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

        const QJsonObject login = QJsonDocument::fromJson(
            ControlPlaneClient::buildLogin(QStringLiteral("someone@example.com"),
                                           QStringLiteral("a password"))).object();
        QCOMPARE(login.value(QStringLiteral("identifier")).toString(),
                 QStringLiteral("someone@example.com"));
        QCOMPARE(login.value(QStringLiteral("password")).toString(), QStringLiteral("a password"));
        QCOMPARE(login.size(), 2);
    }

    // --- Phase 5 plan 02: the launch, balance, sign-out and password routes ------------------

    void fetchMe_getsTheDocumentedRouteWithTheCredentialAndParsesTheAccount()
    {
        ControlPlaneClient client;
        auto* fake = new FakeNetworkAccessManager;
        client.setNetworkAccessManager(fake);
        client.setAccessToken(QStringLiteral("opaque-access-token"));
        fake->status = 200;
        fake->body = QByteArrayLiteral(
            "{\"id\":\"6f1c6f5e-3a1e-4b1e-9f2e-0f1a2b3c4d5e\",\"display_name\":\"Lina\","
            "\"username\":\"lina\",\"email\":\"lina@example.com\",\"phone_e164\":\"+962790000000\","
            "\"role\":\"customer\",\"signup_stage\":\"complete\",\"email_verified\":true,"
            "\"phone_verified\":true,\"created_at\":\"2026-09-01T00:00:00Z\"}");

        ControlPlaneResult captured;
        bool called = false;
        client.fetchMe([&](const ControlPlaneResult& result) {
            captured = result;
            called = true;
        });
        QTRY_VERIFY(called);

        QCOMPARE(fake->lastPath, QStringLiteral("/api/me"));
        QCOMPARE(fake->lastMethod, QByteArrayLiteral("GET"));
        QCOMPARE(fake->lastRequest.rawHeader("Authorization"),
                 QByteArrayLiteral("Bearer opaque-access-token"));

        QVERIFY(captured.ok);
        AccountInfo account;
        QVERIFY(AccountInfo::parse(captured.body, &account));
        QCOMPARE(account.id, QStringLiteral("6f1c6f5e-3a1e-4b1e-9f2e-0f1a2b3c4d5e"));
        QCOMPARE(account.displayName, QStringLiteral("Lina"));
        QCOMPARE(account.username, QStringLiteral("lina"));
        QCOMPARE(account.email, QStringLiteral("lina@example.com"));
        QCOMPARE(account.phoneE164, QStringLiteral("+962790000000"));
        QVERIFY(account.emailVerified);
    }

    void accountParse_needsOnlyAnId()
    {
        // A phone-only account has no username and no email; that is a normal account.
        QJsonObject minimal;
        minimal.insert(QStringLiteral("id"), QStringLiteral("abc"));
        AccountInfo account;
        QVERIFY(AccountInfo::parse(minimal, &account));
        QCOMPARE(account.id, QStringLiteral("abc"));
        QVERIFY(account.email.isEmpty());
        QVERIFY(account.username.isEmpty());
        QVERIFY(!account.emailVerified);

        // Null members (the schema's nullable username/email/phone) read as empty, not as text.
        QJsonObject nulls = minimal;
        nulls.insert(QStringLiteral("username"), QJsonValue::Null);
        nulls.insert(QStringLiteral("email"), QJsonValue::Null);
        QVERIFY(AccountInfo::parse(nulls, &account));
        QVERIFY(account.username.isEmpty());

        // Without an id there is no account.
        QVERIFY(!AccountInfo::parse(QJsonObject(), &account));
        QJsonObject emptyId;
        emptyId.insert(QStringLiteral("id"), QString());
        QVERIFY(!AccountInfo::parse(emptyId, &account));
    }

    void fetchMe_withoutACredential_attachesNoAuthorizationHeader()
    {
        ControlPlaneClient client;
        auto* fake = new FakeNetworkAccessManager;
        client.setNetworkAccessManager(fake);
        fake->status = 401;
        fake->body = refusedBody(QStringLiteral("Please sign in again."), QStringLiteral("SH-3K2XQ1"));

        ControlPlaneResult captured;
        bool called = false;
        client.fetchMe([&](const ControlPlaneResult& result) {
            captured = result;
            called = true;
        });
        QTRY_VERIFY(called);

        QVERIFY2(fake->lastRequest.rawHeader("Authorization").isEmpty(),
                 "no credential means no Authorization header, never an empty Bearer");
        QVERIFY(!captured.ok);
        QCOMPARE(captured.statusCode, 401);
        QCOMPARE(captured.toFailure().kind, FailureKind::Auth);
    }

    void fetchWallet_getsTheDocumentedRouteWithTheCredentialAndParsesTheBalance()
    {
        ControlPlaneClient client;
        auto* fake = new FakeNetworkAccessManager;
        client.setNetworkAccessManager(fake);
        client.setAccessToken(QStringLiteral("opaque-access-token"));
        fake->status = 200;
        fake->body = QByteArrayLiteral(
            "{\"balance_minutes\":135,\"updated_at\":\"2026-09-21T10:00:00Z\"}");

        ControlPlaneResult captured;
        bool called = false;
        client.fetchWallet([&](const ControlPlaneResult& result) {
            captured = result;
            called = true;
        });
        QTRY_VERIFY(called);

        QCOMPARE(fake->lastPath, QStringLiteral("/api/wallet"));
        QCOMPARE(fake->lastMethod, QByteArrayLiteral("GET"));
        QCOMPARE(fake->lastRequest.rawHeader("Authorization"),
                 QByteArrayLiteral("Bearer opaque-access-token"));

        QVERIFY(captured.ok);
        WalletInfo wallet;
        QVERIFY(WalletInfo::parse(captured.body, &wallet));
        QCOMPARE(wallet.balanceMinutes, qint64(135));
        QCOMPARE(wallet.updatedAt, QStringLiteral("2026-09-21T10:00:00Z"));
    }

    void walletParse_neverInventsABalance()
    {
        WalletInfo wallet;

        // Zero is a real balance and parses as zero...
        QJsonObject zero;
        zero.insert(QStringLiteral("balance_minutes"), 0);
        QVERIFY(WalletInfo::parse(zero, &wallet));
        QCOMPARE(wallet.balanceMinutes, qint64(0));

        // ...but a body that has no balance is not zero, and neither is one that is not a number.
        QVERIFY(!WalletInfo::parse(QJsonObject(), &wallet));
        QJsonObject text;
        text.insert(QStringLiteral("balance_minutes"), QStringLiteral("135"));
        QVERIFY(!WalletInfo::parse(text, &wallet));
        QJsonObject nul;
        nul.insert(QStringLiteral("balance_minutes"), QJsonValue::Null);
        QVERIFY(!WalletInfo::parse(nul, &wallet));
        // The schema says `ge: 0`; a negative one is a broken body, not a debt to display.
        QJsonObject negative;
        negative.insert(QStringLiteral("balance_minutes"), -5);
        QVERIFY(!WalletInfo::parse(negative, &wallet));
    }

    void logout_postsWithTheCredentialAndA204IsSuccess()
    {
        ControlPlaneClient client;
        auto* fake = new FakeNetworkAccessManager;
        client.setNetworkAccessManager(fake);
        client.setAccessToken(QStringLiteral("opaque-access-token"));
        fake->status = 204;
        fake->body = QByteArray();

        ControlPlaneResult captured;
        bool called = false;
        client.logout([&](const ControlPlaneResult& result) {
            captured = result;
            called = true;
        });
        QTRY_VERIFY(called);

        QCOMPARE(fake->lastPath, QStringLiteral("/api/auth/logout"));
        QCOMPARE(fake->lastMethod, QByteArrayLiteral("POST"));
        QCOMPARE(fake->lastRequest.rawHeader("Authorization"),
                 QByteArrayLiteral("Bearer opaque-access-token"));
        QVERIFY2(captured.ok, "204 No Content is the documented success for logout");
        QCOMPARE(captured.statusCode, 204);
    }

    void logout_aRefusalOrAnUnreachableServerIsAFailureTheCallerCanIgnore()
    {
        ControlPlaneClient client;
        auto* fake = new FakeNetworkAccessManager;
        client.setNetworkAccessManager(fake);
        client.setAccessToken(QStringLiteral("opaque-access-token"));

        for (int status : { 401, 500, 0 }) {
            fake->status = status;
            fake->body = QByteArray();
            ControlPlaneResult captured;
            bool called = false;
            client.logout([&](const ControlPlaneResult& result) {
                captured = result;
                called = true;
            });
            QTRY_VERIFY(called);
            QVERIFY2(!captured.ok, qPrintable(QString::number(status)));
            QCOMPARE(captured.statusCode, status);
        }
    }

    void classify_only204MayCarryNoBody()
    {
        QVERIFY(ControlPlaneClient::classify(204, QByteArray()).ok);
        // The HR-02 rule stands for every other 2xx: no body is no evidence of success.
        QVERIFY(!ControlPlaneClient::classify(200, QByteArray()).ok);
        QVERIFY(!ControlPlaneClient::classify(202, QByteArray()).ok);
    }

    void login_postsUnauthenticatedAndParsesABodyWithOnlyAnAccessToken()
    {
        ControlPlaneClient client;
        auto* fake = new FakeNetworkAccessManager;
        client.setNetworkAccessManager(fake);
        // A credential is present on the client - login must not send it (it is how you get one).
        client.setAccessToken(QStringLiteral("a-stale-credential"));
        fake->status = 200;
        fake->body = QByteArrayLiteral("{\"access_token\":\"sb_at_permanent\"}");

        ControlPlaneResult captured;
        bool called = false;
        client.login(QStringLiteral("lina@example.com"), QStringLiteral("hunter2"),
                     [&](const ControlPlaneResult& result) {
                         captured = result;
                         called = true;
                     });
        QTRY_VERIFY(called);

        QCOMPARE(fake->lastPath, QStringLiteral("/api/auth/login"));
        QCOMPARE(fake->lastMethod, QByteArrayLiteral("POST"));
        QVERIFY2(fake->lastRequest.rawHeader("Authorization").isEmpty(),
                 "login is unauthenticated: no credential may be attached");

        QVERIFY(captured.ok);
        AuthTokenPair pair;
        QVERIFY2(AuthTokenPair::parse(captured.body, &pair),
                 "ADR-0050: the body carries one access token and nothing else");
        QCOMPARE(pair.accessToken, QStringLiteral("sb_at_permanent"));
        QVERIFY(pair.refreshToken.isEmpty());
    }

    void login_aRefusalKeepsTheServersOwnSentence()
    {
        ControlPlaneClient client;
        auto* fake = new FakeNetworkAccessManager;
        client.setNetworkAccessManager(fake);
        fake->status = 401;
        fake->body = refusedBody(QStringLiteral("That password isn't right."),
                                 QStringLiteral("SH-4F7KQ2"));

        ControlPlaneResult captured;
        bool called = false;
        client.login(QStringLiteral("lina@example.com"), QStringLiteral("wrong"),
                     [&](const ControlPlaneResult& result) {
                         captured = result;
                         called = true;
                     });
        QTRY_VERIFY(called);

        QVERIFY(!captured.ok);
        QCOMPARE(captured.error, QStringLiteral("That password isn't right."));
        QCOMPARE(captured.reference, QStringLiteral("SH-4F7KQ2"));
    }

    void theRetiredRefreshRouteIsNeverBuilt()
    {
        // ADR-0050 made sessions permanent: there is no refresh token and no refresh request. The
        // route is gone from the client, and this proves it two ways - the sources do not name it,
        // and no public call the client makes ever reaches it.
        QDir dir(QCoreApplication::applicationDirPath());
        QString sourceDir;
        for (int depth = 0; depth < 8 && sourceDir.isEmpty(); ++depth) {
            const QString candidate = dir.filePath(QStringLiteral("app/seathub"));
            if (QFile::exists(candidate + QStringLiteral("/control_plane_client.h"))) {
                sourceDir = candidate;
            }
            else if (!dir.cdUp()) {
                break;
            }
        }
        QVERIFY2(!sourceDir.isEmpty(), "app/seathub could not be located from the test binary");
        for (const QString& name : { QStringLiteral("control_plane_client.h"),
                                     QStringLiteral("control_plane_client.cpp") }) {
            QFile file(sourceDir + QLatin1Char('/') + name);
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(name));
            const QString text = QString::fromUtf8(file.readAll());
            QVERIFY2(!text.contains(QStringLiteral("auth/refresh")),
                     qPrintable(name + QStringLiteral(" still names the refresh route")));
            QVERIFY2(!text.contains(QStringLiteral("buildRefreshRequest")),
                     qPrintable(name + QStringLiteral(" still has the refresh request builder")));
        }

        ControlPlaneClient client;
        auto* fake = new FakeNetworkAccessManager;
        client.setNetworkAccessManager(fake);
        client.setAccessToken(QStringLiteral("opaque-access-token"));
        fake->status = 200;
        fake->body = okBody();

        int completed = 0;
        const ControlPlaneClient::Callback done = [&](const ControlPlaneResult&) { ++completed; };
        client.requestOtp(QStringLiteral("+962790000000"), done);
        client.verifyOtp(QStringLiteral("+962790000000"), QStringLiteral("123456"), done);
        client.login(QStringLiteral("a@b.co"), QStringLiteral("pw"), done);
        client.logout(done);
        client.fetchMe(done);
        client.fetchWallet(done);
        client.requestSession(QStringLiteral("1080p60"), done);
        client.fetchSession(QStringLiteral("s"), done);
        client.fetchSessionAuthorization(QStringLiteral("s"), done);
        client.postLiveness(QStringLiteral("s"), QStringLiteral("streaming"), QString(), done);
        client.endSession(QStringLiteral("s"), done);
        QTRY_COMPARE(completed, 11);

        for (const QString& path : fake->paths) {
            QVERIFY2(!path.contains(QStringLiteral("refresh")),
                     qPrintable(QStringLiteral("a request was built for %1").arg(path)));
        }
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
