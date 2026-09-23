/*****************************************************************************
 * SeatHub fork - unit tests for silent pairing (Plan 03-03 Task 2, STREAM-03).
 *
 * STREAM-03 is "Customer connects to a rig without typing a PIN". The strongest form of that
 * is not "we did not show a PIN field" - it is that this client has no PIN surface at all, so
 * there is nothing to show. These tests hold that line directly:
 *
 *   * the control-plane-issued PIN reaches the engine seam and nothing else;
 *   * no signal carries it, so no view can obtain it;
 *   * every failure is fail-closed, produces a SeatHub error with a reference, and never a
 *     dialog;
 *   * the 90-second deadline is real.
 *****************************************************************************/

#include <QtTest>
#include <QBuffer>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTimer>

#include "seathub/pairing_controller.h"
#include "seathub/pairing_seam.h"

namespace {

const char* kSessionId = "aaaabbbb-cccc-dddd-eeee-ffff00001111";
const char* kPin = "4821";
const char* kClientUuid = "9f1c6f5e-3a1e-4b1e-9f2e-0f1a2b3c4d5e";

class FakeReply : public QNetworkReply
{
    Q_OBJECT

public:
    FakeReply(int httpStatus, const QByteArray& body, QObject* parent)
        : QNetworkReply(parent)
    {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, QVariant(httpStatus));
        m_buffer.setData(body);
        m_buffer.open(QIODevice::ReadOnly);
        open(QIODevice::ReadOnly);
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

class FakeNetworkAccessManager : public QNetworkAccessManager
{
    Q_OBJECT

public:
    QList<int> statuses;
    QList<QByteArray> bodies;
    int calls = 0;

protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest&, QIODevice*) override
    {
        const int index = qMin(calls, statuses.size() - 1);
        const int status = statuses.isEmpty() ? 200 : statuses.at(qMax(0, index));
        const QByteArray body = bodies.isEmpty() ? QByteArray() : bodies.at(qMax(0, index));
        ++calls;
        return new FakeReply(status, body, this);
    }
};

// Answers by path, so the session read that rides every poll tick can be told apart from the
// authorization poll it rides with. `/pairing` is always the documented 409 (no target yet).
class PathNetworkAccessManager : public QNetworkAccessManager
{
    Q_OBJECT

public:
    int sessionStatus = 200;
    QString sessionState = QStringLiteral("PREPARING");
    QString sessionId = QLatin1String(kSessionId);
    int sessionReads = 0;
    int pairingPolls = 0;

protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override
    {
        const QString path = request.url().path();
        if (path.endsWith(QLatin1String("/pairing"))) {
            ++pairingPolls;
            QJsonObject object;
            object.insert(QStringLiteral("status_code"), 409);
            object.insert(QStringLiteral("status"), false);
            object.insert(QStringLiteral("error"), QStringLiteral("The session isn't ready yet."));
            object.insert(QStringLiteral("reference"), QStringLiteral("SH-3K2XQ1"));
            return new FakeReply(409, QJsonDocument(object).toJson(QJsonDocument::Compact), this);
        }
        ++sessionReads;
        QJsonObject session;
        session.insert(QStringLiteral("id"), sessionId);
        session.insert(QStringLiteral("state"), sessionState);
        session.insert(QStringLiteral("quality_profile"), QStringLiteral("1080p60"));
        session.insert(QStringLiteral("minutes_billed"), 0);
        session.insert(QStringLiteral("reconnect_count"), 0);
        return new FakeReply(sessionStatus, QJsonDocument(session).toJson(QJsonDocument::Compact), this);
    }
};

// The 409 the contract documents for "this session has no pairing target yet".
QByteArray conflictBody()
{
    QJsonObject object;
    object.insert(QStringLiteral("status_code"), 409);
    object.insert(QStringLiteral("status"), false);
    object.insert(QStringLiteral("error"), QStringLiteral("The session isn't ready yet."));
    object.insert(QStringLiteral("reference"), QStringLiteral("SH-3K2XQ1"));
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray authorizationBody(const QString& pin)
{
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
    body.insert(QStringLiteral("session_id"), QLatin1String(kSessionId));
    body.insert(QStringLiteral("pairing_pin"), pin.isEmpty() ? QJsonValue(QJsonValue::Null)
                                                            : QJsonValue(pin));
    body.insert(QStringLiteral("host_address"), QStringLiteral("203.0.113.7"));
    body.insert(QStringLiteral("ports"), ports);
    body.insert(QStringLiteral("quality_profile"), QStringLiteral("1080p60"));
    body.insert(QStringLiteral("lease"), lease);
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

// Records exactly what it was asked to pair against. This is the only thing in the process that
// is allowed to see the PIN.
class RecordingSeam : public PairingSeam
{
public:
    int calls = 0;
    PairingTarget lastTarget;
    bool ok = true;
    QString uuid = QString::fromLatin1(kClientUuid);
    QString engineError;

    void pair(const PairingTarget& target,
              std::function<void(bool, const QString&, const QString&)> done) override
    {
        ++calls;
        lastTarget = target;
        // Delivered asynchronously, like a real handshake.
        QTimer::singleShot(0, [done, this]() { done(ok, uuid, engineError); });
    }
};

// Instantiates the client the controller drives, points it at the fake transport, and hands it
// over. `ControlPlaneClient::setNetworkAccessManager` is the production injection seam, so the
// test exercises the real request path with no server anywhere.
ControlPlaneClient* wire(PairingController& controller, QNetworkAccessManager* fake)
{
    auto* client = new ControlPlaneClient(&controller);
    client->setNetworkAccessManager(fake);
    controller.setControlPlane(client);
    return client;
}

// ---------------------------------------------------------------------------------------------
// The production seam. Its handshake is the one part of silent pairing that cannot run here - it
// needs a Sunshine host - so these tests fake the handshake and hold the seam's own contract: the
// deadline, the exactly-once rule, the fail-closed branches, and the PIN's confinement.
// ---------------------------------------------------------------------------------------------

PairingTarget pairingTarget()
{
    PairingTarget target;
    target.sessionId = QString::fromLatin1(kSessionId);
    target.hostAddress = QStringLiteral("203.0.113.7");
    target.httpsPort = 47984;
    target.pairingPin = QString::fromLatin1(kPin);
    return target;
}

PairingHandshakeResult handshakeResult(bool ok, const QString& identity,
                                       const QString& diagnostic = QString())
{
    PairingHandshakeResult result;
    result.ok = ok;
    result.clientIdentity = identity;
    result.engineError = diagnostic;
    return result;
}

/// Everything the seam said, in the order it said it.
struct SeamReport
{
    QVector<bool> outcomes;
    QVector<QString> identities;
    QVector<QString> diagnostics;

    int count() const { return outcomes.size(); }
};

std::function<void(bool, const QString&, const QString&)> recordInto(SeamReport* report)
{
    return [report](bool ok, const QString& identity, const QString& diagnostic) {
        report->outcomes.append(ok);
        report->identities.append(identity);
        report->diagnostics.append(diagnostic);
    };
}

} // namespace

class TstPairing : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<SeatHubFailure>("SeatHubFailure");
    }

    void constants_areTheLockedValues()
    {
        // D-08: 250 ms poll, 90 s deadline. Neither is the client's to choose.
        QCOMPARE(PairingController::kPollIntervalMs, 250);
        QCOMPARE(PairingController::kDeadlineMs, 90000);
    }

    void happyPath_authorizesThenPairsAndNeverInvolvesTheCustomer()
    {
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);

        fake->statuses = { 200 };
        fake->bodies = { authorizationBody(QString::fromLatin1(kPin)) };

        QSignalSpy granted(&controller, &PairingController::authorizationGranted);
        QSignalSpy completed(&controller, &PairingController::pairingCompleted);
        QSignalSpy failed(&controller, &PairingController::pairingFailed);

        controller.start(QString::fromLatin1(kSessionId));

        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(granted.count(), 1);
        QCOMPARE(failed.count(), 0);

        // The seam got the address and the PIN...
        QCOMPARE(seam->calls, 1);
        QCOMPARE(seam->lastTarget.hostAddress, QStringLiteral("203.0.113.7"));
        QCOMPARE(seam->lastTarget.httpsPort, 47984);
        QCOMPARE(seam->lastTarget.pairingPin, QString::fromLatin1(kPin));
        QCOMPARE(seam->lastTarget.sessionId, QString::fromLatin1(kSessionId));

        // ...and the result carries the exact Sunshine client UUID, which is the only thing that
        // ever identifies this client (Pitfall 3, D-07).
        QCOMPARE(completed.at(0).at(0).toString(), QString::fromLatin1(kClientUuid));
        QCOMPARE(controller.state(), QStringLiteral("ready"));
    }

    void thePinIsNotOnAnySignal()
    {
        // The PIN's only route out of the control plane is the engine seam. If it ever appeared
        // in a signal argument a view could show it, which is the thing STREAM-03 forbids.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);

        fake->statuses = { 200 };
        fake->bodies = { authorizationBody(QString::fromLatin1(kPin)) };

        QSignalSpy granted(&controller, &PairingController::authorizationGranted);
        QSignalSpy completed(&controller, &PairingController::pairingCompleted);

        controller.start(QString::fromLatin1(kSessionId));
        QTRY_COMPARE(completed.count(), 1);

        // Every argument of every emission, checked for the PIN. `QSignalSpy` is a QObject and
        // cannot be copied into a container, so the check is applied to each spy in turn.
        const auto assertNoPin = [](const QSignalSpy& spy) {
            for (const QList<QVariant>& emission : spy) {
                for (const QVariant& argument : emission) {
                    QVERIFY2(!argument.toString().contains(QLatin1String(kPin)),
                             "the PIN must never be a signal argument");
                }
            }
        };
        assertNoPin(granted);
        assertNoPin(completed);

        // The controller exposes no accessor that returns it either.
        QVERIFY(!controller.sessionId().contains(QLatin1String(kPin)));
    }

    void authorizationGrantedCarriesTheQualityProfile()
    {
        // D-37 / WR-05: the session authorization's `quality_profile` is what
        // `SettingsBridge::applySessionOverride()` turns into this launch's resolution and frame
        // rate - in memory, without writing a saved preference (D-12, D-37). The controller is the
        // only object that ever sees the authorization, so the profile has to ride out on this
        // signal; if it stopped, the override would silently never be applied and the stream would
        // fall back to the customer's saved resolution with nothing to show for it.
        //
        // This is the same signal that used to carry nothing, which is why the override had no
        // caller at all (`applySessionOverride()` was orphaned - verifier G5).
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);

        fake->statuses = { 200 };
        fake->bodies = { authorizationBody(QString::fromLatin1(kPin)) };

        QSignalSpy granted(&controller, &PairingController::authorizationGranted);
        QSignalSpy completed(&controller, &PairingController::pairingCompleted);

        controller.start(QString::fromLatin1(kSessionId));
        QTRY_COMPARE(completed.count(), 1);

        QCOMPARE(granted.count(), 1);
        QCOMPARE(granted.at(0).at(0).toString(), QStringLiteral("1080p60"));

        // The profile is the only field on this signal. The lease, the ports and the PIN stay
        // inside the controller - least of all the PIN, which is on no signal at all.
        QCOMPARE(granted.at(0).size(), 1);
    }

    void conflict_isAWaitNotAFailure()
    {
        // `GET /api/sessions/{id}/pairing` 409s until the host has a pairing target. The early
        // polls are expected and must not surface as an error.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);
        controller.setPollIntervalMs(1);

        fake->statuses = { 409, 409, 200 };
        fake->bodies = { conflictBody(), conflictBody(),
                         authorizationBody(QString::fromLatin1(kPin)) };

        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        QSignalSpy completed(&controller, &PairingController::pairingCompleted);

        controller.start(QString::fromLatin1(kSessionId));
        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(failed.count(), 0);
    }

    void nullPin_pollsAgainRatherThanPairingWithNothing()
    {
        // `pairing_pin` is nullable: null means the host has not been told to expect this client
        // yet. Seating an empty PIN into the engine would be a wasted handshake.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);
        controller.setPollIntervalMs(1);

        fake->statuses = { 200, 200 };
        fake->bodies = { authorizationBody(QString()), authorizationBody(QString::fromLatin1(kPin)) };

        QSignalSpy completed(&controller, &PairingController::pairingCompleted);
        controller.start(QString::fromLatin1(kSessionId));

        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(seam->calls, 1);
        QCOMPARE(seam->lastTarget.pairingPin, QString::fromLatin1(kPin));
    }

    void conflict_doesNotSpendThePairingDeadline()
    {
        // A 409 means the rig is still being prepared. D-08's 90 s is the time a pairing has to
        // resolve (`docs/spec/README.md`), not the time the rig takes to get ready - that has its
        // own server deadline, and a session it fails reaches this client as a terminal session
        // read. Live on 2026-09-23 "Preparing the rig" took 64-81 s on a single-OS rig and the
        // clock that started at Play ran out before the rig could pair, so the customer had to
        // press Try again. Here the 409s outlast the deadline several times over, then the PIN
        // arrives, and the pairing must still go ahead.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);
        controller.setPollIntervalMs(20);
        controller.setDeadlineMs(100);

        // Each poll spends two replies (the authorization and the session read), so forty 409s
        // are about twenty polls, about 400 ms of "not yet" against a 100 ms deadline.
        for (int i = 0; i < 40; ++i) {
            fake->statuses.append(409);
            fake->bodies.append(conflictBody());
        }
        fake->statuses.append(200);
        fake->bodies.append(authorizationBody(QString::fromLatin1(kPin)));

        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        QSignalSpy completed(&controller, &PairingController::pairingCompleted);

        controller.start(QString::fromLatin1(kSessionId));
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(seam->calls, 1);
    }

    void deadline_expiresAndFailsClosedWithASeatHubError()
    {
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);
        controller.setPollIntervalMs(1);
        controller.setDeadlineMs(1);

        // Never resolves: the rig has a pairing target but no PIN ever arrives for it. (A run of
        // 409s no longer spends this deadline - see conflict_doesNotSpendThePairingDeadline.)
        fake->statuses = { 200 };
        fake->bodies = { authorizationBody(QString()) };

        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        controller.start(QString::fromLatin1(kSessionId));

        QTRY_COMPARE(failed.count(), 1);

        // A SeatHub failure with a readable reason - never a dialog, never a Moonlight error
        // surface (D-51). No reference: this deadline is the client's own, and ADR-0008 says a
        // reference the client generates resolves to nothing, which is worse than showing none.
        const SeatHubFailure failure = failed.at(0).at(0).value<SeatHubFailure>();
        QVERIFY(!failure.error.isEmpty());
        QVERIFY2(failure.reference.isEmpty(),
                 "a client-side deadline must not invent an ADR-0008 reference code");
        QCOMPARE(failure.kind, FailureKind::Local);
        QCOMPARE(seam->calls, 0);
        QCOMPARE(controller.state(), QStringLiteral("failed"));
    }

    void controlPlaneRefusalOnHttp200_isAFailure()
    {
        // Pitfall 4: the control plane's `Error` body is valid JSON on an HTTP 200.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);

        QJsonObject body;
        body.insert(QStringLiteral("status_code"), 200);
        body.insert(QStringLiteral("status"), false);
        body.insert(QStringLiteral("error"), QStringLiteral("That session has already ended."));
        body.insert(QStringLiteral("reference"), QStringLiteral("SH-4F7KQ2"));

        fake->statuses = { 200 };
        fake->bodies = { QJsonDocument(body).toJson(QJsonDocument::Compact) };

        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        controller.start(QString::fromLatin1(kSessionId));

        QTRY_COMPARE(failed.count(), 1);
        const SeatHubFailure failure = failed.at(0).at(0).value<SeatHubFailure>();
        // The control plane's own sentence and reference, not the client's substitute.
        QCOMPARE(failure.error, QStringLiteral("That session has already ended."));
        QCOMPARE(failure.reference, QStringLiteral("SH-4F7KQ2"));
        QCOMPARE(failure.kind, FailureKind::Api);
        QCOMPARE(seam->calls, 0);
    }

    void engineFailure_keepsItsTextOutOfWhatTheCustomerReads()
    {
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        seam->ok = false;
        seam->engineError = QStringLiteral("NvPairingManager: salt mismatch at 0x7ffd");
        wire(controller, fake);
        controller.setSeam(seam);

        fake->statuses = { 200 };
        fake->bodies = { authorizationBody(QString::fromLatin1(kPin)) };

        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        controller.start(QString::fromLatin1(kSessionId));

        QTRY_COMPARE(failed.count(), 1);
        const SeatHubFailure failure = failed.at(0).at(0).value<SeatHubFailure>();

        // D-51: engine text is diagnostic only.
        QVERIFY2(!failure.error.contains(QStringLiteral("NvPairingManager")),
                 qPrintable(failure.error));
        QVERIFY(!failure.error.contains(QStringLiteral("0x7ffd")));
        QCOMPARE(failure.diagnostic, QStringLiteral("NvPairingManager: salt mismatch at 0x7ffd"));
        // And `toVariantMap()` - the map QML actually sees - drops it entirely.
        QVERIFY(!failure.toVariantMap().contains(QStringLiteral("diagnostic")));
    }

    void successWithoutAClientUuid_isARefusal()
    {
        // The UUID is the only identifier teardown can verify against (Pitfall 3). A handshake
        // that reports success without one cannot be torn down, so it is not a success.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        seam->uuid.clear();
        wire(controller, fake);
        controller.setSeam(seam);

        fake->statuses = { 200 };
        fake->bodies = { authorizationBody(QString::fromLatin1(kPin)) };

        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        QSignalSpy completed(&controller, &PairingController::pairingCompleted);
        controller.start(QString::fromLatin1(kSessionId));

        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(completed.count(), 0);
    }

    void noSeam_failsClosedRatherThanClaimingSuccess()
    {
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        wire(controller, fake);
        // No seam set.

        fake->statuses = { 200 };
        fake->bodies = { authorizationBody(QString::fromLatin1(kPin)) };

        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        QSignalSpy completed(&controller, &PairingController::pairingCompleted);
        controller.start(QString::fromLatin1(kSessionId));

        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(completed.count(), 0);
    }

    void theSessionIsReadOnEveryPollTickAndReportedAsTheServerSaidIt()
    {
        // CUST-12 / ADR-0055: the connecting stages come from the session's own state, read on the
        // tick this controller already runs - the same interval, no timer of its own.
        PairingController controller;
        auto* fake = new PathNetworkAccessManager;
        wire(controller, fake);
        controller.setPollIntervalMs(1);

        QSignalSpy reads(&controller, &PairingController::sessionRead);
        QSignalSpy failed(&controller, &PairingController::pairingFailed);

        controller.start(QString::fromLatin1(kSessionId));
        QTRY_VERIFY(reads.count() >= 2);
        QCOMPARE(reads.at(0).at(0).value<SessionInfo>().state, QStringLiteral("PREPARING"));
        QCOMPARE(reads.at(0).at(0).value<SessionInfo>().id, QString::fromLatin1(kSessionId));

        // The answer moves on as the session does.
        fake->sessionState = QStringLiteral("READY");
        QTRY_COMPARE(reads.last().at(0).value<SessionInfo>().state, QStringLiteral("READY"));

        // One read per authorization poll at most: it rides the tick, it does not add ticks.
        QVERIFY(fake->sessionReads <= fake->pairingPolls);
        QCOMPARE(failed.count(), 0);

        // And it stops with the poll.
        controller.cancel();
        const int readsAtCancel = fake->sessionReads;
        QTest::qWait(30);
        QCOMPARE(fake->sessionReads, readsAtCancel);
    }

    void aSessionReadThatFailedIsDroppedAndDoesNotFailPairing()
    {
        PairingController controller;
        auto* fake = new PathNetworkAccessManager;
        wire(controller, fake);
        controller.setPollIntervalMs(1);
        QSignalSpy reads(&controller, &PairingController::sessionRead);
        QSignalSpy failed(&controller, &PairingController::pairingFailed);

        // A read that failed says nothing about the session, and does not fail pairing.
        fake->sessionStatus = 500;
        controller.start(QString::fromLatin1(kSessionId));
        QTRY_VERIFY(fake->sessionReads >= 3);
        QCOMPARE(reads.count(), 0);
        QCOMPARE(failed.count(), 0);

        controller.cancel();
    }

    void cancel_stopsEverything()
    {
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);
        controller.setPollIntervalMs(1);

        fake->statuses = { 409 };
        fake->bodies = { conflictBody() };

        controller.start(QString::fromLatin1(kSessionId));
        controller.cancel();

        QCOMPARE(controller.state(), QStringLiteral("idle"));
        QVERIFY(controller.sessionId().isEmpty());

        const int callsAtCancel = fake->calls;
        QTest::qWait(20);
        // No further polls: cancel really stopped the loop rather than merely changing a label.
        QCOMPARE(fake->calls, callsAtCancel);
    }

    void emptySessionId_failsImmediately()
    {
        PairingController controller;
        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        controller.start(QString());
        QCOMPARE(failed.count(), 1);
    }

    // -----------------------------------------------------------------------------------------
    // The production seam (`app/seathub/pairing_seam.{h,cpp}`, Plan 03-03 gap closure).
    // -----------------------------------------------------------------------------------------

    void seam_reportsThePairedIdentityToItsCaller()
    {
        ProductionPairingSeam seam;
        seam.setHandshake([](const PairingTarget&) {
            return handshakeResult(true, QStringLiteral("ca13d3dd610947684e95760011340f214bc8fdb1cc6b93c31d7f3f13d8caf9b0"));
        });

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE(report.count(), 1);

        QCOMPARE(report.outcomes.first(), true);
        QCOMPARE(report.identities.first(),
                 QStringLiteral("ca13d3dd610947684e95760011340f214bc8fdb1cc6b93c31d7f3f13d8caf9b0"));
        QVERIFY(report.diagnostics.first().isEmpty());
    }

    void seam_thePinReachesTheHandshakeAndNothingElseReportsIt()
    {
        ProductionPairingSeam seam;
        QString pinSeenByTheHandshake;
        seam.setHandshake([&pinSeenByTheHandshake](const PairingTarget& target) {
            pinSeenByTheHandshake = target.pairingPin;
            return handshakeResult(true, QStringLiteral("aa"));
        });

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE(report.count(), 1);

        // The handshake is the only holder, and the PIN is not on the way back out - not in the
        // identity, not in the diagnostic, which is the value the controller keeps for support.
        QCOMPARE(pinSeenByTheHandshake, QString::fromLatin1(kPin));
        QVERIFY(!report.identities.first().contains(QLatin1String(kPin)));
        QVERIFY(!report.diagnostics.first().contains(QLatin1String(kPin)));
    }

    void seam_deadline_failsClosedAndSpeaksOnce()
    {
        ProductionPairingSeam seam;
        seam.setDeadlineMs(50);

        // A handshake that does not come back inside the deadline. This is not a contrivance: the
        // first upstream pairing request is issued with no client-side timeout at all
        // (`app/backend/nvpairingmanager.cpp:237` passes 0), so it blocks until the host side
        // answers - up to Sunshine's five-minute pending-session lifetime.
        QSemaphore release;
        seam.setHandshake([&release](const PairingTarget&) {
            release.acquire();
            return handshakeResult(true, QStringLiteral("late"));
        });

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_VERIFY_WITH_TIMEOUT(report.count() == 1, 5000);

        QCOMPARE(report.outcomes.first(), false);
        QVERIFY(report.identities.first().isEmpty());
        QVERIFY(!report.diagnostics.first().isEmpty());

        // The handshake finally returns. It does not get a second word: `done` is called exactly
        // once per `pair()`.
        release.release();
        QTest::qWait(150);
        QCOMPARE(report.count(), 1);
    }

    void seam_withoutAHandshake_failsClosed()
    {
        ProductionPairingSeam seam;
        // No handshake installed - the state 03-03 shipped.

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));

        QCOMPARE(report.count(), 1);
        QCOMPARE(report.outcomes.first(), false);
        QVERIFY(report.identities.first().isEmpty());
    }

    void seam_successWithoutAnIdentity_isNotSuccess()
    {
        ProductionPairingSeam seam;
        seam.setHandshake([](const PairingTarget&) {
            // `ok` with nothing that identifies the client: there is no way to verify a teardown
            // against it, so it cannot be reported as a pairing (fail closed, Pitfall 3).
            return handshakeResult(true, QString());
        });

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE(report.count(), 1);

        QCOMPARE(report.outcomes.first(), false);
        QVERIFY(report.identities.first().isEmpty());
    }

    void seam_incompleteTarget_neverStartsAHandshake()
    {
        ProductionPairingSeam seam;
        int handshakes = 0;
        seam.setHandshake([&handshakes](const PairingTarget&) {
            ++handshakes;
            return handshakeResult(true, QStringLiteral("aa"));
        });

        PairingTarget noAddress = pairingTarget();
        noAddress.hostAddress.clear();
        SeamReport addressReport;
        seam.pair(noAddress, recordInto(&addressReport));
        QCOMPARE(addressReport.count(), 1);
        QCOMPARE(addressReport.outcomes.first(), false);

        PairingTarget noPin = pairingTarget();
        noPin.pairingPin.clear();
        SeamReport pinReport;
        seam.pair(noPin, recordInto(&pinReport));
        QCOMPARE(pinReport.count(), 1);
        QCOMPARE(pinReport.outcomes.first(), false);

        QCOMPARE(handshakes, 0);
    }

    void seam_secondCallWhileOneIsInFlight_isRefused()
    {
        ProductionPairingSeam seam;
        QSemaphore release;
        int handshakes = 0;
        seam.setHandshake([&release, &handshakes](const PairingTarget&) {
            ++handshakes;
            release.acquire();
            return handshakeResult(true, QStringLiteral("aa"));
        });

        SeamReport first;
        SeamReport second;
        seam.pair(pairingTarget(), recordInto(&first));

        // The handshake starts on a pool thread, so wait until it is actually inside it before
        // the second call - that is the state the refusal is about.
        QTRY_COMPARE(handshakes, 1);
        seam.pair(pairingTarget(), recordInto(&second));

        // One handshake at a time: two against the same rig would collide host-side and leave the
        // caller holding two results for one session.
        QCOMPARE(handshakes, 1);
        QCOMPARE(second.count(), 1);
        QCOMPARE(second.outcomes.first(), false);
        QVERIFY(second.identities.first().isEmpty());

        release.release();
        QTRY_COMPARE(first.count(), 1);
        QCOMPARE(first.outcomes.first(), true);
    }

    void fingerprint_ofACertificate_isItsSha256Hex()
    {
        // The fixture is a throwaway self-signed certificate generated for this test; only its
        // public half is stored next to the test source. Expected value: SHA-256 over its DER.
        const QString path = QFINDTESTDATA("fixtures/seathub-client-cert.pem");
        QVERIFY2(!path.isEmpty(), "the certificate fixture was not found next to the test source");

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(clientCertificateFingerprint(file.readAll()),
                 QStringLiteral("ca13d3dd610947684e95760011340f214bc8fdb1cc6b93c31d7f3f13d8caf9b0"));
    }

    void fingerprint_ofAnUnreadablePem_isEmpty()
    {
        // No identity is ever invented: an unreadable certificate produces nothing, and the seam
        // turns that into a failure rather than a success with no handle on it.
        QVERIFY(clientCertificateFingerprint(QByteArray()).isEmpty());
        QVERIFY(clientCertificateFingerprint(QByteArray("not a certificate")).isEmpty());
        QVERIFY(clientCertificateFingerprint(
                    QByteArray("-----BEGIN CERTIFICATE-----\nnope\n-----END CERTIFICATE-----\n"))
                    .isEmpty());
    }
};

QTEST_MAIN(TstPairing)

#include "tst_pairing.moc"
