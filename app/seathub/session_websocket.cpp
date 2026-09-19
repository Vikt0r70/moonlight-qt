#include "session_websocket.h"

#include <QAbstractSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLoggingCategory>
#include <QNetworkRequest>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

// Every routing decision on this channel is observable in the log, because "the warning
// arrived and was ignored" and "the warning never arrived" look identical from the UI.
Q_LOGGING_CATEGORY(seathubSocketLog, "seathub.websocket")

namespace {

// The documented `oneOf` types on /ws/session/{session_id} (openapi.yaml
// `x-websocket-channels`). An unlisted type is a no-op, not an error.
const char* kTypeSessionState = "session.state";
const char* kTypeSessionBilling = "session.billing";
const char* kTypeSessionWarning = "session.warning";
const char* kTypeError = "error";

// `WsPing` - the only client->server message the channel defines.
const char* kTypePing = "ping";

} // namespace

SessionWebSocket::SessionWebSocket(QObject* parent)
    : QObject(parent),
      m_socket(new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this)),
      m_reconnectTimer(new QTimer(this)),
      m_state(QStringLiteral("idle"))
{
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (!m_closing && !m_sessionId.isEmpty()) {
            attachSocket();
        }
    });

    connect(m_socket, &QWebSocket::connected, this, [this]() {
        m_attempt = 0;
        setState(QStringLiteral("open"));
        qCInfo(seathubSocketLog) << "session channel open for" << m_sessionId;
        emit opened();
    });

    connect(m_socket, &QWebSocket::disconnected, this, [this]() {
        if (m_closing) {
            setState(QStringLiteral("closed"));
            qCInfo(seathubSocketLog) << "session channel closed deliberately";
            return;
        }
        // D-33: a drop is not a session end. Schedule the retry and report the gap; the
        // stream keeps running either way.
        scheduleReconnect();
    });

    connect(m_socket, &QWebSocket::textMessageReceived, this, [this](const QString& message) {
        handleMessage(message);
    });

    connect(m_socket, &QWebSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError error) {
                // A socket error is followed by `disconnected()`, which owns the reconnect
                // decision. This only records what happened.
                Q_UNUSED(error);
                emit transportFailed(m_socket->errorString());
                qCWarning(seathubSocketLog) << "session channel error:" << m_socket->errorString();
            });

    connect(m_socket, &QWebSocket::sslErrors, this,
            [this](const QList<QSslError>& errors) {
                // Deliberately not ignored. The control plane presents a publicly-trusted
                // certificate (`api-sevenhills.damra.co`); a certificate we cannot verify is
                // a reason to stop, not to proceed. Sunshine's self-signed certificate is a
                // different problem on a different socket this client never opens.
                QStringList messages;
                for (const QSslError& error : errors) {
                    messages.append(error.errorString());
                }
                emit transportFailed(messages.join(QStringLiteral("; ")));
            });
}

QString SessionWebSocket::channelPath(const QString& sessionId)
{
    return QStringLiteral("/ws/session/") + sessionId;
}

QString SessionWebSocket::endpoint(const QString& baseUrl, const QString& sessionId)
{
    QString base = baseUrl;
    if (base.startsWith(QLatin1String("https://"))) {
        base.replace(0, 8, QStringLiteral("wss://"));
    }
    else if (base.startsWith(QLatin1String("http://"))) {
        base.replace(0, 7, QStringLiteral("ws://"));
    }
    // No trailing-slash branch: the control plane's base URL is a bare origin
    // (`openapi.yaml` `servers[].url`), and a stray slash would produce `//ws/...`.
    return base + channelPath(sessionId);
}

int SessionWebSocket::reconnectDelayMs(int attempt)
{
    if (attempt < 0) {
        attempt = 0;
    }

    // ME-05: clamp the *shift*, not just the product. `1000 * (1 << attempt)` is already past
    // `INT_MAX` at attempt 22 and shifts into the sign bit from attempt 31, so the old guard at
    // 30 was eight attempts too late: from attempt 22 the delay wrapped negative, `QTimer`
    // refused a negative interval, and the channel was permanently dead after roughly nine
    // minutes offline (`scheduleReconnect()` had already set the state to "reconnecting").
    // The cap is reached at attempt 5, so no higher shift can change the answer.
    if (attempt >= 5) {
        return kMaxReconnectDelayMs;
    }

    const int delay = 1000 * (1 << attempt);
    return delay > kMaxReconnectDelayMs ? kMaxReconnectDelayMs : delay;
}

void SessionWebSocket::setBaseUrl(const QString& baseUrl)
{
    m_baseUrl = baseUrl.isEmpty() ? ControlPlaneClient::defaultBaseUrl() : baseUrl;
}

void SessionWebSocket::setAccessToken(const QString& token)
{
    m_accessToken = token;
}

bool SessionWebSocket::isOpen() const
{
    return m_socket->state() == QAbstractSocket::ConnectedState;
}

void SessionWebSocket::setState(const QString& state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged();
}

void SessionWebSocket::open(const QString& sessionId)
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this, [this, sessionId]() { open(sessionId); }, Qt::QueuedConnection);
        return;
    }

    if (sessionId.isEmpty()) {
        return;
    }

    if (sessionId == m_sessionId && !m_closing
        && (isOpen() || m_state == QLatin1String("connecting"))) {
        return;
    }

    m_sessionId = sessionId;
    m_closing = false;
    m_attempt = 0;
    m_reconnectTimer->stop();
    attachSocket();
}

void SessionWebSocket::close()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this]() { close(); }, Qt::QueuedConnection);
        return;
    }

    m_closing = true;
    m_reconnectTimer->stop();
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->close();
    }
    setState(QStringLiteral("closed"));
}

void SessionWebSocket::attachSocket()
{
    setState(QStringLiteral("connecting"));

    QNetworkRequest request{QUrl(endpoint(m_baseUrl, m_sessionId))};
    if (!m_accessToken.isEmpty()) {
        request.setRawHeader("Authorization",
                             QByteArrayLiteral("Bearer ") + m_accessToken.toUtf8());
    }

    m_socket->open(request);
}

void SessionWebSocket::scheduleReconnect()
{
    const int delay = reconnectDelayMs(m_attempt);
    const int attempt = m_attempt;
    ++m_attempt;

    setState(QStringLiteral("reconnecting"));
    qCInfo(seathubSocketLog) << "session channel dropped; retrying in" << delay << "ms (attempt"
                             << attempt << ")";
    emit dropped(delay, attempt);
    emit reconnectingIn(delay, attempt);

    m_reconnectTimer->start(delay);
}

void SessionWebSocket::sendPing()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this]() { sendPing(); }, Qt::QueuedConnection);
        return;
    }

    if (!isOpen()) {
        return;
    }

    QJsonObject ping;
    ping.insert(QStringLiteral("type"), QLatin1String(kTypePing));
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(ping).toJson(QJsonDocument::Compact)));
}

void SessionWebSocket::handleMessage(const QString& message)
{
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &parseError);
    if (!doc.isObject()) {
        // A frame that is not a JSON object cannot be routed and is not worth killing the
        // channel over. Recorded so it is not invisible.
        emit transportFailed(QStringLiteral("malformed channel frame: ")
                             + parseError.errorString());
        qCWarning(seathubSocketLog) << "malformed frame on session channel";
        return;
    }

    const QJsonObject object = doc.object();
    const QString type = object.value(QStringLiteral("type")).toString();

    if (type == QLatin1String(kTypeSessionState)) {
        SessionInfo session;
        if (SessionInfo::parse(object.value(QStringLiteral("session")).toObject(), &session)) {
            emit sessionStateReceived(session);
        }
        else {
            emit transportFailed(QStringLiteral("session.state without a parsable session"));
        }
        return;
    }

    if (type == QLatin1String(kTypeSessionBilling)) {
        emit sessionBillingReceived(
            object.value(QStringLiteral("session_id")).toString(),
            object.value(QStringLiteral("minutes_billed")).toInt(),
            object.value(QStringLiteral("balance_minutes")).toInt(),
            object.value(QStringLiteral("minute_index")).toInt());
        return;
    }

    if (type == QLatin1String(kTypeSessionWarning)) {
        emit sessionWarningReceived(
            object.value(QStringLiteral("session_id")).toString(),
            object.value(QStringLiteral("warning")).toString(),
            object.value(QStringLiteral("deadline_at")).toString());
        return;
    }

    if (type == QLatin1String(kTypeError)) {
        emit serverErrorReceived(object.value(QStringLiteral("error")).toString(),
                                 object.value(QStringLiteral("reference")).toString());
        return;
    }

    // Additive channel: a type this build does not know is a logged no-op. Anything else
    // would make every future server-side addition a client crash.
    qCInfo(seathubSocketLog) << "ignoring unknown channel message type:" << type;
    emit unknownMessageIgnored(type);
}
