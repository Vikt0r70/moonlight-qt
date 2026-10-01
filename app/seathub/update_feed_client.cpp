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
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
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

} // namespace

UpdateFeedClient::UpdateFeedClient(QObject* parent)
    : QObject(parent),
      m_network(new QNetworkAccessManager(this)),
      m_launcher([this](const QString& path) {
          return launchInstaller(path, installerArguments(m_installedVersion,
              qMin(UpdateRetryState::kMaxAttempts, m_retry.attemptsFailed + 1)));
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
        if (!m_available.isEmpty()) {
            m_silentRetry = !mandatory() && !m_retry.lastClass.isEmpty();
            m_state = QString::fromLatin1(m_retry.lastClass.isEmpty() ? kStateAvailable : kStateFailed);
        }
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

    if (normalizedVersion(version) == normalizedVersion(installedVersion)) {
        // Already on this build.
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
    const auto floorValue = release.value(QStringLiteral("min_version"));
    const QString floor = floorValue.isString() ? floorValue.toString() : version;
    QList<quint64> parts;
    if (!numericParts(version, parts) || !numericParts(installedVersion, parts)
        || !numericParts(floor, parts)) return {};
    offer.insert(QStringLiteral("min_version"), floor);
    offer.insert(QStringLiteral("mandatory"), isNewerVersion(floor, installedVersion));
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
    return !m_available.isEmpty()
        && isNewerVersion(m_available.value("min_version").toString(), m_installedVersion);
}
bool UpdateFeedClient::optionalOffer() const
{
    return !mandatory() && !m_optionalDismissed && !m_silentRetry && !m_available.isEmpty()
        && m_state != QLatin1String(kStateFailed)
        && isNewerVersion(m_available.value("version").toString(), m_installedVersion);
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
void UpdateFeedClient::dismissOptional()
{
    if (mandatory()) return;
    m_optionalDismissed = true; emit availableUpdateChanged();
}
bool UpdateFeedClient::openManualDownload()
{
    const auto url = manualDownloadUrl();
    return !url.isEmpty() && m_urlOpener && m_urlOpener(QUrl(url));
}
bool UpdateFeedClient::tryAgain()
{
    m_manualAttempt = true;
    return readyToInstall() ? installDownloaded() : downloadUpdate();
}
void UpdateFeedClient::setRetryStatePath(const QString& path)
{
    m_retryStatePath = path; m_retry = UpdateRetryState::load(path); m_outcomeChecked = false;
    m_available = parseRelease(QJsonDocument(QJsonObject::fromVariantMap(m_retry.lastOffer)).toJson(),
                               m_installedVersion, m_pinnedSha256);
    m_silentRetry = !mandatory() && !m_retry.lastClass.isEmpty();
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
    if (changed) { m_outcomeChecked = true; m_optionalDismissed = false; }
    m_retry.lastOffer = offer; persistRetry();
    inspectPendingLaunch();
    m_silentRetry = !mandatory() && !m_retry.lastClass.isEmpty();
    emit availableUpdateChanged();
    setState(!m_retry.lastClass.isEmpty() || m_retry.exhausted() ? kStateFailed : kStateAvailable);
    reportExhaustion();
}
void UpdateFeedClient::inspectPendingLaunch()
{
    if (m_outcomeChecked) return;
    m_outcomeChecked = true;
    if (!m_retry.launched) return;
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
    if (m_optionalDismissed || m_available.isEmpty()
        || !m_retry.automaticAttemptAllowed(m_retry.now(), m_streaming)) return;
    // Only retries run without a click. A fresh optional/required offer still presents Update.
    if (m_retry.lastClass.isEmpty()) return;
    m_manualAttempt = false;
    m_silentRetry = !mandatory();
    const QUrl url(m_available.value("url").toString());
    const QString path = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/SeatHub/") + QFileInfo(url.path()).fileName();
    m_expectedSha256 = m_available.value("sha256").toString();
    if (verifyFileChecksum(path, m_expectedSha256)) {
        m_downloadedPath = path; emit readyToInstallChanged(); installDownloaded();
    } else {
        downloadUpdate();
    }
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
            emit checkFinished();
            return;
        }

        if (reply->error() != QNetworkReply::NoError && status != 200) {
            finishWithError(reply->errorString(), QString());
            if (!m_retry.lastOffer.isEmpty()) {
                const auto cached = parseRelease(QJsonDocument(QJsonObject::fromVariantMap(m_retry.lastOffer)).toJson(),
                                                  m_installedVersion, m_pinnedSha256);
                if (!cached.isEmpty()) { adoptOffer(cached); maybeAutomaticAttempt(); }
            }
            emit checkFinished();
            return;
        }

        const QVariantMap offer = parseRelease(body, m_installedVersion, expected);
        if (offer.isEmpty()) {
            clearOffer();
            setState(QString::fromLatin1(kStateIdle));
        }
        else {
            adoptOffer(offer);
            qCInfo(seathubUpdates) << "release feed offers version"
                                   << offer.value(QStringLiteral("version")).toString();
            maybeAutomaticAttempt();
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
    emit installRequested();
    return true;
}

bool UpdateFeedClient::launchInstaller(const QString& path, const QString& arguments)
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
    const HINSTANCE rc =
        ShellExecuteW(nullptr, L"runas", exePath.c_str(), params.c_str(), nullptr, SW_SHOWNORMAL);
    const DWORD lastError = GetLastError();
    // ShellExecute returns a value <= 32 on failure (including the user declining the UAC prompt).
    const bool started = (reinterpret_cast<INT_PTR>(rc) > 32);
    if (!started) {
        // Both numbers, so the log tells a declined prompt from a missing or blocked file.
        qCWarning(seathubUpdates) << "ShellExecuteW(runas) failed: code"
                                  << reinterpret_cast<INT_PTR>(rc) << "last error" << lastError;
    }
#else
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
    if (!m_retry.target.isEmpty() && !isNewerVersion(m_retry.target, version)) {
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
    emit availableUpdateChanged(); // optionalOffer changes when a failed optional update is hidden.
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
