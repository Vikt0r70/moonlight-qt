#pragma once

// The control plane's session channel (D-29: QWebSocket).
//
// `docs/spec/openapi.yaml` `x-websocket-channels` defines one customer channel:
//
//   /ws/session/{session_id}    bearerAuth
//
// It is the **control plane's** channel, not Sunshine's. The client has no Sunshine socket
// and no Sunshine credential (COVERAGE.md; `docs/spec/client.md` §Session Token Fields). The
// server sends:
//
//   session.state     a `Session` on connect and on every change
//   session.billing   once per minute charged, carrying balance and the monotonic minute index
//   session.warning   LOW_BALANCE | SAVE_NOW | DISCONNECTED | RECONNECT_LIMIT_NEAR |
//                     OWNER_RESERVATION_NEAR, so the client presents a warning without diffing state
//   error             the `Error` schema's customer sentence plus its ADR-0008 reference
//
// and accepts only `ping`. There is no command surface here: "Every action the client can
// take has an HTTP route; a socket that accepts commands is a second authorisation surface to
// get right, for no benefit."
//
// Two behaviours are deliberate rather than incidental:
//
//   * An unrecognised `type` is a logged no-op, never a fatal error. The channel is additive
//     and a client older than the server must not die on a message it does not know.
//   * A drop never ends the session. D-33: when the control plane is unreachable mid-stream
//     the stream keeps running and the client keeps retrying; the hard stop is the session
//     token's `authorized_through` horizon, which the liveness side owns. So this class
//     reconnects with exponential backoff capped at 30s and reports the gap, rather than
//     tearing anything down.
//
// Threading matches `ControlPlaneClient`: upstream `Session::exec()` suspends all Qt
// processing for the whole stream (`app/streaming/session.cpp:1965-1966`), so a QWebSocket on
// the Qt main thread would be deaf for exactly the interval it exists to serve. This object
// is meant to be moved onto the same network thread as the HTTP client (`moveToThread`) and
// is therefore driven entirely by queued invocations.

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>

#include "control_plane_client.h"

class QTimer;
class QWebSocket;

class SessionWebSocket : public QObject
{
    Q_OBJECT

    /// idle | connecting | open | reconnecting | closed
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)

public:
    explicit SessionWebSocket(QObject* parent = nullptr);

    /// The reconnect backoff ceiling. D-33 keeps retrying for as long as the session is
    /// alive; the cap is what stops the retries from drifting into minutes, not a limit on
    /// how many there may be.
    static const int kMaxReconnectDelayMs = 30000;

    /// `/ws/session/{session_id}` - the documented channel path.
    static QString channelPath(const QString& sessionId);

    /// The absolute endpoint for a control-plane base URL. `https` becomes `wss` and `http`
    /// becomes `ws`; nothing else is rewritten.
    static QString endpoint(const QString& baseUrl, const QString& sessionId);

    /// Exponential backoff, capped: attempt 0 -> 1s, 1 -> 2s, 2 -> 4s ... >= 5 -> 30s. Pure,
    /// so the schedule is asserted without a socket.
    static int reconnectDelayMs(int attempt);

    void setBaseUrl(const QString& baseUrl);
    QString baseUrl() const { return m_baseUrl; }

    /// The same bearer access token the HTTP client uses. Attached as an `Authorization`
    /// header on the handshake; never exposed (D-35).
    void setAccessToken(const QString& token);

    /// Opens the channel for a session. A second call while already open for the same session
    /// is a no-op; for a different session it closes and reopens.
    void open(const QString& sessionId);
    /// Closes deliberately. Suppresses the reconnect path - a close the client asked for is
    /// not a drop to recover from.
    void close();

    /// The only message the channel accepts from the client.
    void sendPing();

    /// Routes one channel frame. The socket calls this for every `textMessageReceived`; it is
    /// public because the routing rules - an unknown `type` is a logged no-op, a malformed
    /// frame is not fatal - are the part of this class worth asserting, and asserting them
    /// needs no socket.
    void handleMessage(const QString& message);

    QString state() const { return m_state; }
    QString sessionId() const { return m_sessionId; }
    int reconnectAttempts() const { return m_attempt; }
    bool isOpen() const;

signals:
    void stateChanged();
    void opened();
    /// The channel dropped and will retry in `delayMs`. Non-fatal by contract (D-33).
    void dropped(int delayMs, int attempt);
    void reconnectingIn(int delayMs, int attempt);

    /// A `session.state` message: the same `Session` schema the REST route returns, parsed by
    /// the same parser so the two can never disagree.
    void sessionStateReceived(const SessionInfo& session);
    /// A `session.billing` message. `minuteIndex` is monotonic, so a gap means a missed
    /// message rather than a billing event that did not happen.
    void sessionBillingReceived(const QString& sessionId, int minutesBilled, int balanceMinutes,
                                int minuteIndex);
    /// A `session.warning` message.
    void sessionWarningReceived(const QString& sessionId, const QString& warning,
                                const QString& deadlineAt);
    /// An `error` message, carrying the control plane's sentence and its ADR-0008 reference.
    void serverErrorReceived(const QString& error, const QString& reference);
    /// A well-formed message whose `type` this build does not know. Logged, not fatal.
    void unknownMessageIgnored(const QString& type);
    /// A malformed frame or a socket-level error. Diagnostics; never customer copy.
    void transportFailed(const QString& message);

private:
    void setState(const QString& state);
    void attachSocket();
    void scheduleReconnect();

    QWebSocket* m_socket = nullptr;
    QTimer* m_reconnectTimer = nullptr;

    QString m_baseUrl;
    QString m_accessToken;
    QString m_state;
    QString m_sessionId;
    /// A close the client asked for. Distinct from a drop, and the flag that turns the
    /// reconnect path off.
    bool m_closing = false;
    int m_attempt = 0;
};
