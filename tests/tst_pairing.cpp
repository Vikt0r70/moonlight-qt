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
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "seathub/attempt_vocab.h"
#include "seathub/pairing_controller.h"
#include "seathub/pairing_recovery.h"
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

// `state` defaults to empty, meaning "absent" - the shape an older server still sends, with no
// `state` field on the wire at all. A non-empty value adds it, per contract 3.5.0.
QByteArray authorizationBody(const QString& pin, const QString& state = QString())
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
    if (!state.isEmpty()) {
        body.insert(QStringLiteral("state"), state);
    }
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
    // G-06.2-2: what cancel() asked the seam to clear, and how long a pair() answer stays in
    // flight (so a poll can be caught mid-handshake).
    int clears = 0;
    PairingTarget lastClearedTarget;
    int responseDelayMs = 0;

    void pair(const PairingTarget& target,
              std::function<void(bool, const QString&, const QString&)> done) override
    {
        ++calls;
        lastTarget = target;
        // Delivered asynchronously, like a real handshake.
        QTimer::singleShot(responseDelayMs, [done, this]() { done(ok, uuid, engineError); });
    }

    void clearPendingPairing(const PairingTarget& target) override
    {
        ++clears;
        lastClearedTarget = target;
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

    void theDefaultsAre30sAnd1s()
    {
        // 06.1's ADR item R1: 30 s from READY. J-16: 1 s. Neither is the client's to choose.
        QCOMPARE(PairingController::kPollIntervalMs, 1000);
        QCOMPARE(PairingController::kDeadlineMs, 30000);

        // And a fresh controller actually holds them, before `setDeadlineMs`/`setPollIntervalMs`
        // (the test-only overrides used everywhere else in this file) ever run.
        PairingController controller;
        QCOMPARE(controller.pollIntervalMs(), 1000);
        QCOMPARE(controller.deadlineMs(), 30000);
    }

    void theClockCountsFromReady()
    {
        // R1: the clock is scoped to a pairing resolving once the rig is READY, not to the rig
        // getting ready. A run of waiting answers restarts it on every "not yet" - the same rule
        // `conflict_doesNotSpendThePairingDeadline` already holds for the 409 shape - and it
        // must still pair once READY with a PIN finally arrives, however long the wait ran.
        {
            PairingController controller;
            auto* fake = new FakeNetworkAccessManager;
            auto* seam = new RecordingSeam;
            wire(controller, fake);
            controller.setSeam(seam);
            controller.setPollIntervalMs(20);
            controller.setDeadlineMs(100);

            // Each poll spends two replies; forty waiting answers is about 400 ms of "not ready",
            // several times the 100 ms deadline under test - and it must still pair.
            for (int i = 0; i < 40; ++i) {
                fake->statuses.append(200);
                fake->bodies.append(authorizationBody(QString(), QStringLiteral("PREPARING")));
            }
            fake->statuses.append(200);
            fake->bodies.append(authorizationBody(QString::fromLatin1(kPin), QStringLiteral("READY")));

            QSignalSpy failed(&controller, &PairingController::pairingFailed);
            QSignalSpy completed(&controller, &PairingController::pairingCompleted);

            controller.start(QString::fromLatin1(kSessionId));
            QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);
            QCOMPARE(failed.count(), 0);
            QCOMPARE(seam->calls, 1);
        }

        // A READY answer with no PIN pending is not the rig getting ready - it is a pairing that
        // has not resolved - so it does spend this clock, and fails closed with the timed-out
        // sentence once the deadline passes.
        {
            PairingController controller;
            auto* fake = new FakeNetworkAccessManager;
            auto* seam = new RecordingSeam;
            wire(controller, fake);
            controller.setSeam(seam);
            controller.setPollIntervalMs(1);
            controller.setDeadlineMs(1);

            fake->statuses = { 200 };
            fake->bodies = { authorizationBody(QString(), QStringLiteral("READY")) };

            QSignalSpy failed(&controller, &PairingController::pairingFailed);
            controller.start(QString::fromLatin1(kSessionId));

            QTRY_COMPARE(failed.count(), 1);
            const SeatHubFailure failure = failed.at(0).at(0).value<SeatHubFailure>();
            QCOMPARE(failure.error, QStringLiteral("The rig didn't finish connecting. Try again."));
            QCOMPARE(seam->calls, 0);
        }
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

    void authorizationGrantedCarriesNoQualityProfile()
    {
        // A-68 / D-06 reversal: the authorization's `quality_profile` is parsed into
        // `SessionAuthorization` (proven elsewhere - `tst_control_plane.cpp`) but nothing reads
        // it any more, so this signal - the only thing that could have carried it onward - now
        // carries nothing at all. This test replaces `authorizationGrantedCarriesTheQualityProfile`
        // (06.6-19), which asserted the exact D-37/WR-05 override contract this plan removes.
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
        // No field rides this signal any more - the lease, the ports, the (unused) quality
        // profile and the PIN all stay inside the controller (STREAM-03).
        QCOMPARE(granted.at(0).size(), 0);
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

    // -----------------------------------------------------------------------------------------
    // J-16 / 06.1's ADR item 3: the tracer for the waiting branch (Plan 09, Task 1).
    // -----------------------------------------------------------------------------------------

    void aWaitingAnswerIsNotAFailure()
    {
        // A 200 answer whose state is REQUESTED, ALLOCATED or PREPARING fails nothing, does not
        // emit `authorizationGranted`, restarts the clock and schedules the next poll - exactly
        // like the 409 branch it replaces on a modern server.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);
        controller.setPollIntervalMs(1);

        fake->statuses = { 200, 200 };
        fake->bodies = { authorizationBody(QString(), QStringLiteral("PREPARING")),
                         authorizationBody(QString::fromLatin1(kPin), QStringLiteral("READY")) };

        QSignalSpy granted(&controller, &PairingController::authorizationGranted);
        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        QSignalSpy completed(&controller, &PairingController::pairingCompleted);

        controller.start(QString::fromLatin1(kSessionId));

        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(failed.count(), 0);
        // Granted exactly once - for the READY answer, never for the waiting one.
        QCOMPARE(granted.count(), 1);
        QCOMPARE(seam->calls, 1);
    }

    void aReadyAnswerWithAPinPairs()
    {
        // A 200 answer with `state: "READY"` and a PIN starts pairing as today.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);

        fake->statuses = { 200 };
        fake->bodies = { authorizationBody(QString::fromLatin1(kPin), QStringLiteral("READY")) };

        QSignalSpy completed(&controller, &PairingController::pairingCompleted);
        QSignalSpy failed(&controller, &PairingController::pairingFailed);

        controller.start(QString::fromLatin1(kSessionId));

        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(seam->calls, 1);
        QCOMPARE(seam->lastTarget.pairingPin, QString::fromLatin1(kPin));
    }

    void anOlderServers409IsStillAWait()
    {
        // The existing 409 behaviour, for an older server that has not moved to the 200-with-
        // state shape, is unchanged.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);
        controller.setPollIntervalMs(1);

        fake->statuses = { 409, 200 };
        fake->bodies = { conflictBody(), authorizationBody(QString::fromLatin1(kPin),
                                                            QStringLiteral("READY")) };

        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        QSignalSpy completed(&controller, &PairingController::pairingCompleted);

        controller.start(QString::fromLatin1(kSessionId));
        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(failed.count(), 0);
    }

    void anOlderServers200WithoutStateAndNoPinPollsAgain()
    {
        // An answer with no `state` field at all (an older server) and a null PIN keeps today's
        // behaviour: granted, then poll again rather than seating an empty PIN into the engine.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);
        controller.setPollIntervalMs(1);

        fake->statuses = { 200, 200 };
        fake->bodies = { authorizationBody(QString()), authorizationBody(QString::fromLatin1(kPin)) };

        QSignalSpy completed(&controller, &PairingController::pairingCompleted);
        QSignalSpy failed(&controller, &PairingController::pairingFailed);
        controller.start(QString::fromLatin1(kSessionId));

        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(seam->calls, 1);
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

    // -----------------------------------------------------------------------------------------
    // 06.6-18/T-06.6-53: a new pair() while a stale handshake is still running abandons it rather
    // than refusing - refusing would leave a customer's Try again stuck behind a handshake that
    // will never resolve into their new session anyway. Upstream's own blocking call cannot be
    // aborted, so the old one simply keeps its pool thread until it returns and finds nothing
    // waiting for it: the seam's generation guard is what makes that harmless.
    // -----------------------------------------------------------------------------------------

    void aSupersededHandshakeResultIsDropped()
    {
        ProductionPairingSeam seam;
        QSemaphore releaseA;
        // Unconditional: an assertion below failing (RED, or a future regression) must not leave
        // A's pool thread blocked forever - a leaked `releaseA.acquire()` hangs the whole binary at
        // exit, not just this one test.
        auto cleanup = qScopeGuard([&releaseA]() { releaseA.release(); });

        int handshakes = 0;
        seam.setHandshake([&releaseA, &handshakes](const PairingTarget&) {
            ++handshakes;
            if (handshakes == 1) {
                // A's own handshake: blocks until the test releases it, well after B has already
                // been reported.
                releaseA.acquire();
                return handshakeResult(true, QStringLiteral("late-a"));
            }
            // B's own handshake: nothing supersedes it, so it returns at once.
            return handshakeResult(true, QStringLiteral("b-identity"));
        });

        PairingTarget targetA = pairingTarget();
        targetA.sessionId = QStringLiteral("session-a");
        PairingTarget targetB = pairingTarget();
        targetB.sessionId = QStringLiteral("session-b");

        SeamReport reportA;
        SeamReport reportB;
        seam.pair(targetA, recordInto(&reportA));

        // Wait until A's handshake is actually inside the pool thread before B supersedes it -
        // that is the state this behavior is about.
        QTRY_COMPARE(handshakes, 1);
        seam.pair(targetB, recordInto(&reportB));

        // B is not refused "already in flight": its own handshake starts at once.
        QTRY_COMPARE(handshakes, 2);
        QTRY_COMPARE(reportB.count(), 1);
        QCOMPARE(reportB.outcomes.first(), true);
        QCOMPARE(reportB.identities.first(), QStringLiteral("b-identity"));

        // A's handshake finally returns (the scope guard above releases it, exactly once - a
        // second, explicit release here would double it). It gets no word: its generation is
        // stale by the time it arrives, superseded by B's own `pair()` call.
        cleanup.dismiss();
        releaseA.release();
        QTest::qWait(150);
        QCOMPARE(reportA.count(), 0);
    }

    void theDeadlineBelongsToTheCurrentHandshake()
    {
        ProductionPairingSeam seam;
        // 500 ms rather than the 150 ms this test once used: the machine-load assumption baked
        // into the old numbers - that `QTRY_COMPARE` + `qWait(100)` land inside A's window -
        // flaked on a loaded machine (A's deadline could fire before B even superseded it,
        // consuming A itself). The property under test is unchanged: B's deadline counts from
        // B's own `pair()` call, and without the restart B's deadline never fires at all, which
        // the `QTRY_VERIFY` below still catches.
        seam.setDeadlineMs(500);

        QSemaphore release;
        // Unconditional, for the same reason as `aSupersededHandshakeResultIsDropped` above: two
        // pool threads can be blocked here, and an early-failing assertion must not leave either
        // of them stuck forever.
        auto cleanup = qScopeGuard([&release]() { release.release(2); });

        int handshakes = 0;
        seam.setHandshake([&release, &handshakes](const PairingTarget&) {
            ++handshakes;
            release.acquire();
            return handshakeResult(true, QStringLiteral("late"));
        });

        PairingTarget targetA = pairingTarget();
        targetA.sessionId = QStringLiteral("session-a");
        PairingTarget targetB = pairingTarget();
        targetB.sessionId = QStringLiteral("session-b");

        SeamReport reportA;
        SeamReport reportB;
        seam.pair(targetA, recordInto(&reportA));
        QTRY_COMPARE(handshakes, 1);

        // A is still deep inside its own 500 ms window when B supersedes it.
        QTest::qWait(100);
        seam.pair(targetB, recordInto(&reportB));
        QTRY_COMPARE(handshakes, 2);

        // If the deadline still belonged to A, it could not have carried B: B's own deadline is
        // a fresh 500 ms counted from B's own `pair()` call, not from whatever remained of A's.
        // Nothing may have fired for B yet.
        QCOMPARE(reportB.count(), 0);

        // B's own full deadline does still apply.
        QTRY_VERIFY_WITH_TIMEOUT(reportB.count() == 1, 5000);
        QCOMPARE(reportB.outcomes.first(), false);

        // Neither blocked handshake gets a word after this: both are stale by the time they
        // return. The scope guard above does the releasing, exactly once each.
        cleanup.dismiss();
        release.release(2);
        QTest::qWait(100);
        QCOMPARE(reportA.count(), 0);
        QCOMPARE(reportB.count(), 1);
    }

    // -----------------------------------------------------------------------------------------
    // G-06.2-2 / Plan 06.2-12 Task 1: pairing conflict recovery.
    //
    // The rig holds pending pairing sessions keyed by the shared upstream uniqueid; a 409 on
    // getservercert means a half-open session holds it. The seam worker's recovery loop clears
    // the rig state and retries exactly once; a non-conflict failure clears without retrying; a
    // superseded run clears nothing (the newer run owns the rig) and is dropped.
    // -----------------------------------------------------------------------------------------

    void seamConflict_clearsOnceRetriesOnce_reportsTheRetry()
    {
        ProductionPairingSeam seam;
        QStringList order;
        int cancels = 0;
        int handshakes = 0;
        seam.setHandshake([&](const PairingTarget&) {
            ++handshakes;
            order.append(QStringLiteral("run%1").arg(handshakes));
            if (handshakes == 1) {
                PairingHandshakeResult conflict = handshakeResult(false, QString(),
                                                                  QStringLiteral("conflict"));
                conflict.pairingConflict = true;
                return conflict;
            }
            return handshakeResult(true, QStringLiteral("retry-identity"));
        });
        seam.setCancelRequest([&](const QString&, int) {
            ++cancels;
            order.append(QStringLiteral("cancel"));
            return true;
        });

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE(report.count(), 1);

        // Exactly two handshake runs, exactly one clear between them, and the clear completed
        // before the retry started.
        QCOMPARE(handshakes, 2);
        QCOMPARE(cancels, 1);
        QCOMPARE(order.join(QLatin1Char(',')), QStringLiteral("run1,cancel,run2"));

        // The retry's result is what the caller hears - reported as-is, once.
        QCOMPARE(report.outcomes.first(), true);
        QCOMPARE(report.identities.first(), QStringLiteral("retry-identity"));
        QVERIFY(report.diagnostics.first().isEmpty());
    }

    void seamConflictTwice_reportsTheSecondFailureOnce()
    {
        ProductionPairingSeam seam;
        int cancels = 0;
        int handshakes = 0;
        seam.setHandshake([&](const PairingTarget&) {
            ++handshakes;
            PairingHandshakeResult conflict = handshakeResult(false, QString(),
                                                              QStringLiteral("still-conflict-%1")
                                                                  .arg(handshakes));
            conflict.pairingConflict = true;
            return conflict;
        });
        seam.setCancelRequest([&](const QString&, int) {
            ++cancels;
            return true;
        });

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE(report.count(), 1);

        // A clean retry that still fails is a failure - the caller hears the SECOND run's own
        // text, once. No third run: exactly one retry, however the retry ends.
        QCOMPARE(handshakes, 2);
        QCOMPARE(cancels, 1);
        QCOMPARE(report.outcomes.first(), false);
        QVERIFY(report.identities.first().isEmpty());
        QCOMPARE(report.diagnostics.first(), QStringLiteral("still-conflict-2"));
    }

    void seamNonConflictFailure_oneClearNoRetry()
    {
        ProductionPairingSeam seam;
        int cancels = 0;
        int handshakes = 0;
        seam.setHandshake([&](const PairingTarget&) {
            ++handshakes;
            // No conflict flag: a plain failure. The failed run may still have left a pending
            // session on the rig, so it still gets one clear - but no retry.
            return handshakeResult(false, QString(), QStringLiteral("plain-failure"));
        });
        seam.setCancelRequest([&](const QString&, int) {
            ++cancels;
            return true;
        });

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE(report.count(), 1);

        QCOMPARE(handshakes, 1);
        QCOMPARE(cancels, 1);
        QCOMPARE(report.outcomes.first(), false);
        QCOMPARE(report.diagnostics.first(), QStringLiteral("plain-failure"));
    }

    void aWrongPinFailureCarriesStepAndClassButNeverTheDiagnostic()
    {
        // ADR-0072 items 1-2: a failed handshake reports WHICH step failed and WHY as closed
        // vocabulary tokens, while `engineError` keeps its local diagnostic for support only.
        // The fake handshake builds exactly what `pairing_handshake.cpp` builds for
        // `NvPairingManager::PIN_WRONG` (the mapper itself is asserted in
        // `tst_attempt_vocab::pairStateMapsToClosedClasses`, which this suite does not link);
        // what is under test here is that the classification survives the seam and reaches the
        // facade as separate members, and that it is never the diagnostic text.
        ProductionPairingSeam seam;
        seam.setHandshake([](const PairingTarget&) {
            PairingHandshakeResult rejected;
            rejected.engineError = QStringLiteral("the rig rejected the pairing PIN");
            rejected.attemptStep = QStringLiteral("pair_handshake");
            rejected.stepClass = QStringLiteral("pin_rejected");
            rejected.attempts = 1;
            return rejected;
        });
        seam.setCancelRequest([](const QString&, int) { return true; });

        QSignalSpy classified(&seam, &ProductionPairingSeam::failureClassified);
        QVERIFY(classified.isValid());

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE(report.count(), 1);

        // The failure itself: not ok, no identity, and the local diagnostic unchanged - it is
        // still there for support, and it is still the only text in the result.
        QCOMPARE(report.outcomes.first(), false);
        QVERIFY(report.identities.first().isEmpty());
        QCOMPARE(report.diagnostics.first(), QStringLiteral("the rig rejected the pairing PIN"));

        // The classification handed to the facade, as separate members.
        QTRY_COMPARE(classified.count(), 1);
        const PairingHandshakeResult delivered =
            classified.at(0).at(0).value<PairingHandshakeResult>();
        QCOMPARE(delivered.attemptStep, QStringLiteral("pair_handshake"));
        QCOMPARE(delivered.stepClass, QStringLiteral("pin_rejected"));
        QCOMPARE(delivered.attempts, 1);

        // The step is one of the 11 frozen tokens, read from the header block the vocabulary
        // script checks - a step outside it could never reach `/end` or a Sentry query.
        bool stepIsFrozen = false;
        for (int i = 0; i < kAttemptStepCount; ++i) {
            if (delivered.attemptStep == QLatin1String(kAttemptStepTokens[i])) {
                stepIsFrozen = true;
            }
        }
        QVERIFY2(stepIsFrozen, "the step must be one of the 11 frozen attempt-step tokens");

        // Never the diagnostic: the class is a token in its own right, and the local text stays
        // out of it (the redaction rule, ADR-0072 item 2).
        QVERIFY(!delivered.engineError.isEmpty());
        QVERIFY2(delivered.stepClass != delivered.engineError,
                 qPrintable(QStringLiteral("the class must not be the diagnostic: %1")
                                .arg(delivered.stepClass)));
        QVERIFY2(!delivered.stepClass.contains(delivered.engineError),
                 "the diagnostic must not leak into the class");

        // The same classification on the accessor the facade reads once `pairingFailed` lands.
        QCOMPARE(seam.lastResult().attemptStep, QStringLiteral("pair_handshake"));
        QCOMPARE(seam.lastResult().stepClass, QStringLiteral("pin_rejected"));
        QCOMPARE(seam.lastResult().attempts, 1);
        QCOMPARE(seam.lastResult().engineError, QStringLiteral("the rig rejected the pairing PIN"));
    }

    void aRecoveredConflictSuccessCarriesTheFrozenRecordOutOfTheSeam()
    {
        // ADR-0072 item 2 / `docs/spec/client.md` "Play diagnostics - Rules": a pairing that
        // succeeds only after the clear-and-retry reports ONE record - `step=pair_handshake`,
        // `failure_class=in_progress`, `attempt=2` - and it reaches the facade on the result the
        // seam reports. The first run below is exactly what production builds for the rig's HTTP
        // 409 on getservercert (step `pair_server_info`, class `http_4xx`): the recovered record
        // is FROZEN by the ADR, not inherited from that mapper.
        ProductionPairingSeam seam;
        int handshakes = 0;
        int cancels = 0;
        seam.setHandshake([&handshakes](const PairingTarget&) {
            ++handshakes;
            if (handshakes == 1) {
                PairingHandshakeResult conflict;
                conflict.attemptStep = QStringLiteral("pair_server_info");
                conflict.stepClass = QStringLiteral("http_4xx");
                conflict.pairingConflict = true;
                conflict.engineError = QStringLiteral(
                    "the rig did not answer the server-info request (HTTP 409)");
                return conflict;
            }
            PairingHandshakeResult paired;
            paired.ok = true;
            paired.clientIdentity = QStringLiteral("recovered-identity");
            return paired;
        });
        seam.setCancelRequest([&cancels](const QString&, int) {
            ++cancels;
            return true;
        });

        QSignalSpy classified(&seam, &ProductionPairingSeam::failureClassified);
        QVERIFY(classified.isValid());

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE(report.count(), 1);

        // One clear, one retry, and the retry's own identity reported - the G-06.2-2 contract.
        QCOMPARE(handshakes, 2);
        QCOMPARE(cancels, 1);
        QVERIFY(report.outcomes.first());
        QCOMPARE(report.identities.first(), QStringLiteral("recovered-identity"));
        QVERIFY(report.diagnostics.first().isEmpty());

        // ...and exactly ONE classification carrying the frozen recovered record, which is what
        // the facade's INFO emission is built from.
        QTRY_COMPARE(classified.count(), 1);
        const PairingHandshakeResult delivered =
            classified.at(0).at(0).value<PairingHandshakeResult>();
        QVERIFY2(delivered.ok, "the recovered result must be reported as a success");
        QCOMPARE(delivered.attemptStep, QStringLiteral("pair_handshake"));
        QCOMPARE(delivered.stepClass, QStringLiteral("in_progress"));
        QCOMPARE(delivered.attempts, 2);

        // No diagnostic text and no address rides the record (ADR-0072 item 2, T-06.7-35).
        QVERIFY2(!delivered.stepClass.isEmpty() && delivered.stepClass != delivered.engineError,
                 "the class must be a token, never the diagnostic");
        QVERIFY2(!delivered.engineError.contains(QStringLiteral("HTTP 409")),
                 "the local diagnostic stays local - it is not part of the record");
    }

    void aRetryThatFailsAgainReportsTheLastFailureClass()
    {
        // ADR-0072 item 2: "A retry that fails again is `outcome=failed`, `failure_class` of the
        // LAST failure, `attempt` 2" - the second run's own classification, not the cleared one's.
        ProductionPairingSeam seam;
        int handshakes = 0;
        int cancels = 0;
        seam.setHandshake([&handshakes](const PairingTarget&) {
            ++handshakes;
            if (handshakes == 1) {
                PairingHandshakeResult conflict;
                conflict.attemptStep = QStringLiteral("pair_server_info");
                conflict.stepClass = QStringLiteral("http_4xx");
                conflict.pairingConflict = true;
                conflict.engineError =
                    QStringLiteral("the rig did not answer the server-info request (HTTP 409)");
                return conflict;
            }
            PairingHandshakeResult rejected;
            rejected.attemptStep = QStringLiteral("pair_handshake");
            rejected.stepClass = QStringLiteral("pin_rejected");
            rejected.engineError = QStringLiteral("the rig rejected the pairing PIN");
            return rejected;
        });
        seam.setCancelRequest([&cancels](const QString&, int) {
            ++cancels;
            return true;
        });

        QSignalSpy classified(&seam, &ProductionPairingSeam::failureClassified);
        QVERIFY(classified.isValid());

        SeamReport report;
        seam.pair(pairingTarget(), recordInto(&report));
        QTRY_COMPARE(report.count(), 1);

        QCOMPARE(handshakes, 2);
        QCOMPARE(cancels, 1);
        QVERIFY(!report.outcomes.first());

        QTRY_COMPARE(classified.count(), 1);
        const PairingHandshakeResult delivered =
            classified.at(0).at(0).value<PairingHandshakeResult>();
        QVERIFY(!delivered.ok);
        QCOMPARE(delivered.attemptStep, QStringLiteral("pair_handshake"));
        QCOMPARE(delivered.stepClass, QStringLiteral("pin_rejected"));
        QCOMPARE(delivered.attempts, 2);
    }

    void theEndBodyCarriesTheAttemptStepAndNoOtherNewField()
    {
        // ADR-0072 item 1 / plan 09: the classified step is the ONE thing this plan sends to the
        // server - `attempt_step` on `POST /api/sessions/{id}/end` (contract 3.8.0) - and only
        // when the client classified one. `buildEndRequest` is the exact builder `endSession()`
        // posts, so what it produces here is what the wire carries.
        EndReport report;
        report.stage = QStringLiteral("pairing");
        report.attemptStep = QStringLiteral("pair_handshake");

        const QJsonObject object =
            QJsonDocument::fromJson(ControlPlaneClient::buildEndRequest(true, report)).object();
        QCOMPARE(object.value(QStringLiteral("attempt_step")).toString(),
                 QStringLiteral("pair_handshake"));
        QCOMPARE(object.value(QStringLiteral("failed")).toBool(), true);
        QCOMPARE(object.value(QStringLiteral("stage")).toString(), QStringLiteral("pairing"));

        // No OTHER field joined the body: the key set is exactly the report's own known fields
        // plus `attempt_step`. The failure class never travels (D-12) - it stays in Sentry.
        QStringList keys = object.keys();
        keys.sort();
        QStringList expected{ QStringLiteral("attempt_step"), QStringLiteral("failed"),
                              QStringLiteral("stage") };
        expected.sort();
        QCOMPARE(keys, expected);

        // An unclassified failure - the controller's own deadline, or a control-plane refusal -
        // leaves the field out entirely: an absent field is not the same as an empty or a guessed
        // one on this leniently-parsed body.
        EndReport unclassified;
        unclassified.stage = QStringLiteral("pairing");
        const QJsonObject without = QJsonDocument::fromJson(
            ControlPlaneClient::buildEndRequest(true, unclassified)).object();
        QVERIFY2(!without.contains(QStringLiteral("attempt_step")),
                 "a step the client never classified must not be invented on the wire");

        // And a customer-initiated end still posts no body at all (contract 3.3.0).
        QVERIFY(ControlPlaneClient::buildEndRequest(false, report).isEmpty());
    }

    void seamSupersededRun_noClearNoRetry_droppedResult()
    {
        ProductionPairingSeam seam;
        QSemaphore releaseA;
        auto cleanup = qScopeGuard([&releaseA]() { releaseA.release(); });
        int cancels = 0;
        int handshakes = 0;
        seam.setHandshake([&](const PairingTarget&) {
            ++handshakes;
            if (handshakes == 1) {
                // A's handshake: blocked until the test releases it, then returns a conflict
                // failure. By then B has superseded it, and B's run owns the rig state.
                releaseA.acquire();
                PairingHandshakeResult conflict = handshakeResult(false, QString(),
                                                                  QStringLiteral("stale"));
                conflict.pairingConflict = true;
                return conflict;
            }
            return handshakeResult(true, QStringLiteral("b-identity"));
        });
        seam.setCancelRequest([&](const QString&, int) {
            ++cancels;
            return true;
        });

        PairingTarget targetA = pairingTarget();
        targetA.sessionId = QStringLiteral("session-a");
        PairingTarget targetB = pairingTarget();
        targetB.sessionId = QStringLiteral("session-b");

        SeamReport reportA;
        SeamReport reportB;
        seam.pair(targetA, recordInto(&reportA));
        QTRY_COMPARE(handshakes, 1);
        seam.pair(targetB, recordInto(&reportB));
        QTRY_COMPARE(handshakes, 2);
        QTRY_COMPARE(reportB.count(), 1);
        QCOMPARE(reportB.outcomes.first(), true);

        // A's conflict failure finally lands - stale. It clears nothing (that would kill B's
        // own pending pairing), retries nothing, and is dropped entirely.
        cleanup.dismiss();
        releaseA.release();
        QTest::qWait(150);
        QCOMPARE(reportA.count(), 0);
        QCOMPARE(cancels, 0);
        QCOMPARE(handshakes, 2);
    }

    void singleFlight_aPollDuringAnInFlightHandshakeNeverStartsASecond()
    {
        // One Play attempt sends one getservercert sequence. The 2026-09-29 incident saw three
        // in one second - a poll tick or a push-driven pollNow() re-entering pairing while the
        // handshake was still running - two of them 409s against the rig's half-open session.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        seam->responseDelayMs = 200; // the handshake stays in flight across the polls below
        wire(controller, fake);
        controller.setSeam(seam);
        controller.setPollIntervalMs(1);

        for (int i = 0; i < 4; ++i) {
            fake->statuses.append(200);
            fake->bodies.append(authorizationBody(QString::fromLatin1(kPin),
                                                  QStringLiteral("READY")));
        }

        QSignalSpy completed(&controller, &PairingController::pairingCompleted);
        QSignalSpy failed(&controller, &PairingController::pairingFailed);

        controller.start(QString::fromLatin1(kSessionId));
        QTest::qWait(30);
        QCOMPARE(seam->calls, 1); // the handshake is in flight

        // Push frames wake the poll twice while it runs. Neither may start a second handshake
        // for the same session.
        controller.pollNow();
        controller.pollNow();
        QTest::qWait(30);
        QCOMPARE(seam->calls, 1);

        // The one in-flight handshake still resolves normally.
        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(failed.count(), 0);
    }

    void teardown_cancelRequestsTheRigSideClearOnceForTheLastTarget()
    {
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        seam->responseDelayMs = 200; // keep the handshake unresolved across the cancel
        wire(controller, fake);
        controller.setSeam(seam);

        fake->statuses = { 200 };
        fake->bodies = { authorizationBody(QString::fromLatin1(kPin),
                                           QStringLiteral("READY")) };

        controller.start(QString::fromLatin1(kSessionId));
        QTest::qWait(30);
        QCOMPARE(seam->calls, 1);

        controller.cancel();
        QCOMPARE(seam->clears, 1);
        QCOMPARE(seam->lastClearedTarget.hostAddress, QStringLiteral("203.0.113.7"));
        QCOMPARE(seam->lastClearedTarget.httpsPort, 47984);
        QCOMPARE(seam->lastClearedTarget.sessionId, QString::fromLatin1(kSessionId));

        // A second cancel - teardown paths call this more than once - must not repeat the clear.
        controller.cancel();
        QCOMPARE(seam->clears, 1);
    }

    void teardown_cancelBeforeAHandshakeStartedSendsNoClear()
    {
        // No handshake ran, so this client left no pending pairing session on the rig - and a
        // clear aimed at a session another client may be pairing would cancel THEIR handshake.
        PairingController controller;
        auto* fake = new FakeNetworkAccessManager;
        auto* seam = new RecordingSeam;
        wire(controller, fake);
        controller.setSeam(seam);

        fake->statuses = { 409 };
        fake->bodies = { conflictBody() };

        controller.start(QString::fromLatin1(kSessionId));
        controller.cancel();
        QCOMPARE(seam->clears, 0);
    }

    void cancelRequest_theWireCarriesTheUpstreamUniqueidAndCancelPhrase()
    {
        // The rig-side clear only clears when it names the session the rig actually holds: the
        // request line must be exactly what Sunshine's pair() handler reads
        // (`src/nvhttp.cpp:705-713` - a /pair request with uniqueid and a cancel phrase).
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));

        QString requestLine;
        connect(&server, &QTcpServer::newConnection, this, [&]() {
            QTcpSocket* socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [socket, &requestLine]() {
                if (requestLine.isEmpty()) {
                    requestLine = QString::fromLatin1(socket->readLine());
                }
                socket->write("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
                socket->disconnectFromHost();
            });
        });

        // Any HTTP answer counts: the 400 here is Sunshine's no-op-clear answer.
        QVERIFY(sendPairingCancelRequest(QStringLiteral("127.0.0.1"), server.serverPort()));

        QTRY_VERIFY(!requestLine.isEmpty());
        QCOMPARE(requestLine.trimmed(),
                 QStringLiteral("GET /pair?uniqueid=0123456789ABCDEF&phrase=cancel HTTP/1.1"));
    }

    void parity_theSharedUniqueIdStillMatchesUpstream()
    {
        // The recovery constant and the handshake's own pairing requests must name the same
        // uniqueid, or the clear would miss the rig's session forever. Upstream's hard-coded
        // value lives at app/backend/nvhttp.cpp:482; this test opens that file and asserts the
        // literal is still there.
        const QString upstream = QStringLiteral("%1/../app/backend/nvhttp.cpp")
                                     .arg(QCoreApplication::applicationDirPath());
        QFile file(upstream);
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(upstream));
        QVERIFY(file.readAll().contains(QByteArrayLiteral("0123456789ABCDEF")));
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
