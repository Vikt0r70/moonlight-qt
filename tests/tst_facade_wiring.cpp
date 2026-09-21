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
 *   1. The facade compiles and links at all, and constructs in its restore state (Phase 5 plan 02).
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
#include "seathub/countries.h"
#include "seathub/duration_text.h"
#include "seathub/engine_session.h"
#include "seathub/moonlight_engine_session.h"
#include "seathub/pairing_handshake.h"
#include "seathub/seathub_client.h"
#include "seathub/session_lifecycle.h"
#include "seathub/teardown_controller.h"
#include "seathub/token_store.h"
#include "seathub/web_origin.h"

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
    /// What `POST /api/sessions` (Play) answers. Status 0 is a transport failure.
    void answerPlay(int status, const QByteArray& body)
    {
        QMutexLocker lock(&m_mutex);
        m_playStatus = status;
        m_playBody = body;
    }
    /// What `POST /api/auth/login` answers (status 0 is a transport failure).
    void answerLogin(int status, const QByteArray& body)
    {
        QMutexLocker lock(&m_mutex);
        m_loginStatus = status;
        m_loginBody = body;
    }

    /// What `POST /api/auth/otp/request` answers (status 0 is a transport failure).
    void answerOtpRequest(int status)
    {
        QMutexLocker lock(&m_mutex);
        m_otpRequestStatus = status;
    }

    /// What `GET /api/sessions/{id}/pairing` answers. The default is the documented refusal (no rig is
    /// assigned), which stops pairing at once; 409 is "the rig is not ready yet", which keeps the poll
    /// going - and with it the session read that rides every tick.
    void answerPairing(int status, const QByteArray& body = QByteArray())
    {
        QMutexLocker lock(&m_mutex);
        m_pairingStatus = status;
        m_pairingBody = body;
        m_pairingCustom = true;
    }

    /// What `GET /api/sessions/{id}` answers: the session in `state`, under `id`, with an end reason
    /// and a billed-minutes count when given. The default is a terminal session under another id,
    /// which teardown's verification wants and the connecting stages ignore.
    void answerSession(const QString& id, const QString& state, const QString& endReason = QString(),
                       int minutesBilled = 0)
    {
        QMutexLocker lock(&m_mutex);
        QJsonObject object;
        object.insert(QStringLiteral("id"), id);
        object.insert(QStringLiteral("state"), state);
        object.insert(QStringLiteral("quality_profile"), QStringLiteral("1080p60"));
        object.insert(QStringLiteral("minutes_billed"), minutesBilled);
        object.insert(QStringLiteral("reconnect_count"), 0);
        object.insert(QStringLiteral("requested_at"), QStringLiteral("2026-09-19T00:00:00Z"));
        if (!endReason.isEmpty()) {
            object.insert(QStringLiteral("end_reason"), endReason);
        }
        m_sessionBody = QJsonDocument(object).toJson(QJsonDocument::Compact);
    }

    /// The body the last request for `path` carried.
    QByteArray bodyFor(const QString& path) const
    {
        QMutexLocker lock(&m_mutex);
        return m_bodies.value(path);
    }

    /// The `Authorization` header the last request for `path` carried.
    QByteArray authorizationFor(const QString& path) const
    {
        QMutexLocker lock(&m_mutex);
        return m_auth.value(path);
    }

protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request,
                                 QIODevice* outgoing) override
    {
        const QString path = request.url().path();
        int meStatus, walletStatus, logoutStatus, loginStatus, otpRequestStatus, playStatus;
        int pairingStatus;
        bool pairingCustom;
        QByteArray meBody, walletBody, loginBody, playBody, pairingBody, sessionBody;
        {
            QMutexLocker lock(&m_mutex);
            m_paths.append(path);
            m_auth.insert(path, request.rawHeader("Authorization"));
            if (outgoing) {
                m_bodies.insert(path, outgoing->peek(outgoing->size()));
            }
            loginStatus = m_loginStatus;
            loginBody = m_loginBody;
            playStatus = m_playStatus;
            playBody = m_playBody;
            otpRequestStatus = m_otpRequestStatus;
            meStatus = m_meStatus;
            meBody = m_meBody;
            walletStatus = m_walletStatus;
            walletBody = m_walletBody;
            logoutStatus = m_logoutStatus;
            pairingStatus = m_pairingStatus;
            pairingBody = m_pairingBody;
            pairingCustom = m_pairingCustom;
            sessionBody = m_sessionBody;
        }

        if (path == QLatin1String("/api/me")) {
            return new FakeReply(meStatus, meBody, this);
        }
        if (path == QLatin1String("/api/wallet")) {
            return new FakeReply(walletStatus, walletBody, this);
        }
        if (path == QLatin1String("/api/sessions")) {
            return new FakeReply(playStatus, playBody, this);
        }
        if (path == QLatin1String("/api/auth/logout")) {
            return new FakeReply(logoutStatus, QByteArray(), this);
        }
        if (path == QLatin1String("/api/auth/login")) {
            return new FakeReply(loginStatus, loginBody, this);
        }
        if (path == QLatin1String("/api/auth/otp/request")) {
            return new FakeReply(otpRequestStatus, QByteArrayLiteral("{}"), this);
        }
        if (path == QLatin1String("/api/auth/otp/verify")) {
            return new FakeReply(200, QByteArrayLiteral("{\"access_token\":\"sb_at_from_otp\"}"), this);
        }

        if (path.endsWith(QLatin1String("/end"))) {
            return new FakeReply(200, QByteArrayLiteral("{\"status\":true}"), this);
        }
        if (path.endsWith(QLatin1String("/pairing")) && pairingCustom) {
            return new FakeReply(pairingStatus, pairingBody, this);
        }
        if (path.startsWith(QLatin1String("/api/sessions/")) && path.count(QLatin1Char('/')) == 3
                && !sessionBody.isEmpty()) {
            return new FakeReply(200, sessionBody, this);
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
    QHash<QString, QByteArray> m_bodies;
    int m_loginStatus = 200;
    QByteArray m_loginBody;
    int m_playStatus = 201;
    QByteArray m_playBody;
    int m_otpRequestStatus = 200;
    int m_meStatus = 200;
    QByteArray m_meBody;
    int m_walletStatus = 200;
    QByteArray m_walletBody;
    int m_logoutStatus = 204;
    int m_pairingStatus = 404;
    QByteArray m_pairingBody;
    bool m_pairingCustom = false;
    QByteArray m_sessionBody;
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
    QScopedPointer<QTemporaryDir> m_backupRoot;
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
    ///
    /// The store is pointed at the scratch directory AND its update-backup root at another scratch
    /// directory: `recoverAtStartup()` looks in the user's real temporary directory by default, and
    /// a customer's real `SeatHub-sign-in-*` backup must never be a thing a test can consume.
    bool storeACredential(SeatHubClient& client, QString* pathOut)
    {
        TokenStore* store = client.credentialStore();
        store->setDirectory(m_dir->path());
        store->setBackupRoot(m_backupRoot->path());
        if (!store->storeToken(TokenStore::accessTokenName(),
                               QString::fromLatin1(kPlaintextMarker))) {
            return false;
        }
        *pathOut = store->pathFor(TokenStore::accessTokenName());
        return QFile::exists(*pathOut);
    }

    /// Points a fresh facade's store at the scratch directories without storing anything.
    void isolateStore(SeatHubClient& client)
    {
        client.credentialStore()->setDirectory(m_dir->path());
        client.credentialStore()->setBackupRoot(m_backupRoot->path());
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

    /// A signed-in facade on Home with `minutes` in the wallet, its control plane faked and pointed at
    /// an address that resolves nowhere (a session's channel opens against it and simply fails).
    void reachHome(SeatHubClient& client, int minutes)
    {
        isolateStore(client);
        client.controlPlane()->setBaseUrl(QStringLiteral("https://control.invalid"));
        armControlPlane(client);
        m_fake->answerMe(200, accountBody());
        m_fake->answerWallet(200, walletBody(minutes));
        QVERIFY(client.credentialStore()->storeToken(TokenStore::accessTokenName(),
                                                     QString::fromLatin1(kAccessToken)));
        client.restoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(client.balanceMinutes(), qint64(minutes), 15000);
    }

    static QByteArray playRefusalBody(const QString& sentence, const QString& reference,
                                      const QString& failure = QString())
    {
        QJsonObject body;
        body.insert(QStringLiteral("status"), false);
        body.insert(QStringLiteral("error"), sentence);
        body.insert(QStringLiteral("reference"), reference);
        if (!failure.isEmpty()) {
            body.insert(QStringLiteral("failure"), failure);
        }
        return QJsonDocument(body).toJson(QJsonDocument::Compact);
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
        m_backupRoot.reset(new QTemporaryDir);
        QVERIFY(m_backupRoot->isValid());
        m_fake = nullptr;
    }

    void cleanup()
    {
        m_fake = nullptr;
        m_dir.reset();
        m_backupRoot.reset();
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
        QVERIFY2(client.liveSession(), "a session is live from the moment it is attached");
        armControlPlane(client);

        QSignalSpy completed(client.teardown(), &TeardownController::teardownCompleted);
        QSignalSpy failed(client.teardown(), &TeardownController::teardownFailed);
        QSignalSpy stages(client.teardown(), &TeardownController::stageEntered);
        QSignalSpy lifecycleEnded(client.session(), &SessionLifecycle::readyForDeletion);

        // The engine's own signal, arriving the way the engine delivers it.
        emit engine->readyForDeletion();

        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 15000);
        QCOMPARE(failed.count(), 0);
        QVERIFY2(!client.liveSession(), "a session that has been torn down is not offered for resume");

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

    void aCredentialWrittenBy014IsRestoredFromItsOldSlot()
    {
        // 0.1.4 stored the credential under the refresh-token slot name. An install that updates
        // to this build must open on Home, not on sign-in (Pitfall 6, WINDOWS #22).
        SeatHubClient client;
        isolateStore(client);
        QVERIFY(client.credentialStore()->storeToken(TokenStore::refreshTokenName(),
                                                     QStringLiteral("opaque-access-token")));
        armControlPlane(client);
        m_fake->answerMe(200, accountBody());
        m_fake->answerWallet(200, walletBody(30));

        client.restoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);

        QCOMPARE(m_fake->authorizationFor(QStringLiteral("/api/me")),
                 QByteArrayLiteral("Bearer opaque-access-token"));
        QVERIFY(client.credentialStore()->hasToken(TokenStore::accessTokenName()));
        QVERIFY2(!client.credentialStore()->hasToken(TokenStore::refreshTokenName()),
                 "the old slot is emptied once the credential is carried across");
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

    // --- Phase 5 plan 06: the email-and-password route and the website links --------------------

    static QByteArray refusalBody(const QString& sentence, const QString& reference)
    {
        QJsonObject body;
        body.insert(QStringLiteral("error"), sentence);
        body.insert(QStringLiteral("reference"), reference);
        return QJsonDocument(body).toJson(QJsonDocument::Compact);
    }

    void aPasswordSignInStoresTheCredentialTheSameWayTheCodePathDoesAndReachesHome()
    {
        SeatHubClient client;
        isolateStore(client);
        armControlPlane(client);
        m_fake->answerLogin(200, QByteArrayLiteral("{\"access_token\":\"sb_at_from_login\"}"));
        m_fake->answerWallet(200, walletBody(45));
        // A 0.1.x install left a credential in the old slot: two at rest would let a later launch
        // restore the wrong one.
        QVERIFY(client.credentialStore()->storeToken(TokenStore::refreshTokenName(),
                                                     QStringLiteral("sb_rt_OLD-SLOT")));

        QSignalSpy accepted(&client, &SeatHubClient::passwordSignInAccepted);
        QSignalSpy rejected(&client, &SeatHubClient::passwordSignInRejected);

        // Surrounding space is not part of an identifier; nothing else is normalised.
        client.signInWithPassword(QStringLiteral("  Lina@Example.com "),
                                  QStringLiteral("correct horse battery"));
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);

        QCOMPARE(accepted.count(), 1);
        QCOMPARE(rejected.count(), 0);

        // The account sign-in route, unauthenticated, with the identifier as typed.
        QVERIFY(m_fake->requestPaths().contains(QStringLiteral("/api/auth/login")));
        QVERIFY(m_fake->authorizationFor(QStringLiteral("/api/auth/login")).isEmpty());
        const QJsonObject sent = QJsonDocument::fromJson(
            m_fake->bodyFor(QStringLiteral("/api/auth/login"))).object();
        QCOMPARE(sent.value(QStringLiteral("identifier")).toString(),
                 QStringLiteral("Lina@Example.com"));
        QCOMPARE(sent.value(QStringLiteral("password")).toString(),
                 QStringLiteral("correct horse battery"));

        // The credential is stored exactly as the code path stores it: the access slot, nothing in
        // the old refresh slot, and the same bearer on the next request.
        QCOMPARE(client.credentialStore()->retrieveToken(TokenStore::accessTokenName()),
                 QStringLiteral("sb_at_from_login"));
        QVERIFY(!client.credentialStore()->hasToken(TokenStore::refreshTokenName()));
        QCOMPARE(client.identity(), QStringLiteral("Lina@Example.com"));
        QTRY_COMPARE_WITH_TIMEOUT(client.balanceText(), QStringLiteral("45 min"), 15000);
        QCOMPARE(m_fake->authorizationFor(QStringLiteral("/api/wallet")),
                 QByteArrayLiteral("Bearer sb_at_from_login"));

        // The password is nowhere on disk in the clear (the store holds DPAPI blobs only).
        const QStringList files = QDir(m_dir->path()).entryList(QDir::Files);
        for (const QString& name : files) {
            QFile blob(QDir(m_dir->path()).filePath(name));
            QVERIFY(blob.open(QIODevice::ReadOnly));
            const QByteArray bytes = blob.readAll();
            QVERIFY(!bytes.contains("correct horse battery"));
            QVERIFY(!bytes.contains("sb_at_from_login"));
        }
    }

    void eachPasswordRefusalIsShownVerbatimWithItsReference_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QString>("sentence");
        QTest::addColumn<QString>("reference");

        QTest::newRow("no-account") << 404
                                    << QStringLiteral("No account uses that email or phone.")
                                    << QStringLiteral("SH-3K2XQ1");
        QTest::newRow("wrong-password") << 401 << QStringLiteral("That password is wrong.")
                                        << QStringLiteral("SH-4F7KQ2");
        QTest::newRow("disabled")
            << 403 << QString::fromUtf16(u"That account is disabled \u2014 contact support.")
            << QStringLiteral("SH-5M8NP3");
    }

    void eachPasswordRefusalIsShownVerbatimWithItsReference()
    {
        QFETCH(int, status);
        QFETCH(QString, sentence);
        QFETCH(QString, reference);

        SeatHubClient client;
        isolateStore(client);
        armControlPlane(client);
        m_fake->answerLogin(status, refusalBody(sentence, reference));

        QSignalSpy accepted(&client, &SeatHubClient::passwordSignInAccepted);
        QSignalSpy rejected(&client, &SeatHubClient::passwordSignInRejected);

        client.signInWithPassword(QStringLiteral("lina@example.com"), QStringLiteral("hunter22hunter"));
        QTRY_COMPARE_WITH_TIMEOUT(rejected.count(), 1, 15000);

        QCOMPARE(rejected.first().at(0).toString(), sentence);
        QCOMPARE(rejected.first().at(1).toString(), reference);
        QCOMPARE(accepted.count(), 0);
        // A refusal signs nobody in and stores nothing.
        QVERIFY(client.appState() != QStringLiteral("home"));
        QVERIFY(!client.credentialStore()->hasToken(TokenStore::accessTokenName()));
        QVERIFY(!client.controlPlane()->hasAccessToken());
    }

    void aPasswordSignInThatNeverReachesTheServerSaysTheOfflineSentence()
    {
        SeatHubClient client;
        isolateStore(client);
        armControlPlane(client);
        m_fake->answerLogin(0, QByteArray());

        QSignalSpy rejected(&client, &SeatHubClient::passwordSignInRejected);
        client.signInWithPassword(QStringLiteral("lina@example.com"), QStringLiteral("hunter22hunter"));
        QTRY_COMPARE_WITH_TIMEOUT(rejected.count(), 1, 15000);

        QCOMPARE(rejected.first().at(0).toString(),
                 QStringLiteral("We couldn't reach SevenHills. Try again in a moment."));
        QVERIFY(rejected.first().at(1).toString().isEmpty());
        QVERIFY(!client.credentialStore()->hasToken(TokenStore::accessTokenName()));
    }

    void anEmptyIdentifierOrPasswordIsNamedBeforeAnythingIsSent()
    {
        SeatHubClient client;
        isolateStore(client);
        armControlPlane(client);

        QSignalSpy rejected(&client, &SeatHubClient::passwordSignInRejected);
        client.signInWithPassword(QStringLiteral("   "), QStringLiteral("hunter22hunter"));
        client.signInWithPassword(QStringLiteral("lina@example.com"), QString());

        QCOMPARE(rejected.count(), 2);
        QCOMPARE(rejected.at(0).at(0).toString(), QStringLiteral("Enter your email or phone number."));
        QCOMPARE(rejected.at(1).at(0).toString(), QStringLiteral("Enter your password."));
        QVERIFY2(!m_fake->requestPaths().contains(QStringLiteral("/api/auth/login")),
                 "a field-level mistake must not cost a round trip");
    }

    void theCodePathSaysTheOfflineSentenceToo()
    {
        SeatHubClient client;
        isolateStore(client);
        armControlPlane(client);
        m_fake->answerOtpRequest(0);

        QSignalSpy rejected(&client, &SeatHubClient::otpRejected);
        client.requestOtp(QStringLiteral("+962790000000"));
        QTRY_COMPARE_WITH_TIMEOUT(rejected.count(), 1, 15000);
        QCOMPARE(rejected.first().at(0).toString(),
                 QStringLiteral("We couldn't reach SevenHills. Try again in a moment."));
        QVERIFY(rejected.first().at(1).toString().isEmpty());
    }

    void aNationalNumberWithNoCountryIsRefusedLocallyAndNamed()
    {
        SeatHubClient client;
        isolateStore(client);
        armControlPlane(client);

        QSignalSpy rejected(&client, &SeatHubClient::otpRejected);
        client.requestOtp(QStringLiteral("0790000000"));
        QCOMPARE(rejected.count(), 1);
        QCOMPARE(rejected.first().at(0).toString(),
                 QStringLiteral("That doesn't look like a phone number."));
        QVERIFY(!m_fake->requestPaths().contains(QStringLiteral("/api/auth/otp/request")));
    }

    void theWebsiteLinksAreTheSpecsAddressesAndCarryNoCredential()
    {
        SeatHubClient client;
        isolateStore(client);
        armControlPlane(client);
        m_fake->answerMe(200, accountBody());
        m_fake->answerWallet(200, walletBody(10));
        client.credentialStore()->storeToken(TokenStore::accessTokenName(),
                                             QStringLiteral("opaque-access-token"));
        client.restoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);

        QCOMPARE(client.websiteUrl(QStringLiteral("signup")),
                 QStringLiteral("https://sevenhills.damra.co/login?mode=signup"));
        QCOMPARE(client.websiteUrl(QStringLiteral("reset")),
                 QStringLiteral("https://sevenhills.damra.co/forgot-password"));
        QCOMPARE(client.websiteUrl(QStringLiteral("topup")),
                 QStringLiteral("https://sevenhills.damra.co/topup"));
        QVERIFY(client.websiteUrl(QStringLiteral("anywhere-else")).isEmpty());

        // What would open is recorded, not launched.
        QList<QUrl> opened;
        client.setUrlOpener([&opened](const QUrl& url) {
            opened.append(url);
            return true;
        });
        QVERIFY(client.openWebsite(QStringLiteral("signup")));
        QVERIFY(client.openWebsite(QStringLiteral("reset")));
        QVERIFY(!client.openWebsite(QStringLiteral("anywhere-else")));
        QCOMPARE(opened.size(), 2);

        // Signed in, with a credential in memory and on disk - and no address holds any of it: no
        // token, no identity, and no query beyond the one `signup` path the spec records.
        for (const QUrl& url : opened) {
            const QString text = url.toString();
            QVERIFY2(!text.contains(QStringLiteral("opaque-access-token")), qPrintable(text));
            QVERIFY2(!text.contains(QStringLiteral("lina")), qPrintable(text));
            QVERIFY2(!text.contains(QStringLiteral("6f1c6f5e")), qPrintable(text));
            QVERIFY2(!text.contains(QStringLiteral("token"), Qt::CaseInsensitive), qPrintable(text));
        }
        QCOMPARE(opened.at(0).query(), QStringLiteral("mode=signup"));
        QVERIFY(opened.at(1).query().isEmpty());
    }

    // --- Phase 5 plan 07: the header's two facade needs -----------------------------------------

    void theTopUpInvokableOpensTheSpecsExactAddressAndNothingElse()
    {
        // The menu's `Top up` and Home's quiet top-up control both call this and nothing else: the
        // view holds no address and spells no target (T-05-31).
        SeatHubClient client;
        isolateStore(client);
        armControlPlane(client);
        m_fake->answerMe(200, accountBody());
        m_fake->answerWallet(200, walletBody(10));
        client.credentialStore()->storeToken(TokenStore::accessTokenName(),
                                             QStringLiteral("opaque-access-token"));
        client.restoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);

        QList<QUrl> opened;
        client.setUrlOpener([&opened](const QUrl& url) {
            opened.append(url);
            return true;
        });

        QVERIFY(client.openTopUp());
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened.at(0).toString(), QStringLiteral("https://sevenhills.damra.co/topup"));

        // Signed in, with a credential in memory and on disk, and the address carries none of it: no
        // query, no fragment, no identity, no token.
        QVERIFY(opened.at(0).query().isEmpty());
        QVERIFY(opened.at(0).fragment().isEmpty());
        const QString text = opened.at(0).toString();
        QVERIFY2(!text.contains(QStringLiteral("opaque-access-token")), qPrintable(text));
        QVERIFY2(!text.contains(QStringLiteral("lina")), qPrintable(text));
        QVERIFY2(!text.contains(QStringLiteral("6f1c6f5e")), qPrintable(text));

        // It is the same address `websiteUrl("topup")` gives, so the two cannot drift apart.
        QCOMPARE(opened.at(0).toString(), client.websiteUrl(QStringLiteral("topup")));

        // A browser that could not be started reports it, and nothing else happens.
        client.setUrlOpener([](const QUrl&) { return false; });
        QVERIFY(!client.openTopUp());
    }

    void signedInIsTrueFromConfirmationToSignOutAndSaysSoWhenItChanges()
    {
        // The shell puts the header on a view only while this holds, so it has to follow every way a
        // customer becomes signed in (a confirmed restore, an offline restore) and signed out.
        SeatHubClient client;
        isolateStore(client);
        armControlPlane(client);
        m_fake->answerMe(200, accountBody());
        m_fake->answerWallet(200, walletBody(90));
        client.credentialStore()->storeToken(TokenStore::accessTokenName(),
                                             QStringLiteral("opaque-access-token"));

        QVERIFY(!client.signedIn());
        QSignalSpy changes(&client, &SeatHubClient::signedInChanged);

        client.restoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);
        QVERIFY(client.signedIn());
        QCOMPARE(changes.count(), 1);

        client.signOut();
        QVERIFY(!client.signedIn());
        QCOMPARE(changes.count(), 2);
        QCOMPARE(client.appState(), QStringLiteral("signed_out"));
    }

    void anOfflineRestoreIsStillSignedInSoTheHeaderShowsItsLastKnownBalance()
    {
        SeatHubClient client;
        isolateStore(client);
        armControlPlane(client);
        m_fake->answerMe(0, QByteArray());
        m_fake->answerWallet(0, QByteArray());
        client.credentialStore()->storeToken(TokenStore::accessTokenName(),
                                             QStringLiteral("opaque-access-token"));

        client.restoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);
        QVERIFY(client.signedIn());
        // The wallet read failed and none ever succeeded: the header's word is `Unavailable`.
        QTRY_VERIFY_WITH_TIMEOUT(client.balanceStale(), 15000);
        QCOMPARE(client.balanceMinutes(), qint64(-1));
    }

    void aRefusedCredentialIsNeverSignedIn()
    {
        SeatHubClient client;
        isolateStore(client);
        armControlPlane(client);
        m_fake->answerMe(401, refusedBody());
        client.credentialStore()->storeToken(TokenStore::accessTokenName(),
                                             QStringLiteral("opaque-access-token"));

        client.restoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("signed_out"), 15000);
        QVERIFY(!client.signedIn());
    }

    // --- Phase 5 plan 07: what Home needs from the facade ----------------------------------------

    void aRefusalFromTheServerIsShownOnHomeInItsOwnWordsAndAnythingElseIsAnError_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QString>("failure");
        QTest::addColumn<QString>("expectedHome");
        QTest::addColumn<QString>("expectedApp");

        // The balance floor and a session the customer already has: the server said no, in words.
        QTest::newRow("the balance floor (402)")
            << 402 << QString() << "refused" << "home";
        QTest::newRow("a session already live (409)")
            << 409 << "USER_HAS_NONTERMINAL_SESSION" << "refused" << "home";
        // Nothing free is its own line, not a refusal.
        QTest::newRow("nothing free (409)")
            << 409 << "NO_HOST_AVAILABLE" << "busy" << "home";
        // Anything else is a real failure and keeps the error view.
        QTest::newRow("an unknown profile (400)") << 400 << QString() << "ready" << "error";
        QTest::newRow("the server broke (500)") << 500 << QString() << "ready" << "error";
        QTest::newRow("a refused credential (401)") << 401 << QString() << "ready" << "error";
        // A request that never arrived is the offline line.
        QTest::newRow("no route to the control plane") << 0 << QString() << "offline" << "home";
    }

    void aRefusalFromTheServerIsShownOnHomeInItsOwnWordsAndAnythingElseIsAnError()
    {
        QFETCH(int, status);
        QFETCH(QString, failure);
        QFETCH(QString, expectedHome);
        QFETCH(QString, expectedApp);

        SeatHubClient client;
        // A balance of nothing: the client must still ask, because it has no rule of its own about
        // whether a customer may play - only the server refuses (CUST-10, T-05-32).
        reachHome(client, 0);
        QVERIFY(!QTest::currentTestFailed());

        const QString sentence = QStringLiteral("The server's own sentence, exactly as written.");
        m_fake->answerPlay(status, playRefusalBody(sentence, QStringLiteral("SH-4F7KQ2"), failure));

        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.homeStatus() == QLatin1String("checking"), false, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), expectedApp, 15000);
        QCOMPARE(client.homeStatus(), expectedHome);

        // It asked, once, whatever the balance.
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions")), 1);

        if (expectedHome == QLatin1String("refused")) {
            // The sentence and the reference are the server's, unchanged, and readable while Home is
            // showing; the client did not turn the refusal into a failure screen.
            QCOMPARE(client.failure().value(QStringLiteral("error")).toString(), sentence);
            QCOMPARE(client.reference(), QStringLiteral("SH-4F7KQ2"));
            QVERIFY(!client.liveSession());
        }
        if (expectedHome == QLatin1String("busy") || expectedHome == QLatin1String("offline")) {
            QVERIFY(client.failure().isEmpty());
        }
        if (expectedApp == QLatin1String("error")) {
            QVERIFY(!client.failure().isEmpty());
        }

        // Pressing Play again asks again, and clears whatever was on screen from the last answer.
        m_fake->answerPlay(500, playRefusalBody(sentence, QStringLiteral("SH-9K2XQ1")));
        client.dismissError();
        QCOMPARE(client.appState(), QStringLiteral("home"));
    }

    void aSessionTheServerHasNotEndedIsLiveAndPlayResumesItInsteadOfAskingForAnother()
    {
        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());
        QVERIFY(!client.liveSession());
        QSignalSpy liveChanges(&client, &SeatHubClient::liveSessionChanged);

        // A session is attached (Play's allocation returned it). The rig has no answer in this test,
        // so pairing fails and the customer lands on the error view with the session still attached.
        client.beginSession(QStringLiteral("session-live"));
        QVERIFY(client.liveSession());
        QCOMPARE(liveChanges.count(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("error"), 15000);
        QVERIFY2(client.liveSession(), "a failed step does not end the session on the server");

        // Back on Home the session is still live: Play reads Resume session (the screen binds to this
        // flag), and pressing it resumes that session. It does NOT ask for a new one.
        client.dismissError();
        QCOMPARE(client.appState(), QStringLiteral("home"));
        QVERIFY(client.liveSession());
        const int pairingBefore =
            m_fake->requestPaths().count(QStringLiteral("/api/sessions/session-live/pairing"));
        QVERIFY(pairingBefore >= 1);

        client.start();
        QCOMPARE(client.appState(), QStringLiteral("connecting"));
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().count(QStringLiteral("/api/sessions/session-live/pairing"))
                > pairingBefore,
            15000);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions")), 0);

        // Sign-out forgets it: the next customer on this PC is never offered this one's session.
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("error"), 15000);
        client.signOut();
        QVERIFY(!client.liveSession());
    }

    void aSessionTheServerReportsOverIsNoLongerOfferedForResume()
    {
        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());

        client.beginSession(QStringLiteral("session-live"));
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("error"), 15000);
        client.dismissError();
        QVERIFY(client.liveSession());

        // The control plane says it is over (the wallet ran out, an operator ended it). It arrives on
        // the session channel, on the network thread, and reaches the facade the way it does in
        // production: queued.
        SessionInfo info;
        info.id = QStringLiteral("session-live");
        info.state = QStringLiteral("COMPLETED");
        info.endReason = QStringLiteral("BALANCE_EXHAUSTED");
        SessionWebSocket* channel = client.sessionChannel();
        QMetaObject::invokeMethod(
            channel, [channel, info]() { emit channel->sessionStateReceived(info); },
            Qt::BlockingQueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(!client.liveSession(), 15000);

        // So Play is Play again, and pressing it asks the server for a new session.
        m_fake->answerPlay(402, playRefusalBody(QStringLiteral("Not enough credit."),
                                            QStringLiteral("SH-3K2XQ1")));
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.homeStatus(), QStringLiteral("refused"), 15000);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions")), 1);
    }

    void aSessionThatEndsAndTearsDownIsNoLongerLive()
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
        QVERIFY(client.liveSession());
        armControlPlane(client);

        QSignalSpy completed(client.teardown(), &TeardownController::teardownCompleted);
        emit engine->readyForDeletion();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!client.liveSession(), 15000);

        client.session()->attachSession(nullptr);
        delete engine;
    }

    // --- the connecting stages are the session's own state (CUST-12, ADR-0055) ------------------------

    /// A session as the pairing poll would hand it to the facade.
    static SessionInfo sessionIn(const QString& state, const QString& id = QStringLiteral("s-stages"))
    {
        SessionInfo info;
        info.id = id;
        info.state = state;
        return info;
    }

    /// Reports `info` the way a poll tick does: a signal of the pairing controller, which the facade
    /// listens to. Emitted from here it reaches the facade directly, in order, with no timing.
    static void report(SeatHubClient& client, const SessionInfo& info)
    {
        emit client.pairing()->sessionRead(info);
    }

    /// A facade that has begun `s-stages` and whose rig is never ready: pairing keeps polling (409)
    /// so the poll's own session read has something to ride on.
    void beginStagedSession(SeatHubClient& client)
    {
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());
        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));
        client.beginSession(QStringLiteral("s-stages"));
        QCOMPARE(client.appState(), QStringLiteral("connecting"));
    }

    void beginningASessionNeverOpensTheSessionSocket()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());

        QSignalSpy channelState(client.sessionChannel(), &SessionWebSocket::stateChanged);
        QSignalSpy dropped(client.sessionChannel(), &SessionWebSocket::dropped);

        // Long enough for several poll ticks - and for a socket that had been opened to have tried,
        // failed against the unresolvable base address and scheduled its first reconnect.
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-stages/pairing")) >= 3, 15000);

        QVERIFY(!client.sessionChannel()->isOpen());
        QCOMPARE(client.sessionChannel()->reconnectAttempts(), 0);
        QCOMPARE(channelState.count(), 0);
        QCOMPARE(dropped.count(), 0);
        // The session is read over HTTP instead, on that same tick.
        QVERIFY(m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-stages")));
        QVERIFY(!m_fake->requestPaths().contains(QStringLiteral("/ws/session/s-stages")));
    }

    void noStageIsDoneBeforeTheFirstAnswerAboutTheSession()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());

        // Nothing has been read: no stage reached, no line, nothing marked done.
        QCOMPARE(client.connectStage(), 0);
        QVERIFY(client.stageText().isEmpty());
    }

    void aSessionWalkingTheRealStatesAdvancesTheStepperInOrder()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        QSignalSpy stages(&client, &SeatHubClient::connectStageChanged);

        report(client, sessionIn(QStringLiteral("ALLOCATED")));
        QCOMPARE(client.connectStage(), 1);
        QCOMPARE(client.stageText(), QStringLiteral("Preparing the rig"));

        report(client, sessionIn(QStringLiteral("PREPARING")));
        QCOMPARE(client.connectStage(), 1);

        report(client, sessionIn(QStringLiteral("READY")));
        QCOMPARE(client.connectStage(), 2);
        QCOMPARE(client.stageText(), QStringLiteral("Preparing the stream"));

        report(client, sessionIn(QStringLiteral("ACTIVE")));
        QCOMPARE(client.connectStage(), 3);
        QCOMPARE(client.stageText(), QStringLiteral("Streaming"));

        // Three moves, each announced once, in order.
        QCOMPARE(stages.count(), 3);
    }

    void aLateReplyCarryingAnEarlierStateChangesNothing()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());

        report(client, sessionIn(QStringLiteral("READY")));
        QCOMPARE(client.connectStage(), 2);
        QSignalSpy stages(&client, &SeatHubClient::connectStageChanged);
        QSignalSpy lines(&client, &SeatHubClient::stageTextChanged);

        // The same tick's slower reply, or last tick's, arriving second.
        report(client, sessionIn(QStringLiteral("PREPARING")));
        report(client, sessionIn(QStringLiteral("ALLOCATED")));

        QCOMPARE(client.connectStage(), 2);
        QCOMPARE(client.stageText(), QStringLiteral("Preparing the stream"));
        QCOMPARE(stages.count(), 0);
        QCOMPARE(lines.count(), 0);
    }

    void twoStatesSeenInOneTickLandOnTheLaterStageWithoutShowingTheEarlierOneTwice()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        QSignalSpy stages(&client, &SeatHubClient::connectStageChanged);

        // A tick that brings two answers at once: the rig was preparing and is already ready.
        report(client, sessionIn(QStringLiteral("PREPARING")));
        report(client, sessionIn(QStringLiteral("READY")));
        QCOMPARE(client.connectStage(), 2);
        QCOMPARE(stages.count(), 2);
    }

    void aFirstAnswerAlreadyPastTheFirstStageGoesStraightThere()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        QSignalSpy stages(&client, &SeatHubClient::connectStageChanged);

        // The earlier stage is never shown as if it had been waited on.
        report(client, sessionIn(QStringLiteral("READY")));
        QCOMPARE(client.connectStage(), 2);
        QCOMPARE(stages.count(), 1);
    }

    void aTickThatReportsNoChangeLeavesTheStepperExactlyAsItWas()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());

        report(client, sessionIn(QStringLiteral("PREPARING")));
        QSignalSpy stages(&client, &SeatHubClient::connectStageChanged);
        QSignalSpy lines(&client, &SeatHubClient::stageTextChanged);

        report(client, sessionIn(QStringLiteral("PREPARING")));
        report(client, sessionIn(QStringLiteral("PREPARING")));
        // A state that belongs to no stage moves nothing either: a value this client does not know is
        // never turned into progress.
        report(client, sessionIn(QStringLiteral("SOMETHING_NEW")));
        report(client, sessionIn(QString()));

        QCOMPARE(client.connectStage(), 1);
        QCOMPARE(stages.count(), 0);
        QCOMPARE(lines.count(), 0);
    }

    void anAnswerAboutAnotherSessionIsNotThisOnesToSpeakFor()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());

        report(client, sessionIn(QStringLiteral("READY"), QStringLiteral("some-other-session")));
        QCOMPARE(client.connectStage(), 0);
    }

    void theSessionIsReadOnThePairingPollAndTheStagesFollowIt()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        QSignalSpy stages(&client, &SeatHubClient::connectStageChanged);

        // The control plane's answer changes between ticks, as a real session's does.
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("PREPARING"));
        QTRY_COMPARE_WITH_TIMEOUT(client.connectStage(), 1, 15000);

        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("READY"));
        QTRY_COMPARE_WITH_TIMEOUT(client.connectStage(), 2, 15000);

        // Later ticks that bring the same answer change nothing.
        QTest::qWait(800);
        QCOMPARE(client.connectStage(), 2);
        QCOMPARE(stages.count(), 2);

        // The read rides the poll the pairing controller already runs: for every authorization poll
        // there is at most one session read, and there is no timer of the facade's own.
        const int polls =
            m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-stages/pairing"));
        const int reads = m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-stages"));
        QVERIFY2(reads >= 1 && reads <= polls, "one session read per poll tick, at most");
    }

    void theEnginesOwnStagesStayInsideTheSecondStageAndOnlyTheStreamStartingReachesTheThird()
    {
        auto* engine = new FakeEngineSession;
        {
            SeatHubClient client;
            client.session()->attachSession(engine);

            client.start();
            QCOMPARE(client.appState(), QStringLiteral("connecting"));
            QCOMPARE(client.connectStage(), 0);

            // Any of the engine's own stages is the second stage under way, and never past it; its own
            // name for the stage is not what the customer reads.
            emit engine->stageStarting(QStringLiteral("RTSP handshake"));
            QCOMPARE(client.connectStage(), 2);
            QCOMPARE(client.stageText(), QStringLiteral("Preparing the stream"));
            emit engine->stageStarting(QStringLiteral("Audio stream initialization"));
            QCOMPARE(client.connectStage(), 2);

            emit engine->connectionStarted();
            QCOMPARE(client.connectStage(), 3);
            QCOMPARE(client.stageText(), QStringLiteral("Streaming"));

            emit engine->sessionFinished(0);
            emit engine->readyForDeletion();
            QCOMPARE(client.appState(), QStringLiteral("signed_out"));
        }
        delete engine;
    }

    void aNewSessionStartsItsStagesAgainFromNothing()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        report(client, sessionIn(QStringLiteral("READY")));
        QCOMPARE(client.connectStage(), 2);

        client.beginSession(QStringLiteral("s-stages"));
        QCOMPARE(client.connectStage(), 0);
        QVERIFY(client.stageText().isEmpty());
    }

    void theFacadeHandsTheScreenTheBundledCountriesAndARegionThatIsOneOfThem()
    {
        SeatHubClient client;
        QCOMPARE(client.countries().size(), SeatHubCountries::all().size());
        QVERIFY(client.countries().size() > 0);
        QVERIFY(SeatHubCountries::contains(client.defaultCountryCode()));
        QCOMPARE(client.toE164(QStringLiteral("0790000000"), QStringLiteral("+962")),
                 QStringLiteral("+962790000000"));
        QCOMPARE(client.toE164(QStringLiteral("0790000000"), QString()), QString());
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
