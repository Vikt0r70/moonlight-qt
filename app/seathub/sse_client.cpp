#include "sse_client.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTimer>
#include <algorithm>

#include "session_websocket.h"

// Connection log lines carry closed outcomes, attempt numbers and elapsed milliseconds - never the
// stream URL, a token or a frame body.
Q_LOGGING_CATEGORY(seathubSse, "seathub.sse")

bool AccountStateInfo::parse(const QJsonObject& account, AccountStateInfo* out)
{
    if (!out) {
        return false;
    }

    AccountStateInfo info;

    const QJsonObject wallet = account.value(QStringLiteral("wallet")).toObject();
    const QJsonValue balance = wallet.value(QStringLiteral("balance_minutes"));
    // Signed (ADR-0075): a negative pushed balance is shown as it is, never drawn as `0 min`.
    info.balanceMinutes = balance.isDouble() ? static_cast<qint64>(balance.toDouble()) : 0;

    info.liveSessionId = account.value(QStringLiteral("live_session_id")).toString();

    // `open_topup_notice` is the notice object, or null while nothing is open (`openapi.yaml`
    // `AccountStateEvent`). Only its `id` is carried - nothing here needs the rest of the notice.
    const QJsonValue notice = account.value(QStringLiteral("open_topup_notice"));
    if (notice.isObject()) {
        info.openTopupNoticeId = notice.toObject().value(QStringLiteral("id")).toString();
    }

    info.signupStage = account.value(QStringLiteral("signup_stage")).toString();

    *out = info;
    return true;
}

SseClient::SseClient(QObject* parent) : QObject(parent)
{
    m_deadStreamTimer = new QTimer(this);
    m_deadStreamTimer->setSingleShot(true);
    connect(m_deadStreamTimer, &QTimer::timeout, this, [this]() {
        // L3: silence past the bound aborts the reply; `handleFinished()` does the reconnect once
        // the abort's own `finished()` arrives, exactly like any other dropped connection.
        if (m_reply) {
            m_reply->abort();
        }
    });

    m_pausedTimer = new QTimer(this);
    m_pausedTimer->setSingleShot(true);
    connect(m_pausedTimer, &QTimer::timeout, this, [this]() {
        if (m_live || m_paused) {
            return;
        }
        m_paused = true;
        emit pausedChanged(true);
        setState(QStringLiteral("paused"));
    });

    // V-L4 (ADR-0083 decision 4): a connection that never sends `stream.hello` is closed and
    // retried. Like the dead-stream timer, the abort's own `finished()` drives the reconnect
    // through `handleFinished()`. `stream.alive` deliberately does not stop this.
    m_helloTimer = new QTimer(this);
    m_helloTimer->setSingleShot(true);
    connect(m_helloTimer, &QTimer::timeout, this, [this]() {
        if (m_reply) {
            m_reply->abort();
        }
    });

    // R7 (ADR-0067 amendment 2026-10-06): the live-updates indicator's own clock. It is not the L5
    // fallback timer above: it runs from the first open or from the close of a connection that had
    // received `stream.hello`, and a hello stops it, so a routine reconnect never shows the indicator.
    m_lostTimer = new QTimer(this);
    m_lostTimer->setSingleShot(true);
    connect(m_lostTimer, &QTimer::timeout, this, [this]() {
        if (m_updatesLost) {
            return;
        }
        m_updatesLost = true;
        emit liveUpdatesLostChanged(true);
    });

    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &SseClient::beginConnection);
    // V-COUNTDOWN: one tick per second while a reconnect is armed, re-publishing the wait.
    m_retryTickTimer = new QTimer(this);
    connect(m_retryTickTimer, &QTimer::timeout, this, &SseClient::publishRetrySeconds);

    m_liveResetTimer = new QTimer(this);
    m_liveResetTimer->setSingleShot(true);
    connect(m_liveResetTimer, &QTimer::timeout, this, [this]() { m_attempt = 0; });

    connect(this, &SseClient::hello, this, &SseClient::onHello);
    connect(this, &SseClient::alive, this, &SseClient::onAlive);
    connect(this, &SseClient::closing, this, &SseClient::onClosing);
    connect(this, &SseClient::resync, this, &SseClient::onNamedEvent);
    connect(this, &SseClient::accountState, this,
            [this](const AccountStateInfo&) { onNamedEvent(); });
    connect(this, &SseClient::sessionState, this, [this](const SessionInfo&) { onNamedEvent(); });
}

void SseClient::setControlPlane(ControlPlaneClient* controlPlane)
{
    m_controlPlane = controlPlane;
}

void SseClient::start()
{
    if (m_running || !m_controlPlane) {
        return;
    }
    m_running = true;
    m_stopping = false;
    m_attempt = 0;
    // R7: "from start-up" - no hello within L5's 10 s of the first open is a real loss.
    m_lostTimer->start(m_pausedDelayMs);
    beginConnection();
}

void SseClient::stop()
{
    m_running = false;
    m_stopping = true;
    m_deadStreamTimer->stop();
    m_helloTimer->stop();
    m_pausedTimer->stop();
    m_reconnectTimer->stop();
    m_retryTickTimer->stop();
    m_liveResetTimer->stop();
    clearLiveUpdatesLost();
    if (m_reply) {
        QNetworkReply* reply = m_reply;
        m_reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    m_live = false;
    if (m_paused) {
        m_paused = false;
        emit pausedChanged(false);
    }
    setState(QStringLiteral("idle"));
}

void SseClient::setDeadStreamTimeoutMs(int ms)
{
    m_deadStreamTimeoutMs = ms;
}

void SseClient::setHelloTimeoutMs(int ms)
{
    m_helloTimeoutMs = ms;
}

int SseClient::nextRetrySeconds() const
{
    if (!m_running || m_stopping || !m_reconnectTimer->isActive()) {
        return -1;
    }
    const int remaining = m_reconnectTimer->remainingTime();
    if (remaining < 0) {
        return -1;
    }
    return (remaining + 999) / 1000;
}

void SseClient::publishRetrySeconds()
{
    // V-COUNTDOWN (ADR-0083 decision 7): publish the genuine wait on this object's own thread, so
    // the facade never reads `m_reconnectTimer` across threads.
    const int seconds = nextRetrySeconds();
    emit retrySecondsChanged(seconds);
    if (seconds > 0) {
        if (!m_retryTickTimer->isActive()) {
            m_retryTickTimer->start(1000);
        }
    } else {
        m_retryTickTimer->stop();
    }
}

void SseClient::wakeNow()
{
    // V-WAKE (ADR-0083 decision 3): a network-back wake-up hint. If a reconnect is armed, reset
    // the ordinary attempt count and start it at once; otherwise nothing (an attempt in flight or
    // a live stream is not interrupted). The reset is what makes the wake a fresh start: an
    // attempt started by the wake that then fails must wait the 1 s floor again, not the wait the
    // old count had reached (wave 21 cycle 2, F3).
    if (m_running && !m_stopping && m_reconnectTimer->isActive()) {
        m_attempt = 0;
        m_reconnectTimer->start(0);
        publishRetrySeconds();
    }
}

void SseClient::setPausedDelayMs(int ms)
{
    m_pausedDelayMs = ms;
}

void SseClient::setReconnectCapMs(int ms)
{
    m_reconnectCapMs = ms;
}

void SseClient::setJitterProvider(std::function<double()> provider)
{
    m_jitterProvider = std::move(provider);
}

int SseClient::reconnectDelayMs(int attempt, double jitter)
{
    const double clampedJitter = std::min(std::max(jitter, 0.0), 0.2);
    const int base = SessionWebSocket::reconnectDelayMs(attempt);
    return static_cast<int>(base * (1.0 + clampedJitter));
}

double SseClient::jitterValue() const
{
    if (m_jitterProvider) {
        return m_jitterProvider();
    }
    return QRandomGenerator::global()->generateDouble() * 0.2;
}

void SseClient::beginConnection()
{
    if (m_stopping) {
        return;
    }
    m_helloNamedAliveMs = false;
    m_connectionHelloed = false;
    m_live = false;
    m_lastCloseReason.clear();
    m_deadStreamTimer->stop();
    m_connectionTimer.start();
    qCInfo(seathubSse).noquote()
        << QStringLiteral("sse event=connect attempt=%1").arg(m_attempt + 1);
    setState(QStringLiteral("connecting"));
    // V-COUNTDOWN: the attempt is starting, so no countdown is pending any more.
    publishRetrySeconds();
    openStream();
    // V-L4: arm the hello limit for this attempt; `stream.hello` stops it.
    m_helloTimer->start(m_helloTimeoutMs);
    // L5: not live starts (or restarts, on reconnect) the fallback-indicator countdown.
    m_pausedTimer->start(m_pausedDelayMs);
}

void SseClient::openStream()
{
    if (m_reply) {
        QNetworkReply* previous = m_reply;
        m_reply = nullptr;
        previous->disconnect(this);
        previous->deleteLater();
    }
    m_reply = m_controlPlane->openAccountStream();
    connect(m_reply, &QNetworkReply::readyRead, this, &SseClient::handleReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &SseClient::handleFinished);
}

void SseClient::scheduleReconnect(int delayMs)
{
    if (m_stopping) {
        return;
    }
    qCInfo(seathubSse).noquote()
        << QStringLiteral("sse event=reconnect attempt=%1 delay_ms=%2").arg(m_attempt + 1).arg(delayMs);
    m_reconnectTimer->start(delayMs);
    publishRetrySeconds();
}

void SseClient::setState(const QString& state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged();
}

void SseClient::clearLiveUpdatesLost()
{
    m_lostTimer->stop();
    if (m_updatesLost) {
        m_updatesLost = false;
        emit liveUpdatesLostChanged(false);
    }
}

void SseClient::markLive()
{
    if (m_live) {
        return;
    }
    m_live = true;
    m_pausedTimer->stop();
    if (m_paused) {
        m_paused = false;
        emit pausedChanged(false);
    }
    setState(QStringLiteral("live"));
    // L4: "back to 1 s after a connection that stayed live 60 s" - `m_attempt` only resets once
    // this fires, not the moment liveness starts, so a connection that flaps right after
    // reconnecting still backs off further rather than retrying at the floor forever.
    m_liveResetTimer->start(kLiveResetMs);
}

void SseClient::handleReadyRead()
{
    if (m_reply) {
        feed(m_reply->readAll());
    }
}

void SseClient::handleFinished()
{
    QNetworkReply* reply = m_reply;
    m_reply = nullptr;
    m_deadStreamTimer->stop();
    m_helloTimer->stop();
    m_liveResetTimer->stop();

    int status = 0;
    if (reply) {
        status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->disconnect(this);
        reply->deleteLater();
    }

    if (m_stopping) {
        // `stop()` already tore everything down; this is just the reply it asked to abort
        // finally reporting in.
        return;
    }

    const QString reason = m_lastCloseReason;
    m_lastCloseReason.clear();

    // The close reasons are the server's closed set (`revoked`, `replaced`, `shutdown`); anything
    // else is logged as `other` so a stray string never reaches the log.
    const bool knownReason = reason == QStringLiteral("revoked") || reason == QStringLiteral("replaced")
        || reason == QStringLiteral("shutdown");
    qCInfo(seathubSse).noquote()
        << QStringLiteral("sse event=loss reason=%1 http_status=%2 helloed=%3 elapsed_ms=%4")
               .arg(reason.isEmpty() ? QStringLiteral("transport")
                                     : (knownReason ? reason : QStringLiteral("other")))
               .arg(status)
               .arg(m_connectionHelloed ? QStringLiteral("true") : QStringLiteral("false"))
               .arg(m_connectionTimer.isValid() ? m_connectionTimer.elapsed() : 0);

    // `stream.closing {reason: "revoked"}`, or the transport itself answering 401/403 to the
    // stream request (no closing frame at all - e.g. the bearer was already revoked before this
    // attempt opened). Both mean the same thing: stop for good (§2.3).
    if (reason == QStringLiteral("revoked") || status == 401 || status == 403) {
        m_pausedTimer->stop();
        clearLiveUpdatesLost();
        m_live = false;
        if (m_paused) {
            m_paused = false;
            emit pausedChanged(false);
        }
        setState(QStringLiteral("stopped"));
        qCInfo(seathubSse) << "sse event=stopped reason=revoked";
        emit revoked();
        return;
    }

    m_live = false;

    // R7: the loss clock counts from the close of a connection that had received `stream.hello`. A
    // connection that never got one adds nothing: the clock already running (from start-up or from the
    // last such close) keeps counting, so repeated failed attempts cannot postpone a real loss forever.
    if (m_connectionHelloed) {
        m_connectionHelloed = false;
        m_lostTimer->start(m_pausedDelayMs);
    }

    if (reason == QStringLiteral("replaced")) {
        // §2.3: reconnecting immediately would fight the connection that just replaced this one.
        // Wait the cap, not the ordinary schedule's floor.
        scheduleReconnect(m_reconnectCapMs);
    }
    else {
        // `shutdown`, a dead-stream abort, or an ordinary transport drop - the ordinary schedule.
        scheduleReconnect(reconnectDelayMs(m_attempt, jitterValue()));
        ++m_attempt;
    }
}

void SseClient::onHello(int aliveMs)
{
    // V-L4: a hello proves the connection is alive; stop the hello limit for this attempt.
    m_helloTimer->stop();
    qCInfo(seathubSse).noquote()
        << QStringLiteral("sse event=hello alive_ms=%1 elapsed_ms=%2")
               .arg(aliveMs)
               .arg(m_connectionTimer.isValid() ? m_connectionTimer.elapsed() : 0);
    // R7: any hello proves a working stream - the indicator hides at once, whether or not the first
    // `stream.alive` (which counts the connection live for L3/L5) has come yet.
    m_connectionHelloed = true;
    clearLiveUpdatesLost();
    if (aliveMs > 0) {
        m_helloNamedAliveMs = true;
        m_deadStreamTimer->start(m_deadStreamTimeoutMs);
    }
    else {
        // L3: "or from stream.hello when the hello has no alive_ms" - live immediately, and the
        // dead-stream bound stays unenforced for this attempt.
        m_helloNamedAliveMs = false;
        m_deadStreamTimer->stop();
        markLive();
    }
}

void SseClient::onAlive()
{
    onNamedEvent();
    markLive();
}

void SseClient::onClosing(const QString& reason)
{
    // The server is about to close this connection; the actual `finished()` (real or, in tests,
    // simulated) is what `handleFinished()` acts on - recording the reason here is all this does,
    // so a closing frame is never handled twice.
    m_lastCloseReason = reason;
}

void SseClient::onNamedEvent()
{
    if (m_helloNamedAliveMs) {
        m_deadStreamTimer->start(m_deadStreamTimeoutMs);
    }
}

void SseClient::feed(const QByteArray& chunk)
{
    for (int i = 0; i < chunk.size(); ++i) {
        const char c = chunk.at(i);

        if (c == '\n') {
            if (m_lastWasCR) {
                // The second half of a CRLF pair already dispatched on the '\r' below. A chunk
                // boundary may fall between the two bytes, so this state has to survive across
                // `feed()` calls rather than being decided by lookahead within one chunk.
                m_lastWasCR = false;
                continue;
            }
            processLine(m_lineBuffer);
            m_lineBuffer.clear();
            continue;
        }

        m_lastWasCR = false;

        if (c == '\r') {
            processLine(m_lineBuffer);
            m_lineBuffer.clear();
            m_lastWasCR = true;
            continue;
        }

        m_lineBuffer.append(c);
        if (m_lineBuffer.size() > kMaxLineBytes) {
            // T-06.4-46: drop rather than let a corrupt/hostile stream grow this without bound.
            m_lineBuffer.clear();
        }
    }
}

void SseClient::processLine(const QByteArray& line)
{
    if (line.isEmpty()) {
        dispatchFrame();
        return;
    }

    if (line.startsWith(':')) {
        // A comment - ignored. The transport keep-alive is exactly this shape (`: ping`), and it
        // must never be mistaken for a real event (that is `stream.alive`'s whole reason to
        // exist, per ADR-0067 decision 2 / `06.4-RESEARCH-FORK.md` §Summary).
        return;
    }

    const int colon = line.indexOf(':');
    QByteArray field;
    QByteArray value;
    if (colon < 0) {
        field = line;
    }
    else {
        field = line.left(colon);
        value = line.mid(colon + 1);
        if (value.startsWith(' ')) {
            value.remove(0, 1);
        }
    }

    if (field == "event") {
        m_eventType = value;
    }
    else if (field == "data") {
        if (m_hasData) {
            m_dataBuffer += '\n';
        }
        m_dataBuffer += value;
        m_hasData = true;
    }
    // Any other field name (`id`, `retry`, a future addition) is SSE's own additive contract -
    // silently ignored, never an error.
}

void SseClient::dispatchFrame()
{
    const QByteArray eventType = m_eventType;
    const QByteArray dataBytes = m_dataBuffer;
    const bool hadData = m_hasData;

    m_eventType.clear();
    m_dataBuffer.clear();
    m_hasData = false;

    // A frame with no `data:` line at all (e.g. a bare blank-line-terminated `event:` frame, or
    // two blank lines in a row) dispatches nothing - there is nothing to parse and nothing this
    // build is required to act on.
    if (!hadData) {
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(dataBytes);
    if (!doc.isObject()) {
        return;
    }
    const QJsonObject data = doc.object();

    if (eventType == "stream.hello") {
        const QJsonValue aliveMs = data.value(QStringLiteral("alive_ms"));
        emit hello(aliveMs.isDouble() ? aliveMs.toInt() : 0);
        // `server_time` is an ISO-8601 UTC instant. A missing or malformed one emits nothing.
        const QDateTime serverUtc = QDateTime::fromString(
            data.value(QStringLiteral("server_time")).toString(), Qt::ISODateWithMs);
        if (serverUtc.isValid()) {
            emit serverTime(serverUtc.toUTC());
        }
    }
    else if (eventType == "stream.alive") {
        emit alive();
    }
    else if (eventType == "stream.resync") {
        emit resync();
    }
    else if (eventType == "stream.closing") {
        emit closing(data.value(QStringLiteral("reason")).toString());
    }
    else if (eventType == "account.state") {
        AccountStateInfo info;
        if (AccountStateInfo::parse(data.value(QStringLiteral("account")).toObject(), &info)) {
            emit accountState(info);
        }
    }
    else if (eventType == "session.state") {
        SessionInfo info;
        if (SessionInfo::parse(data.value(QStringLiteral("session")).toObject(), &info)) {
            emit sessionState(info);
        }
    }
    // Anything else (`agent.wake`, `rig.state`, a future event type this build predates) is not
    // SeatHub's to act on - ignored, exactly like an unrecognised field name above.
}
