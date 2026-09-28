#pragma once

// SeatHub's SSE frame reader for `/api/stream` (ADR-0062 decisions 1, 9, 10; ADR-0067 decisions
// 2-3; ADR-0044 confinement to `app/seathub/`).
//
// Hand-written, not a third-party library: the wire format here is `event:`/`data:`/`: comment`
// lines with no `Last-Event-ID` replay (ADR-0062 decision 4, `06.4-RESEARCH-FORK.md` "Don't
// Hand-Roll"), and Qt has no `EventSource` of its own. `feed()` is the test seam and the
// production entry point alike: it takes raw bytes exactly as `QNetworkReply::readyRead` would
// hand them, split anywhere a chunk boundary happens to fall, so the parser is asserted without a
// socket (Task 1). `ControlPlaneClient::openAccountStream()` is what actually opens the
// connection, owns the reply, and calls `feed()` (Task 2) - the bearer token never crosses into
// this object (D-35, T-06.4-45).
//
// Reuses `SessionInfo::parse` for `session.state` verbatim - one parser, never a second one
// (`06.4-RESEARCH-FORK.md` §1.2). `AccountStateInfo` is new: no existing fork struct carries
// `wallet` + `live_session_id` + `open_topup_notice` + `signup_stage` together.

#include <QByteArray>
#include <QMetaType>
#include <QObject>
#include <QString>

#include <functional>

#include "control_plane_client.h"

class QJsonObject;
class QNetworkReply;
class QTimer;
class ControlPlaneClient;

/// `AccountStateEvent.account` (`docs/spec/openapi.yaml`): the `account:{user_id}` topic's
/// snapshot and `GET /api/account-state`'s body. Defensive like `SessionInfo::parse` - a
/// malformed or absent field reads empty/zero, never a crash (T-06.4-46).
struct AccountStateInfo
{
    /// `wallet.balance_minutes`. Never negative on the wire; a malformed/absent value reads 0.
    qint64 balanceMinutes = 0;
    /// `live_session_id`. Empty when the account has no live session.
    QString liveSessionId;
    /// `open_topup_notice`'s own `id`, when the notice object is present; empty when
    /// `open_topup_notice` is null (the common case).
    QString openTopupNoticeId;
    /// `SignupStage`: `email_pending` | `phone_pending` | `complete`.
    QString signupStage;

    static bool parse(const QJsonObject& account, AccountStateInfo* out);
};
// Crosses a QSignalSpy/queued-connection boundary the same way `SessionInfo` already does.
Q_DECLARE_METATYPE(AccountStateInfo)

/// One connection's frame reader and the connection's own state machine (Task 2: L3-L5,
/// ADR-0067). Owns no `QNetworkAccessManager` of its own - `openAccountStream()` on the
/// `ControlPlaneClient` given to `setControlPlane()` opens each attempt, on its own manager, so
/// the bearer never crosses into this object (D-35, T-06.4-45). `feed()` stays the test seam
/// (Task 1); `start()`/`stop()` are the production entry points that wire a real `QNetworkReply`
/// to it.
class SseClient : public QObject
{
    Q_OBJECT

    /// `idle` (never started, or `stop()` called) | `connecting` (a reply is open, not live yet)
    /// | `live` | `paused` (L5: not live for `pausedDelayMs`, shows the fallback indicator) |
    /// `stopped` (revoked, or a 401/403 - requests no further stream).
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)

public:
    explicit SseClient(QObject* parent = nullptr);

    /// Raw bytes from the wire, in any chunking - a whole frame at once, or one byte at a time
    /// (`splitsFramesAtAnyByte`). Reassembles lines across calls (CR, LF or CRLF, split anywhere),
    /// ignores comment lines (`: ...`) and field names this build does not recognise, and
    /// dispatches one frame per blank line - SSE's own additive contract: an event type this
    /// build has no case for is a silent no-op, never an error.
    void feed(const QByteArray& chunk);

    /// The `ControlPlaneClient` whose `openAccountStream()`/`m_streamNetwork` opens every attempt
    /// this object makes. Must be set before `start()`.
    void setControlPlane(ControlPlaneClient* controlPlane);

    /// Opens the first attempt and arms the paused-fallback timer. A second call while already
    /// running is a no-op (mirrors `SessionWebSocket::open`'s own idempotence).
    void start();
    /// Closes deliberately: aborts any open reply, stops every timer, and - unlike a `revoked`
    /// closing - never reconnects and never emits `revoked()`. `idle`, not `stopped`: this is not
    /// the server saying no, it is the caller saying stop.
    void stop();

    QString state() const { return m_state; }

    /// L3: 45 000 ms by default (`docs/spec/timing.md` L3). Only enforced once a hello has named
    /// `alive_ms` - see `feed()`'s `hello` handling.
    void setDeadStreamTimeoutMs(int ms);
    /// L5: 10 000 ms by default (`docs/spec/timing.md` L5) - not live this long shows the
    /// fallback indicator.
    void setPausedDelayMs(int ms);
    /// L4's `replaced` case: 30 000 ms by default (`docs/spec/timing.md` L4) - waits the cap
    /// rather than the ordinary schedule, so as not to fight the connection that just replaced
    /// this one.
    void setReconnectCapMs(int ms);
    /// Overrides the jitter term `reconnectDelayMs` reads by default (`QRandomGenerator`, `[0,
    /// 0.2]`) - the test seam for `reconnectDelayUsesTheScheduleWithJitter` and the paced-backoff
    /// tests above it.
    void setJitterProvider(std::function<double()> provider);

    /// `SessionWebSocket::reconnectDelayMs(attempt)` scaled by `1 + min(jitter, 0.2)` (L4/V15:
    /// "1 s doubling to a 30 s cap plus up to 20% jitter"). Pure, so the schedule is asserted
    /// without a socket - the reused schedule itself is `SessionWebSocket`'s (§1.2); this only
    /// adds the jitter term that schedule does not have.
    static int reconnectDelayMs(int attempt, double jitter);

signals:
    void stateChanged();
    /// L5's fallback indicator (D-04): `true` on the paused-delay's expiry while not live,
    /// `false` the moment the connection goes live again. Fires at most once per transition -
    /// `pausedAfterTheFallbackDelay` checks this, not just `state()`.
    void pausedChanged(bool paused);
    /// `stream.closing {reason: "revoked"}`, or a 401/403 answer to the stream request itself
    /// (§2.3: the same `handleCredentialRefused()` path a REST 401 already takes). Requests no
    /// further stream.
    void revoked();
    /// `stream.hello`. `aliveMs` is `alive_ms` when the hello named it, 0 when it did not (L3:
    /// the dead-stream timeout is enforced only when this is present).
    void hello(int aliveMs);
    /// `stream.alive` - the hub's own probe (ADR-0067 decision 2), distinct from the transport's
    /// `: ping` comment, which proves only TCP.
    void alive();
    /// `stream.resync` - the hub's LISTEN connection reconnected; every subscription's snapshot
    /// should be treated as dirty.
    void resync();
    /// `stream.closing`. `reason` is `revoked` | `shutdown` | `replaced`.
    void closing(const QString& reason);
    /// `account.state`, parsed by `AccountStateInfo::parse`.
    void accountState(const AccountStateInfo& account);
    /// `session.state`, parsed by the same `SessionInfo::parse` the REST routes use - one parser,
    /// so the two can never disagree.
    void sessionState(const SessionInfo& session);

private:
    void processLine(const QByteArray& line);
    void dispatchFrame();

    /// Starts (or restarts, on reconnect) one connection attempt: resets the per-attempt hello
    /// state, moves to `connecting`, opens the reply and arms the paused timer.
    void beginConnection();
    void openStream();
    void scheduleReconnect(int delayMs);
    void setState(const QString& state);
    void markLive();
    double jitterValue() const;

    void handleReadyRead();
    void handleFinished();
    void onHello(int aliveMs);
    void onAlive();
    void onClosing(const QString& reason);
    /// `resync`/`account.state`/`session.state` all do the same one thing here: restart the
    /// dead-stream timer when L3 enforces it. Connected to all three signals.
    void onNamedEvent();

    ControlPlaneClient* m_controlPlane = nullptr;
    QNetworkReply* m_reply = nullptr;

    QString m_state = QStringLiteral("idle");
    bool m_running = false;
    /// Set by `stop()` only - distinguishes a deliberate close (never reconnects, never emits
    /// `revoked()`) from every other way a connection ends.
    bool m_stopping = false;
    bool m_live = false;
    bool m_paused = false;
    /// True once the current attempt's `stream.hello` has named `alive_ms` - L3's dead-stream
    /// timeout is enforced only then, and clears at the start of every new attempt.
    bool m_helloNamedAliveMs = false;
    /// The `reason` from the most recent `stream.closing` frame on the current attempt, consumed
    /// (and cleared) by `handleFinished()` once the reply the server closed actually completes.
    QString m_lastCloseReason;
    /// Consecutive failed-to-stay-live attempts; feeds `reconnectDelayMs`'s exponential term and
    /// resets to 0 once a connection has stayed live 60 s (L4).
    int m_attempt = 0;

    QTimer* m_deadStreamTimer = nullptr;
    QTimer* m_pausedTimer = nullptr;
    QTimer* m_reconnectTimer = nullptr;
    /// Single-shot, armed on every `markLive()`: 60 s of continuous liveness resets `m_attempt`
    /// (L4: "back to 1 s after a connection that stayed live 60 s").
    QTimer* m_liveResetTimer = nullptr;

    int m_deadStreamTimeoutMs = 45000;
    int m_pausedDelayMs = 10000;
    int m_reconnectCapMs = 30000;
    std::function<double()> m_jitterProvider;

    static const int kLiveResetMs = 60000;

    /// Bytes of the line currently being assembled, across however many `feed()` calls it takes.
    QByteArray m_lineBuffer;
    /// True immediately after a bare `\r` was treated as a line terminator, so a `\n` that
    /// follows it - in this call or the next, since a chunk boundary can fall between them - is
    /// consumed as the second half of one CRLF pair rather than read as an empty line of its own.
    bool m_lastWasCR = false;
    /// The current frame's `event:` value. Cleared after every dispatch.
    QByteArray m_eventType;
    /// The current frame's `data:` lines, joined by `\n` (SSE's own multi-line `data` rule).
    /// Cleared after every dispatch.
    QByteArray m_dataBuffer;
    /// Whether at least one `data:` line has been seen for the frame in progress - a frame with
    /// no `data:` line at all dispatches nothing (SSE's own rule), distinct from a frame whose
    /// `data:` value is the empty string.
    bool m_hasData = false;

    /// T-06.4-46: SeatHub's own bound (mirrors the protocol crate's), not part of the SSE spec
    /// itself - a pending line this long is almost certainly a corrupt stream, not a real frame,
    /// and is dropped rather than let it grow unbounded.
    static const int kMaxLineBytes = 65536;
};
