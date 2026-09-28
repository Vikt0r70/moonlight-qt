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

#include "control_plane_client.h"

class QJsonObject;

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

/// One connection's frame reader. Owns no socket and no `QNetworkAccessManager` of its own;
/// `feed()` is handed raw bytes by whatever opened the stream
/// (`ControlPlaneClient::openAccountStream`, Task 2).
class SseClient : public QObject
{
    Q_OBJECT

public:
    explicit SseClient(QObject* parent = nullptr);

    /// Raw bytes from the wire, in any chunking - a whole frame at once, or one byte at a time
    /// (`splitsFramesAtAnyByte`). Reassembles lines across calls (CR, LF or CRLF, split anywhere),
    /// ignores comment lines (`: ...`) and field names this build does not recognise, and
    /// dispatches one frame per blank line - SSE's own additive contract: an event type this
    /// build has no case for is a silent no-op, never an error.
    void feed(const QByteArray& chunk);

signals:
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
