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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include "seathub/control_plane_client.h"
#include "seathub/countries.h"
#include "seathub/region.h"
#include "seathub/web_origin.h"

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
    /// The query the last request carried, as it went on the wire (percent-encoded).
    QString lastQuery;
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
        lastQuery = request.url().query(QUrl::FullyEncoded);
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

    void aRequestThatNeverArrivedSaysTheDecksOfflineSentenceInFullAndHasNoReference()
    {
        // copy.md Support & errors, "Offline". The same sentence Home shows; the client invents no
        // reference for a request the control plane never saw (ADR-0008).
        const ControlPlaneResult result = ControlPlaneClient::classify(0, QByteArray());
        const SeatHubFailure failure = result.toFailure();
        QCOMPARE(failure.kind, FailureKind::Network);
        QCOMPARE(failure.error,
                 QStringLiteral("Can't reach SevenHills right now. Showing the last known balance."));
        QCOMPARE(failure.error, SeatHubFailure::offlineSentence());
        QVERIFY(failure.reference.isEmpty());
    }

    void aServerRefusalKeepsTheServersOwnSentenceAndItsReference()
    {
        const ControlPlaneResult result = ControlPlaneClient::classify(
            409, refusedBody(QStringLiteral("You already have a session."),
                             QStringLiteral("SH-3K2XQ1")));
        const SeatHubFailure failure = result.toFailure();
        QCOMPARE(failure.kind, FailureKind::Api);
        QCOMPARE(failure.error, QStringLiteral("You already have a session."));
        QCOMPARE(failure.reference, QStringLiteral("SH-3K2XQ1"));
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

    // --- Phase 5 plan 06: a number typed the way its country writes it becomes E.164 -----------
    //
    // The chosen country's dial code is applied, one leading trunk zero is dropped, separators are
    // ignored, and a number already written with a plus or a double zero is believed as it stands.
    // These are the website's own rules (`auth-validation.ts` `toE164`). The six cases the plan
    // names are the first rows; the rest are the edges around them.

    void phoneNormalisationWithACountry_data()
    {
        QTest::addColumn<QString>("raw");
        QTest::addColumn<QString>("dial");
        QTest::addColumn<QString>("expected");

        const QString jo = QStringLiteral("+962");
        // 1. A local number with its trunk zero, the way a customer in Amman writes it.
        QTest::newRow("local-with-trunk-zero") << QStringLiteral("0790000000") << jo
                                               << QStringLiteral("+962790000000");
        // 2. The same number without one.
        QTest::newRow("local-without-trunk-zero") << QStringLiteral("790000000") << jo
                                                  << QStringLiteral("+962790000000");
        // 3. Already international: believed over the country the tag shows.
        QTest::newRow("already-international-plus") << QStringLiteral("+447911123456") << jo
                                                    << QStringLiteral("+447911123456");
        QTest::newRow("already-international-double-zero") << QStringLiteral("00447911123456") << jo
                                                           << QStringLiteral("+447911123456");
        // 4. Written with separators.
        QTest::newRow("separators") << QStringLiteral("(079) 000-00.00") << jo
                                    << QStringLiteral("+962790000000");
        // 5. Arabic-Indic and Persian digits are read as the Latin digits they are.
        QTest::newRow("arabic-indic-digits")
            << QString::fromUtf16(u"٠٧٩٠٠٠٠٠٠٠")
            << jo << QStringLiteral("+962790000000");
        QTest::newRow("persian-digits")
            << QString::fromUtf16(u"۰۷۹۰۰۰۰۰۰۰")
            << jo << QStringLiteral("+962790000000");
        // 6. Too short after the country code is applied: refused, not sent.
        QTest::newRow("too-short") << QStringLiteral("079") << jo << QString();
        // Edges: a country whose dial code carries an area code; no dial code means nothing is
        // guessed; letters are refused.
        QTest::newRow("dial-with-area-code") << QStringLiteral("5550123") << QStringLiteral("+1876")
                                             << QStringLiteral("+18765550123");
        QTest::newRow("no-dial-code-no-guess") << QStringLiteral("0790000000") << QString()
                                               << QString();
        QTest::newRow("letters-are-refused") << QStringLiteral("07900abc00") << jo << QString();
    }

    void phoneNormalisationWithACountry()
    {
        QFETCH(QString, raw);
        QFETCH(QString, dial);
        QFETCH(QString, expected);

        QCOMPARE(ControlPlaneClient::normalisePhoneE164(raw, dial), expected);
    }

    // --- Phase 5 plan 06: the bundled country list is the website's, and the default is Jordan ---
    //
    // `countries.json` is a copy of `seathub-web/src/lib/countries.ts`. The fork cannot read the
    // website's file (it is another repository), so the copy is pinned here by its size and by the
    // entry the default depends on; the plan's own verify compares it with the website's list.
    // Changing either list means changing the other and this number in the same breath.

    void countryList_isTheWebsitesListInSizeAndInTheEntryTheDefaultDependsOn()
    {
        const QVariantList rows = SeatHubCountries::all();
        QCOMPARE(rows.size(), 197);

        const QVariantMap jordan = SeatHubCountries::find(SeatHubCountries::defaultIso());
        QCOMPARE(SeatHubCountries::defaultIso(), QStringLiteral("JO"));
        QCOMPARE(jordan.value(QStringLiteral("name")).toString(), QStringLiteral("Jordan"));
        QCOMPARE(jordan.value(QStringLiteral("dial")).toString(), QStringLiteral("+962"));
    }

    void countryList_hasWellFormedRowsWithUniqueIsoCodes()
    {
        QSet<QString> seen;
        for (const QVariant& row : SeatHubCountries::all()) {
            const QVariantMap map = row.toMap();
            const QString iso = map.value(QStringLiteral("iso")).toString();
            QVERIFY2(iso.size() == 2 && iso == iso.toUpper(), qPrintable(iso));
            QVERIFY2(!seen.contains(iso), qPrintable(iso));
            seen.insert(iso);
            QVERIFY(!map.value(QStringLiteral("name")).toString().isEmpty());
            const QString dial = map.value(QStringLiteral("dial")).toString();
            QVERIFY2(QRegularExpression(QStringLiteral("^\\+[1-9][0-9]{0,6}$")).match(dial).hasMatch(),
                     qPrintable(iso + QLatin1Char(' ') + dial));
        }
        // Names with non-ASCII letters survived the trip through the resource.
        QCOMPARE(SeatHubCountries::find(QStringLiteral("ci")).value(QStringLiteral("name")).toString(),
                 QString::fromUtf16(u"Côte d'Ivoire"));
    }

    // --- Phase 5 plan 06: where the country tag starts -----------------------------------------

    void region_machineThenLocaleThenTheDefault()
    {
        // The machine's own region wins.
        QCOMPARE(SeatHubRegion::pick(QStringLiteral("GB"), QStringLiteral("US")), QStringLiteral("GB"));
        // Case does not matter, and the answer is upper-case.
        QCOMPARE(SeatHubRegion::pick(QStringLiteral("gb"), QString()), QStringLiteral("GB"));
        // A machine answer that is not a country the list knows is no answer.
        QCOMPARE(SeatHubRegion::pick(QStringLiteral("001"), QStringLiteral("US")), QStringLiteral("US"));
        QCOMPARE(SeatHubRegion::pick(QStringLiteral("ZZ"), QStringLiteral("DE")), QStringLiteral("DE"));
        // Neither: Jordan, the website's default.
        QCOMPARE(SeatHubRegion::pick(QString(), QString()), QStringLiteral("JO"));
        QCOMPARE(SeatHubRegion::pick(QStringLiteral("ZZ"), QStringLiteral("QQ")), QStringLiteral("JO"));
    }

    void region_theMachinesOwnAnswerIsAlwaysARowInTheList()
    {
        // Whatever this machine reports, the tag never starts on a country the picker lacks.
        const QString initial = SeatHubRegion::initialCountryCode();
        QVERIFY2(SeatHubCountries::contains(initial), qPrintable(initial));
    }

    // --- Phase 5 plan 06: the website addresses are the spec's four strings --------------------

    void webOrigin_holdsTheSpecsAddressesAndNothingElse()
    {
        QCOMPARE(QString::fromLatin1(SeatHubWeb::kOrigin), QStringLiteral("https://sevenhills.damra.co"));
        QCOMPARE(SeatHubWeb::url(SeatHubWeb::kTopUpPath).toString(),
                 QStringLiteral("https://sevenhills.damra.co/topup"));
        QCOMPARE(SeatHubWeb::url(SeatHubWeb::kSignUpPath).toString(),
                 QStringLiteral("https://sevenhills.damra.co/login?mode=signup"));
        QCOMPARE(SeatHubWeb::url(SeatHubWeb::kResetPasswordPath).toString(),
                 QStringLiteral("https://sevenhills.damra.co/forgot-password"));
        // A path the spec does not record yields no address at all.
        QVERIFY(SeatHubWeb::url("/somewhere-else").isEmpty());
        // The profile's `Open the website` link is the origin itself (OD-11), nothing after it.
        QCOMPARE(SeatHubWeb::homeUrl().toString(), QStringLiteral("https://sevenhills.damra.co"));
        QVERIFY(SeatHubWeb::homeUrl().path().isEmpty());
        QVERIFY(SeatHubWeb::homeUrl().query().isEmpty());
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
        client.fetchSessionList(QString(), 15, done);
        client.fetchWalletHistory(QString(), 15, done);
        client.fetchTopupNotices(QString(), 15, done);
        client.fetchUsage(done);
        QTRY_COMPARE(completed, 15);

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

    // --- Phase 5 plan 09: the profile's three lists and its totals ----------------------------------

    void listPath_data()
    {
        QTest::addColumn<QString>("cursor");
        QTest::addColumn<QString>("expected");

        QTest::newRow("first page") << QString()
                                    << QStringLiteral("/api/sessions?limit=15");
        QTest::newRow("a plain cursor") << QStringLiteral("abc123")
                                        << QStringLiteral("/api/sessions?limit=15&cursor=abc123");
        // The cursor is the server's and opaque: whatever it holds, nothing in it can add structure to
        // the query, and nothing else is done to it.
        QTest::newRow("base64 characters")
            << QStringLiteral("ab+/=cd")
            << QStringLiteral("/api/sessions?limit=15&cursor=ab%2B%2F%3Dcd");
        QTest::newRow("a cursor that tries to add a parameter")
            << QStringLiteral("x&limit=100")
            << QStringLiteral("/api/sessions?limit=15&cursor=x%26limit%3D100");
        QTest::newRow("a cursor that tries to end the query")
            << QStringLiteral("x#y") << QStringLiteral("/api/sessions?limit=15&cursor=x%23y");
    }

    void listPath()
    {
        QFETCH(QString, cursor);
        QFETCH(QString, expected);

        QCOMPARE(ControlPlaneClient::listPath(QStringLiteral("/api/sessions"), 15, cursor), expected);
    }

    void listRoutes_data()
    {
        QTest::addColumn<int>("which");
        QTest::addColumn<QString>("path");

        QTest::newRow("sessions") << 0 << QStringLiteral("/api/sessions");
        QTest::newRow("credit history") << 1 << QStringLiteral("/api/wallet/history");
        QTest::newRow("top-up notices") << 2 << QStringLiteral("/api/topup-notices");
    }

    void listRoutes()
    {
        QFETCH(int, which);
        QFETCH(QString, path);

        ControlPlaneClient client;
        auto* fake = new FakeNetworkAccessManager;
        client.setNetworkAccessManager(fake);
        client.setAccessToken(QStringLiteral("opaque-access-token"));
        fake->status = 200;
        fake->body = QByteArrayLiteral("{}");

        int completed = 0;
        const ControlPlaneClient::Callback done = [&](const ControlPlaneResult&) { ++completed; };
        auto ask = [&](const QString& cursor) {
            switch (which) {
            case 0:
                client.fetchSessionList(cursor, 15, done);
                break;
            case 1:
                client.fetchWalletHistory(cursor, 15, done);
                break;
            default:
                client.fetchTopupNotices(cursor, 15, done);
                break;
            }
        };

        // The first page: the documented route, a page of fifteen and no cursor at all.
        ask(QString());
        QTRY_COMPARE(completed, 1);
        QCOMPARE(fake->lastPath, path);
        QCOMPARE(fake->lastMethod, QByteArrayLiteral("GET"));
        QCOMPARE(fake->lastQuery, QStringLiteral("limit=15"));
        QCOMPARE(fake->lastRequest.rawHeader("Authorization"),
                 QByteArrayLiteral("Bearer opaque-access-token"));

        // The next page: the server's cursor, sent back as it came.
        ask(QStringLiteral("opaque+cursor/=="));
        QTRY_COMPARE(completed, 2);
        QCOMPARE(fake->lastPath, path);
        QCOMPARE(fake->lastQuery, QStringLiteral("limit=15&cursor=opaque%2Bcursor%2F%3D%3D"));
        QCOMPARE(fake->lastRequest.rawHeader("Authorization"),
                 QByteArrayLiteral("Bearer opaque-access-token"));
    }

    void usageRoute_getsTheDocumentedRouteWithTheCredentialAndNothingElse()
    {
        ControlPlaneClient client;
        auto* fake = new FakeNetworkAccessManager;
        client.setNetworkAccessManager(fake);
        client.setAccessToken(QStringLiteral("opaque-access-token"));
        fake->status = 200;
        fake->body = QByteArrayLiteral("{\"minutes_played\":135,\"balance_minutes\":45}");

        ControlPlaneResult captured;
        bool called = false;
        client.fetchUsage([&](const ControlPlaneResult& result) {
            captured = result;
            called = true;
        });
        QTRY_VERIFY(called);

        QCOMPARE(fake->lastPath, QStringLiteral("/api/usage"));
        QCOMPARE(fake->lastMethod, QByteArrayLiteral("GET"));
        QVERIFY(fake->lastQuery.isEmpty());
        QCOMPARE(fake->lastRequest.rawHeader("Authorization"),
                 QByteArrayLiteral("Bearer opaque-access-token"));

        UsageInfo usage;
        QVERIFY(captured.ok);
        QVERIFY(UsageInfo::parse(captured.body, &usage));
        QCOMPARE(usage.minutesPlayed, qint64(135));
        QCOMPARE(usage.balanceMinutes, qint64(45));
    }

    void aListReadWithoutACredentialIsRefusedAndAttachesNoHeader()
    {
        ControlPlaneClient client;
        auto* fake = new FakeNetworkAccessManager;
        client.setNetworkAccessManager(fake);
        fake->status = 401;
        fake->body = QByteArrayLiteral("{\"error\":\"Please sign in.\",\"reference\":\"SH-3K2XQ1\"}");

        ControlPlaneResult captured;
        bool called = false;
        client.fetchSessionList(QString(), 15, [&](const ControlPlaneResult& result) {
            captured = result;
            called = true;
        });
        QTRY_VERIFY(called);

        QVERIFY(fake->lastRequest.rawHeader("Authorization").isEmpty());
        QVERIFY(!captured.ok);
        QCOMPARE(captured.statusCode, 401);
        QCOMPARE(captured.toFailure().kind, FailureKind::Auth);
    }

    void sessionPage_parsesTheRowsAndTheCursorAndDrawsNothingAboutARig()
    {
        // The wire may carry members this client does not draw. Even a rig's name and id, which the
        // contract's session row does not have, are not read into anything.
        const QByteArray text = QByteArrayLiteral(
            "{\"sessions\":["
            "{\"id\":\"a\",\"state\":\"COMPLETED\",\"minutes_billed\":135,"
            "\"requested_at\":\"2026-09-12T18:40:00Z\",\"started_at\":\"2026-09-12T18:41:00Z\","
            "\"ended_at\":\"2026-09-12T20:56:00Z\",\"end_reason\":\"CUSTOMER_ENDED\","
            "\"host_id\":\"rig-7\",\"host_name\":\"Rig 07\"},"
            "{\"id\":\"b\",\"state\":\"EXPIRED\",\"minutes_billed\":0,"
            "\"requested_at\":\"2026-09-11T10:00:00Z\",\"started_at\":null,\"ended_at\":null,"
            "\"end_reason\":null}"
            "],\"next_cursor\":\"opaque-1\"}");
        const QJsonObject body = QJsonDocument::fromJson(text).object();

        CustomerSessionPage page;
        QVERIFY(CustomerSessionPage::parse(body, &page));
        QCOMPARE(page.rows.size(), 2);
        QCOMPARE(page.nextCursor, QStringLiteral("opaque-1"));

        QCOMPARE(page.rows.at(0).id, QStringLiteral("a"));
        QCOMPARE(page.rows.at(0).state, QStringLiteral("COMPLETED"));
        QCOMPARE(page.rows.at(0).minutesBilled, 135);
        QCOMPARE(page.rows.at(0).requestedAt, QStringLiteral("2026-09-12T18:40:00Z"));
        QCOMPARE(page.rows.at(0).endReason, QStringLiteral("CUSTOMER_ENDED"));

        // A null end reason is no reason, not the word "null".
        QCOMPARE(page.rows.at(1).endReason, QString());
        QCOMPARE(page.rows.at(1).minutesBilled, 0);

        // The last page has a null cursor, and that is the empty string.
        QJsonObject last = body;
        last.insert(QStringLiteral("next_cursor"), QJsonValue::Null);
        QVERIFY(CustomerSessionPage::parse(last, &page));
        QVERIFY(page.nextCursor.isEmpty());

        // An empty page is a page.
        QJsonObject empty;
        empty.insert(QStringLiteral("sessions"), QJsonArray());
        empty.insert(QStringLiteral("next_cursor"), QJsonValue::Null);
        QVERIFY(CustomerSessionPage::parse(empty, &page));
        QVERIFY(page.rows.isEmpty());
    }

    void ledgerPage_parsesTheSignedMinutesAsTheServerSentThem()
    {
        const QByteArray text = QByteArrayLiteral(
            "{\"entries\":["
            "{\"id\":\"l1\",\"kind\":\"topup_credit\",\"amount_minutes\":300,\"session_id\":null,"
            "\"minute_index\":null,\"created_at\":\"2026-09-12T18:40:00Z\"},"
            "{\"id\":\"l2\",\"kind\":\"session_debit\",\"amount_minutes\":-1,"
            "\"session_id\":\"a\",\"minute_index\":4,\"created_at\":\"2026-09-12T18:45:00Z\"}"
            "],\"next_cursor\":null}");

        LedgerPage page;
        QVERIFY(LedgerPage::parse(QJsonDocument::fromJson(text).object(), &page));
        QCOMPARE(page.rows.size(), 2);
        QCOMPARE(page.rows.at(0).kind, QStringLiteral("topup_credit"));
        QCOMPARE(page.rows.at(0).amountMinutes, qint64(300));
        QCOMPARE(page.rows.at(1).kind, QStringLiteral("session_debit"));
        QCOMPARE(page.rows.at(1).amountMinutes, qint64(-1));
        QVERIFY(page.nextCursor.isEmpty());
    }

    void noticePage_anOpenNoticeHasNoMinutesAndAClosedOneHasTheOperatorsOwn()
    {
        const QByteArray text = QByteArrayLiteral(
            "{\"notices\":["
            "{\"id\":\"n1\",\"sent_at\":\"2026-09-13T09:00:00Z\",\"credited_at\":null,"
            "\"credited_minutes\":null},"
            "{\"id\":\"n2\",\"sent_at\":\"2026-09-10T09:00:00Z\","
            "\"credited_at\":\"2026-09-10T09:30:00Z\",\"credited_minutes\":300}"
            "],\"next_cursor\":\"opaque-2\"}");

        TopupNoticePage page;
        QVERIFY(TopupNoticePage::parse(QJsonDocument::fromJson(text).object(), &page));
        QCOMPARE(page.rows.size(), 2);
        QCOMPARE(page.nextCursor, QStringLiteral("opaque-2"));

        QCOMPARE(page.rows.at(0).creditedAt, QString());
        QCOMPARE(page.rows.at(0).creditedMinutes, qint64(-1));
        QCOMPARE(page.rows.at(1).creditedAt, QStringLiteral("2026-09-10T09:30:00Z"));
        QCOMPARE(page.rows.at(1).creditedMinutes, qint64(300));
    }

    void aPageThatIsNotAPageIsNotAnEmptyOne()
    {
        // A body with no array where the list belongs, and a row with no id, are contract violations:
        // reading either as "nothing to show" would tell a customer their history is empty.
        CustomerSessionPage sessions;
        LedgerPage ledger;
        TopupNoticePage notices;

        QVERIFY(!CustomerSessionPage::parse(QJsonObject(), &sessions));
        QVERIFY(!LedgerPage::parse(QJsonObject(), &ledger));
        QVERIFY(!TopupNoticePage::parse(QJsonObject(), &notices));

        QJsonObject notAnArray;
        notAnArray.insert(QStringLiteral("sessions"), QStringLiteral("none"));
        QVERIFY(!CustomerSessionPage::parse(notAnArray, &sessions));

        QJsonObject rowWithoutAnId;
        QJsonArray rows;
        QJsonObject row;
        row.insert(QStringLiteral("minutes_billed"), 5);
        rows.append(row);
        rowWithoutAnId.insert(QStringLiteral("sessions"), rows);
        QVERIFY(!CustomerSessionPage::parse(rowWithoutAnId, &sessions));

        // A ledger row whose amount is not a number is not an amount of zero.
        QJsonObject ledgerRow;
        ledgerRow.insert(QStringLiteral("id"), QStringLiteral("l"));
        ledgerRow.insert(QStringLiteral("kind"), QStringLiteral("refund"));
        ledgerRow.insert(QStringLiteral("amount_minutes"), QStringLiteral("30"));
        QJsonArray ledgerRows;
        ledgerRows.append(ledgerRow);
        QJsonObject ledgerBody;
        ledgerBody.insert(QStringLiteral("entries"), ledgerRows);
        QVERIFY(!LedgerPage::parse(ledgerBody, &ledger));
    }

    void usageParse_neverInventsATotal()
    {
        UsageInfo usage;

        // Zero played and zero left are real answers.
        QJsonObject zero;
        zero.insert(QStringLiteral("minutes_played"), 0);
        zero.insert(QStringLiteral("balance_minutes"), 0);
        QVERIFY(UsageInfo::parse(zero, &usage));
        QCOMPARE(usage.minutesPlayed, qint64(0));
        QCOMPARE(usage.balanceMinutes, qint64(0));

        // A body missing either total, carrying text, or negative is not a total.
        QVERIFY(!UsageInfo::parse(QJsonObject(), &usage));
        QJsonObject onlyOne;
        onlyOne.insert(QStringLiteral("minutes_played"), 30);
        QVERIFY(!UsageInfo::parse(onlyOne, &usage));
        QJsonObject text;
        text.insert(QStringLiteral("minutes_played"), QStringLiteral("30"));
        text.insert(QStringLiteral("balance_minutes"), 10);
        QVERIFY(!UsageInfo::parse(text, &usage));
        QJsonObject negative;
        negative.insert(QStringLiteral("minutes_played"), -3);
        negative.insert(QStringLiteral("balance_minutes"), 10);
        QVERIFY(!UsageInfo::parse(negative, &usage));
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
