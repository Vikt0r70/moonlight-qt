#include "update_retry_state.h"
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

QString UpdateRetryState::defaultPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/update/retry-state.json");
}
UpdateRetryState UpdateRetryState::load(const QString& path)
{
    UpdateRetryState state;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return state;
    const auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return state;
    const auto obj = doc.object();
    if (obj.value("v").toInteger(-1) != state.v) return state;
    state.target = obj.value("target").toString();
    state.attemptsFailed = qBound(0, obj.value("attempts_failed").toInt(), 4);
    state.declines = qMax(0, obj.value("declines").toInt());
    state.firstFailedAt = QDateTime::fromString(obj.value("first_failed_at").toString(), Qt::ISODate).toUTC();
    state.lastFailedAt = QDateTime::fromString(obj.value("last_failed_at").toString(), Qt::ISODate).toUTC();
    state.nextAllowedAt = QDateTime::fromString(obj.value("next_allowed_at").toString(), Qt::ISODate).toUTC();
    state.lastClass = obj.value("last_class").toString();
    state.exhaustedReported = obj.value("exhausted_reported").toBool();
    state.launched = obj.value("launched").toBool();
    static const QRegularExpression id("^[0-9a-f]{16}$");
    for (const auto& value : obj.value("reported_ids").toArray())
        if (value.isString() && id.match(value.toString()).hasMatch()) state.rememberReport(value.toString());
    return state;
}
bool UpdateRetryState::save(const QString& path) const
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QJsonArray ids;
    for (const auto& id : reportedIds) ids.append(id);
    const QJsonObject obj{{"v", v}, {"target", target}, {"attempts_failed", attemptsFailed},
        {"declines", declines}, {"first_failed_at", firstFailedAt.toUTC().toString(Qt::ISODate)},
        {"last_failed_at", lastFailedAt.toUTC().toString(Qt::ISODate)},
        {"next_allowed_at", nextAllowedAt.toUTC().toString(Qt::ISODate)}, {"last_class", lastClass},
        {"exhausted_reported", exhaustedReported}, {"reported_ids", ids}, {"launched", launched}};
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) return false;
    const auto bytes = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    return file.write(bytes) == bytes.size() && file.commit();
}
void UpdateRetryState::rememberReport(const QString& id)
{
    if (reportedIds.contains(id)) return;
    reportedIds.append(id);
    // V37 / ADR-0070 item 6: persist only the last 16 handed-off attempt ids.
    while (reportedIds.size() > 16) reportedIds.removeFirst();
}
