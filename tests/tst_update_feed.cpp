// Release-feed and forced-update tests (Plan 03-04 Task 2).
//
// Two things are being proven here, and they are the two the threat register cares about:
//
//   * T-03-14 (tampering with the downloaded binary): a release row with no SHA-256 is refused before
//     anything is downloaded (D-43), and the digest helpers are verified against NIST's published
//     vectors, so a test that passed by comparing a hash against itself cannot happen. The package
//     itself is Velopack's to verify (ADR-0086).
//   * T-03-17 / Pitfall 8 (replacing the binary under a live stream): while a session is running
//     nothing is checked, nothing is downloaded, nothing is installed, and the modal renders
//     nothing.
//
// The feed's own HTTP path is exercised only against a closed local port - the release feed is a
// real service and a unit test has no business calling it. Download and apply go through the
// Velopack seam (`VelopackUpdater::UpdateApplier`); every test here drives a fake one, so no network,
// installed copy or updater runs, and no test needs the Velopack library.

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QMutex>
#include <QMutexLocker>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>

#include <atomic>
#include <functional>
#include <memory>

#include "seathub/update_feed_client.h"
#include "seathub/velopack_updater.h"

namespace {
QString guiDir()
{
#ifdef FORK_ROOT
    const QString fromRoot =
        QDir(QString::fromUtf8(FORK_ROOT) + QStringLiteral("/app/gui")).absolutePath();
    if (QDir(fromRoot).exists()) {
        return fromRoot;
    }
#endif
    return QDir(QCoreApplication::applicationDirPath() + QStringLiteral("/../app/gui"))
        .absolutePath();
}

QByteArray feedRow(const QString& version, const QString& url = QStringLiteral("https://example.invalid/SeatHub-0.2.0.exe"),
                   const QString& notes = QString())
{
    return QStringLiteral("{\"app\":\"client\",\"platform\":\"windows-x86_64\",\"version\":\"%1\","
                          "\"url\":\"%2\",\"signature\":null,\"notes\":%3,"
                          "\"published_at\":\"2026-09-19T00:00:00Z\"}")
        .arg(version, url,
             notes.isEmpty() ? QStringLiteral("null") : QStringLiteral("\"%1\"").arg(notes))
        .toUtf8();
}

// The modal is a full-window overlay and says `anchors.fill: parent`; in the product it is
// instantiated inside the application window's item tree. Created on its own it would have no
// parent to fill, so the tests give it a host item of the same size the window has.
QQuickItem* hostItem(QObject* modal)
{
    QQuickItem* item = qobject_cast<QQuickItem*>(modal);
    if (item == nullptr) {
        return nullptr;
    }
    QQuickItem* host = new QQuickItem;
    host->setWidth(960);
    host->setHeight(640);
    item->setParentItem(host);
    return item;
}

// NIST's published SHA-256 vector for "abc".
const char* const kAbcSha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

// The release row the tests publish: Setup.exe sits in a folder on the control plane's host, and the
// Velopack feed (releases.win.json and the full package) is the folder beside it.
const char* const kSetupUrl = "https://api-sevenhills.damra.co/releases/client/SeatHub-Setup-0.2.0.exe";
const char* const kSetupFolder = "https://api-sevenhills.damra.co/releases/client/";

// The Velopack seam, scripted. `download` runs on the client's worker thread exactly as the real one
// does; `hold` lets a test keep a download in flight.
class FakeApplier : public VelopackUpdater::UpdateApplier
{
public:
    bool downloadOk = true;
    bool applyOk = true;
    QString carries = QStringLiteral("0.2.0"); // the version the release folder actually carries
    QSemaphore* hold = nullptr;
    std::function<void()> onApply;
    std::atomic<int> downloads{0};
    std::atomic<int> applies{0};

    bool download(const QString& folder, const std::function<void(int)>& progress, QString* version,
                  QString* error) override
    {
        ++downloads;
        {
            QMutexLocker lock(&m_mutex);
            m_folders.append(folder);
        }
        if (hold != nullptr)
            hold->acquire();
        progress(40);
        if (!downloadOk) {
            *error = QStringLiteral("the update folder could not be read");
            return false;
        }
        progress(100);
        *version = carries;
        return true;
    }

    bool applyAfterExit(QString* error) override
    {
        ++applies;
        if (onApply)
            onApply();
        if (!applyOk) {
            *error = QStringLiteral("the app folder is in use");
            return false;
        }
        return true;
    }

    QStringList seenFolders()
    {
        QMutexLocker lock(&m_mutex);
        return m_folders;
    }

private:
    QMutex m_mutex;
    QStringList m_folders;
};

// Puts a fake applier and a counting exit handler on `client`: the application quitting is what the
// real exit handler does, and a unit test must never do that.
std::shared_ptr<FakeApplier> useFake(UpdateFeedClient& client, int* exits)
{
    auto applier = std::make_shared<FakeApplier>();
    client.setUpdateApplier(applier);
    client.setExitHandler([exits]() {
        if (exits != nullptr)
            ++*exits;
    });
    return applier;
}

// Every piece of text the modal currently shows, for asserting what the customer reads.
QStringList visibleTexts(QObject* root)
{
    QStringList texts;
    for (QObject* node : root->findChildren<QObject*>()) {
        QQuickItem* item = qobject_cast<QQuickItem*>(node);
        if (item != nullptr && node->inherits("QQuickText") && item->isVisible()) {
            texts.append(node->property("text").toString());
        }
    }
    return texts;
}
}

// The forced-update modal reads the release client through these members only, so the QML tests
// can drive it without a feed, a socket or a download (D-35's duck-typed boundary).
class FakeUpdates : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state READ state WRITE setState NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap availableUpdate READ availableUpdate WRITE setAvailableUpdate NOTIFY availableUpdateChanged)
    Q_PROPERTY(int progress READ progress WRITE setProgress NOTIFY progressChanged)
    Q_PROPERTY(QVariantMap failure READ failure WRITE setFailure NOTIFY failureChanged)
    Q_PROPERTY(bool blockedBySession READ blockedBySession WRITE setBlockedBySession NOTIFY blockedBySessionChanged)
    Q_PROPERTY(bool readyToInstall READ readyToInstall WRITE setReadyToInstall NOTIFY readyToInstallChanged)
    Q_PROPERTY(bool mandatory READ mandatory CONSTANT)
    Q_PROPERTY(bool optionalOffer READ optionalOffer CONSTANT)
    Q_PROPERTY(bool exhausted READ exhausted CONSTANT)
    Q_PROPERTY(QString nextAttemptText READ nextAttemptText CONSTANT)

public:
    QString state() const { return m_state; }
    void setState(const QString& state) { m_state = state; emit stateChanged(); }
    QVariantMap availableUpdate() const { return m_available; }
    void setAvailableUpdate(const QVariantMap& offer) { m_available = offer; emit availableUpdateChanged(); }
    int progress() const { return m_progress; }
    void setProgress(int progress) { m_progress = progress; emit progressChanged(); }
    QVariantMap failure() const { return m_failure; }
    void setFailure(const QVariantMap& failure) { m_failure = failure; emit failureChanged(); }
    bool blockedBySession() const { return m_blocked; }
    void setBlockedBySession(bool blocked) { m_blocked = blocked; emit blockedBySessionChanged(); }
    bool readyToInstall() const { return m_readyToInstall; }
    void setReadyToInstall(bool ready) { m_readyToInstall = ready; emit readyToInstallChanged(); }

    Q_INVOKABLE bool downloadUpdate(const QString&, const QString&) { ++m_downloads; return true; }
    Q_INVOKABLE bool downloadUpdate() { ++m_downloads; return true; }
    Q_INVOKABLE bool installDownloaded() { ++m_installs; return true; }

    int downloads() const { return m_downloads; }
    int installs() const { return m_installs; }
    bool mandatory() const { return true; }
    bool optionalOffer() const { return false; }
    bool exhausted() const { return false; }
    QString nextAttemptText() const { return "the next time you open SeatHub"; }
    Q_INVOKABLE bool tryAgain() { return m_readyToInstall ? installDownloaded() : downloadUpdate(); }

signals:
    void stateChanged();
    void availableUpdateChanged();
    void progressChanged();
    void failureChanged();
    void blockedBySessionChanged();
    void readyToInstallChanged();

private:
    QString m_state = QStringLiteral("idle");
    QVariantMap m_available;
    QVariantMap m_failure;
    int m_progress = 0;
    bool m_blocked = false;
    bool m_readyToInstall = false;
    int m_downloads = 0;
    int m_installs = 0;
};

class TstUpdateFeed : public QObject
{
    Q_OBJECT

private slots:
    void feedAddressesAreTheFrozenOnes();
    void versionComparisonIsSemantic();
    void checkForUpdatesOffersAnAvailableRelease();
    void checkForUpdatesStaysQuietWhenTheVersionMatches();
    void downloadUpdateVerifiesTheChecksum();
    void downloadUpdateRefusesWithoutADigest();
    void updatesAreBlockedDuringAStream();
    void forcedUpdateModalBlocksInteraction();
    void forcedUpdateModalIsNotShownDuringAStream();
    void forcedUpdateModalOnlySaysStartingWhileTheInstallerIsLaunched();
    void latestReleaseIsMandatoryFloor();
    void aNewerInstalledBuildIsCurrentAndGetsNoOffer();
    void persistentFailureReReadsTheFeedForReinstall();
    void tryAgainDownloadsTheRowJustRead();
    void aCachedOfferNeverLaunchesAnInstaller();
    void aCachedOfferIsReReadBeforeUpdateDownloads();
    void integrityExhaustionIsReportedOnceAcrossRestart();
    void theLastKnownOfferWorksOfflineAndFormatsTheRetryTime();
    void anAvailableReleaseIsAppliedThroughVelopack();
    void aFailedApplyKeepsTheOldVersionAndTheRetryBudget();
    void aFolderThatCarriesAnotherVersionIsNotApplied();
    void aPendingHandOffThatLeftTheOldVersionIsOneFailedAttempt();
    void aManualPendingHandOffDoesNotSpendAutomaticBudget();
    void theReleaseFolderIsTheOneBesideSetupAndMustBeTrusted();
    void theUninstallHookRemovesExactlyTheD45State();

private:
    QTemporaryDir m_dir;
};

void TstUpdateFeed::feedAddressesAreTheFrozenOnes()
{
    // ADR-0015/ADR-0018: the control plane's public API host, and ADR-0026's one route.
    QCOMPARE(UpdateFeedClient::defaultBaseUrl(), QStringLiteral("https://api-sevenhills.damra.co"));
    QCOMPARE(UpdateFeedClient::feedPath(QStringLiteral("client")),
             QStringLiteral("/api/releases/client"));
    QCOMPARE(UpdateFeedClient::appName(), QStringLiteral("client"));
}

void TstUpdateFeed::versionComparisonIsSemantic()
{
    QVERIFY(UpdateFeedClient::isNewerVersion(QStringLiteral("0.2.0"), QStringLiteral("0.1.0")));
    QVERIFY(UpdateFeedClient::isNewerVersion(QStringLiteral("0.1.1"), QStringLiteral("0.1.0")));
    QVERIFY(UpdateFeedClient::isNewerVersion(QStringLiteral("1.0.0"), QStringLiteral("0.9.9")));
    QVERIFY(UpdateFeedClient::isNewerVersion(QStringLiteral("v0.2.0"), QStringLiteral("0.1.0")));
    // A build suffix names the release it belongs to; the feed publishes one current row per app.
    QVERIFY(UpdateFeedClient::isNewerVersion(QStringLiteral("0.2.0+12"), QStringLiteral("0.1.0")));

    QVERIFY(!UpdateFeedClient::isNewerVersion(QStringLiteral("0.1.0"), QStringLiteral("0.1.0")));
    QVERIFY(!UpdateFeedClient::isNewerVersion(QStringLiteral("0.1.0"), QStringLiteral("0.2.0")));
    // An unparseable version is never "newer": refusing an offer is recoverable, offering a
    // downgrade nobody asked for is not.
    QVERIFY(!UpdateFeedClient::isNewerVersion(QStringLiteral("latest"), QStringLiteral("0.1.0")));
    QVERIFY(!UpdateFeedClient::isNewerVersion(QString(), QStringLiteral("0.1.0")));
    QVERIFY(!UpdateFeedClient::isNewerVersion(QStringLiteral("0.2.0"), QStringLiteral("not-a-version")));

    // ADR-0077 Amendment R5: an install is current when it is equal to or newer than the release.
    QVERIFY(UpdateFeedClient::isCurrentVersion(QStringLiteral("0.2.0"), QStringLiteral("0.2.0")));
    QVERIFY(UpdateFeedClient::isCurrentVersion(QStringLiteral("0.1.28"), QStringLiteral("0.1.27")));
    QVERIFY(UpdateFeedClient::isCurrentVersion(QStringLiteral("v0.10"), QStringLiteral("0.10.0")));
    QVERIFY(!UpdateFeedClient::isCurrentVersion(QStringLiteral("0.1.26"), QStringLiteral("0.1.27")));
    QVERIFY(!UpdateFeedClient::isCurrentVersion(QStringLiteral("0.9.9"), QStringLiteral("0.10.0")));
    // Unparseable is never proven current.
    QVERIFY(!UpdateFeedClient::isCurrentVersion(QStringLiteral("not-a-version"), QStringLiteral("0.1.0")));
    QVERIFY(!UpdateFeedClient::isCurrentVersion(QStringLiteral("0.1.0"), QString()));
}

void TstUpdateFeed::checkForUpdatesOffersAnAvailableRelease()
{
    const QVariantMap offer = UpdateFeedClient::parseRelease(
        feedRow(QStringLiteral("0.2.0"), QStringLiteral("https://example.invalid/a.exe"),
                QStringLiteral("Fixes the settings page.")),
        QStringLiteral("0.1.0"), QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    QCOMPARE(offer.value(QStringLiteral("version")).toString(), QStringLiteral("0.2.0"));
    QCOMPARE(offer.value(QStringLiteral("url")).toString(), QStringLiteral("https://example.invalid/a.exe"));
    QCOMPARE(offer.value(QStringLiteral("sha256")).toString(),
             QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    QCOMPARE(offer.value(QStringLiteral("notes")).toString(),
             QStringLiteral("Fixes the settings page."));
    QVERIFY(!offer.contains(QStringLiteral("rollback")));

    // An earlier row is no offer: a newer install is never moved down (ADR-0077 Amendment R5).
    QVERIFY(UpdateFeedClient::parseRelease(feedRow(QStringLiteral("0.0.9")),
                                           QStringLiteral("0.1.0"), QString()).isEmpty());
}

void TstUpdateFeed::aNewerInstalledBuildIsCurrentAndGetsNoOffer()
{
    // F-LIVE-1: installed 0.1.28 against a current release of 0.1.27 gets no offer and no window.
    QVERIFY(UpdateFeedClient::parseRelease(feedRow("0.1.27"), "0.1.28", {}).isEmpty());
    QVERIFY(UpdateFeedClient::parseRelease(feedRow("0.10.0"), "0.10.1", {}).isEmpty());
    QVERIFY(UpdateFeedClient::parseRelease(feedRow("0.10.0"), "5.0.0", {}).isEmpty());
    // An older install still gets the mandatory window, targeting the current release.
    const auto older = UpdateFeedClient::parseRelease(feedRow("0.1.27"), "0.1.26", {});
    QCOMPARE(older.value("version").toString(), QString("0.1.27"));
    QVERIFY(older.value("mandatory").toBool());
    QVERIFY(!older.contains("rollback"));

    // The cached offer is judged the same way, in either order of loading it.
    QTemporaryDir dir; QVERIFY(dir.isValid());
    UpdateRetryState cached; cached.target = "0.1.27"; cached.lastClass = "installer.locked_file";
    cached.lastOffer = QJsonDocument::fromJson(feedRow("0.1.27")).object().toVariantMap();
    const auto path = dir.filePath("retry-state.json"); QVERIFY(cached.save(path));

    UpdateFeedClient newer; newer.setInstalledVersion("0.1.28"); newer.setRetryStatePath(path);
    QVERIFY(!newer.mandatory()); QVERIFY(newer.availableUpdate().isEmpty());
    QCOMPARE(newer.state(), QString("idle"));

    UpdateFeedClient upgraded; upgraded.setInstalledVersion("0.1.26"); upgraded.setRetryStatePath(path);
    QVERIFY(upgraded.mandatory()); QCOMPARE(upgraded.state(), QString("failed"));
    upgraded.setInstalledVersion("0.1.28");
    QVERIFY(!upgraded.mandatory()); QVERIFY(upgraded.availableUpdate().isEmpty());

    UpdateFeedClient older2; older2.setInstalledVersion("0.1.26"); older2.setRetryStatePath(path);
    QVERIFY(older2.mandatory());
}

void TstUpdateFeed::checkForUpdatesStaysQuietWhenTheVersionMatches()
{
    // Test 2: the installed version is the published one, so there is nothing to do.
    QVERIFY(UpdateFeedClient::parseRelease(feedRow(QStringLiteral("0.1.0")),
                                           QStringLiteral("0.1.0"), QString()).isEmpty());
    // The feed answers 404 with an error body when nothing has been published yet; that is the
    // ordinary state of a fresh environment and must not become an offer or a failure.
    QVERIFY(UpdateFeedClient::parseRelease(QByteArray("{\"status_code\":404}"),
                                           QStringLiteral("0.1.0"), QString()).isEmpty());
    QVERIFY(UpdateFeedClient::parseRelease(QByteArray("not json at all"),
                                           QStringLiteral("0.1.0"), QString()).isEmpty());
    // A row missing its download address cannot be acted on.
    QVERIFY(UpdateFeedClient::parseRelease(QByteArray("{\"version\":\"0.2.0\",\"url\":\"\"}"),
                                           QStringLiteral("0.1.0"), QString()).isEmpty());
}

void TstUpdateFeed::downloadUpdateVerifiesTheChecksum()
{
    QVERIFY(m_dir.isValid());
    const QString path = m_dir.path() + QStringLiteral("/payload.bin");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArrayLiteral("abc"));
    file.close();

    // NIST's published SHA-256 test vector for "abc": a hash that returned anything else - or that
    // compared a value against itself - fails here.
    const QString abc =
        QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    QCOMPARE(UpdateFeedClient::fileSha256(path), abc);
    QVERIFY(UpdateFeedClient::verifyFileChecksum(path, abc));
    // Digests are hex, and a feed row may publish them in either case.
    QVERIFY(UpdateFeedClient::verifyFileChecksum(path, abc.toUpper()));

    // D-43: a mismatch is a refusal, and so is having nothing to compare against.
    QVERIFY(!UpdateFeedClient::verifyFileChecksum(
        path, QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));
    QVERIFY(!UpdateFeedClient::verifyFileChecksum(path, QString()));
    QVERIFY(!UpdateFeedClient::verifyFileChecksum(m_dir.path() + QStringLiteral("/absent"), abc));

    // The empty file's vector, for completeness.
    const QString emptyPath = m_dir.path() + QStringLiteral("/empty.bin");
    QFile empty(emptyPath);
    QVERIFY(empty.open(QIODevice::WriteOnly));
    empty.close();
    QVERIFY(UpdateFeedClient::verifyFileChecksum(
        emptyPath, QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));
}

void TstUpdateFeed::downloadUpdateRefusesWithoutADigest()
{
    UpdateFeedClient client;
    // Nothing here talks to the network: the refusal happens before any request is made.
    client.setBaseUrl(QStringLiteral("http://127.0.0.1:1"));

    int exits = 0;
    const auto applier = useFake(client, &exits);

    QCOMPARE(client.state(), QStringLiteral("idle"));
    QVERIFY(!client.downloadUpdate(QStringLiteral("https://example.invalid/a.exe"), QString()));
    QCOMPARE(client.state(), QStringLiteral("failed"));
    QCOMPARE(applier->downloads.load(), 0); // a row with no digest never reaches Velopack
    QVERIFY(!client.failure().value(QStringLiteral("error")).toString().isEmpty());
    // ADR-0008: the client never generates a reference code, so a refusal it makes itself (no
    // digest, no URL) carries none - a code support cannot resolve is worse than no code.
    QVERIFY2(client.failure().value(QStringLiteral("reference")).toString().isEmpty(),
             "the client must not invent an ADR-0008 reference code");
    QVERIFY(!client.readyToInstall());

    // An empty URL is equally unactionable.
    QVERIFY(!client.downloadUpdate(QString(), QStringLiteral("ba7816bf")));
    QCOMPARE(applier->downloads.load(), 0);
}

void TstUpdateFeed::updatesAreBlockedDuringAStream()
{
    UpdateFeedClient client;
    client.setBaseUrl(QStringLiteral("http://127.0.0.1:1"));
    int exits = 0;
    const auto applier = useFake(client, &exits);
    QVERIFY(!client.blockedBySession());

    // Pitfall 8 / T-03-17: no check, no download, no install while a session is running.
    client.setStreamingActive(true);
    QVERIFY(client.blockedBySession());
    QCOMPARE(client.checkForUpdates(), false);

    // Refused before the URL or the digest is even looked at.
    QVERIFY(!client.downloadUpdate(QStringLiteral("https://example.invalid/a.exe"),
                                   QStringLiteral("ba7816bf")));
    QVERIFY(!client.installDownloaded());
    QCOMPARE(applier->downloads.load(), 0); QCOMPARE(applier->applies.load(), 0); QCOMPARE(exits, 0);

    // D-41: when the session ends the offer is looked for again. The address here is a closed
    // local port, so this exercises the failure path through the one error shape (D-51) without
    // calling the real feed.
    client.setStreamingActive(false);
    client.sessionFinished();
    QTRY_VERIFY(client.state() == QStringLiteral("failed"));
    QVERIFY2(client.failure().value(QStringLiteral("reference")).toString().isEmpty(),
             "an unreachable feed is the client's own observation, so there is no code to show");
    QVERIFY(!client.readyToInstall());
}

void TstUpdateFeed::forcedUpdateModalBlocksInteraction()
{
    const QString qmlPath = guiDir() + QStringLiteral("/ForcedUpdateModal.qml");
    QVERIFY2(QFile::exists(qmlPath), qPrintable(QStringLiteral("missing ") + qmlPath));

    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QQmlEngine engine;
    qmlRegisterSingletonType(QUrl::fromLocalFile(guiDir() + QStringLiteral("/Tokens.qml")),
                             "SeatHub.Tokens", 1, 0, "Tokens");
    qmlRegisterSingletonType(QUrl::fromLocalFile(guiDir() + QStringLiteral("/Metrics.qml")),
                             "SeatHub.Tokens", 1, 0, "Metrics");

    QQmlComponent component(&engine, QUrl::fromLocalFile(qmlPath));
    QScopedPointer<QObject> root(component.create());
    if (!root) {
        QFAIL(qPrintable(component.errorString()));
    }
    QVERIFY(hostItem(root.data()) != nullptr);

    FakeUpdates updates;
    QVariantMap offer;
    offer.insert(QStringLiteral("version"), QStringLiteral("0.2.0"));
    offer.insert(QStringLiteral("url"), QStringLiteral("https://example.invalid/a.exe"));
    offer.insert(QStringLiteral("sha256"), QStringLiteral("ba7816bf"));
    offer.insert(QStringLiteral("notes"), QStringLiteral("Fixes the settings page."));
    offer.insert(QStringLiteral("rollback"), false);
    updates.setAvailableUpdate(offer);
    updates.setState(QStringLiteral("available"));

    QVERIFY(root->setProperty("updates", QVariant::fromValue(static_cast<QObject*>(&updates))));

    // Test 4: visible, and blocking.
    QVERIFY2(root->property("visible").toBool(), qPrintable(
        QStringLiteral("the modal must be up when the feed offers a build: hasOffer=")
        + root->property("hasOffer").toString()
        + QStringLiteral(" offeredVersion=") + root->property("offeredVersion").toString()
        + QStringLiteral(" shown=") + root->property("shown").toString()
        + QStringLiteral(" busy=") + root->property("busy").toString()
        + QStringLiteral(" cppBlocked=") + (updates.blockedBySession() ? QStringLiteral("true") : QStringLiteral("false"))
        + QStringLiteral(" cppState=") + updates.state()
        + QStringLiteral(" w=") + root->property("width").toString()
        + QStringLiteral(" h=") + root->property("height").toString()));

    // Something covers the whole page and swallows every pointer event aimed at it.
    bool sawBlocker = false;
    for (QObject* child : root->children()) {
        if (child->inherits("QQuickMouseArea")
            && child->property("acceptedButtons").toInt() == static_cast<int>(Qt::AllButtons)) {
            sawBlocker = true;
            break;
        }
    }
    QVERIFY2(sawBlocker, "the modal must swallow pointer input aimed at the page behind it");

    // Exactly one action, and it is the update (D-41): no second way out.
    QList<QObject*> buttons;
    const std::function<void(QObject*)> collect = [&](QObject* node) {
        for (QObject* child : node->children()) {
            if (child->inherits("QQuickButton")) {
                buttons.append(child);
            }
            collect(child);
        }
    };
    collect(root.data());
    QCOMPARE(buttons.size(), 1);
    QCOMPARE(buttons.first()->property("text").toString(), QStringLiteral("Update"));

    const int downloadsBefore = updates.downloads();
    QMetaObject::invokeMethod(buttons.first(), "clicked");
    QCOMPARE(updates.downloads(), downloadsBefore + 1);
}

void TstUpdateFeed::forcedUpdateModalIsNotShownDuringAStream()
{
    QQmlEngine engine;
    qmlRegisterSingletonType(QUrl::fromLocalFile(guiDir() + QStringLiteral("/Tokens.qml")),
                             "SeatHub.Tokens", 1, 0, "Tokens");
    qmlRegisterSingletonType(QUrl::fromLocalFile(guiDir() + QStringLiteral("/Metrics.qml")),
                             "SeatHub.Tokens", 1, 0, "Metrics");

    QQmlComponent component(&engine, QUrl::fromLocalFile(guiDir() + QStringLiteral("/ForcedUpdateModal.qml")));
    QScopedPointer<QObject> root(component.create());
    if (!root) {
        QFAIL(qPrintable(component.errorString()));
    }
    QVERIFY(hostItem(root.data()) != nullptr);

    FakeUpdates updates;
    QVariantMap offer;
    offer.insert(QStringLiteral("version"), QStringLiteral("0.2.0"));
    offer.insert(QStringLiteral("url"), QStringLiteral("https://example.invalid/a.exe"));
    offer.insert(QStringLiteral("sha256"), QStringLiteral("ba7816bf"));
    updates.setAvailableUpdate(offer);
    updates.setState(QStringLiteral("available"));

    QVERIFY(root->setProperty("updates", QVariant::fromValue(static_cast<QObject*>(&updates))));
    QVERIFY2(root->property("visible").toBool(), qPrintable(
        QStringLiteral("the modal must be up when the feed offers a build: hasOffer=")
        + root->property("hasOffer").toString()
        + QStringLiteral(" offeredVersion=") + root->property("offeredVersion").toString()));

    // Test 5: a release that arrives mid-session is not shown mid-session (Pitfall 8).
    updates.setBlockedBySession(true);
    QCOMPARE(root->property("visible").toBool(), false);

    // Once the session ends the offer comes back.
    updates.setBlockedBySession(false);
    QCOMPARE(root->property("visible").toBool(), true);

    // Nothing to offer means nothing on screen, whatever the state says.
    updates.setAvailableUpdate(QVariantMap());
    QCOMPARE(root->property("visible").toBool(), false);
}



void TstUpdateFeed::forcedUpdateModalOnlySaysStartingWhileTheInstallerIsLaunched()
{
    QQmlEngine engine;
    qmlRegisterSingletonType(QUrl::fromLocalFile(guiDir() + QStringLiteral("/Tokens.qml")),
                             "SeatHub.Tokens", 1, 0, "Tokens");
    qmlRegisterSingletonType(QUrl::fromLocalFile(guiDir() + QStringLiteral("/Metrics.qml")),
                             "SeatHub.Tokens", 1, 0, "Metrics");

    QQmlComponent component(&engine, QUrl::fromLocalFile(guiDir() + QStringLiteral("/ForcedUpdateModal.qml")));
    QScopedPointer<QObject> root(component.create());
    if (!root) {
        QFAIL(qPrintable(component.errorString()));
    }
    QVERIFY(hostItem(root.data()) != nullptr);

    FakeUpdates updates;
    QVariantMap offer;
    offer.insert(QStringLiteral("version"), QStringLiteral("0.2.0"));
    offer.insert(QStringLiteral("url"), QStringLiteral("https://example.invalid/a.exe"));
    offer.insert(QStringLiteral("sha256"), QString::fromLatin1(kAbcSha256));
    updates.setAvailableUpdate(offer);
    updates.setState(QStringLiteral("downloading"));
    QVERIFY(root->setProperty("updates", QVariant::fromValue(static_cast<QObject*>(&updates))));
    QVERIFY(root->property("visible").toBool());

    QList<QObject*> buttons;
    for (QObject* node : root->findChildren<QObject*>()) {
        if (node->inherits("QQuickButton")) {
            buttons.append(node);
        }
    }
    QCOMPARE(buttons.size(), 1);
    QObject* action = buttons.first();

    const QString starting = QStringLiteral("Starting the installer") + QChar(0x2026);
    const QString launchFailed = QStringLiteral("The installer couldn't be started.");

    // Verified, not yet launched: the copy may not claim a launch that has not been attempted.
    updates.setProgress(100);
    updates.setState(QStringLiteral("ready"));
    QVERIFY2(!visibleTexts(root.data()).contains(starting),
             qPrintable(QStringLiteral("state ready shows: ") + visibleTexts(root.data()).join(QStringLiteral(" | "))));
    QCOMPARE(action->property("enabled").toBool(), false);

    // The launch is under way: now, and only now, the copy says so, and there is nothing to press.
    updates.setState(QStringLiteral("installing"));
    QVERIFY2(visibleTexts(root.data()).contains(starting),
             qPrintable(QStringLiteral("state installing shows: ") + visibleTexts(root.data()).join(QStringLiteral(" | "))));
    QCOMPARE(root->property("busy").toBool(), true);
    QCOMPARE(action->property("enabled").toBool(), false);

    // The launch failed (a declined UAC prompt): the reason is on screen, the false "starting"
    // line is gone, and Try again retries the launch of the package already verified on disk.
    QVariantMap failure;
    failure.insert(QStringLiteral("kind"), QStringLiteral("local"));
    failure.insert(QStringLiteral("error"), launchFailed);
    failure.insert(QStringLiteral("reference"), QString());
    updates.setFailure(failure);
    updates.setReadyToInstall(true);
    updates.setState(QStringLiteral("failed"));
    QVERIFY(!visibleTexts(root.data()).contains(starting));
    QVERIFY(visibleTexts(root.data()).contains(launchFailed));
    QCOMPARE(action->property("enabled").toBool(), true);
    QCOMPARE(action->property("text").toString(), QStringLiteral("Try again"));

    const int downloadsBefore = updates.downloads();
    const int installsBefore = updates.installs();
    QMetaObject::invokeMethod(action, "clicked");
    QCOMPARE(updates.installs(), installsBefore + 1);
    QCOMPARE(updates.downloads(), downloadsBefore);

    // With no verified package on disk (a failed download or checksum), Try again downloads again.
    updates.setReadyToInstall(false);
    QMetaObject::invokeMethod(action, "clicked");
    QCOMPARE(updates.downloads(), downloadsBefore + 1);
    QCOMPARE(updates.installs(), installsBefore + 1);
}

void TstUpdateFeed::latestReleaseIsMandatoryFloor()
{
    // ADR-0077 / INTENT SC10: the newest published release is the floor. Whatever min_version a row
    // carries (or does not), an installed version that is not the release's version is required.
    auto row = QJsonDocument::fromJson(feedRow("0.10.0")).object();
    const QList<QJsonValue> floors{QJsonValue(QStringLiteral("0.9.0")), QJsonValue(QStringLiteral("0.1.0")),
                                   QJsonValue(QStringLiteral("99.0.0")), QJsonValue(QJsonValue::Null)};
    for (const QJsonValue& floor : floors) {
        row.insert("min_version", floor);
        for (const char* installed : {"0.1.0", "0.9.0", "0.9.9"}) {
            const auto offer = UpdateFeedClient::parseRelease(QJsonDocument(row).toJson(), installed, {});
            QVERIFY2(offer.value("mandatory").toBool(), installed);
            QVERIFY2(!offer.contains("min_version"), "no independent floor is carried");
        }
        // A newer install is current whatever the row's floor says (ADR-0077 Amendment R5).
        for (const char* installed : {"0.10.1", "5.0.0"})
            QVERIFY2(UpdateFeedClient::parseRelease(QJsonDocument(row).toJson(), installed, {}).isEmpty(), installed);
        // Equal means current, however the version is spelled: no window.
        QVERIFY(UpdateFeedClient::parseRelease(QJsonDocument(row).toJson(), "0.10.0", {}).isEmpty());
        QVERIFY(UpdateFeedClient::parseRelease(QJsonDocument(row).toJson(), "v0.10", {}).isEmpty());
    }
    row.remove("min_version");
    QVERIFY(UpdateFeedClient::parseRelease(QJsonDocument(row).toJson(), "0.9.9", {}).value("mandatory").toBool());
    // The operator re-pinned the current row to an older release: the newer build stays (roll forward).
    QVERIFY(UpdateFeedClient::parseRelease(QJsonDocument(row).toJson(), "0.11.0", {}).isEmpty());
    QVERIFY(UpdateFeedClient::isSameVersion("0.10.0", "0.10"));
    QVERIFY(!UpdateFeedClient::isSameVersion("0.10.0", "0.10.1"));

    // The client has no optional path left at all.
    UpdateFeedClient client;
    QCOMPARE(client.metaObject()->indexOfProperty("optionalOffer"), -1);
    QCOMPARE(client.metaObject()->indexOfMethod("dismissOptional()"), -1);

    // An offer an older build cached with a low floor is required now.
    QTemporaryDir dir; QVERIFY(dir.isValid());
    UpdateRetryState cached; cached.target = "0.10.0"; cached.lastClass = "installer.locked_file";
    cached.lastOffer = QJsonDocument::fromJson(feedRow("0.10.0")).object().toVariantMap();
    cached.lastOffer.insert("min_version", "0.1.0");
    const auto path = dir.filePath("retry-state.json"); QVERIFY(cached.save(path));
    UpdateFeedClient older; older.setRetryStatePath(path); older.setInstalledVersion("0.5.0");
    QVERIFY(older.mandatory()); QCOMPARE(older.state(), QString("failed"));
}
namespace {
QString writeLocalFeed(const QString& folder, const QString& url, const QString& floor,
                       const QString& digest = QString::fromLatin1(kAbcSha256))
{
    QDir().mkpath(folder + "/api/releases");
    QFile file(folder + "/api/releases/client");
    if (!file.open(QIODevice::WriteOnly)) return {};
    const QJsonObject row{{"version", "0.2.0"}, {"url", url}, {"min_version", floor}, {"sha256", digest}};
    if (file.write(QJsonDocument(row).toJson()) <= 0) return {};
    return QUrl::fromLocalFile(folder).toString();
}
}
namespace {
const char* const kTrustedOld = "https://api-sevenhills.damra.co/releases/client/SeatHub-Setup-OLD.exe";
const char* const kTrustedNew = "https://api-sevenhills.damra.co/releases/client/new/SeatHub-Setup-NEW.exe";
// A client whose four automatic attempts are spent on 0.2.0, with a stale cached address.
void exhaustedState(const QString& path)
{
    UpdateRetryState state; state.target = "0.2.0"; state.attemptsFailed = 4;
    state.lastClass = "installer.locked_file"; state.exhaustedReported = true;
    state.lastOffer = UpdateFeedClient::parseRelease(feedRow("0.2.0", QString::fromLatin1(kTrustedOld)), "0.1.0",
                                                     QString::fromLatin1(kAbcSha256));
    QVERIFY(state.save(path));
}
}
void TstUpdateFeed::persistentFailureReReadsTheFeedForReinstall()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto path = dir.filePath("retry-state.json");
    exhaustedState(path);
    UpdateFeedClient client; client.setRetryStatePath(path); client.setInstalledVersion("0.1.0");
    // Blocked, with the in-house reinstall available: the window is up and the budget is spent.
    QVERIFY(client.mandatory()); QVERIFY(client.exhausted());
    QCOMPARE(client.manualDownloadUrl(), QString::fromLatin1(kTrustedOld));
    QVERIFY(!client.retryState().automaticAttemptAllowed(client.retryState().now().addDays(30), false));

    // Download re-reads the current release and opens THAT address, never the cached one.
    QStringList opened;
    client.setUrlOpener([&](const QUrl& url){ opened.append(url.toString()); return true; });
    client.setBaseUrl(writeLocalFeed(dir.path(), QString::fromLatin1(kTrustedNew), "0.1.0"));
    QVERIFY(client.openManualDownload());
    QTRY_COMPARE(opened.size(), 1);
    QCOMPARE(opened.first(), QString::fromLatin1(kTrustedNew));
    QCOMPARE(client.retryState().attemptsFailed, 4); // opening a page spends nothing

    // The address is still checked after the read: hostile rows open nothing.
    for (const auto& hostile : QStringList{"http://api-sevenhills.damra.co/a", "file:///C:/private",
                                         "https://evil.invalid/a", "https://api-sevenhills.damra.co.evil.invalid/a"}) {
        client.setBaseUrl(writeLocalFeed(dir.path(), hostile, "0.1.0"));
        QVERIFY(client.openManualDownload()); QTRY_VERIFY(client.state() != "checking");
        QVERIFY(client.manualDownloadUrl().isEmpty());
    }
    QCOMPARE(opened.size(), 1);

    // An unreachable feed opens nothing, says so, and still spends no attempt.
    client.setBaseUrl("http://127.0.0.1:1");
    QVERIFY(client.openManualDownload());
    QTRY_COMPARE(client.state(), QString("failed"));
    QCOMPARE(opened.size(), 1);
    QCOMPARE(client.failure().value("error").toString(), QString("We couldn't reach SevenHills. Try again in a moment."));
    QCOMPARE(client.retryState().attemptsFailed, 4);
    QVERIFY(client.mandatory()); // the window stays: a failed update never unblocks Play
}
void TstUpdateFeed::tryAgainDownloadsTheRowJustRead()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto path = dir.filePath("retry-state.json");
    exhaustedState(path);
    UpdateFeedClient client; client.setRetryStatePath(path); client.setInstalledVersion("0.1.0");
    int exits = 0;
    const auto applier = useFake(client, &exits);
    client.setBaseUrl("http://127.0.0.1:1");
    // Unreachable feed: nothing is downloaded or applied and the budget is untouched.
    QVERIFY(client.tryAgain());
    QTRY_COMPARE(client.state(), QString("failed"));
    QCOMPARE(applier->downloads.load(), 0); QCOMPARE(applier->applies.load(), 0);
    QCOMPARE(client.retryState().attemptsFailed, 4);
    // Reachable feed: the release of the row just read is downloaded and applied - the folder beside the
    // NEW row's Setup.exe, never the folder of the cached (old) row.
    client.setBaseUrl(writeLocalFeed(dir.path(), QString::fromLatin1(kTrustedNew), "0.1.0"));
    QVERIFY(client.tryAgain());
    QTRY_COMPARE(applier->applies.load(), 1);
    QCOMPARE(applier->seenFolders(), QStringList{QStringLiteral("https://api-sevenhills.damra.co/releases/client/new/")});
    QCOMPARE(client.retryState().attemptsFailed, 4); // a manual Try again is free
    QTRY_COMPARE(exits, 1);
}
void TstUpdateFeed::aCachedOfferNeverLaunchesAnInstaller()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    UpdateRetryState state; state.target = "0.2.0"; state.attemptsFailed = 1; state.lastClass = "installer.locked_file";
    state.lastOffer = UpdateFeedClient::parseRelease(feedRow("0.2.0"), "0.1.0", QString::fromLatin1(kAbcSha256));
    const auto path = dir.filePath("retry-state.json"); QVERIFY(state.save(path));
    UpdateFeedClient client; client.setRetryStatePath(path); client.setInstalledVersion("0.1.0");
    int exits = 0;
    const auto applier = useFake(client, &exits);
    int downloads = 0;
    QObject::connect(&client, &UpdateFeedClient::stateChanged, [&]{ if (client.state() == "downloading") ++downloads; });
    client.setBaseUrl("http://127.0.0.1:1"); // the feed cannot be read
    QVERIFY(client.checkForUpdates()); QTRY_COMPARE(client.state(), QString("failed"));
    QTest::qWait(100);
    QCOMPARE(applier->applies.load(), 0); QCOMPARE(applier->downloads.load(), 0); QCOMPARE(downloads, 0);
    QCOMPARE(exits, 0);
    QVERIFY(client.mandatory()); // the last known offer still keeps the window up
    QCOMPARE(client.retryState().attemptsFailed, 1); // an unreachable feed is not an install failure
}
void TstUpdateFeed::aCachedOfferIsReReadBeforeUpdateDownloads()
{
    // No earlier failure: the restored offer is `available` and the window offers Update. Pressing it
    // downloads the row read NOW, never the cached address (ADR-0077 items 3 and 5).
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto path = dir.filePath("retry-state.json");
    UpdateRetryState state; state.target = "0.2.0";
    state.lastOffer = UpdateFeedClient::parseRelease(feedRow("0.2.0", "https://example.invalid/CACHED-OLD.exe"),
                                                     "0.1.0", QString::fromLatin1(kAbcSha256));
    QVERIFY(state.save(path));
    UpdateFeedClient client; client.setRetryStatePath(path); client.setInstalledVersion("0.1.0");
    QCOMPARE(client.state(), QString("available"));
    int exits = 0;
    const auto applier = useFake(client, &exits);
    // Offline: the press starts a read, finds nothing reachable, downloads and applies nothing.
    client.setBaseUrl("http://127.0.0.1:1");
    QVERIFY(client.downloadUpdate(client.availableUpdate().value("url").toString(),
                                  client.availableUpdate().value("sha256").toString()));
    QTRY_COMPARE(client.state(), QString("failed"));
    QTest::qWait(100);
    QCOMPARE(applier->downloads.load(), 0); QVERIFY(!client.readyToInstall());
    QCOMPARE(client.failure().value("error").toString(), QString("We couldn't reach SevenHills. Try again in a moment."));
    QCOMPARE(client.retryState().attemptsFailed, 0);
    // Online: the same press downloads what the feed says now, whatever the cache said.
    client.setBaseUrl(writeLocalFeed(dir.path(), QString::fromLatin1(kSetupUrl), "0.1.0"));
    QVERIFY(client.downloadUpdate(client.availableUpdate().value("url").toString(),
                                  client.availableUpdate().value("sha256").toString()));
    QTRY_COMPARE(applier->applies.load(), 1);
    QCOMPARE(applier->seenFolders(), QStringList{QString::fromLatin1(kSetupFolder)});
    QTRY_COMPARE(exits, 1);
}
void TstUpdateFeed::integrityExhaustionIsReportedOnceAcrossRestart()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto path = dir.filePath("retry-state.json");
    const auto base = writeLocalFeed(dir.path(), QString::fromLatin1(kSetupUrl), "0.2.0", {});
    int reports = 0;
    for (int launch = 0; launch < 2; ++launch) {
        UpdateFeedClient client; client.setRetryStatePath(path); client.setInstalledVersion("0.1.0");
        client.setBaseUrl(base);
        client.setInstallerReporter([&](const InstallJournalRecord& r){
            if (r.failureClass == "installer.retries_exhausted") ++reports;
            return true;
        });
        QVERIFY(client.checkForUpdates()); QTRY_VERIFY(client.state() != "checking");
        if (launch == 0) QVERIFY(!client.downloadUpdate());
        QVERIFY(client.exhausted()); QVERIFY(client.mandatory());
        QCOMPARE(reports, 1); QVERIFY(UpdateRetryState::load(path).exhaustedReported);
    }
}

void TstUpdateFeed::theLastKnownOfferWorksOfflineAndFormatsTheRetryTime()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto at = QDateTime::fromString("2026-09-30T12:00:00Z", Qt::ISODate);
    UpdateRetryState state; state.target = "0.2.0"; state.attemptsFailed = 2;
    state.lastClass = "installer.locked_file"; state.nextAllowedAt = at.addSecs(5400);
    state.lastOffer = UpdateFeedClient::parseRelease(feedRow("0.2.0"), "0.1.0", QString::fromLatin1(kAbcSha256));
    const auto path = dir.filePath("retry-state.json"); QVERIFY(state.save(path));
    UpdateFeedClient client; client.setRetryStatePath(path); client.setInstalledVersion("0.1.0");
    client.setRetryClock([at]{ return at; }); client.setBaseUrl("http://127.0.0.1:1");
    QVERIFY(client.mandatory()); QCOMPARE(client.nextAttemptText(), QString("in about 90 minutes"));
    QVERIFY(client.checkForUpdates()); QTRY_COMPARE(client.state(), QString("failed"));
    QVERIFY(client.mandatory()); QCOMPARE(client.availableUpdate().value("version").toString(), QString("0.2.0"));
    state.nextAllowedAt = at.addSecs(10800); QVERIFY(state.save(path)); client.setRetryStatePath(path);
    client.setRetryClock([at]{ return at; }); QCOMPARE(client.nextAttemptText(), QString("in about 3 hours"));
    state.lastOffer.insert("min_version", "0.1.0"); QVERIFY(state.save(path));
    UpdateFeedClient older; older.setRetryStatePath(path); older.setInstalledVersion("0.1.5");
    QVERIFY(older.mandatory()); QCOMPARE(older.state(), QString("failed")); // the cached floor is ignored (ADR-0077)
    older.setInstalledVersion("0.2.0"); QVERIFY(!older.retryState().exhausted()); // equal to the target: done
}

void TstUpdateFeed::anAvailableReleaseIsAppliedThroughVelopack()
{
    // SC8: an available release chosen by the feed client is downloaded with progress and applied
    // through Velopack, the application is asked to quit so the updater can swap the app folder, and the
    // person pressed Update once.
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto path = dir.filePath("retry-state.json");
    UpdateFeedClient client; client.setRetryStatePath(path); client.setInstalledVersion("0.1.0");
    int exits = 0;
    const auto applier = useFake(client, &exits);
    bool markerDurableAtApply = false;
    QString stateAtApply;
    applier->onApply = [&]() {
        markerDurableAtApply = UpdateRetryState::load(path).launched;
        stateAtApply = client.state();
    };
    const QString base = writeLocalFeed(dir.path(), QString::fromLatin1(kSetupUrl), "0.1.0");
    QVERIFY(!base.isEmpty());
    client.setBaseUrl(base);
    QSignalSpy downloaded(&client, &UpdateFeedClient::downloaded);
    QSignalSpy requested(&client, &UpdateFeedClient::installRequested);
    QList<int> progress;
    QObject::connect(&client, &UpdateFeedClient::progressChanged, [&]() { progress.append(client.progress()); });

    QVERIFY(client.checkForUpdates());
    QTRY_COMPARE(client.state(), QStringLiteral("available"));
    QVERIFY(client.mandatory());
    QCOMPARE(applier->downloads.load(), 0); // an offer alone starts nothing

    QVERIFY(client.downloadUpdate(client.availableUpdate().value("url").toString(),
                                  client.availableUpdate().value("sha256").toString()));
    QTRY_COMPARE(requested.count(), 1);
    QCOMPARE(downloaded.count(), 1);
    QCOMPARE(downloaded.first().first().toString(), QStringLiteral("0.2.0"));
    // The updater is pointed at the folder beside the row's Setup.exe, and nowhere else.
    QCOMPARE(applier->seenFolders(), QStringList{QString::fromLatin1(kSetupFolder)});
    QCOMPARE(applier->applies.load(), 1);
    QCOMPARE(stateAtApply, QStringLiteral("installing"));
    QVERIFY2(markerDurableAtApply, "the pending hand-off marker must be on disk before Velopack is asked");
    QVERIFY(progress.contains(40));
    QCOMPARE(client.progress(), 100);
    QTRY_COMPARE(exits, 1);
    QCOMPARE(client.state(), QStringLiteral("installing"));
    QVERIFY(client.failure().isEmpty());
    QCOMPARE(client.installedVersion(), QStringLiteral("0.1.0")); // the swap happens after this process exits
    QCOMPARE(client.retryState().attemptsFailed, 0);

    // Exactly one hand-off: nothing queued behind it asks the updater a second time.
    QTest::qWait(100);
    QCOMPARE(applier->applies.load(), 1);
    QCOMPARE(requested.count(), 1);
    QCOMPARE(exits, 1);
}

void TstUpdateFeed::aFailedApplyKeepsTheOldVersionAndTheRetryBudget()
{
    // SC8 + ADR-0077/V34: when the updater does not take the release, nothing changes - the old version
    // keeps running, Play stays blocked behind the window, and the failure is one attempt under the
    // bounded retry: four in total, the first retry at the next launch with no wait, then 1 to 2 h and 2 to
    // 4 h, a manual Try again free, one report on exhaustion.
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto path = dir.filePath("retry-state.json");
    const QString base = writeLocalFeed(dir.path(), QString::fromLatin1(kSetupUrl), "0.1.0");
    QVERIFY(!base.isEmpty());
    QDateTime clock = QDateTime::fromString("2026-10-09T09:00:00Z", Qt::ISODate);
    QStringList reported;
    const auto reporter = [&](const InstallJournalRecord& record) { reported.append(record.failureClass); return true; };
    const auto launch = [&](UpdateFeedClient& client, int* exits) {
        client.setRetryStatePath(path); client.setInstalledVersion("0.1.0");
        client.setBaseUrl(base);
        client.setRetryClock([&clock]() { return clock; });
        client.setInstallerReporter(reporter);
        const auto applier = useFake(client, exits);
        applier->applyOk = false;
        return applier;
    };

    // Launch 1: the person presses Update and the updater refuses.
    {
        UpdateFeedClient client; int exits = 0;
        const auto applier = launch(client, &exits);
        QVERIFY(client.checkForUpdates());
        QTRY_COMPARE(client.state(), QStringLiteral("available"));
        QVERIFY(client.downloadUpdate(client.availableUpdate().value("url").toString(),
                                      client.availableUpdate().value("sha256").toString()));
        QTRY_COMPARE(applier->applies.load(), 1);
        QCOMPARE(client.state(), QStringLiteral("failed"));
        QCOMPARE(exits, 0);                                              // the app did not quit
        QCOMPARE(client.installedVersion(), QStringLiteral("0.1.0"));    // the old version keeps running
        QCOMPARE(client.failure().value("kind").toString(), QStringLiteral("local"));
        QCOMPARE(client.failure().value("error").toString(), QStringLiteral("The installer couldn't be started."));
        QVERIFY2(client.failure().value("reference").toString().isEmpty(), "the client invents no reference");
        QVERIFY(client.mandatory());                                     // a failed update never unblocks Play
        QVERIFY(client.readyToInstall());
        QCOMPARE(client.retryState().attemptsFailed, 1);
        QVERIFY(!UpdateRetryState::load(path).launched);

        // A manual Try again re-reads the feed, downloads again, and is free.
        QVERIFY(client.tryAgain());
        QTRY_COMPARE(applier->applies.load(), 2);
        QCOMPARE(client.state(), QStringLiteral("failed"));
        QCOMPARE(applier->downloads.load(), 2);
        QCOMPARE(client.retryState().attemptsFailed, 1);
        // The first retry is the NEXT launch, not this one.
        QVERIFY(!client.retryState().automaticAttemptAllowed(clock, false));
    }

    // Launch 2 (no wait): the first automatic retry runs at once and fails.
    {
        UpdateFeedClient client; int exits = 0;
        const auto applier = launch(client, &exits);
        QVERIFY(client.checkForUpdates());
        QTRY_COMPARE(applier->applies.load(), 1);
        QCOMPARE(client.state(), QStringLiteral("failed"));
        QCOMPARE(exits, 0);
        QCOMPARE(client.retryState().attemptsFailed, 2);
        QVERIFY(client.retryState().nextAllowedAt >= clock.addSecs(3600));
        QVERIFY(client.retryState().nextAllowedAt <= clock.addSecs(7200));
    }

    // Launch 3, too early: nothing is downloaded or applied, and nothing is spent.
    {
        clock = clock.addSecs(60);
        UpdateFeedClient client; int exits = 0;
        const auto applier = launch(client, &exits);
        QSignalSpy checked(&client, &UpdateFeedClient::checkFinished);
        QVERIFY(client.checkForUpdates());
        QTRY_VERIFY(!checked.isEmpty());
        QTest::qWait(100);
        QCOMPARE(applier->downloads.load(), 0);
        QCOMPARE(client.retryState().attemptsFailed, 2);
    }

    // Launch 3, after the wait: the second retry runs and fails.
    {
        clock = clock.addSecs(7200);
        UpdateFeedClient client; int exits = 0;
        const auto applier = launch(client, &exits);
        QVERIFY(client.checkForUpdates());
        QTRY_COMPARE(applier->applies.load(), 1);
        QCOMPARE(client.retryState().attemptsFailed, 3);
        QVERIFY(client.retryState().nextAllowedAt >= clock.addSecs(7200));
        QVERIFY(client.retryState().nextAllowedAt <= clock.addSecs(14400));
        QVERIFY(reported.isEmpty()); // nothing is reported before the budget is spent
    }

    // Launch 4: the last attempt fails, the budget is spent, one report is sent, and the window stays.
    {
        clock = clock.addSecs(14400);
        UpdateFeedClient client; int exits = 0;
        const auto applier = launch(client, &exits);
        QVERIFY(client.checkForUpdates());
        QTRY_COMPARE(applier->applies.load(), 1);
        QCOMPARE(client.retryState().attemptsFailed, 4);
        QVERIFY(client.exhausted());
        QVERIFY(client.mandatory());
        QCOMPARE(reported, QStringList{QStringLiteral("installer.retries_exhausted")});
        QCOMPARE(exits, 0);
    }

    // Launch 5, a year later: no automatic attempt and no second report - only the target changing resets it.
    {
        clock = clock.addDays(365);
        UpdateFeedClient client; int exits = 0;
        const auto applier = launch(client, &exits);
        QSignalSpy checked(&client, &UpdateFeedClient::checkFinished);
        QVERIFY(client.checkForUpdates());
        QTRY_VERIFY(!checked.isEmpty());
        QTest::qWait(100);
        QCOMPARE(applier->downloads.load(), 0);
        QVERIFY(client.exhausted());
        QVERIFY(client.mandatory());
        QCOMPARE(reported.size(), 1);
    }
}

void TstUpdateFeed::aFolderThatCarriesAnotherVersionIsNotApplied()
{
    // The row names 0.2.0 but the release folder carries 0.3.0 (for example the row was updated before the
    // folder was): nothing is applied, the old version keeps running, and it is an ordinary failed attempt.
    QTemporaryDir dir; QVERIFY(dir.isValid());
    UpdateFeedClient client; client.setRetryStatePath(dir.filePath("retry-state.json"));
    client.setInstalledVersion("0.1.0");
    int exits = 0;
    const auto applier = useFake(client, &exits);
    applier->carries = QStringLiteral("0.3.0");
    client.setBaseUrl(writeLocalFeed(dir.path(), QString::fromLatin1(kSetupUrl), "0.1.0"));
    QVERIFY(client.checkForUpdates());
    QTRY_COMPARE(client.state(), QStringLiteral("available"));
    QVERIFY(client.downloadUpdate(client.availableUpdate().value("url").toString(),
                                  client.availableUpdate().value("sha256").toString()));
    QTRY_COMPARE(client.state(), QStringLiteral("failed"));
    QCOMPARE(applier->downloads.load(), 1);
    QCOMPARE(applier->applies.load(), 0);
    QCOMPARE(exits, 0);
    QVERIFY(!client.readyToInstall());
    QVERIFY(client.mandatory());
    QCOMPARE(client.retryState().attemptsFailed, 1);
}

void TstUpdateFeed::aPendingHandOffThatLeftTheOldVersionIsOneFailedAttempt()
{
    // ADR-0086: a release was handed to the updater (the marker was durable) and this start is still the
    // OLD version, so the swap did not happen and the old version relaunched. That is one failed attempt,
    // reported once, and the first retry runs at this launch with no wait.
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto path = dir.filePath("retry-state.json");
    UpdateRetryState pending; pending.target = "0.2.0"; pending.launched = true; QVERIFY(pending.save(path));
    UpdateFeedClient client; client.setRetryStatePath(path); client.setInstalledVersion("0.1.0");
    int exits = 0;
    const auto applier = useFake(client, &exits);
    QSemaphore gate;
    applier->hold = &gate;       // keep the retry's download in flight so the first failure stands alone
    applier->downloadOk = false;
    client.setBaseUrl(writeLocalFeed(dir.path(), QString::fromLatin1(kSetupUrl), "0.1.0"));
    int reports = 0;
    client.setInstallerReporter([&](const InstallJournalRecord& r) {
        if (r.failureClass == "installer.unknown") ++reports;
        return true;
    });
    QVERIFY(client.checkForUpdates());
    QTRY_COMPARE(applier->downloads.load(), 1);                 // the first retry runs at this launch
    QCOMPARE(reports, 1);
    QCOMPARE(client.retryState().attemptsFailed, 1);
    QVERIFY(!client.retryState().launched);
    QVERIFY(!UpdateRetryState::load(path).launched);
    QCOMPARE(client.state(), QStringLiteral("downloading"));
    gate.release();
    QTRY_COMPARE(client.state(), QStringLiteral("failed"));
    QCOMPARE(client.retryState().attemptsFailed, 2);
    QCOMPARE(reports, 1);                                       // one failure, one report
    QCOMPARE(exits, 0);
}

void TstUpdateFeed::aManualPendingHandOffDoesNotSpendAutomaticBudget()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto path = dir.filePath("retry-state.json");
    UpdateRetryState state; state.target = "0.2.0"; state.launched = state.launchedManual = true;
    QVERIFY(state.save(path));
    UpdateFeedClient client; client.setRetryStatePath(path); client.setInstalledVersion("0.1.0");
    int exits = 0;
    const auto applier = useFake(client, &exits);
    QSemaphore gate;
    applier->hold = &gate;
    applier->downloadOk = false;
    client.setBaseUrl(writeLocalFeed(dir.path(), QString::fromLatin1(kSetupUrl), "0.1.0"));
    QVERIFY(client.checkForUpdates());
    QTRY_COMPARE(applier->downloads.load(), 1);
    QCOMPARE(client.retryState().attemptsFailed, 0); // the pending hand-off was a manual press: free
    QVERIFY(!client.retryState().launchedManual);
    QVERIFY(!client.retryState().launched);
    gate.release();
    QTRY_COMPARE(client.state(), QStringLiteral("failed"));
}

void TstUpdateFeed::theReleaseFolderIsTheOneBesideSetupAndMustBeTrusted()
{
    using namespace VelopackUpdater;
    const QString host = QUrl(UpdateFeedClient::defaultBaseUrl()).host();
    QCOMPARE(feedFolderOf(QUrl(QString::fromLatin1(kSetupUrl))).toString(), QString::fromLatin1(kSetupFolder));
    QCOMPARE(feedFolderOf(QUrl(QStringLiteral("https://api-sevenhills.damra.co/downloads/Setup.exe?x=1#frag"))).toString(),
             QStringLiteral("https://api-sevenhills.damra.co/downloads/"));
    QVERIFY(isTrustedFeedFolder(QUrl(QString::fromLatin1(kSetupFolder)), host));
    // Only https on the control plane's own host: a hostile feed row points the updater nowhere else.
    for (const QString& hostile : {QStringLiteral("http://api-sevenhills.damra.co/releases/"),
                                   QStringLiteral("https://evil.invalid/releases/"),
                                   QStringLiteral("https://api-sevenhills.damra.co.evil.invalid/releases/"),
                                   QStringLiteral("https://user:pass@api-sevenhills.damra.co/releases/"),
                                   QStringLiteral("file:///C:/private/"), QStringLiteral("C:/private/"), QString()})
        QVERIFY2(!isTrustedFeedFolder(QUrl(hostile), host), qPrintable(hostile));
    QVERIFY(!isTrustedFeedFolder(QUrl(QString::fromLatin1(kSetupFolder)), QString()));
}

namespace {
void touch(const QString& path)
{
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("x");
}
}

void TstUpdateFeed::theUninstallHookRemovesExactlyTheD45State()
{
    // I-SEATHUB / client.md: the before-uninstall hook removes the sign-in and telemetry folder under
    // %APPDATA%, the crash-db, log-spool and update folders under %LOCALAPPDATA%\Seven Hills\SeatHub, and the
    // HKCU Settings key - and nothing else: not the install folder, not the rest of the local folder, not a
    // neighbouring app, not the retired client's own folder.
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString roaming = dir.filePath("Roaming");
    const QString local = dir.filePath("Local");
    const QString localApp = local + "/Seven Hills/SeatHub";
    const QStringList removed{
        roaming + "/Seven Hills/SeatHub/access.dpapi", roaming + "/Seven Hills/SeatHub/telemetry.json",
        roaming + "/Seven Hills/SeatHub/end-outbox/pending.json",
        localApp + "/crash-db/reports/a.dmp", localApp + "/log-spool/spool-1.jsonl",
        localApp + "/update/retry-state.json"};
    const QStringList kept{
        roaming + "/Seven Hills/HostHub/keep.txt", roaming + "/Other/keep.txt",
        localApp + "/qmlcache/keep.bin", localApp + "/keep.log",
        local + "/Seven Hills/HostHub/keep.txt", local + "/SeatHub/keep.txt",
        local + "/SeatHubVelopackProof/current/SeatHub.exe", local + "/SeatHubVelopackProof/Update.exe"};
    for (const QString& file : removed + kept) { touch(file); QVERIFY(!QTest::currentTestFailed()); }

    const VelopackUpdater::D45Roots roots{roaming, local};
    const auto state = VelopackUpdater::d45State(roots);
    QStringList expectedDirs{roaming + "/Seven Hills/SeatHub", localApp + "/crash-db", localApp + "/log-spool",
                             localApp + "/update"};
    QStringList actualDirs = state.directories;
    expectedDirs.sort(); actualDirs.sort();
    QCOMPARE(actualDirs, expectedDirs);
    QCOMPARE(state.registryKey, QStringLiteral("Software\\Seven Hills\\SeatHub"));

    QStringList keys;
    QVERIFY(VelopackUpdater::removeD45State(roots, [&](const QString& key) { keys.append(key); return true; }));
    QCOMPARE(keys, QStringList{QStringLiteral("Software\\Seven Hills\\SeatHub")}); // exactly one key
    for (const QString& file : removed) QVERIFY2(!QFileInfo::exists(file), qPrintable(file));
    for (const QString& folder : expectedDirs) QVERIFY2(!QFileInfo::exists(folder), qPrintable(folder));
    for (const QString& file : kept) QVERIFY2(QFileInfo::exists(file), qPrintable(file));

    // Running it again (an uninstall retried) finds nothing and still reports success.
    QVERIFY(VelopackUpdater::removeD45State(roots, [](const QString&) { return true; }));
    for (const QString& file : kept) QVERIFY2(QFileInfo::exists(file), qPrintable(file));

    // A key that will not delete is reported, and does not stop the folders from being removed.
    touch(roaming + "/Seven Hills/SeatHub/access.dpapi");
    QVERIFY(!VelopackUpdater::removeD45State(roots, [](const QString&) { return false; }));
    QVERIFY(!QFileInfo::exists(roaming + "/Seven Hills/SeatHub"));

    // With no roots (the environment gave none) nothing is derived, so nothing relative can be removed.
    QVERIFY(VelopackUpdater::d45State({}).directories.isEmpty());
}

QTEST_MAIN(TstUpdateFeed)

#include "tst_update_feed.moc"
