#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QUrl>
#include "update_retry_state.h"
#include "install_journal.h"
#include "velopack_updater.h"

#include <functional>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;

// The release feed client (ADR-0026, D-38, D-41, D-43, D-44; ADR-0077 floor and retry; ADR-0086 Velopack).
//
// One source of truth: `GET /api/releases/{app}` - the same row the website's download button
// reads. There is no generated `latest.json`, no second manifest, and no separate update
// channel (D-15, ADR-0026). The base URL is the control plane's public API host
// (`openapi.yaml` `servers[0].url`).
//
// Download and apply are Velopack's (ADR-0086): the release row's `url` is the release's `Setup.exe`, the
// folder beside it carries Velopack's `releases.win.json` and the full package, and `Update.exe` swaps the
// app folder after this process exits (a failed swap relaunches the old version). Everything else - the
// floor, the mandatory window, newer-is-current, the bounded retry - stays here.
//
// Two rules this class exists to hold, both of which fail closed:
//
//   * Integrity (D-43). A release row with no SHA-256 is refused before anything is downloaded; the
//     digest comes from the feed row or the pin baked in at release prep. The package itself is
//     verified by Velopack against the digest in the release manifest it downloads. Code signing is
//     deliberately absent and its absence is expected (D-43).
//   * Timing (D-41, Pitfall 8, T-03-17). While a stream is running nothing is offered and
//     nothing is installed: replacing the binary a live session is running from would be
//     tampering with the running application. A release detected while streaming is held and
//     offered after the session ends.
class UpdateFeedClient : public QObject
{
    Q_OBJECT

    /// idle | checking | available | downloading | verifying | ready | installing | failed
    /// `ready` is transient: a downloaded release moves on to `installing` by itself. `installing`
    /// is set only when the hand-off to Velopack's updater is actually attempted.
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    /// version, url, sha256, notes, mandatory. Empty when there is nothing to offer.
    Q_PROPERTY(QVariantMap availableUpdate READ availableUpdate NOTIFY availableUpdateChanged)
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QVariantMap failure READ failure NOTIFY failureChanged)
    Q_PROPERTY(QString installedVersion READ installedVersion WRITE setInstalledVersion NOTIFY installedVersionChanged)
    /// True while a stream is running; updates are neither offered nor installed (D-41).
    Q_PROPERTY(bool blockedBySession READ blockedBySession NOTIFY blockedBySessionChanged)
    /// True once a release is downloaded and waiting to be applied.
    Q_PROPERTY(bool readyToInstall READ readyToInstall NOTIFY readyToInstallChanged)
    /// ADR-0077: the newest published release is the floor. Every offer is mandatory: an installed
    /// version older than the current release is blocked by the update window. A newer one is current.
    Q_PROPERTY(bool mandatory READ mandatory NOTIFY availableUpdateChanged)
    Q_PROPERTY(bool exhausted READ exhausted NOTIFY retryChanged)
    Q_PROPERTY(QString nextAttemptText READ nextAttemptText NOTIFY retryChanged)
    Q_PROPERTY(QString manualDownloadUrl READ manualDownloadUrl NOTIFY availableUpdateChanged)

public:
    explicit UpdateFeedClient(QObject* parent = nullptr);
    ~UpdateFeedClient() override;

    /// The control plane's public API host (ADR-0018, `openapi.yaml` `servers[0].url`).
    static QString defaultBaseUrl();
    /// The feed path for an app: `/api/releases/{app}` (`openapi.yaml`).
    static QString feedPath(const QString& app);
    /// The app name this client tracks: the customer-facing client, `client`.
    static QString appName();

    /// A candidate version counts as an offer only when it is newer than the installed one
    /// (ADR-0077 Amendment R5: a newer install is never moved down; roll forward instead).
    static bool isNewerVersion(const QString& candidate, const QString& current);
    /// ADR-0077 Amendment R5: an install is current when its version is equal to or newer than the
    /// release's (dotted numbers). An unparseable version is never proven current.
    static bool isCurrentVersion(const QString& installed, const QString& release);
    /// Two spellings of the same dotted version (`0.10` equals `0.10.0`).
    static bool isSameVersion(const QString& left, const QString& right);

    /// Parses a feed response into an offer, or an empty map when there is nothing to do.
    /// A 404 body ("nothing published yet") and a same-version row both land here as empty.
    static QVariantMap parseRelease(const QByteArray& json, const QString& installedVersion,
                                    const QString& pinnedSha256);
    /// D-43: the whole integrity check. Returns false when either side is empty.
    static bool verifyFileChecksum(const QString& path, const QString& expectedSha256);
    static QString fileSha256(const QString& path);

    Q_INVOKABLE bool checkForUpdates();
    /// Downloads the offered release through Velopack, with progress. `expectedSha256` overrides the
    /// digest the offer carries; it exists so the caller can hand in the release pin from
    /// `seathub-ops/pins.yaml` at release prep time. An empty expected digest is a refusal.
    Q_INVOKABLE bool downloadUpdate(const QString& url, const QString& expectedSha256);
    Q_INVOKABLE bool downloadUpdate();
    /// Hands the downloaded release to Velopack's updater and asks the application to quit so it can
    /// replace the app folder (no administrator prompt, ADR-0086). Runs by itself once a download
    /// completes (one press, D-41). When the hand-off fails the running version is untouched and the
    /// failure is counted under the bounded retry.
    Q_INVOKABLE bool installDownloaded();
    /// ADR-0077 item 5: re-reads the current release from the feed, then downloads THAT row's release
    /// and applies it. No cached address and no cached download is used. True when the attempt
    /// started; an unreachable feed starts nothing, shows why and consumes no attempt.
    Q_INVOKABLE bool tryAgain();
    /// Re-reads the current release from the feed, then opens only that row's installer address after
    /// the https and control-plane host check (client.md § Website links). Never opens a cached address.
    Q_INVOKABLE bool openManualDownload();
    bool mandatory() const;
    bool exhausted() const { return m_retry.exhausted(); }
    QString nextAttemptText() const;
    QString manualDownloadUrl() const;
    using UrlOpener = std::function<bool(const QUrl&)>;
    void setUrlOpener(UrlOpener opener) { m_urlOpener = std::move(opener); }
    using InstallerReporter = std::function<bool(const InstallJournalRecord&)>;
    void setInstallerReporter(InstallerReporter reporter) { m_reporter = std::move(reporter); }
    void setRetryStatePath(const QString& path);
    /// Velopack writes no install journal (ADR-0086); kept only so existing callers still compile.
    void setJournalFolder(const QString&) {}
    void setRetryClock(std::function<QDateTime()> clock) { m_retry.setClock(std::move(clock)); }
    UpdateRetryState retryState() const { return m_retry; }
    void reportPendingInstallerEvents() { reportExhaustion(); }

    QString state() const { return m_state; }
    QVariantMap availableUpdate() const { return m_available; }
    int progress() const { return m_progress; }
    QVariantMap failure() const { return m_failure; }
    QString installedVersion() const { return m_installedVersion; }
    bool blockedBySession() const { return m_streaming; }
    bool readyToInstall() const { return m_staged; }

    /// The Velopack seam: downloads a release and hands it to the updater. The default is the real
    /// Velopack applier; tests replace it with a fake so that no network, installed copy or updater runs.
    void setUpdateApplier(std::shared_ptr<VelopackUpdater::UpdateApplier> applier) { m_applier = std::move(applier); }
    /// What the application does once the updater has the release: quit, so `Update.exe` can replace the
    /// app folder (it waits at most 60 s). The default is `QCoreApplication::quit`; tests replace it.
    using ExitHandler = std::function<void()>;
    void setExitHandler(ExitHandler handler) { m_exit = std::move(handler); }

    void setInstalledVersion(const QString& version);
    void setPinnedSha256(const QString& sha256) { m_pinnedSha256 = sha256; }
    void setBaseUrl(const QString& baseUrl) { m_baseUrl = baseUrl; }
    QString baseUrl() const { return m_baseUrl; }

    /// D-41 / Pitfall 8: while this is true there is no offer and no install.
    void setStreamingActive(bool active);
    /// Called when a session ends: a release that arrived during the session is offered now
    /// (D-41), which is why this re-checks rather than trusting what was held.
    void sessionFinished();

signals:
    void stateChanged();
    void availableUpdateChanged();
    void progressChanged();
    void failureChanged();
    void installedVersionChanged();
    void blockedBySessionChanged();
    void readyToInstallChanged();
    void checkFinished();
    void retryChanged();
    /// Emitted after a completed download. The client hands the release to the updater itself straight
    /// after; nothing has to answer this for the update to proceed.
    void downloaded(QString version);
    /// The updater has the release: the application should exit so it can replace the app folder.
    void installRequested();

private:
    struct WorkerLink;
    void onDownloadProgress(int percent);
    void onDownloadFinished(bool ok, const QString& resolvedVersion, const QString& error);
    void startFreshDownload();
    void finishPendingActions(bool feedRead);
    void adoptOffer(const QVariantMap& offer);
    void persistRetry();
    void inspectPendingLaunch();
    void maybeAutomaticAttempt();
    void recordFailure(const QString& failureClass, bool permanent = false);
    void reportFailure(const QString& failureClass);
    void reportExhaustion();

    void setState(const QString& state);
    void setFailure(const QVariantMap& failure);
    void clearFailure();
    void clearOffer();
    void finishWithError(const QString& error, const QString& reference);

    QNetworkAccessManager* m_network = nullptr;
    QNetworkReply* m_reply = nullptr;
    std::shared_ptr<VelopackUpdater::UpdateApplier> m_applier;
    ExitHandler m_exit;
    std::shared_ptr<WorkerLink> m_link;
    // True while a Velopack download runs on its worker thread.
    bool m_downloading = false;
    // True once a release is downloaded and waiting to be handed to the updater.
    bool m_staged = false;

    QString m_baseUrl;
    QString m_installedVersion;
    QString m_pinnedSha256;
    QString m_expectedSha256;
    QString m_state;

    bool m_streaming = false;
    // A release seen while a stream was running, offered once the session ends (D-41).
    bool m_heldDuringSession = false;

    int m_progress = 0;
    QVariantMap m_available;
    QVariantMap m_failure;
    UpdateRetryState m_retry;
    QString m_retryStatePath = UpdateRetryState::defaultPath();
    bool m_manualAttempt = false, m_outcomeChecked = false;
    // True only while the offer in `m_available` came from a feed read made by THIS process. An offer
    // restored from the retry state keeps the window up but never launches an installer (ADR-0077 item 3).
    bool m_offerFresh = false;
    // Try again and Download first re-read the feed; these carry the press across that read.
    bool m_pendingTryAgain = false, m_pendingDownload = false;
    UrlOpener m_urlOpener;
    InstallerReporter m_reporter;
};
