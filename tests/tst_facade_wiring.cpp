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
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
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
#include <QTimeZone>
#include <QTimer>
#include <QUrlQuery>
#include <QWindow>

#include <atomic>
#include <functional>

#include "seathub/control_plane_client.h"
#include "seathub/countries.h"
#include "seathub/duration_text.h"
#include "seathub/engine_session.h"
#include "seathub/jordan_time.h"
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

// Phase 5 plan 12 (CUST-17): `create()` above always returns null, so `handleHostResolved()`
// never reaches a real `MoonlightEngineSession` to call this on - it is unreachable-by-
// construction in this binary, exactly like `create()` itself. Defined only so this translation
// unit links without pulling in the real engine (`moonlight_engine_session.cpp`, deliberately not
// part of this suite - see this file's own header).
void MoonlightEngineSession::setDebugLineFilter(const QStringList&)
{
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
    FakeReply(int httpStatus, const QByteArray& body, QObject* parent, int delayMs = 0)
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
        QTimer::singleShot(delayMs, this, [this]() {
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

    /// What a profile list route answers for the page at `cursor` (empty for the first page). `path` is
    /// the route (`/api/sessions`, `/api/wallet/history`, `/api/topup-notices`). Status 0 is a
    /// transport failure. A page nobody answered for is an empty last page.
    void answerList(const QString& path, const QString& cursor, int status, const QByteArray& body)
    {
        QMutexLocker lock(&m_mutex);
        m_lists.insert(path + QLatin1Char('|') + cursor, qMakePair(status, body));
    }
    /// What `GET /api/usage` answers.
    void answerUsage(int status, const QByteArray& body)
    {
        QMutexLocker lock(&m_mutex);
        m_usageStatus = status;
        m_usageBody = body;
    }
    /// Every list reply arrives this many milliseconds after it was asked for, so a test can ask a
    /// second time while the first is still in flight.
    void delayLists(int milliseconds)
    {
        QMutexLocker lock(&m_mutex);
        m_listDelay = milliseconds;
    }
    /// The query of every request `path` has received, in order (`limit=15&cursor=...`).
    QStringList queriesFor(const QString& path) const
    {
        QMutexLocker lock(&m_mutex);
        return m_queries.value(path);
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
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request,
                                 QIODevice* outgoing) override
    {
        const QString path = request.url().path();
        const bool isGet = operation == QNetworkAccessManager::GetOperation;
        const bool isList = isGet && (path == QLatin1String("/api/sessions")
                                      || path == QLatin1String("/api/wallet/history")
                                      || path == QLatin1String("/api/topup-notices"));
        const QString cursor = QUrlQuery(request.url()).queryItemValue(
            QStringLiteral("cursor"), QUrl::FullyDecoded);
        int meStatus, walletStatus, logoutStatus, loginStatus, otpRequestStatus, playStatus;
        int pairingStatus;
        bool pairingCustom;
        QByteArray meBody, walletBody, loginBody, playBody, pairingBody, sessionBody;
        {
            QMutexLocker lock(&m_mutex);
            m_paths.append(path);
            m_queries[path].append(request.url().query(QUrl::FullyEncoded));
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

        if (isList) {
            QPair<int, QByteArray> answer;
            int delay;
            {
                QMutexLocker lock(&m_mutex);
                delay = m_listDelay;
                answer = m_lists.value(path + QLatin1Char('|') + cursor, qMakePair(200, QByteArray()));
            }
            if (answer.second.isEmpty()) {
                const char* member = path.endsWith(QLatin1String("history")) ? "entries"
                                     : path.endsWith(QLatin1String("notices")) ? "notices"
                                                                               : "sessions";
                answer.second = QStringLiteral("{\"%1\":[],\"next_cursor\":null}")
                                    .arg(QLatin1String(member)).toUtf8();
            }
            return new FakeReply(answer.first, answer.second, this, delay);
        }
        if (isGet && path == QLatin1String("/api/usage")) {
            int usageStatus;
            QByteArray usageBody;
            {
                QMutexLocker lock(&m_mutex);
                usageStatus = m_usageStatus;
                usageBody = m_usageBody;
            }
            return new FakeReply(usageStatus, usageBody, this);
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
    QHash<QString, QStringList> m_queries;
    QHash<QString, QPair<int, QByteArray>> m_lists;
    int m_listDelay = 0;
    int m_usageStatus = 200;
    QByteArray m_usageBody = QByteArrayLiteral("{\"minutes_played\":0,\"balance_minutes\":0}");
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

/// Waits for `condition` WITHOUT running this thread's event loop, and says whether it came true.
///
/// This is the point of the in-stream tests below. The facade's own thread is suspended for the whole
/// of a stream (`session.cpp:1965-1966`), so anything a stream must do has to happen without it; a
/// `QTRY_*` spins the event loop and would let a queued hop to this thread rescue a design that is
/// dead in production. Here the loop is never run, so only what the network thread does by itself
/// can make the condition true.
bool waitWithoutTheEventLoop(const std::function<bool()>& condition, int timeoutMs = 10000)
{
    QElapsedTimer clock;
    clock.start();
    while (!condition()) {
        if (clock.elapsed() > timeoutMs) {
            return false;
        }
        QThread::msleep(5);
    }
    return true;
}

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

    // --- Phase 5 plan 09: builders for the profile's pages -----------------------------------------

    static QByteArray pageBody(const char* member, const QJsonArray& rows, const QString& next)
    {
        QJsonObject body;
        body.insert(QLatin1String(member), rows);
        body.insert(QStringLiteral("next_cursor"),
                    next.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(next));
        return QJsonDocument(body).toJson(QJsonDocument::Compact);
    }

    /// One finished session as the contract's `CustomerSessionRow` has it. `host_id` and `host_name`
    /// are NOT part of that row: they are put on the wire here on purpose, to prove a client that
    /// were handed a rig's identity would still never show it.
    static QJsonObject sessionRow(const QString& id, int minutes,
                                  const QString& endReason = QStringLiteral("CUSTOMER_ENDED"),
                                  const QString& requestedAt = QStringLiteral("2026-09-12T18:40:00Z"))
    {
        QJsonObject row;
        row.insert(QStringLiteral("id"), id);
        row.insert(QStringLiteral("state"), QStringLiteral("COMPLETED"));
        row.insert(QStringLiteral("minutes_billed"), minutes);
        row.insert(QStringLiteral("requested_at"), requestedAt);
        row.insert(QStringLiteral("end_reason"), endReason);
        row.insert(QStringLiteral("host_id"), QStringLiteral("rig-7-secret-id"));
        row.insert(QStringLiteral("host_name"), QStringLiteral("Rig 07 Secret Name"));
        return row;
    }

    static QJsonObject ledgerRowJson(const QString& id, const QString& kind, int amount,
                                     const QString& createdAt = QStringLiteral("2026-09-12T18:40:00Z"))
    {
        QJsonObject row;
        row.insert(QStringLiteral("id"), id);
        row.insert(QStringLiteral("kind"), kind);
        row.insert(QStringLiteral("amount_minutes"), amount);
        row.insert(QStringLiteral("created_at"), createdAt);
        return row;
    }

    /// An open notice when `creditedAt` is empty (both credited members null), a closed one otherwise.
    static QJsonObject noticeRow(const QString& id, const QString& sentAt, const QString& creditedAt,
                                 int creditedMinutes)
    {
        QJsonObject row;
        row.insert(QStringLiteral("id"), id);
        row.insert(QStringLiteral("sent_at"), sentAt);
        row.insert(QStringLiteral("credited_at"),
                   creditedAt.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(creditedAt));
        row.insert(QStringLiteral("credited_minutes"),
                   creditedAt.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(creditedMinutes));
        return row;
    }

    static QByteArray usageBody(int played, int balance)
    {
        return QStringLiteral("{\"minutes_played\":%1,\"balance_minutes\":%2}")
            .arg(played).arg(balance).toUtf8();
    }

    static QByteArray errorJson(const QString& sentence, const QString& reference)
    {
        return QStringLiteral("{\"error\":\"%1\",\"reference\":\"%2\"}").arg(sentence, reference)
            .toUtf8();
    }

    /// A signed-in facade on Home with the profile open and the totals and identity read.
    void reachProfile(SeatHubClient& client, int minutes = 90)
    {
        reachHome(client, minutes);
        m_fake->answerUsage(200, usageBody(135, minutes));
        client.openProfile();
        QVERIFY(client.inProfile());
        QTRY_COMPARE_WITH_TIMEOUT(client.totalsStatus(), QStringLiteral("ready"), 15000);
    }

    static QString cell(CustomerListModel* list, int row, int role)
    {
        return list->index(row, 0).data(role).toString();
    }

    /// `app/seathub`, found from the test binary's own directory the way the other suites find it.
    static QString seathubSourceDir()
    {
        QDir dir(QCoreApplication::applicationDirPath());
        for (int depth = 0; depth < 8; ++depth) {
            const QString candidate = dir.filePath(QStringLiteral("app/seathub"));
            if (QFile::exists(candidate + QStringLiteral("/customer_lists.cpp"))) {
                return candidate;
            }
            if (!dir.cdUp()) {
                break;
            }
        }
        return QString();
    }

    /// One in-stream wallet answer, as counted on the network thread. Counted with direct connections,
    /// because the main thread's event loop is deliberately never run while these tests wait.
    struct WalletTicks
    {
        std::atomic<int> read{0};
        std::atomic<int> failed{0};
        std::atomic<qint64> lastBalance{-1};
    };

    /// A facade streaming under a control-plane session, its HUD begun and its liveness reporter
    /// running on the network thread, with `walletMinutes` as what the wallet answers. The pre-stream
    /// balance (what Home showed) is `homeMinutes`.
    void beginStreaming(SeatHubClient& client, FakeEngineSession* engine, WalletTicks* ticks,
                        int homeMinutes, int walletMinutes)
    {
        reachHome(client, homeMinutes);
        QVERIFY(!QTest::currentTestFailed());
        client.session()->attachSession(engine);
        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));
        m_fake->answerWallet(200, walletBody(walletMinutes));

        // Counted where the liveness reporter emits them: on its own thread, as the HUD receives them.
        connect(client.liveness(), &LivenessTimer::walletRead, &client,
                [ticks](qint64 minutes) {
                    ticks->lastBalance.store(minutes);
                    ticks->read.fetch_add(1);
                },
                Qt::DirectConnection);
        connect(client.liveness(), &LivenessTimer::walletReadFailed, &client,
                [ticks]() { ticks->failed.fetch_add(1); }, Qt::DirectConnection);

        client.beginSession(QStringLiteral("s-live"));
        emit engine->connectionStarted();
        QCOMPARE(client.appState(), QStringLiteral("streaming"));
    }

    /// Runs one more liveness tick and waits, without the event loop, for its wallet answer to have
    /// been dealt with (read or failed).
    bool tickAndWait(SeatHubClient& client, WalletTicks* ticks)
    {
        const int before = ticks->read.load() + ticks->failed.load();
        client.liveness()->tick();
        return waitWithoutTheEventLoop(
            [&]() { return ticks->read.load() + ticks->failed.load() > before; });
    }

    static void endStreaming(SeatHubClient& client, FakeEngineSession* engine)
    {
        client.session()->attachSession(nullptr);
        delete engine;
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
        // so pairing fails and connecting stops where it was, with the session still attached.
        client.beginSession(QStringLiteral("session-live"));
        QVERIFY(client.liveSession());
        QCOMPARE(liveChanges.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(client.connectFailed(), 15000);
        QCOMPARE(client.appState(), QStringLiteral("connecting"));
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
        QTRY_VERIFY_WITH_TIMEOUT(client.connectFailed(), 15000);
        client.signOut();
        QVERIFY(!client.liveSession());
    }

    void aSessionTheServerReportsOverIsNoLongerOfferedForResume()
    {
        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());

        client.beginSession(QStringLiteral("session-live"));
        QTRY_VERIFY_WITH_TIMEOUT(client.connectFailed(), 15000);
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

    // --- a stall names its step and says what the server decided (CUST-13) --------------------------------

    /// What the customer must never read anywhere on a stalled connect: an internal state, an end-reason
    /// key, or a stage string of the engine's.
    static void verifyNoInternalNameIsShown(const SeatHubClient& client)
    {
        const QStringList shown = {client.stalledStepText(), client.stalledReasonText(),
                                   client.failure().value(QStringLiteral("error")).toString(),
                                   client.stageText()};
        const QStringList internal = {
            QStringLiteral("READINESS_TIMEOUT"), QStringLiteral("CONNECT_TIMEOUT"),
            QStringLiteral("MODE_BOOT_TIMEOUT"),  QStringLiteral("BALANCE_EXHAUSTED"),
            QStringLiteral("HOST_LOST"),          QStringLiteral("CLIENT_SILENT"),
            QStringLiteral("SESSION_LOST"),       QStringLiteral("SOMETHING_NEW"),
            QStringLiteral("PREPARING"),          QStringLiteral("ALLOCATED"),
            QStringLiteral("EXPIRED"),            QStringLiteral("FAILED"),
            QStringLiteral("RTSP"),               QStringLiteral("STAGE_")};
        for (const QString& text : shown) {
            for (const QString& name : internal) {
                QVERIFY2(!text.contains(name), qPrintable(QStringLiteral("'%1' shows '%2'").arg(text, name)));
            }
        }
    }

    void aStalledConnectNamesItsStepAndSaysWhatTheServerDecided_data()
    {
        QTest::addColumn<QString>("reachedState");
        QTest::addColumn<QString>("state");
        QTest::addColumn<QString>("endReason");
        QTest::addColumn<int>("minutes");
        QTest::addColumn<QString>("step");
        QTest::addColumn<QString>("plain");
        QTest::addColumn<QString>("styled");

        const QString rig = QStringLiteral("Stopped at: Preparing the rig");
        const QString stream = QStringLiteral("Stopped at: Preparing the stream");
        const QString mono = QStringLiteral("<font face=\"Geist Mono\">%1</font>");

        QTest::newRow("the rig did not come back (no charge)")
            << "PREPARING" << "FAILED" << "READINESS_TIMEOUT" << 0 << rig
            << "This rig didn't come back in time. You were not charged."
            << "This rig didn't come back in time. You were not charged.";
        QTest::newRow("a mode boot that ran out (no charge)")
            << "PREPARING" << "FAILED" << "MODE_BOOT_TIMEOUT" << 0 << rig
            << "This rig didn't come back in time. You were not charged."
            << "This rig didn't come back in time. You were not charged.";
        QTest::newRow("the customer never started streaming (no charge)")
            << "READY" << "EXPIRED" << "CONNECT_TIMEOUT" << 0 << stream
            << "You didn't start streaming in time, so the session was released. You were not charged."
            << "You didn't start streaming in time, so the session was released. You were not charged.";
        QTest::newRow("the rig was lost, with minutes charged")
            << "READY" << "FAILED" << "HOST_LOST" << 3 << stream
            << "We lost contact with this rig, so the session ended. You were charged for 3 minutes."
            << QStringLiteral("We lost contact with this rig, so the session ended. You were charged "
                              "for %1 minutes.").arg(mono.arg(3));
        QTest::newRow("support ended it")
            << "ALLOCATED" << "CANCELLED" << "OPERATOR_FORCED" << 0 << rig
            << "Support ended this session. You were charged for 0 minutes."
            << QStringLiteral("Support ended this session. You were charged for %1 minutes.")
                   .arg(mono.arg(0));
        QTest::newRow("the balance ran out")
            << "READY" << "COMPLETED" << "BALANCE_EXHAUSTED" << 12 << stream
            << "Your balance ran out, so the session ended."
            << "Your balance ran out, so the session ended.";
    }

    void aStalledConnectNamesItsStepAndSaysWhatTheServerDecided()
    {
        QFETCH(QString, reachedState);
        QFETCH(QString, state);
        QFETCH(QString, endReason);
        QFETCH(int, minutes);
        QFETCH(QString, step);
        QFETCH(QString, plain);
        QFETCH(QString, styled);

        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        report(client, sessionIn(reachedState));
        const int reached = client.connectStage();
        QSignalSpy stalls(&client, &SeatHubClient::connectFailedChanged);
        QVERIFY(!client.connectFailed());

        SessionInfo over = sessionIn(state);
        over.endReason = endReason;
        over.minutesBilled = minutes;
        report(client, over);

        // Still the connecting view: the stage that was active is what failed, and it says so.
        QCOMPARE(client.appState(), QStringLiteral("connecting"));
        QVERIFY(client.connectFailed());
        QCOMPARE(stalls.count(), 1);
        QCOMPARE(client.stalledStepText(), step);
        QCOMPARE(client.stalledReasonText(), styled);
        QCOMPARE(client.failure().value(QStringLiteral("error")).toString(), plain);
        // The stepper is frozen where it stopped; a later answer does not move it.
        QCOMPARE(client.connectStage(), reached);
        report(client, sessionIn(QStringLiteral("ACTIVE")));
        QCOMPARE(client.connectStage(), reached);
        // The server ended it, so it is not a session Home offers to resume.
        QVERIFY(!client.liveSession());
        // A second failure while this one is showing does not rewrite what the customer read.
        emit client.pairing()->pairingFailed(SeatHubFailure::local(QStringLiteral("Something else.")));
        QCOMPARE(client.stalledReasonText(), styled);
        QCOMPARE(stalls.count(), 1);

        verifyNoInternalNameIsShown(client);
        QVERIFY(client.reference().isEmpty());

        // The pairing poll stopped with the session: nothing keeps asking for a pairing that cannot
        // happen.
        QTest::qWait(400);
        const int polls = m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-stages/pairing"));
        QTest::qWait(600);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-stages/pairing")), polls);
    }

    void aFailureTheServerGaveNoReasonForStillNamesTheStepAndFallsBackToTheGenericSentence_data()
    {
        QTest::addColumn<QString>("state");
        QTest::addColumn<QString>("endReason");

        QTest::newRow("no reason at all") << "FAILED" << QString();
        // Reasons the deck has no line for fall back the same way, and are never shown as a bare key.
        QTest::newRow("a reason the deck has no line for") << "FAILED" << "SESSION_LOST";
        QTest::newRow("a reason this client has never heard of") << "EXPIRED" << "SOMETHING_NEW";
    }

    void aFailureTheServerGaveNoReasonForStillNamesTheStepAndFallsBackToTheGenericSentence()
    {
        QFETCH(QString, state);
        QFETCH(QString, endReason);

        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        report(client, sessionIn(QStringLiteral("PREPARING")));

        SessionInfo over = sessionIn(state);
        over.endReason = endReason;
        report(client, over);

        QVERIFY(client.connectFailed());
        QCOMPARE(client.stalledStepText(), QStringLiteral("Stopped at: Preparing the rig"));
        // The generic sentence, never blank and never a bare code. It has no reference: the client
        // does not make one up (ADR-0008), and the server named none for this.
        QCOMPARE(client.stalledReasonText(), QStringLiteral("Something went wrong on our side."));
        QCOMPARE(client.failure().value(QStringLiteral("error")).toString(),
                 QStringLiteral("Something went wrong on our side."));
        QVERIFY(client.reference().isEmpty());
        verifyNoInternalNameIsShown(client);
    }

    void theClientsOwnPairingDeadlineIsAStallToo()
    {
        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());
        // A run of 409s no longer stalls this deadline (fork `15962514`: "a 409 while the rig is
        // prepared does not spend the pairing deadline" - it restarts D-08's clock on every "not
        // yet" answer). A transport failure still does, so the authorization poll never resolving
        // at all is what makes this deadline expire.
        m_fake->answerPairing(0);

        // The deadline is measured by the pairing controller, on its own thread: shorten it there, then
        // let a real pairing wait for a rig that never becomes ready.
        PairingController* pairing = client.pairing();
        QMetaObject::invokeMethod(
            pairing, [pairing]() { pairing->setDeadlineMs(400); }, Qt::BlockingQueuedConnection);
        client.beginSession(QStringLiteral("s-stages"));

        QTRY_VERIFY_WITH_TIMEOUT(client.connectFailed(), 15000);
        QCOMPARE(client.appState(), QStringLiteral("connecting"));
        // Nothing was read about the session, so it stopped at the stage every session begins at.
        QCOMPARE(client.stalledStepText(), QStringLiteral("Stopped at: Preparing the rig"));
        // The client's own sentence, from the copy it already had; no reference, because none exists.
        QCOMPARE(client.stalledReasonText(), QStringLiteral("The rig didn't finish connecting. Try again."));
        QVERIFY(client.reference().isEmpty());
        // The session is still the server's to end: Try again picks it up rather than asking for another.
        QVERIFY(client.liveSession());
        verifyNoInternalNameIsShown(client);
    }

    void aRefusalOfTheConnectReadIsTheServersOwnSentenceWithItsReference()
    {
        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());
        // The default answer for a session's pairing read: "no rig is assigned", with a reference.
        client.beginSession(QStringLiteral("session-refused"));

        QTRY_VERIFY_WITH_TIMEOUT(client.connectFailed(), 15000);
        QCOMPARE(client.stalledReasonText(), QStringLiteral("no rig is assigned"));
        QCOMPARE(client.reference(), QStringLiteral("SH-9K2XQ1"));
        QCOMPARE(client.failure().value(QStringLiteral("error")).toString(), QStringLiteral("no rig is assigned"));
    }

    void anEngineFailureBeforeTheStreamStartsIsAStallAtTheSecondStage()
    {
        auto* engine = new FakeEngineSession;
        {
            SeatHubClient client;
            client.session()->attachSession(engine);
            beginStagedSession(client);
            QVERIFY(!QTest::currentTestFailed());

            emit engine->stageStarting(QStringLiteral("RTSP handshake"));
            QCOMPARE(client.connectStage(), 2);
            emit engine->stageFailed(QStringLiteral("RTSP handshake"), -1, QString());

            QCOMPARE(client.appState(), QStringLiteral("connecting"));
            QVERIFY(client.connectFailed());
            QCOMPARE(client.stalledStepText(), QStringLiteral("Stopped at: Preparing the stream"));
            // The engine's own words are diagnostic only.
            QCOMPARE(client.stalledReasonText(), QStringLiteral("Something went wrong on our side."));
            verifyNoInternalNameIsShown(client);

            client.session()->attachSession(nullptr);
        }
        delete engine;
    }

    void aStallOffersTryAgainAndBackToHomeAndNeitherIsAnotherRig()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        report(client, sessionIn(QStringLiteral("PREPARING")));
        SessionInfo over = sessionIn(QStringLiteral("FAILED"));
        over.endReason = QStringLiteral("READINESS_TIMEOUT");
        report(client, over);
        QVERIFY(client.connectFailed());
        QVERIFY(!client.liveSession());

        // Try again asks the server for a session, exactly as Play does: the server chooses the rig
        // (CUST-03). The client names none and offers none.
        m_fake->answerPlay(402, playRefusalBody(QStringLiteral("Not enough credit."),
                                            QStringLiteral("SH-3K2XQ1")));
        client.retry();
        QVERIFY(!client.connectFailed());
        QTRY_COMPARE_WITH_TIMEOUT(client.homeStatus(), QStringLiteral("refused"), 15000);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions")), 1);
        const QByteArray sent = m_fake->bodyFor(QStringLiteral("/api/sessions"));
        QVERIFY2(!sent.contains("host"), "the request names no rig");
    }

    void backToHomeLeavesTheStalledViewAndForgetsIt()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        report(client, sessionIn(QStringLiteral("READY")));
        SessionInfo over = sessionIn(QStringLiteral("EXPIRED"));
        over.endReason = QStringLiteral("CONNECT_TIMEOUT");
        report(client, over);
        QVERIFY(client.connectFailed());
        QSignalSpy stalls(&client, &SeatHubClient::connectFailedChanged);

        client.dismissError();

        QCOMPARE(client.appState(), QStringLiteral("home"));
        QVERIFY(!client.connectFailed());
        QVERIFY(client.stalledStepText().isEmpty());
        QVERIFY(client.stalledReasonText().isEmpty());
        QVERIFY(client.failure().isEmpty());
        QCOMPARE(stalls.count(), 1);
    }

    // --- the end reason on Home comes from the session itself (CUST-15, D-21) -----------------------------

    void aFinishedSessionTellsHomeWhyItEndedFromTheSessionReadAfterTeardown_data()
    {
        QTest::addColumn<QString>("endReason");
        QTest::addColumn<int>("minutes");
        QTest::addColumn<QString>("expected");

        const QString mono = QStringLiteral("<font face=\"Geist Mono\">%1</font>");
        // The one CUST-15 asks for.
        QTest::newRow("the balance ran out")
            << "BALANCE_EXHAUSTED" << 42 << "Your balance ran out, so the session ended.";
        QTest::newRow("the customer ended it")
            << "CUSTOMER_ENDED" << 15
            << "You ended the session. Unused minutes stay in your account.";
        // A sentence with a number in it takes the session's own billed minutes, in mono.
        QTest::newRow("the rig was lost, with a number")
            << "HOST_LOST" << 23
            << QStringLiteral("We lost contact with this rig, so the session ended. You were charged "
                              "for %1 minutes.").arg(mono.arg(23));
        // No reason: Home says nothing rather than something invented.
        QTest::newRow("no reason at all") << QString() << 5 << QString();
    }

    void aFinishedSessionTellsHomeWhyItEndedFromTheSessionReadAfterTeardown()
    {
        QFETCH(QString, endReason);
        QFETCH(int, minutes);
        QFETCH(QString, expected);

        auto* engine = new FakeEngineSession;
        {
            SeatHubClient client;
            client.session()->attachSession(engine);
            beginStagedSession(client);
            QVERIFY(!QTest::currentTestFailed());
            client.teardown()->setVerifyIntervalMs(1);

            // The stream ran: the pairing poll is over, so what happens next is the session's end and
            // the read teardown makes.
            PairingController* pairing = client.pairing();
            QMetaObject::invokeMethod(
                pairing, [pairing]() { pairing->cancel(); }, Qt::BlockingQueuedConnection);
            QSignalSpy channelDropped(client.sessionChannel(), &SessionWebSocket::dropped);
            emit engine->connectionStarted();
            QCOMPARE(client.appState(), QStringLiteral("streaming"));
            QVERIFY(client.endReasonText().isEmpty());

            m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("COMPLETED"), endReason,
                                  minutes);
            QSignalSpy completed(client.teardown(), &TeardownController::teardownCompleted);
            emit engine->sessionFinished(0);
            emit engine->readyForDeletion();
            QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 15000);

            QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);
            QCOMPARE(client.endReasonText(), expected);
            // Fed by the session read, not by a socket: none was ever opened (ending a session closes
            // the channel, which is not opening it).
            QVERIFY(!client.sessionChannel()->isOpen());
            QCOMPARE(client.sessionChannel()->reconnectAttempts(), 0);
            QCOMPARE(channelDropped.count(), 0);
            QVERIFY(client.sessionChannel()->state() != QLatin1String("connecting"));

            // The next Play forgets it.
            client.session()->attachSession(nullptr);
        }
        delete engine;
    }

    void theEndReasonIsForgottenWhenTheNextSessionBegins()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        SessionInfo over = sessionIn(QStringLiteral("FAILED"));
        over.endReason = QStringLiteral("READINESS_TIMEOUT");
        report(client, over);
        QVERIFY(!client.endReasonText().isEmpty());

        client.beginSession(QStringLiteral("s-stages"));
        QVERIFY(client.endReasonText().isEmpty());
        QVERIFY(!client.connectFailed());
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

    // --- Phase 5 plan 09: the profile ---------------------------------------------------------------

    void theProfileIsAViewInsideHomeThatReadsTheTotalsAndTheIdentityAndNoListYet()
    {
        SeatHubClient client;
        reachHome(client, 45);
        m_fake->answerUsage(200, usageBody(135, 45));
        QSignalSpy changes(&client, &SeatHubClient::inProfileChanged);
        const int meReadsBefore = m_fake->countOfPathEndingWith(QStringLiteral("/api/me"));

        client.openProfile();

        // A view inside Home, like Settings: it keeps the header and is not a state of its own.
        QVERIFY(client.inProfile());
        QCOMPARE(changes.count(), 1);
        QCOMPARE(client.appState(), QStringLiteral("home"));

        // The two totals are the server's numbers, formatted and nothing more.
        QTRY_COMPARE_WITH_TIMEOUT(client.totalsStatus(), QStringLiteral("ready"), 15000);
        QCOMPARE(client.hoursPlayedText(), QStringLiteral("2 h 15 min"));
        QCOMPARE(client.creditLeftText(), QStringLiteral("45 min"));
        QCOMPARE(client.accountStatus(), QStringLiteral("ready"));

        // The identity is read afresh on every visit (the in-session sign-ins never fill it).
        QTRY_COMPARE_WITH_TIMEOUT(m_fake->countOfPathEndingWith(QStringLiteral("/api/me")),
                                  meReadsBefore + 1, 15000);
        QCOMPARE(client.account().value(QStringLiteral("username")).toString(), QStringLiteral("lina"));

        // Lazy: the lists are asked for by the view, one at a time as their tab is opened.
        QCOMPARE(m_fake->queriesFor(QStringLiteral("/api/sessions")).size(), 0);
        QCOMPARE(m_fake->queriesFor(QStringLiteral("/api/wallet/history")).size(), 0);
        QCOMPARE(m_fake->queriesFor(QStringLiteral("/api/topup-notices")).size(), 0);
        QCOMPARE(client.sessionHistory()->status(), QStringLiteral("idle"));
        QCOMPARE(client.creditHistory()->status(), QStringLiteral("idle"));
        QCOMPARE(client.topupHistory()->status(), QStringLiteral("idle"));

        client.closeProfile();
        QVERIFY(!client.inProfile());
        QCOMPARE(changes.count(), 2);
        QCOMPARE(client.totalsStatus(), QStringLiteral("idle"));
        QVERIFY(client.hoursPlayedText().isEmpty());
    }

    void theProfileOpensOnlyForASignedInCustomerOnHome()
    {
        SeatHubClient client;
        // Constructed in "restoring": nobody is signed in and no view is Home yet.
        client.openProfile();
        QVERIFY(!client.inProfile());

        // Signed out.
        isolateStore(client);
        client.restoreSession();
        QCOMPARE(client.appState(), QStringLiteral("signed_out"));
        client.openProfile();
        QVERIFY(!client.inProfile());

        // The list calls do nothing either: there is no credential to ask with.
        client.loadFirstPage(QStringLiteral("sessions"));
        client.loadNextPage(QStringLiteral("sessions"));
        client.reloadList(QStringLiteral("sessions"));
        QCOMPARE(client.sessionHistory()->status(), QStringLiteral("idle"));
    }

    void anUnknownListNameIsANoOp()
    {
        SeatHubClient client;
        reachProfile(client);
        client.loadFirstPage(QStringLiteral("rigs"));
        client.loadNextPage(QStringLiteral("rigs"));
        client.reloadList(QStringLiteral("rigs"));
        QTest::qWait(150);
        QCOMPARE(m_fake->queriesFor(QStringLiteral("/api/sessions")).size(), 0);
        QCOMPARE(m_fake->queriesFor(QStringLiteral("/api/wallet/history")).size(), 0);
        QCOMPARE(m_fake->queriesFor(QStringLiteral("/api/topup-notices")).size(), 0);
    }

    // The six paging behaviours (CUST-14, D-14) ------------------------------------------------------

    void theFirstPageFillsTheListWithRowsThatAreAlreadyDisplayText()
    {
        SeatHubClient client;
        reachProfile(client);
        m_fake->answerList(QStringLiteral("/api/sessions"), QString(), 200,
                           pageBody("sessions",
                                    { sessionRow(QStringLiteral("a"), 135),
                                      sessionRow(QStringLiteral("b"), 45,
                                                 QStringLiteral("BALANCE_EXHAUSTED")),
                                      sessionRow(QStringLiteral("c"), 0, QStringLiteral("CONNECT_TIMEOUT"),
                                                 QStringLiteral("2026-09-11T10:00:00Z")) },
                                    QStringLiteral("c1")));

        SessionListModel* list = client.sessionHistory();
        client.loadFirstPage(QStringLiteral("sessions"));
        QCOMPARE(list->status(), QStringLiteral("loading"));
        QTRY_COMPARE_WITH_TIMEOUT(list->status(), QStringLiteral("ready"), 15000);

        QCOMPARE(list->count(), 3);
        QVERIFY(list->hasMore());
        QVERIFY(!list->loadingMore());
        QVERIFY(list->errorText().isEmpty());

        // Date and time in Jordan time, how it ended in the deck's short words, and the length.
        QCOMPARE(cell(list, 0, CustomerListModel::WhenRole), QStringLiteral("Sat 12 Sep, 21:40"));
        QCOMPARE(cell(list, 0, CustomerListModel::KindRole), QStringLiteral("You ended it"));
        QCOMPARE(cell(list, 0, CustomerListModel::AmountRole), QStringLiteral("2 h 15 min"));
        QCOMPARE(cell(list, 1, CustomerListModel::KindRole), QStringLiteral("Balance ran out"));
        QCOMPARE(cell(list, 1, CustomerListModel::AmountRole), QStringLiteral("45 min"));
        QCOMPARE(cell(list, 2, CustomerListModel::WhenRole), QStringLiteral("Fri 11 Sep, 13:00"));
        QCOMPARE(cell(list, 2, CustomerListModel::KindRole),
                 QStringLiteral("Not started in time, not charged"));
        QCOMPARE(cell(list, 2, CustomerListModel::AmountRole), QStringLiteral("0 min"));

        // The first page: a page of fifteen and no cursor at all.
        QCOMPARE(CustomerListModel::kPageSize, 15);
        QCOMPARE(m_fake->queriesFor(QStringLiteral("/api/sessions")),
                 QStringList{ QStringLiteral("limit=15") });
        QCOMPARE(m_fake->authorizationFor(QStringLiteral("/api/sessions")),
                 QByteArrayLiteral("Bearer opaque-access-token"));
    }

    void reachingTheEndWithACursorLoadsTheNextPageAndAppendsItWithoutRefetching()
    {
        SeatHubClient client;
        reachProfile(client);
        m_fake->answerList(QStringLiteral("/api/sessions"), QString(), 200,
                           pageBody("sessions",
                                    { sessionRow(QStringLiteral("a"), 10), sessionRow(QStringLiteral("b"), 20) },
                                    QStringLiteral("c1")));
        m_fake->answerList(QStringLiteral("/api/sessions"), QStringLiteral("c1"), 200,
                           pageBody("sessions",
                                    { sessionRow(QStringLiteral("c"), 30), sessionRow(QStringLiteral("d"), 40) },
                                    QString()));

        SessionListModel* list = client.sessionHistory();
        client.loadFirstPage(QStringLiteral("sessions"));
        QTRY_COMPARE_WITH_TIMEOUT(list->status(), QStringLiteral("ready"), 15000);
        QCOMPARE(list->count(), 2);
        QVERIFY(list->hasMore());

        QSignalSpy inserted(list, &QAbstractItemModel::rowsInserted);
        client.loadNextPage(QStringLiteral("sessions"));
        QVERIFY(list->loadingMore());
        QTRY_COMPARE_WITH_TIMEOUT(list->count(), 4, 15000);

        QVERIFY(!list->loadingMore());
        QVERIFY(!list->hasMore());
        // Appended after the two already there, in the order the server gave them.
        QCOMPARE(cell(list, 0, CustomerListModel::AmountRole), QStringLiteral("10 min"));
        QCOMPARE(cell(list, 1, CustomerListModel::AmountRole), QStringLiteral("20 min"));
        QCOMPARE(cell(list, 2, CustomerListModel::AmountRole), QStringLiteral("30 min"));
        QCOMPARE(cell(list, 3, CustomerListModel::AmountRole), QStringLiteral("40 min"));
        // Two rows arrived, once: what was on screen was not asked for again.
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(m_fake->queriesFor(QStringLiteral("/api/sessions")),
                 (QStringList{ QStringLiteral("limit=15"), QStringLiteral("limit=15&cursor=c1") }));
    }

    void reachingTheEndWithNoCursorLeftAsksForNothing()
    {
        SeatHubClient client;
        reachProfile(client);
        m_fake->answerList(QStringLiteral("/api/sessions"), QString(), 200,
                           pageBody("sessions", { sessionRow(QStringLiteral("a"), 10) }, QString()));

        SessionListModel* list = client.sessionHistory();

        // Before the first page has been asked for there is nothing to page.
        client.loadNextPage(QStringLiteral("sessions"));
        QCOMPARE(list->status(), QStringLiteral("idle"));
        QCOMPARE(m_fake->queriesFor(QStringLiteral("/api/sessions")).size(), 0);

        client.loadFirstPage(QStringLiteral("sessions"));
        QTRY_COMPARE_WITH_TIMEOUT(list->status(), QStringLiteral("ready"), 15000);
        QVERIFY(!list->hasMore());

        // The last page: reaching the end again and again asks for nothing.
        for (int i = 0; i < 3; ++i) {
            client.loadNextPage(QStringLiteral("sessions"));
        }
        QTest::qWait(200);
        QCOMPARE(list->count(), 1);
        QVERIFY(!list->loadingMore());
        QCOMPARE(m_fake->queriesFor(QStringLiteral("/api/sessions")).size(), 1);
    }

    void aPageThatFailsKeepsTheRowsAlreadyLoadedAndRecordsWhy()
    {
        SeatHubClient client;
        reachProfile(client);
        m_fake->answerList(QStringLiteral("/api/sessions"), QString(), 200,
                           pageBody("sessions",
                                    { sessionRow(QStringLiteral("a"), 10), sessionRow(QStringLiteral("b"), 20) },
                                    QStringLiteral("c1")));
        m_fake->answerList(QStringLiteral("/api/sessions"), QStringLiteral("c1"), 500,
                           errorJson(QStringLiteral("We couldn't load that page."),
                                     QStringLiteral("SH-4F7KQ2")));

        SessionListModel* list = client.sessionHistory();
        client.loadFirstPage(QStringLiteral("sessions"));
        QTRY_COMPARE_WITH_TIMEOUT(list->status(), QStringLiteral("ready"), 15000);

        client.loadNextPage(QStringLiteral("sessions"));
        QTRY_VERIFY_WITH_TIMEOUT(list->moreFailed(), 15000);

        // The rows on screen stay; the list is still "ready", not "error" (that is the first page's
        // state); the server's own sentence and reference are kept for the footer row.
        QCOMPARE(list->status(), QStringLiteral("ready"));
        QCOMPARE(list->count(), 2);
        QVERIFY(!list->loadingMore());
        QVERIFY2(list->hasMore(), "the cursor is kept so the same page can be asked for again");
        QCOMPARE(list->errorText(), QStringLiteral("We couldn't load that page."));
        QCOMPARE(list->errorReference(), QStringLiteral("SH-4F7KQ2"));

        // Try again asks for that same page, and the rows arrive after the two that were kept.
        m_fake->answerList(QStringLiteral("/api/sessions"), QStringLiteral("c1"), 200,
                           pageBody("sessions", { sessionRow(QStringLiteral("c"), 30) }, QString()));
        client.loadNextPage(QStringLiteral("sessions"));
        QTRY_COMPARE_WITH_TIMEOUT(list->count(), 3, 15000);
        QVERIFY(!list->moreFailed());
        QVERIFY(list->errorText().isEmpty());
        QCOMPARE(cell(list, 2, CustomerListModel::AmountRole), QStringLiteral("30 min"));
    }

    void aPageThatNeverArrivedSaysTheDecksOfflineSentenceAndKeepsTheRows()
    {
        SeatHubClient client;
        reachProfile(client);
        m_fake->answerList(QStringLiteral("/api/sessions"), QString(), 200,
                           pageBody("sessions", { sessionRow(QStringLiteral("a"), 10) },
                                    QStringLiteral("c1")));
        m_fake->answerList(QStringLiteral("/api/sessions"), QStringLiteral("c1"), 0, QByteArray());

        SessionListModel* list = client.sessionHistory();
        client.loadFirstPage(QStringLiteral("sessions"));
        QTRY_COMPARE_WITH_TIMEOUT(list->status(), QStringLiteral("ready"), 15000);
        client.loadNextPage(QStringLiteral("sessions"));
        QTRY_VERIFY_WITH_TIMEOUT(list->moreFailed(), 15000);

        QCOMPARE(list->errorText(), SeatHubFailure::offlineSentence());
        QVERIFY2(list->errorReference().isEmpty(), "a request that never arrived has no reference");
        QCOMPARE(list->count(), 1);
    }

    void aSecondRequestWhileOneIsInFlightIsIgnored()
    {
        SeatHubClient client;
        reachProfile(client);
        m_fake->answerList(QStringLiteral("/api/sessions"), QString(), 200,
                           pageBody("sessions",
                                    { sessionRow(QStringLiteral("a"), 10), sessionRow(QStringLiteral("b"), 20) },
                                    QStringLiteral("c1")));
        m_fake->answerList(QStringLiteral("/api/sessions"), QStringLiteral("c1"), 200,
                           pageBody("sessions", { sessionRow(QStringLiteral("c"), 30) }, QString()));
        m_fake->delayLists(400);

        SessionListModel* list = client.sessionHistory();

        // The first page, asked for three times while the first is still out: one request.
        client.loadFirstPage(QStringLiteral("sessions"));
        client.loadFirstPage(QStringLiteral("sessions"));
        client.loadNextPage(QStringLiteral("sessions"));
        QTRY_COMPARE_WITH_TIMEOUT(list->status(), QStringLiteral("ready"), 15000);
        QCOMPARE(m_fake->queriesFor(QStringLiteral("/api/sessions")).size(), 1);
        QCOMPARE(list->count(), 2);

        // The next page, asked for four times while the first is still out: one request, one append.
        for (int i = 0; i < 4; ++i) {
            client.loadNextPage(QStringLiteral("sessions"));
        }
        QVERIFY(list->loadingMore());
        QTRY_COMPARE_WITH_TIMEOUT(list->count(), 3, 15000);
        QTest::qWait(700);
        QCOMPARE(list->count(), 3);
        QCOMPARE(m_fake->queriesFor(QStringLiteral("/api/sessions")).size(), 2);
    }

    void eachListAndTheTotalsFailOnTheirOwn()
    {
        SeatHubClient client;
        reachProfile(client);
        // Sessions fails; credit history and top-ups answer; the totals are already in.
        m_fake->answerList(QStringLiteral("/api/sessions"), QString(), 500,
                           errorJson(QStringLiteral("We couldn't load your sessions."),
                                     QStringLiteral("SH-4F7KQ2")));
        m_fake->answerList(QStringLiteral("/api/wallet/history"), QString(), 200,
                           pageBody("entries",
                                    { ledgerRowJson(QStringLiteral("l1"), QStringLiteral("topup_credit"), 300) },
                                    QString()));
        m_fake->answerList(QStringLiteral("/api/topup-notices"), QString(), 200,
                           pageBody("notices",
                                    { noticeRow(QStringLiteral("n1"), QStringLiteral("2026-09-13T09:00:00Z"),
                                                QString(), 0) },
                                    QString()));

        client.loadFirstPage(QStringLiteral("sessions"));
        client.loadFirstPage(QStringLiteral("credit"));
        client.loadFirstPage(QStringLiteral("topups"));

        QTRY_COMPARE_WITH_TIMEOUT(client.sessionHistory()->status(), QStringLiteral("error"), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(client.creditHistory()->status(), QStringLiteral("ready"), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(client.topupHistory()->status(), QStringLiteral("ready"), 15000);

        // The failed list says why, in the server's words, and has no rows; the others and the totals
        // and the identity rows are intact.
        QCOMPARE(client.sessionHistory()->errorText(), QStringLiteral("We couldn't load your sessions."));
        QCOMPARE(client.sessionHistory()->errorReference(), QStringLiteral("SH-4F7KQ2"));
        QCOMPARE(client.sessionHistory()->count(), 0);
        QCOMPARE(client.creditHistory()->count(), 1);
        QVERIFY(client.creditHistory()->errorText().isEmpty());
        QCOMPARE(client.topupHistory()->count(), 1);
        QCOMPARE(client.totalsStatus(), QStringLiteral("ready"));
        QCOMPARE(client.accountStatus(), QStringLiteral("ready"));

        // Loading a failed list again neither refetches the others nor needs them.
        client.loadFirstPage(QStringLiteral("sessions"));
        QTest::qWait(150);
        QCOMPARE(m_fake->queriesFor(QStringLiteral("/api/sessions")).size(), 1);

        m_fake->answerList(QStringLiteral("/api/sessions"), QString(), 200,
                           pageBody("sessions", { sessionRow(QStringLiteral("a"), 10) }, QString()));
        client.reloadList(QStringLiteral("sessions"));
        QCOMPARE(client.sessionHistory()->status(), QStringLiteral("loading"));
        QTRY_COMPARE_WITH_TIMEOUT(client.sessionHistory()->status(), QStringLiteral("ready"), 15000);
        QCOMPARE(client.sessionHistory()->count(), 1);
        QVERIFY(client.sessionHistory()->errorText().isEmpty());
        QCOMPARE(m_fake->queriesFor(QStringLiteral("/api/wallet/history")).size(), 1);
        QCOMPARE(m_fake->queriesFor(QStringLiteral("/api/topup-notices")).size(), 1);
    }

    void aReloadStartsAgainFromTheTopAndDropsTheRowsAndAnyReplyStillOnItsWay()
    {
        SeatHubClient client;
        reachProfile(client);
        m_fake->answerList(QStringLiteral("/api/sessions"), QString(), 200,
                           pageBody("sessions",
                                    { sessionRow(QStringLiteral("a"), 10), sessionRow(QStringLiteral("b"), 20) },
                                    QStringLiteral("c1")));
        m_fake->answerList(QStringLiteral("/api/sessions"), QStringLiteral("c1"), 200,
                           pageBody("sessions", { sessionRow(QStringLiteral("c"), 30) }, QString()));

        SessionListModel* list = client.sessionHistory();
        client.loadFirstPage(QStringLiteral("sessions"));
        QTRY_COMPARE_WITH_TIMEOUT(list->status(), QStringLiteral("ready"), 15000);

        // A next page is on its way when the customer reloads: the reply belongs to the list that was
        // reset, so it must never be appended to the new one.
        m_fake->delayLists(300);
        client.loadNextPage(QStringLiteral("sessions"));
        QVERIFY(list->loadingMore());
        client.reloadList(QStringLiteral("sessions"));
        QCOMPARE(list->count(), 0);
        QCOMPARE(list->status(), QStringLiteral("loading"));
        QVERIFY(!list->loadingMore());

        QTRY_COMPARE_WITH_TIMEOUT(list->status(), QStringLiteral("ready"), 15000);
        QTest::qWait(600);
        QCOMPARE(list->count(), 2);
        QCOMPARE(cell(list, 0, CustomerListModel::AmountRole), QStringLiteral("10 min"));
        QCOMPARE(cell(list, 1, CustomerListModel::AmountRole), QStringLiteral("20 min"));
    }

    // The other two lists' rows (D-15) -----------------------------------------------------------------

    void creditHistoryRowsSayThePlainKindAndTheSignedAmount()
    {
        SeatHubClient client;
        reachProfile(client);
        m_fake->answerList(
            QStringLiteral("/api/wallet/history"), QString(), 200,
            pageBody("entries",
                     { ledgerRowJson(QStringLiteral("l1"), QStringLiteral("topup_credit"), 300),
                       ledgerRowJson(QStringLiteral("l2"), QStringLiteral("first_bonus"), 60),
                       ledgerRowJson(QStringLiteral("l3"), QStringLiteral("session_debit"), -1),
                       ledgerRowJson(QStringLiteral("l4"), QStringLiteral("refund"), 45),
                       ledgerRowJson(QStringLiteral("l5"), QStringLiteral("adjustment"), -90),
                       ledgerRowJson(QStringLiteral("l6"), QStringLiteral("shortfall"), -5) },
                     QString()));

        CreditHistoryModel* list = client.creditHistory();
        client.loadFirstPage(QStringLiteral("credit"));
        QTRY_COMPARE_WITH_TIMEOUT(list->status(), QStringLiteral("ready"), 15000);
        QCOMPARE(list->count(), 6);

        const QStringList kinds = { QStringLiteral("Top-up"), QStringLiteral("First top-up bonus"),
                                    QStringLiteral("Played"), QStringLiteral("Refund"),
                                    QStringLiteral("Adjustment by support"),
                                    QStringLiteral("Unpaid minutes") };
        const QStringList amounts = { QStringLiteral("+5 h 00 min"), QStringLiteral("+1 h 00 min"),
                                      QStringLiteral("-1 min"), QStringLiteral("+45 min"),
                                      QStringLiteral("-1 h 30 min"), QStringLiteral("-5 min") };
        for (int row = 0; row < 6; ++row) {
            QCOMPARE(cell(list, row, CustomerListModel::WhenRole), QStringLiteral("Sat 12 Sep, 21:40"));
            QCOMPARE(cell(list, row, CustomerListModel::KindRole), kinds.at(row));
            QCOMPARE(cell(list, row, CustomerListModel::AmountRole), amounts.at(row));
            QVERIFY(cell(list, row, CustomerListModel::ToneRole).isEmpty());
        }
    }

    void topupRowsSayWaitingOrCreditedAndShowMinutesOnlyOnceCredited()
    {
        SeatHubClient client;
        reachProfile(client);
        m_fake->answerList(
            QStringLiteral("/api/topup-notices"), QString(), 200,
            pageBody("notices",
                     { noticeRow(QStringLiteral("n1"), QStringLiteral("2026-09-13T09:00:00Z"), QString(), 0),
                       noticeRow(QStringLiteral("n2"), QStringLiteral("2026-09-10T09:00:00Z"),
                                 QStringLiteral("2026-09-10T09:30:00Z"), 300) },
                     QString()));

        TopupListModel* list = client.topupHistory();
        client.loadFirstPage(QStringLiteral("topups"));
        QTRY_COMPARE_WITH_TIMEOUT(list->status(), QStringLiteral("ready"), 15000);
        QCOMPARE(list->count(), 2);

        QCOMPARE(cell(list, 0, CustomerListModel::WhenRole), QStringLiteral("Sun 13 Sep, 12:00"));
        QCOMPARE(cell(list, 0, CustomerListModel::KindRole), QStringLiteral("Waiting"));
        QCOMPARE(cell(list, 0, CustomerListModel::ToneRole), QStringLiteral("waiting"));
        QVERIFY2(cell(list, 0, CustomerListModel::AmountRole).isEmpty(),
                 "no minutes are shown while the notice is open");

        QCOMPARE(cell(list, 1, CustomerListModel::KindRole), QStringLiteral("Credited"));
        QCOMPARE(cell(list, 1, CustomerListModel::ToneRole), QStringLiteral("credited"));
        QCOMPARE(cell(list, 1, CustomerListModel::AmountRole), QStringLiteral("+5 h 00 min"));
    }

    void noRowNamesARigEvenWhenTheBodyHandsOneOver()
    {
        // The rows above carry `host_id` and `host_name` on the wire (see `sessionRow`). Nothing the
        // model exposes - through any role of any row - holds them (CUST-01, T-05-37).
        SeatHubClient client;
        reachProfile(client);
        m_fake->answerList(QStringLiteral("/api/sessions"), QString(), 200,
                           pageBody("sessions",
                                    { sessionRow(QStringLiteral("a"), 10), sessionRow(QStringLiteral("b"), 20) },
                                    QString()));
        SessionListModel* list = client.sessionHistory();
        client.loadFirstPage(QStringLiteral("sessions"));
        QTRY_COMPARE_WITH_TIMEOUT(list->status(), QStringLiteral("ready"), 15000);

        QCOMPARE(list->count(), 2);
        const QHash<int, QByteArray> roles = list->roleNames();
        QCOMPARE(roles.size(), 4);
        for (int row = 0; row < list->count(); ++row) {
            for (auto it = roles.constBegin(); it != roles.constEnd(); ++it) {
                const QString text = cell(list, row, it.key());
                QVERIFY2(!text.contains(QStringLiteral("rig-7")), qPrintable(text));
                QVERIFY2(!text.contains(QStringLiteral("Rig 07")), qPrintable(text));
                QVERIFY2(!text.contains(QStringLiteral("Secret")), qPrintable(text));
            }
        }

        // And the source that turns a page into rows never names one.
        const QString dir = seathubSourceDir();
        QVERIFY2(!dir.isEmpty(), "app/seathub could not be located from the test binary");
        QFile file(dir + QStringLiteral("/customer_lists.cpp"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString source = QString::fromUtf8(file.readAll());
        for (const QString& forbidden : { QStringLiteral("host_id"), QStringLiteral("hostName"),
                                          QStringLiteral("host_name") }) {
            QVERIFY2(!source.contains(forbidden), qPrintable(forbidden));
        }
    }

    void theListModelsDoNoArithmeticOnAMinuteTheServerSent()
    {
        // T-05-39: every number a row shows is the server's, printed as it came. There is no adding
        // up, counting or inferring in the code that builds a row.
        const QString dir = seathubSourceDir();
        QVERIFY2(!dir.isEmpty(), "app/seathub could not be located from the test binary");
        QFile file(dir + QStringLiteral("/customer_lists.cpp"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString source = QString::fromUtf8(file.readAll());
        for (const QString& forbidden :
             { QStringLiteral("+="), QStringLiteral("-="), QStringLiteral("accumulate"),
               QStringLiteral("std::reduce"), QStringLiteral("qSum"), QStringLiteral("minutesBilled +"),
               QStringLiteral("amountMinutes +"), QStringLiteral("creditedMinutes +") }) {
            QVERIFY2(!source.contains(forbidden), qPrintable(forbidden));
        }
    }

    // The totals (CUST-14, D-13) -----------------------------------------------------------------------

    void theTotalsAreTheServersOwnNumbersAndAFailureIsNeverAZero()
    {
        SeatHubClient client;
        reachHome(client, 90);
        m_fake->answerUsage(500, errorJson(QStringLiteral("We couldn't total that."),
                                           QStringLiteral("SH-4F7KQ2")));
        client.openProfile();
        QCOMPARE(client.totalsStatus(), QStringLiteral("loading"));
        QTRY_COMPARE_WITH_TIMEOUT(client.totalsStatus(), QStringLiteral("error"), 15000);

        QCOMPARE(client.totalsError(), QStringLiteral("We couldn't total that."));
        QCOMPARE(client.totalsErrorReference(), QStringLiteral("SH-4F7KQ2"));
        QVERIFY2(client.hoursPlayedText().isEmpty() && client.creditLeftText().isEmpty(),
                 "a failed read shows no total, and certainly not zero");

        // A 2xx that is not the totals is the deck's generic sentence and no reference, still no zero.
        m_fake->answerUsage(200, QByteArrayLiteral("{}"));
        client.reloadTotals();
        QTRY_COMPARE_WITH_TIMEOUT(client.totalsError(), SeatHubFailure::generic().error, 15000);
        QCOMPARE(client.totalsStatus(), QStringLiteral("error"));
        QVERIFY(client.totalsErrorReference().isEmpty());
        QVERIFY(client.hoursPlayedText().isEmpty());

        // Zero really played is a real answer, and reads as zero.
        m_fake->answerUsage(200, usageBody(0, 0));
        client.reloadTotals();
        QTRY_COMPARE_WITH_TIMEOUT(client.totalsStatus(), QStringLiteral("ready"), 15000);
        QCOMPARE(client.hoursPlayedText(), QStringLiteral("0 min"));
        QCOMPARE(client.creditLeftText(), QStringLiteral("0 min"));
        QVERIFY(client.totalsError().isEmpty());

        // The lists never noticed any of it.
        QCOMPARE(client.sessionHistory()->status(), QStringLiteral("idle"));
    }

    void aFailedTotalsReadLeavesTheListsAndTheIdentityAlone()
    {
        SeatHubClient client;
        reachHome(client, 90);
        m_fake->answerUsage(500, errorJson(QStringLiteral("We couldn't total that."),
                                           QStringLiteral("SH-4F7KQ2")));
        m_fake->answerList(QStringLiteral("/api/sessions"), QString(), 200,
                           pageBody("sessions", { sessionRow(QStringLiteral("a"), 10) }, QString()));
        client.openProfile();
        client.loadFirstPage(QStringLiteral("sessions"));

        QTRY_COMPARE_WITH_TIMEOUT(client.totalsStatus(), QStringLiteral("error"), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(client.sessionHistory()->status(), QStringLiteral("ready"), 15000);
        QCOMPARE(client.sessionHistory()->count(), 1);
        QCOMPARE(client.accountStatus(), QStringLiteral("ready"));
    }

    // The identity rows (CUST-08) -----------------------------------------------------------------------

    void theIdentityRowsHaveTheirOwnLoadingAndErrorStatesAndAFailedRefreshNeverBlanksThem()
    {
        // An offline restore keeps the customer signed in and reads no account, so the profile has
        // nothing to draw until its own read succeeds.
        SeatHubClient client;
        isolateStore(client);
        client.controlPlane()->setBaseUrl(QStringLiteral("https://control.invalid"));
        armControlPlane(client);
        m_fake->answerMe(0, QByteArray());
        m_fake->answerWallet(200, walletBody(90));
        QVERIFY(client.credentialStore()->storeToken(TokenStore::accessTokenName(),
                                                     QString::fromLatin1(kAccessToken)));
        client.restoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);
        QVERIFY(client.account().isEmpty());

        client.openProfile();
        QCOMPARE(client.accountStatus(), QStringLiteral("loading"));
        QTRY_COMPARE_WITH_TIMEOUT(client.accountStatus(), QStringLiteral("error"), 15000);
        QCOMPARE(client.accountError(), SeatHubFailure::offlineSentence());
        QVERIFY(client.accountErrorReference().isEmpty());

        // Try again reads it again on its own and the rows fill in.
        m_fake->answerMe(200, accountBody());
        client.reloadAccount();
        QTRY_COMPARE_WITH_TIMEOUT(client.accountStatus(), QStringLiteral("ready"), 15000);
        QVERIFY(client.accountError().isEmpty());
        const QVariantMap account = client.account();
        QCOMPARE(account.value(QStringLiteral("username")).toString(), QStringLiteral("lina"));
        QCOMPARE(account.value(QStringLiteral("email")).toString(), QStringLiteral("lina@example.com"));
        QCOMPARE(account.value(QStringLiteral("phone")).toString(), QStringLiteral("+962790000000"));
        QVERIFY(account.value(QStringLiteral("email_verified")).toBool());

        // A later refresh that fails never blanks what was already read.
        const int reads = m_fake->countOfPathEndingWith(QStringLiteral("/api/me"));
        m_fake->answerMe(500, errorJson(QStringLiteral("Try again."), QStringLiteral("SH-4F7KQ2")));
        client.reloadAccount();
        QTRY_COMPARE_WITH_TIMEOUT(m_fake->countOfPathEndingWith(QStringLiteral("/api/me")), reads + 1,
                                  15000);
        QTest::qWait(200);
        QCOMPARE(client.accountStatus(), QStringLiteral("ready"));
        QCOMPARE(client.account().value(QStringLiteral("username")).toString(), QStringLiteral("lina"));
    }

    // A rejected credential anywhere, and sign-out (CUST-08) ---------------------------------------------

    void aRejectedCredentialOnAListClearsItAndLandsOnSignIn()
    {
        SeatHubClient client;
        reachProfile(client);
        m_fake->answerList(QStringLiteral("/api/sessions"), QString(), 401,
                           errorJson(QStringLiteral("Please sign in again."), QStringLiteral("SH-3K2XQ1")));

        client.loadFirstPage(QStringLiteral("sessions"));
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("signed_out"), 15000);

        QVERIFY(!client.signedIn());
        QVERIFY(!client.inProfile());
        QVERIFY2(client.credentialStore()->retrieveToken(TokenStore::accessTokenName()).isEmpty(),
                 "a refused credential is removed from this machine");
        QCOMPARE(client.sessionHistory()->status(), QStringLiteral("idle"));
        QCOMPARE(client.sessionHistory()->count(), 0);
        QCOMPARE(client.totalsStatus(), QStringLiteral("idle"));
        QTRY_VERIFY_WITH_TIMEOUT(!client.controlPlane()->hasAccessToken(), 15000);
    }

    void aRejectedCredentialOnTheTotalsOrTheIdentityDoesTheSame()
    {
        {
            SeatHubClient client;
            reachHome(client, 90);
            m_fake->answerUsage(401, errorJson(QStringLiteral("Please sign in again."),
                                               QStringLiteral("SH-3K2XQ1")));
            client.openProfile();
            QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("signed_out"), 15000);
            QVERIFY(client.credentialStore()->retrieveToken(TokenStore::accessTokenName()).isEmpty());
        }
        m_fake = nullptr;
        {
            SeatHubClient client;
            reachHome(client, 90);
            m_fake->answerMe(401, errorJson(QStringLiteral("Please sign in again."),
                                            QStringLiteral("SH-3K2XQ1")));
            client.openProfile();
            QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("signed_out"), 15000);
            QVERIFY(client.credentialStore()->retrieveToken(TokenStore::accessTokenName()).isEmpty());
        }
    }

    void aFailureOtherThanARejectionNeverSignsAnybodyOut()
    {
        SeatHubClient client;
        reachProfile(client);
        m_fake->answerList(QStringLiteral("/api/sessions"), QString(), 503,
                           errorJson(QStringLiteral("Try again later."), QStringLiteral("SH-4F7KQ2")));
        m_fake->answerList(QStringLiteral("/api/wallet/history"), QString(), 0, QByteArray());
        client.loadFirstPage(QStringLiteral("sessions"));
        client.loadFirstPage(QStringLiteral("credit"));
        QTRY_COMPARE_WITH_TIMEOUT(client.sessionHistory()->status(), QStringLiteral("error"), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(client.creditHistory()->status(), QStringLiteral("error"), 15000);
        QVERIFY(client.signedIn());
        QCOMPARE(client.appState(), QStringLiteral("home"));
        QVERIFY(!client.credentialStore()->retrieveToken(TokenStore::accessTokenName()).isEmpty());
    }

    void signingOutForgetsTheHistoryTheTotalsAndAnyReplyStillOnItsWay()
    {
        SeatHubClient client;
        reachProfile(client);
        m_fake->answerList(QStringLiteral("/api/sessions"), QString(), 200,
                           pageBody("sessions", { sessionRow(QStringLiteral("a"), 10) }, QString()));
        client.loadFirstPage(QStringLiteral("sessions"));
        QTRY_COMPARE_WITH_TIMEOUT(client.sessionHistory()->status(), QStringLiteral("ready"), 15000);
        QCOMPARE(client.sessionHistory()->count(), 1);

        // Another list is still out when the customer signs out: its reply is nobody's to show.
        m_fake->answerList(QStringLiteral("/api/wallet/history"), QString(), 200,
                           pageBody("entries",
                                    { ledgerRowJson(QStringLiteral("l1"), QStringLiteral("topup_credit"), 300) },
                                    QString()));
        m_fake->delayLists(300);
        client.loadFirstPage(QStringLiteral("credit"));
        client.signOut();

        QCOMPARE(client.appState(), QStringLiteral("signed_out"));
        QVERIFY(!client.inProfile());
        QCOMPARE(client.sessionHistory()->status(), QStringLiteral("idle"));
        QCOMPARE(client.sessionHistory()->count(), 0);
        QCOMPARE(client.creditHistory()->status(), QStringLiteral("idle"));
        QCOMPARE(client.totalsStatus(), QStringLiteral("idle"));
        QVERIFY(client.hoursPlayedText().isEmpty());
        QVERIFY(client.creditLeftText().isEmpty());

        QTest::qWait(700);
        QCOMPARE(client.creditHistory()->status(), QStringLiteral("idle"));
        QCOMPARE(client.creditHistory()->count(), 0);
    }

    void aVisitStartsCleanNothingFromTheLastOneIsShownAsCurrent()
    {
        SeatHubClient client;
        reachProfile(client);
        m_fake->answerList(QStringLiteral("/api/sessions"), QString(), 200,
                           pageBody("sessions", { sessionRow(QStringLiteral("a"), 10) }, QString()));
        client.loadFirstPage(QStringLiteral("sessions"));
        QTRY_COMPARE_WITH_TIMEOUT(client.sessionHistory()->status(), QStringLiteral("ready"), 15000);

        client.closeProfile();
        QCOMPARE(client.sessionHistory()->status(), QStringLiteral("idle"));
        QCOMPARE(client.sessionHistory()->count(), 0);

        // The next visit asks for the first page again, and it is the new answer that shows.
        m_fake->answerList(QStringLiteral("/api/sessions"), QString(), 200,
                           pageBody("sessions",
                                    { sessionRow(QStringLiteral("z"), 99), sessionRow(QStringLiteral("a"), 10) },
                                    QString()));
        m_fake->answerUsage(200, usageBody(200, 30));
        client.openProfile();
        client.loadFirstPage(QStringLiteral("sessions"));
        QTRY_COMPARE_WITH_TIMEOUT(client.sessionHistory()->count(), 2, 15000);
        QCOMPARE(cell(client.sessionHistory(), 0, CustomerListModel::AmountRole),
                 QStringLiteral("1 h 39 min"));
        QTRY_COMPARE_WITH_TIMEOUT(client.hoursPlayedText(), QStringLiteral("3 h 20 min"), 15000);
    }

    void theFacadeAndTheListModelsExposeExactlyWhatTheProfileScreenReads()
    {
        // `tst_ui_screens` drives the profile with stand-ins for the facade and the list models. This
        // is the other half: the real ones have every property, role and invokable those stand-ins
        // have, under the same names, so a rename on either side fails a suite instead of a customer.
        SeatHubClient client;

        const QMetaObject* facade = client.metaObject();
        for (const char* name : { "inProfile", "account", "accountStatus", "accountError",
                                  "accountErrorReference", "totalsStatus", "hoursPlayedText",
                                  "creditLeftText", "totalsError", "totalsErrorReference",
                                  "sessionHistory", "creditHistory", "topupHistory" }) {
            QVERIFY2(facade->indexOfProperty(name) >= 0, name);
        }
        for (const char* signature : { "openProfile()", "closeProfile()", "loadFirstPage(QString)",
                                       "loadNextPage(QString)", "reloadList(QString)", "reloadTotals()",
                                       "reloadAccount()", "signOut()", "openTopUp()",
                                       "openWebsite(QString)" }) {
            QVERIFY2(facade->indexOfMethod(signature) >= 0, signature);
        }

        const QList<CustomerListModel*> lists = { client.sessionHistory(), client.creditHistory(),
                                                  client.topupHistory() };
        for (CustomerListModel* list : lists) {
            const QMetaObject* meta = list->metaObject();
            for (const char* name : { "status", "loadingMore", "moreFailed", "hasMore", "errorText",
                                      "errorReference", "count" }) {
                QVERIFY2(meta->indexOfProperty(name) >= 0, name);
            }
            const QHash<int, QByteArray> roles = list->roleNames();
            QCOMPARE(roles.size(), 4);
            QCOMPARE(roles.value(CustomerListModel::WhenRole), QByteArrayLiteral("whenText"));
            QCOMPARE(roles.value(CustomerListModel::KindRole), QByteArrayLiteral("kindText"));
            QCOMPARE(roles.value(CustomerListModel::AmountRole), QByteArrayLiteral("amountText"));
            QCOMPARE(roles.value(CustomerListModel::ToneRole), QByteArrayLiteral("tone"));
        }
    }

    // The words and the dates (D-15) ----------------------------------------------------------------------

    void endReasonShortText_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QString>("text");

        QTest::newRow("CUSTOMER_ENDED") << "CUSTOMER_ENDED" << "You ended it";
        QTest::newRow("BALANCE_EXHAUSTED") << "BALANCE_EXHAUSTED" << "Balance ran out";
        QTest::newRow("HOST_LOST") << "HOST_LOST" << "Lost contact with the rig";
        QTest::newRow("CLIENT_SILENT") << "CLIENT_SILENT" << "Lost contact with your device";
        QTest::newRow("CONNECT_TIMEOUT") << "CONNECT_TIMEOUT" << "Not started in time, not charged";
        QTest::newRow("READINESS_TIMEOUT") << "READINESS_TIMEOUT" << "Rig didn't come back, not charged";
        QTest::newRow("MODE_BOOT_TIMEOUT") << "MODE_BOOT_TIMEOUT" << "Rig didn't come back, not charged";
        QTest::newRow("GRACE_EXPIRED") << "GRACE_EXPIRED" << "Couldn't reconnect";
        QTest::newRow("OWNER_RESERVATION") << "OWNER_RESERVATION" << "Owner reserved the rig";
        QTest::newRow("TEARDOWN_TIMEOUT") << "TEARDOWN_TIMEOUT" << "Closed after a problem";
        QTest::newRow("OPERATOR_FORCED") << "OPERATOR_FORCED" << "Ended by support";
        QTest::newRow("a reason the deck has no form for") << "RECONNECT_LIMIT" << "Ended";
        QTest::newRow("no reason at all") << "" << "Ended";
    }

    void endReasonShortText()
    {
        QFETCH(QString, key);
        QFETCH(QString, text);
        QCOMPARE(::endReasonShortText(key), text);
        // The key itself is never what a customer reads.
        QVERIFY(key.isEmpty() || ::endReasonShortText(key) != key);
    }

    void ledgerKindText_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QString>("text");

        QTest::newRow("topup_credit") << "topup_credit" << "Top-up";
        QTest::newRow("first_bonus") << "first_bonus" << "First top-up bonus";
        QTest::newRow("session_debit") << "session_debit" << "Played";
        QTest::newRow("refund") << "refund" << "Refund";
        QTest::newRow("adjustment") << "adjustment" << "Adjustment by support";
        QTest::newRow("shortfall") << "shortfall" << "Unpaid minutes";
        // The deck has no word for a kind it does not list, and a guess would be an invented string.
        QTest::newRow("a kind the deck does not list") << "chargeback" << "";
    }

    void ledgerKindText()
    {
        QFETCH(QString, key);
        QFETCH(QString, text);
        QCOMPARE(::ledgerKindText(key), text);
    }

    void jordanTimeReadsAnInstantInJordanTimeInEnglishWhateverTheMachineIs_data()
    {
        QTest::addColumn<QString>("instant");
        QTest::addColumn<QString>("text");

        // Jordan has been on UTC+3 all year since 28 October 2022.
        QTest::newRow("a summer evening") << "2026-09-12T18:40:00Z" << "Sat 12 Sep, 21:40";
        QTest::newRow("a winter noon") << "2026-01-15T10:00:00Z" << "Thu 15 Jan, 13:00";
        QTest::newRow("across midnight") << "2026-09-12T21:30:00Z" << "Sun 13 Sep, 00:30";
        QTest::newRow("no leading zero on the day") << "2026-03-05T05:07:00Z" << "Thu 5 Mar, 08:07";
        QTest::newRow("fractional seconds") << "2026-09-12T18:40:00.123456Z" << "Sat 12 Sep, 21:40";
        // The same instant, said with another offset, reads the same.
        QTest::newRow("an offset of +00:00") << "2026-09-12T18:40:00+00:00" << "Sat 12 Sep, 21:40";
        QTest::newRow("already Jordan's own offset") << "2026-09-12T21:40:00+03:00" << "Sat 12 Sep, 21:40";
        QTest::newRow("another zone's offset") << "2026-09-12T14:40:00-04:00" << "Sat 12 Sep, 21:40";
        // A timestamp with no zone at all is read as UTC, which is what every stored instant is.
        QTest::newRow("no zone at all") << "2026-09-12T18:40:00" << "Sat 12 Sep, 21:40";
        QTest::newRow("empty") << "" << "";
        QTest::newRow("not a time") << "yesterday" << "";
    }

    void jordanTimeReadsAnInstantInJordanTimeInEnglishWhateverTheMachineIs()
    {
        QFETCH(QString, instant);
        QFETCH(QString, text);
        QCOMPARE(jordanDateTimeText(instant), text);
    }

    void jordanTimeUsesTheJordanZoneItselfWhereTheMachineHasIt()
    {
        // Not a check of the fallback: on a machine whose zone data has Jordan in it, the answer is the
        // zone's own. A winter before Jordan gave up daylight saving was UTC+2.
        const QTimeZone amman(QByteArrayLiteral("Asia/Amman"));
        if (!amman.isValid()) {
            QSKIP("this machine has no Asia/Amman zone data; the fixed +03:00 fallback answers instead");
        }
        QCOMPARE(jordanDateTimeText(QStringLiteral("2020-01-15T10:00:00Z")),
                 QStringLiteral("Wed 15 Jan, 12:00"));
        QCOMPARE(jordanDateTimeText(QStringLiteral("2020-07-15T10:00:00Z")),
                 QStringLiteral("Wed 15 Jul, 13:00"));
    }

    void theProfilesWebsiteLinkOpensTheOriginItselfAndNothingElse()
    {
        SeatHubClient client;
        reachHome(client, 10);

        QCOMPARE(client.websiteUrl(QStringLiteral("home")), QStringLiteral("https://sevenhills.damra.co"));

        QList<QUrl> opened;
        client.setUrlOpener([&opened](const QUrl& url) {
            opened.append(url);
            return true;
        });
        QVERIFY(client.openWebsite(QStringLiteral("home")));
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened.at(0).toString(), QStringLiteral("https://sevenhills.damra.co"));
        QVERIFY(opened.at(0).path().isEmpty());
        QVERIFY(opened.at(0).query().isEmpty());
        const QString text = opened.at(0).toString();
        QVERIFY2(!text.contains(QStringLiteral("opaque-access-token")), qPrintable(text));
        QVERIFY2(!text.contains(QStringLiteral("lina")), qPrintable(text));
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

    // --- Phase 5 plan 10: the credit and the low-balance warnings, on the liveness tick -------------------

    void theWalletIsReadOnTheLivenessTickAndReachesTheHudWhileTheFacadesThreadIsSuspended()
    {
        // The whole of CUST-15 rests on this: nothing in the client knew the balance during a stream,
        // and the one thread that is stopped for the stream is the facade's. The reads are made and
        // the HUD is fed on the network thread; this test never runs its own event loop, so a design
        // that hopped to the facade's thread to update the HUD would leave the value at the seed.
        SeatHubClient client;
        auto* engine = new FakeEngineSession;
        WalletTicks ticks;
        beginStreaming(client, engine, &ticks, /*home*/ 40, /*wallet*/ 7);
        QVERIFY(!QTest::currentTestFailed());

        // At the first frame: the balance Home showed, and no card - that value can be old.
        QVERIFY(client.hud()->isVisible());
        QVERIFY(client.hud()->creditMinutes() >= 0);

        // The immediate first report reads the wallet; the server's answer replaces the seed and, at
        // seven minutes, brings the ten-minute card up.
        QVERIFY2(waitWithoutTheEventLoop([&]() { return client.hud()->creditMinutes() == 7; }),
                 "the wallet read on the liveness tick never reached the HUD");
        QCOMPARE(int(client.hud()->activeCard()), int(HudOverlay::Card::TenMinutes));
        QCOMPARE(client.hud()->warningsFired(HudOverlay::Card::TenMinutes), 1);

        // The facade's own properties have not moved: they are marshalled to its thread, which has
        // not run. They catch up when it does - and the value they end on is the server's.
        QCOMPARE(client.balanceMinutes(), qint64(40));
        QTRY_COMPARE_WITH_TIMEOUT(client.balanceMinutes(), qint64(7), 15000);
        QCOMPARE(client.balanceText(), QStringLiteral("7 min"));

        endStreaming(client, engine);
    }

    void theWalletIsReadOnTheExistingTickAndNowhereElse()
    {
        SeatHubClient client;
        auto* engine = new FakeEngineSession;
        WalletTicks ticks;
        beginStreaming(client, engine, &ticks, 90, 90);
        QVERIFY(!QTest::currentTestFailed());
        QVERIFY(waitWithoutTheEventLoop([&]() { return ticks.read.load() >= 1; }));

        // D-11: the stage moving to "streaming" inside `beginStreaming` (`handleConnectionStarted`)
        // reports at once, the same way any stage change does, and that report carries no wallet
        // read - so the running totals are captured here, before the two ticks below, rather than
        // assumed to start at zero.
        const int reportsBeforeTicks = m_fake->countOfPathEndingWith(QStringLiteral("/liveness"));
        const int readsBeforeTicks = ticks.read.load() + ticks.failed.load();

        // Two more ticks, and each is one liveness report and one wallet read: the read has no
        // cadence of its own.
        QVERIFY(tickAndWait(client, &ticks));
        QVERIFY(tickAndWait(client, &ticks));
        QVERIFY(waitWithoutTheEventLoop([&]() {
            return m_fake->countOfPathEndingWith(QStringLiteral("/liveness")) >= reportsBeforeTicks + 2;
        }));
        const int reports =
            m_fake->countOfPathEndingWith(QStringLiteral("/liveness")) - reportsBeforeTicks;
        const int reads = ticks.read.load() + ticks.failed.load() - readsBeforeTicks;
        QCOMPARE(reads, reports);
        QCOMPARE(reports, 2);

        // And the source agrees: the reporter still has its two locked constants and no third.
        // (`kIntervalMs` is D-31's 10 seconds; the grace is the server's 30.)
        const QString dir = seathubSourceDir();
        QVERIFY2(!dir.isEmpty(), "app/seathub could not be found from the test binary");
        QFile header(dir + QStringLiteral("/liveness_timer.h"));
        QVERIFY(header.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(header.readAll());
        QCOMPARE(text.count(QStringLiteral("static const int k")), 2);
        QVERIFY(text.contains(QStringLiteral("kIntervalMs = 10000")));
        QFile hud(dir + QStringLiteral("/hud_overlay.cpp"));
        QVERIFY(hud.open(QIODevice::ReadOnly));
        QVERIFY2(!QString::fromUtf8(hud.readAll()).contains(QStringLiteral("SDL_AddTimer(kWallet")),
                 "the HUD must not run a wallet timer of its own");

        endStreaming(client, engine);
    }

    void aFailedWalletReadKeepsTheHudsLastValueAndFiresNothing()
    {
        SeatHubClient client;
        auto* engine = new FakeEngineSession;
        WalletTicks ticks;
        beginStreaming(client, engine, &ticks, 90, 30);
        QVERIFY(!QTest::currentTestFailed());
        QVERIFY(waitWithoutTheEventLoop([&]() { return client.hud()->creditMinutes() == 30; }));

        struct Failure { int status; QByteArray body; const char* what; };
        const Failure failures[] = {
            { 0, QByteArray(), "the control plane is unreachable" },
            { 500, errorJson(QStringLiteral("Something went wrong."), QStringLiteral("SH-9K2XQ1")),
              "a server error" },
            { 200, QByteArrayLiteral("{}"), "an answer with no balance in it" },
            { 401, refusedBody(), "a rejected credential" },
        };
        for (const Failure& failure : failures) {
            m_fake->answerWallet(failure.status, failure.body);
            const int failedBefore = ticks.failed.load();
            QVERIFY2(tickAndWait(client, &ticks), failure.what);
            QCOMPARE(ticks.failed.load(), failedBefore + 1);

            // Nothing changed: the last value stays, no card fired, and it is not zero.
            QCOMPARE(client.hud()->creditMinutes(), qint64(30));
            QCOMPARE(int(client.hud()->activeCard()), int(HudOverlay::Card::None));
            QCOMPARE(client.hud()->warningsFired(HudOverlay::Card::TenMinutes), 0);
            QCOMPARE(client.hud()->warningsFired(HudOverlay::Card::TwoMinutes), 0);
        }

        // Not even a rejected credential ends the stream or signs anybody out from a wallet read.
        QCOMPARE(client.appState(), QStringLiteral("streaming"));
        QVERIFY(client.signedIn());

        // A later read that succeeds is taken up as if nothing had happened.
        m_fake->answerWallet(200, walletBody(9));
        QVERIFY(tickAndWait(client, &ticks));
        QCOMPARE(client.hud()->creditMinutes(), qint64(9));
        QCOMPARE(int(client.hud()->activeCard()), int(HudOverlay::Card::TenMinutes));

        endStreaming(client, engine);
    }

    void eachThresholdFiresExactlyOnceAcrossASession()
    {
        SeatHubClient client;
        auto* engine = new FakeEngineSession;
        WalletTicks ticks;
        beginStreaming(client, engine, &ticks, 90, 90);
        QVERIFY(!QTest::currentTestFailed());
        QVERIFY(waitWithoutTheEventLoop([&]() { return client.hud()->creditMinutes() == 90; }));

        // One minute at a time, past both thresholds and to zero, and a read repeated at each.
        for (int minutes = 15; minutes >= 0; --minutes) {
            m_fake->answerWallet(200, walletBody(minutes));
            QVERIFY(tickAndWait(client, &ticks));
            QVERIFY(tickAndWait(client, &ticks));   // the same balance again changes nothing
            QCOMPARE(client.hud()->creditMinutes(), qint64(minutes));

            const bool tenFired = minutes <= 10;
            const bool twoFired = minutes <= 2;
            QCOMPARE(client.hud()->warningsFired(HudOverlay::Card::TenMinutes), tenFired ? 1 : 0);
            QCOMPARE(client.hud()->warningsFired(HudOverlay::Card::TwoMinutes), twoFired ? 1 : 0);
            // Ten minutes: the ten-minute card, until its lifetime; two minutes: the two-minute
            // one, which replaced it and is still there at zero.
            if (minutes <= 2) {
                QCOMPARE(int(client.hud()->activeCard()), int(HudOverlay::Card::TwoMinutes));
            }
        }
        QCOMPARE(client.hud()->warningsFired(HudOverlay::Card::TenMinutes), 1);
        QCOMPARE(client.hud()->warningsFired(HudOverlay::Card::TwoMinutes), 1);

        // A top-up mid-stream and back down: the thresholds are spent for this session.
        m_fake->answerWallet(200, walletBody(60));
        QVERIFY(tickAndWait(client, &ticks));
        m_fake->answerWallet(200, walletBody(8));
        QVERIFY(tickAndWait(client, &ticks));
        QCOMPARE(client.hud()->warningsFired(HudOverlay::Card::TenMinutes), 1);
        QCOMPARE(client.hud()->warningsFired(HudOverlay::Card::TwoMinutes), 1);

        endStreaming(client, engine);
    }

    void aSessionThatBeginsUnderTwoMinutesFiresOnlyTheLowerWarning()
    {
        SeatHubClient client;
        auto* engine = new FakeEngineSession;
        WalletTicks ticks;
        beginStreaming(client, engine, &ticks, 90, 1);
        QVERIFY(!QTest::currentTestFailed());
        QVERIFY(waitWithoutTheEventLoop([&]() { return client.hud()->creditMinutes() == 1; }));

        QCOMPARE(client.hud()->warningsFired(HudOverlay::Card::TwoMinutes), 1);
        QCOMPARE(client.hud()->warningsFired(HudOverlay::Card::TenMinutes), 0);
        QCOMPARE(int(client.hud()->activeCard()), int(HudOverlay::Card::TwoMinutes));

        endStreaming(client, engine);
    }

    void theFirstReadFailingLeavesTheSeedOnTheHudAndFiresNothing()
    {
        // Home said one minute a moment ago; the wallet cannot be reached now. A warning built on a
        // balance that may be stale would spend a threshold the real one has not reached, so the seed
        // is shown and nothing fires until a read has actually answered.
        SeatHubClient client;
        auto* engine = new FakeEngineSession;
        WalletTicks ticks;
        reachHome(client, 1);
        QVERIFY(!QTest::currentTestFailed());
        client.session()->attachSession(engine);
        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));
        m_fake->answerWallet(0, QByteArray());
        connect(client.liveness(), &LivenessTimer::walletReadFailed, &client,
                [&ticks]() { ticks.failed.fetch_add(1); }, Qt::DirectConnection);

        client.beginSession(QStringLiteral("s-live"));
        emit engine->connectionStarted();
        QVERIFY(waitWithoutTheEventLoop([&]() { return ticks.failed.load() >= 1; }));

        QCOMPARE(client.hud()->creditMinutes(), qint64(1));
        QCOMPARE(int(client.hud()->activeCard()), int(HudOverlay::Card::None));
        QCOMPARE(client.hud()->warningsFired(HudOverlay::Card::TwoMinutes), 0);

        endStreaming(client, engine);
    }

    void theRemainingCreditIsNeverDerivedFromTheLeasesHorizon()
    {
        // The horizon (`authorized_through`) is advisory: the facade arms its stop from it (D-33) and
        // nothing else reads it. The HUD and the liveness reporter's code, which is where the credit
        // comes from, name it nowhere. (The reporter's header explains the horizon in a comment, as the
        // backstop this report is not, so it is the code that is checked, not that page.)
        const QString dir = seathubSourceDir();
        QVERIFY2(!dir.isEmpty(), "app/seathub could not be found from the test binary");
        for (const char* name : {"hud_overlay.cpp", "hud_overlay.h", "liveness_timer.cpp"}) {
            QFile file(dir + QLatin1Char('/') + QLatin1String(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QString text = QString::fromUtf8(file.readAll());
            QVERIFY2(!text.contains(QStringLiteral("authorized_through"))
                         && !text.contains(QStringLiteral("authorizedThrough")),
                     name);
        }
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
