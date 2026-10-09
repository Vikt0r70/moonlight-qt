#include "update_feed_client.h"

#include "error_map.h"
#include "seathub_version.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <thread>

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

} // namespace

// The worker thread of a Velopack download reaches the client only through this link, under its
// mutex, and the client cuts it in its destructor: a download that outlives the client posts nothing.
struct UpdateFeedClient::WorkerLink
{
    QMutex mutex;
    UpdateFeedClient* target = nullptr;
};

UpdateFeedClient::UpdateFeedClient(QObject* parent)
    : QObject(parent),
      m_network(new QNetworkAccessManager(this)),
      m_applier(VelopackUpdater::createApplier(QUrl(defaultBaseUrl()).host())),
      m_exit([] { QCoreApplication::quit(); }),
      m_link(std::make_shared<WorkerLink>()),
      m_baseUrl(defaultBaseUrl()),
      m_installedVersion(QString::fromLatin1(SEATHUB_VERSION)),
      m_state(QString::fromLatin1(kStateIdle))
{
    m_link->target = this;
    m_retry = UpdateRetryState::load(m_retryStatePath);
    m_urlOpener = &QDesktopServices::openUrl;
    if (!m_retry.lastOffer.isEmpty()) {
        m_available = parseRelease(QJsonDocument(QJsonObject::fromVariantMap(m_retry.lastOffer)).toJson(),
                                   m_installedVersion, m_pinnedSha256);
        if (!m_available.isEmpty())
            m_state = QString::fromLatin1(m_retry.lastClass.isEmpty() ? kStateAvailable : kStateFailed);
    }
}

UpdateFeedClient::~UpdateFeedClient()
{
    QMutexLocker lock(&m_link->mutex);
    m_link->target = nullptr;
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

bool UpdateFeedClient::isCurrentVersion(const QString& installed, const QString& release)
{
    QList<quint64> have;
    QList<quint64> want;
    if (!numericParts(installed, have) || !numericParts(release, want)) {
        // Unparseable is never proven current; the caller decides what that means for an offer.
        return false;
    }
    return !isNewerVersion(release, installed);
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
        || isCurrentVersion(installedVersion, version)) {
        // ADR-0077 Amendment 2026-10-06 R5: an install equal to or newer than the current release is
        // current. It gets no offer and is never moved down: a bad release is fixed by publishing a newer
        // fixed build (roll forward).
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
    QList<quint64> parts;
    if (!numericParts(version, parts) || !numericParts(installedVersion, parts)) return {};
    // ADR-0077: the current release IS the floor. The row's own `min_version` (still sent for installed
    // 0.1.16/0.1.27 readers) and any value cached by an older build are ignored, so an installed version
    // older than the release is always required.
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
bool UpdateFeedClient::openManualDownload()
{
    // ADR-0077 item 5: read the current release first; open only the row just read.
    m_pendingDownload = true;
    if (m_reply != nullptr) return true; // that read carries this press
    if (!checkForUpdates()) { m_pendingDownload = false; return false; }
    return true;
}
bool UpdateFeedClient::tryAgain()
{
    // ADR-0077 item 5: the release of the row read now, downloaded now, never a cached one.
    m_manualAttempt = true;
    m_pendingTryAgain = true;
    if (m_reply != nullptr) return true;
    if (!checkForUpdates()) { m_pendingTryAgain = false; m_manualAttempt = false; return false; }
    return true;
}
void UpdateFeedClient::startFreshDownload()
{
    if (m_staged) {
        m_staged = false;
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
    if (changed && m_staged) {
        // A release downloaded for an earlier target is not this one.
        m_staged = false;
        emit readyToInstallChanged();
    }
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
    m_outcomeChecked = true;
    if (!m_retry.launched) return;
    // ADR-0086: a release was handed to Velopack's updater, which swaps the app folder after the old
    // process exits and relaunches SeatHub. A successful swap runs the new version, whose target is reached
    // (so this method never sees `launched` for it). This process is a start of the OLD version with that
    // hand-off still pending: the swap did not happen and the old version is intact. It is one failed
    // attempt under the bounded retry; a manual press stays free.
    const bool manual = m_retry.launchedManual;
    m_retry.noteFailure("installer.unknown", false, manual, true);
    reportFailure("installer.unknown");
    persistRetry();
}
void UpdateFeedClient::reportFailure(const QString& failureClass)
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
    if (m_retry.exhausted() && !m_retry.exhaustedReported) reportFailure("installer.retries_exhausted");
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
    // ADR-0077 item 5: always the release of the row just read, downloaded now.
    startFreshDownload();
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
    if (m_reply != nullptr || m_downloading || m_state == QLatin1String(kStateInstalling)) {
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
    if (m_reply != nullptr || m_downloading) {
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
        // D-43: a release row with no digest is never downloaded as an update candidate.
        qCWarning(seathubUpdates) << "refused a download with no expected checksum";
        setFailure(SeatHubFailure::local(QString::fromLatin1(kNoChecksum)).toVariantMap());
        setState(QString::fromLatin1(kStateFailed));
        recordFailure("installer.verify_failed", true);
        return false;
    }

    QUrl setup(url);
    if (!setup.isValid()) {
        finishWithError(QStringLiteral("The release download address is unusable."), QString());
        recordFailure("installer.unknown");
        return false;
    }

    clearFailure();
    m_expectedSha256 = expectedSha256.trimmed();
    if (m_staged) {
        m_staged = false;
        emit readyToInstallChanged();
    }

    m_progress = 0;
    emit progressChanged();
    setState(QString::fromLatin1(kStateDownloading));
    m_downloading = true;

    // Velopack downloads from the folder that holds the row's Setup.exe and reports progress as it goes.
    // The call blocks, so it runs on its own thread; everything it reports is posted back here.
    const QString folder = VelopackUpdater::feedFolderOf(setup).toString();
    const std::shared_ptr<VelopackUpdater::UpdateApplier> applier = m_applier;
    const std::shared_ptr<WorkerLink> link = m_link;
    std::thread([applier, link, folder]() {
        QString resolvedVersion, error;
        const bool ok = applier->download(folder, [link](int percent) {
            QMutexLocker lock(&link->mutex);
            if (UpdateFeedClient* client = link->target)
                QMetaObject::invokeMethod(client, [client, percent]() { client->onDownloadProgress(percent); },
                                          Qt::QueuedConnection);
        }, &resolvedVersion, &error);
        QMutexLocker lock(&link->mutex);
        if (UpdateFeedClient* client = link->target)
            QMetaObject::invokeMethod(client, [client, ok, resolvedVersion, error]() {
                client->onDownloadFinished(ok, resolvedVersion, error);
            }, Qt::QueuedConnection);
    }).detach();

    return true;
}

void UpdateFeedClient::onDownloadProgress(int percent)
{
    if (!m_downloading || percent == m_progress) {
        return;
    }
    m_progress = qBound(0, percent, 100);
    emit progressChanged();
}

void UpdateFeedClient::onDownloadFinished(bool ok, const QString& resolvedVersion, const QString& error)
{
    m_downloading = false;
    if (!ok) {
        qCWarning(seathubUpdates) << "the release could not be downloaded:" << error;
        finishWithError(error, QString());
        recordFailure("installer.unknown");
        return;
    }
    const QString offered = m_available.value(QStringLiteral("version")).toString();
    if (!isSameVersion(resolvedVersion, offered)) {
        // The release folder does not carry the release the row names (for example the row was updated
        // before the folder was). Nothing was applied; it is an ordinary failed attempt.
        qCWarning(seathubUpdates) << "the release folder carries" << resolvedVersion << "but the row names" << offered;
        finishWithError(QStringLiteral("The update on the server isn't the release it listed. Try again in a moment."),
                        QString());
        recordFailure("installer.unknown");
        return;
    }

    m_staged = true;
    m_progress = 100;
    emit progressChanged();
    emit readyToInstallChanged();
    // "ready" before `downloaded`, so anything answering the signal sees the state it describes.
    setState(QString::fromLatin1(kStateReady));
    qCInfo(seathubUpdates) << "update downloaded through Velopack:" << offered;
    emit downloaded(offered);

    // One press carries through (D-41: the modal has a single action, and it started this download).
    // Queued, not called inline: the hop lets the view take "ready" first.
    QMetaObject::invokeMethod(this, [this]() { installDownloaded(); }, Qt::QueuedConnection);
}

bool UpdateFeedClient::installDownloaded()
{
    if (m_streaming || !m_staged) {
        return false;
    }
    if (m_state == QLatin1String(kStateInstalling)) {
        // Already handing over: a second hand-off would start the updater twice for the same update.
        return false;
    }

    // "installing" is set here and nowhere else - where the hand-off is actually attempted - so the
    // modal's installing copy is never shown for a hand-off that has not happened.
    clearFailure();
    setState(QString::fromLatin1(kStateInstalling));
    qCInfo(seathubUpdates) << "handing the update to Velopack's updater";

    m_retry.launched = true;
    m_retry.launchedManual = m_manualAttempt;
    // Durable BEFORE the hand-off. Refuse it if its pending marker cannot be saved.
    if (!m_retry.target.isEmpty() && !m_retry.save(m_retryStatePath)) {
        m_retry.launched = false;
        setFailure(SeatHubFailure::local(QStringLiteral("The installer couldn't be started.")).toVariantMap());
        setState(QString::fromLatin1(kStateFailed)); return false;
    }
    QString error;
    if (!m_applier->applyAfterExit(&error)) {
        // Nothing changed: the running version is intact and keeps running. The modal shows why it
        // stopped, and the failure is one attempt under the bounded retry (a manual press is free).
        qCWarning(seathubUpdates) << "Velopack did not take the update:" << error;
        setFailure(SeatHubFailure::local(QStringLiteral("The installer couldn't be started.")).toVariantMap());
        setState(QString::fromLatin1(kStateFailed));
        recordFailure("installer.unknown");
        return false;
    }

    qCInfo(seathubUpdates) << "the updater has the release; asking the application to quit";
    persistRetry(); // the pending marker must outlive this process
    emit installRequested();
    if (m_exit) {
        QMetaObject::invokeMethod(this, [this]() { m_exit(); }, Qt::QueuedConnection);
    }
    return true;
}

// ------------------------------------------------------------------------------------ state

void UpdateFeedClient::setInstalledVersion(const QString& version)
{
    if (m_installedVersion == version) {
        return;
    }
    m_installedVersion = version;
    // ADR-0077 Amendment R5: an installed version equal to or newer than the target ends the attempt budget.
    if (!m_retry.target.isEmpty() && isCurrentVersion(version, m_retry.target)) {
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
