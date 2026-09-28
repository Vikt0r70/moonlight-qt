/*****************************************************************************
 * SeatHub fork - unit tests for the SSE frame reader (06.4 Plan 14 Task 1).
 *
 * `SseClient::feed()` is the test seam: these tests hand it raw bytes exactly as
 * `QNetworkReply::readyRead` would (Task 2 wires the real socket), built from the same
 * cross-language fixtures pytest and cargo use (`docs/spec/fixtures/events/`, synced
 * byte-for-byte into `tests/fixtures/events/` - the verify block's `cmp` proves it).
 *****************************************************************************/

#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>

#include "seathub/sse_client.h"

namespace {

// Loads one `tests/fixtures/events/<name>.json` file - `{"event": ..., "data": ...}` - and
// renders it as one SSE frame: `event: <event>\ndata: <compact data>\n\n`.
QByteArray sseFrameFromFixture(const QString& name)
{
    const QString path = QFINDTESTDATA("fixtures/events/" + name + QStringLiteral(".json"));
    if (path.isEmpty()) {
        return QByteArray();
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    const QJsonObject fixture = QJsonDocument::fromJson(file.readAll()).object();
    const QString event = fixture.value(QStringLiteral("event")).toString();
    const QJsonObject data = fixture.value(QStringLiteral("data")).toObject();
    const QByteArray dataJson = QJsonDocument(data).toJson(QJsonDocument::Compact);

    QByteArray frame;
    frame += "event: ";
    frame += event.toUtf8();
    frame += "\n";
    frame += "data: ";
    frame += dataJson;
    frame += "\n\n";
    return frame;
}

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
};

QTEST_MAIN(TstSseClient)

#include "tst_sse_client.moc"
