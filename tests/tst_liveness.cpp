/*****************************************************************************
 * SeatHub fork - unit tests for liveness and the billing-safety horizon (Plan 03-03 Task 2,
 * D-31/D-33/D-34, ADR-0041).
 *
 * The most important assertion in this file is a negative one: a failed liveness report must
 * NOT end the session. The media path runs straight from the rig to this PC and never through
 * the control plane, so an undeliverable heartbeat says nothing about whether the stream works
 * - and ending a working stream because a *report* about it failed would turn a control-plane
 * outage into a customer-visible one. What actually stops a session locally is the token's
 * `authorized_through` horizon, which is the second half of this file.
 *
 * Nothing here waits 10 seconds. The interval and the grace window are injectable, and both
 * `tick()` and the horizon's `hardStop()` are callable, so the whole timer story is
 * deterministic.
 *****************************************************************************/

#include <QtTest>
#include <QBuffer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTimer>

#include "seathub/authorized_through_timer.h"
#include "seathub/liveness_timer.h"

namespace {

const char* kSessionId = "aaaabbbb-cccc-dddd-eeee-ffff00001111";

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
    int status = 200;
    QByteArray body;
    QStringList paths;
    QList<QByteArray> postedBodies;
    int calls = 0;

protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request,
                                 QIODevice* outgoingData) override
    {
        paths.append(request.url().path());
        // The liveness payload is the thing under test, so it is kept.
        postedBodies.append(outgoingData != nullptr ? outgoingData->readAll() : QByteArray());
        ++calls;
        return new FakeReply(status, body, this);
    }
};

QByteArray sessionBody()
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), QLatin1String(kSessionId));
    object.insert(QStringLiteral("state"), QStringLiteral("ACTIVE"));
    object.insert(QStringLiteral("quality_profile"), QStringLiteral("1080p60"));
    object.insert(QStringLiteral("minutes_billed"), 7);
    object.insert(QStringLiteral("reconnect_count"), 0);
    object.insert(QStringLiteral("requested_at"), QStringLiteral("2026-09-19T00:00:00Z"));
    object.insert(QStringLiteral("authorized_through"), QStringLiteral("2026-09-19T02:00:00Z"));
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QJsonObject lastPostedObject(const FakeNetworkAccessManager& fake)
{
    if (fake.postedBodies.isEmpty()) {
        return QJsonObject();
    }
    return QJsonDocument::fromJson(fake.postedBodies.last()).object();
}

} // namespace

class TstLiveness : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<SeatHubFailure>("SeatHubFailure");
    }

    // --- the locked values -----------------------------------------------------------------

    void constants_areTheFrozenValues()
    {
        // D-31: report every 10 s, 30 s of grace on the control-plane side.
        QCOMPARE(LivenessTimer::kIntervalMs, 10000);
        QCOMPARE(LivenessTimer::kGraceMs, 30000);
    }

    // --- reporting -------------------------------------------------------------------------

    void tick_postsToTheDocumentedRouteWithStateAndErrorCode()
    {
        LivenessTimer timer;
        auto* client = new ControlPlaneClient(&timer);
        auto* fake = new FakeNetworkAccessManager;
        fake->body = sessionBody();
        client->setNetworkAccessManager(fake);
        timer.setControlPlane(client);
        timer.setReportedState(QStringLiteral("streaming"));
        timer.setErrorCode(QStringLiteral("SH-4F7KQ2"));

        timer.start(QString::fromLatin1(kSessionId));

        QTRY_VERIFY(!fake->postedBodies.isEmpty());
        QCOMPARE(fake->paths.last(),
                 QString(QStringLiteral("/api/sessions/%1/liveness").arg(QLatin1String(kSessionId))));

        // ADR-0041 (1.6.0): the report carries `state` and `error_code`.
        const QJsonObject payload = lastPostedObject(*fake);
        QCOMPARE(payload.value(QStringLiteral("state")).toString(), QStringLiteral("streaming"));
        QCOMPARE(payload.value(QStringLiteral("error_code")).toString(),
                 QStringLiteral("SH-4F7KQ2"));
    }

    void malformedErrorCode_isDroppedRatherThanSent()
    {
        // The schema pattern is `^SH-[0-9A-HJ-KM-NP-TV-Z]{6}$`. Sending something else would be
        // a 422 at best and a wrong support key at worst.
        LivenessTimer timer;
        auto* client = new ControlPlaneClient(&timer);
        auto* fake = new FakeNetworkAccessManager;
        fake->body = sessionBody();
        client->setNetworkAccessManager(fake);
        timer.setControlPlane(client);
        timer.setReportedState(QStringLiteral("streaming"));
        timer.setErrorCode(QStringLiteral("sh-4f7kq2"));

        timer.start(QString::fromLatin1(kSessionId));

        QTRY_VERIFY(!fake->postedBodies.isEmpty());
        const QJsonObject payload = lastPostedObject(*fake);
        QVERIFY(!payload.contains(QStringLiteral("error_code")));
        QCOMPARE(payload.value(QStringLiteral("state")).toString(), QStringLiteral("streaming"));
    }

    void onlyTheThreeDocumentedStatesAreReportable()
    {
        LivenessTimer timer;
        auto* client = new ControlPlaneClient(&timer);
        auto* fake = new FakeNetworkAccessManager;
        fake->body = sessionBody();
        client->setNetworkAccessManager(fake);
        timer.setControlPlane(client);
        timer.setReportedState(QStringLiteral("ACTIVE")); // an engine state name, not a report

        timer.start(QString::fromLatin1(kSessionId));

        QTRY_VERIFY(!fake->postedBodies.isEmpty());
        // Falls back to something valid in the enum rather than sending the invalid string.
        QCOMPARE(timer.reportedState(), QStringLiteral("streaming"));
    }

    void stop_endsReporting()
    {
        LivenessTimer timer;
        auto* client = new ControlPlaneClient(&timer);
        auto* fake = new FakeNetworkAccessManager;
        fake->body = sessionBody();
        client->setNetworkAccessManager(fake);
        timer.setControlPlane(client);

        timer.start(QString::fromLatin1(kSessionId));
        QTRY_VERIFY(fake->calls > 0);
        timer.stop();

        QVERIFY(!timer.isRunning());
        const int callsAtStop = fake->calls;
        QTest::qWait(20);
        QCOMPARE(fake->calls, callsAtStop);
    }

    // --- D-33: a failed report is never a session end ---------------------------------------

    void failureBeforeTheGraceWindow_isNotAWarningYet()
    {
        LivenessTimer timer;
        auto* client = new ControlPlaneClient(&timer);
        auto* fake = new FakeNetworkAccessManager;
        fake->status = 0; // transport failure
        client->setNetworkAccessManager(fake);
        timer.setControlPlane(client);
        timer.setIntervalMs(10);
        timer.setGraceMs(30);

        QSignalSpy warnings(&timer, &LivenessTimer::livenessWarning);
        QSignalSpy failures(&timer, &LivenessTimer::livenessFailed);

        timer.start(QString::fromLatin1(kSessionId));
        QVERIFY(timer.isRunning());
        timer.tick();
        QTRY_COMPARE(failures.count(), 2);
        QCOMPARE(timer.consecutiveFailures(), 2);
        // Two failures at 10 ms each is 20 ms - inside the 30 ms window.
        QCOMPARE(warnings.count(), 0);
    }

    void graceWindowElapsed_raisesANonFatalWarningAndKeepsRetrying()
    {
        LivenessTimer timer;
        auto* client = new ControlPlaneClient(&timer);
        auto* fake = new FakeNetworkAccessManager;
        fake->status = 0;
        client->setNetworkAccessManager(fake);
        timer.setControlPlane(client);
        timer.setIntervalMs(10);
        timer.setGraceMs(30);

        QSignalSpy warnings(&timer, &LivenessTimer::livenessWarning);
        QSignalSpy failures(&timer, &LivenessTimer::livenessFailed);

        timer.start(QString::fromLatin1(kSessionId));
        QVERIFY(timer.isRunning());
        timer.tick(); // 1
        QTRY_COMPARE(failures.count(), 2);
        timer.tick(); // 2 - 20 ms
        timer.tick(); // 3 - 30 ms: the window has elapsed
        QTRY_COMPARE(warnings.count(), 1);

        // THE assertion for D-33: the timer did not stop, and no session-ending error exists to
        // be raised. The stream is untouched.
        QVERIFY2(timer.isRunning(), "a liveness failure must never stop the timer");
        QVERIFY(timer.warnedSinceLastSuccess());

        // It warns once, not once per tick.
        timer.tick();
        timer.tick();
        QTRY_VERIFY(failures.count() >= 5);
        QCOMPARE(warnings.count(), 1);
    }

    void aSuccessfulReport_clearsTheWarningState()
    {
        LivenessTimer timer;
        auto* client = new ControlPlaneClient(&timer);
        auto* fake = new FakeNetworkAccessManager;
        fake->status = 0;
        client->setNetworkAccessManager(fake);
        timer.setControlPlane(client);
        timer.setIntervalMs(10);
        timer.setGraceMs(30);

        QSignalSpy warnings(&timer, &LivenessTimer::livenessWarning);
        QSignalSpy reported(&timer, &LivenessTimer::livenessReported);

        timer.start(QString::fromLatin1(kSessionId));
        QVERIFY(timer.isRunning());
        timer.tick();
        QTRY_COMPARE(timer.consecutiveFailures(), 2);
        timer.tick();
        QTRY_COMPARE(warnings.count(), 1);

        // The control plane comes back.
        fake->status = 200;
        fake->body = sessionBody();
        timer.tick();
        QTRY_COMPARE(reported.count(), 1);
        QCOMPARE(timer.consecutiveFailures(), 0);
        QVERIFY(!timer.warnedSinceLastSuccess());

        // And the next outage warns again rather than being suppressed by the earlier one.
        fake->status = 0;
        timer.tick();
        QTRY_COMPARE(timer.consecutiveFailures(), 1);
        QTest::qWait(20);
        timer.tick();
        timer.tick();
        QTRY_VERIFY(warnings.count() >= 2);
    }

    // --- D-33: the horizon is the only locally enforced stop --------------------------------

    void msUntil_isPlainArithmetic()
    {
        const QDateTime now = QDateTime::fromString(QStringLiteral("2026-09-19T01:00:00Z"),
                                                    Qt::ISODate);
        QCOMPARE(AuthorizedThroughTimer::msUntil(
                     now, QDateTime::fromString(QStringLiteral("2026-09-19T01:00:30Z"),
                                                Qt::ISODate)),
                 30000LL);
        // A horizon already behind us is non-positive, not a wrapped-around positive. The exact
        // magnitude is deliberately not asserted: `remainingMs()` is a signed "time to horizon",
        // and `schedule()` treats anything <= 0 as "fire now" - which `anElapsedHorizon_...`
        // below proves end to end. What matters here is that it can never come back positive.
        QVERIFY(AuthorizedThroughTimer::msUntil(
                    now, QDateTime::fromString(QStringLiteral("2026-09-19T00:59:00Z"),
                                               Qt::ISODate))
                <= 0LL);
        QCOMPARE(AuthorizedThroughTimer::msUntil(now, QDateTime()), 0LL);
        QCOMPARE(AuthorizedThroughTimer::msUntil(QDateTime(), now), 0LL);
    }

    void nullHorizon_isNotAHorizon()
    {
        // `Session.authorized_through` is nullable. Null means the control plane has not named a
        // horizon, which is not a reason to invent one.
        AuthorizedThroughTimer horizon;
        QSignalSpy reached(&horizon, &AuthorizedThroughTimer::horizonReached);

        horizon.arm(QString());
        QVERIFY(!horizon.isArmed());
        horizon.arm(QStringLiteral("not a timestamp"));
        QVERIFY(!horizon.isArmed());

        QTest::qWait(10);
        QCOMPARE(reached.count(), 0);
    }

    void aFutureHorizon_armsTheTimer()
    {
        AuthorizedThroughTimer horizon;
        horizon.arm(QDateTime::currentDateTimeUtc().addSecs(600).toString(Qt::ISODate));

        QVERIFY(horizon.isArmed());
        QVERIFY(horizon.remainingMs() > 0);
        QVERIFY(!horizon.horizon().isEmpty());
    }

    void anElapsedHorizon_stopsLocallyAtOnce()
    {
        AuthorizedThroughTimer horizon;
        QSignalSpy reached(&horizon, &AuthorizedThroughTimer::horizonReached);

        horizon.arm(QDateTime::currentDateTimeUtc().addSecs(-5).toString(Qt::ISODate));
        QTRY_COMPARE(reached.count(), 1);
    }

    void aLaterHorizon_rearmsRatherThanEndingTheSession()
    {
        // The control plane extends the session by moving `authorized_through` forward. That is
        // the extension path, and it must not race the old horizon into a stop.
        AuthorizedThroughTimer horizon;
        horizon.arm(QDateTime::currentDateTimeUtc().addSecs(1).toString(Qt::ISODate));
        const qint64 first = horizon.remainingMs();

        horizon.extend(QDateTime::currentDateTimeUtc().addSecs(600).toString(Qt::ISODate));
        QVERIFY(horizon.isArmed());
        QVERIFY2(horizon.remainingMs() > first, "an extension must move the horizon forward");

        QSignalSpy reached(&horizon, &AuthorizedThroughTimer::horizonReached);
        QTest::qWait(1200);
        QCOMPARE(reached.count(), 0);
    }

    void hardStop_firesImmediatelyAndDisarms()
    {
        AuthorizedThroughTimer horizon;
        horizon.arm(QDateTime::currentDateTimeUtc().addSecs(600).toString(Qt::ISODate));
        QSignalSpy reached(&horizon, &AuthorizedThroughTimer::horizonReached);

        horizon.hardStop();
        QCOMPARE(reached.count(), 1);
        QVERIFY(!horizon.isArmed());
    }

    void disarm_isIdempotent()
    {
        AuthorizedThroughTimer horizon;
        horizon.disarm();
        QVERIFY(!horizon.isArmed());
        horizon.arm(QDateTime::currentDateTimeUtc().addSecs(600).toString(Qt::ISODate));
        horizon.disarm();
        QVERIFY(!horizon.isArmed());
        horizon.disarm();
        QVERIFY(!horizon.isArmed());
    }
};

QTEST_MAIN(TstLiveness)

#include "tst_liveness.moc"
