/*****************************************************************************
 * SeatHub fork - unit tests for teardown (Plan 03-03 Task 2, STREAM-10).
 *
 * Two properties are worth more than a live server here, and both are assertable:
 *
 *   1. The order. Disable, then remove, then verify - never removal first, because removing the
 *      record does not stop a live stream (Pitfall 5). The controller encodes the order as a
 *      stage enum it advances through, so the property is enforced rather than commented.
 *   2. Teardown does not sign the customer out. STREAM-10 ends with "the client keeps no stored
 *      rig, address or pairing of its own", which holds because the client stores none of the
 *      three; the one thing the DPAPI store does hold is the sign-in credential, and CUST-08 (a
 *      customer who plays once is still signed in at the next launch) makes teardown leave it
 *      alone. The test stores a real token, runs teardown, and checks it is intact. (Until Phase 5
 *      plan 02 this case asserted the opposite - teardown deleted the credential - which signed
 *      every customer out after their first session.)
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
#include <QScopedPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

#include "seathub/teardown_controller.h"
#include "seathub/teardown_guard.h"
#include "seathub/token_store.h"

namespace {

const char* kSessionId = "aaaabbbb-cccc-dddd-eeee-ffff00001111";
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
    QStringList paths;
    int calls = 0;

protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override
    {
        const int index = qMax(0, qMin(calls, statuses.size() - 1));
        const int status = statuses.isEmpty() ? 200 : statuses.at(index);
        const QByteArray body = bodies.isEmpty() ? QByteArray() : bodies.at(index);
        paths.append(request.url().path());
        ++calls;
        return new FakeReply(status, body, this);
    }
};

QByteArray sessionBody(const QString& state)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), QLatin1String(kSessionId));
    object.insert(QStringLiteral("state"), state);
    object.insert(QStringLiteral("quality_profile"), QStringLiteral("1080p60"));
    object.insert(QStringLiteral("minutes_billed"), 7);
    object.insert(QStringLiteral("reconnect_count"), 0);
    object.insert(QStringLiteral("requested_at"), QStringLiteral("2026-09-19T00:00:00Z"));
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray refusedBody(const QString& error, const QString& reference)
{
    QJsonObject object;
    object.insert(QStringLiteral("status_code"), 200);
    object.insert(QStringLiteral("status"), false);
    object.insert(QStringLiteral("error"), error);
    object.insert(QStringLiteral("reference"), reference);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

} // namespace

class TstTeardown : public QObject
{
    Q_OBJECT

private:
    QScopedPointer<QTemporaryDir> m_dir;

private slots:
    void initTestCase()
    {
        qRegisterMetaType<SeatHubFailure>("SeatHubFailure");
        qRegisterMetaType<TeardownStage>("TeardownStage");
    }

    void init()
    {
        m_dir.reset(new QTemporaryDir);
        QVERIFY(m_dir->isValid());
    }

    void cleanup()
    {
        m_dir.reset();
    }

    // --- the order is the safety property --------------------------------------------------

    void stages_areOrderedDisableThenRemoveThenVerify()
    {
        // This is Pitfall 5 as an assertion. If a future edit reorders the enum, removal can be
        // reached before the disable step and this fails.
        QVERIFY(static_cast<int>(TeardownStage::Disable)
                < static_cast<int>(TeardownStage::Unpair));
        QVERIFY(static_cast<int>(TeardownStage::Unpair)
                < static_cast<int>(TeardownStage::Verify));
        QVERIFY(static_cast<int>(TeardownStage::Verify)
                < static_cast<int>(TeardownStage::Clear));
        QVERIFY(static_cast<int>(TeardownStage::Clear)
                < static_cast<int>(TeardownStage::Done));
    }

    void advanceIsStrictlyForward()
    {
        QVERIFY(TeardownController::isOrdered(TeardownStage::Idle, TeardownStage::Disable));
        QVERIFY(TeardownController::isOrdered(TeardownStage::Disable, TeardownStage::Unpair));
        QVERIFY(TeardownController::isOrdered(TeardownStage::Unpair, TeardownStage::Verify));

        // Backwards is refused - the whole point.
        QVERIFY(!TeardownController::isOrdered(TeardownStage::Unpair, TeardownStage::Disable));
        QVERIFY(!TeardownController::isOrdered(TeardownStage::Verify, TeardownStage::Unpair));
        QVERIFY(!TeardownController::isOrdered(TeardownStage::Done, TeardownStage::Disable));
        // And so is standing still.
        QVERIFY(!TeardownController::isOrdered(TeardownStage::Disable, TeardownStage::Disable));
    }

    void graceWindow_isTheFrozenThirtySeconds()
    {
        // `TEARDOWN_GRACE_SECONDS` (`docs/spec/timing.md`, D-13/D-16).
        QCOMPARE(TeardownController::kTeardownGraceMs, 30000);
    }

    // --- the happy path --------------------------------------------------------------------

    void teardown_endsTheSessionVerifiesItAndLeavesNothingBehind()
    {
        TeardownController controller;
        auto* client = new ControlPlaneClient(&controller);
        auto* fake = new FakeNetworkAccessManager;
        client->setNetworkAccessManager(fake);
        controller.setControlPlane(client);

        // The customer's stored sign-in credential. Teardown must leave it alone (CUST-08): a
        // customer who plays once is still signed in at the next launch.
        TokenStore store;
        store.setDirectory(m_dir->path());
        QVERIFY(store.storeToken(TokenStore::accessTokenName(),
                                 QStringLiteral("sb_at_SIGN-IN-MUST-SURVIVE")));
        const QString tokenPath = store.pathFor(TokenStore::accessTokenName());
        QVERIFY(QFile::exists(tokenPath));

        // POST /end, then the verification polls: still ENDING once, then terminal.
        fake->statuses = { 202, 200, 200 };
        fake->bodies = { sessionBody(QStringLiteral("ENDING")),
                         sessionBody(QStringLiteral("ENDING")),
                         sessionBody(QStringLiteral("COMPLETED")) };
        controller.setVerifyIntervalMs(1);

        QSignalSpy completed(&controller, &TeardownController::teardownCompleted);
        QSignalSpy failed(&controller, &TeardownController::teardownFailed);
        QSignalSpy stages(&controller, &TeardownController::stageEntered);

        controller.teardown(QString::fromLatin1(kSessionId), QString::fromLatin1(kClientUuid));
        QTRY_COMPARE(completed.count(), 1);
        QCOMPARE(failed.count(), 0);

        // Step 1 went out on the documented route.
        QVERIFY(!fake->paths.isEmpty());
        QCOMPARE(fake->paths.first(),
                 QString(QStringLiteral("/api/sessions/%1/end").arg(QLatin1String(kSessionId))));

        // The stages arrived in the required order and never went backwards.
        QList<int> seen;
        for (const QList<QVariant>& emission : stages) {
            seen.append(emission.at(0).toInt());
        }
        int previous = -1;
        for (int stage : seen) {
            QVERIFY2(stage > previous, "teardown stages must advance strictly forward");
            previous = stage;
        }
        QVERIFY(seen.contains(static_cast<int>(TeardownStage::Disable)));
        QVERIFY(seen.contains(static_cast<int>(TeardownStage::Unpair)));
        QVERIFY(seen.contains(static_cast<int>(TeardownStage::Verify)));
        QVERIFY(seen.contains(static_cast<int>(TeardownStage::Clear)));
        QVERIFY(seen.contains(static_cast<int>(TeardownStage::Done)));

        // On the disk: teardown removes no rig, address or pairing (this client stores none), and it
        // does not sign the customer out. The credential is exactly as it was.
        QVERIFY2(QFile::exists(tokenPath), "teardown must not remove the sign-in credential");
        QCOMPARE(store.retrieveToken(TokenStore::accessTokenName()),
                 QStringLiteral("sb_at_SIGN-IN-MUST-SURVIVE"));
        QCOMPARE(controller.stage(), TeardownStage::Done);
    }

    void teardownCompletion_carriesTheSessionsOwnEndReasonAndBilledMinutes()
    {
        // Home says why a session ended, and that comes from the terminal read teardown ends on - the
        // session itself, not a socket (CUST-15, ADR-0055).
        TeardownController controller;
        auto* client = new ControlPlaneClient(&controller);
        auto* fake = new FakeNetworkAccessManager;
        client->setNetworkAccessManager(fake);
        controller.setControlPlane(client);

        QJsonObject terminal = QJsonDocument::fromJson(sessionBody(QStringLiteral("COMPLETED"))).object();
        terminal.insert(QStringLiteral("end_reason"), QStringLiteral("BALANCE_EXHAUSTED"));
        terminal.insert(QStringLiteral("minutes_billed"), 42);

        fake->statuses = { 202, 200 };
        fake->bodies = { sessionBody(QStringLiteral("ENDING")),
                         QJsonDocument(terminal).toJson(QJsonDocument::Compact) };
        controller.setVerifyIntervalMs(1);

        QSignalSpy completed(&controller, &TeardownController::teardownCompleted);
        controller.teardown(QString::fromLatin1(kSessionId), QString::fromLatin1(kClientUuid));
        QTRY_COMPARE(completed.count(), 1);

        const SessionInfo finalSession = completed.at(0).at(0).value<SessionInfo>();
        QCOMPARE(finalSession.state, QStringLiteral("COMPLETED"));
        QCOMPARE(finalSession.endReason, QStringLiteral("BALANCE_EXHAUSTED"));
        QCOMPARE(finalSession.minutesBilled, 42);
    }

    void teardown_isIdempotentOnTheEndRequest()
    {
        // `POST /end` is documented idempotent, so a second End press is not an error.
        TeardownController controller;
        auto* client = new ControlPlaneClient(&controller);
        auto* fake = new FakeNetworkAccessManager;
        client->setNetworkAccessManager(fake);
        controller.setControlPlane(client);
        controller.setVerifyIntervalMs(1);

        fake->statuses = { 202, 200 };
        fake->bodies = { sessionBody(QStringLiteral("ENDING")),
                         sessionBody(QStringLiteral("CANCELLED")) };

        QSignalSpy completed(&controller, &TeardownController::teardownCompleted);
        controller.teardown(QString::fromLatin1(kSessionId), QString::fromLatin1(kClientUuid));
        QTRY_COMPARE(completed.count(), 1);
    }

    // --- failure modes ---------------------------------------------------------------------

    void sessionStuckEnding_failsWithTeardownTimeout()
    {
        TeardownController controller;
        auto* client = new ControlPlaneClient(&controller);
        auto* fake = new FakeNetworkAccessManager;
        client->setNetworkAccessManager(fake);
        controller.setControlPlane(client);
        controller.setVerifyIntervalMs(1);
        controller.setTeardownGraceMs(1);

        // The rig never finishes. `timing.md` makes this `TEARDOWN_TIMEOUT`, not a normal end.
        fake->statuses = { 202, 200 };
        fake->bodies = { sessionBody(QStringLiteral("ENDING")),
                         sessionBody(QStringLiteral("ENDING")) };

        QSignalSpy failed(&controller, &TeardownController::teardownFailed);
        QSignalSpy completed(&controller, &TeardownController::teardownCompleted);
        controller.teardown(QString::fromLatin1(kSessionId), QString::fromLatin1(kClientUuid));

        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(completed.count(), 0);

        const SeatHubFailure failure = failed.at(0).at(0).value<SeatHubFailure>();
        QCOMPARE(failure.failure, QStringLiteral("TEARDOWN_TIMEOUT"));
        // The deadline is the client's own clock, so there is no control-plane reference to show
        // (ADR-0008: the client may not generate one).
        QVERIFY2(failure.reference.isEmpty(),
                 "a local teardown deadline must not invent an ADR-0008 reference code");
        QVERIFY(!failure.error.isEmpty());
        QCOMPARE(controller.stage(), TeardownStage::Failed);
    }

    void controlPlaneRefusalOnHttp200_isAFailure()
    {
        TeardownController controller;
        auto* client = new ControlPlaneClient(&controller);
        auto* fake = new FakeNetworkAccessManager;
        client->setNetworkAccessManager(fake);
        controller.setControlPlane(client);

        // Pitfall 4 again, on the teardown path.
        fake->statuses = { 200 };
        fake->bodies = { refusedBody(QStringLiteral("That session has already ended."),
                                     QStringLiteral("SH-4F7KQ2")) };

        QSignalSpy failed(&controller, &TeardownController::teardownFailed);
        controller.teardown(QString::fromLatin1(kSessionId), QString::fromLatin1(kClientUuid));

        QTRY_COMPARE(failed.count(), 1);
        const SeatHubFailure failure = failed.at(0).at(0).value<SeatHubFailure>();
        QCOMPARE(failure.kind, FailureKind::Api);
        QCOMPARE(failure.reference, QStringLiteral("SH-4F7KQ2"));
    }

    void unreachableControlPlane_waitsRatherThanGuessing()
    {
        TeardownController controller;
        auto* client = new ControlPlaneClient(&controller);
        auto* fake = new FakeNetworkAccessManager;
        client->setNetworkAccessManager(fake);
        controller.setControlPlane(client);
        controller.setVerifyIntervalMs(1);
        controller.setTeardownGraceMs(10000);

        // Every verification poll fails at the transport. The controller cannot know whether the
        // rig-side removal happened, so it must not report either outcome.
        fake->statuses = { 202, 0 };
        fake->bodies = { sessionBody(QStringLiteral("ENDING")), QByteArray() };

        QSignalSpy failed(&controller, &TeardownController::teardownFailed);
        QSignalSpy completed(&controller, &TeardownController::teardownCompleted);
        controller.teardown(QString::fromLatin1(kSessionId), QString::fromLatin1(kClientUuid));

        QTest::qWait(40);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(completed.count(), 0);
        QVERIFY2(fake->calls > 2, "the controller must keep verifying, not give up after one try");
    }

    void emptySessionId_failsImmediately()
    {
        TeardownController controller;
        QSignalSpy failed(&controller, &TeardownController::teardownFailed);
        controller.teardown(QString(), QString::fromLatin1(kClientUuid));
        QCOMPARE(failed.count(), 1);
    }

    void cancel_stopsTheSequence()
    {
        TeardownController controller;
        auto* client = new ControlPlaneClient(&controller);
        auto* fake = new FakeNetworkAccessManager;
        client->setNetworkAccessManager(fake);
        controller.setControlPlane(client);
        controller.setVerifyIntervalMs(1);

        fake->statuses = { 202, 200 };
        fake->bodies = { sessionBody(QStringLiteral("ENDING")),
                         sessionBody(QStringLiteral("ENDING")) };

        controller.teardown(QString::fromLatin1(kSessionId), QString::fromLatin1(kClientUuid));
        controller.cancel();

        const int callsAtCancel = fake->calls;
        QTest::qWait(20);
        QCOMPARE(fake->calls, callsAtCancel);
        QCOMPARE(controller.stage(), TeardownStage::Idle);
    }

    // --- defect F-9: a second session must tear down too ------------------------------------

    void aSecondSessionTearsDownEvenThoughTheStageStaysDone()
    {
        // The verifier's second blocker, as an assertion. `TeardownController::stage()` reaches
        // `Done` when a teardown finishes and stays there - `cancel()` is the only other writer and
        // only `signOut()` calls it. So the expression `SeatHubClient::handleReadyForDeletion()`
        // guarded session teardown with - `stage() != TeardownStage::Done` - is false from the
        // second session onward, and the second session (and every one after it) skipped
        // `POST /api/sessions/{id}/end`, the rig-side disable/unpair, the local clear and
        // `teardownCompleted()` entirely. That is how STREAM-10 went unmet: the client kept its
        // token, and nothing on the rig was disabled or unpaired.
        //
        // The replacement is a per-session flag the facade resets in `beginSession()` and claims
        // through `markStarted()`, so this test drives exactly that.
        TeardownController controller;
        auto* client = new ControlPlaneClient(&controller);
        auto* fake = new FakeNetworkAccessManager;
        client->setNetworkAccessManager(fake);
        controller.setControlPlane(client);

        TokenStore store;
        store.setDirectory(m_dir->path());

        controller.setVerifyIntervalMs(1);

        const QString endPath =
            QString(QStringLiteral("/api/sessions/%1/end").arg(QLatin1String(kSessionId)));

        // --- session one, driven to its end
        QVERIFY(store.storeToken(TokenStore::accessTokenName(),
                                 QStringLiteral("sb_at_SESSION-ONE")));
        const QString tokenPath = store.pathFor(TokenStore::accessTokenName());
        QVERIFY(QFile::exists(tokenPath));

        fake->statuses = { 202, 200 };
        fake->bodies = { sessionBody(QStringLiteral("ENDING")),
                         sessionBody(QStringLiteral("COMPLETED")) };

        SessionTeardownGuard guard;
        guard.reset();
        QVERIFY(guard.markStarted());

        QSignalSpy firstCompleted(&controller, &TeardownController::teardownCompleted);
        controller.teardown(QString::fromLatin1(kSessionId), QString::fromLatin1(kClientUuid));
        QTRY_COMPARE(firstCompleted.count(), 1);

        QCOMPARE(fake->paths.count(endPath), 1);
        QVERIFY2(QFile::exists(tokenPath), "session one must not sign the customer out");

        // The sticky state the old guard read. Asserting it is the point: it is the fact that made
        // a stage-based guard wrong, and nothing between two sessions resets it.
        QCOMPARE(controller.stage(), TeardownStage::Done);

        // --- session two begins, exactly as `SeatHubClient::beginSession()` marks it
        guard.reset();
        QVERIFY2(guard.markStarted(),
                 "the second session must be allowed to tear down even though the stage is Done");

        // The credential is still there for session two - and it is the one a sign-in would have
        // left, not a stale one.
        QCOMPARE(store.retrieveToken(TokenStore::accessTokenName()),
                 QStringLiteral("sb_at_SESSION-ONE"));

        QSignalSpy secondCompleted(&controller, &TeardownController::teardownCompleted);
        controller.teardown(QString::fromLatin1(kSessionId), QString::fromLatin1(kClientUuid));
        QTRY_COMPARE(secondCompleted.count(), 1);

        // The authorisation is not decoration on a path that then refuses: the request went out,
        // and the second teardown ran to its end too.
        QCOMPARE(fake->paths.count(endPath), 2);
        QVERIFY2(QFile::exists(tokenPath),
                 "the second teardown must not sign the customer out either");

        // And the flag is claimed exactly once, so a duplicate cannot re-run a teardown.
        QVERIFY2(!guard.markStarted(), "a session's teardown is claimed exactly once");
    }
};

QTEST_MAIN(TstTeardown)

#include "tst_teardown.moc"
