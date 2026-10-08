#include "end_outbox.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

QString sanitiseForFile(const QString& sessionId)
{
    // The session id is opaque to this class: the real id travels inside the JSON, so the file
    // name only has to be unique and filesystem-safe. A uuid passes through unchanged; a test id
    // like `s-live` passes too; anything exotic is flattened.
    QString safe;
    safe.reserve(sessionId.size());
    for (const QChar c : sessionId) {
        safe.append(c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_')
                        || c == QLatin1Char('.')
                    ? c
                    : QLatin1Char('_'));
    }
    return safe;
}

QJsonObject reportToJson(const EndReport& report)
{
    QJsonObject object;
    if (!report.stage.isEmpty()) {
        object.insert(QStringLiteral("stage"), report.stage);
    }
    if (!report.engineStage.isEmpty()) {
        object.insert(QStringLiteral("engine_stage"), report.engineStage);
    }
    if (report.hasEngineError) {
        object.insert(QStringLiteral("has_engine_error"), true);
        object.insert(QStringLiteral("engine_error"), report.engineError);
    }
    if (!report.failingPorts.isEmpty()) {
        object.insert(QStringLiteral("failing_ports"), report.failingPorts);
    }
    if (!report.errorCode.isEmpty()) {
        object.insert(QStringLiteral("error_code"), report.errorCode);
    }
    if (!report.attemptStep.isEmpty()) {
        object.insert(QStringLiteral("attempt_step"), report.attemptStep);
    }
    object.insert(QStringLiteral("customer_quit"), report.customerQuit);
    return object;
}

EndReport reportFromJson(const QJsonObject& object)
{
    EndReport report;
    report.stage = object.value(QStringLiteral("stage")).toString();
    report.engineStage = object.value(QStringLiteral("engine_stage")).toString();
    report.hasEngineError = object.value(QStringLiteral("has_engine_error")).toBool(false);
    report.engineError = object.value(QStringLiteral("engine_error")).toInt(0);
    report.failingPorts = object.value(QStringLiteral("failing_ports")).toString();
    report.errorCode = object.value(QStringLiteral("error_code")).toString();
    report.attemptStep = object.value(QStringLiteral("attempt_step")).toString();
    report.customerQuit = object.value(QStringLiteral("customer_quit")).toBool(false);
    return report;
}

} // namespace

EndOutbox::Outcome EndOutbox::classify(const ControlPlaneResult& result)
{
    if (result.wasAborted) {
        return Aborted;
    }
    if (result.statusCode >= 200 && result.statusCode < 300) {
        return Delivered;
    }
    if (result.statusCode == 404) {
        // The session is already gone. That is a confirmation, not a failure to retry.
        return Refused;
    }
    if (result.statusCode == 401 || result.statusCode == 403) {
        return AuthFailed;
    }
    return Retryable;
}

EndOutbox::EndOutbox(QString directory)
    : m_directory(directory.isEmpty() ? defaultDirectory() : std::move(directory))
{
}

void EndOutbox::setDirectory(const QString& directory)
{
    QMutexLocker locker(&m_mutex);
    m_directory = directory.isEmpty() ? defaultDirectory() : directory;
}

QString EndOutbox::directory() const
{
    QMutexLocker locker(&m_mutex);
    return m_directory;
}

QString EndOutbox::defaultDirectory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("end-outbox"));
}

QString EndOutbox::pathFor(const QString& sessionId) const
{
    return QDir(m_directory).filePath(sanitiseForFile(sessionId) + QStringLiteral(".json"));
}

bool EndOutbox::put(const QString& sessionId, const QString& accountId, bool failed,
                    const EndReport& report)
{
    if (sessionId.isEmpty()) {
        return false;
    }

    QMutexLocker locker(&m_mutex);
    QDir().mkpath(m_directory);

    QJsonObject object;
    object.insert(QStringLiteral("account_id"), accountId);
    object.insert(QStringLiteral("session_id"), sessionId);
    object.insert(QStringLiteral("failed"), failed);
    object.insert(QStringLiteral("report"), reportToJson(report));

    // QSaveFile: a crash mid-write leaves the previous copy (or nothing) rather than a truncated
    // record, so a restart never reads a half-written End.
    QSaveFile file(pathFor(sessionId));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    return file.commit();
}

void EndOutbox::remove(const QString& sessionId)
{
    if (sessionId.isEmpty()) {
        return;
    }
    QMutexLocker locker(&m_mutex);
    QFile::remove(pathFor(sessionId));
}

bool EndOutbox::hasQueued(const QString& sessionId) const
{
    if (sessionId.isEmpty()) {
        return false;
    }
    QMutexLocker locker(&m_mutex);
    return QFile::exists(pathFor(sessionId));
}

bool EndOutbox::hasQueuedForAccount(const QString& accountId) const
{
    return !entriesForAccount(accountId).isEmpty();
}

QList<EndOutbox::Entry> EndOutbox::entriesForAccount(const QString& accountId) const
{
    QList<Entry> entries;
    if (accountId.isEmpty()) {
        return entries;
    }

    QMutexLocker locker(&m_mutex);
    QDir dir(m_directory);
    const QStringList names =
        dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString& name : names) {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (!doc.isObject()) {
            continue;
        }
        const QJsonObject object = doc.object();
        // Another account's End stays on disk: it is not this drain's to send, and dropping it
        // would lose the previous account's record (V-OUTBOX).
        if (object.value(QStringLiteral("account_id")).toString() != accountId) {
            continue;
        }
        const QString sessionId = object.value(QStringLiteral("session_id")).toString();
        if (sessionId.isEmpty()) {
            continue;
        }
        Entry entry;
        entry.sessionId = sessionId;
        entry.failed = object.value(QStringLiteral("failed")).toBool(false);
        entry.report = reportFromJson(object.value(QStringLiteral("report")).toObject());
        entries.append(entry);
    }
    return entries;
}
