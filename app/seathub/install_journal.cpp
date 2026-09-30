#include "install_journal.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
bool oneOf(const QJsonValue& value, const QStringList& allowed)
{
    return value.isString() && allowed.contains(value.toString());
}
bool integer(const QJsonValue& value)
{
    return value.isDouble() && std::isfinite(value.toDouble())
        && std::floor(value.toDouble()) == value.toDouble();
}
qint64 clamp(const QJsonValue& value, qint64 lo, qint64 hi)
{
    return static_cast<qint64>(std::clamp(value.toDouble(), double(lo), double(hi)));
}
}

QList<InstallJournalRecord> InstallJournal::adopt(const QString& folder, const QDateTime& now)
{
    // V35 / ADR-0070 item 6: read-only, 8 newest files, 4 KiB each, 7 days.
    static const QRegularExpression filename("^attempt-([0-9a-f]{16})\\.(start|end)\\.json$");
    static const QRegularExpression version("^[0-9]+(?:\\.[0-9]+)+$");
    QList<QFileInfo> candidates;
    const QDir directory(folder);
    for (const auto& info : directory.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Time)) {
        if (filename.match(info.fileName()).hasMatch()) candidates.append(info);
        if (candidates.size() == 8) break;
    }
    QList<InstallJournalRecord> records;
    for (const auto& info : candidates) {
        if (info.size() > 4096 || info.lastModified() < now.addDays(-7)) continue;
        const auto match = filename.match(info.fileName());
        const bool start = match.captured(2) == QLatin1String("start");
        if (start && (info.lastModified().secsTo(now) < 1800
            || directory.exists("attempt-" + match.captured(1) + ".end.json"))) continue;
        QFile file(info.filePath());
        if (!file.open(QIODevice::ReadOnly) || file.size() > 4096) continue;
        const QByteArray bytes = file.read(4096);
        if (!file.atEnd() || file.error() != QFileDevice::NoError) continue;
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) continue;
        const auto obj = doc.object();
        if (!integer(obj.value("v")) || obj.value("v").toInteger(-1) < 0
            || obj.value("attempt").toString() != match.captured(1)
            || !obj.value("from").isString() || !version.match(obj.value("from").toString()).hasMatch()
            || !obj.value("to").isString() || !version.match(obj.value("to").toString()).hasMatch()
            || !oneOf(obj.value("mode"), {"staged"})
            || !oneOf(obj.value("outcome"), {"failed", "success", "cancelled", "recovered"})
            || !oneOf(obj.value("step"), {"preflight", "stop_client", "stage", "extract", "verify",
                                        "swap_aside", "swap_in", "registry", "cleanup", "recover"})
            || !oneOf(obj.value("class"), {"installer.locked_file", "installer.access_denied",
                 "installer.disk_space", "installer.extract_error", "installer.verify_failed",
                 "installer.rollback_failed", "installer.script_error", "installer.interrupted",
                 "installer.unknown"})
            || !oneOf(obj.value("rollback"), {"not_needed", "ok", "failed"})
            || !oneOf(obj.value("state"), {"old_intact", "new_live", "both", "none"})
            || !integer(obj.value("code")) || !obj.value("ms").isObject()
            || !obj.value("elevated").isBool()) continue;
        InstallJournalRecord r;
        r.started = QDateTime::fromString(obj.value("started").toString(), Qt::ISODate).toUTC();
        r.ended = QDateTime::fromString(obj.value("ended").toString(), Qt::ISODate).toUTC();
        if (!r.started.isValid() || (!start && !r.ended.isValid())) continue;
        bool validMs = true;
        for (const auto& key : {"stage", "swap", "total"}) {
            const auto value = obj.value("ms").toObject().value(key);
            if (!integer(value)) { validMs = false; break; }
            // V37 / ADR-0070 item 6: never transmit out-of-range durations.
            r.ms.insert(key, clamp(value, 0, 3600000));
        }
        if (!validMs) continue;
        r.v = obj.value("v").toInteger();
        r.attempt = match.captured(1);
        r.from = obj.value("from").toString(); r.to = obj.value("to").toString();
        r.mode = obj.value("mode").toString(); r.step = obj.value("step").toString();
        r.outcome = start ? "failed" : obj.value("outcome").toString();
        r.failureClass = start ? "installer.interrupted" : obj.value("class").toString();
        r.rollback = obj.value("rollback").toString(); r.state = obj.value("state").toString();
        // V37 / ADR-0070 item 6: signed 32-bit exit codes.
        r.code = static_cast<qint32>(clamp(obj.value("code"), std::numeric_limits<qint32>::min(),
                                         std::numeric_limits<qint32>::max()));
        r.elevated = obj.value("elevated").toBool();
        records.append(r);
    }
    return records;
}

QJsonObject InstallJournalRecord::toJson() const
{
    // Construct from typed, validated fields only; unknown input keys never survive.
    return {{"v", v}, {"attempt", attempt}, {"from", from}, {"to", to}, {"mode", mode},
            {"started", started.toUTC().toString(Qt::ISODate)},
            {"ended", ended.toUTC().toString(Qt::ISODate)}, {"outcome", outcome},
            {"step", step}, {"class", failureClass}, {"code", code}, {"rollback", rollback},
            {"state", state}, {"ms", ms}, {"elevated", elevated}};
}
