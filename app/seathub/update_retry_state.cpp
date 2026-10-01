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
#include <QVersionNumber>
#include <limits>

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
    state.launchedManual = obj.value("launched_manual").toBool();
    state.lastOffer = obj.value("last_offer").toObject().toVariantMap();
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
        {"exhausted_reported", exhaustedReported}, {"reported_ids", ids}, {"launched", launched},
        {"launched_manual", launchedManual},
        {"last_offer", QJsonObject::fromVariantMap(lastOffer)}};
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
qint64 UpdateRetryState::nextDelayFor(int k) const
{
    if (k <= 1) return 0;
    qint64 temp = kBase;
    for (int i = 1; i < k && temp < kCap; ++i) temp = qMin(kCap, temp * 2);
    const auto half = quint32(temp / 2);
    return half + m_random->bounded(half + 1); // equal jitter, inclusive [temp/2, temp]. V34.
}
void UpdateRetryState::noteFailure(const QString& failureClass, bool permanent, bool manual,
                                   bool previousLaunch)
{
    launched = launchedManual = false;
    const bool alreadyExhausted = exhausted();
    if (!alreadyExhausted) lastClass = permanent ? "installer.verify_failed" : failureClass;
    // ADR-0070: permanent integrity refusals and manual clicks spend no retry budget.
    if (!permanent && !manual) attemptsFailed = qMin(kMaxAttempts, attemptsFailed + 1);
    if (!firstFailedAt.isValid()) firstFailedAt = now();
    lastFailedAt = now();
    nextAllowedAt = now().addSecs(nextDelayFor(attemptsFailed));
    m_firstFailureThisLaunch = attemptsFailed == 1 && !previousLaunch;
}
void UpdateRetryState::noteDecline()
{
    if (!exhausted()) lastClass = "installer.elevation_declined";
    if (declines < std::numeric_limits<int>::max()) ++declines;
    launched = launchedManual = false; m_declinedThisLaunch = true;
}
void UpdateRetryState::noteSuccess()
{
    attemptsFailed = declines = 0; firstFailedAt = lastFailedAt = nextAllowedAt = {};
    lastClass.clear(); exhaustedReported = launched = launchedManual = false;
    m_declinedThisLaunch = m_firstFailureThisLaunch = false;
}
bool UpdateRetryState::automaticAttemptAllowed(const QDateTime& at, bool streaming) const
{
    return !streaming && !launched && !target.isEmpty() && !exhausted()
        && !m_declinedThisLaunch && !m_firstFailureThisLaunch
        && (!nextAllowedAt.isValid() || at >= nextAllowedAt);
}
bool UpdateRetryState::exhausted() const
{
    return attemptsFailed >= kMaxAttempts || lastClass == QLatin1String("installer.verify_failed")
        || exhaustedReported;
}
bool UpdateRetryState::reset(const QString& nextTarget, const QString& installed)
{
    auto current = installed.trimmed();
    while (current.startsWith('v')) current.remove(0, 1);
    const auto have = QVersionNumber::fromString(current).normalized();
    const auto want = QVersionNumber::fromString(nextTarget).normalized();
    const bool reached = !have.isNull() && !want.isNull() && QVersionNumber::compare(have, want) >= 0;
    if (target == nextTarget && !reached) return false;
    target = nextTarget; noteSuccess();
    return true;
}
