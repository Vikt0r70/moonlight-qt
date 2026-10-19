#include "end_outbox.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QStandardPaths>

Q_LOGGING_CATEGORY(seathubEndOutbox, "seathub.end_outbox")

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
    // ADR-0104: a retried End (same run, or after a restart) posts the same `port_test` as its
    // first POST. Absent when none was recorded, so an entry from before this key reads back as none.
    if (!report.portTest.isEmpty()) {
        object.insert(QStringLiteral("port_test"), report.portTest);
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
    report.portTest = object.value(QStringLiteral("port_test")).toObject();
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

QString EndOutbox::answerToken(const ControlPlaneResult& result)
{
    const int status = result.statusCode;
    if (status >= 200 && status < 300) {
        return QStringLiteral("2xx");
    }
    if (status >= 500 && status < 600) {
        return QStringLiteral("5xx");
    }
    if (status == 400 || status == 401 || status == 403 || status == 404 || status == 422) {
        return QString::number(status);
    }
    return status <= 0 ? QStringLiteral("transport") : QStringLiteral("none");
}

void EndOutbox::setObserver(Observer observer)
{
    m_observer = std::move(observer);
}

void EndOutbox::noteEvent(const QString& event, const QString& sessionId, const QString& answer,
                          int count) const
{
    qCInfo(seathubEndOutbox).noquote()
        << QStringLiteral("end_outbox event=%1 answer=%2 session=%3 count=%4")
               .arg(event, answer, sessionId.isEmpty() ? QStringLiteral("-") : sessionId,
                    QString::number(count));
    if (m_observer) {
        m_observer(event, answer, count);
    }
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

    bool written = false;
    {
        QMutexLocker locker(&m_mutex);
        QDir().mkpath(m_directory);

        QJsonObject object;
        object.insert(QStringLiteral("account_id"), accountId);
        object.insert(QStringLiteral("session_id"), sessionId);
        object.insert(QStringLiteral("failed"), failed);
        object.insert(QStringLiteral("report"), reportToJson(report));

        // QSaveFile: a crash mid-write leaves the previous copy (or nothing) rather than a
        // truncated record, so a restart never reads a half-written End.
        QSaveFile file(pathFor(sessionId));
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
            written = file.commit();
        }
    }
    noteEvent(written ? QStringLiteral("put") : QStringLiteral("dropped"), sessionId,
              QStringLiteral("none"), 1);
    return written;
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

    int unreadable = 0;
    QMutexLocker locker(&m_mutex);
    QDir dir(m_directory);
    const QStringList names =
        dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString& name : names) {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::ReadOnly)) {
            ++unreadable;
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (!doc.isObject()) {
            ++unreadable;
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
    locker.unlock();
    // An unreadable file stays on disk (it is not this drain's to delete); it is counted and logged
    // instead of skipped without a trace. The "drain" line is the facade's, once per real drain.
    if (unreadable > 0) {
        noteEvent(QStringLiteral("unreadable"), QString(), QStringLiteral("none"), unreadable);
    }
    return entries;
}
