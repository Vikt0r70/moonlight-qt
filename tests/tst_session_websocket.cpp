/*****************************************************************************
 * SeatHub fork - unit tests for the control plane's session channel (Plan 03-03 Task 1).
 *
 * Three things about this class are worth pinning down, and none of them needs a socket:
 *
 *   1. The endpoint is the control plane's `/ws/session/{session_id}` - never a Sunshine
 *      socket. The client has no Sunshine credential and no Sunshine channel.
 *   2. The reconnect schedule is exponential and capped at 30s. D-33's promise is "keep
 *      streaming and keep retrying", and an uncapped backoff would quietly turn into "stop
 *      retrying".
 *   3. Routing rules: every documented `type` maps to its own signal, an unknown `type` is a
 *      logged no-op, and a malformed frame does not take the channel down.
 *****************************************************************************/

#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>

#include "seathub/session_websocket.h"

namespace {

QString frame(const QJsonObject& object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QJsonObject sessionBody()
{
    QJsonObject session;
    session.insert(QStringLiteral("id"), QStringLiteral("aaaabbbb-cccc-dddd-eeee-ffff00001111"));
    session.insert(QStringLiteral("state"), QStringLiteral("ACTIVE"));
    session.insert(QStringLiteral("quality_profile"), QStringLiteral("1080p60"));
    session.insert(QStringLiteral("minutes_billed"), 7);
    session.insert(QStringLiteral("reconnect_count"), 0);
    session.insert(QStringLiteral("requested_at"), QStringLiteral("2026-09-19T00:00:00Z"));
    return session;
}

} // namespace

class TstSessionWebSocket : public QObject
{
    Q_OBJECT

private slots:
    // --- endpoint ---------------------------------------------------------------------------

    void channelPath_isTheDocumentedRoute()
    {
        QCOMPARE(SessionWebSocket::channelPath(QStringLiteral("abc")),
                 QStringLiteral("/ws/session/abc"));
    }

    void endpoint_mapsHttpsToWss()
    {
        QCOMPARE(SessionWebSocket::endpoint(QStringLiteral("https://api-sevenhills.damra.co"),
                                            QStringLiteral("abc")),
                 QStringLiteral("wss://api-sevenhills.damra.co/ws/session/abc"));
    }

    void endpoint_mapsHttpToWs()
    {
        QCOMPARE(SessionWebSocket::endpoint(QStringLiteral("http://127.0.0.1:8000"),
                                            QStringLiteral("abc")),
                 QStringLiteral("ws://127.0.0.1:8000/ws/session/abc"));
    }

    void endpoint_neverTargetsSunshine()
    {
        // Sunshine would be at :47989. The client's channel is the control plane's, and the
        // port in the URL is the one it was given.
        const QString url = SessionWebSocket::endpoint(QStringLiteral("https://api-sevenhills.damra.co"),
                                                       QStringLiteral("abc"));
        QVERIFY2(!url.contains(QStringLiteral("47989")), qPrintable(url));
    }

    // --- backoff ----------------------------------------------------------------------------

    void reconnectDelay_isExponentialAndCappedAt30s()
    {
        QCOMPARE(SessionWebSocket::reconnectDelayMs(0), 1000);
        QCOMPARE(SessionWebSocket::reconnectDelayMs(1), 2000);
        QCOMPARE(SessionWebSocket::reconnectDelayMs(2), 4000);
        QCOMPARE(SessionWebSocket::reconnectDelayMs(3), 8000);
        QCOMPARE(SessionWebSocket::reconnectDelayMs(4), 16000);

        // The cap. 1 << 5 would be 32s, so the ceiling takes over here and stays.
        QCOMPARE(SessionWebSocket::reconnectDelayMs(5), 30000);
        QCOMPARE(SessionWebSocket::reconnectDelayMs(6), 30000);
        QCOMPARE(SessionWebSocket::reconnectDelayMs(1000), 30000);
        QCOMPARE(SessionWebSocket::kMaxReconnectDelayMs, 30000);

        // A negative attempt is treated as the first one rather than shifting by a negative.
        QCOMPARE(SessionWebSocket::reconnectDelayMs(-1), 1000);
    }

    void reconnectDelay_neverExceedsTheCap()
    {
        for (int attempt = 0; attempt < 64; ++attempt) {
            QVERIFY(SessionWebSocket::reconnectDelayMs(attempt)
                    <= SessionWebSocket::kMaxReconnectDelayMs);
        }
    }

    // --- routing ----------------------------------------------------------------------------

    void routing_sessionState_reachesTheTypedSignal()
    {
        SessionWebSocket socket;
        QSignalSpy spy(&socket, &SessionWebSocket::sessionStateReceived);

        QJsonObject message;
        message.insert(QStringLiteral("type"), QStringLiteral("session.state"));
        message.insert(QStringLiteral("session"), sessionBody());
        socket.handleMessage(frame(message));

        QCOMPARE(spy.count(), 1);
        const SessionInfo info = spy.at(0).at(0).value<SessionInfo>();
        QCOMPARE(info.state, QStringLiteral("ACTIVE"));
        QCOMPARE(info.minutesBilled, 7);
    }

    void routing_sessionBilling_carriesTheMonotonicIndex()
    {
        SessionWebSocket socket;
        QSignalSpy spy(&socket, &SessionWebSocket::sessionBillingReceived);

        QJsonObject message;
        message.insert(QStringLiteral("type"), QStringLiteral("session.billing"));
        message.insert(QStringLiteral("session_id"), QStringLiteral("abc"));
        message.insert(QStringLiteral("minutes_billed"), 4);
        message.insert(QStringLiteral("balance_minutes"), 26);
        message.insert(QStringLiteral("minute_index"), 3);
        socket.handleMessage(frame(message));

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("abc"));
        QCOMPARE(spy.at(0).at(1).toInt(), 4);
        QCOMPARE(spy.at(0).at(2).toInt(), 26);
        QCOMPARE(spy.at(0).at(3).toInt(), 3);
    }

    void routing_sessionWarning_carriesItsEnumValueAndDeadline()
    {
        SessionWebSocket socket;
        QSignalSpy spy(&socket, &SessionWebSocket::sessionWarningReceived);

        QJsonObject message;
        message.insert(QStringLiteral("type"), QStringLiteral("session.warning"));
        message.insert(QStringLiteral("session_id"), QStringLiteral("abc"));
        message.insert(QStringLiteral("warning"), QStringLiteral("LOW_BALANCE"));
        message.insert(QStringLiteral("deadline_at"), QStringLiteral("2026-09-19T00:50:00Z"));
        socket.handleMessage(frame(message));

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("LOW_BALANCE"));
        QCOMPARE(spy.at(0).at(2).toString(), QStringLiteral("2026-09-19T00:50:00Z"));
    }

    void routing_errorCarriesTheReference()
    {
        SessionWebSocket socket;
        QSignalSpy spy(&socket, &SessionWebSocket::serverErrorReceived);

        QJsonObject message;
        message.insert(QStringLiteral("type"), QStringLiteral("error"));
        message.insert(QStringLiteral("error"), QStringLiteral("The session has ended."));
        message.insert(QStringLiteral("reference"), QStringLiteral("SH-4F7KQ2"));
        socket.handleMessage(frame(message));

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("SH-4F7KQ2"));
    }

    void routing_unknownType_isALoggedNoOpAndNotAFailure()
    {
        SessionWebSocket socket;
        QSignalSpy unknown(&socket, &SessionWebSocket::unknownMessageIgnored);
        QSignalSpy failures(&socket, &SessionWebSocket::transportFailed);
        QSignalSpy states(&socket, &SessionWebSocket::sessionStateReceived);

        QJsonObject message;
        message.insert(QStringLiteral("type"), QStringLiteral("session.something.new"));
        socket.handleMessage(frame(message));

        QVERIFY2(unknown.count() == 1, "an unknown type is reported, once");
        QCOMPARE(unknown.at(0).at(0).toString(), QStringLiteral("session.something.new"));
        // The channel is additive: a message a future server sends must not read as an error.
        QCOMPARE(failures.count(), 0);
        QCOMPARE(states.count(), 0);
    }

    void routing_malformedFrame_doesNotKillTheChannel()
    {
        SessionWebSocket socket;
        QSignalSpy failures(&socket, &SessionWebSocket::transportFailed);

        socket.handleMessage(QStringLiteral("{not json"));
        QCOMPARE(failures.count(), 1);
        // Still usable afterwards - the socket is not closed by a bad frame.
        QCOMPARE(socket.state(), QStringLiteral("idle"));
    }

    void routing_sessionStateWithoutASession_isReportedNotGuessed()
    {
        SessionWebSocket socket;
        QSignalSpy failures(&socket, &SessionWebSocket::transportFailed);
        QSignalSpy states(&socket, &SessionWebSocket::sessionStateReceived);

        QJsonObject message;
        message.insert(QStringLiteral("type"), QStringLiteral("session.state"));
        socket.handleMessage(frame(message));

        QCOMPARE(states.count(), 0);
        QCOMPARE(failures.count(), 1);
    }

    // --- lifecycle without a server ---------------------------------------------------------

    void open_withAnEmptySessionId_doesNothing()
    {
        SessionWebSocket socket;
        socket.open(QString());
        QCOMPARE(socket.state(), QStringLiteral("idle"));
        QVERIFY(!socket.isOpen());
    }

    void close_isDeliberateAndSuppressesReconnect()
    {
        SessionWebSocket socket;
        socket.close();
        QCOMPARE(socket.state(), QStringLiteral("closed"));
    }

    void ping_withoutAnOpenSocket_isHarmless()
    {
        SessionWebSocket socket;
        socket.sendPing();
        QVERIFY(!socket.isOpen());
    }

    void defaultBaseUrlFollowsTheReleaseFeedWhenUnset()
    {
        SessionWebSocket socket;
        socket.setBaseUrl(QString());
        QCOMPARE(socket.baseUrl(), ControlPlaneClient::defaultBaseUrl());
    }
};

QTEST_MAIN(TstSessionWebSocket)

#include "tst_session_websocket.moc"
