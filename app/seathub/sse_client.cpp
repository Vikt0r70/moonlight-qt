#include "sse_client.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

bool AccountStateInfo::parse(const QJsonObject& account, AccountStateInfo* out)
{
    if (!out) {
        return false;
    }

    AccountStateInfo info;

    const QJsonObject wallet = account.value(QStringLiteral("wallet")).toObject();
    const QJsonValue balance = wallet.value(QStringLiteral("balance_minutes"));
    info.balanceMinutes = balance.isDouble() ? static_cast<qint64>(balance.toDouble()) : 0;
    if (info.balanceMinutes < 0) {
        info.balanceMinutes = 0;
    }

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
