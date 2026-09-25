#include "quality_outbox.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

Q_LOGGING_CATEGORY(seathubQualityOutbox, "seathub.quality_outbox")

namespace {

const char* kAccountIdKey = "account_id";
const char* kReportKey = "report";

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
    // second pass over the same files.
    if (m_draining) {
        return;
    }

    QDir dir(m_directory);
    const QStringList files = dir.entryList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name);
    if (files.isEmpty()) {
        return;
    }

    m_draining = true;
    drainFiles(files, 0, tokenGeneration, accountId, postFn);
}

void QualityOutbox::drainFiles(const QStringList& files, int index, const QString& tokenGeneration,
                               const QString& accountId, const PostFn& postFn)
{
    if (index >= files.size()) {
        m_draining = false;
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
        drainFiles(files, index + 1, tokenGeneration, accountId, postFn);
        return;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        // Not a report this outbox wrote - never trusted, never sent.
        QFile::remove(path);
        drainFiles(files, index + 1, tokenGeneration, accountId, postFn);
        return;
    }
    const QJsonObject envelope = doc.object();
    const QString storedAccountId = envelope.value(QLatin1String(kAccountIdKey)).toString();
    const QJsonObject report = envelope.value(QLatin1String(kReportKey)).toObject();

    if (storedAccountId != accountId) {
        qCWarning(seathubQualityOutbox)
            << "quality report for" << sessionId << "belongs to another account; dropped";
        QFile::remove(path);
        drainFiles(files, index + 1, tokenGeneration, accountId, postFn);
        return;
    }

    postFn(sessionId, report,
           [this, files, index, tokenGeneration, accountId, postFn, path,
            sessionId](const ControlPlaneResult& result) {
               const Outcome outcome = classify(result);
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
                   m_blockedTokenGeneration = tokenGeneration;
                   break;
               case Outcome::Retryable:
                   break;
               }

               if (outcome == Outcome::AuthFailed) {
                   // The same token would fail the rest too - stop here, not just this file.
                   m_draining = false;
                   return;
               }
               drainFiles(files, index + 1, tokenGeneration, accountId, postFn);
           });
}
