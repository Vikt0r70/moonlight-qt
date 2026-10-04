#include "update_feed_client.h"

#include "error_map.h"
#include "seathub_version.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QCoreApplication>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#endif

Q_LOGGING_CATEGORY(seathubUpdates, "seathub.updates")

namespace {

const char* kStateIdle = "idle";
const char* kStateChecking = "checking";
const char* kStateAvailable = "available";
const char* kStateDownloading = "downloading";
const char* kStateVerifying = "verifying";
const char* kStateReady = "ready";
const char* kStateInstalling = "installing";
const char* kStateFailed = "failed";

const char* kNoChecksum =
    "This build doesn't have a verified checksum for the new version yet, so SeatHub won't "
    "install it automatically. Download it from the SevenHills website in the meantime.";

const char* kChecksumMismatch =
    "The downloaded update didn't match its checksum, so it wasn't installed. Nothing on this PC "
    "was changed.";

// copy.md § Support & errors: the customer sentence for an unreachable control plane.
const char* kUnreachable = "We couldn't reach SevenHills. Try again in a moment.";

const char* kWhileStreaming =
    "SeatHub doesn't change its own files while a stream is running. The update will be offered "
    "when the session ends.";

// Mirrors the retired Tauri client's `numeric_parts` exactly (`src-tauri/src/updater.rs`): a
// leading `v` is dropped, a pre-release or build suffix is ignored (the feed publishes one
// current row per app), and the remaining dotted parts must all be numbers.
bool numericParts(const QString& version, QList<quint64>& out)
{
    QString trimmed = version.trimmed();
    while (trimmed.startsWith(QLatin1Char('v'))) {
        trimmed.remove(0, 1);
    }

    int cut = trimmed.size();
    const int dash = trimmed.indexOf(QLatin1Char('-'));
    const int plus = trimmed.indexOf(QLatin1Char('+'));
    if (dash >= 0) {
        cut = qMin(cut, dash);
    }
    if (plus >= 0) {
        cut = qMin(cut, plus);
    }

    const QString core = trimmed.left(cut);
    if (core.isEmpty()) {
        return false;
    }

    const QStringList parts = core.split(QLatin1Char('.'));
    out.clear();
    for (const QString& part : parts) {
        bool ok = false;
        const quint64 value = part.toULongLong(&ok);
        if (!ok) {
            out.clear();
            return false;
        }
        out.append(value);
    }
    return !out.isEmpty();
}

QString normalizedVersion(const QString& version)
{
    QString trimmed = version.trimmed();
    while (trimmed.startsWith(QLatin1Char('v'))) {
        trimmed.remove(0, 1);
    }
    return trimmed;
}

#ifdef Q_OS_WIN
quint64 fileTimeValue(const FILETIME& time)
{
    return (quint64(time.dwHighDateTime) << 32) | quint64(time.dwLowDateTime);
}

// A SeatHub setup (the downloaded SeatHub-Setup-<version>.exe, including a hand-run copy) or the SeatHub
// maintenance tool. The installed client SeatHub.exe is deliberately not one of them.
bool isInstallerImage(const QString& image)
{
    static const QRegularExpression setup(QStringLiteral("^SeatHub.*Setup.*\\.exe$"),
                                          QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression maintenance(QStringLiteral("^SeatHubMaintenanceTool\\.exe$"),
                                                QRegularExpression::CaseInsensitiveOption);
    return setup.match(image).hasMatch() || maintenance.match(image).hasMatch();
}
#endif

} // namespace

UpdateFeedClient::UpdateFeedClient(QObject* parent)
    : QObject(parent),
      m_network(new QNetworkAccessManager(this)),
      m_launcher([this](const QString& path) {
          return launchInstaller(path, installerArguments(m_installedVersion,
              qMin(UpdateRetryState::kMaxAttempts, m_retry.attemptsFailed + 1)), &m_retry.trace);
      }),
      m_baseUrl(defaultBaseUrl()),
      m_installedVersion(QString::fromLatin1(SEATHUB_VERSION)),
      m_state(QString::fromLatin1(kStateIdle))
{
    m_retry = UpdateRetryState::load(m_retryStatePath);
    m_journalFolder = QDir(qEnvironmentVariable("ProgramData")).filePath("SeatHubSetup/install-journal");
    m_urlOpener = &QDesktopServices::openUrl;
    if (!m_retry.lastOffer.isEmpty()) {
        m_available = parseRelease(QJsonDocument(QJsonObject::fromVariantMap(m_retry.lastOffer)).toJson(),
                                   m_installedVersion, m_pinnedSha256);
        if (!m_available.isEmpty())
            m_state = QString::fromLatin1(m_retry.lastClass.isEmpty() ? kStateAvailable : kStateFailed);
    }
}

QString UpdateFeedClient::defaultBaseUrl()
{
    // `docs/spec/openapi.yaml` `servers[0].url`, frozen by ADR-0015 and moved to this host by
    // ADR-0018 (`api-sevenhills.damra.co`, one label deep under the wildcard certificate).
    return QStringLiteral("https://api-sevenhills.damra.co");
}

QString UpdateFeedClient::feedPath(const QString& app)
{
    // The public release feed of ADR-0026: the row the website's download button reads and the
    // only thing an installed copy needs in order to know a new build exists (D-38).
    return QStringLiteral("/api/releases/") + app;
}

QString UpdateFeedClient::appName()
{
    // The retired client's `APP_CLIENT` (`src-tauri/src/updater.rs`).
    return QStringLiteral("client");
}

bool UpdateFeedClient::isNewerVersion(const QString& candidate, const QString& current)
{
    QList<quint64> left;
    QList<quint64> right;
    if (!numericParts(candidate, left) || !numericParts(current, right)) {
        // Unparseable is never newer. Refusing to offer an update is recoverable; offering a
        // bogus one is not.
        return false;
    }

    const int width = qMax(left.size(), right.size());
    for (int i = 0; i < width; i++) {
        const quint64 l = i < left.size() ? left.at(i) : 0;
        const quint64 r = i < right.size() ? right.at(i) : 0;
        if (l != r) {
            return l > r;
        }
    }
    return false;
}

bool UpdateFeedClient::isRollback(const QString& candidate, const QString& current)
{
    return isNewerVersion(current, candidate);
}

bool UpdateFeedClient::isSameVersion(const QString& left, const QString& right)
{
    QList<quint64> a;
    QList<quint64> b;
    if (!numericParts(left, a) || !numericParts(right, b)) {
        return false;
    }
    const int width = qMax(a.size(), b.size());
    for (int i = 0; i < width; i++) {
        if ((i < a.size() ? a.at(i) : 0) != (i < b.size() ? b.at(i) : 0)) {
            return false;
        }
    }
    return true;
}

QVariantMap UpdateFeedClient::parseRelease(const QByteArray& json, const QString& installedVersion,
                                           const QString& pinnedSha256)
{
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (!document.isObject()) {
        // A 404 body ("Nothing published for this app yet") and a malformed body both mean
        // "nothing to offer"; the caller tells the two apart by the reply's status.
        return {};
    }

    const QJsonObject release = document.object();
    const QString version = release.value(QStringLiteral("version")).toString();
    const QString url = release.value(QStringLiteral("url")).toString();
    if (version.isEmpty() || url.isEmpty()) {
        return {};
    }

    if (normalizedVersion(version) == normalizedVersion(installedVersion)
        || isSameVersion(version, installedVersion)) {
        // Already on this build: the installed version equals the current release (ADR-0077 item 3).
        return {};
    }

    // D-43: the checksum is the whole integrity story, since nothing is code-signed. The frozen
    // `Release` schema (ADR-0026, `openapi.yaml` §Release) has no digest field - its only
    // integrity field is `signature`, which is a detached signature and stays null - so the
    // applicable digest is read from the feed when a `sha256` field is ever added, and
    // otherwise from the pin carried in from `seathub-ops/pins.yaml` at release prep.
    QString sha256 = release.value(QStringLiteral("sha256")).toString().trimmed();
    if (sha256.isEmpty()) {
        sha256 = pinnedSha256.trimmed();
    }

    QVariantMap offer;
    offer.insert(QStringLiteral("version"), version);
    offer.insert(QStringLiteral("url"), url);
    offer.insert(QStringLiteral("sha256"), sha256);
    offer.insert(QStringLiteral("notes"), release.value(QStringLiteral("notes")).toString());
    offer.insert(QStringLiteral("rollback"), isRollback(version, installedVersion));
    QList<quint64> parts;
    if (!numericParts(version, parts) || !numericParts(installedVersion, parts)) return {};
    // ADR-0077: the current release IS the floor. The row's own `min_version` (still sent for installed
    // 0.1.16/0.1.27 readers) and any value cached by an older build are ignored, so an installed version
    // that differs from the release, newer or older, is always required.
    offer.insert(QStringLiteral("mandatory"), true);
    return offer;
}

QString UpdateFeedClient::fileSha256(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool UpdateFeedClient::verifyFileChecksum(const QString& path, const QString& expectedSha256)
{
    const QString expected = expectedSha256.trimmed();
    if (expected.isEmpty()) {
        // D-43, fail closed: without an expected digest there is nothing to verify against, so
        // the package is not installable.
        qCWarning(seathubUpdates) << "no expected checksum - refusing to treat the download as "
                                     "verified";
        return false;
    }

    const QString actual = fileSha256(path);
    if (actual.isEmpty()) {
        return false;
    }
    return actual.compare(expected, Qt::CaseInsensitive) == 0;
}

// ------------------------------------------------------------------------------------- checks
bool UpdateFeedClient::mandatory() const
{
    return !m_available.isEmpty();
}
QString UpdateFeedClient::nextAttemptText() const
{
    const auto seconds = m_retry.now().secsTo(m_retry.nextAllowedAt);
    if (m_retry.attemptsFailed <= 1 || seconds <= 0) return tr("the next time you open SeatHub");
    if (seconds < 7200) return tr("in about %1 minutes").arg(qRound(double(seconds) / 60));
    return tr("in about %1 hours").arg(qRound(double(seconds) / 3600));
}
QString UpdateFeedClient::manualDownloadUrl() const
{
    const QUrl url(m_available.value("url").toString(), QUrl::StrictMode);
    // client.md §Website links / ADR-0070: only the current installer's https control-plane host.
    return url.isValid() && url.scheme() == QLatin1String("https")
        && url.host() == QUrl(defaultBaseUrl()).host() ? url.toString() : QString();
}
QString UpdateFeedClient::installerArguments(const QString& installed, int attempt)
{
    QList<quint64> parts;
    if (!numericParts(installed, parts)) return {};
    QStringList dotted;
    for (const auto part : parts) dotted.append(QString::number(part));
    return QStringLiteral("SeatHubFromVersion=%1 SeatHubAttempt=%2").arg(dotted.join('.')).arg(attempt);
}
bool UpdateFeedClient::openManualDownload()
{
    // ADR-0077 item 5: read the current release first; open only the row just read.
    m_pendingDownload = true;
    if (m_reply != nullptr && m_downloadFile == nullptr) return true; // that read carries this press
    if (!checkForUpdates()) { m_pendingDownload = false; return false; }
    return true;
}
bool UpdateFeedClient::tryAgain()
{
    // ADR-0077 item 5: the setup of the row read now, downloaded and verified now, never a cached one.
    m_manualAttempt = true;
    m_pendingTryAgain = true;
    if (m_reply != nullptr && m_downloadFile == nullptr) return true;
    if (!checkForUpdates()) { m_pendingTryAgain = false; m_manualAttempt = false; return false; }
    return true;
}
void UpdateFeedClient::startFreshDownload()
{
    if (!m_downloadedPath.isEmpty()) {
        QFile::remove(m_downloadedPath);
        m_downloadedPath.clear();
        emit readyToInstallChanged();
    }
    downloadUpdate();
}
void UpdateFeedClient::finishPendingActions(bool feedRead)
{
    const bool download = m_pendingDownload, retry = m_pendingTryAgain;
    m_pendingDownload = m_pendingTryAgain = false;
    if (!feedRead || m_available.isEmpty()) {
        // An unreachable feed starts nothing and consumes no attempt (ADR-0077 item 3).
        m_manualAttempt = false;
        return;
    }
    if (download) {
        const auto url = manualDownloadUrl();
        if (url.isEmpty() || !m_urlOpener || !m_urlOpener(QUrl(url)))
            qCWarning(seathubUpdates) << "the release address was not opened";
    }
    if (retry) startFreshDownload();
}
void UpdateFeedClient::setRetryStatePath(const QString& path)
{
    m_retryStatePath = path; m_retry = UpdateRetryState::load(path); m_outcomeChecked = false;
    m_offerFresh = false;
    m_available = parseRelease(QJsonDocument(QJsonObject::fromVariantMap(m_retry.lastOffer)).toJson(),
                               m_installedVersion, m_pinnedSha256);
    emit availableUpdateChanged(); emit retryChanged();
    setState(m_available.isEmpty() ? kStateIdle : m_retry.lastClass.isEmpty() ? kStateAvailable : kStateFailed);
}
void UpdateFeedClient::persistRetry()
{
    if (m_retry.target.isEmpty()) return;
    // The SDK adopter may have handed off journal records since this feed instance loaded.
    for (const auto& id : UpdateRetryState::load(m_retryStatePath).reportedIds) m_retry.rememberReport(id);
    if (!m_retry.save(m_retryStatePath)) qCWarning(seathubUpdates) << "could not persist update retry state";
    emit retryChanged();
}
void UpdateFeedClient::adoptOffer(const QVariantMap& offer)
{
    const bool changed = m_retry.target != offer.value("version").toString();
    m_available = offer;
    if (changed) m_downloadedPath.clear();
    m_expectedSha256 = offer.value("sha256").toString();
    m_retry.reset(offer.value("version").toString(), m_installedVersion);
    if (changed) m_outcomeChecked = true;
    m_retry.lastOffer = offer; persistRetry();
    inspectPendingLaunch();
    emit availableUpdateChanged();
    setState(!m_retry.lastClass.isEmpty() || m_retry.exhausted() ? kStateFailed : kStateAvailable);
    reportExhaustion();
}
void UpdateFeedClient::inspectPendingLaunch()
{
    if (m_outcomeChecked) return;
    if (!m_retry.launched) { m_outcomeChecked = true; return; }
    // ADR-0077 item 7: a launched installer is judged only once the state is quiescent. While the setup
    // may still be writing, its outcome is not inferred and no new attempt starts (the `launched` marker
    // keeps automatic attempts off). Blocked, not abandoned: every later feed read looks again.
    if (!installerQuiescent()) {
        qCInfo(seathubUpdates) << "an earlier installer is not proven finished; not judging its outcome";
        return;
    }
    m_outcomeChecked = true;
    const bool manual = m_retry.launchedManual;
    const auto records = InstallJournal::adopt(m_journalFolder, m_retry.now());
    for (const auto& record : records) {
        if (record.to != m_retry.target || record.from != m_installedVersion
            || (m_retry.lastFailedAt.isValid() && record.ended.isValid() && record.ended <= m_retry.lastFailedAt))
            continue;
        m_retry.launched = m_retry.launchedManual = false;
        if (record.outcome == QLatin1String("failed") || record.outcome == QLatin1String("recovered"))
            m_retry.noteFailure(record.failureClass, false, manual, true);
        persistRetry();
        return; // a verified journal outcome is never also inferred as no_result.
    }
    m_retry.noteFailure("installer.no_result", false, manual, true);
    reportInference("installer.no_result"); persistRetry();
}
void UpdateFeedClient::reportInference(const QString& failureClass)
{
    if (!m_reporter) return;
    InstallJournalRecord record;
    record.v = 1;
    record.attempt = QString::number(QRandomGenerator::global()->generate64(), 16).rightJustified(16, '0');
    record.from = m_installedVersion; record.to = m_retry.target; record.mode = "staged";
    record.started = record.ended = m_retry.now(); record.outcome = "failed";
    record.step = "preflight"; record.failureClass = failureClass;
    record.rollback = "not_needed"; record.state = "old_intact";
    record.ms = {{"stage", 0}, {"swap", 0}, {"total", 0}};
    if (m_reporter(record)) {
        m_retry.rememberReport(record.attempt);
        if (failureClass == QLatin1String("installer.retries_exhausted")) m_retry.exhaustedReported = true;
        persistRetry();
    }
}
void UpdateFeedClient::reportExhaustion()
{
    if (m_retry.exhausted() && !m_retry.exhaustedReported) reportInference("installer.retries_exhausted");
}
void UpdateFeedClient::recordFailure(const QString& failureClass, bool permanent)
{
    m_retry.noteFailure(failureClass, permanent, m_manualAttempt); m_manualAttempt = false;
    persistRetry(); reportExhaustion();
}
void UpdateFeedClient::maybeAutomaticAttempt()
{
    if (m_available.isEmpty() || !m_retry.automaticAttemptAllowed(m_retry.now(), m_streaming)) return;
    // Only retries run without a click. A fresh offer still presents Update.
    if (m_retry.lastClass.isEmpty()) return;
    m_manualAttempt = false;
    // ADR-0077 item 5: always the setup of the row just read, downloaded and verified now.
    startFreshDownload();
}

// ------------------------------------------------------------------------------ quiescence (ADR-0077)

bool UpdateFeedClient::installerQuiescent()
{
    const InstallerObservation observation = m_probe
        ? m_probe(m_retry)
        : observeMachine(m_retry, m_journalFolder, &m_retry.trace);
    recordObservation(observation);
    return admitInstallerLaunch(observation) == Admission::Quiescent;
}

void UpdateFeedClient::recordObservation(const InstallerObservation& observation)
{
    auto& trace = m_retry.trace;
    trace.observations = qMin(trace.observations + 1, 1000000);
    trace.lastOwner = QString::fromLatin1(ownerName(observation.owner));
    trace.lastScan = QString::fromLatin1(scanName(observation.scan));
    trace.lastBoot = QString::fromLatin1(bootName(observation.boot));
    trace.observerChanged = trace.launcherPid != 0 && trace.launcherPid != QCoreApplication::applicationPid();
    qCInfo(seathubUpdates) << "installer observation: owner" << trace.lastOwner << "scan" << trace.lastScan
                           << "boot" << trace.lastBoot;
    persistRetry();
}

InstallerObservation UpdateFeedClient::observeMachine(const UpdateRetryState& retry,
                                                      const QString& journalFolder, InstallerTrace* trace)
{
    using Owner = InstallerObservation::Owner;
    using Scan = InstallerObservation::Scan;
    using Boot = InstallerObservation::Boot;
    InstallerObservation observation;
    const InstallerTrace& launched = retry.trace;
#ifdef Q_OS_WIN
    // Scan: every process image on the machine, from any account. Names come from the snapshot itself, so
    // no process has to be opened; an incomplete walk is unknown, never clear.
    observation.scan = Scan::Unknown;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        bool present = false;
        bool complete = false;
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (entry.th32ProcessID != GetCurrentProcessId()
                    && isInstallerImage(QString::fromWCharArray(entry.szExeFile)))
                    present = true;
            } while (Process32NextW(snapshot, &entry));
            complete = GetLastError() == ERROR_NO_MORE_FILES;
        }
        CloseHandle(snapshot);
        observation.scan = present ? Scan::Present : complete ? Scan::Clear : Scan::Unknown;
    }

    // Owner: the installer this client launched, identified by PID plus creation time. A PID that now
    // belongs to a process created at another time is a different process, so the launched one is gone;
    // a process that cannot be opened is unknown (for example another account's elevated installer).
    observation.owner = Owner::None;
    if (launched.pid > 0) {
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(launched.pid));
        if (process == nullptr) {
            const DWORD error = GetLastError();
            observation.owner = error == ERROR_INVALID_PARAMETER ? Owner::Gone : Owner::Unknown;
            if (trace) trace->ownerAccessDenied = error == ERROR_ACCESS_DENIED;
        } else {
            FILETIME created{}, exited{}, kernel{}, user{};
            DWORD exitCode = 0;
            if (!GetProcessTimes(process, &created, &exited, &kernel, &user)
                || !GetExitCodeProcess(process, &exitCode)) {
                observation.owner = Owner::Unknown;
            } else if (exitCode != STILL_ACTIVE) {
                observation.owner = Owner::Gone;
            } else if (launched.pidCreated.isEmpty()) {
                observation.owner = Owner::Unknown; // identity was never recorded, so it cannot be proven
            } else {
                observation.owner = QString::number(fileTimeValue(created)) == launched.pidCreated
                    ? Owner::Alive : Owner::Gone;
            }
            CloseHandle(process);
        }
    }

    // Boot: uptime only grows within one boot, so a smaller value than at launch means a reboot.
    observation.boot = Boot::Unknown;
    if (launched.bootUptimeMs > 0)
        observation.boot = qint64(GetTickCount64()) < launched.bootUptimeMs ? Boot::Changed : Boot::Same;
#else
    Q_UNUSED(launched)
    observation.scan = Scan::Unknown;
    observation.owner = Owner::None;
    observation.boot = Boot::Unknown;
#endif
    if (trace) {
        // Growth is read as a change of state between observations, never as elapsed time.
        bool journalStart = false;
        const QDir journal(journalFolder);
        for (const QString& name : journal.entryList({QStringLiteral("attempt-*.start.json")}, QDir::Files)) {
            QString end = name;
            end.replace(QStringLiteral(".start.json"), QStringLiteral(".end.json"));
            if (!journal.exists(end)) journalStart = true;
        }
        QDir parent(QCoreApplication::applicationDirPath());
        const QString leaf = parent.dirName();
        parent.cdUp();
        const bool stage = !parent.entryList({leaf + QStringLiteral(".stage-*")}, QDir::Dirs).isEmpty();
        trace->progressChanged = trace->journalStartPresent != journalStart || trace->stagePresent != stage;
        trace->journalStartPresent = journalStart;
        trace->stagePresent = stage;
    }
    return observation;
}

bool UpdateFeedClient::checkForUpdates()
{
    if (m_streaming) {
        // D-41 / Pitfall 8 / T-03-17: nothing is offered while a stream runs. The offer is made
        // once the session ends (`sessionFinished`).
        m_heldDuringSession = true;
        qCInfo(seathubUpdates) << "update check deferred: a stream is running";
        return false;
    }
    if (m_reply != nullptr || m_downloadFile != nullptr || m_state == QLatin1String(kStateInstalling)) {
        return false;
    }

    clearFailure();
    setState(QString::fromLatin1(kStateChecking));

    QUrl url(m_baseUrl + feedPath(appName()));
    if (!url.isValid()) {
        finishWithError(QStringLiteral("The release feed address is unusable."), QString());
        return false;
    }

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    m_reply = m_network->get(request);
    connect(m_reply, &QNetworkReply::finished, this, [this]() {
        QNetworkReply* reply = m_reply;
        m_reply = nullptr;
        reply->deleteLater();

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const QString expected = m_pinnedSha256;

        if (status == 404) {
            // "Nothing published for this app yet" is the ordinary answer in a fresh
            // environment, not a fault to show anybody.
            qCInfo(seathubUpdates) << "no release published for" << appName();
            clearOffer();
            setState(QString::fromLatin1(kStateIdle));
            finishPendingActions(true);
            emit checkFinished();
            return;
        }

        if (reply->error() != QNetworkReply::NoError && status != 200) {
            const bool pressed = m_pendingTryAgain || m_pendingDownload;
            m_offerFresh = false;
            finishWithError(pressed ? QString::fromLatin1(kUnreachable) : reply->errorString(), QString());
            if (!m_retry.lastOffer.isEmpty()) {
                const auto cached = parseRelease(QJsonDocument(QJsonObject::fromVariantMap(m_retry.lastOffer)).toJson(),
                                                  m_installedVersion, m_pinnedSha256);
                // ADR-0077 item 3: a cached offer keeps the window up, but it never launches an installer
                // and never feeds Download or Try again.
                if (!cached.isEmpty()) adoptOffer(cached);
            }
            // A press that could not read the feed ends visibly failed, with the reason and Try again.
            if (pressed && !m_available.isEmpty()) setState(QString::fromLatin1(kStateFailed));
            finishPendingActions(false);
            emit checkFinished();
            return;
        }

        const QVariantMap offer = parseRelease(body, m_installedVersion, expected);
        if (offer.isEmpty()) {
            clearOffer();
            setState(QString::fromLatin1(kStateIdle));
            finishPendingActions(true);
        }
        else {
            m_offerFresh = true;
            adoptOffer(offer);
            qCInfo(seathubUpdates) << "release feed offers version"
                                   << offer.value(QStringLiteral("version")).toString();
            if (m_pendingTryAgain || m_pendingDownload) finishPendingActions(true);
            else maybeAutomaticAttempt();
        }
        emit checkFinished();
    });

    return true;
}

// ------------------------------------------------------------------------------------ download

bool UpdateFeedClient::downloadUpdate()
{
    return downloadUpdate(m_available.value(QStringLiteral("url")).toString(),
                          m_available.value(QStringLiteral("sha256")).toString());
}

bool UpdateFeedClient::downloadUpdate(const QString& url, const QString& expectedSha256)
{
    if (m_streaming) {
        qCWarning(seathubUpdates) << "refused a download during an active stream";
        setFailure(SeatHubFailure::local(QString::fromLatin1(kWhileStreaming)).toVariantMap());
        setState(QString::fromLatin1(kStateFailed));
        return false;
    }
    if (m_reply != nullptr || m_downloadFile != nullptr) {
        return false;
    }
    if (!m_available.isEmpty() && !m_offerFresh && url == m_available.value(QStringLiteral("url")).toString()) {
        // ADR-0077 items 3 and 5: this offer was restored from the last known state, so it only keeps the
        // window up. An install attempt downloads the row read now: read the feed first, then come back.
        m_pendingTryAgain = true;
        if (!checkForUpdates()) { m_pendingTryAgain = false; return false; }
        return true;
    }
    if (url.isEmpty() || expectedSha256.trimmed().isEmpty()) {
        // D-43: an unverifiable package is never downloaded as an update candidate.
        qCWarning(seathubUpdates) << "refused a download with no expected checksum";
        setFailure(SeatHubFailure::local(QString::fromLatin1(kNoChecksum)).toVariantMap());
        setState(QString::fromLatin1(kStateFailed));
        recordFailure("installer.verify_failed", true);
        return false;
    }

    QUrl feed(url);
    if (!feed.isValid()) {
        finishWithError(QStringLiteral("The release download address is unusable."), QString());
        recordFailure("installer.unknown");
        return false;
    }

    clearFailure();
    m_expectedSha256 = expectedSha256.trimmed();

    QString fileName = QFileInfo(feed.path()).fileName();
    if (fileName.isEmpty()) {
        fileName = QStringLiteral("SeatHub-%1-installer.exe")
                       .arg(m_available.value(QStringLiteral("version")).toString());
    }
    const QString directory =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/SeatHub");
    QDir().mkpath(directory);
    const QString target = directory + QLatin1Char('/') + fileName;

    m_downloadFile = new QFile(target, this);
    if (!m_downloadFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        delete m_downloadFile;
        m_downloadFile = nullptr;
        finishWithError(QStringLiteral("The update couldn't be saved on this PC."), QString());
        recordFailure("installer.disk_space");
        return false;
    }

    m_progress = 0;
    emit progressChanged();
    setState(QString::fromLatin1(kStateDownloading));

    QNetworkRequest request(feed);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    m_reply = m_network->get(request);
    connect(m_reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                if (total <= 0) {
                    return;
                }
                const int percent = int((received * 100) / total);
                if (percent != m_progress) {
                    m_progress = percent;
                    emit progressChanged();
                }
            });
    connect(m_reply, &QNetworkReply::readyRead, this, [this]() {
        if (m_downloadFile != nullptr && m_reply != nullptr) {
            m_downloadFile->write(m_reply->readAll());
        }
    });
    connect(m_reply, &QNetworkReply::finished, this, [this]() {
        QNetworkReply* reply = m_reply;
        m_reply = nullptr;
        reply->deleteLater();

        if (m_downloadFile != nullptr) {
            m_downloadFile->write(reply->readAll());
            m_downloadFile->close();
        }

        const QString path = m_downloadFile != nullptr ? m_downloadFile->fileName() : QString();
        delete m_downloadFile;
        m_downloadFile = nullptr;

        if (reply->error() != QNetworkReply::NoError) {
            if (!path.isEmpty()) {
                QFile::remove(path);
            }
            finishWithError(reply->errorString(), QString());
            recordFailure("installer.unknown");
            return;
        }

        setState(QString::fromLatin1(kStateVerifying));
        if (!verifyFileChecksum(path, m_expectedSha256)) {
            // T-03-14: a package that does not match the expected digest is discarded. It is
            // never executed, whatever it contains.
            QFile::remove(path);
            m_progress = 0;
            emit progressChanged();
            setFailure(SeatHubFailure::local(QString::fromLatin1(kChecksumMismatch)).toVariantMap());
            setState(QString::fromLatin1(kStateFailed));
            recordFailure("installer.verify_failed", true);
            qCWarning(seathubUpdates) << "downloaded package failed checksum verification and was "
                                         "deleted";
            return;
        }

        m_downloadedPath = path;
        m_progress = 100;
        emit progressChanged();
        emit readyToInstallChanged();
        // "ready" before `verified`, so anything answering the signal sees the state it describes.
        setState(QString::fromLatin1(kStateReady));
        qCInfo(seathubUpdates) << "update verified against its expected checksum";
        emit verified(m_available.value(QStringLiteral("version")).toString(), path);

        // One press carries through (D-41: the modal has a single action, and it started this
        // download). Nothing else ever called installDownloaded() once the modal went busy, so the
        // update stopped here with the modal claiming the installer was starting. Queued, not
        // called inline: the launch blocks while the UAC prompt is up (D-42), which does not belong
        // inside a network reply's finished handler, and the hop lets the view take "ready" first.
        QMetaObject::invokeMethod(this, [this]() { installDownloaded(); }, Qt::QueuedConnection);
    });

    return true;
}

bool UpdateFeedClient::installDownloaded()
{
    if (m_streaming || m_downloadedPath.isEmpty()) {
        return false;
    }
    if (m_state == QLatin1String(kStateInstalling)) {
        // Already launching: a second start would raise a second UAC prompt for the same update.
        return false;
    }
    if (!verifyFileChecksum(m_downloadedPath, m_expectedSha256)) {
        // Re-verified immediately before execution: the bytes on disk are the bytes that run.
        QFile::remove(m_downloadedPath);
        m_downloadedPath.clear();
        emit readyToInstallChanged();
        setFailure(SeatHubFailure::local(QString::fromLatin1(kChecksumMismatch)).toVariantMap());
        setState(QString::fromLatin1(kStateFailed));
        recordFailure("installer.verify_failed", true);
        return false;
    }

    // ADR-0077 item 7: never overlap another writer. Nothing is spent: no attempt, no decline, no report.
    if (!installerQuiescent()) {
        qCWarning(seathubUpdates) << "an earlier installer is not proven finished; not launching";
        m_manualAttempt = false;
        setState(QString::fromLatin1(kStateFailed));
        return false;
    }

    // "installing" is set here and nowhere else - where the launch is actually attempted - so the
    // modal's "Starting the installer" copy is never shown for a launch that has not happened.
    clearFailure();
    setState(QString::fromLatin1(kStateInstalling));
    qCInfo(seathubUpdates) << "starting the installer" << QDir::toNativeSeparators(m_downloadedPath);

    m_retry.launched = true;
    m_retry.launchedManual = m_manualAttempt;
    // Durable BEFORE the elevated call. Refuse the launch if its pending marker cannot be saved.
    if (!m_retry.target.isEmpty() && !m_retry.save(m_retryStatePath)) {
        m_retry.launched = false;
        setFailure(SeatHubFailure::local(QStringLiteral("The installer couldn't be started.")).toVariantMap());
        setState(QString::fromLatin1(kStateFailed)); return false;
    }
    const bool started = m_launcher(m_downloadedPath);
    if (!started) {
        // A declined UAC prompt or a failed start. The modal shows why it stopped, and the
        // verified package is kept, so Try again re-runs the launch (re-verifying first) rather
        // than downloading the installer again. The failure is this PC's, not the network's.
        qCWarning(seathubUpdates) << "the installer did not start";
        setFailure(SeatHubFailure::local(QStringLiteral("The installer couldn't be started."))
                       .toVariantMap());
        setState(QString::fromLatin1(kStateFailed));
        m_retry.noteDecline(); m_manualAttempt = false;
        reportInference("installer.elevation_declined"); persistRetry();
        return false;
    }

    qCInfo(seathubUpdates) << "installer launched; asking the application to quit";
    persistRetry(); // the launch trace (PID, creation time, uptime) must outlive this process
    emit installRequested();
    return true;
}

bool UpdateFeedClient::launchInstaller(const QString& path, const QString& arguments, InstallerTrace* trace)
{
    // The installer is unsigned (D-43), so Windows shows its own SmartScreen warning, and the
    // per-machine install raises a UAC prompt (D-42). Both are expected, not defects.
    // ADR-0070 adds only the from-version and attempt number; none of its flow is replaced.
#ifdef Q_OS_WIN
    // The measured installer manifest is asInvoker (ADR-0070/research). Elevation is supplied
    // by ShellExecuteW's runas verb for the per-machine installation, not by that manifest.
    const QString nativePath = QDir::toNativeSeparators(path);
    const std::wstring exePath = nativePath.toStdWString();
    const std::wstring params = arguments.toStdWString();
    // ShellExecuteExW with SEE_MASK_NOCLOSEPROCESS returns the installer's process handle, so the observer
    // can identify it later (ADR-0077 item 8). The call returns after the elevation prompt, SmartScreen
    // and the person have answered, so requested/returned bracket exactly the interval T005 could not read.
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = exePath.c_str();
    info.lpParameters = params.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (trace) {
        *trace = InstallerTrace{};
        trace->requestedAt = QDateTime::currentDateTimeUtc();
        trace->launcherPid = qint64(GetCurrentProcessId());
        trace->bootUptimeMs = qint64(GetTickCount64());
        DWORD session = 0;
        trace->launcherSession = ProcessIdToSessionId(GetCurrentProcessId(), &session) ? qint64(session) : -1;
    }
    const BOOL launched = ShellExecuteExW(&info);
    const DWORD lastError = launched ? 0 : GetLastError();
    // ShellExecuteExW fails with ERROR_CANCELLED (1223) when the prompt is declined.
    const bool started = launched && reinterpret_cast<INT_PTR>(info.hInstApp) > 32;
    if (trace) {
        trace->returnedAt = QDateTime::currentDateTimeUtc();
        trace->shellError = qint64(lastError);
        if (started && info.hProcess != nullptr) {
            const DWORD pid = GetProcessId(info.hProcess);
            FILETIME created{}, exited{}, kernel{}, user{};
            if (pid != 0) {
                trace->pid = qint64(pid);
                DWORD session = 0;
                trace->installerSession = ProcessIdToSessionId(pid, &session) ? qint64(session) : -1;
                if (GetProcessTimes(info.hProcess, &created, &exited, &kernel, &user))
                    trace->pidCreated = QString::number(fileTimeValue(created));
            }
        }
    }
    if (info.hProcess != nullptr) CloseHandle(info.hProcess);
    if (!started) {
        // The number, so the log tells a declined prompt from a missing or blocked file.
        qCWarning(seathubUpdates) << "ShellExecuteExW(runas) failed: last error" << lastError;
    }
#else
    Q_UNUSED(trace)
    const bool started = QProcess::startDetached(path, arguments.split(' '));
#endif
    return started;
}

// ------------------------------------------------------------------------------------ state

void UpdateFeedClient::setInstalledVersion(const QString& version)
{
    if (m_installedVersion == version) {
        return;
    }
    m_installedVersion = version;
    // ADR-0077 item 2: only an installed version EQUAL to the target ends the attempt budget.
    if (!m_retry.target.isEmpty() && isSameVersion(m_retry.target, version)) {
        m_retry.noteSuccess(); persistRetry(); clearOffer();
    }
    emit installedVersionChanged();
    emit availableUpdateChanged();
}

void UpdateFeedClient::setStreamingActive(bool active)
{
    if (m_streaming == active) {
        return;
    }
    m_streaming = active;
    if (active && !m_available.isEmpty()) {
        // Held, not dropped: D-41 shows it the moment the session ends.
        m_heldDuringSession = true;
    }
    emit blockedBySessionChanged();
}

void UpdateFeedClient::sessionFinished()
{
    m_heldDuringSession = false;
    if (!m_streaming) checkForUpdates();
}

void UpdateFeedClient::setState(const QString& state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged();
    emit availableUpdateChanged();
}

void UpdateFeedClient::setFailure(const QVariantMap& failure)
{
    m_failure = failure;
    emit failureChanged();
}

void UpdateFeedClient::clearFailure()
{
    if (m_failure.isEmpty()) {
        return;
    }
    m_failure.clear();
    emit failureChanged();
}

void UpdateFeedClient::clearOffer()
{
    if (m_available.isEmpty()) {
        return;
    }
    m_available.clear();
    emit availableUpdateChanged();
}

void UpdateFeedClient::finishWithError(const QString& error, const QString& reference)
{
    // One error shape for the whole client (D-51): the modal shows `error` plus `reference`.
    const SeatHubFailure failure = reference.isEmpty()
            ? SeatHubFailure::network(error)
            : SeatHubFailure::engine(error, reference);
    setFailure(failure.toVariantMap());
    setState(QString::fromLatin1(kStateFailed));
}
