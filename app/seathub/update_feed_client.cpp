#include "update_feed_client.h"

#include "error_map.h"
#include "seathub_version.h"

#include <QCryptographicHash>
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
      m_launcher(&UpdateFeedClient::launchInstaller),
      m_baseUrl(defaultBaseUrl()),
      m_installedVersion(QString::fromLatin1(SEATHUB_VERSION)),
      m_state(QString::fromLatin1(kStateIdle))
{
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

bool UpdateFeedClient::checkForUpdates()
{
    if (m_streaming) {
        // D-41 / Pitfall 8 / T-03-17: nothing is offered while a stream runs. The offer is made
        // once the session ends (`sessionFinished`).
        m_heldDuringSession = true;
        qCInfo(seathubUpdates) << "update check deferred: a stream is running";
        return false;
    }
    if (m_reply != nullptr || m_downloadFile != nullptr) {
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
            emit checkFinished();
            return;
        }

        const QVariantMap offer = parseRelease(body, m_installedVersion, expected);
        if (offer.isEmpty()) {
            clearOffer();
            setState(QString::fromLatin1(kStateIdle));
        }
        else {
            m_available = offer;
            emit availableUpdateChanged();
            setState(QString::fromLatin1(kStateAvailable));
            qCInfo(seathubUpdates) << "release feed offers version"
                                   << offer.value(QStringLiteral("version")).toString();
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
        return false;
    }

    QUrl feed(url);
    if (!feed.isValid()) {
        finishWithError(QStringLiteral("The release download address is unusable."), QString());
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
        return false;
    }

    // "installing" is set here and nowhere else - where the launch is actually attempted - so the
    // modal's "Starting the installer" copy is never shown for a launch that has not happened.
    clearFailure();
    setState(QString::fromLatin1(kStateInstalling));
    qCInfo(seathubUpdates) << "starting the installer" << QDir::toNativeSeparators(m_downloadedPath);

    const bool started = m_launcher(m_downloadedPath);
    if (!started) {
        // A declined UAC prompt or a failed start. The modal shows why it stopped, and the
        // verified package is kept, so Try again re-runs the launch (re-verifying first) rather
        // than downloading the installer again. The failure is this PC's, not the network's.
        qCWarning(seathubUpdates) << "the installer did not start";
        setFailure(SeatHubFailure::local(QStringLiteral("The installer couldn't be started."))
                       .toVariantMap());
        setState(QString::fromLatin1(kStateFailed));
        return false;
    }

    qCInfo(seathubUpdates) << "installer launched; asking the application to quit";
    emit installRequested();
    return true;
}

bool UpdateFeedClient::launchInstaller(const QString& path)
{
    // The installer is unsigned (D-43), so Windows shows its own SmartScreen warning, and the
    // per-machine install raises a UAC prompt (D-42). Both are expected, not defects. No
    // arguments are invented for it - the installer's own flow runs as published.
#ifdef Q_OS_WIN
    // The installer's manifest requires administrator (per-machine, D-42). CreateProcess - which
    // QProcess::startDetached uses - cannot launch an elevation-required binary; it fails with
    // ERROR_ELEVATION_REQUIRED and the update hangs at "Starting the installer". Only
    // ShellExecute's "runas" verb raises the UAC prompt that lets the install proceed.
    const QString nativePath = QDir::toNativeSeparators(path);
    const std::wstring exePath = nativePath.toStdWString();
    const HINSTANCE rc =
        ShellExecuteW(nullptr, L"runas", exePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    const DWORD lastError = GetLastError();
    // ShellExecute returns a value <= 32 on failure (including the user declining the UAC prompt).
    const bool started = (reinterpret_cast<INT_PTR>(rc) > 32);
    if (!started) {
        // Both numbers, so the log tells a declined prompt from a missing or blocked file.
        qCWarning(seathubUpdates) << "ShellExecuteW(runas) failed: code"
                                  << reinterpret_cast<INT_PTR>(rc) << "last error" << lastError;
    }
#else
    const bool started = QProcess::startDetached(path, QStringList());
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
    emit installedVersionChanged();
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
    if (m_heldDuringSession) {
        m_heldDuringSession = false;
        qCInfo(seathubUpdates) << "session ended - checking the release feed";
        checkForUpdates();
    }
}

void UpdateFeedClient::setState(const QString& state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged();
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
