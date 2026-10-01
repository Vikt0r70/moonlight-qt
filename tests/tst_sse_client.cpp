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
};

QTEST_MAIN(TstSseClient)

#include "tst_sse_client.moc"
