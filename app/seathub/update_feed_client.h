#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;

// The release feed client (ADR-0026, D-38, D-41, D-43, D-44).
//
// One source of truth: `GET /api/releases/{app}` - the same row the website's download button
// reads. There is no generated `latest.json`, no second manifest, and no separate update
// channel (D-15, ADR-0026). The base URL is the control plane's public API host
// (`openapi.yaml` `servers[0].url`).
//
// Two rules this class exists to hold, both of which fail closed:
//
//   * Integrity (D-43). A downloaded package is never installed without a SHA-256 that matches.
//     The expected digest comes from the release feed's integrity field when the feed carries
//     one, and otherwise from the pin baked in at release prep from `seathub-ops/pins.yaml`.
//     When neither is available - which is the state of the world until release prep closes -
//     the download is reported as uninstallable and nothing is executed. Code signing is
//     deliberately absent and its absence is expected (D-43); the checksum is the whole of the
//     integrity story.
//   * Timing (D-41, Pitfall 8, T-03-17). While a stream is running nothing is offered and
//     nothing is installed: replacing the binary a live session is running from would be
//     tampering with the running application. A release detected while streaming is held and
//     offered after the session ends.
class UpdateFeedClient : public QObject
{
    Q_OBJECT

    /// idle | checking | available | downloading | verifying | ready | installing | failed
    /// `ready` is transient: a verified download moves on to `installing` by itself. `installing`
    /// is set only when the installer's launch is actually attempted.
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    /// version, url, sha256, notes, rollback. Empty when there is nothing to offer.
    Q_PROPERTY(QVariantMap availableUpdate READ availableUpdate NOTIFY availableUpdateChanged)
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QVariantMap failure READ failure NOTIFY failureChanged)
    Q_PROPERTY(QString installedVersion READ installedVersion WRITE setInstalledVersion NOTIFY installedVersionChanged)
    /// True while a stream is running; updates are neither offered nor installed (D-41).
    Q_PROPERTY(bool blockedBySession READ blockedBySession NOTIFY blockedBySessionChanged)
    /// True once a verified package is on disk and waiting to be run.
    Q_PROPERTY(bool readyToInstall READ readyToInstall NOTIFY readyToInstallChanged)

public:
    explicit UpdateFeedClient(QObject* parent = nullptr);

    /// The control plane's public API host (ADR-0018, `openapi.yaml` `servers[0].url`).
    static QString defaultBaseUrl();
    /// The feed path for an app: `/api/releases/{app}` (`openapi.yaml`).
    static QString feedPath(const QString& app);
    /// The app name this client tracks: the customer-facing client, `client`.
    static QString appName();

    /// D-46/D-44: a candidate version counts as an offer when it differs from the installed one.
    /// A rollback is an ordinary operator action - the client installs whatever the feed's
    /// current row says, including an earlier version, once the checksum verifies.
    static bool isNewerVersion(const QString& candidate, const QString& current);
    /// True when the feed's row is an earlier version than the one installed (a rollback).
    static bool isRollback(const QString& candidate, const QString& current);

    /// Parses a feed response into an offer, or an empty map when there is nothing to do.
    /// A 404 body ("nothing published yet") and a same-version row both land here as empty.
    static QVariantMap parseRelease(const QByteArray& json, const QString& installedVersion,
                                    const QString& pinnedSha256);
    /// D-43: the whole integrity check. Returns false when either side is empty.
    static bool verifyFileChecksum(const QString& path, const QString& expectedSha256);
    static QString fileSha256(const QString& path);

    Q_INVOKABLE bool checkForUpdates();
    /// Downloads the offered package and verifies it. `expectedSha256` overrides the digest the
    /// offer carries; it exists so the caller can hand in the release pin from
    /// `seathub-ops/pins.yaml` at release prep time. An empty expected digest is a refusal.
    Q_INVOKABLE bool downloadUpdate(const QString& url, const QString& expectedSha256);
    Q_INVOKABLE bool downloadUpdate();
    /// Launches the verified installer and asks the application to quit so the installer can
    /// replace it. The installer is unsigned (D-43), so Windows shows its own SmartScreen
    /// warning and the per-machine install raises a UAC prompt (D-42) - both expected.
    /// Runs by itself once a download verifies (one press, D-41). Callers only need it to retry
    /// a launch that failed - a declined UAC prompt - while the verified package is still on disk.
    Q_INVOKABLE bool installDownloaded();

    QString state() const { return m_state; }
    QVariantMap availableUpdate() const { return m_available; }
    int progress() const { return m_progress; }
    QVariantMap failure() const { return m_failure; }
    QString installedVersion() const { return m_installedVersion; }
    bool blockedBySession() const { return m_streaming; }
    bool readyToInstall() const { return !m_downloadedPath.isEmpty(); }
    QString downloadedPath() const { return m_downloadedPath; }

    /// Starts the verified installer at `path`; false when it did not start (including a declined
    /// UAC prompt). The default is `launchInstaller`. Tests replace it so that no real installer
    /// runs and no UAC prompt is raised.
    using InstallerLauncher = std::function<bool(const QString& path)>;
    void setInstallerLauncher(InstallerLauncher launcher) { m_launcher = std::move(launcher); }

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
    /// Emitted after a verified download. The client starts the installer itself straight after;
    /// nothing has to answer this for the update to proceed.
    void verified(QString version, QString path);
    /// The application should exit so the installer can replace the running binary.
    void installRequested();

private:
    /// The production launcher: an elevated start of the per-machine installer (D-42).
    static bool launchInstaller(const QString& path);

    void setState(const QString& state);
    void setFailure(const QVariantMap& failure);
    void clearFailure();
    void clearOffer();
    void finishWithError(const QString& error, const QString& reference);

    QNetworkAccessManager* m_network = nullptr;
    QNetworkReply* m_reply = nullptr;
    QFile* m_downloadFile = nullptr;
    InstallerLauncher m_launcher;

    QString m_baseUrl;
    QString m_installedVersion;
    QString m_pinnedSha256;
    QString m_expectedSha256;
    QString m_downloadedPath;
    QString m_state;

    bool m_streaming = false;
    // A release seen while a stream was running, offered once the session ends (D-41).
    bool m_heldDuringSession = false;

    int m_progress = 0;
    QVariantMap m_available;
    QVariantMap m_failure;
};
