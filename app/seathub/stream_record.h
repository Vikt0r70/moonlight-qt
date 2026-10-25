#pragma once

// StreamRecorder (T054, SC23/SC24, ADR-0117, docs/spec/client.md "Stream record"): the client half
// of the per-Play stream record. It turns the same stats samples the quality sampler reads into
// one `stream_minute` evidence entry per minute of the Play, keeps the last 30 seconds in a ring,
// and on a bad end writes the `stream_tail` and the `drop_check` into the same capsule sequence.
//
// Pure logic, no network, no GUI, no SDL: time is passed in as monotonic milliseconds, so the
// class is tested without waiting a minute (S3). It writes only through `EvidenceJournal::record`,
// the one front door to a capsule (S1, S5), and every value it writes is a number, a boolean, a
// UUID, an instant or a member of a closed set - never a log line, address or host name.
//
// Server time basis: `server_time = local_now + offset`, the offset `SeatHubClient` already keeps
// from `stream.hello`; the facade passes the server instant of `begin()` and the recorder adds the
// elapsed monotonic time to it. No new clock exchange.
//
// Rollover (ADR-0117): a capsule holds 256 entries. Before a minute is written the recorder rolls
// the journal onto a new `run_id` when fewer than `kClosingReserve` + 1 entries are free, so the
// closing entries (tail, drop check) always have room. `minute_seq` counts from 0 inside each
// `run_id`; the window start keeps rising across runs, so `(run_id, minute_seq)` names a minute.

#include <QDateTime>
#include <QJsonObject>
#include <QMutex>
#include <QString>
#include <QVector>

#include "stream_stats.h"

class EvidenceJournal;

namespace StreamRecord {

/// The closed `StreamWarningCode` table (docs/spec/openapi.yaml), `other` last.
enum class WarningCode { PoorConnection, ConnectionRecovered, DecoderReset, AudioUnavailable, Stall, Other };

QString warningCodeName(WarningCode code);

/// Maps one engine log line to a code. A line that is not recognised is `Other`; the text is read
/// and then forgotten - it is never stored or shipped.
WarningCode warningCodeForLine(const QString& text);

/// The closed `StreamEndReason` table.
enum class EndReason { ConnectionTerminated, EngineFailure, StallTimeout, DecoderFailure, Other };

QString endReasonName(EndReason reason);

/// From the Play's closed failure class (`attempt_vocab` `classForStreamError`) and whether the
/// engine reported a stage failure.
EndReason endReasonForClass(const QString& failureClass, bool engineFailed);

/// Whether the in-stream statistics overlay was on screen. The client reads it from the engine's
/// own preference flag; `Unknown` when it cannot tell.
enum class Overlay { Unknown, Hidden, Visible };

} // namespace StreamRecord

class StreamRecorder
{
public:
    /// Entries kept free in a capsule for the closing entries (stream_tail, drop_check).
    static constexpr int kClosingReserve = 2;
    static constexpr int kMaxWarnings = 16;
    static constexpr int kTailSeconds = 30;
    static constexpr qint64 kMinuteMs = 60000;

    /// The result of the one reachability check (`drop_check.h` produces it).
    struct DropCheckResult {
        bool serverReachable = false;
        int serverMs = 0;
        bool rigPortOpen = false;
        int rigMs = 0;
    };

    explicit StreamRecorder(EvidenceJournal* journal);

    /// Start recording a Play. `monoMs` is any monotonic reading; `serverNow` is the same instant
    /// on the server's time basis. Resets every minute and ring.
    void begin(qint64 monoMs, const QDateTime& serverNow);
    bool active() const { return m_active; }

    void setOverlay(StreamRecord::Overlay overlay);
    /// A minute shorter than 60 s, so a facade test sees several rows without waiting. Production
    /// never calls it.
    void setMinuteMsForTests(qint64 ms);

    /// One statistics sample.
    void addSample(qint64 monoMs, const VideoStats& stats);
    /// One warning, at its offset inside the current minute.
    void addWarning(qint64 monoMs, StreamRecord::WarningCode code);
    /// Closes every whole minute that has ended by `monoMs`, writing a minute with no samples when
    /// nothing arrived (E37).
    void advance(qint64 monoMs);
    /// Writes the last, partial minute. Idempotent; nothing is written for a Play shorter than 1 ms.
    void finish(qint64 monoMs);

    /// Bad end: writes the `stream_tail` anchored at `monoMs` (the first local bad-end signal).
    /// Returns false when nothing was written (no Play, or the entry was refused).
    bool writeTail(qint64 monoMs, StreamRecord::EndReason reason);
    /// Writes the `drop_check` for this Play, after its tail. `checkedAtMono` is when the check
    /// finished; the instant is never earlier than the tail's `ended_at_server_time`. False when no
    /// tail was written (a drop check only follows a bad end) or the journal is closed.
    bool writeDropCheck(qint64 checkedAtMono, const DropCheckResult& result);

    /// The server instant of a monotonic reading.
    QDateTime serverTimeAt(qint64 monoMs) const;

    /// The `run_id` of the capsule this Play's entries currently go to (empty before `begin`).
    QString runId() const;

private:
    struct Second {
        qint64 index = -1; // seconds since begin; -1 = empty slot
        double fps = 0;
        double lossPct = 0;
        int delayMs = 0;
    };
    struct Warning {
        int offsetS = 0;
        int code = 0;
    };

    // Helpers; the caller holds m_mutex.
    void advanceLocked(qint64 monoMs);
    void closeMinute(bool partial, qint64 endMonoMs);
    bool ensureRoom(int entries);
    bool recordEntry(QJsonObject entry);
    void resetMinute();

    // The render thread (stats tap), the network thread (sampler windows, LogTee sink) and the
    // facade's thread (the end of the Play) all feed one recorder, so every public call locks.
    mutable QMutex m_mutex;
    qint64 m_minuteMs = kMinuteMs;
    EvidenceJournal* m_journal;
    bool m_active = false;
    bool m_finished = false;
    qint64 m_beginMono = 0;
    QDateTime m_serverBase;
    StreamRecord::Overlay m_overlay = StreamRecord::Overlay::Unknown;

    // The minute being collected.
    qint64 m_minuteIndex = 0;   // global, since begin
    qint64 m_minuteSeq = 0;     // inside the current run_id
    QVector<VideoStats> m_samples;
    QVector<qint64> m_sampleSeconds;
    QVector<Warning> m_warnings;
    int m_warningsDropped = 0;
    int m_poorFlags = 0;

    QVector<Second> m_ring;
    qint64 m_endedAtMono = -1;
    bool m_tailWritten = false;
};
