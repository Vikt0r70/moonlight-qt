#include "stream_record.h"

#include <QJsonArray>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>

#include "evidence_journal.h"

namespace StreamRecord {

QString warningCodeName(WarningCode code)
{
    switch (code) {
    case WarningCode::PoorConnection:
        return QStringLiteral("poor_connection");
    case WarningCode::ConnectionRecovered:
        return QStringLiteral("connection_recovered");
    case WarningCode::DecoderReset:
        return QStringLiteral("decoder_reset");
    case WarningCode::AudioUnavailable:
        return QStringLiteral("audio_unavailable");
    case WarningCode::Stall:
        return QStringLiteral("stall");
    case WarningCode::Other:
        break;
    }
    return QStringLiteral("other");
}

// The engine strings are the ones the pinned engine writes (app/streaming/session.cpp and
// video/ffmpeg.cpp at seathub-v0.1.35). The text is matched and forgotten.
WarningCode warningCodeForLine(const QString& text)
{
    static const QRegularExpression status(QStringLiteral("Connection status update: (-?\\d+)"));
    const QRegularExpressionMatch m = status.match(text);
    if (m.hasMatch()) {
        const int value = m.captured(1).toInt();
        if (value == 1) {
            return WarningCode::PoorConnection;
        }
        if (value == 0) {
            return WarningCode::ConnectionRecovered;
        }
        return WarningCode::Other;
    }
    if (text.contains(QLatin1String("Resetting decoder"))) {
        return WarningCode::DecoderReset;
    }
    if (text.contains(QLatin1String("Failed to open audio device"))
        || text.contains(QLatin1String("Audio will be unavailable"))) {
        return WarningCode::AudioUnavailable;
    }
    return WarningCode::Other;
}

QString endReasonName(EndReason reason)
{
    switch (reason) {
    case EndReason::ConnectionTerminated:
        return QStringLiteral("connection_terminated");
    case EndReason::EngineFailure:
        return QStringLiteral("engine_failure");
    case EndReason::StallTimeout:
        return QStringLiteral("stall_timeout");
    case EndReason::DecoderFailure:
        return QStringLiteral("decoder_failure");
    case EndReason::Other:
        break;
    }
    return QStringLiteral("other");
}

EndReason endReasonForClass(const QString& failureClass, bool engineFailed)
{
    if (failureClass == QLatin1String("no_video_traffic") || failureClass == QLatin1String("no_video_frame")) {
        return EndReason::StallTimeout;
    }
    if (failureClass == QLatin1String("frame_conversion") || failureClass == QLatin1String("protected_content")) {
        return EndReason::DecoderFailure;
    }
    if (failureClass == QLatin1String("early_termination")) {
        return EndReason::ConnectionTerminated;
    }
    if (engineFailed) {
        return EndReason::EngineFailure;
    }
    return failureClass == QLatin1String("net_other") ? EndReason::ConnectionTerminated : EndReason::Other;
}

} // namespace StreamRecord

namespace {

double clampTo(double v, double lo, double hi)
{
    if (!std::isfinite(v)) {
        return lo;
    }
    return std::max(lo, std::min(hi, v));
}

double round2(double v)
{
    return std::round(v * 100.0) / 100.0;
}

// Nearest-rank percentile of a non-empty, sorted list.
double percentile(const QVector<double>& sorted, double p)
{
    const int n = sorted.size();
    const int rank = std::max(1, static_cast<int>(std::ceil(p * n)));
    return sorted.at(std::min(rank, n) - 1);
}

} // namespace

StreamRecorder::StreamRecorder(EvidenceJournal* journal) : m_journal(journal)
{
    m_ring.resize(kTailSeconds);
}

void StreamRecorder::resetMinute()
{
    m_samples.clear();
    m_sampleSeconds.clear();
    m_warnings.clear();
    m_warningsDropped = 0;
    m_poorFlags = 0;
}

void StreamRecorder::begin(qint64 monoMs, const QDateTime& serverNow)
{
    QMutexLocker lock(&m_mutex);
    m_active = true;
    m_finished = false;
    m_beginMono = monoMs;
    m_serverBase = serverNow.toUTC();
    m_overlay = StreamRecord::Overlay::Unknown;
    m_minuteIndex = 0;
    m_minuteSeq = 0;
    m_endedAtMono = -1;
    m_tailWritten = false;
    m_ring.fill(Second());
    resetMinute();
}

void StreamRecorder::setOverlay(StreamRecord::Overlay overlay)
{
    QMutexLocker lock(&m_mutex);
    m_overlay = overlay;
}

void StreamRecorder::setMinuteMsForTests(qint64 ms)
{
    QMutexLocker lock(&m_mutex);
    m_minuteMs = std::max<qint64>(1, ms);
}

QDateTime StreamRecorder::serverTimeAt(qint64 monoMs) const
{
    return m_serverBase.addMSecs(monoMs - m_beginMono);
}

QString StreamRecorder::runId() const
{
    return m_journal != nullptr ? m_journal->runId() : QString();
}

void StreamRecorder::advanceLocked(qint64 monoMs)
{
    while (m_active && !m_finished && monoMs >= m_beginMono + (m_minuteIndex + 1) * m_minuteMs) {
        closeMinute(false, m_beginMono + (m_minuteIndex + 1) * m_minuteMs);
    }
}

void StreamRecorder::advance(qint64 monoMs)
{
    QMutexLocker lock(&m_mutex);
    advanceLocked(monoMs);
}

void StreamRecorder::addSample(qint64 monoMs, const VideoStats& stats)
{
    QMutexLocker lock(&m_mutex);
    if (!m_active || m_finished || monoMs < m_beginMono) {
        return;
    }
    advanceLocked(monoMs);
    const qint64 second = (monoMs - m_beginMono) / 1000;
    m_samples.append(stats);
    m_sampleSeconds.append(second);

    // The ring keeps the latest reading of each second.
    Second& slot = m_ring[static_cast<int>(second % kTailSeconds)];
    slot.index = second;
    slot.fps = stats.renderedFps.present ? clampTo(stats.renderedFps.value, 0, 1000) : 0;
    slot.lossPct = stats.networkDroppedFramePct.present ? clampTo(stats.networkDroppedFramePct.value, 0, 100) : 0;
    slot.delayMs = stats.rttMs.present ? static_cast<int>(std::lround(std::max(0.0, stats.rttMs.value))) : 0;
}

void StreamRecorder::addWarning(qint64 monoMs, StreamRecord::WarningCode code)
{
    QMutexLocker lock(&m_mutex);
    if (!m_active || m_finished || monoMs < m_beginMono) {
        return;
    }
    advanceLocked(monoMs);
    if (code == StreamRecord::WarningCode::PoorConnection) {
        ++m_poorFlags;
    }
    if (m_warnings.size() >= kMaxWarnings) {
        ++m_warningsDropped; // counted, never stored (N27, E38)
        return;
    }
    const qint64 windowStart = m_beginMono + m_minuteIndex * m_minuteMs;
    Warning warning;
    warning.offsetS = static_cast<int>(std::max<qint64>(0, std::min<qint64>(59, (monoMs - windowStart) / 1000)));
    warning.code = static_cast<int>(code);
    m_warnings.append(warning);
}

void StreamRecorder::finish(qint64 monoMs)
{
    QMutexLocker lock(&m_mutex);
    if (!m_active || m_finished) {
        return;
    }
    advanceLocked(monoMs);
    const qint64 windowStart = m_beginMono + m_minuteIndex * m_minuteMs;
    if (monoMs > windowStart) {
        closeMinute(true, monoMs);
    }
    m_finished = true;
}

bool StreamRecorder::ensureRoom(int entries)
{
    if (m_journal == nullptr || !m_journal->isOpen()) {
        return false;
    }
    if (m_journal->entryCap() - m_journal->entryCount() >= entries) {
        return true;
    }
    // ADR-0117: close this capsule (it stays on disk for delivery) and carry on under a new run_id.
    if (!m_journal->rollRun()) {
        return false;
    }
    m_minuteSeq = 0;
    return true;
}

bool StreamRecorder::recordEntry(QJsonObject entry)
{
    if (m_journal == nullptr || !m_journal->isOpen()) {
        return false;
    }
    entry.insert(QStringLiteral("run_id"), m_journal->runId());
    const int before = m_journal->entryCount();
    m_journal->record(entry);
    return m_journal->entryCount() > before;
}

void StreamRecorder::closeMinute(bool partial, qint64 endMonoMs)
{
    const qint64 windowStart = m_beginMono + m_minuteIndex * m_minuteMs;
    const qint64 lengthMs = partial ? std::max<qint64>(0, endMonoMs - windowStart) : m_minuteMs;

    QJsonObject entry;
    entry.insert(QStringLiteral("kind"), QStringLiteral("stream_minute"));
    entry.insert(QStringLiteral("window_start_server_time"),
                 serverTimeAt(windowStart).toString(Qt::ISODateWithMs));
    const int count = m_samples.size();
    entry.insert(QStringLiteral("sample_count"), count);

    // Coverage: the share of the window's seconds that produced at least one sample.
    const qint64 windowSeconds = std::max<qint64>(1, (lengthMs + 999) / 1000);
    const QSet<qint64> covered(m_sampleSeconds.cbegin(), m_sampleSeconds.cend());
    entry.insert(QStringLiteral("coverage_pct"),
                 round2(clampTo(100.0 * covered.size() / static_cast<double>(windowSeconds), 0, 100)));
    entry.insert(QStringLiteral("poor_connection_flags"), m_poorFlags);
    entry.insert(QStringLiteral("overlay_visible"),
                 m_overlay == StreamRecord::Overlay::Visible
                     ? QStringLiteral("true")
                     : (m_overlay == StreamRecord::Overlay::Hidden ? QStringLiteral("false")
                                                                   : QStringLiteral("unknown")));

    // Ordered by offset then code text: the order the server checks.
    QVector<Warning> ordered = m_warnings;
    std::stable_sort(ordered.begin(), ordered.end(), [](const Warning& a, const Warning& b) {
        if (a.offsetS != b.offsetS) {
            return a.offsetS < b.offsetS;
        }
        return StreamRecord::warningCodeName(static_cast<StreamRecord::WarningCode>(a.code))
            < StreamRecord::warningCodeName(static_cast<StreamRecord::WarningCode>(b.code));
    });
    QJsonArray warnings;
    for (const Warning& w : ordered) {
        QJsonObject o;
        o.insert(QStringLiteral("code"), StreamRecord::warningCodeName(static_cast<StreamRecord::WarningCode>(w.code)));
        o.insert(QStringLiteral("offset_s"), w.offsetS);
        warnings.append(o);
    }
    entry.insert(QStringLiteral("warnings"), warnings);
    entry.insert(QStringLiteral("warnings_dropped"), m_warningsDropped);

    // The sixteen readings exist only for a minute that had samples, and are omitted - never null -
    // otherwise (ADR-0117). A metric no sample carried reads 0.
    if (count > 0) {
        QVector<double> fps, loss, jitter, rtt, decode;
        for (const VideoStats& s : m_samples) {
            if (s.renderedFps.present) fps.append(clampTo(s.renderedFps.value, 0, 1000));
            if (s.networkDroppedFramePct.present) loss.append(clampTo(s.networkDroppedFramePct.value, 0, 100));
            if (s.jitterDroppedFramePct.present) jitter.append(clampTo(s.jitterDroppedFramePct.value, 0, 100));
            if (s.rttMs.present) rtt.append(std::max(0.0, s.rttMs.value));
            if (s.decodeTimeMs.present) decode.append(std::max(0.0, s.decodeTimeMs.value));
        }
        auto mean = [](const QVector<double>& v) {
            if (v.isEmpty()) return 0.0;
            double sum = 0;
            for (double x : v) sum += x;
            return sum / v.size();
        };
        auto pct = [](QVector<double> v, double p) {
            if (v.isEmpty()) return qint64(0);
            std::sort(v.begin(), v.end());
            return static_cast<qint64>(std::llround(percentile(v, p)));
        };
        // The same five families also carry their MIN and MAX (T088, contract 3.37.0): taken over
        // the samples this minute holds, rounded and bounded like the averages beside them.
        auto low = [](const QVector<double>& v) {
            if (v.isEmpty()) return 0.0;
            double best = v.first();
            for (double x : v) best = qMin(best, x);
            return best;
        };
        auto high = [](const QVector<double>& v) {
            if (v.isEmpty()) return 0.0;
            double best = v.first();
            for (double x : v) best = qMax(best, x);
            return best;
        };
        entry.insert(QStringLiteral("fps_avg"), round2(mean(fps)));
        entry.insert(QStringLiteral("network_loss_pct"), round2(mean(loss)));
        entry.insert(QStringLiteral("jitter_drop_pct"), round2(mean(jitter)));
        entry.insert(QStringLiteral("delay_ms_p50"), pct(rtt, 0.50));
        entry.insert(QStringLiteral("delay_ms_p95"), pct(rtt, 0.95));
        entry.insert(QStringLiteral("decode_ms_p50"), pct(decode, 0.50));
        entry.insert(QStringLiteral("fps_min"), round2(low(fps)));
        entry.insert(QStringLiteral("fps_max"), round2(high(fps)));
        entry.insert(QStringLiteral("network_loss_pct_min"), round2(low(loss)));
        entry.insert(QStringLiteral("network_loss_pct_max"), round2(high(loss)));
        entry.insert(QStringLiteral("jitter_drop_pct_min"), round2(low(jitter)));
        entry.insert(QStringLiteral("jitter_drop_pct_max"), round2(high(jitter)));
        entry.insert(QStringLiteral("delay_ms_min"), static_cast<qint64>(std::llround(low(rtt))));
        entry.insert(QStringLiteral("delay_ms_max"), static_cast<qint64>(std::llround(high(rtt))));
        entry.insert(QStringLiteral("decode_ms_min"), static_cast<qint64>(std::llround(low(decode))));
        entry.insert(QStringLiteral("decode_ms_max"), static_cast<qint64>(std::llround(high(decode))));
    }

    // Room for this minute and, behind it, the closing entries (tail, drop check) (E36).
    if (ensureRoom(1 + kClosingReserve)) {
        entry.insert(QStringLiteral("minute_seq"), static_cast<int>(m_minuteSeq));
        if (recordEntry(entry)) {
            ++m_minuteSeq;
        }
    }
    ++m_minuteIndex;
    resetMinute();
}

bool StreamRecorder::writeTail(qint64 monoMs, StreamRecord::EndReason reason)
{
    QMutexLocker lock(&m_mutex);
    if (!m_active || m_tailWritten) {
        return false;
    }
    m_endedAtMono = monoMs;

    // d seconds of Play owe min(30, ceil(d)) distinct offsets 0..n-1, 0 being the last second
    // (T049 tail_window / tail_offsets_match). A second with no sample is written as zeros;
    // an offset is never repeated to fill the count.
    const double durationS = std::max<qint64>(0, monoMs - m_beginMono) / 1000.0;
    const qint64 playedSeconds = static_cast<qint64>(std::ceil(durationS));
    const int owed = static_cast<int>(std::min<qint64>(kTailSeconds, playedSeconds));
    const qint64 lastSecond = playedSeconds - 1;

    QJsonArray seconds;
    for (int offset = 0; offset < owed; ++offset) {
        const qint64 index = lastSecond - offset;
        const Second& slot = m_ring[static_cast<int>(index % kTailSeconds)];
        const bool have = slot.index == index;
        QJsonObject o;
        o.insert(QStringLiteral("offset_s"), offset);
        o.insert(QStringLiteral("fps"), have ? round2(slot.fps) : 0.0);
        o.insert(QStringLiteral("network_loss_pct"), have ? round2(slot.lossPct) : 0.0);
        o.insert(QStringLiteral("delay_ms"), have ? slot.delayMs : 0);
        seconds.append(o);
    }

    QJsonObject entry;
    entry.insert(QStringLiteral("kind"), QStringLiteral("stream_tail"));
    entry.insert(QStringLiteral("end_reason"), StreamRecord::endReasonName(reason));
    entry.insert(QStringLiteral("ended_at_server_time"), serverTimeAt(monoMs).toString(Qt::ISODateWithMs));
    entry.insert(QStringLiteral("seconds"), seconds);

    // The tail and the drop check that follows it share one capsule: keep room for both.
    if (!ensureRoom(2) || !recordEntry(entry)) {
        return false;
    }
    m_tailWritten = true;
    return true;
}

bool StreamRecorder::writeDropCheck(qint64 checkedAtMono, const DropCheckResult& result)
{
    QMutexLocker lock(&m_mutex);
    if (!m_active || !m_tailWritten) {
        return false;
    }
    // Never earlier than the bad end it checks.
    const qint64 at = std::max(checkedAtMono, m_endedAtMono);
    QJsonObject entry;
    entry.insert(QStringLiteral("kind"), QStringLiteral("drop_check"));
    entry.insert(QStringLiteral("server_reachable"), result.serverReachable);
    entry.insert(QStringLiteral("server_ms"), std::max(0, result.serverMs));
    entry.insert(QStringLiteral("rig_port_open"), result.rigPortOpen);
    entry.insert(QStringLiteral("rig_ms"), std::max(0, result.rigMs));
    entry.insert(QStringLiteral("checked_at_server_time"), serverTimeAt(at).toString(Qt::ISODateWithMs));
    return ensureRoom(1) && recordEntry(entry);
}
