// Release-feed and forced-update tests (Plan 03-04 Task 2).
//
// Two things are being proven here, and they are the two the threat register cares about:
//
//   * T-03-14 (tampering with the downloaded binary): a package is only ever reported as
//     installable when its SHA-256 matches the digest the release pin carries. The digest check is
//     verified against NIST's published vectors, so a test that passed by comparing a hash against
//     itself cannot happen.
//   * T-03-17 / Pitfall 8 (replacing the binary under a live stream): while a session is running
//     nothing is checked, nothing is downloaded, nothing is installed, and the modal renders
//     nothing.
//
// The feed's own HTTP path is exercised only against a closed local port - the release feed is a
// real service and a unit test has no business calling it.

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>

#include <functional>

#include "seathub/update_feed_client.h"

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

// A stand-in "installer" served through QNetworkAccessManager's own file:// backend, so the real
// download -> verify -> install path runs end to end without a socket or the real release feed.
QString writeAbcPackage(const QString& directory, const QString& name)
{
    const QString path = directory + QLatin1Char('/') + name;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return QString();
    }
    file.write(QByteArrayLiteral("abc"));
    file.close();
    return path;
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
    void aVerifiedDownloadStartsTheInstallerWithoutASecondPress();
    void aFailedLaunchIsShownAndTryAgainRetriesTheLaunch();
    void forcedUpdateModalOnlySaysStartingWhileTheInstallerIsLaunched();

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

    // D-44: an operator rollback is a row pointing at an earlier version, and the client is
    // expected to install it - so an offer is "different", and this only classifies it.
    QVERIFY(UpdateFeedClient::isRollback(QStringLiteral("0.1.0"), QStringLiteral("0.2.0")));
    QVERIFY(!UpdateFeedClient::isRollback(QStringLiteral("0.2.0"), QStringLiteral("0.1.0")));
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
    QCOMPARE(offer.value(QStringLiteral("rollback")).toBool(), false);

    // An earlier row is an offer too (D-44), flagged so the modal can say so.
    const QVariantMap rollback = UpdateFeedClient::parseRelease(feedRow(QStringLiteral("0.0.9")),
                                                               QStringLiteral("0.1.0"), QString());
    QCOMPARE(rollback.value(QStringLiteral("version")).toString(), QStringLiteral("0.0.9"));
    QCOMPARE(rollback.value(QStringLiteral("rollback")).toBool(), true);
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

    QCOMPARE(client.state(), QStringLiteral("idle"));
    QVERIFY(!client.downloadUpdate(QStringLiteral("https://example.invalid/a.exe"), QString()));
    QCOMPARE(client.state(), QStringLiteral("failed"));
    QVERIFY(!client.failure().value(QStringLiteral("error")).toString().isEmpty());
    // ADR-0008: the client never generates a reference code, so a refusal it makes itself (no
    // digest, no URL) carries none - a code support cannot resolve is worse than no code.
    QVERIFY2(client.failure().value(QStringLiteral("reference")).toString().isEmpty(),
             "the client must not invent an ADR-0008 reference code");
    QVERIFY(!client.readyToInstall());

    // An empty URL is equally unactionable.
    QVERIFY(!client.downloadUpdate(QString(), QStringLiteral("ba7816bf")));
}

void TstUpdateFeed::updatesAreBlockedDuringAStream()
{
    UpdateFeedClient client;
    client.setBaseUrl(QStringLiteral("http://127.0.0.1:1"));
    QVERIFY(!client.blockedBySession());

    // Pitfall 8 / T-03-17: no check, no download, no install while a session is running.
    client.setStreamingActive(true);
    QVERIFY(client.blockedBySession());
    QCOMPARE(client.checkForUpdates(), false);

    // Refused before the URL or the digest is even looked at.
    QVERIFY(!client.downloadUpdate(QStringLiteral("https://example.invalid/a.exe"),
                                   QStringLiteral("ba7816bf")));
    QVERIFY(!client.installDownloaded());

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

void TstUpdateFeed::aVerifiedDownloadStartsTheInstallerWithoutASecondPress()
{
    // The "hangs at Starting the installer" defect (WINDOWS #21, CUST-18): a verified download
    // stopped in "ready" with the modal's only action disabled, so nothing ever launched the
    // installer. The one press - the download - has to carry through to the launch on its own.
    QVERIFY(m_dir.isValid());
    const QString source = writeAbcPackage(m_dir.path(), QStringLiteral("tst_update_feed-auto.exe"));
    QVERIFY(!source.isEmpty());

    UpdateFeedClient client;
    client.setBaseUrl(QStringLiteral("http://127.0.0.1:1"));

    QStringList launchedPaths;
    QStringList statesAtLaunch;
    client.setInstallerLauncher([&](const QString& path) {
        launchedPaths.append(path);
        statesAtLaunch.append(client.state());
        return true;
    });
    QSignalSpy verified(&client, &UpdateFeedClient::verified);
    QSignalSpy requested(&client, &UpdateFeedClient::installRequested);

    QVERIFY(client.downloadUpdate(QUrl::fromLocalFile(source).toString(),
                                  QString::fromLatin1(kAbcSha256)));

    QTRY_COMPARE(requested.count(), 1);
    QCOMPARE(verified.count(), 1);
    QCOMPARE(launchedPaths.size(), 1);
    // The bytes that run are the bytes that were verified, and the launch happens in the one state
    // whose copy says the installer is starting.
    QCOMPARE(launchedPaths.first(), client.downloadedPath());
    QCOMPARE(statesAtLaunch.first(), QStringLiteral("installing"));
    QCOMPARE(client.state(), QStringLiteral("installing"));
    QVERIFY(client.failure().isEmpty());

    // Exactly one launch: nothing queued behind it starts the installer a second time.
    QTest::qWait(100);
    QCOMPARE(launchedPaths.size(), 1);
    QCOMPARE(requested.count(), 1);

    QFile::remove(client.downloadedPath());
}

void TstUpdateFeed::aFailedLaunchIsShownAndTryAgainRetriesTheLaunch()
{
    // A declined UAC prompt, or any other failed start, must end on a visible failure - never on
    // "Starting the installer" forever. The verified package stays on disk, so Try again re-runs
    // the launch (re-verifying first) instead of downloading the whole installer again.
    QVERIFY(m_dir.isValid());
    const QString source =
        writeAbcPackage(m_dir.path(), QStringLiteral("tst_update_feed-declined.exe"));
    QVERIFY(!source.isEmpty());

    UpdateFeedClient client;
    client.setBaseUrl(QStringLiteral("http://127.0.0.1:1"));

    int attempts = 0;
    bool starts = false;
    client.setInstallerLauncher([&](const QString&) {
        ++attempts;
        return starts;
    });
    QSignalSpy requested(&client, &UpdateFeedClient::installRequested);

    QVERIFY(client.downloadUpdate(QUrl::fromLocalFile(source).toString(),
                                  QString::fromLatin1(kAbcSha256)));

    QTRY_COMPARE(client.state(), QStringLiteral("failed"));
    QCOMPARE(attempts, 1);
    QCOMPARE(requested.count(), 0);
    // Something on this machine failed, not the network, and the client invents no reference.
    QCOMPARE(client.failure().value(QStringLiteral("kind")).toString(), QStringLiteral("local"));
    QVERIFY(!client.failure().value(QStringLiteral("error")).toString().isEmpty());
    QVERIFY2(client.failure().value(QStringLiteral("reference")).toString().isEmpty(),
             "the client must not invent an ADR-0008 reference code");
    QVERIFY(client.readyToInstall());

    starts = true;
    QVERIFY(client.installDownloaded());
    QCOMPARE(attempts, 2);
    QCOMPARE(requested.count(), 1);
    QCOMPARE(client.state(), QStringLiteral("installing"));
    QVERIFY(client.failure().isEmpty());

    QFile::remove(client.downloadedPath());
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

QTEST_MAIN(TstUpdateFeed)

#include "tst_update_feed.moc"
