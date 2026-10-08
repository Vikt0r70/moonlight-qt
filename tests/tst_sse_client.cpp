/*****************************************************************************
 * SeatHub fork - unit tests for the SSE frame reader and connection (06.4 Plan 14).
 *
 * Task 1: `SseClient::feed()` is the test seam - these slots hand it raw bytes exactly as
 * `QNetworkReply::readyRead` would, built from the same cross-language fixtures pytest and cargo
 * use (`docs/spec/fixtures/events/`, synced byte-for-byte into `tests/fixtures/events/` - the
 * verify block's `cmp` proves it).
 *
 * Task 2: `start()`/`stop()` wire a real `QNetworkReply` - `StreamNetworkAccessManager` below
 * answers with a `StreamReply` that stays open (unlike an ordinary REST fake reply, which
 * completes on the next event-loop turn) so a test can `pushBytes()` frames and choose when the
 * reply itself finishes, exactly what the dead-stream and reconnect slots need to drive by hand.
 *****************************************************************************/

#include <QtTest>
#include <QBuffer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTimer>

#include <cstring>

#include "seathub/session_websocket.h"
#include "seathub/sse_client.h"

namespace {

// One fixture file's `{"event": ..., "data": ...}` body.
QJsonObject loadFixture(const QString& name)
{
    const QString path = QFINDTESTDATA("fixtures/events/" + name + QStringLiteral(".json"));
    if (path.isEmpty()) {
        return QJsonObject();
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QJsonObject();
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

// `event: <event>\ndata: <compact data>\n\n` - one SSE frame.
QByteArray sseFrame(const QString& event, const QJsonObject& data)
{
    QByteArray frame;
    frame += "event: ";
    frame += event.toUtf8();
    frame += "\n";
    frame += "data: ";
    frame += QJsonDocument(data).toJson(QJsonDocument::Compact);
    frame += "\n\n";
    return frame;
}

// Renders one `tests/fixtures/events/<name>.json` fixture as one SSE frame.
QByteArray sseFrameFromFixture(const QString& name)
{
    const QJsonObject fixture = loadFixture(name);
    if (fixture.isEmpty()) {
        return QByteArray();
    }
    return sseFrame(fixture.value(QStringLiteral("event")).toString(),
                     fixture.value(QStringLiteral("data")).toObject());
}

// A `stream.hello` frame naming `alive_ms` (the first `stream.alive` then comes later), or none (0).
QByteArray helloFrame(int aliveMs)
{
    QJsonObject data;
    data.insert(QStringLiteral("connection_id"), QStringLiteral("c-test"));
    data.insert(QStringLiteral("server_time"), QStringLiteral("2026-10-06T10:00:01.123Z"));
    data.insert(QStringLiteral("topics"), QJsonArray());
    data.insert(QStringLiteral("retry_ms"), 3000);
    if (aliveMs > 0) {
        data.insert(QStringLiteral("alive_ms"), aliveMs);
    }
    return sseFrame(QStringLiteral("stream.hello"), data);
}

QByteArray closingFrame(const QString& reason)
{
    return sseFrame(QStringLiteral("stream.closing"),
                    QJsonObject{ { QStringLiteral("reason"), reason } });
}

// --- Task 2: a QNetworkReply that stays open, unlike tst_control_plane.cpp's FakeReply (which
// --- always completes on the next event-loop turn - unsuitable for the "silence times out" and
// --- "the server sent stream.closing, then closed the connection" tests below). ----------------

class StreamReply : public QNetworkReply
{
    Q_OBJECT

public:
    explicit StreamReply(QObject* parent) : QNetworkReply(parent)
    {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/event-stream"));
        open(QIODevice::ReadOnly);
    }

    void setHttpStatus(int status) { setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status); }

    // Surfaces as readyRead(), exactly like a real streaming response's next chunk.
    void pushBytes(const QByteArray& bytes)
    {
        m_pending += bytes;
        emit readyRead();
    }

    // The server closed the connection - the real counterpart of every `finished()` this reply
    // ever emits (whether the server sent it, or `abort()` below forced it).
    void finishStream()
    {
        if (isFinished()) {
            return;
        }
        setFinished(true);
        emit finished();
    }

    void abort() override
    {
        m_aborted = true;
        finishStream();
    }

    bool wasAborted() const { return m_aborted; }

    qint64 readData(char* data, qint64 maxSize) override
    {
        const qint64 n = qMin<qint64>(maxSize, m_pending.size());
        if (n > 0) {
            std::memcpy(data, m_pending.constData(), static_cast<size_t>(n));
            m_pending.remove(0, static_cast<int>(n));
        }
        return n;
    }

    qint64 bytesAvailable() const override { return m_pending.size() + QNetworkReply::bytesAvailable(); }

private:
    QByteArray m_pending;
    bool m_aborted = false;
};

class StreamNetworkAccessManager : public QNetworkAccessManager
{
    Q_OBJECT

public:
    QList<StreamReply*> replies;

protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request,
                                 QIODevice* = nullptr) override
    {
        Q_UNUSED(request);
        auto* reply = new StreamReply(this);
        replies.append(reply);
        return reply;
    }
};

// --- Task 2: `fetchAccountState`'s own ordinary one-shot REST fake - the same shape
// --- tst_control_plane.cpp's FakeReply/FakeNetworkAccessManager use, kept local since those are
// --- private to that file. -----------------------------------------------------------------------

class RestFakeReply : public QNetworkReply
{
    Q_OBJECT

public:
    RestFakeReply(int httpStatus, const QByteArray& body, QObject* parent) : QNetworkReply(parent)
    {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, httpStatus);
        setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        m_buffer.setData(body);
        m_buffer.open(QIODevice::ReadOnly);
        open(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this]() {
            setFinished(true);
            emit finished();
        });
    }

    void abort() override {}
    qint64 readData(char* data, qint64 maxSize) override { return m_buffer.read(data, maxSize); }
    qint64 bytesAvailable() const override { return m_buffer.size() + QNetworkReply::bytesAvailable(); }

private:
    QBuffer m_buffer;
};

class RestFakeNetworkAccessManager : public QNetworkAccessManager
{
    Q_OBJECT

public:
    QByteArray body;
    QNetworkRequest lastRequest;

protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request,
                                 QIODevice* = nullptr) override
    {
        lastRequest = request;
        return new RestFakeReply(200, body, this);
    }
};

} // namespace

class TstSseClient : public QObject
{
    Q_OBJECT

private slots:

    void parsesEveryFixtureFrame()
    {
        {
            const QByteArray frame = sseFrameFromFixture(QStringLiteral("account-state"));
            QVERIFY2(!frame.isEmpty(), "account-state fixture not found");
            SseClient client;
            QSignalSpy spy(&client, &SseClient::accountState);
            client.feed(frame);
            QCOMPARE(spy.count(), 1);
        }
        {
            const QByteArray frame = sseFrameFromFixture(QStringLiteral("session-state"));
            QVERIFY2(!frame.isEmpty(), "session-state fixture not found");
            SseClient client;
            QSignalSpy spy(&client, &SseClient::sessionState);
            client.feed(frame);
            QCOMPARE(spy.count(), 1);
        }
        {
            const QByteArray frame = sseFrameFromFixture(QStringLiteral("stream-hello"));
            QVERIFY2(!frame.isEmpty(), "stream-hello fixture not found");
            SseClient client;
            QSignalSpy spy(&client, &SseClient::hello);
            client.feed(frame);
            QCOMPARE(spy.count(), 1);
        }
        {
            const QByteArray frame = sseFrameFromFixture(QStringLiteral("stream-alive"));
            QVERIFY2(!frame.isEmpty(), "stream-alive fixture not found");
            SseClient client;
            QSignalSpy spy(&client, &SseClient::alive);
            client.feed(frame);
            QCOMPARE(spy.count(), 1);
        }
        {
            const QByteArray frame = sseFrameFromFixture(QStringLiteral("stream-resync"));
            QVERIFY2(!frame.isEmpty(), "stream-resync fixture not found");
            SseClient client;
            QSignalSpy spy(&client, &SseClient::resync);
            client.feed(frame);
            QCOMPARE(spy.count(), 1);
        }
        {
            const QByteArray frame = sseFrameFromFixture(QStringLiteral("stream-closing"));
            QVERIFY2(!frame.isEmpty(), "stream-closing fixture not found");
            SseClient client;
            QSignalSpy spy(&client, &SseClient::closing);
            client.feed(frame);
            QCOMPARE(spy.count(), 1);
        }
        // `agent.wake`/`rig.state` are not SeatHub's - no signal exists to carry them, and none
        // of the six above fires for them either.
        {
            const QByteArray agentWake = sseFrameFromFixture(QStringLiteral("agent-wake"));
            const QByteArray rigState = sseFrameFromFixture(QStringLiteral("rig-state"));
            QVERIFY2(!agentWake.isEmpty(), "agent-wake fixture not found");
            QVERIFY2(!rigState.isEmpty(), "rig-state fixture not found");

            SseClient client;
            QSignalSpy hello(&client, &SseClient::hello);
            QSignalSpy alive(&client, &SseClient::alive);
            QSignalSpy resync(&client, &SseClient::resync);
            QSignalSpy closing(&client, &SseClient::closing);
            QSignalSpy accountState(&client, &SseClient::accountState);
            QSignalSpy sessionState(&client, &SseClient::sessionState);

            client.feed(agentWake);
            client.feed(rigState);

            QCOMPARE(hello.count(), 0);
            QCOMPARE(alive.count(), 0);
            QCOMPARE(resync.count(), 0);
            QCOMPARE(closing.count(), 0);
            QCOMPARE(accountState.count(), 0);
            QCOMPARE(sessionState.count(), 0);
        }
    }

    void splitsFramesAtAnyByte()
    {
        const QByteArray frame = sseFrameFromFixture(QStringLiteral("account-state"));
        QVERIFY2(!frame.isEmpty(), "account-state fixture not found");

        SseClient client;
        QSignalSpy spy(&client, &SseClient::accountState);
        for (int i = 0; i < frame.size(); ++i) {
            client.feed(frame.mid(i, 1));
        }
        QCOMPARE(spy.count(), 1);
    }

    void ignoresCommentsAndUnknownEvents()
    {
        SseClient client;
        QSignalSpy hello(&client, &SseClient::hello);
        QSignalSpy alive(&client, &SseClient::alive);
        QSignalSpy resync(&client, &SseClient::resync);
        QSignalSpy closing(&client, &SseClient::closing);
        QSignalSpy accountState(&client, &SseClient::accountState);
        QSignalSpy sessionState(&client, &SseClient::sessionState);

        // The transport keep-alive: a comment line, never an event of its own.
        client.feed(QByteArrayLiteral(": ping\n\n"));
        client.feed(QByteArrayLiteral(": ping\n"));

        // An event type a future server sends that this build predates - additive, not an error.
        QJsonObject data;
        data.insert(QStringLiteral("some"), QStringLiteral("field"));
        QByteArray frame;
        frame += "event: something.new\n";
        frame += "data: ";
        frame += QJsonDocument(data).toJson(QJsonDocument::Compact);
        frame += "\n\n";
        client.feed(frame);

        QCOMPARE(hello.count(), 0);
        QCOMPARE(alive.count(), 0);
        QCOMPARE(resync.count(), 0);
        QCOMPARE(closing.count(), 0);
        QCOMPARE(accountState.count(), 0);
        QCOMPARE(sessionState.count(), 0);
    }

    void readsAliveMsFromHello()
    {
        {
            QJsonObject data;
            data.insert(QStringLiteral("connection_id"), QStringLiteral("c-test"));
            data.insert(QStringLiteral("server_time"), QStringLiteral("2026-09-27T10:00:01.123Z"));
            data.insert(QStringLiteral("topics"), QJsonArray());
            data.insert(QStringLiteral("retry_ms"), 3000);
            data.insert(QStringLiteral("alive_ms"), 20000);
            QByteArray frame;
            frame += "event: stream.hello\n";
            frame += "data: ";
            frame += QJsonDocument(data).toJson(QJsonDocument::Compact);
            frame += "\n\n";

            SseClient client;
            QSignalSpy spy(&client, &SseClient::hello);
            client.feed(frame);
            QCOMPARE(spy.count(), 1);
            QCOMPARE(spy.at(0).at(0).toInt(), 20000);
        }
        {
            // The fixture hello names no alive_ms - L3's dead-stream timeout stays unenforced
            // until a hello that does.
            const QByteArray frame = sseFrameFromFixture(QStringLiteral("stream-hello"));
            QVERIFY2(!frame.isEmpty(), "stream-hello fixture not found");

            SseClient client;
            QSignalSpy spy(&client, &SseClient::hello);
            client.feed(frame);
            QCOMPARE(spy.count(), 1);
            QCOMPARE(spy.at(0).at(0).toInt(), 0);
        }
    }

    void accountStateCarriesWalletLiveSessionAndNotice()
    {
        const QByteArray frame = sseFrameFromFixture(QStringLiteral("account-state"));
        QVERIFY2(!frame.isEmpty(), "account-state fixture not found");

        SseClient client;
        QSignalSpy spy(&client, &SseClient::accountState);
        client.feed(frame);
        QCOMPARE(spy.count(), 1);

        const AccountStateInfo info = spy.at(0).at(0).value<AccountStateInfo>();
        QCOMPARE(info.balanceMinutes, qint64(294));
        QCOMPARE(info.liveSessionId, QStringLiteral("11111111-1111-4111-8111-111111111111"));
        // The fixture's open_topup_notice is null.
        QCOMPARE(info.openTopupNoticeId, QString());
        QCOMPARE(info.signupStage, QStringLiteral("complete"));

        // A malformed/empty account object never crashes and reads empty/zero throughout.
        AccountStateInfo malformed;
        QVERIFY(AccountStateInfo::parse(QJsonObject(), &malformed));
        QCOMPARE(malformed.balanceMinutes, qint64(0));
        QCOMPARE(malformed.liveSessionId, QString());
        QCOMPARE(malformed.openTopupNoticeId, QString());
        QCOMPARE(malformed.signupStage, QString());
    }

    void accountStateCarriesANegativeBalanceUnclamped()
    {
        // ADR-0075: a valid delayed debit can leave the balance below zero; the pushed frame says
        // so and the client shows it, never `0`.
        SseClient client;
        QSignalSpy spy(&client, &SseClient::accountState);
        client.feed(QByteArrayLiteral(
            "event: account.state\n"
            "data: {\"topic\":\"account:acc-1\",\"at\":\"2026-09-27T00:00:00Z\","
            "\"account\":{\"wallet\":{\"balance_minutes\":-3},\"live_session_id\":\"\","
            "\"open_topup_notice\":null,\"signup_stage\":\"complete\"}}\n"
            "\n"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).value<AccountStateInfo>().balanceMinutes, qint64(-3));
    }

    // --- Task 2: the connection --------------------------------------------------------------

    void liveOnlyAfterAliveWhenHelloNamesIt()
    {
        ControlPlaneClient controlPlane;
        StreamNetworkAccessManager net;
        controlPlane.setStreamNetworkAccessManager(&net);
        controlPlane.setBaseUrl(QStringLiteral("https://example.test"));

        SseClient client;
        client.setControlPlane(&controlPlane);
        client.start();
        QCOMPARE(client.state(), QStringLiteral("connecting"));
        QCOMPARE(net.replies.count(), 1);

        QJsonObject helloData;
        helloData.insert(QStringLiteral("connection_id"), QStringLiteral("c-test"));
        helloData.insert(QStringLiteral("server_time"), QStringLiteral("2026-09-27T10:00:01.123Z"));
        helloData.insert(QStringLiteral("topics"), QJsonArray());
        helloData.insert(QStringLiteral("retry_ms"), 3000);
        helloData.insert(QStringLiteral("alive_ms"), 20000);
        net.replies.first()->pushBytes(sseFrame(QStringLiteral("stream.hello"), helloData));
        QCOMPARE(client.state(), QStringLiteral("connecting"));

        net.replies.first()->pushBytes(
            sseFrame(QStringLiteral("stream.alive"),
                     QJsonObject{ { QStringLiteral("at"), QStringLiteral("2026-09-27T10:00:20.000Z") } }));
        QCOMPARE(client.state(), QStringLiteral("live"));
    }

    void liveAtHelloWithoutAliveMs()
    {
        ControlPlaneClient controlPlane;
        StreamNetworkAccessManager net;
        controlPlane.setStreamNetworkAccessManager(&net);
        controlPlane.setBaseUrl(QStringLiteral("https://example.test"));

        SseClient client;
        client.setControlPlane(&controlPlane);
        client.start();
        QCOMPARE(client.state(), QStringLiteral("connecting"));

        net.replies.first()->pushBytes(sseFrameFromFixture(QStringLiteral("stream-hello")));
        QCOMPARE(client.state(), QStringLiteral("live"));
    }

    void pausedAfterTheFallbackDelay()
    {
        ControlPlaneClient controlPlane;
        StreamNetworkAccessManager net;
        controlPlane.setStreamNetworkAccessManager(&net);
        controlPlane.setBaseUrl(QStringLiteral("https://example.test"));

        SseClient client;
        client.setControlPlane(&controlPlane);
        client.setPausedDelayMs(50);
        QSignalSpy pausedSpy(&client, &SseClient::pausedChanged);
        client.start();

        QJsonObject helloData;
        helloData.insert(QStringLiteral("connection_id"), QStringLiteral("c-test"));
        helloData.insert(QStringLiteral("server_time"), QStringLiteral("2026-09-27T10:00:01.123Z"));
        helloData.insert(QStringLiteral("topics"), QJsonArray());
        helloData.insert(QStringLiteral("retry_ms"), 3000);
        helloData.insert(QStringLiteral("alive_ms"), 20000);
        net.replies.first()->pushBytes(sseFrame(QStringLiteral("stream.hello"), helloData));

        QTest::qWait(150);
        QCOMPARE(client.state(), QStringLiteral("paused"));
        QCOMPARE(pausedSpy.count(), 1);
        QCOMPARE(pausedSpy.at(0).at(0).toBool(), true);

        net.replies.first()->pushBytes(
            sseFrame(QStringLiteral("stream.alive"),
                     QJsonObject{ { QStringLiteral("at"), QStringLiteral("2026-09-27T10:00:20.000Z") } }));
        QCOMPARE(client.state(), QStringLiteral("live"));
        QCOMPARE(pausedSpy.count(), 2);
        QCOMPARE(pausedSpy.at(1).at(0).toBool(), false);
    }

    void deadStreamTimeoutReconnects()
    {
        ControlPlaneClient controlPlane;
        StreamNetworkAccessManager net;
        controlPlane.setStreamNetworkAccessManager(&net);
        controlPlane.setBaseUrl(QStringLiteral("https://example.test"));

        bool firstWasAborted = false;
        QObject abortObserver;
        SseClient client;
        client.setControlPlane(&controlPlane);
        client.setDeadStreamTimeoutMs(50);
        client.setJitterProvider([]() { return 0.0; });
        client.start();

        auto* firstReply = net.replies.first();
        // handleFinished() disconnects the client and defers reply deletion. Observe the
        // abort synchronously with an independent receiver, never through a dangling reply.
        QVERIFY(QObject::connect(firstReply, &QNetworkReply::finished, &abortObserver,
                                 [&firstWasAborted, firstReply]() {
                                     firstWasAborted = firstReply->wasAborted();
                                 }, Qt::DirectConnection));
        QJsonObject helloData;
        helloData.insert(QStringLiteral("connection_id"), QStringLiteral("c-test"));
        helloData.insert(QStringLiteral("server_time"), QStringLiteral("2026-09-27T10:00:01.123Z"));
        helloData.insert(QStringLiteral("topics"), QJsonArray());
        helloData.insert(QStringLiteral("retry_ms"), 3000);
        helloData.insert(QStringLiteral("alive_ms"), 20000);
        net.replies.first()->pushBytes(sseFrame(QStringLiteral("stream.hello"), helloData));

        QVERIFY(!firstWasAborted);
        QTRY_VERIFY(firstWasAborted);
        QTRY_COMPARE(net.replies.count(), 2);
    }

    void reconnectDelayUsesTheScheduleWithJitter()
    {
        for (int attempt = 0; attempt <= 6; ++attempt) {
            const int base = SessionWebSocket::reconnectDelayMs(attempt);
            QCOMPARE(SseClient::reconnectDelayMs(attempt, 0.0), base);
            QCOMPARE(SseClient::reconnectDelayMs(attempt, 0.2), static_cast<int>(base * 1.2));
        }
        // A jitter above 0.2 is clamped to 0.2, never applied unclamped.
        const int base = SessionWebSocket::reconnectDelayMs(2);
        QCOMPARE(SseClient::reconnectDelayMs(2, 0.5), static_cast<int>(base * 1.2));
        QCOMPARE(SseClient::reconnectDelayMs(2, 1.0), static_cast<int>(base * 1.2));
    }

    void revokedStopsForGood()
    {
        ControlPlaneClient controlPlane;
        StreamNetworkAccessManager net;
        controlPlane.setStreamNetworkAccessManager(&net);
        controlPlane.setBaseUrl(QStringLiteral("https://example.test"));

        SseClient client;
        client.setControlPlane(&controlPlane);
        QSignalSpy revokedSpy(&client, &SseClient::revoked);
        client.start();
        QCOMPARE(net.replies.count(), 1);

        net.replies.first()->pushBytes(sseFrame(
            QStringLiteral("stream.closing"),
            QJsonObject{ { QStringLiteral("reason"), QStringLiteral("revoked") } }));
        net.replies.first()->finishStream();

        QCOMPARE(client.state(), QStringLiteral("stopped"));
        QCOMPARE(revokedSpy.count(), 1);
        QCOMPARE(net.replies.count(), 1);

        // A 401 answer to the stream request itself - no closing frame at all - does the same.
        SseClient client2;
        client2.setControlPlane(&controlPlane);
        QSignalSpy revokedSpy2(&client2, &SseClient::revoked);
        client2.start();
        QCOMPARE(net.replies.count(), 2);
        net.replies.last()->setHttpStatus(401);
        net.replies.last()->finishStream();

        QCOMPARE(client2.state(), QStringLiteral("stopped"));
        QCOMPARE(revokedSpy2.count(), 1);
        QCOMPARE(net.replies.count(), 2);
    }

    void replacedWaitsForTheCap()
    {
        ControlPlaneClient controlPlane;
        StreamNetworkAccessManager net;
        controlPlane.setStreamNetworkAccessManager(&net);
        controlPlane.setBaseUrl(QStringLiteral("https://example.test"));

        SseClient client;
        client.setControlPlane(&controlPlane);
        client.setReconnectCapMs(80);
        client.setJitterProvider([]() { return 0.0; });
        client.start();
        QCOMPARE(net.replies.count(), 1);

        net.replies.first()->pushBytes(sseFrame(
            QStringLiteral("stream.closing"),
            QJsonObject{ { QStringLiteral("reason"), QStringLiteral("replaced") } }));
        net.replies.first()->finishStream();

        // Not at the schedule's 1 s floor - the cap, deliberately slower, so as not to fight the
        // connection that just replaced this one.
        QTest::qWait(40);
        QCOMPARE(net.replies.count(), 1);
        QTest::qWait(150);
        QCOMPARE(net.replies.count(), 2);
    }

    // F3 repair (wave 21 cycle 2): a network-back wake resets the ordinary attempt count before it
    // arms the immediate attempt, so an attempt started by the wake that then fails must wait the
    // 1 s floor again - even when the ordinary schedule had already reached its cap. Without the
    // reset the woken attempt's failure re-arms the old capped wait.
    void aWakeAtTheCapResetsTheNextOrdinaryWaitToTheFloor()
    {
        ControlPlaneClient controlPlane;
        StreamNetworkAccessManager net;
        controlPlane.setStreamNetworkAccessManager(&net);
        controlPlane.setBaseUrl(QStringLiteral("https://example.test"));

        SseClient client;
        client.setControlPlane(&controlPlane);
        client.setJitterProvider([]() { return 0.0; });
        client.start();
        QCOMPARE(net.replies.count(), 1);

        // Ordinary failures walk the schedule: the 1 s floor, then doubling. Each armed wait
        // elapsing opens the next attempt, which the test then fails again.
        const int waitsMs[] = { 1000, 2000, 4000, 8000, 16000 };
        for (int i = 0; i < 5; ++i) {
            net.replies.last()->finishStream();
            QTRY_COMPARE_WITH_TIMEOUT(client.nextRetrySeconds(), waitsMs[i] / 1000, 5000);
            QTRY_COMPARE_WITH_TIMEOUT(net.replies.count(), i + 2, waitsMs[i] + 15000);
        }

        // The next failure reaches the cap: the deliberate 30 s wait.
        net.replies.last()->finishStream();
        QTRY_COMPARE_WITH_TIMEOUT(client.nextRetrySeconds(), 30, 5000);

        // The network-back wake starts an attempt at once and resets the ordinary count.
        client.wakeNow();
        QVERIFY(client.nextRetrySeconds() <= 1);
        QTRY_COMPARE_WITH_TIMEOUT(net.replies.count(), 7, 5000);

        // The woken attempt fails: the next ordinary wait is the 1 s floor again, not the cap.
        net.replies.last()->finishStream();
        QTRY_COMPARE_WITH_TIMEOUT(client.nextRetrySeconds(), 1, 5000);
    }

    // --- R7 (ADR-0067 amendment 2026-10-06): the live-updates indicator shows only on a real loss ------

    void aRoutineReconnectWithAPromptHelloNeverShowsTheIndicator()
    {
        ControlPlaneClient controlPlane;
        StreamNetworkAccessManager net;
        controlPlane.setStreamNetworkAccessManager(&net);
        controlPlane.setBaseUrl(QStringLiteral("https://example.test"));

        SseClient client;
        client.setControlPlane(&controlPlane);
        client.setPausedDelayMs(500);       // stands for L5's 10 s
        client.setReconnectCapMs(100);
        client.setJitterProvider([]() { return 0.0; });
        QSignalSpy lostSpy(&client, &SseClient::liveUpdatesLostChanged);
        QSignalSpy pausedSpy(&client, &SseClient::pausedChanged);
        client.start();

        // The first connection gets its hello, then the server closes it (a routine reconnect).
        net.replies.first()->pushBytes(helloFrame(20000));
        net.replies.first()->pushBytes(closingFrame(QStringLiteral("replaced")));
        net.replies.first()->finishStream();
        QTRY_COMPARE_WITH_TIMEOUT(net.replies.count(), 2, 2000);

        // The next connection gets its hello promptly, but its first `stream.alive` is held back for
        // longer than the whole window: the indicator must never show.
        net.replies.last()->pushBytes(helloFrame(20000));
        QTest::qWait(800);
        QCOMPARE(lostSpy.count(), 0);

        // The L5 fallback read keeps its own trigger: the connection was not live for the delay.
        QCOMPARE(pausedSpy.count(), 1);
        QCOMPARE(pausedSpy.at(0).at(0).toBool(), true);

        // The late alive then clears the fallback state, and the indicator was still never shown.
        net.replies.last()->pushBytes(sseFrame(
            QStringLiteral("stream.alive"),
            QJsonObject{ { QStringLiteral("at"), QStringLiteral("2026-10-06T10:00:20.000Z") } }));
        QCOMPARE(client.state(), QStringLiteral("live"));
        QCOMPARE(pausedSpy.count(), 2);
        QCOMPARE(lostSpy.count(), 0);
    }

    void aDeadStreamReconnectWithAPromptHelloNeverShowsTheIndicator()
    {
        ControlPlaneClient controlPlane;
        StreamNetworkAccessManager net;
        controlPlane.setStreamNetworkAccessManager(&net);
        controlPlane.setBaseUrl(QStringLiteral("https://example.test"));

        SseClient client;
        client.setControlPlane(&controlPlane);
        client.setPausedDelayMs(1600);      // stands for L5's 10 s; the ordinary schedule's floor is 1 s
        client.setDeadStreamTimeoutMs(50);
        client.setJitterProvider([]() { return 0.0; });
        QSignalSpy lostSpy(&client, &SseClient::liveUpdatesLostChanged);
        client.start();

        net.replies.first()->pushBytes(helloFrame(20000));
        // Silence past the bound aborts the connection; the ordinary 1 s schedule reconnects it.
        QTRY_COMPARE_WITH_TIMEOUT(net.replies.count(), 2, 3000);
        net.replies.last()->pushBytes(helloFrame(0));

        // Past the window counted from the close: still never shown.
        QTest::qWait(1900);
        QCOMPARE(lostSpy.count(), 0);
    }

    void tenSecondsWithoutAnyHelloAfterALossShowsTheIndicator()
    {
        ControlPlaneClient controlPlane;
        StreamNetworkAccessManager net;
        controlPlane.setStreamNetworkAccessManager(&net);
        controlPlane.setBaseUrl(QStringLiteral("https://example.test"));

        SseClient client;
        client.setControlPlane(&controlPlane);
        client.setPausedDelayMs(200);       // stands for L5's 10 s
        client.setReconnectCapMs(600);
        client.setJitterProvider([]() { return 0.0; });
        QSignalSpy lostSpy(&client, &SseClient::liveUpdatesLostChanged);
        QSignalSpy pausedSpy(&client, &SseClient::pausedChanged);
        client.start();

        // From start-up: no hello within the window is a loss, and a hello hides it at once.
        QTRY_COMPARE_WITH_TIMEOUT(lostSpy.count(), 1, 2000);
        QCOMPARE(lostSpy.at(0).at(0).toBool(), true);
        net.replies.first()->pushBytes(helloFrame(0));
        QCOMPARE(lostSpy.count(), 2);
        QCOMPARE(lostSpy.at(1).at(0).toBool(), false);

        // A connection that had its hello closes; nothing is shown inside the window ...
        net.replies.first()->pushBytes(closingFrame(QStringLiteral("replaced")));
        net.replies.first()->finishStream();
        QTest::qWait(100);
        QCOMPARE(lostSpy.count(), 2);

        // ... and with no hello for the whole window after that close, the indicator shows.
        QTRY_COMPARE_WITH_TIMEOUT(lostSpy.count(), 3, 2000);
        QCOMPARE(lostSpy.at(2).at(0).toBool(), true);

        // The reconnect that follows without a hello changes nothing, and the L5 fallback trigger still
        // fires for that connection on its own clock.
        QTRY_COMPARE_WITH_TIMEOUT(net.replies.count(), 2, 2000);
        const int pausedBefore = pausedSpy.count();
        QTRY_VERIFY_WITH_TIMEOUT(pausedSpy.count() > pausedBefore, 2000);
        QCOMPARE(pausedSpy.last().at(0).toBool(), true);
        QCOMPARE(lostSpy.count(), 3);

        // The next hello hides it at once.
        net.replies.last()->pushBytes(helloFrame(20000));
        QCOMPARE(lostSpy.count(), 4);
        QCOMPARE(lostSpy.at(3).at(0).toBool(), false);

        // A deliberate stop never leaves it showing.
        client.stop();
        QCOMPARE(lostSpy.count(), 4);
    }

    void theFallbackReadCarriesTheMarker()
    {
        ControlPlaneClient controlPlane;
        RestFakeNetworkAccessManager net;
        controlPlane.setNetworkAccessManager(&net);
        controlPlane.setBaseUrl(QStringLiteral("https://example.test"));
        controlPlane.setAccessToken(QStringLiteral("test-token"));

        const QJsonObject accountStateBody = loadFixture(QStringLiteral("account-state"))
                                                  .value(QStringLiteral("data"))
                                                  .toObject();
        net.body = QJsonDocument(accountStateBody).toJson(QJsonDocument::Compact);

        bool fallbackOk = false;
        AccountStateInfo fallbackInfo;
        controlPlane.fetchAccountState(true, [&](const ControlPlaneResult& result) {
            fallbackOk = result.ok;
            AccountStateInfo::parse(result.body.value(QStringLiteral("account")).toObject(),
                                     &fallbackInfo);
        });
        QTest::qWait(50);
        QVERIFY(fallbackOk);
        QCOMPARE(net.lastRequest.rawHeader("X-Stream-Fallback"), QByteArray("seathub"));
        QCOMPARE(net.lastRequest.rawHeader("Authorization"), QByteArray("Bearer test-token"));
        QCOMPARE(fallbackInfo.balanceMinutes, qint64(294));

        bool plainOk = false;
        controlPlane.fetchAccountState(false, [&](const ControlPlaneResult& result) {
            plainOk = result.ok;
        });
        QTest::qWait(50);
        QVERIFY(plainOk);
        QVERIFY(!net.lastRequest.hasRawHeader("X-Stream-Fallback"));
    }

    // --- V-L4 (ADR-0083 decision 4): a push-stream connection with no `stream.hello` within the
    // --- hello time limit is closed and retried. `stream.alive` is deliberately not a substitute. --

    void aHelloLessConnectionIsClosedAndRetried()
    {
        ControlPlaneClient controlPlane;
        StreamNetworkAccessManager net;
        controlPlane.setStreamNetworkAccessManager(&net);
        controlPlane.setBaseUrl(QStringLiteral("https://example.test"));

        SseClient client;
        client.setControlPlane(&controlPlane);
        client.setHelloTimeoutMs(50);        // stands for the 15 s hello time limit
        client.setJitterProvider([]() { return 0.0; });
        client.start();
        QCOMPARE(net.replies.count(), 1);

        auto* firstReply = net.replies.first();
        bool firstWasAborted = false;
        QObject abortObserver;
        QVERIFY(QObject::connect(firstReply, &QNetworkReply::finished, &abortObserver,
                                 [&firstWasAborted, firstReply]() {
                                     firstWasAborted = firstReply->wasAborted();
                                 }, Qt::DirectConnection));

        // No `stream.hello` ever arrives: the hello time limit closes the connection and the
        // ordinary reconnect opens a fresh one.
        QTRY_VERIFY_WITH_TIMEOUT(firstWasAborted, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(net.replies.count(), 2, 2000);
    }

    void anAliveWithoutAHelloDoesNotStopTheHelloTimer()
    {
        ControlPlaneClient controlPlane;
        StreamNetworkAccessManager net;
        controlPlane.setStreamNetworkAccessManager(&net);
        controlPlane.setBaseUrl(QStringLiteral("https://example.test"));

        SseClient client;
        client.setControlPlane(&controlPlane);
        client.setHelloTimeoutMs(80);
        client.setJitterProvider([]() { return 0.0; });
        QSignalSpy helloSpy(&client, &SseClient::hello);
        client.start();
        QCOMPARE(net.replies.count(), 1);

        // A bare `stream.alive` arrives, but never a `stream.hello`.
        net.replies.first()->pushBytes(sseFrame(
            QStringLiteral("stream.alive"),
            QJsonObject{ { QStringLiteral("at"), QStringLiteral("2026-10-06T10:00:20.000Z") } }));
        QCOMPARE(helloSpy.count(), 0);

        // Alive is not a substitute for hello: the hello time limit still expires and retries.
        QTRY_COMPARE_WITH_TIMEOUT(net.replies.count(), 2, 2000);
        QCOMPARE(helloSpy.count(), 0);
    }

    // --- V-L5 (ADR-0083 decision 5): the loss clock runs from the close of the last connection
    // --- that received `stream.hello`, or from start-up - not from every attempt's start. --------

    void theLossClockRunsFromTheLastHelloedClose()
    {
        ControlPlaneClient controlPlane;
        StreamNetworkAccessManager net;
        controlPlane.setStreamNetworkAccessManager(&net);
        controlPlane.setBaseUrl(QStringLiteral("https://example.test"));

        SseClient client;
        client.setControlPlane(&controlPlane);
        client.setPausedDelayMs(400);        // stands for L5's 10 s
        client.setReconnectCapMs(800);
        client.setJitterProvider([]() { return 0.0; });
        QSignalSpy lostSpy(&client, &SseClient::liveUpdatesLostChanged);
        client.start();

        // Start-up armed the clock. Hold the hello back so the close re-anchors it well after
        // start-up, but still inside the first window so nothing fires early.
        QTest::qWait(300);
        QCOMPARE(lostSpy.count(), 0);

        net.replies.first()->pushBytes(helloFrame(0));
        net.replies.first()->pushBytes(closingFrame(QStringLiteral("replaced")));
        net.replies.first()->finishStream();

        // Past start-up's own window (~400 ms), but not the close's: nothing is shown yet. This is
        // what tells the two anchors apart.
        QTest::qWait(150);
        QCOMPARE(lostSpy.count(), 0);

        // The clock fires from the helloed close.
        QTRY_COMPARE_WITH_TIMEOUT(lostSpy.count(), 1, 800);
        QCOMPARE(lostSpy.at(0).at(0).toBool(), true);
    }
};

QTEST_MAIN(TstSseClient)

#include "tst_sse_client.moc"
