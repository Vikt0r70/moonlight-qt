/*****************************************************************************
 * SeatHub fork - the facade's own tests (the phase verifier's W2).
 *
 * Before this suite, no binary in this tree compiled `app/seathub/seathub_client.cpp` or
 * `moonlight_engine_session.cpp`. The app build was the only thing that ever saw them, which is
 * how three compile errors - a missing `<QThread>` include, a call to the private
 * `NvComputer::sortAppList()`, and a non-const `NvApp::isInitialized()` called through a const
 * reference - reached a tree that was passing every test suite it had (03-06 closure, N2).
 *
 * Two translation units the facade needs cannot come with it, and both are substituted at their
 * own seam rather than faked inside the facade:
 *
 *   * `moonlight_engine_session.cpp` builds upstream's `Session` and drags the whole engine in
 *     with it. Its factory is replaced here with one that returns null - which is the facade's
 *     documented fail-closed contract - and the engine itself is injected the way the fork's seam
 *     already allows: `SeatHubClient::session()` is public, so a test attaches its own
 *     `EngineSession` and the *real* facade drives it.
 *   * `pairing_handshake.cpp` needs a live Sunshine host. Its entry point is replaced with one
 *     that fails, which is also its documented behaviour when no host answers.
 *
 * What this suite proves that nothing else could:
 *
 *   1. The facade compiles and links at all, and constructs in its signed-out state.
 *   2. The composite runs: the facade drives the attached engine (Play, D-02 interrupt, the
 *      streaming/home transition on the lifecycle's signals) - the hop the gap-closure report
 *      could only read.
 *   3. `readyForDeletion` at the facade starts the documented teardown sequence, once per
 *      session, and the second session tears down exactly like the first (defect F-9, whose
 *      guard used to read `TeardownController::stage()` - a stage that is sticky at `Done`).
 *      `tst_teardown` proves the controller works when called; nothing proved the facade called
 *      it. This does.
 *
 * Build recipe:
 *   call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
 *   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
 *   cd tests && qmake tst_facade_wiring.pro && jom && tst_facade_wiring.exe -o tst_facade_wiring-out.txt,txt
 *****************************************************************************/

#include <QtTest>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWindow>

#include "seathub/control_plane_client.h"
#include "seathub/duration_text.h"
#include "seathub/engine_session.h"
#include "seathub/moonlight_engine_session.h"
#include "seathub/pairing_handshake.h"
#include "seathub/seathub_client.h"
#include "seathub/session_lifecycle.h"
#include "seathub/teardown_controller.h"
#include "seathub/token_store.h"

// ---------------------------------------------------------------- the two stitched seams -------
//
// These are the definitions of the two translation units this binary deliberately does not link.
// Both are unreachable-by-construction in this test (no host is ever paired, no engine is ever
// built), and both report the fail-closed answer their production counterparts document for
// exactly this situation.

MoonlightEngineSession* MoonlightEngineSession::create(const PairedHostPtr&, QObject* parent)
{
    Q_UNUSED(parent);
    // The production factory returns null when the host is not a `MoonlightPairedHost` or its
    // application list does not name a single application, and the caller then fails closed
    // instead of substituting a stub. Returning null here is that same answer: no host is paired
    // in this test environment.
    return nullptr;
}

PairingHandshakeResult runUpstreamPairingHandshake(const PairingTarget&)
{
    PairingHandshakeResult result;
    result.ok = false;
    result.engineError = QStringLiteral("no Sunshine host exists in this test environment");
    return result;
}

namespace {

const char* kAccessToken = "opaque-access-token";
const char* kPlaintextMarker = "sb_rt_PLAINTEXT-MARKER-4F7K";

class FakeReply : public QNetworkReply
{
    Q_OBJECT

public:
    FakeReply(int httpStatus, const QByteArray& body, QObject* parent)
        : QNetworkReply(parent)
    {
        if (httpStatus == 0) {
            // The transport never produced a response: an unreachable control plane.
            setError(QNetworkReply::HostNotFoundError, QStringLiteral("no route to control plane"));
        }
        else {
            setAttribute(QNetworkRequest::HttpStatusCodeAttribute, QVariant(httpStatus));
        }
        m_buffer.setData(body);
        m_buffer.open(QIODevice::ReadOnly);
        open(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this]() {
            setFinished(true);
            emit finished();
        });
    }

    void abort() override {}
    qint64 readData(char* data, qint64 maxSize) override { return m_buffer.read(data, maxSize); }
    qint64 bytesAvailable() const override
    {
        return m_buffer.size() + QNetworkReply::bytesAvailable();
    }

private:
    QBuffer m_buffer;
};

/// The control plane as the facade's teardown sees it: `POST /end` succeeds, `GET
/// /api/sessions/{id}` reports the session terminal, and the pairing poll is refused (there is no
/// rig). Answers by path rather than by call position, because the pairing controller polls while
/// teardown runs and the two must not be confused.
class FakeControlPlane : public QNetworkAccessManager
{
    Q_OBJECT

public:
    explicit FakeControlPlane(QObject* parent) : QNetworkAccessManager(parent) {}

    QStringList requestPaths() const
    {
        QMutexLocker lock(&m_mutex);
        return m_paths;
    }

    int countOfPathEndingWith(const QString& suffix) const
    {
        QMutexLocker lock(&m_mutex);
        int count = 0;
        for (const QString& path : m_paths) {
            if (path.endsWith(suffix)) {
                ++count;
            }
        }
        return count;
    }

    // What `GET /api/me`, `GET /api/wallet`, `POST /api/auth/logout` and `POST
    // /api/auth/otp/verify` answer. Status 0 is a transport failure.
    void answerMe(int status, const QByteArray& body)
    {
        QMutexLocker lock(&m_mutex);
        m_meStatus = status;
        m_meBody = body;
    }
    void answerWallet(int status, const QByteArray& body)
    {
        QMutexLocker lock(&m_mutex);
        m_walletStatus = status;
        m_walletBody = body;
    }
    void answerLogout(int status)
    {
        QMutexLocker lock(&m_mutex);
        m_logoutStatus = status;
    }

    /// The `Authorization` header the last request for `path` carried.
    QByteArray authorizationFor(const QString& path) const
    {
        QMutexLocker lock(&m_mutex);
        return m_auth.value(path);
    }

protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override
    {
        const QString path = request.url().path();
        int meStatus, walletStatus, logoutStatus;
        QByteArray meBody, walletBody;
        {
            QMutexLocker lock(&m_mutex);
            m_paths.append(path);
            m_auth.insert(path, request.rawHeader("Authorization"));
            meStatus = m_meStatus;
            meBody = m_meBody;
            walletStatus = m_walletStatus;
            walletBody = m_walletBody;
            logoutStatus = m_logoutStatus;
        }

        if (path == QLatin1String("/api/me")) {
            return new FakeReply(meStatus, meBody, this);
        }
        if (path == QLatin1String("/api/wallet")) {
            return new FakeReply(walletStatus, walletBody, this);
        }
        if (path == QLatin1String("/api/auth/logout")) {
            return new FakeReply(logoutStatus, QByteArray(), this);
        }
        if (path == QLatin1String("/api/auth/otp/verify")) {
            return new FakeReply(200, QByteArrayLiteral("{\"access_token\":\"sb_at_from_otp\"}"), this);
        }

        if (path.endsWith(QLatin1String("/end"))) {
            return new FakeReply(200, QByteArrayLiteral("{\"status\":true}"), this);
        }
        if (path.endsWith(QLatin1String("/pairing"))) {
            // No host was assigned to this session: the documented refusal, so pairing stops.
            QJsonObject body;
            body.insert(QStringLiteral("status"), false);
            body.insert(QStringLiteral("error"), QStringLiteral("no rig is assigned"));
            body.insert(QStringLiteral("reference"), QStringLiteral("SH-9K2XQ1"));
            return new FakeReply(404, QJsonDocument(body).toJson(QJsonDocument::Compact), this);
        }
        return new FakeReply(200, terminalSessionBody(), this);
    }

private:
    static QByteArray terminalSessionBody()
    {
        QJsonObject object;
        object.insert(QStringLiteral("id"), QStringLiteral("session"));
        object.insert(QStringLiteral("state"), QStringLiteral("COMPLETED"));
        object.insert(QStringLiteral("quality_profile"), QStringLiteral("1080p60"));
        object.insert(QStringLiteral("minutes_billed"), 7);
        object.insert(QStringLiteral("reconnect_count"), 0);
        object.insert(QStringLiteral("requested_at"), QStringLiteral("2026-09-19T00:00:00Z"));
        return QJsonDocument(object).toJson(QJsonDocument::Compact);
    }

    mutable QMutex m_mutex;
    QStringList m_paths;
    QHash<QString, QByteArray> m_auth;
    int m_meStatus = 200;
    QByteArray m_meBody;
    int m_walletStatus = 200;
    QByteArray m_walletBody;
    int m_logoutStatus = 204;
};

/// An `EngineSession` that records what the facade asked it to do. Engine-free by construction:
/// this is what `app/seathub/engine_session.h` exists for.
class FakeEngineSession : public EngineSession
{
    Q_OBJECT

public:
    using EngineSession::EngineSession;

    void run(QWindow* window) override { runs.append(window); }
    void interrupt() override { ++interrupts; }
    bool publishOverlaySurface(SDL_Surface*) override { return true; }

    QList<QWindow*> runs;
    int interrupts = 0;
};

} // namespace

class TstFacadeWiring : public QObject
{
    Q_OBJECT

private:
    QScopedPointer<QTemporaryDir> m_dir;
    FakeControlPlane* m_fake = nullptr;

    /// The control-plane client only lives on the network thread the facade moves it to, so the
    /// fake answers from there. A blocking invocation keeps the main thread from racing it.
    void armControlPlane(SeatHubClient& client)
    {
        ControlPlaneClient* plane = client.controlPlane();
        if (plane->thread() == QThread::currentThread()) {
            m_fake = new FakeControlPlane(plane);
            plane->setNetworkAccessManager(m_fake);
            return;
        }
        QMetaObject::invokeMethod(
            plane,
            [this, plane]() {
                m_fake = new FakeControlPlane(plane);
                plane->setNetworkAccessManager(m_fake);
            },
            Qt::BlockingQueuedConnection);
    }

    /// A real stored credential, so "teardown leaves the sign-in alone" is a statement about the
    /// disk and not about a flag. The value is a marker, never a credential.
    bool storeACredential(SeatHubClient& client, QString* pathOut)
    {
        TokenStore* store = client.credentialStore();
        store->setDirectory(m_dir->path());
        if (!store->storeToken(TokenStore::accessTokenName(),
                               QString::fromLatin1(kPlaintextMarker))) {
            return false;
        }
        *pathOut = store->pathFor(TokenStore::accessTokenName());
        return QFile::exists(*pathOut);
    }

    /// Points a fresh facade's store at the scratch directory without storing anything.
    void isolateStore(SeatHubClient& client)
    {
        client.credentialStore()->setDirectory(m_dir->path());
    }

    /// The account and wallet the control plane answers for a signed-in customer.
    static QByteArray accountBody()
    {
        return QByteArrayLiteral(
            "{\"id\":\"6f1c6f5e-3a1e-4b1e-9f2e-0f1a2b3c4d5e\",\"display_name\":\"Lina\","
            "\"username\":\"lina\",\"email\":\"lina@example.com\",\"phone_e164\":\"+962790000000\","
            "\"role\":\"customer\",\"signup_stage\":\"complete\",\"email_verified\":true,"
            "\"phone_verified\":true,\"created_at\":\"2026-09-01T00:00:00Z\"}");
    }
    static QByteArray walletBody(int minutes)
    {
        return QStringLiteral("{\"balance_minutes\":%1,\"updated_at\":\"2026-09-21T10:00:00Z\"}")
            .arg(minutes).toUtf8();
    }
    static QByteArray refusedBody()
    {
        return QByteArrayLiteral(
            "{\"error\":\"Please sign in again.\",\"reference\":\"SH-3K2XQ1\"}");
    }

    static QList<int> stagesSeen(const QSignalSpy& spy)
    {
        QList<int> seen;
        for (const QList<QVariant>& emission : spy) {
            seen.append(emission.at(0).toInt());
        }
        return seen;
    }

private slots:
    void initTestCase()
    {
        qRegisterMetaType<SeatHubFailure>("SeatHubFailure");
        qRegisterMetaType<TeardownStage>("TeardownStage");
    }

    void init()
    {
        m_dir.reset(new QTemporaryDir);
        QVERIFY(m_dir->isValid());
        m_fake = nullptr;
    }

    void cleanup()
    {
        m_fake = nullptr;
        m_dir.reset();
    }

    // --- the facade compiles, links and starts in the right state ------------------------------

    void theFacadeLinksAndConstructsInTheRestoreState()
    {
        SeatHubClient client;

        // Constructed in "restoring", not "signed_out": the sign-in form must never be shown while
        // the stored credential is being checked (CUST-08). `restoreSession()` resolves it.
        QCOMPARE(client.appState(), QStringLiteral("restoring"));
        QCOMPARE(client.balanceMinutes(), qint64(-1));
        QVERIFY(client.balanceText().isEmpty());
        QVERIFY(!client.balanceStale());
        QVERIFY(client.identity().isEmpty());
        QCOMPARE(client.homeStatus(), QStringLiteral("ready"));
        QVERIFY(client.session() != nullptr);
        QVERIFY(client.teardown() != nullptr);
        QVERIFY(client.credentialStore() != nullptr);
        QVERIFY(client.failure().isEmpty());
        QVERIFY(client.reference().isEmpty());
        QCOMPARE(client.session()->active(), false);
        QVERIFY(client.session()->upstreamSession() == nullptr);
    }

    void theFacadeDrivesTheAttachedEngineAndInterruptReachesIt()
    {
        auto* engine = new FakeEngineSession;
        {
            SeatHubClient client;
            client.session()->attachSession(engine);
            QVERIFY(client.session()->upstreamSession() != nullptr);

            // No access token and no control plane: the lifecycle is asked to run and the
            // attached session - not a tracer - is what gets driven.
            client.start();
            QCOMPARE(client.appState(), QStringLiteral("connecting"));
            QCOMPARE(engine->runs.size(), 1);
            QCOMPARE(client.session()->active(), true);
            QCOMPARE(client.failure().isEmpty(), true);

            // The engine's own stage name never becomes the customer's line (D-51).
            emit engine->stageStarting(QStringLiteral("RTSP handshake"));
            QVERIFY(!client.stageText().contains(QStringLiteral("RTSP")));

            emit engine->connectionStarted();
            QCOMPARE(client.appState(), QStringLiteral("streaming"));

            // D-02 reaches the engine through the facade.
            client.interrupt();
            QCOMPARE(engine->interrupts, 1);

            emit engine->sessionFinished(0);
            emit engine->readyForDeletion();
            QCOMPARE(client.appState(), QStringLiteral("signed_out"));
            QCOMPARE(client.session()->active(), false);
            QVERIFY(client.session()->upstreamSession() == nullptr);
            QVERIFY(client.failure().isEmpty());
        }
        delete engine;
    }

    // --- teardown, at the facade, for the first session and the second -------------------------

    void theFirstSessionTearsDownThroughTheFacade()
    {
        SeatHubClient client;
        auto* engine = new FakeEngineSession;
        client.session()->attachSession(engine);
        client.controlPlane()->setBaseUrl(QStringLiteral("https://control.invalid"));
        client.controlPlane()->setAccessToken(QString::fromLatin1(kAccessToken));
        client.teardown()->setVerifyIntervalMs(1);

        QString tokenPath;
        QVERIFY(storeACredential(client, &tokenPath));

        client.beginSession(QStringLiteral("session-one"));
        armControlPlane(client);

        QSignalSpy completed(client.teardown(), &TeardownController::teardownCompleted);
        QSignalSpy failed(client.teardown(), &TeardownController::teardownFailed);
        QSignalSpy stages(client.teardown(), &TeardownController::stageEntered);
        QSignalSpy lifecycleEnded(client.session(), &SessionLifecycle::readyForDeletion);

        // The engine's own signal, arriving the way the engine delivers it.
        emit engine->readyForDeletion();

        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 15000);
        QCOMPARE(failed.count(), 0);

        // Step 1 went out on the documented route, for this session.
        QCOMPARE(m_fake->countOfPathEndingWith(QStringLiteral("/end")), 1);
        QVERIFY(m_fake->requestPaths().contains(QStringLiteral("/api/sessions/session-one/end")));

        // All five stages, in the required order and never backwards.
        const QList<int> seen = stagesSeen(stages);
        int previous = -1;
        for (int stage : seen) {
            QVERIFY2(stage > previous, "teardown stages must advance strictly forward");
            previous = stage;
        }
        QVERIFY(seen.contains(static_cast<int>(TeardownStage::Disable)));
        QVERIFY(seen.contains(static_cast<int>(TeardownStage::Unpair)));
        QVERIFY(seen.contains(static_cast<int>(TeardownStage::Verify)));
        QVERIFY(seen.contains(static_cast<int>(TeardownStage::Clear)));
        QVERIFY(seen.contains(static_cast<int>(TeardownStage::Done)));
        QCOMPARE(client.teardown()->stage(), TeardownStage::Done);

        // On the disk: teardown does not sign the customer out (CUST-08). The client stores no rig,
        // address or pairing (STREAM-10), so the credential is the only thing there, and it stays.
        QVERIFY2(QFile::exists(tokenPath), "teardown must not remove the sign-in credential");
        QCOMPARE(client.credentialStore()->retrieveToken(TokenStore::accessTokenName()),
                 QString::fromLatin1(kPlaintextMarker));

        // The whole composite ran: the engine's signal, through the lifecycle, through the facade,
        // into the teardown controller - not just the controller on its own (tst_teardown) and not
        // just the seam (tst_engine_seam).
        QCOMPARE(lifecycleEnded.count(), 1);

        // Detach before destroying: the lifecycle outlives this statement and must not be holding a
        // pointer to a session that no longer exists.
        client.session()->attachSession(nullptr);
        delete engine;
    }

    void theSecondSessionTearsDownToo()
    {
        // Defect F-9: the guard that decided whether to start teardown read
        // `TeardownController::stage() != TeardownStage::Done`, and that stage is sticky at `Done`
        // once one teardown has completed. Session two therefore never tore down at all: no
        // `POST /end`, no rig-side disable or unpair, no local clear, no `teardownCompleted()`.
        // The facade's own per-session flag replaced it; this is the regression test for that.
        SeatHubClient client;
        auto* engine = new FakeEngineSession;
        client.session()->attachSession(engine);
        client.controlPlane()->setBaseUrl(QStringLiteral("https://control.invalid"));
        client.controlPlane()->setAccessToken(QString::fromLatin1(kAccessToken));
        client.teardown()->setVerifyIntervalMs(1);

        QString firstTokenPath;
        QVERIFY(storeACredential(client, &firstTokenPath));

        client.beginSession(QStringLiteral("session-one"));
        armControlPlane(client);

        QSignalSpy completed(client.teardown(), &TeardownController::teardownCompleted);
        QSignalSpy failed(client.teardown(), &TeardownController::teardownFailed);
        QSignalSpy stages(client.teardown(), &TeardownController::stageEntered);
        QSignalSpy lifecycleEnded(client.session(), &SessionLifecycle::readyForDeletion);

        emit engine->readyForDeletion();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 15000);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(client.teardown()->stage(), TeardownStage::Done);
        QVERIFY(QFile::exists(firstTokenPath));

        // Session two. A fresh credential on disk, and a fresh engine: the lifecycle detached the
        // first one when its session ended (the W1 fix clears `active` and disconnects there), and
        // in production `handleHostResolved()` attaches a new `MoonlightEngineSession` for the next
        // launch. Re-emitting from the first engine would prove nothing - by design nothing is
        // listening to it any more.
        QString secondTokenPath;
        QVERIFY(storeACredential(client, &secondTokenPath));

        auto* secondEngine = new FakeEngineSession;
        client.beginSession(QStringLiteral("session-two"));
        client.session()->attachSession(secondEngine);
        QVERIFY(client.session()->upstreamSession() != nullptr);

        emit secondEngine->readyForDeletion();

        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 2, 15000);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(lifecycleEnded.count(), 2);

        // The step that F-9 removed entirely, and the one that proves the sequence completed
        // rather than merely started.
        QCOMPARE(m_fake->countOfPathEndingWith(QStringLiteral("/end")), 2);
        QVERIFY(m_fake->requestPaths().contains(QStringLiteral("/api/sessions/session-two/end")));

        const QList<int> seen = stagesSeen(stages);
        QCOMPARE(seen.count(static_cast<int>(TeardownStage::Disable)), 2);
        QCOMPARE(seen.count(static_cast<int>(TeardownStage::Unpair)), 2);
        QCOMPARE(seen.count(static_cast<int>(TeardownStage::Clear)), 2);
        QCOMPARE(seen.count(static_cast<int>(TeardownStage::Done)), 2);

        QVERIFY2(QFile::exists(secondTokenPath),
                 "the second session must not sign the customer out either");
        QCOMPARE(client.credentialStore()->retrieveToken(TokenStore::accessTokenName()),
                 QString::fromLatin1(kPlaintextMarker));

        client.session()->attachSession(nullptr);
        delete secondEngine;
        delete engine;
    }

    // --- Phase 5 plan 02: launch, real balance, real sign-out -----------------------------------
    //
    // The phase tracer, driven end to end at the facade with a stubbed control plane: a stored
    // credential is read, confirmed with GET /api/me, Home opens, the wallet is read and drawn in
    // the hours-and-minutes format, and sign-out revokes it on the server.

    void aStoredCredentialOpensHomeWithTheIdentityAndTheRealBalance()
    {
        SeatHubClient client;
        QString tokenPath;
        QVERIFY(storeACredential(client, &tokenPath));
        // A credential of a shape a real one has, replacing the marker: the header must carry it.
        QVERIFY(client.credentialStore()->storeToken(TokenStore::accessTokenName(),
                                                     QStringLiteral("opaque-access-token")));
        armControlPlane(client);
        m_fake->answerMe(200, accountBody());
        m_fake->answerWallet(200, walletBody(135));

        QCOMPARE(client.appState(), QStringLiteral("restoring"));
        client.restoreSession();

        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(client.balanceText(), QStringLiteral("2 h 15 min"), 15000);

        // Confirmed against the control plane, with the credential, before Home opened.
        QVERIFY(m_fake->requestPaths().contains(QStringLiteral("/api/me")));
        QCOMPARE(m_fake->authorizationFor(QStringLiteral("/api/me")),
                 QByteArrayLiteral("Bearer opaque-access-token"));

        // The identity comes from the account, and the balance is the server's number, formatted
        // - never computed here.
        QCOMPARE(client.identity(), QStringLiteral("+962790000000"));
        QCOMPARE(client.account().value(QStringLiteral("display_name")).toString(),
                 QStringLiteral("Lina"));
        QCOMPARE(client.account().value(QStringLiteral("email")).toString(),
                 QStringLiteral("lina@example.com"));
        QCOMPARE(client.balanceMinutes(), qint64(135));
        QVERIFY(!client.balanceStale());
        QVERIFY(m_fake->requestPaths().contains(QStringLiteral("/api/wallet")));
        QCOMPARE(m_fake->authorizationFor(QStringLiteral("/api/wallet")),
                 QByteArrayLiteral("Bearer opaque-access-token"));

        // Still stored: launching signs nobody out.
        QVERIFY(client.credentialStore()->hasToken(TokenStore::accessTokenName()));
    }

    void restoreDoesNotShowTheSignInFormWhileTheCredentialIsBeingChecked()
    {
        SeatHubClient client;
        QString tokenPath;
        QVERIFY(storeACredential(client, &tokenPath));
        armControlPlane(client);
        m_fake->answerMe(200, accountBody());
        m_fake->answerWallet(200, walletBody(5));

        QStringList states;
        QObject::connect(&client, &SeatHubClient::appStateChanged,
                         [&client, &states]() { states.append(client.appState()); });

        client.restoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);

        // restoring -> home, and never through signed_out on the way.
        QVERIFY2(!states.contains(QStringLiteral("signed_out")),
                 qPrintable(QStringLiteral("states seen: %1").arg(states.join(QLatin1Char(',')))));
        QCOMPARE(states.first(), QStringLiteral("home"));
    }

    void aRefusedCredentialClearsTheStoreAndLandsOnSignIn()
    {
        SeatHubClient client;
        QString tokenPath;
        QVERIFY(storeACredential(client, &tokenPath));
        armControlPlane(client);
        m_fake->answerMe(401, refusedBody());

        client.restoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("signed_out"), 15000);

        QVERIFY2(!QFile::exists(tokenPath), "a credential the server refused is not kept");
        QCOMPARE(QDir(m_dir->path()).entryList(QDir::Files).size(), 0);
        QVERIFY(!client.controlPlane()->hasAccessToken());
        QVERIFY(client.identity().isEmpty());
        // Nothing to read a balance for.
        QVERIFY(!m_fake->requestPaths().contains(QStringLiteral("/api/wallet")));
    }

    void noStoredCredentialLandsOnSignInWithoutAskingTheNetwork()
    {
        SeatHubClient client;
        isolateStore(client);
        armControlPlane(client);

        client.restoreSession();
        QCOMPARE(client.appState(), QStringLiteral("signed_out"));
        QVERIFY(m_fake->requestPaths().isEmpty());
    }

    void anUnreachableControlPlaneKeepsTheCustomerSignedInAndOffline()
    {
        SeatHubClient client;
        QString tokenPath;
        QVERIFY(storeACredential(client, &tokenPath));
        armControlPlane(client);
        m_fake->answerMe(0, QByteArray());
        m_fake->answerWallet(0, QByteArray());

        client.restoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);

        // Signed in, and Home says the control plane cannot be reached rather than signing out.
        QCOMPARE(client.homeStatus(), QStringLiteral("offline"));
        QVERIFY2(QFile::exists(tokenPath), "a lost network must never delete the credential");
        QVERIFY(client.controlPlane()->hasAccessToken());

        // The wallet could not be read either, and no read has ever succeeded: nothing to show.
        QTRY_VERIFY_WITH_TIMEOUT(client.balanceStale(), 15000);
        QCOMPARE(client.balanceMinutes(), qint64(-1));
        QVERIFY(client.balanceText().isEmpty());
    }

    void aFailedWalletReadKeepsTheLastKnownBalanceAndSignsNobodyOut()
    {
        SeatHubClient client;
        QString tokenPath;
        QVERIFY(storeACredential(client, &tokenPath));
        armControlPlane(client);
        m_fake->answerMe(200, accountBody());
        m_fake->answerWallet(200, walletBody(45));

        client.restoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(client.balanceText(), QStringLiteral("45 min"), 15000);

        // The next read fails - a transport failure, then a 401, then a 500. None of them may blank
        // the balance, and none of them may sign the customer out.
        for (int status : { 0, 401, 500 }) {
            m_fake->answerWallet(status, status == 401 ? refusedBody() : QByteArray());
            client.refreshBalance();
            QTRY_VERIFY_WITH_TIMEOUT(client.balanceStale(), 15000);

            QCOMPARE(client.balanceText(), QStringLiteral("45 min"));
            QCOMPARE(client.balanceMinutes(), qint64(45));
            QCOMPARE(client.appState(), QStringLiteral("home"));
            QVERIFY(QFile::exists(tokenPath));

            // And a later good read clears the label and takes the new number.
            m_fake->answerWallet(200, walletBody(45));
            client.refreshBalance();
            QTRY_VERIFY_WITH_TIMEOUT(!client.balanceStale(), 15000);
        }

        m_fake->answerWallet(200, walletBody(60));
        client.refreshBalance();
        QTRY_COMPARE_WITH_TIMEOUT(client.balanceText(), QStringLiteral("1 h 00 min"), 15000);
    }

    void signInByCodeStoresTheCredentialInTheAccessSlotAndReadsTheBalance()
    {
        SeatHubClient client;
        isolateStore(client);
        armControlPlane(client);
        m_fake->answerWallet(200, walletBody(90));

        client.verifyOtp(QStringLiteral("+962790000000"), QStringLiteral("123456"));
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);

        QCOMPARE(client.credentialStore()->retrieveToken(TokenStore::accessTokenName()),
                 QStringLiteral("sb_at_from_otp"));
        QVERIFY(!client.credentialStore()->hasToken(TokenStore::refreshTokenName()));
        QTRY_COMPARE_WITH_TIMEOUT(client.balanceText(), QStringLiteral("1 h 30 min"), 15000);
        QCOMPARE(m_fake->authorizationFor(QStringLiteral("/api/wallet")),
                 QByteArrayLiteral("Bearer sb_at_from_otp"));
    }

    void signOutRevokesOnTheServerThenClearsTheStore()
    {
        SeatHubClient client;
        QString tokenPath;
        QVERIFY(storeACredential(client, &tokenPath));
        QVERIFY(client.credentialStore()->storeToken(TokenStore::accessTokenName(),
                                                     QStringLiteral("opaque-access-token")));
        armControlPlane(client);
        m_fake->answerMe(200, accountBody());
        m_fake->answerWallet(200, walletBody(135));

        client.restoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(client.balanceText(), QStringLiteral("2 h 15 min"), 15000);

        client.signOut();

        // The revoke was attempted, with the credential, on the documented route.
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->requestPaths().contains(QStringLiteral("/api/auth/logout")),
                                 15000);
        QCOMPARE(m_fake->authorizationFor(QStringLiteral("/api/auth/logout")),
                 QByteArrayLiteral("Bearer opaque-access-token"));

        // And nothing usable is left anywhere.
        QCOMPARE(client.appState(), QStringLiteral("signed_out"));
        QVERIFY(client.identity().isEmpty());
        QVERIFY(client.account().isEmpty());
        QCOMPARE(client.balanceMinutes(), qint64(-1));
        QVERIFY(client.balanceText().isEmpty());
        QVERIFY(!QFile::exists(tokenPath));
        QCOMPARE(QDir(m_dir->path()).entryList(QDir::Files).size(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!client.controlPlane()->hasAccessToken(), 15000);
    }

    void signOutStillClearsEverythingWhenTheServerCannotBeReached()
    {
        SeatHubClient client;
        QString tokenPath;
        QVERIFY(storeACredential(client, &tokenPath));
        armControlPlane(client);
        m_fake->answerMe(200, accountBody());
        m_fake->answerWallet(200, walletBody(20));
        m_fake->answerLogout(0);

        client.restoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(client.balanceText(), QStringLiteral("20 min"), 15000);

        client.signOut();

        QTRY_VERIFY_WITH_TIMEOUT(m_fake->requestPaths().contains(QStringLiteral("/api/auth/logout")),
                                 15000);
        QCOMPARE(client.appState(), QStringLiteral("signed_out"));
        QVERIFY2(!QFile::exists(tokenPath),
                 "an offline sign-out still removes the credential from this machine");
        QCOMPARE(QDir(m_dir->path()).entryList(QDir::Files).size(), 0);
        // The reply failed and the in-memory copy is still dropped once it arrives.
        QTRY_VERIFY_WITH_TIMEOUT(!client.controlPlane()->hasAccessToken(), 15000);

        // A wallet answer that lands after sign-out is not the next customer's to see.
        QVERIFY(client.balanceText().isEmpty());
    }

    void aSessionThatEndsHandsTheSignedInCustomerBackToHomeAndRereadsTheBalance()
    {
        // The return to Home after a stream reads the wallet again (CUST-06), and the credential
        // is still stored - so the next launch opens on Home too (CUST-08).
        SeatHubClient client;
        QString tokenPath;
        QVERIFY(storeACredential(client, &tokenPath));
        auto* engine = new FakeEngineSession;
        client.session()->attachSession(engine);
        armControlPlane(client);
        m_fake->answerMe(200, accountBody());
        m_fake->answerWallet(200, walletBody(40));

        client.restoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(client.balanceText(), QStringLiteral("40 min"), 15000);

        // The stream ran and used fifteen minutes; only the server knows that.
        m_fake->answerWallet(200, walletBody(25));
        QVERIFY(client.session()->start(nullptr));
        emit engine->connectionStarted();
        QCOMPARE(client.appState(), QStringLiteral("streaming"));
        emit engine->sessionFinished(0);
        emit engine->readyForDeletion();

        QCOMPARE(client.appState(), QStringLiteral("home"));
        QTRY_COMPARE_WITH_TIMEOUT(client.balanceText(), QStringLiteral("25 min"), 15000);
        QVERIFY2(QFile::exists(tokenPath), "playing once must not sign the customer out");

        client.session()->attachSession(nullptr);
        delete engine;
    }

    // --- the one hours-and-minutes formatter (OD-01) ---------------------------------------------

    void durationText_isHoursAndMinutesAndNeverARawMinuteCount_data()
    {
        QTest::addColumn<qint64>("minutes");
        QTest::addColumn<QString>("plain");
        QTest::addColumn<QString>("signedText");

        QTest::newRow("zero") << qint64(0) << QStringLiteral("0 min") << QStringLiteral("0 min");
        QTest::newRow("under an hour") << qint64(45) << QStringLiteral("45 min")
                                       << QStringLiteral("+45 min");
        QTest::newRow("one minute") << qint64(1) << QStringLiteral("1 min")
                                    << QStringLiteral("+1 min");
        QTest::newRow("59 minutes") << qint64(59) << QStringLiteral("59 min")
                                    << QStringLiteral("+59 min");
        QTest::newRow("a whole hour keeps its minutes") << qint64(60) << QStringLiteral("1 h 00 min")
                                                        << QStringLiteral("+1 h 00 min");
        QTest::newRow("three whole hours") << qint64(180) << QStringLiteral("3 h 00 min")
                                           << QStringLiteral("+3 h 00 min");
        QTest::newRow("two and a quarter") << qint64(135) << QStringLiteral("2 h 15 min")
                                           << QStringLiteral("+2 h 15 min");
        QTest::newRow("minutes are two digits from one hour up") << qint64(65)
                                                                 << QStringLiteral("1 h 05 min")
                                                                 << QStringLiteral("+1 h 05 min");
        QTest::newRow("a long balance") << qint64(6000) << QStringLiteral("100 h 00 min")
                                        << QStringLiteral("+100 h 00 min");
        QTest::newRow("a debit") << qint64(-45) << QStringLiteral("-45 min")
                                 << QStringLiteral("-45 min");
        QTest::newRow("a debit over an hour") << qint64(-65) << QStringLiteral("-1 h 05 min")
                                              << QStringLiteral("-1 h 05 min");
    }

    void durationText_isHoursAndMinutesAndNeverARawMinuteCount()
    {
        QFETCH(qint64, minutes);
        QFETCH(QString, plain);
        QFETCH(QString, signedText);

        QCOMPARE(durationText(minutes), plain);
        QCOMPARE(signedDurationText(minutes), signedText);
    }
};

QTEST_MAIN(TstFacadeWiring)

#include "tst_facade_wiring.moc"
