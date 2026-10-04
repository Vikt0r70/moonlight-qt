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

Admission admitInstallerLaunch(const InstallerObservation& o)
{
    using Owner = InstallerObservation::Owner;
    using Scan = InstallerObservation::Scan;
    using Boot = InstallerObservation::Boot;
    // A live installer of ours, or any setup anywhere, is never overlapped.
    if (o.owner == Owner::Alive || o.scan == Scan::Present) return Admission::Blocked;
    // If the machine could not be read, the absence of a writer is not proven.
    if (o.scan == Scan::Unknown) return Admission::Blocked;
    // An owner that cannot be identified stays blocked until a reboot proves it ended.
    if (o.owner == Owner::Unknown && o.boot != Boot::Changed) return Admission::Blocked;
    return Admission::Quiescent;
}
const char* ownerName(InstallerObservation::Owner owner)
{
    switch (owner) {
    case InstallerObservation::Owner::None: return "none";
    case InstallerObservation::Owner::Alive: return "alive";
    case InstallerObservation::Owner::Gone: return "gone";
    case InstallerObservation::Owner::Unknown: return "unknown";
    }
    return "unknown";
}
const char* scanName(InstallerObservation::Scan scan)
{
    switch (scan) {
    case InstallerObservation::Scan::Clear: return "clear";
    case InstallerObservation::Scan::Present: return "present";
    case InstallerObservation::Scan::Unknown: return "unknown";
    }
    return "unknown";
}
const char* bootName(InstallerObservation::Boot boot)
{
    switch (boot) {
    case InstallerObservation::Boot::Same: return "same";
    case InstallerObservation::Boot::Changed: return "changed";
    case InstallerObservation::Boot::Unknown: return "unknown";
    }
    return "unknown";
}

namespace {
QJsonObject traceToJson(const InstallerTrace& t)
{
    return {{"requested_at", t.requestedAt.toUTC().toString(Qt::ISODateWithMs)},
        {"returned_at", t.returnedAt.toUTC().toString(Qt::ISODateWithMs)},
        {"shell_error", double(t.shellError)}, {"pid", double(t.pid)}, {"pid_created", t.pidCreated},
        {"launcher_pid", double(t.launcherPid)}, {"launcher_session", double(t.launcherSession)},
        {"installer_session", double(t.installerSession)}, {"boot_uptime_ms", double(t.bootUptimeMs)},
        {"observations", t.observations}, {"last_owner", t.lastOwner}, {"last_scan", t.lastScan},
        {"last_boot", t.lastBoot}, {"owner_access_denied", t.ownerAccessDenied},
        {"observer_changed", t.observerChanged}, {"journal_start_present", t.journalStartPresent},
        {"stage_present", t.stagePresent}, {"progress_changed", t.progressChanged}};
}
InstallerTrace traceFromJson(const QJsonObject& o)
{
    InstallerTrace t;
    t.requestedAt = QDateTime::fromString(o.value("requested_at").toString(), Qt::ISODateWithMs).toUTC();
    t.returnedAt = QDateTime::fromString(o.value("returned_at").toString(), Qt::ISODateWithMs).toUTC();
    t.shellError = qint64(o.value("shell_error").toDouble());
    t.pid = qint64(o.value("pid").toDouble());
    static const QRegularExpression digits("^[0-9]{1,20}$");
    const auto created = o.value("pid_created").toString();
    if (digits.match(created).hasMatch()) t.pidCreated = created;
    t.launcherPid = qint64(o.value("launcher_pid").toDouble());
    t.launcherSession = qint64(o.value("launcher_session").toDouble(-1));
    t.installerSession = qint64(o.value("installer_session").toDouble(-1));
    t.bootUptimeMs = qint64(o.value("boot_uptime_ms").toDouble());
    t.observations = qBound(0, o.value("observations").toInt(), 1000000);
    t.lastOwner = o.value("last_owner").toString();
    t.lastScan = o.value("last_scan").toString();
    t.lastBoot = o.value("last_boot").toString();
    t.ownerAccessDenied = o.value("owner_access_denied").toBool();
    t.observerChanged = o.value("observer_changed").toBool();
    t.journalStartPresent = o.value("journal_start_present").toBool();
    t.stagePresent = o.value("stage_present").toBool();
    t.progressChanged = o.value("progress_changed").toBool();
    return t;
}
}

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
    state.trace = traceFromJson(obj.value("launch_trace").toObject());
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
        {"launched_manual", launchedManual}, {"launch_trace", traceToJson(trace)},
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
    // ADR-0077 item 2: a client is current only when its version EQUALS the target. A newer one is a
    // supported rollback and still has to converge, so it spends the same bounded budget.
    const bool reached = !have.isNull() && !want.isNull() && QVersionNumber::compare(have, want) == 0;
    if (target == nextTarget && !reached) return false;
    target = nextTarget; noteSuccess();
    return true;
}
