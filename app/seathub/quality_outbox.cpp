#include "quality_outbox.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

Q_LOGGING_CATEGORY(seathubQualityOutbox, "seathub.quality_outbox")

namespace {

const char* kAccountIdKey = "account_id";
const char* kReportKey = "report";
// ADR-0101 / T024: Quality report payload schema carries nullable received_fps, decoded_fps,
// host_latency_min_ms, host_latency_max_ms, host_latency_avg_ms, and rtt_ms rounded half-up.

// Session ids are UUIDs (`docs/spec/schema.sql`, every `sessions.id` column). Checked before a
// session id is ever turned into a filename or a directory entry, so a malformed or hostile
// value (a `..`, a `/`) can neither escape the outbox directory nor be mistaken for one of this
// module's own files.
bool isUuidShaped(const QString& value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
                       "[0-9a-fA-F]{12}$"));
    return pattern.match(value).hasMatch();
}

} // namespace

QString QualityOutbox::defaultDirectory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("quality-outbox"));
}

QualityOutbox::QualityOutbox(const QString& directory)
    : m_directory(directory.isEmpty() ? defaultDirectory() : directory)
{
}

void QualityOutbox::setDirectory(const QString& directory)
{
    m_directory = directory;
}

QString QualityOutbox::pathFor(const QString& sessionId) const
{
    return QDir(m_directory).filePath(sessionId + QStringLiteral(".json"));
}

QualityOutbox::Outcome QualityOutbox::classify(const ControlPlaneResult& result)
{
    // CR-03: checked first - an aborted attempt never reached the network at all, so `ok` and
    // `statusCode` are both still their unset defaults and must not be read as "a transport
    // failure" (which `Outcome::Retryable`'s own default case below would otherwise call it).
    if (result.wasAborted) {
        return Outcome::Aborted;
    }
    // `ok` is the only success this outbox trusts (Pitfall 4 - a 200 carrying `status:false` is
    // already `ok == false` by the time it reaches here); 409 ("this session never streamed",
    // `docs/spec/openapi.yaml`) is grouped with it per this plan's own truth line - no retry ever
    // changes either answer.
    if (result.ok || result.statusCode == 409) {
        return Outcome::Delivered;
    }
    if (result.statusCode == 401 || result.statusCode == 403) {
        return Outcome::AuthFailed;
    }
    switch (result.statusCode) {
    case 400:
    case 404:
    case 413:
    case 422:
        return Outcome::Refused;
    default:
        // 0 (a transport failure), 5xx, 408, 429, or any other status this route does not
        // document: kept, in case a later attempt succeeds.
        return Outcome::Retryable;
    }
}

void QualityOutbox::put(const QString& sessionId, const QString& accountId, const QJsonObject& report)
{
    if (sessionId.isEmpty() || !isUuidShaped(sessionId)) {
        qCWarning(seathubQualityOutbox) << "refusing to store a quality report for a session id "
                                           "that is not a UUID";
        return;
    }

    QJsonObject envelope;
    envelope.insert(QLatin1String(kAccountIdKey), accountId);
    envelope.insert(QLatin1String(kReportKey), report);
    const QByteArray bytes = QJsonDocument(envelope).toJson(QJsonDocument::Compact);

    QDir().mkpath(m_directory);

    // QSaveFile: a crash mid-write leaves the previous copy (or nothing) rather than a truncated
    // report the next `drain()` would refuse to parse.
    QSaveFile file(pathFor(sessionId));
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        qCWarning(seathubQualityOutbox) << "could not write the quality outbox for" << sessionId;
    }
}

void QualityOutbox::remove(const QString& sessionId)
{
    if (sessionId.isEmpty() || !isUuidShaped(sessionId)) {
        return;
    }
    QFile::remove(pathFor(sessionId));
}

bool QualityOutbox::hasQueuedReports() const
{
    return !QDir(m_directory).entryList({ QStringLiteral("*.json") }, QDir::Files).isEmpty();
}

void QualityOutbox::drain(const QString& tokenGeneration, const QString& accountId, PostFn postFn)
{
    // D-17: nothing is sent before both a token and the signed-in account id are known.
    if (tokenGeneration.isEmpty() || accountId.isEmpty() || !postFn) {
        return;
    }
    // "Stops sending until the next token is set": the same generation that already drew a
    // 401/403 is not retried.
    if (!m_blockedTokenGeneration.isEmpty() && m_blockedTokenGeneration == tokenGeneration) {
        return;
    }
    // A drain already in flight (a `postFn` callback has not yet returned) must not start a
    // second pass over the same files - unless `cancel()` has already reset it (CR-03: a sign-out
    // or a sign-in as someone else calls `cancel()` first, precisely so this guard does not
    // silently swallow the new account's own drain).
    if (m_draining) {
        return;
    }

    QDir dir(m_directory);
    const QStringList files = dir.entryList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name);
    if (files.isEmpty()) {
        return;
    }

    m_draining = true;
    const quint64 generation = ++m_drainGeneration;
    drainFiles(files, 0, tokenGeneration, accountId, postFn, generation);
}

void QualityOutbox::cancel()
{
    ++m_drainGeneration;
    m_draining = false;
}

void QualityOutbox::drainFiles(const QStringList& files, int index, const QString& tokenGeneration,
                               const QString& accountId, const PostFn& postFn, quint64 generation)
{
    if (index >= files.size()) {
        if (generation == m_drainGeneration) {
            m_draining = false;
        }
        return;
    }

    const QDir dir(m_directory);
    const QString fileName = files.at(index);
    const QString path = dir.filePath(fileName);
    const QString sessionId = QFileInfo(fileName).completeBaseName();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        // Could not even read it (removed from under us, or a permissions error): leave it and
        // move on - nothing was sent, and nothing was lost.
        drainFiles(files, index + 1, tokenGeneration, accountId, postFn, generation);
        return;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        // Not a report this outbox wrote - never trusted, never sent.
        QFile::remove(path);
        drainFiles(files, index + 1, tokenGeneration, accountId, postFn, generation);
        return;
    }
    const QJsonObject envelope = doc.object();
    const QString storedAccountId = envelope.value(QLatin1String(kAccountIdKey)).toString();
    const QJsonObject report = envelope.value(QLatin1String(kReportKey)).toObject();

    if (storedAccountId != accountId) {
        qCWarning(seathubQualityOutbox)
            << "quality report for" << sessionId << "belongs to another account; dropped";
        QFile::remove(path);
        drainFiles(files, index + 1, tokenGeneration, accountId, postFn, generation);
        return;
    }

    postFn(sessionId, report,
           [this, files, index, tokenGeneration, accountId, postFn, path, sessionId,
            generation](const ControlPlaneResult& result) {
               const Outcome outcome = classify(result);
               // File-side effects reflect real server truth (or a real local decision, like an
               // aborted send) whatever `drain()` call asked for them, so they always run -
               // CR-03's generation check below gates only this call's own loop continuation and
               // its `m_draining`/`m_blockedTokenGeneration` bookkeeping, which belong to whichever
               // `drain()` call is CURRENT, not to a superseded one.
               switch (outcome) {
               case Outcome::Delivered:
                   QFile::remove(path);
                   break;
               case Outcome::Refused:
                   qCWarning(seathubQualityOutbox) << "quality report for" << sessionId
                                                    << "refused" << result.statusCode;
                   QFile::remove(path);
                   break;
               case Outcome::AuthFailed:
                   if (generation == m_drainGeneration) {
                       m_blockedTokenGeneration = tokenGeneration;
                   }
                   break;
               case Outcome::Retryable:
                   break;
               case Outcome::Aborted:
                   // CR-03: the account changed mid-drain. Kept, and the loop stops here exactly
                   // like AuthFailed - but `m_blockedTokenGeneration` is deliberately left alone:
                   // nothing about this (now stale) generation's credential failed, so a fresh
                   // `drain()` call under the new generation must not find itself blocked by it.
                   break;
               }

               if (generation != m_drainGeneration) {
                   // A newer `drain()` call (or a `cancel()`) has started since this attempt was
                   // issued; that call's own loop owns `m_draining` now, and continuing this one
                   // would run a second pass concurrently with it.
                   return;
               }
               if (outcome == Outcome::AuthFailed || outcome == Outcome::Aborted) {
                   // AuthFailed: the same token would fail the rest too. Aborted: the account this
                   // call started for is no longer the signed-in one. Either way, stop here, not
                   // just this file.
                   m_draining = false;
                   return;
               }
               drainFiles(files, index + 1, tokenGeneration, accountId, postFn, generation);
           });
}

// ---------------------------------------------------------------- StreamTimeOutbox (HI06)

namespace {

// One lock for every record file and for each outbox's directory string. The beat and the
// segment-closed hook write from the network thread, teardown and the launch drain from the
// facade's, and a relaunch reads what a killed process left. Held for a few small file operations
// only, never across a network call.
QMutex& streamTimeFileMutex()
{
    static QMutex mutex;
    return mutex;
}

const char* kStreamAccountKey = "account_id";
const char* kStreamSegmentsKey = "segments";
const char* kStreamSegmentNoKey = "segment_no";
const char* kStreamDeliveredKey = "delivered_ms";
const char* kStreamClosedKey = "closed";

// The largest value the server can store for `delivered_ms` (W04: a 32-bit `integer`).
constexpr qint64 kStreamMaxDeliveredMs = 2147483647;

struct StreamTimeFile
{
    QString accountId;
    QVector<StreamSegmentReport> segments;
};

QString streamTimePath(const QString& directory, const QString& sessionId)
{
    return QDir(directory).filePath(sessionId + QStringLiteral(".json"));
}

// Reads one record file. False for a file that is not a record this outbox wrote.
bool readStreamTimeFile(const QString& path, StreamTimeFile* out)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    const QJsonObject envelope = doc.object();
    const QJsonValue segments = envelope.value(QLatin1String(kStreamSegmentsKey));
    if (!segments.isArray() || !envelope.value(QLatin1String(kStreamAccountKey)).isString()) {
        return false;
    }
    StreamTimeFile record;
    record.accountId = envelope.value(QLatin1String(kStreamAccountKey)).toString();
    for (const QJsonValue& value : segments.toArray()) {
        const QJsonObject entry = value.toObject();
        const int number = entry.value(QLatin1String(kStreamSegmentNoKey)).toInt();
        const qint64 delivered =
            static_cast<qint64>(entry.value(QLatin1String(kStreamDeliveredKey)).toDouble(-1));
        if (number < 1 || delivered < 0) {
            // Not an entry this outbox wrote: never trusted, never sent.
            continue;
        }
        StreamSegmentReport segment;
        segment.segmentNo = number;
        segment.deliveredMs = qMin(delivered, kStreamMaxDeliveredMs);
        segment.closed = entry.value(QLatin1String(kStreamClosedKey)).toBool(false);
        record.segments.push_back(segment);
    }
    std::sort(record.segments.begin(), record.segments.end(),
              [](const StreamSegmentReport& a, const StreamSegmentReport& b) {
                  return a.segmentNo < b.segmentNo;
              });
    *out = record;
    return true;
}

bool writeStreamTimeFile(const QString& directory, const QString& path, const StreamTimeFile& record)
{
    QJsonArray segments;
    for (const StreamSegmentReport& segment : record.segments) {
        QJsonObject entry;
        entry.insert(QLatin1String(kStreamSegmentNoKey), segment.segmentNo);
        entry.insert(QLatin1String(kStreamDeliveredKey), QJsonValue(segment.deliveredMs));
        entry.insert(QLatin1String(kStreamClosedKey), segment.closed);
        segments.append(entry);
    }
    QJsonObject envelope;
    envelope.insert(QLatin1String(kStreamAccountKey), record.accountId);
    envelope.insert(QLatin1String(kStreamSegmentsKey), segments);
    const QByteArray bytes = QJsonDocument(envelope).toJson(QJsonDocument::Compact);

    QDir().mkpath(directory);
    // QSaveFile: a kill mid-write leaves the previous copy (or nothing), never a truncated record.
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

} // namespace

StreamTimeOutbox::Outcome StreamTimeOutbox::classify(const ControlPlaneResult& result)
{
    if (result.wasAborted) {
        return Outcome::Aborted;
    }
    if (result.ok) {
        return Outcome::Delivered;
    }
    if (result.statusCode == 401 || result.statusCode == 403) {
        return Outcome::AuthFailed;
    }
    // The W04 route documents 404, 409 and 422 as its refusals (docs/spec openapi.yaml); those are
    // what is dropped. Any other status is not one the route promises, so it is kept and tried again.
    switch (result.statusCode) {
    case 404: // another account's session, or none by that id
    case 409: // this session never streamed (stream_started_at is null)
    case 422: // a malformed body
        return Outcome::Refused;
    default:
        // 0 (no answer), 5xx, 408, 429, or any other status this route does not document.
        return Outcome::Retryable;
    }
}

QString StreamTimeOutbox::defaultDirectory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("stream-time-outbox"));
}

StreamTimeOutbox::StreamTimeOutbox(const QString& directory)
    : m_directory(directory.isEmpty() ? defaultDirectory() : directory)
{
}

void StreamTimeOutbox::setDirectory(const QString& directory)
{
    QMutexLocker locker(&streamTimeFileMutex());
    m_directory = directory;
}

QString StreamTimeOutbox::directory() const
{
    QMutexLocker locker(&streamTimeFileMutex());
    return m_directory;
}

void StreamTimeOutbox::put(const QString& sessionId, const QString& accountId,
                           const StreamSegmentReport& segment)
{
    if (!isUuidShaped(sessionId)) {
        qCWarning(seathubQualityOutbox) << "refusing to store stream time for a session id "
                                           "that is not a UUID";
        return;
    }
    if (accountId.isEmpty() || segment.segmentNo < 1 || segment.deliveredMs < 0) {
        return;
    }

    QMutexLocker locker(&streamTimeFileMutex());
    const QString path = streamTimePath(m_directory, sessionId);

    StreamTimeFile record;
    StreamTimeFile existing;
    if (readStreamTimeFile(path, &existing) && existing.accountId == accountId) {
        record = existing;
    }
    record.accountId = accountId;

    StreamSegmentReport incoming = segment;
    incoming.deliveredMs = qMin(incoming.deliveredMs, kStreamMaxDeliveredMs);

    bool merged = false;
    for (StreamSegmentReport& stored : record.segments) {
        if (stored.segmentNo != incoming.segmentNo) {
            continue;
        }
        merged = true;
        if (stored.closed && !incoming.closed) {
            // A closed segment never reopens: a late checkpoint of it changes nothing.
        }
        else if (incoming.closed && !stored.closed) {
            // The end of the segment is authoritative, even when it is smaller than the last
            // checkpoint (the reduction for a lost connection can make it so).
            stored = incoming;
        }
        else {
            // Open over open, or closed over closed: the larger, as the server keeps.
            stored.deliveredMs = qMax(stored.deliveredMs, incoming.deliveredMs);
        }
        break;
    }
    if (!merged) {
        record.segments.push_back(incoming);
        std::sort(record.segments.begin(), record.segments.end(),
                  [](const StreamSegmentReport& a, const StreamSegmentReport& b) {
                      return a.segmentNo < b.segmentNo;
                  });
    }

    if (!writeStreamTimeFile(m_directory, path, record)) {
        qCWarning(seathubQualityOutbox) << "could not write the stream-time record for" << sessionId;
    }
}

QVector<StreamSegmentReport> StreamTimeOutbox::entries(const QString& sessionId,
                                                      const QString& accountId) const
{
    if (!isUuidShaped(sessionId)) {
        return {};
    }
    QMutexLocker locker(&streamTimeFileMutex());
    StreamTimeFile record;
    if (!readStreamTimeFile(streamTimePath(m_directory, sessionId), &record)
        || record.accountId != accountId) {
        return {};
    }
    return record.segments;
}

void StreamTimeOutbox::removeIfUnchanged(const QString& sessionId, const StreamSegmentReport& segment)
{
    if (!isUuidShaped(sessionId)) {
        return;
    }
    QMutexLocker locker(&streamTimeFileMutex());
    const QString path = streamTimePath(m_directory, sessionId);
    StreamTimeFile record;
    if (!readStreamTimeFile(path, &record)) {
        return;
    }
    bool removed = false;
    for (int i = 0; i < record.segments.size(); ++i) {
        const StreamSegmentReport& stored = record.segments.at(i);
        if (stored.segmentNo == segment.segmentNo && stored.deliveredMs == segment.deliveredMs
            && stored.closed == segment.closed) {
            record.segments.remove(i);
            removed = true;
            break;
        }
    }
    if (!removed) {
        return;
    }
    if (record.segments.isEmpty()) {
        QFile::remove(path);
    }
    else if (!writeStreamTimeFile(m_directory, path, record)) {
        qCWarning(seathubQualityOutbox) << "could not rewrite the stream-time record for" << sessionId;
    }
}

bool StreamTimeOutbox::hasRecords() const
{
    QMutexLocker locker(&streamTimeFileMutex());
    return !QDir(m_directory).entryList({ QStringLiteral("*.json") }, QDir::Files).isEmpty();
}

void StreamTimeOutbox::drain(const QString& tokenGeneration, const QString& accountId,
                             const QString& skipSessionId, PostFn postFn)
{
    // Nothing is sent before both a token and the signed-in account id are known (D-17).
    if (tokenGeneration.isEmpty() || accountId.isEmpty() || !postFn) {
        return;
    }
    if (!m_blockedTokenGeneration.isEmpty() && m_blockedTokenGeneration == tokenGeneration) {
        return;
    }
    if (m_draining) {
        return;
    }

    QVector<Task> tasks;
    {
        QMutexLocker locker(&streamTimeFileMutex());
        const QDir dir(m_directory);
        const QStringList files = dir.entryList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name);
        for (const QString& fileName : files) {
            const QString sessionId = QFileInfo(fileName).completeBaseName();
            if (sessionId == skipSessionId) {
                continue;
            }
            const QString path = dir.filePath(fileName);
            StreamTimeFile record;
            if (!isUuidShaped(sessionId) || !readStreamTimeFile(path, &record)) {
                // Not a record this outbox wrote - never trusted, never sent.
                QFile::remove(path);
                continue;
            }
            if (record.accountId != accountId) {
                // Another account's record: not this token's to send, and not this call's to
                // delete. It waits for its own account to sign in again.
                continue;
            }
            for (const StreamSegmentReport& segment : record.segments) {
                tasks.push_back(Task{ sessionId, segment });
            }
        }
    }
    if (tasks.isEmpty()) {
        return;
    }

    m_draining = true;
    const quint64 generation = ++m_drainGeneration;
    drainTasks(tasks, 0, tokenGeneration, postFn, generation);
}

void StreamTimeOutbox::cancel()
{
    ++m_drainGeneration;
    m_draining = false;
}

void StreamTimeOutbox::drainTasks(const QVector<Task>& tasks, int index,
                                  const QString& tokenGeneration, const PostFn& postFn,
                                  quint64 generation)
{
    if (index >= tasks.size()) {
        if (generation == m_drainGeneration) {
            m_draining = false;
        }
        return;
    }

    const Task task = tasks.at(index);
    postFn(task.sessionId, task.segment,
           [this, tasks, index, tokenGeneration, postFn, generation,
            task](const ControlPlaneResult& result) {
               const Outcome outcome = classify(result);
               // File effects reflect real server truth whichever drain asked for them, so they
               // always run; only this call's own loop and bookkeeping are gated on its generation.
               switch (outcome) {
               case Outcome::Delivered:
                   removeIfUnchanged(task.sessionId, task.segment);
                   break;
               case Outcome::Refused:
                   qCWarning(seathubQualityOutbox) << "stream time for" << task.sessionId
                                                    << "refused" << result.statusCode;
                   removeIfUnchanged(task.sessionId, task.segment);
                   break;
               case Outcome::AuthFailed:
                   if (generation == m_drainGeneration) {
                       m_blockedTokenGeneration = tokenGeneration;
                   }
                   break;
               case Outcome::Retryable:
               case Outcome::Aborted:
                   break;
               }

               if (generation != m_drainGeneration) {
                   return;
               }
               if (outcome == Outcome::AuthFailed || outcome == Outcome::Aborted) {
                   m_draining = false;
                   return;
               }
               drainTasks(tasks, index + 1, tokenGeneration, postFn, generation);
           });
}

void checkpointStreamTime(const std::shared_ptr<DeliveredStreamTime>& clock,
                          const std::shared_ptr<StreamTimeOutbox>& outbox,
                          StreamTimeOutbox::PostFn post)
{
    if (!clock || !outbox || !post) {
        return;
    }
    const StreamTimeSnapshot snapshot = clock->pending();
    if (snapshot.sessionId.isEmpty() || snapshot.segments.isEmpty()) {
        return;
    }

    for (const StreamSegmentReport& segment : snapshot.segments) {
        // Written before it is sent: a kill between the two loses nothing the record holds. With
        // no account known yet nothing is written (an untagged record could not be proven to
        // belong to anyone), and the send below still happens.
        if (!snapshot.accountId.isEmpty()) {
            outbox->put(snapshot.sessionId, snapshot.accountId, segment);
        }
        const QString sessionId = snapshot.sessionId;
        post(sessionId, segment,
             [clock, outbox, sessionId, segment](const ControlPlaneResult& result) {
                 switch (StreamTimeOutbox::classify(result)) {
                 case StreamTimeOutbox::Outcome::Delivered:
                     outbox->removeIfUnchanged(sessionId, segment);
                     clock->settle(sessionId, segment.segmentNo, segment.deliveredMs);
                     break;
                 case StreamTimeOutbox::Outcome::Refused:
                     outbox->removeIfUnchanged(sessionId, segment);
                     if (result.statusCode == 409) {
                         // The session has not reached ACTIVE (or never will): a closed segment
                         // records nothing and is let go; an open one is tried again at the next
                         // checkpoint, by which time the server may have started it.
                         clock->settle(sessionId, segment.segmentNo, segment.deliveredMs);
                     }
                     else {
                         clock->refuse(sessionId);
                     }
                     break;
                 case StreamTimeOutbox::Outcome::AuthFailed:
                 case StreamTimeOutbox::Outcome::Retryable:
                 case StreamTimeOutbox::Outcome::Aborted:
                     break;
                 }
             });
    }
}
