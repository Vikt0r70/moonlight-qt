#pragma once

// D-17/Plan 30: a quality report that fails to reach the control plane is never lost to a
// dropped connection - and never sent under the wrong account. One small on-disk outbox, one
// JSON file per session, holds it until the next attempt names a token and an account to send
// it under.
//
// Where the file lives: `<directory>/<session_id>.json`, `directory` being a subfolder of
// whatever `TokenStore` is currently pointed at (`SeatHubClient` sets it from
// `m_tokenStore->directory()` before every call, so a test that isolates the token store - the
// existing `isolateStore()` pattern every `tst_facade_wiring.cpp` sign-in test already calls -
// isolates this outbox too, with no test file changed for this plan). The same per-user,
// per-machine scope a stored report's account tag needs to mean anything: a shared PC's other
// account never reads or sends another's report.
//
// The outcome an attempt is judged by (`classify()`), wherever it is attempted - the first try in
// `SeatHubClient::handleTeardownCompleted()`, or a later `drain()` retry:
//
//   * `ok` (200 - the only success code `POST .../quality` documents), or `409` (this session
//     never streamed - `docs/spec/openapi.yaml`; no retry changes that answer either, so it is
//     grouped with the successes rather than the refusals per this plan's own truth line) -
//     `Delivered`. Nothing to keep.
//   * `400`/`404`/`413`/`422` - `Refused`. No retry can change this answer. Dropped, with one
//     WARN line naming the session and the status.
//   * `401`/`403` - `AuthFailed`. Kept for the next attempt, and stops `drain()`'s own loop for
//     this call: the token just failed, and trying the rest of the outbox against it would fail
//     the same way. `drain()` additionally refuses to even start a second attempt under the same
//     `tokenGeneration` until a different one is passed - "stops sending until the next token is
//     set" without ever holding the real bearer token (D-35: it never leaves `ControlPlaneClient`;
//     `tokenGeneration` is an opaque caller-chosen id - `SeatHubClient` passes
//     `QString::number(m_authEpoch)` - not a credential).
//   * anything else (a transport failure, `statusCode == 0`, a `5xx`, `408`, `429`) - `Retryable`.
//     Kept, and `drain()`'s loop continues to the next file.
//
// `put()` is called only for `AuthFailed` and `Retryable` outcomes (Rule 2: a `401`/`403` is not
// in this plan's own literal write-trigger enumeration, but dropping a report that could still
// succeed once the credential is refreshed is the missing-critical-functionality this outbox
// exists to prevent - see `06.3-30-SUMMARY.md` Deviations).

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>

#include "control_plane_client.h"
#include "stream_stats.h"

class QualityOutbox
{
public:
    /// `Delivered`/`Refused`/`AuthFailed`/`Retryable` - see the header comment above for the
    /// exact status-code membership of each.
    enum class Outcome
    {
        Delivered,
        Refused,
        AuthFailed,
        Retryable,
        /// CR-03: the caller aborted this attempt (`ControlPlaneResult::wasAborted`) before or
        /// after issuing it, because the account it would have gone out under is no longer the
        /// signed-in one (a sign-out, or a sign-in as someone else, mid-drain). Kept, exactly like
        /// `AuthFailed`, and stops the rest of this call's loop the same way - but unlike
        /// `AuthFailed` it does NOT set `m_blockedTokenGeneration`: nothing about the credential
        /// itself failed, so a later `drain()` call under a fresh generation must not be blocked
        /// by this one's own captured (now-stale) generation.
        Aborted,
    };

    /// The rule every attempt (the first try and every `drain()` retry) is judged by. Exposed so
    /// both call it - the one place this outbox's policy is decided.
    static Outcome classify(const ControlPlaneResult& result);

    explicit QualityOutbox(const QString& directory = QString());

    /// Overrides the directory `put()`/`drain()`/`hasQueuedReports()` read and write. Exists for
    /// tests and so `SeatHubClient` can follow wherever `TokenStore` is currently pointed;
    /// production never has to call it explicitly (the constructor's default already resolves to
    /// the real per-user directory).
    void setDirectory(const QString& directory);
    QString directory() const { return m_directory; }

    /// `TokenStore::defaultDirectory() + "/quality-outbox"` - the same `QStandardPaths` root
    /// `TokenStore` resolves, so both land in the one real per-user SeatHub folder without this
    /// module owning a `TokenStore` instance of its own.
    static QString defaultDirectory();

    /// Writes (or overwrites) `<sessionId>.json`: the report body and `accountId`, the account
    /// this report belongs to. A no-op with no WARN for an empty `sessionId` (nothing to hold it
    /// against). Untagged reports (empty `accountId`) are stored ahead until `accountId` is known.
    void put(const QString& sessionId, const QString& accountId, const QJsonObject& report);

    /// Removes `<sessionId>.json` from disk, e.g. after delivery.
    void remove(const QString& sessionId);

    /// True when at least one report is currently stored, whichever account it is tagged for.
    /// Lets a caller that does not yet know the signed-in account's id (a fresh sign-in, before
    /// `GET /api/me` has ever run) decide whether asking for it is worth a request at all - every
    /// existing test's outbox directory is empty, so this returns false for every one of them.
    bool hasQueuedReports() const;

    /// Mirrors `ControlPlaneClient::postSessionQuality`'s own shape, so a caller's bound member
    /// function is a valid argument with no adapter; a test supplies a fake instead.
    using PostFn = std::function<void(const QString& sessionId, const QJsonObject& report,
                                      ControlPlaneClient::Callback callback)>;

    /// One send attempt per stored file tagged for `accountId`, oldest-first is not tracked -
    /// directory order is whatever `QDir` gives, name-sorted for determinism. A file tagged for
    /// another account is deleted with one WARN line and never reaches `postFn` at all.
    ///
    /// A no-op (nothing read, nothing sent) when `tokenGeneration` or `accountId` is empty -
    /// "nothing is sent before both a token and the signed-in account id are known" (D-17) - when
    /// a drain is already in flight (a second `drain()` call while a `postFn` callback has not
    /// yet returned does not re-send the same file), or when `tokenGeneration` is the same one an
    /// earlier call's `AuthFailed` outcome was refused under (see the header comment's `401`/`403`
    /// bullet).
    void drain(const QString& tokenGeneration, const QString& accountId, PostFn postFn);

    /// CR-03 (code review 06.3-REVIEW-fork.md): aborts whatever `drain()` call is currently in
    /// flight and resets `m_draining` immediately, so the very next `drain()` call - typically the
    /// next thing `SeatHubClient` does, a sign-out or a sign-in as someone else - is never
    /// silently swallowed by a stale in-flight state that may never resolve (the account it was
    /// draining for is no longer the signed-in one, so nothing says its reply will ever arrive, or
    /// arrive soon). Any `drainFiles()` callback still in flight from before this call becomes a
    /// no-op for its own bookkeeping - see `drainFiles()`'s own comment on the generation check -
    /// though a genuine server answer it already received (a 200, a 404, ...) still deletes or
    /// keeps the file on disk exactly as `classify()` says, since that reflects real server state
    /// regardless of which `drain()` call asked for it. Called from `SeatHubClient::signOut()` and
    /// the 401 branch of `applyRestoreResult()`.
    void cancel();

private:
    QString pathFor(const QString& sessionId) const;
    void drainFiles(const QStringList& files, int index, const QString& tokenGeneration,
                    const QString& accountId, const PostFn& postFn, quint64 generation);

    QString m_directory;
    bool m_draining = false;
    /// CR-03: bumped by every `drain()` call and by `cancel()`. Captured into each `drainFiles()`
    /// callback; a callback whose captured value no longer matches this one belongs to a
    /// superseded `drain()` (or one `cancel()` explicitly ended) and must not touch `m_draining`,
    /// `m_blockedTokenGeneration`, or continue that call's own loop - a plain `m_draining` guard
    /// alone cannot express "a NEWER drain() may proceed even though an OLDER one has not answered
    /// yet", which is exactly what a sign-out mid-drain, followed immediately by a sign-in as
    /// someone else, needs.
    quint64 m_drainGeneration = 0;
    /// The `tokenGeneration` an `AuthFailed` outcome was last refused under, or empty. Compared by
    /// value, not cleared on a successful `Delivered`/`Refused` elsewhere in the same generation -
    /// only a *different* `tokenGeneration` argument to `drain()` clears the block, which is
    /// exactly "the next token" (D-17).
    QString m_blockedTokenGeneration;
};

// ---------------------------------------------------------------- StreamTimeOutbox (HI06)

/// ADR-0074 HI06 (`docs/spec/client.md` § Reporting delivered stream time, and what a kill may
/// lose): the local record of the stream time the app measured (`DeliveredStreamTime`), kept so
/// that a kill loses at most the part of the open segment after its last checkpoint and so that a
/// report the control plane did not accept is sent again.
///
/// Where the file lives: `<directory>/<session_id>.json`, one per session, in a subfolder of
/// whatever `TokenStore` is currently pointed at (`SeatHubClient` sets it, exactly like the
/// quality outbox above, so a test that isolates the token store isolates this too). The file holds
/// the signed-in account's id and one entry per segment: `{segment_no, delivered_ms, closed}`.
///
/// A record is only ever sent to the session id it is named for and under the account it is tagged
/// with: `drain()` sends a file only when its tag equals the signed-in account, and leaves another
/// account's file exactly where it is (a shared PC's other customer keeps their record; it is
/// neither sent under this token nor deleted). Time is never added to a different session or
/// account, and a replayed entry adds nothing: the server stores the larger of the stored and the
/// reported `delivered_ms` for its (session, segment) key (W04), and this file never sums anything.
///
/// What each answer does to an entry (`classify()`):
///   * 200 - `Delivered`: the entry is on the server; it is removed (only while it still holds
///     what was sent, so a newer checkpoint written meanwhile survives).
///   * 404, 409, 422 - `Refused` (the three refusals W04 documents): the server will not take it (404
///     another account's or no such session, 409 the session never streamed, 422 a malformed body);
///     dropped.
///   * 401, 403 - `AuthFailed`: kept; `drain()` stops and does not retry the same token generation.
///   * anything else (no answer, 5xx, 408, 429) - `Retryable`: kept for the next beat, the next
///     teardown or the next launch.
class StreamTimeOutbox
{
public:
    enum class Outcome
    {
        Delivered,
        Refused,
        AuthFailed,
        Retryable,
        /// The caller abandoned the attempt before or after issuing it because the account it
        /// would have gone out under is no longer the signed-in one. Kept, and stops the drain.
        Aborted,
    };

    static Outcome classify(const ControlPlaneResult& result);

    explicit StreamTimeOutbox(const QString& directory = QString());

    /// `<AppDataLocation>/stream-time-outbox`, beside the quality outbox's own folder.
    static QString defaultDirectory();
    void setDirectory(const QString& directory);
    QString directory() const;

    /// Writes the segment into `<sessionId>.json`, tagged `accountId`. A no-op for a session id
    /// that is not a UUID or an empty account (an untagged record could never be proven to belong
    /// to anyone). An entry for the same segment number is merged, never added to: an open entry
    /// takes the larger of the two values; a closed value replaces an open one, even a smaller
    /// one (the end of the segment is authoritative, and the reduction for a lost connection can
    /// make it smaller than the last checkpoint); a closed entry stays closed.
    void put(const QString& sessionId, const QString& accountId, const StreamSegmentReport& segment);

    /// The entries of `sessionId`'s file when it is tagged `accountId`, ordered by segment number;
    /// empty otherwise.
    QVector<StreamSegmentReport> entries(const QString& sessionId, const QString& accountId) const;

    /// Removes the entry for `segment.segmentNo` only if it still holds exactly the value and the
    /// closed flag that were sent, and the file when it was the last entry.
    void removeIfUnchanged(const QString& sessionId, const StreamSegmentReport& segment);

    /// True when at least one record file exists, whichever account it is tagged for.
    bool hasRecords() const;

    /// Mirrors `ControlPlaneClient::postStreamTime`'s shape, so a bound call is a valid argument
    /// with no adapter; a test supplies a fake instead.
    using PostFn = std::function<void(const QString& sessionId, const StreamSegmentReport& segment,
                                      ControlPlaneClient::Callback callback)>;

    /// Sends every entry of every file tagged `accountId`, one at a time, name-sorted, skipping the
    /// file named `skipSessionId` (the live session's own file belongs to the beat, not to this
    /// pass). A no-op when `tokenGeneration` or `accountId` is empty, when a drain is already in
    /// flight, or when `tokenGeneration` is the one an earlier call's `AuthFailed` was refused
    /// under. A file that is not a record this outbox wrote is deleted.
    void drain(const QString& tokenGeneration, const QString& accountId,
               const QString& skipSessionId, PostFn postFn);

    /// Aborts whatever `drain()` is in flight (a sign-out, or a sign-in as someone else), the same
    /// way `QualityOutbox::cancel()` does.
    void cancel();

private:
    struct Task
    {
        QString sessionId;
        StreamSegmentReport segment;
    };

    void drainTasks(const QVector<Task>& tasks, int index, const QString& tokenGeneration,
                    const PostFn& postFn, quint64 generation);

    QString m_directory;
    bool m_draining = false;
    quint64 m_drainGeneration = 0;
    QString m_blockedTokenGeneration;
};

/// One checkpoint of the delivered stream time (ADR-0074 HI06): for the segments `clock` has not
/// yet settled - an open one at its running time, a closed one at its final value - writes the
/// local record (when the account is known) and then sends each through `post`. Called at every
/// client liveness beat, when a segment closes and at teardown; calling it more often only repeats
/// reports the server stores once.
///
/// A reply is judged against the session it was sent for, never against whichever one is current
/// when it lands: a delivered or refused entry leaves the record (only while it still holds what
/// was sent), a closed segment that was delivered or refused is settled and not sent again, a 409
/// ("this session never streamed", still in its first seconds) on an open segment is retried at the
/// next checkpoint, and a refusal of the session itself (404, 422) stops the reports for it.
void checkpointStreamTime(const std::shared_ptr<DeliveredStreamTime>& clock,
                          const std::shared_ptr<StreamTimeOutbox>& outbox,
                          StreamTimeOutbox::PostFn post);
