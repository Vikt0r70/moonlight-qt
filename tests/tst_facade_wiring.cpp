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
#include <limits>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScopedPointer>
#include <QSemaphore>
#include <QSignalSpy>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimeZone>
#include <QTimer>
#include <QUrlQuery>
#include <QWindow>

#include <atomic>
#include <functional>
#include <thread>

#include "seathub/control_plane_client.h"
#include "seathub/countries.h"
#include "seathub/duration_text.h"
#include "seathub/engine_session.h"
#include "seathub/evidence_journal.h"
#include "seathub/jordan_time.h"
#include "seathub/moonlight_engine_session.h"
#include "seathub/pairing_handshake.h"
#include "seathub/port_test_relay.h"
#include "seathub/quality_outbox.h"
#include "seathub/reporting_consent.h"
#include "seathub/seathub_client.h"
#include "seathub/session_lifecycle.h"
#include "seathub/teardown_controller.h"
#include "seathub/telemetry.h"
#include "seathub/token_store.h"
#include "seathub/web_origin.h"
#include "settings/streamingpreferences.h"

// ---------------------------------------------------------------- the two stitched seams -------
//
// These are the definitions of the two translation units this binary deliberately does not link.
// Both are unreachable-by-construction in this test (no host is ever paired, no engine is ever
// built), and both report the fail-closed answer their production counterparts document for
// exactly this situation.

// 06.6-18 (T-06.6-52): `create()` always returns null below, so a real engine can never be
// observed as attached in this binary - but whether `handleHostResolved()` even REACHED `create()`
// is exactly what a stale, session-mismatched host must never do. This counts calls so a test can
// tell "dropped before create()" from "reached create() and failed closed the ordinary way" - the
// two things `handleHostResolved()`'s session-id check must tell apart.
int g_engineCreateCalls = 0;
EngineLaunchReason g_engineCreateReason = EngineLaunchReason::CreateFailed;
std::function<void()> g_engineFactoryHook;

MoonlightEngineSession* MoonlightEngineSession::create(const PairedHostPtr&, QObject* parent,
                                                       EngineLaunchReason* reason)
{
    Q_UNUSED(parent);
    ++g_engineCreateCalls;
    if (reason != nullptr) {
        *reason = g_engineCreateReason;
    }
    if (g_engineFactoryHook) {
        g_engineFactoryHook();
    }
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

// Plan 14 (D-09/D-13): the same unreachable-by-construction reasoning as `setDebugLineFilter()`
// above - `create()` always returns null, so `handleHostResolved()` never reaches a real
// `MoonlightEngineSession` to call `setTextRasterizer()` on. Defined only so this translation
// unit links without pulling in the real engine (Rule 3, `<shared_tree_rules>`: not in Plan 14's
// own `files_modified`, added the same way 06.3-15 and 06.3-30 added their own link-only stubs
// here - see those plans' SUMMARYs).
void MoonlightEngineSession::setTextRasterizer(Overlay::OverlayManager::TextRasterizer, void*)
{
}

// Plan 09 correction: the facade's own recovered-pairing record is produced through the REAL
// production route (the seam's clear-and-retry, the controller, then the facade), and the only
// part of that route a test binary cannot run is upstream's handshake against a live Sunshine.
// Armed by `aRecoveredPairingEmitsExactlyOneInfoPlayStepWithTheFrozenRecord` alone: every other
// test in this suite keeps the original always-fail answer (and, because no other test grants a
// pairing target, none of them ever reaches this function at all).
bool g_recoveredHandshakeArmed = false;
bool g_noAppHandshakeArmed = false;
bool g_failedRetryHandshakeArmed = false;
// T093: a first attempt that fails with the conflict tag, then a retry that blocks (and says so) until
// the test releases it. The seam has published the first failure by the time the retry is entered.
bool g_blockedRetryHandshakeArmed = false;
QSemaphore g_blockedRetryEntered;
QSemaphore g_blockedRetryGate;
int g_handshakeCalls = 0;
// ADR-0074 W03: how many times the seam fell back to (or started with) the normal handshake, in any
// mode. `g_handshakeCalls` above counts only the armed retry scenarios.
int g_plainHandshakeRuns = 0;

PairingHandshakeResult runUpstreamPairingHandshake(const PairingTarget&)
{
    ++g_plainHandshakeRuns;
    if (g_noAppHandshakeArmed) {
        PairingHandshakeResult paired;
        paired.ok = true;
        paired.clientIdentity = QStringLiteral("no-app-identity");
        paired.host = std::make_shared<PairedHost>();
        return paired;
    }
    if (g_blockedRetryHandshakeArmed) {
        ++g_handshakeCalls;
        if (g_handshakeCalls == 1) {
            PairingHandshakeResult conflict;
            conflict.attemptStep = QStringLiteral("pair_server_info");
            conflict.stepClass = QStringLiteral("http_4xx");
            conflict.pairingConflict = true;
            conflict.engineError = QStringLiteral("fixture conflict before blocked retry");
            return conflict;
        }
        g_blockedRetryEntered.release();
        g_blockedRetryGate.tryAcquire(1, 30000);
        // A late, proper worker result: after a cancel it must reach nobody.
        PairingHandshakeResult late;
        late.ok = true;
        late.clientIdentity = QStringLiteral("cancelled-late-identity");
        late.host = std::make_shared<PairedHost>();
        return late;
    }
    if (g_failedRetryHandshakeArmed) {
        ++g_handshakeCalls;
        if (g_handshakeCalls == 1) {
            PairingHandshakeResult conflict;
            conflict.attemptStep = QStringLiteral("pair_server_info");
            conflict.stepClass = QStringLiteral("http_4xx");
            conflict.pairingConflict = true;
            conflict.engineError = QStringLiteral("fixture conflict before retry");
            return conflict;
        }
        PairingHandshakeResult rejected;
        rejected.attemptStep = QStringLiteral("pair_handshake");
        rejected.stepClass = QStringLiteral("pin_rejected");
        rejected.engineError = QStringLiteral("fixture retry rejection text");
        return rejected;
    }
    if (g_recoveredHandshakeArmed) {
        ++g_handshakeCalls;
        if (g_handshakeCalls == 1) {
            // Exactly what production builds for the rig's HTTP 409 on getservercert
            // (`pairing_handshake.cpp`): the conflict tag is what makes the seam clear the rig's
            // pending session and retry once. Its own step/class are the mapper's - the recovered
            // record the ADR freezes is the seam's to produce.
            PairingHandshakeResult conflict;
            conflict.attemptStep = QStringLiteral("pair_server_info");
            conflict.stepClass = QStringLiteral("http_4xx");
            conflict.pairingConflict = true;
            conflict.engineError = QStringLiteral(
                "the rig did not answer the server-info request (HTTP 409)");
            return conflict;
        }
        PairingHandshakeResult paired;
        paired.ok = true;
        paired.clientIdentity = QStringLiteral("recovered-identity");
        paired.host = std::make_shared<PairedHost>();
        return paired;
    }

    PairingHandshakeResult result;
    result.ok = false;
    result.engineError = QStringLiteral("no Sunshine host exists in this test environment");
    return result;
}

// ADR-0074 W03 / X02: the other two entry points of `pairing_handshake.cpp`, substituted the same
// way. The warm-return check answers "paired" only when a test arms it, and counts every call; the
// client certificate fingerprint is empty (so a Play body stays `{}`) unless a test sets one.
bool g_warmCheckArmed = false;
int g_warmChecks = 0;
QString g_ownCertFingerprint;

PairingHandshakeResult runUpstreamWarmReturnCheck(const PairingTarget&, const QByteArray&)
{
    ++g_warmChecks;
    PairingHandshakeResult result;
    if (g_warmCheckArmed) {
        result.ok = true;
        result.clientIdentity = QStringLiteral("warm-identity");
        result.host = std::make_shared<PairedHost>();
    }
    return result;
}

QString ownClientCertificateFingerprint()
{
    return g_ownCertFingerprint;
}

namespace {

const char* kAccessToken = "opaque-access-token";
const char* kPlaintextMarker = "sb_rt_PLAINTEXT-MARKER-4F7K";

/// Plan 09 correction: stops the process-wide `LogShipper` on EVERY exit path of the test that
/// starts it (a `QVERIFY` failure returns early), so its worker - and the lambda it captured
/// from that test's locals - never outlives the test. Declared last in the test so it runs
/// first, before the capture locals it must not outlive are destroyed.
struct StopShipperOnScopeExit
{
    ~StopShipperOnScopeExit() { LogShipper::instance().stop(); }
};

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

    // What `GET /api/me`, `GET /api/wallet` and `POST /api/auth/logout` answer. Status 0 is a
    // transport failure.
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
    /// Answers for the next `POST /api/sessions` requests, one each and in the order queued. Once they
    /// are used up `answerPlay()`'s answer applies again.
    void queuePlay(int status, const QByteArray& body)
    {
        QMutexLocker lock(&m_mutex);
        m_playQueue.append(qMakePair(status, body));
    }
    /// The `Idempotency-Key` of every `POST /api/sessions` so far, in order.
    QList<QByteArray> playIdempotencyKeys() const
    {
        QMutexLocker lock(&m_mutex);
        return m_playKeys;
    }
    /// What `POST /api/auth/login` answers (status 0 is a transport failure).
    void answerLogin(int status, const QByteArray& body)
    {
        QMutexLocker lock(&m_mutex);
        m_loginStatus = status;
        m_loginBody = body;
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

    /// What `POST /api/sessions/{id}/end` answers. The default (never called) is 200
    /// `{"status":true}` - CR-02's own tests override it to a transport failure.
    void answerEnd(int status, const QByteArray& body = QByteArrayLiteral("{\"status\":true}"))
    {
        QMutexLocker lock(&m_mutex);
        m_endStatus = status;
        m_endBody = body;
    }

    /// V-OUTBOX ordering probe: run as the next `/end` request arrives, before any answer, so a test
    /// can prove the durable record already existed on disk before the first POST was sent.
    /// Installing the probe resets the count, so `/end` traffic before it does not consume it.
    void setEndProbe(std::function<bool()> probe)
    {
        QMutexLocker lock(&m_mutex);
        m_endProbe = std::move(probe);
        m_endRequests = 0;
        m_endOutboxExistedAtFirstEnd = false;
    }

    bool endOutboxExistedAtFirstEnd() const
    {
        QMutexLocker lock(&m_mutex);
        return m_endOutboxExistedAtFirstEnd;
    }

    /// What `POST /api/sessions/{id}/quality` answers. The default (never called) is 200
    /// `{"status":true}`.
    void answerQuality(int status, const QByteArray& body = QByteArrayLiteral("{\"status\":true}"))
    {
        QMutexLocker lock(&m_mutex);
        m_qualityStatus = status;
        m_qualityBody = body;
    }
    /// What `POST /api/sessions/{id}/evidence` (ADR-0105) answers. The default (never called) is 200
    /// with the documented stored/duplicates/rejected counts.
    void answerEvidence(int status,
                        const QByteArray& body = QByteArrayLiteral(
                            "{\"stored\":1,\"duplicates\":0,\"rejected\":0}"))
    {
        QMutexLocker lock(&m_mutex);
        m_evidenceStatus = status;
        m_evidenceBody = body;
    }
    /// CR-03: every `/quality` reply arrives this many milliseconds after it was asked for, so a
    /// test can sign out (and a different account sign in) while an earlier account's send is
    /// still in flight.
    void delayQuality(int milliseconds)
    {
        QMutexLocker lock(&m_mutex);
        m_qualityDelay = milliseconds;
    }

    /// What `GET /api/sessions/{id}` answers: the session in `state`, under `id`, with an end reason
    /// and a billed-minutes count when given. The default is a terminal session under another id,
    /// which teardown's verification wants and the connecting stages ignore. `endBlame` and
    /// `endReference` (06.1's ADR item 4, J-22) ride the same optional-when-empty rule as
    /// `endReason` - a session with no blame at all sends neither.
    void answerSession(const QString& id, const QString& state, const QString& endReason = QString(),
                       int minutesBilled = 0, const QString& endBlame = QString(),
                       const QString& endReference = QString())
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
        if (!endBlame.isEmpty()) {
            object.insert(QStringLiteral("end_blame"), endBlame);
        }
        if (!endReference.isEmpty()) {
            object.insert(QStringLiteral("end_reference"), endReference);
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
    /// What `GET /api/account-state` answers (06.4/ADR-0067, the D-06 fallback read).
    void answerAccountState(int status, const QByteArray& body)
    {
        QMutexLocker lock(&m_mutex);
        m_accountStateStatus = status;
        m_accountStateBody = body;
    }
    /// The last `X-Stream-Fallback` header a request for `path` carried, or empty for a request
    /// that carried none (an ordinary, non-fallback read).
    QByteArray streamFallbackHeaderFor(const QString& path) const
    {
        QMutexLocker lock(&m_mutex);
        return m_streamFallbackHeaders.value(path);
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

    /// Every body a request for `path` has carried, in order. `bodyFor()` keeps only the last one
    /// - 06.1-19/J-07's own reconnect tests need the whole sequence of liveness reports a drop and
    /// a return post, not just the most recent.
    QList<QByteArray> bodiesFor(const QString& path) const
    {
        QMutexLocker lock(&m_mutex);
        return m_bodyHistory.value(path);
    }

    /// The `Authorization` header the last request for `path` carried.
    QByteArray authorizationFor(const QString& path) const
    {
        QMutexLocker lock(&m_mutex);
        return m_auth.value(path);
    }

    /// D-27: the `traceparent` header of every request, in order (empty when the request carried
    /// none) - so a test can tell "no trace id was set" from "the trace id changed between two
    /// Plays" without needing the span, which is fresh per request by design.
    QList<QByteArray> traceparents() const
    {
        QMutexLocker lock(&m_mutex);
        return m_traceparents;
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
        int meStatus, walletStatus, logoutStatus, loginStatus, playStatus;
        int pairingStatus, endStatus, qualityStatus, qualityDelay, evidenceStatus;
        bool pairingCustom;
        QByteArray meBody, walletBody, loginBody, playBody, pairingBody, sessionBody, endBody,
            qualityBody, evidenceBody;
        {
            QMutexLocker lock(&m_mutex);
            m_paths.append(path);
            m_queries[path].append(request.url().query(QUrl::FullyEncoded));
            m_auth.insert(path, request.rawHeader("Authorization"));
            m_traceparents.append(request.rawHeader("traceparent"));
            m_streamFallbackHeaders.insert(path, request.rawHeader("X-Stream-Fallback"));
            if (outgoing) {
                const QByteArray body = outgoing->peek(outgoing->size());
                m_bodies.insert(path, body);
                m_bodyHistory[path].append(body);
            }
            loginStatus = m_loginStatus;
            loginBody = m_loginBody;
            playStatus = m_playStatus;
            playBody = m_playBody;
            if (path == QLatin1String("/api/sessions") && !isGet) {
                m_playKeys.append(request.rawHeader("Idempotency-Key"));
                if (!m_playQueue.isEmpty()) {
                    const QPair<int, QByteArray> queued = m_playQueue.takeFirst();
                    playStatus = queued.first;
                    playBody = queued.second;
                }
            }
            meStatus = m_meStatus;
            meBody = m_meBody;
            walletStatus = m_walletStatus;
            walletBody = m_walletBody;
            logoutStatus = m_logoutStatus;
            pairingStatus = m_pairingStatus;
            pairingBody = m_pairingBody;
            pairingCustom = m_pairingCustom;
            sessionBody = m_sessionBody;
            endStatus = m_endStatus;
            endBody = m_endBody;
            qualityStatus = m_qualityStatus;
            qualityBody = m_qualityBody;
            qualityDelay = m_qualityDelay;
            evidenceStatus = m_evidenceStatus;
            evidenceBody = m_evidenceBody;
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
        if (isGet && path == QLatin1String("/api/account-state")) {
            int accountStateStatus;
            QByteArray accountStateBody;
            {
                QMutexLocker lock(&m_mutex);
                accountStateStatus = m_accountStateStatus;
                accountStateBody = m_accountStateBody;
            }
            return new FakeReply(accountStateStatus, accountStateBody, this);
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

        if (path.endsWith(QLatin1String("/end"))) {
            {
                QMutexLocker lock(&m_mutex);
                if (++m_endRequests == 1 && m_endProbe) {
                    m_endOutboxExistedAtFirstEnd = m_endProbe();
                }
            }
            return new FakeReply(endStatus, endBody, this);
        }
        if (path.endsWith(QLatin1String("/evidence"))) {
            return new FakeReply(evidenceStatus, evidenceBody, this);
        }
        if (path.endsWith(QLatin1String("/quality"))) {
            return new FakeReply(qualityStatus, qualityBody, this, qualityDelay);
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
    int m_accountStateStatus = 200;
    QByteArray m_accountStateBody = QByteArrayLiteral(
        "{\"topic\":\"account:acc-1\",\"at\":\"2026-09-27T00:00:00Z\","
        "\"account\":{\"wallet\":{\"balance_minutes\":0},\"live_session_id\":\"\","
        "\"open_topup_notice\":null,\"signup_stage\":\"complete\"}}");
    QHash<QString, QByteArray> m_streamFallbackHeaders;
    QHash<QString, QByteArray> m_auth;
    QList<QByteArray> m_traceparents;
    QHash<QString, QByteArray> m_bodies;
    QHash<QString, QList<QByteArray>> m_bodyHistory;
    int m_loginStatus = 200;
    QByteArray m_loginBody;
    int m_playStatus = 201;
    QByteArray m_playBody;
    QList<QPair<int, QByteArray>> m_playQueue;
    QList<QByteArray> m_playKeys;
    int m_meStatus = 200;
    QByteArray m_meBody;
    int m_walletStatus = 200;
    QByteArray m_walletBody;
    int m_logoutStatus = 204;
    int m_pairingStatus = 404;
    QByteArray m_pairingBody;
    bool m_pairingCustom = false;
    QByteArray m_sessionBody;
    int m_endStatus = 200;
    QByteArray m_endBody = QByteArrayLiteral("{\"status\":true}");
    std::function<bool()> m_endProbe;
    bool m_endOutboxExistedAtFirstEnd = false;
    int m_endRequests = 0;
    int m_evidenceStatus = 200;
    QByteArray m_evidenceBody = QByteArrayLiteral("{\"stored\":1,\"duplicates\":0,\"rejected\":0}");
    int m_qualityStatus = 200;
    QByteArray m_qualityBody = QByteArrayLiteral("{\"status\":true}");
    int m_qualityDelay = 0;
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
    std::atomic<int> interrupts{0}; // atomic: read from test threads
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

/// Fires the real engine's own end-of-stream "Global video stats" block through the real `LogTee`
/// singleton (CR-02, WR-04, WR-06 facade tests below), exactly the two `SDL_LogInfo()` calls
/// `FFmpegVideoDecoder::logVideoStats()` makes (`app/streaming/video/ffmpeg.cpp:858-870`,
/// read-only) - the dashes line is in the second call's OWN format string, not the body, matching
/// production. `StatsWatcher` is a process-wide `LogTee` sink (installed by whichever
/// `SeatHubClient` was constructed first in this binary), so this reaches every attached facade's
/// own `handleVideoStatsParsed()` synchronously, on this (the test) thread, exactly as a
/// same-thread dispatch does for a real decoder destroyed on the SDL/main thread.
void emitStatsBlock(double renderedFps)
{
    // IN-08 (code review 06.3-REVIEW-fork.md, second review pass): the "Host processing
    // latency..." line a Sunshine host's own encode latency adds
    // (`app/streaming/video/ffmpeg.cpp:811-819`, gated on `framesWithHostProcessingLatency > 0`)
    // between `Rendering frame rate:` and the drop lines, matching production - see
    // `tst_stream_stats.cpp`'s own `productionStatsBlock()` comment for the same fix.
    const QByteArray body = QStringLiteral(
                                 "Incoming frame rate from network: 60.00 FPS\n"
                                 "Decoding frame rate: 59.98 FPS\n"
                                 "Rendering frame rate: %1 FPS\n"
                                 "Host processing latency min/max/average: 0.8/3.4/1.6 ms\n"
                                 "Frames dropped by your network connection: 0.42%\n"
                                 "Frames dropped due to network jitter: 0.10%\n"
                                 "Average network latency: 23 ms (variance: 4 ms)\n"
                                 "Average decoding time: 3.21 ms\n"
                                 "Average frame queue delay: 1.05 ms\n"
                                 "Average rendering time (including monitor V-sync latency): 2.77 ms\n")
                                 .arg(renderedFps, 0, 'f', 2)
                                 .toUtf8();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Global video stats");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "----------------------------------------------------------\n%s", body.constData());
}

/// T073 (ADR-0101 amendment 2026-10-19): the two start lines the real engine logs once per Play,
/// byte for byte as `app/streaming/session.cpp:339` (`Session::drSetup`) and `:664-666`
/// (`Session::initialize`) print them, read-only, through the real `LogTee` singleton.
void emitVideoStreamLine(int width, int height, int frameRate, unsigned videoFormat)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Video stream is %dx%dx%d (format 0x%x)", width,
                height, frameRate, videoFormat);
}

void emitVideoBitrateLine(int kbps)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Video bitrate: %d kbps", kbps);
}

/// Fires upstream's own termination log line through the real `LogTee` singleton (06.1-19/J-07,
/// `engine_termination.h`), exactly the one `SDL_LogError()` call `Session::clConnectionTerminated`
/// makes - the one route to the engine's own code, unconditionally, on every ending including `0`
/// (`ML_ERROR_GRACEFUL_TERMINATION`). Reaches every attached facade's own termination sink
/// synchronously, on this (the test) thread, the same way `emitStatsBlock()` above does for the
/// stats sink.
void emitConnectionTerminated(int code)
{
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Connection terminated: %d", code);
}

/// Collects every line the process-wide `LogShipper` hands out for the lifetime of one test, and
/// stops the shipper before the collected list goes away (the worker must not outlive it).
class PlayCapture
{
public:
    PlayCapture()
    {
        LogShipper::instance().setSpoolDirectoryForTests(m_spool.path());
        LogShipper::instance().start([this](const ShippedLine& line) {
            QMutexLocker lock(&m_mutex);
            m_records.append(line);
            return true;
        });
        LogShipper::instance().setCanShip(true);
    }
    ~PlayCapture() { LogShipper::instance().stop(); }

    QList<ShippedLine> named(const QString& body) const
    {
        QMutexLocker lock(&m_mutex);
        QList<ShippedLine> result;
        for (const ShippedLine& line : m_records) {
            if (line.body == body) {
                result.append(line);
            }
        }
        return result;
    }

private:
    QTemporaryDir m_spool;
    mutable QMutex m_mutex;
    QList<ShippedLine> m_records;
};

/// T063: collects every line the `seathub.*` logging categories write while it lives, from any
/// thread (the outbox lines are written on the network thread and the facade's thread), and puts the
/// previous message handler back. It records the message text only - the same string a log sink sees.
class LogLineCapture
{
public:
    struct Line
    {
        QString category;
        QString text;
    };

    LogLineCapture()
    {
        State& s = state();
        QMutexLocker lock(&s.mutex);
        s.active = this;
        s.previous = qInstallMessageHandler(&LogLineCapture::handle);
    }
    ~LogLineCapture()
    {
        State& s = state();
        QMutexLocker lock(&s.mutex);
        qInstallMessageHandler(s.previous);
        s.active = nullptr;
    }

    QStringList texts(const QString& category) const
    {
        QMutexLocker lock(&m_mutex);
        QStringList found;
        for (const Line& line : m_lines) {
            if (line.category == category) found.append(line.text);
        }
        return found;
    }
    QList<Line> all() const
    {
        QMutexLocker lock(&m_mutex);
        return m_lines;
    }

private:
    struct State
    {
        QMutex mutex;
        LogLineCapture* active = nullptr;
        QtMessageHandler previous = nullptr;
    };
    static State& state()
    {
        static State s;
        return s;
    }
    static void handle(QtMsgType type, const QMessageLogContext& context, const QString& message)
    {
        State& s = state();
        QMutexLocker lock(&s.mutex);
        if (s.active && context.category) {
            QMutexLocker mine(&s.active->m_mutex);
            s.active->m_lines.append({QString::fromLatin1(context.category), message});
        }
        if (s.previous) s.previous(type, context, message);
    }

    mutable QMutex m_mutex;
    QList<Line> m_lines;
};

} // namespace

class TstFacadeWiring : public QObject
{
    Q_OBJECT

private:
    QScopedPointer<QTemporaryDir> m_dir;
    QScopedPointer<QTemporaryDir> m_backupRoot;
    FakeControlPlane* m_fake = nullptr;
    /// T063: the status `/evidence` answers once the fake exists (0 = the fake's own default). The
    /// fake is only created inside `reachHome()`, so a test that must keep its capsules on disk
    /// states the answer here first.
    int m_evidenceStatusAtArm = 0;

    /// The control-plane client only lives on the network thread the facade moves it to, so the
    /// fake answers from there. A blocking invocation keeps the main thread from racing it.
    void armControlPlane(SeatHubClient& client)
    {
        ControlPlaneClient* plane = client.controlPlane();
        if (plane->thread() == QThread::currentThread()) {
            m_fake = new FakeControlPlane(plane);
            plane->setNetworkAccessManager(m_fake);
            if (m_evidenceStatusAtArm != 0) m_fake->answerEvidence(m_evidenceStatusAtArm);
            return;
        }
        QMetaObject::invokeMethod(
            plane,
            [this, plane]() {
                m_fake = new FakeControlPlane(plane);
                plane->setNetworkAccessManager(m_fake);
                if (m_evidenceStatusAtArm != 0) m_fake->answerEvidence(m_evidenceStatusAtArm);
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
        client.setEndOutboxDirectory(m_dir->path() + QStringLiteral("/end-outbox"));
        client.setEvidenceJournalDirectory(m_dir->path() + QStringLiteral("/evidence-journal"));
    }

    /// Every evidence entry (ADR-0105) on disk for this test, across every capsule, in capsule order.
    QList<QJsonObject> evidenceEntries(const QString& kind = QString()) const
    {
        QList<QJsonObject> found;
        const QDir dir(m_dir->path() + QStringLiteral("/evidence-journal"));
        const QStringList names = dir.entryList({QStringLiteral("evj-*.json")}, QDir::Files, QDir::Name);
        for (const QString& name : names) {
            QFile file(dir.filePath(name));
            if (!file.open(QIODevice::ReadOnly)) continue;
            const QJsonArray entries = QJsonDocument::fromJson(file.readAll()).object()
                                           .value(QStringLiteral("entries")).toArray();
            for (const QJsonValue& value : entries) {
                const QJsonObject entry = value.toObject();
                if (kind.isEmpty() || entry.value(QStringLiteral("kind")).toString() == kind) {
                    found.append(entry);
                }
            }
        }
        return found;
    }

    /// Every entry of one kind in the capsule(s) on disk for `sessionId` (ADR-0105), in capsule order.
    QList<QJsonObject> evidenceEntriesFor(const QString& sessionId, const QString& kind) const
    {
        QList<QJsonObject> found;
        const QDir dir(m_dir->path() + QStringLiteral("/evidence-journal"));
        for (const QString& name : dir.entryList({QStringLiteral("evj-*.json")}, QDir::Files, QDir::Name)) {
            QFile file(dir.filePath(name));
            if (!file.open(QIODevice::ReadOnly)) continue;
            const QJsonObject capsule = QJsonDocument::fromJson(file.readAll()).object();
            if (capsule.value(QStringLiteral("session_id")).toString() != sessionId) continue;
            for (const QJsonValue& value : capsule.value(QStringLiteral("entries")).toArray()) {
                const QJsonObject entry = value.toObject();
                if (entry.value(QStringLiteral("kind")).toString() == kind) found.append(entry);
            }
        }
        return found;
    }

    /// Feeds one real stats sample through the compositor's stats tap, the same route the engine's
    /// video-stats overlay takes, so the facade's sampler receives it from this (the render) thread.
    static void feedOneSample(SeatHubClient& client)
    {
        SDL_Surface* surface = OsdCompositor::rasterize(
            Overlay::OverlayDebug,
            "Rendering frame rate: 59.00 FPS\nAverage network latency: 23 ms (variance: 1 ms)",
            true, SDL_Color{255, 255, 255, 255}, &client.hud()->compositor());
        if (surface) SDL_FreeSurface(surface);
    }

    /// T063: the two accounts these tests sign in as (the first is what `accountBody()` answers).
    static QString accountAId() { return QStringLiteral("6f1c6f5e-3a1e-4b1e-9f2e-0f1a2b3c4d5e"); }
    static QString accountBId() { return QStringLiteral("77777777-7777-7777-7777-777777777777"); }

    /// Leaves one closed evidence capsule on disk for `accountId`, the way an earlier run that died
    /// before the server had it would. It is written by a second journal on the same folder, so the
    /// file is the product's own format.
    void plantCapsule(const QString& sessionId, const QString& accountId)
    {
        EvidenceJournal earlier(m_dir->path() + QStringLiteral("/evidence-journal"));
        QVERIFY(earlier.open(sessionId, accountId));
        earlier.record(EvidenceJournal::stallWindow(2, 120000));
        earlier.close();
    }

    bool capsuleOnDisk(const QString& sessionId) const
    {
        const QDir dir(m_dir->path() + QStringLiteral("/evidence-journal"));
        for (const QString& name : dir.entryList({QStringLiteral("evj-*.json")}, QDir::Files)) {
            QFile file(dir.filePath(name));
            if (!file.open(QIODevice::ReadOnly)) continue;
            if (QJsonDocument::fromJson(file.readAll()).object()
                    .value(QStringLiteral("session_id")).toString() == sessionId) {
                return true;
            }
        }
        return false;
    }

    /// One outbox log line is `<prefix> event=<closed> answer=<closed> session=<id|-|none> count=<n>`.
    static bool outboxLineIsClosed(const QString& prefix, const QString& text)
    {
        const QRegularExpression pattern(QStringLiteral(
            "^%1 event=(put|drain|delivered|kept|dropped|unreadable) "
            "answer=(2xx|400|401|403|404|422|5xx|transport|none) session=[A-Za-z0-9_-]+ "
            "count=[1-9][0-9]*$").arg(prefix));
        return pattern.match(text).hasMatch();
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
        // An unknown balance also reads 0 (ADR-0075: no number stands for "unknown"), so a first
        // read of 0 is told apart only by the text.
        QTRY_VERIFY_WITH_TIMEOUT(!client.balanceText().isEmpty(), 15000);
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

    /// Waits, without the event loop, for the session's opening wallet answer, then drives one tick
    /// inside the stream and waits for that tick's answer too. Every in-stream test calls this before
    /// it reads the HUD or samples a counter.
    ///
    /// Why (06.3 test-race fix): since D-11 (Plan 06.3-06) liveness starts at `beginSession()`, and
    /// its immediate first tick reads the wallet there - before `handleConnectionStarted()` has begun
    /// the HUD's session. Whether that answer lands before or after `HudOverlay::beginSession()` is up
    /// to the network thread; one that lands before is dropped by design (`noteCreditMinutes`: not a
    /// session) and the HUD shows the seed until the next tick. The move to stage `streaming` is one
    /// more report, with no wallet read (`reportNow()`), queued to the network thread with nothing
    /// ordering it against that first answer. So the first answer is not a read the HUD is sure to
    /// get, and a counter sampled right after it may or may not include the stage report.
    ///
    /// The tick driven here settles both. It is queued behind the stage report (events posted to one
    /// thread at one priority run in the order posted), and its read is made inside the HUD's session.
    /// When this returns, every report the session start made has been sent, no wallet answer is in
    /// flight, and the HUD has had one real read - all without this thread's event loop running.
    bool settleIntoTheStream(SeatHubClient& client, WalletTicks* ticks)
    {
        if (!waitWithoutTheEventLoop(
                [ticks]() { return ticks->read.load() + ticks->failed.load() >= 1; })) {
            return false;
        }
        return tickAndWait(client, ticks);
    }

    static void endStreaming(SeatHubClient& client, FakeEngineSession* engine)
    {
        client.session()->attachSession(nullptr);
        delete engine;
    }

private slots:
    void onePlaySummaryPerExitPath_data()
    {
        QTest::addColumn<int>("exit");
        QTest::addColumn<QString>("token");
        QTest::addColumn<QString>("outcome");
        QTest::addColumn<bool>("liveFrame");
        const char* tokens[] = {"refused", "pair_failed", "stage_failed", "launch_failed",
            "start_refused", "cancelled", "server_ended_pre", "ended", "terminated",
            "server_ended", "horizon", "signed_out", "app_quit"};
        for (int i = 0; i < 13; ++i) {
            // E7 (server_ended_pre): an outside CUSTOMER_ENDED while connecting is a cancel, not a
            // failed start (E16), so its outcome is neutral; the End the facade sends is unchanged.
            const QString outcome = i < 5 || i == 8
                ? QStringLiteral("failed") : (i == 5 || i == 6 || i >= 11 ? QStringLiteral("cancelled")
                                                                       : QStringLiteral("ok"));
            QTest::newRow(qPrintable(QStringLiteral("E%1-%2").arg(i + 1).arg(tokens[i])))
                << i + 1 << QString::fromLatin1(tokens[i]) << outcome << false;
        }
    }

    void onePlaySummaryPerExitPath()
    {
        QFETCH(int, exit);
        QFETCH(QString, token);
        QFETCH(QString, outcome);
        QFETCH(bool, liveFrame);
        QTemporaryDir spool;
        QMutex mutex;
        QList<ShippedLine> records;
        QString userAtSummary;
        auto engine = std::make_unique<FakeEngineSession>();
        auto client = std::make_unique<SeatHubClient>();
        if (exit == 1) reachHome(*client, 90);
        else beginStagedSession(*client);
        QVERIFY(!QTest::currentTestFailed());
        client->teardown()->setVerifyIntervalMs(1);
        LogShipper::instance().setSpoolDirectoryForTests(spool.path());
        LogShipper::instance().start([&](const ShippedLine& line) {
            QMutexLocker lock(&mutex);
            records.append(line);
            if (line.body == QLatin1String("play.summary")) userAtSummary = SeatHubTelemetry::currentUserId();
            return true;
        });
        LogShipper::instance().setCanShip(true);
        StopShipperOnScopeExit guard;
        if (exit >= 8) {
            client->session()->attachSession(engine.get());
            client->session()->start(nullptr);
            emit engine->connectionStarted();
            if (liveFrame) {
                auto& compositor = client->hud()->compositor();
                SDL_Surface* surface = OsdCompositor::rasterize(Overlay::OverlayDebug,
                    "Rendering frame rate: 59.00 FPS\nAverage network latency: 23 ms (variance: 1 ms)",
                    true, SDL_Color{255, 255, 255, 255}, &compositor);
                if (surface) SDL_FreeSurface(surface);
                QTest::qWait(5);
                emitStatsBlock(59.0);
            }
        }
        switch (exit) {
        case 1:
            m_fake->answerPlay(402, playRefusalBody(QStringLiteral("refused"), QStringLiteral("SH-TEST")));
            client->start();
            QTRY_COMPARE(client->homeStatus(), QStringLiteral("refused"));
            break;
        case 2: emit client->pairing()->pairingFailed(SeatHubFailure::generic()); break;
        case 3:
            client->session()->attachSession(engine.get());
            client->session()->start(nullptr);
            emit engine->stageFailed(QStringLiteral("RTSP handshake"), -1, QString()); break;
        case 4:
            client->session()->attachSession(engine.get());
            client->session()->start(nullptr);
            emit engine->displayLaunchError(QStringLiteral("local-only diagnostic")); break;
        case 5: emit client->pairing()->pairingCompleted(QStringLiteral("fixture-client")); break;
        case 6: client->interrupt(); break;
        case 7: case 10: {
            SessionInfo over = sessionIn(QStringLiteral("COMPLETED"), QStringLiteral("s-stages"));
            over.endReason = QStringLiteral("CUSTOMER_ENDED");
            report(*client, over); break;
        }
        case 8: emitConnectionTerminated(0); emit engine->readyForDeletion(); break;
        case 9:
            emitConnectionTerminated(-102);
            emit engine->displayLaunchError(QStringLiteral("local-only diagnostic"));
            emit engine->readyForDeletion(); break;
        case 11: QVERIFY(QMetaObject::invokeMethod(client.get(), "handleHorizonReached", Qt::DirectConnection)); break;
        case 12: client->signOut(); break;
        case 13: client.reset(); break;
        }
        // Duplicate teardown/exit notifications must not produce another summary.
        if (client && exit >= 8) emit engine->readyForDeletion();
        auto summaries = [&]() {
            QMutexLocker lock(&mutex);
            QList<ShippedLine> result;
            for (const auto& line : records) if (line.body == QLatin1String("play.summary")) result.append(line);
            return result;
        };
        QTRY_COMPARE_WITH_TIMEOUT(summaries().size(), 1, 10000);
        LogShipper::instance().drainBeforeSignOut();
        const auto summary = summaries().first();
        QCOMPARE(summary.attrs.value("exit_path").toString(), token);
        QCOMPARE(summary.attrs.value("outcome").toString(), outcome);
        QCOMPARE(summaries().size(), 1);
        if (exit == 1) {
            QVERIFY(summary.attrs.contains("t_allocate_ms"));
            QVERIFY(summary.sessionId.isEmpty());
            QVERIFY(!summary.attrs.contains("stream_s"));
            QVERIFY(!summary.attrs.contains("rollups"));
            QVERIFY(!summary.attrs.contains("cfg_res"));
        }
        if (exit >= 8) {
            QVERIFY(summary.attrs.contains("stream_s"));
            QVERIFY(summary.attrs.contains("rollups"));
            QVERIFY(summary.attrs.contains("cfg_res"));
            QVERIFY(summary.attrs.contains("cfg_fps"));
            QVERIFY(summary.attrs.contains("cfg_bitrate_kbps"));
            QVERIFY(summary.attrs.contains("bad_episodes"));
            QVERIFY(summary.attrs.contains("bad_seconds"));
            QVERIFY(summary.attrs.contains("sampling_gap_s"));
            if (liveFrame) {
                QVERIFY(summary.attrs.contains("t_first_frame_ms"));
                QCOMPARE(summary.attrs.value("last_step").toString(), QStringLiteral("stream"));
                QCOMPARE(summary.attrs.value("fps_avg").toDouble(), 59.0);
            }
            else {
                QCOMPARE(summary.attrs.value("last_step").toString(), QStringLiteral("first_frame"));
                QCOMPARE(summary.attrs.value("failure_class").toString(), QStringLiteral("no_first_frame"));
                QVERIFY(!summary.attrs.contains("t_first_frame_ms"));
            }
        }
        if (exit == 3) QVERIFY(summary.attrs.contains("t_engine_connect_ms"));
        if (exit == 12 || exit == 13) QVERIFY(!userAtSummary.isEmpty());
        if (exit == 12) QVERIFY(SeatHubTelemetry::currentUserId().isEmpty());
        if (client) client->session()->attachSession(nullptr);
    }

    void aPlayNeverSummarisesTwice_data()
    {
        QTest::addColumn<int>("exit");
        QTest::addColumn<QString>("token");
        QTest::addColumn<QString>("outcome");
        QTest::addColumn<bool>("liveFrame");
        QTest::newRow("server-end-then-engine-deletion") << 10 << QStringLiteral("server_ended")
            << QStringLiteral("ok") << true;
    }
    void aPlayNeverSummarisesTwice() { onePlaySummaryPerExitPath(); }

    void signOutSummarisesBeforeTheDrain_data()
    {
        QTest::addColumn<int>("exit");
        QTest::addColumn<QString>("token");
        QTest::addColumn<QString>("outcome");
        QTest::addColumn<bool>("liveFrame");
        QTest::newRow("signed-out") << 12 << QStringLiteral("signed_out") << QStringLiteral("cancelled") << true;
        QTest::newRow("quit") << 13 << QStringLiteral("app_quit") << QStringLiteral("cancelled") << true;
    }
    void signOutSummarisesBeforeTheDrain() { onePlaySummaryPerExitPath(); }

    void theSummaryCarriesThePerStepTimingsAndTheConfiguredValues_data()
    {
        QTest::addColumn<int>("exit");
        QTest::addColumn<QString>("token");
        QTest::addColumn<QString>("outcome");
        QTest::addColumn<bool>("liveFrame");
        QTest::newRow("live-frame-and-final-decoder-stats") << 8 << QStringLiteral("ended")
            << QStringLiteral("ok") << true;
    }
    void theSummaryCarriesThePerStepTimingsAndTheConfiguredValues() { onePlaySummaryPerExitPath(); }

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
        m_evidenceStatusAtArm = 0;

        // Plan 15: SeatHubTelemetry's identity is process-global state (this suite never calls
        // start(), so there is no SDK scope to reset instead) - clear it before every test so an
        // earlier test's sign-in/session/trace never leaks into this one.
        SeatHubTelemetry::clearUser();
        SeatHubTelemetry::clearSession();
        SeatHubTelemetry::clearTrace();
        // ADR-0107: the covered-upload switch is process-global; every test starts unloaded.
        ReportingConsent::setStateForTests(false, reporting_consent_store::Record{});
        // The engine's own start-up state for its Debug (stats) overlay (upstream default: hidden).
        StreamingPreferences::get()->showPerformanceOverlay = false;
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
        QCOMPARE(client.balanceMinutes(), qint64(0));
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

    void theNoticeVersionGatesTheProperty()
    {
        const QString organization = QCoreApplication::organizationName();
        const QString application = QCoreApplication::applicationName();
        const auto format = QSettings::defaultFormat();
        auto restore = qScopeGuard([&]() {
            QCoreApplication::setOrganizationName(organization);
            QCoreApplication::setApplicationName(application);
            QSettings::setDefaultFormat(format);
        });
        QCoreApplication::setOrganizationName("SeatHubTest");
        QCoreApplication::setApplicationName("DiagnosticsNoticeTest");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_dir->path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, m_dir->path());
        QSettings settings;
        const QString key = QStringLiteral("diagnosticsNoticeVersion");
        QVERIFY(!settings.contains(key));
        {
            SeatHubClient client;
            QVERIFY2(client.metaObject()->indexOfProperty("diagnosticsNoticePending") >= 0,
                     "the facade lacks the version-gated diagnostics notice property");
            QVERIFY(!client.property("diagnosticsNoticePending").toBool());
            reachHome(client, 90); QVERIFY(!QTest::currentTestFailed());
            QVERIFY(client.property("diagnosticsNoticePending").toBool());
            const QString feedPath = m_dir->path() + "/feed/api/releases";
            QVERIFY(QDir().mkpath(feedPath));
            client.updates()->setBaseUrl(QUrl::fromLocalFile(m_dir->path() + "/feed").toString());
            client.updates()->setInstalledVersion("2.0.0");
            client.updates()->setRetryStatePath(m_dir->path() + "/retry.json");
            client.updates()->setJournalFolder(m_dir->path() + "/journal");
            client.openSettings(); QVERIFY(!client.property("diagnosticsNoticePending").toBool());
            client.closeSettings(); QVERIFY(client.property("diagnosticsNoticePending").toBool());
            client.openProfile(); QVERIFY(client.property("diagnosticsNoticePending").toBool());
            client.closeProfile();
            QTemporaryDir spool;
            QMutex shippedMutex;
            QStringList shipped;
            LogShipper::instance().setSpoolDirectoryForTests(spool.path());
            LogShipper::instance().start([&](const ShippedLine& line) {
                QMutexLocker lock(&shippedMutex); shipped.append(line.body); return true;
            });
            LogShipper::instance().setCanShip(true);
            StopShipperOnScopeExit stopShipper;
            auto* engine = new FakeEngineSession;
            client.session()->attachSession(engine);
            m_fake->answerPairing(409, playRefusalBody("The rig is not ready yet.", "SH-2K2XQ1"));
            client.beginSession("s-notice");
            QVERIFY(!client.property("diagnosticsNoticePending").toBool());
            emit engine->connectionStarted();
            QCOMPARE(client.appState(), QStringLiteral("streaming"));
            QVERIFY(!client.property("diagnosticsNoticePending").toBool());
            m_fake->answerSession("s-notice", "CANCELLED", "CUSTOMER_ENDED");
            client.interrupt(); emit engine->sessionFinished(0); emit engine->readyForDeletion();
            QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);
            QVERIFY(client.property("diagnosticsNoticePending").toBool());
            LogShipper::instance().drainBeforeSignOut();
            {
                QMutexLocker lock(&shippedMutex);
                QVERIFY2(shipped.contains("play.summary"), "actual diagnostic hand-off must not wait for OK");
            }
            QVERIFY(!QSettings().contains(key));
            // Drive actual feed replies. ADR-0077: every offer is mandatory, so a feed row that
            // still carries an old min_version is mandatory too (the floor is the newest release).
            for (const bool withMinVersion : {false, true}) {
                QFile feed(feedPath + "/client"); QVERIFY(feed.open(QIODevice::WriteOnly));
                feed.write(!withMinVersion
                    ? QByteArrayLiteral("{\"version\":\"3.0.0\",\"url\":\"https://example.invalid/setup.exe\"}")
                    : QByteArrayLiteral("{\"version\":\"3.0.0\",\"min_version\":\"1.0.0\",\"url\":\"https://example.invalid/setup.exe\"}"));
                feed.close();
                QSignalSpy checked(client.updates(), &UpdateFeedClient::checkFinished);
                QTRY_VERIFY(client.updates()->state() != QStringLiteral("checking"));
                checked.clear(); // exclude the stream-end check that was already in flight
                QVERIFY(client.updates()->checkForUpdates());
                QTRY_VERIFY(!checked.isEmpty());
                QVERIFY(client.updates()->mandatory());
                QVERIFY(!client.property("diagnosticsNoticePending").toBool());
                QVERIFY(feed.open(QIODevice::WriteOnly)); feed.write("{\"version\":\"2.0.0\"}"); feed.close();
                checked.clear(); QVERIFY(client.updates()->checkForUpdates());
                QTRY_VERIFY(!checked.isEmpty());
                QVERIFY(client.property("diagnosticsNoticePending").toBool());
            }
            QVERIFY(QMetaObject::invokeMethod(&client, "acknowledgeDiagnosticsNotice"));
            QVERIFY(!client.property("diagnosticsNoticePending").toBool());
            QSettings disk; QCOMPARE(disk.value(key, 0).toInt(), 1);
            QCOMPARE(disk.value(key).metaType(), QMetaType::fromType<int>());
            QVERIFY(QFile::exists(disk.fileName()));
            client.signOut(); QCOMPARE(QSettings().value(key, 0).toInt(), 1);
        }
        {
            SeatHubClient updated;
            reachHome(updated, 90); QVERIFY(!QTest::currentTestFailed());
            QVERIFY(!updated.property("diagnosticsNoticePending").toBool());
        }
        settings.setValue(key, 0); settings.sync();
        {
            SeatHubClient newerWording;
            reachHome(newerWording, 90); QVERIFY(!QTest::currentTestFailed());
            QVERIFY(newerWording.property("diagnosticsNoticePending").toBool());
            newerWording.signOut(); QCOMPARE(QSettings().value(key).toInt(), 0);
        }
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
            QCOMPARE(engine->interrupts.load(), 1);

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

        armControlPlane(client);
        // The rig never answers in this test either way; keep the background pairing poll from
        // resolving on its own - D-05/C2 would otherwise end this session before the test's own
        // `readyForDeletion` gets a chance to.
        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));

        client.beginSession(QStringLiteral("session-one"));
        emit engine->connectionStarted();
        QVERIFY2(!client.attachedSessionId().isEmpty(), "a session that has streamed stays attached");

        QSignalSpy completed(client.teardown(), &TeardownController::teardownCompleted);
        QSignalSpy failed(client.teardown(), &TeardownController::teardownFailed);
        QSignalSpy stages(client.teardown(), &TeardownController::stageEntered);
        QSignalSpy lifecycleEnded(client.session(), &SessionLifecycle::readyForDeletion);

        // The engine's own signal, arriving the way the engine delivers it.
        emit engine->readyForDeletion();

        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 15000);
        QCOMPARE(failed.count(), 0);
        QTRY_VERIFY2_WITH_TIMEOUT(client.attachedSessionId().isEmpty(),
                                  "a session that has been torn down is no longer attached", 15000);

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
        QCOMPARE(client.balanceMinutes(), qint64(0));
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

    // --- Phase 5 plan 06: the email-and-password route and the website links --------------------

    static QByteArray refusalBody(const QString& sentence, const QString& reference)
    {
        QJsonObject body;
        body.insert(QStringLiteral("error"), sentence);
        body.insert(QStringLiteral("reference"), reference);
        return QJsonDocument(body).toJson(QJsonDocument::Compact);
    }

    void aPasswordSignInStoresTheCredentialAndReachesHome()
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

        // The credential is stored in the access slot, nothing in the old refresh slot, and the
        // same bearer is used on the next request.
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

    void aPhoneSignsInWithThePasswordThroughLogin()
    {
        // A-ID / C-SIGNIN: a phone number signs in through the same call as an email. The client sends
        // exactly what was typed and the picked ISO region; it never builds E.164 or a dial code, and
        // no code step exists.
        SeatHubClient client;
        isolateStore(client);
        armControlPlane(client);
        m_fake->answerLogin(200, QByteArrayLiteral("{\"access_token\":\"sb_at_from_login\"}"));
        m_fake->answerWallet(200, walletBody(90));

        QSignalSpy accepted(&client, &SeatHubClient::passwordSignInAccepted);
        QSignalSpy rejected(&client, &SeatHubClient::passwordSignInRejected);

        // How a customer in Amman writes it: separators, a trunk zero, and a lower-case region.
        client.signInWithPassword(QStringLiteral(" (079) 000-0000 "), QStringLiteral("a password"),
                                  QStringLiteral("jo"));
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);
        QCOMPARE(accepted.count(), 1);
        QCOMPARE(rejected.count(), 0);

        const QJsonObject sent = QJsonDocument::fromJson(
            m_fake->bodyFor(QStringLiteral("/api/auth/login"))).object();
        QCOMPARE(sent.value(QStringLiteral("identifier")).toString(),
                 QStringLiteral("(079) 000-0000"));
        QCOMPARE(sent.value(QStringLiteral("password")).toString(), QStringLiteral("a password"));
        QCOMPARE(sent.value(QStringLiteral("region")).toString(), QStringLiteral("JO"));
        QCOMPARE(sent.size(), 3);
        QVERIFY2(!QString::fromUtf8(m_fake->bodyFor(QStringLiteral("/api/auth/login")))
                      .contains(QStringLiteral("962")),
                 "the client never puts a dial code in front of what was typed");

        // The same credential handling as an email, and no code route was ever asked.
        QCOMPARE(client.credentialStore()->retrieveToken(TokenStore::accessTokenName()),
                 QStringLiteral("sb_at_from_login"));
        QTRY_COMPARE_WITH_TIMEOUT(client.balanceText(), QStringLiteral("1 h 30 min"), 15000);
        QCOMPARE(m_fake->authorizationFor(QStringLiteral("/api/wallet")),
                 QByteArrayLiteral("Bearer sb_at_from_login"));
        for (const QString& path : m_fake->requestPaths()) {
            QVERIFY2(!path.contains(QStringLiteral("/otp")), qPrintable(path));
        }

        // A number written with its own country code is sent as typed too; the region is only the
        // picked country, the server decides which one wins.
        client.signOut();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("signed_out"), 15000);
        client.signInWithPassword(QStringLiteral("+44 7911 123456"), QStringLiteral("a password"),
                                  QStringLiteral("JO"));
        QTRY_COMPARE_WITH_TIMEOUT(accepted.count(), 2, 15000);
        const QJsonObject second = QJsonDocument::fromJson(
            m_fake->bodyFor(QStringLiteral("/api/auth/login"))).object();
        QCOMPARE(second.value(QStringLiteral("identifier")).toString(), QStringLiteral("+44 7911 123456"));
        QCOMPARE(second.value(QStringLiteral("region")).toString(), QStringLiteral("JO"));
    }

    void noAccountOffersTheWebsiteSignUp()
    {
        // C-SIGNIN: when the server answers that no account uses that email or phone, the screen is
        // told so (the third argument) and offers the website's sign-up page. SeatHub never creates an
        // account: nothing here asks any route that could.
        SeatHubClient client;
        isolateStore(client);
        armControlPlane(client);
        const QString sentence = QStringLiteral("No account uses that email or phone.");
        QJsonObject refusal;
        refusal.insert(QStringLiteral("status"), false);
        refusal.insert(QStringLiteral("error"), sentence);
        refusal.insert(QStringLiteral("reference"), QStringLiteral("SH-3K2XQ1"));
        refusal.insert(QStringLiteral("kind"), QStringLiteral("account.login.no_account"));
        m_fake->answerLogin(401, QJsonDocument(refusal).toJson(QJsonDocument::Compact));

        QSignalSpy rejected(&client, &SeatHubClient::passwordSignInRejected);
        client.signInWithPassword(QStringLiteral("0790000000"), QStringLiteral("a password"),
                                  QStringLiteral("JO"));
        QTRY_COMPARE_WITH_TIMEOUT(rejected.count(), 1, 15000);
        QCOMPARE(rejected.at(0).at(0).toString(), sentence);
        QCOMPARE(rejected.at(0).at(1).toString(), QStringLiteral("SH-3K2XQ1"));
        QVERIFY2(rejected.at(0).at(2).toBool(), "the refusal is flagged as no account");

        // Every other refusal is not.
        m_fake->answerLogin(401, refusalBody(QStringLiteral("That password is wrong."),
                                             QStringLiteral("SH-4F7KQ2")));
        client.signInWithPassword(QStringLiteral("0790000000"), QStringLiteral("a password"),
                                  QStringLiteral("JO"));
        QTRY_COMPARE_WITH_TIMEOUT(rejected.count(), 2, 15000);
        QVERIFY(!rejected.at(1).at(2).toBool());
        // An unreachable control plane says nothing about whether an account exists.
        m_fake->answerLogin(0, QByteArray());
        client.signInWithPassword(QStringLiteral("0790000000"), QStringLiteral("a password"),
                                  QStringLiteral("JO"));
        QTRY_COMPARE_WITH_TIMEOUT(rejected.count(), 3, 15000);
        QVERIFY(!rejected.at(2).at(2).toBool());

        // The offer opens the website's sign-up page, and only that page.
        QList<QUrl> opened;
        client.setUrlOpener([&opened](const QUrl& url) {
            opened.append(url);
            return true;
        });
        QVERIFY(client.openWebsite(QStringLiteral("signup")));
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened.first().toString(), QStringLiteral("https://rentapc.online/login?mode=signup"));
        QVERIFY(opened.first().userInfo().isEmpty());

        // Nothing was signed in and no route that makes an account was asked.
        QVERIFY(!client.credentialStore()->hasToken(TokenStore::accessTokenName()));
        for (const QString& path : m_fake->requestPaths()) {
            QVERIFY2(path == QLatin1String("/api/auth/login"), qPrintable(path));
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
        // Only the no-account refusal asks the screen to offer the website's sign-up page.
        QCOMPARE(rejected.first().at(2).toBool(),
                 sentence == QStringLiteral("No account uses that email or phone."));
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
                 QStringLiteral("We couldn't reach RentaPC. Try again in a moment."));
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
                 QStringLiteral("https://rentapc.online/login?mode=signup"));
        QCOMPARE(client.websiteUrl(QStringLiteral("reset")),
                 QStringLiteral("https://rentapc.online/forgot-password"));
        QCOMPARE(client.websiteUrl(QStringLiteral("topup")),
                 QStringLiteral("https://rentapc.online/topup"));
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
        QCOMPARE(opened.at(0).toString(), QStringLiteral("https://rentapc.online/topup"));

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
        QCOMPARE(client.balanceMinutes(), qint64(0));
        QVERIFY(client.balanceText().isEmpty());
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
            QVERIFY(client.attachedSessionId().isEmpty());
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

    // D-05/C2: a failed step before the stream starts ends the session at once rather than leaving
    // it attached for Play to find later. This test used to be named the other way around
    // ("...IsLiveAndPlayResumesItInsteadOfAskingForAnother") when the pre-D-05 contract left a
    // failed, never-streamed session attached and resumable; D-05 replaces that, and UD-04 R14
    // removes Resume altogether.
    void aSessionThatFailsBeforeStreamingIsEndedAndPlayStartsAFreshOne()
    {
        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());
        QVERIFY(client.attachedSessionId().isEmpty());
        m_fake->answerSession(QStringLiteral("session-live"), QStringLiteral("CANCELLED"));
        client.teardown()->setVerifyIntervalMs(1);

        // A session is attached (Play's allocation returned it), but it never streams: the rig has
        // no answer in this test, so pairing fails - and D-05/C2 ends the session at once.
        client.beginSession(QStringLiteral("session-live"));
        QCOMPARE(client.attachedSessionId(), QStringLiteral("session-live"));
        QTRY_VERIFY_WITH_TIMEOUT(client.connectFailed(), 15000);
        QCOMPARE(client.appState(), QStringLiteral("connecting"));
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/session-live/end")),
            15000);

        // Back on Home, Play is a fresh Play: the ended session is detached and never asked to
        // resume.
        client.dismissError();
        QCOMPARE(client.appState(), QStringLiteral("home"));
        QTRY_VERIFY_WITH_TIMEOUT(client.attachedSessionId().isEmpty(), 15000);

        m_fake->answerPlay(201, QByteArrayLiteral("{\"id\":\"session-live-2\"}"));
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("connecting"), 15000);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions")), 1);
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/session-live-2/pairing")),
            15000);

        // Sign-out forgets it either way: the next customer on this PC inherits no session.
        client.signOut();
        QVERIFY(client.attachedSessionId().isEmpty());
    }

    // --- A-LC / C-SAMEPLAY: the Play that answers PREVIOUS_SESSION_ENDING is repeated ----------------

    void aReplacedPlayRepeatsWithTheSameKeyUntilItGetsASession()
    {
        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());
        // No jitter, so the repeats land on the schedule (V9: 1 s, then x1.6).
        client.setReconnectJitterProvider([]() { return 0.0; });
        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));

        const QByteArray ending = playRefusalBody(
            QStringLiteral("Your previous session is still ending."), QStringLiteral("SH-7K2XQ1"),
            QStringLiteral("PREVIOUS_SESSION_ENDING"));
        m_fake->queuePlay(409, ending);
        m_fake->queuePlay(409, ending);
        m_fake->answerPlay(201, QByteArrayLiteral("{\"id\":\"s-replaced\"}"));

        QStringList statuses;
        QObject::connect(&client, &SeatHubClient::homeStatusChanged,
                         [&]() { statuses.append(client.homeStatus()); });
        client.start();

        // The busy Play stays and says what is happening: no failure, no refusal, still Home.
        QTRY_COMPARE_WITH_TIMEOUT(client.homeStatus(), QStringLiteral("previous_ending"), 15000);
        QCOMPARE(client.appState(), QStringLiteral("home"));
        QVERIFY(client.failure().isEmpty());
        // A second press while it waits is the same Play, not a new one.
        client.start();
        QCOMPARE(m_fake->playIdempotencyKeys().size(), 1);

        // Then the session arrives, on the third ask.
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("connecting"), 30000);
        QCOMPARE(client.attachedSessionId(), QStringLiteral("s-replaced"));
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions")), 3);
        const QList<QByteArray> keys = m_fake->playIdempotencyKeys();
        QCOMPARE(keys.size(), 3);
        QVERIFY(!keys.at(0).isEmpty());
        QCOMPARE(keys.at(1), keys.at(0));
        QCOMPARE(keys.at(2), keys.at(0));
        // The wait is one stretch of the busy state, then ready - never a refusal or an error.
        QVERIFY2(!statuses.contains(QStringLiteral("refused")), qPrintable(statuses.join(',')));
        QVERIFY2(!statuses.contains(QStringLiteral("busy")), qPrintable(statuses.join(',')));
        QCOMPARE(client.homeStatus(), QStringLiteral("ready"));
        QVERIFY(client.failure().isEmpty());
    }

    void aRepeatedPlayStopsAtTheFirstOtherAnswerAndTheNextPlayHasAFreshKey()
    {
        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());
        client.setReconnectJitterProvider([]() { return 0.0; });

        m_fake->queuePlay(409, playRefusalBody(QStringLiteral("Your previous session is still ending."),
                                               QStringLiteral("SH-7K2XQ1"),
                                               QStringLiteral("PREVIOUS_SESSION_ENDING")));
        // `NO_HOST_AVAILABLE` keeps its own branch: the empty state, not an error and not a repeat.
        m_fake->answerPlay(409, playRefusalBody(QStringLiteral("None available."),
                                                QStringLiteral("SH-8K2XQ1"),
                                                QStringLiteral("NO_HOST_AVAILABLE")));
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.homeStatus(), QStringLiteral("busy"), 30000);
        QCOMPARE(m_fake->playIdempotencyKeys().size(), 2);
        QCOMPARE(m_fake->playIdempotencyKeys().at(1), m_fake->playIdempotencyKeys().at(0));
        // Nothing repeats after an answer that is not the retryable one.
        QTest::qWait(2500);
        QCOMPARE(m_fake->playIdempotencyKeys().size(), 2);

        // The next press is a new Play with a new key.
        m_fake->answerPlay(402, playRefusalBody(QStringLiteral("Not enough credit."),
                                                QStringLiteral("SH-3K2XQ1")));
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.homeStatus(), QStringLiteral("refused"), 15000);
        QCOMPARE(m_fake->playIdempotencyKeys().size(), 3);
        QVERIFY(m_fake->playIdempotencyKeys().at(2) != m_fake->playIdempotencyKeys().at(0));

        // `USER_HAS_NONTERMINAL_SESSION` is another answer too: it is shown, not repeated.
        m_fake->answerPlay(409, playRefusalBody(QStringLiteral("You already have a session."),
                                                QStringLiteral("SH-9K2XQ1"),
                                                QStringLiteral("USER_HAS_NONTERMINAL_SESSION")));
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(m_fake->playIdempotencyKeys().size(), 4, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(client.homeStatus(), QStringLiteral("refused"), 15000);
        QTest::qWait(1500);
        QCOMPARE(m_fake->playIdempotencyKeys().size(), 4);
    }

    void aSignOutEndsTheWaitingPlayRepeat()
    {
        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());
        client.setReconnectJitterProvider([]() { return 0.0; });
        m_fake->answerPlay(409, playRefusalBody(QStringLiteral("Your previous session is still ending."),
                                                QStringLiteral("SH-7K2XQ1"),
                                                QStringLiteral("PREVIOUS_SESSION_ENDING")));
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.homeStatus(), QStringLiteral("previous_ending"), 15000);
        QCOMPARE(m_fake->playIdempotencyKeys().size(), 1);

        client.signOut();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("signed_out"), 15000);
        QCOMPARE(client.homeStatus(), QStringLiteral("ready"));
        // The armed repeat finds nothing to repeat for.
        QTest::qWait(2500);
        QCOMPARE(m_fake->playIdempotencyKeys().size(), 1);
        QCOMPARE(client.homeStatus(), QStringLiteral("ready"));
    }

    // --- R6A: Profile and Settings over Connecting and Error ----------------------------------------

    void menuSettingsOverConnectingShowsAtOnceAndBackReturns()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        QCOMPARE(client.attachedSessionId(), QStringLiteral("s-stages"));

        // Settings from the menu on the Connecting view: shown at once, over it. The state and the
        // session underneath are exactly where they were.
        QSignalSpy settingsChanges(&client, &SeatHubClient::inSettingsChanged);
        client.openSettings();
        QVERIFY(client.inSettings());
        QCOMPARE(settingsChanges.count(), 1);
        QCOMPARE(client.appState(), QStringLiteral("connecting"));
        QCOMPARE(client.attachedSessionId(), QStringLiteral("s-stages"));

        // Back returns to Connecting, not to Home.
        client.closeSettings();
        QVERIFY(!client.inSettings());
        QCOMPARE(client.appState(), QStringLiteral("connecting"));
        QCOMPARE(client.attachedSessionId(), QStringLiteral("s-stages"));

        // Profile opens there too, replaces Settings (only one shows), and Back returns the same way.
        client.openProfile();
        QVERIFY(client.inProfile());
        QVERIFY(!client.inSettings());
        QCOMPARE(client.appState(), QStringLiteral("connecting"));
        client.openSettings();
        QVERIFY(client.inSettings());
        QVERIFY(!client.inProfile());
        client.openProfile();
        QVERIFY(client.inProfile() && !client.inSettings());
        client.closeProfile();
        QVERIFY(!client.inProfile() && !client.inSettings());
        QCOMPARE(client.appState(), QStringLiteral("connecting"));
        QCOMPARE(client.attachedSessionId(), QStringLiteral("s-stages"));
    }

    void menuSettingsOverTheErrorViewShowsAtOnceAndBackReturns()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"));
        // No engine is attached, so pairing completing has nothing to start a stream with: the error view.
        emit client.pairing()->pairingCompleted(QStringLiteral("aa:bb:cc:dd:ee:ff"));
        QCOMPARE(client.appState(), QStringLiteral("error"));
        const QVariantMap failure = client.failure();
        QVERIFY(!failure.isEmpty());

        client.openSettings();
        QVERIFY(client.inSettings());
        QCOMPARE(client.appState(), QStringLiteral("error"));
        QCOMPARE(client.failure(), failure);
        client.closeSettings();
        QVERIFY(!client.inSettings());
        QCOMPARE(client.appState(), QStringLiteral("error"));
        QCOMPARE(client.failure(), failure);

        client.openProfile();
        QVERIFY(client.inProfile());
        QCOMPARE(client.appState(), QStringLiteral("error"));
        client.closeProfile();
        QVERIFY(!client.inProfile());
        QCOMPARE(client.appState(), QStringLiteral("error"));
        QCOMPARE(client.failure(), failure);

        // Try again leaves the menu's view behind with the error: it re-runs the step that failed.
        client.openSettings();
        client.retry();
        QVERIFY(!client.inSettings());
        QVERIFY(!client.inProfile());
    }

    void aMenuViewDoesNotSurviveTheStreamStartingOrASignOut()
    {
        auto* engine = new FakeEngineSession;
        {
            SeatHubClient client;
            client.session()->attachSession(engine);
            beginStagedSession(client);
            QVERIFY(!QTest::currentTestFailed());
            PairingController* pairing = client.pairing();
            QMetaObject::invokeMethod(
                pairing, [pairing]() { pairing->cancel(); }, Qt::BlockingQueuedConnection);

            client.openSettings();
            QVERIFY(client.inSettings());
            emit engine->connectionStarted();
            QCOMPARE(client.appState(), QStringLiteral("streaming"));
            // The stream hides this window; nothing of the menu's view is left to come back to.
            QVERIFY(!client.inSettings());
            QVERIFY(!client.inProfile());

            client.session()->attachSession(nullptr);
        }
        delete engine;
    }

    void aSessionTheServerReportsOverIsEndedAndPlayAsksForANewOne()
    {
        SeatHubClient client;
        auto* engine = new FakeEngineSession;
        client.session()->attachSession(engine);
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());
        // Keep the background pairing poll from resolving on its own (C2 would otherwise end this
        // session before it ever streams); the test drives the engine directly instead.
        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("session-live"), QStringLiteral("COMPLETED"),
                             QStringLiteral("BALANCE_EXHAUSTED"));

        client.beginSession(QStringLiteral("session-live"));
        emit engine->connectionStarted();
        QCOMPARE(client.appState(), QStringLiteral("streaming"));
        QVERIFY2(!client.attachedSessionId().isEmpty(), "a session that streamed stays attached");

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
        // The facade has handled the server's ending: it marked the session ended and said why.
        QTRY_VERIFY_WITH_TIMEOUT(!client.endReasonText().isEmpty(), 15000);

        // The engine acknowledges the interrupt the way it does in production: SDL finishes
        // tearing down and the lifecycle reports `readyForDeletion`, which is what actually
        // carries the customer back to Home.
        emit engine->readyForDeletion();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);

        // C-ENDING (screens.md §23): the End this session wrote is unconfirmed until the server
        // answers, so Home is not ready to Play over it - the outbox re-sends it and the teardown's
        // own terminal read confirms the session is over, at which point Home returns to ready.
        QTRY_VERIFY_WITH_TIMEOUT(!client.hasSavedEnd(), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(client.homeStatus(), QStringLiteral("ready"), 15000);

        // So Play is Play again, and pressing it asks the server for a new session.
        m_fake->answerPlay(402, playRefusalBody(QStringLiteral("Not enough credit."),
                                            QStringLiteral("SH-3K2XQ1")));
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.homeStatus(), QStringLiteral("refused"), 15000);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions")), 1);

        client.session()->attachSession(nullptr);
        delete engine;
    }

    void aSessionThatEndsAndTearsDownIsDetached()
    {
        SeatHubClient client;
        auto* engine = new FakeEngineSession;
        client.session()->attachSession(engine);
        client.controlPlane()->setBaseUrl(QStringLiteral("https://control.invalid"));
        client.controlPlane()->setAccessToken(QString::fromLatin1(kAccessToken));
        client.teardown()->setVerifyIntervalMs(1);
        QString tokenPath;
        QVERIFY(storeACredential(client, &tokenPath));

        armControlPlane(client);
        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));

        client.beginSession(QStringLiteral("session-one"));
        emit engine->connectionStarted();
        QVERIFY(!client.attachedSessionId().isEmpty());

        QSignalSpy completed(client.teardown(), &TeardownController::teardownCompleted);
        emit engine->readyForDeletion();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(client.attachedSessionId().isEmpty(), 15000);

        client.session()->attachSession(nullptr);
        delete engine;
    }

    // --- D-27: one trace id per Play --------------------------------------------------------

    void oneTraceIdPerPlayMintedAtSessionCreateAndClearedAtTeardown()
    {
        // Everything a Play does over its lifetime: the session create and its pairing polls all
        // carry the same trace id (only the span differs); teardown completing clears it, so a
        // request outside any Play carries none. (That a fresh Play mints a *different* trace id
        // is not re-proven end to end here - `randomTraceId()` draws fresh bits from
        // `QRandomGenerator::system()` on every call, and `tst_control_plane.cpp`'s
        // `traceparent_afterSetTraceId_carriesItOnEveryRequestWithAFreshSpan` already covers the
        // "no two draws are ever compared equal" shape of that same call. Reusing one
        // `FakeEngineSession` across a second full Play+teardown cycle - something no production
        // path or existing test in this file does; every real Play resolves a fresh engine
        // session through `handleHostResolved()` - fights this harness rather than this feature.)
        SeatHubClient client;
        auto* engine = new FakeEngineSession;
        client.session()->attachSession(engine);
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);

        // `reachHome` itself already made trace-id-less requests (the restore's `/api/me`,
        // `/api/wallet`); everything from here on is indexed relative to a snapshot taken right
        // before each Play, not from the start of the whole list, so an earlier request - or a
        // later Play's - is never mistaken for this one's.
        const int beforeFirstPlay = m_fake->requestPaths().size();
        // Keep the pairing poll from resolving on its own (C2 would otherwise end this session
        // before it ever streams); the test drives the engine's own `connectionStarted` directly.
        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));
        m_fake->answerPlay(201, QByteArrayLiteral("{\"id\":\"s-trace-1\"}"));
        client.start();

        // Wait for at least one pairing poll of this session, so there is a second request of
        // this Play to compare the session-create's trace id against.
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-trace-1/pairing")) >= 1,
            15000);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions")), 1);
        emit engine->connectionStarted();
        QVERIFY2(!client.attachedSessionId().isEmpty(), "a session that streamed stays attached");

        // Every request of this Play carries the same trace id - the session create itself
        // first of all - and only the span differs (D-27, ADR-0060 item 10).
        {
            const QStringList paths = m_fake->requestPaths();
            const QList<QByteArray> traceparents = m_fake->traceparents();
            QVERIFY(paths.size() > beforeFirstPlay);
            QCOMPARE(paths.at(beforeFirstPlay), QStringLiteral("/api/sessions"));
            const QByteArray firstTraceparent = traceparents.at(beforeFirstPlay);
            QVERIFY2(firstTraceparent.startsWith("00-"),
                     "the session create itself must carry traceparent");
            const QByteArray tracePrefix = firstTraceparent.left(36); // "00-<32 lowercase hex>-"
            int matchingSpans = 0;
            for (int i = beforeFirstPlay; i < paths.size(); ++i) {
                QVERIFY2(traceparents.at(i).startsWith(tracePrefix),
                         "every request of one Play carries the same trace id");
                if (traceparents.at(i) == firstTraceparent) {
                    ++matchingSpans;
                }
            }
            QVERIFY2(matchingSpans < paths.size() - beforeFirstPlay,
                     "the span differs between two requests of the same Play");
        }
        const QByteArray firstTracePrefix = m_fake->traceparents().at(beforeFirstPlay).left(36);

        QSignalSpy completed(client.teardown(), &TeardownController::teardownCompleted);
        emit engine->readyForDeletion();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(client.attachedSessionId().isEmpty(), 15000);

        // A request outside any Play, once teardown has completed, carries no traceparent - the
        // id this Play used is gone (`handleTeardownCompleted`).
        const int beforeReload = m_fake->requestPaths().size();
        m_fake->answerMe(200, accountBody());
        client.reloadAccount();
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->requestPaths().size() > beforeReload, 15000);
        QCOMPARE(m_fake->requestPaths().at(beforeReload), QStringLiteral("/api/me"));
        QVERIFY2(m_fake->traceparents().at(beforeReload).isEmpty(),
                 "a request after teardown, outside any Play, carries no traceparent");
        QVERIFY2(!firstTracePrefix.isEmpty(), "sanity: the first Play's own trace id was captured");

        client.session()->attachSession(nullptr);
        delete engine;
    }

    void twoRefusedPlaysMintTwoDifferentTraceIds()
    {
        // A refused Play never leaves Home (`applyPlayFailure`), so this needs neither an
        // engine nor a session - just two `client.start()` calls back to back, the same pattern
        // `aRefusalFromTheServerIsShownOnHomeInItsOwnWordsAndAnythingElseIsAnError` already uses.
        SeatHubClient client;
        reachHome(client, 0);
        QVERIFY(!QTest::currentTestFailed());

        m_fake->answerPlay(402, playRefusalBody(QStringLiteral("Not enough credit."),
                                                 QStringLiteral("SH-3K2XQ1")));
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.homeStatus(), QStringLiteral("refused"), 15000);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions")), 1);
        const QByteArray firstTraceparent = m_fake->traceparents().last();
        QVERIFY2(firstTraceparent.startsWith("00-"), "even a refused Play's request carries a trace id");

        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(m_fake->requestPaths().count(QStringLiteral("/api/sessions")), 2, 15000);
        const QByteArray secondTraceparent = m_fake->traceparents().last();
        QVERIFY2(!secondTraceparent.isEmpty(), "the second Play carries a trace id too");
        QVERIFY2(secondTraceparent.left(36) != firstTraceparent.left(36),
                 "each Play mints its own trace id, refused or not");
    }

    void aBalanceRefusalEmitsExactlyOneAllocateStep()
    {
        SeatHubClient client;
        reachHome(client, 0);
        QVERIFY(!QTest::currentTestFailed());

        QTemporaryDir spoolDir;
        QVERIFY(spoolDir.isValid());
        QMutex captureMutex;
        QList<ShippedLine> captured;
        LogShipper::instance().setSpoolDirectoryForTests(spoolDir.path());
        LogShipper::instance().start([&captureMutex, &captured](const ShippedLine& line) {
            QMutexLocker lock(&captureMutex);
            captured.append(line);
            return true;
        });
        LogShipper::instance().setCanShip(true);
        StopShipperOnScopeExit stopGuard;
        const auto playStepCount = [&captureMutex, &captured]() {
            QMutexLocker lock(&captureMutex);
            return static_cast<int>(std::count_if(captured.cbegin(), captured.cend(),
                [](const ShippedLine& line) { return line.body == QLatin1String("play.step"); }));
        };

        m_fake->answerPlay(402, playRefusalBody(QStringLiteral("Not enough credit."),
                                                QStringLiteral("SH-3K2XQ1")));
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.homeStatus(), QStringLiteral("refused"), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(playStepCount(), 1, 15000);

        LogShipper::instance().stop();
        QMutexLocker lock(&captureMutex);
        const auto record = std::find_if(captured.cbegin(), captured.cend(),
            [](const ShippedLine& line) { return line.body == QLatin1String("play.step"); });
        QVERIFY(record != captured.cend());
        QCOMPARE(record->attrs.value(QStringLiteral("step")).toString(), QStringLiteral("allocate"));
        QCOMPARE(record->attrs.value(QStringLiteral("outcome")).toString(), QStringLiteral("failed"));
        QCOMPARE(record->attrs.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("refused_balance"));
    }

    // --- WR-01: every Play-end path clears the trace id (06.3-REVIEW-fork.md) ---------------------

    void aRefusedPlayLeavesNoTraceIdOnALaterNonPlayRequest()
    {
        SeatHubClient client;
        reachHome(client, 0);
        QVERIFY(!QTest::currentTestFailed());

        m_fake->answerPlay(402, playRefusalBody(QStringLiteral("Not enough credit."),
                                                 QStringLiteral("SH-3K2XQ1")));
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.homeStatus(), QStringLiteral("refused"), 15000);

        const int beforeReload = m_fake->requestPaths().size();
        m_fake->answerMe(200, accountBody());
        client.reloadAccount();
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->requestPaths().size() > beforeReload, 15000);
        QCOMPARE(m_fake->requestPaths().at(beforeReload), QStringLiteral("/api/me"));
        QVERIFY2(m_fake->traceparents().at(beforeReload).isEmpty(),
                 "a refused Play must not leak its trace id onto a later, unrelated request");
    }

    // WR-01 and WR-02 together: `handlePairingFailed()` calls `reportFailure()`/`stop()` (which
    // self-marshal onto the network thread, since this handler runs on the facade thread) and
    // then clears the trace id - a plain, synchronous clear there would very likely beat the
    // still-queued failure report to the wire (the same "queue, then clear" race CR-02 closed for
    // the quality POST, on a different call site). This proves both halves at once: the failure
    // report itself still carries the Play's id, liveness genuinely stops, and a later request
    // carries none.
    void aPairingFailureStopsLivenessAndClearsTheTraceIdWithoutLosingItsOwnFailureReport()
    {
        SeatHubClient client;
        auto* engine = new FakeEngineSession;
        client.session()->attachSession(engine);
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());
        // The default answerPairing (no rig assigned) fails pairing at once.

        const int beforePlay = m_fake->requestPaths().size();
        m_fake->answerPlay(201, QByteArrayLiteral("{\"id\":\"aaaaaaaa-1111-1111-1111-111111111111\"}"));
        client.start();
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().count(QStringLiteral("/api/sessions/aaaaaaaa-1111-1111-1111-111111111111/pairing")) >= 1,
            15000);
        QTRY_VERIFY_WITH_TIMEOUT(!client.liveness()->isRunning(), 15000); // WR-02

        const QByteArray sessionCreateTrace = m_fake->traceparents().at(beforePlay).left(36);
        QVERIFY2(!sessionCreateTrace.isEmpty(), "sanity: the session create minted a trace id");

        int livenessIndex = -1;
        {
            const QStringList paths = m_fake->requestPaths();
            for (int i = beforePlay; i < paths.size(); ++i) {
                if (paths.at(i).endsWith(QStringLiteral("/liveness"))) {
                    livenessIndex = i;
                }
            }
        }
        QVERIFY2(livenessIndex >= 0, "the pairing-failure liveness report must have been sent");
        QVERIFY2(m_fake->traceparents().at(livenessIndex).startsWith(sessionCreateTrace),
                 "the failed liveness report must still carry this Play's own trace id");

        // WR-02: liveness has genuinely stopped - no further /liveness requests arrive.
        const int livenessCountAfterFailure = m_fake->countOfPathEndingWith(QStringLiteral("/liveness"));
        QTest::qWait(300);
        QCOMPARE(m_fake->countOfPathEndingWith(QStringLiteral("/liveness")), livenessCountAfterFailure);

        // WR-01: the id is cleared - a later, unrelated request carries none.
        const int beforeReload = m_fake->requestPaths().size();
        m_fake->answerMe(200, accountBody());
        client.reloadAccount();
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->requestPaths().size() > beforeReload, 15000);
        QVERIFY2(m_fake->traceparents().at(beforeReload).isEmpty(),
                 "a pairing failure must not leak its trace id onto a later request");

        client.session()->attachSession(nullptr);
        delete engine;
    }

    void aTeardownFailureClearsTheTraceIdToo()
    {
        SeatHubClient client;
        auto* engine = new FakeEngineSession;
        client.session()->attachSession(engine);
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);

        QTemporaryDir spoolDir;
        QVERIFY(spoolDir.isValid());
        QMutex captureMutex;
        QList<ShippedLine> captured;
        LogShipper::instance().setSpoolDirectoryForTests(spoolDir.path());
        LogShipper::instance().start([&captureMutex, &captured](const ShippedLine& line) {
            QMutexLocker lock(&captureMutex);
            captured.append(line);
            return true;
        });
        LogShipper::instance().setCanShip(true);
        StopShipperOnScopeExit stopGuard;

        // `beginSession()` skips `beginPlayRequest()` (the teardown tests' own established
        // pattern), so it mints no trace id of its own - set one directly, standing in for the
        // Play that would have minted it in production.
        client.controlPlane()->setTraceId(QStringLiteral("fedcbafedcbafedcbafedcbafedcbafe"));
        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));
        client.beginSession(QStringLiteral("aaaaaaaa-2222-2222-2222-222222222222"));
        emit engine->connectionStarted();
        QVERIFY2(!client.attachedSessionId().isEmpty(), "a session that streamed stays attached");

        m_fake->answerEnd(401);
        QSignalSpy failed(client.teardown(), &TeardownController::teardownFailed);
        emit engine->readyForDeletion();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 15000);
        const auto teardownDiagnosticCount = [&captureMutex, &captured]() {
            QMutexLocker lock(&captureMutex);
            return std::count_if(captured.cbegin(), captured.cend(),
                [](const ShippedLine& line) {
                    return line.body == QLatin1String("play.step")
                        && line.attrs.value(QStringLiteral("step")).toString()
                            == QLatin1String("teardown");
                });
        };
        QTRY_COMPARE_WITH_TIMEOUT(teardownDiagnosticCount(), 1, 15000);
        LogShipper::instance().stop();
        QMutexLocker captureLock(&captureMutex);
        const auto teardownRecord = std::find_if(captured.cbegin(), captured.cend(),
            [](const ShippedLine& line) {
                return line.body == QLatin1String("play.step")
                    && line.attrs.value(QStringLiteral("step")).toString()
                        == QLatin1String("teardown");
            });
        QVERIFY(teardownRecord != captured.cend());
        QCOMPARE(teardownRecord->attrs.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("failed_auth"));
        QCOMPARE(std::count_if(captured.cbegin(), captured.cend(),
            [](const ShippedLine& line) {
                return line.body == QLatin1String("play.step")
                    && line.attrs.value(QStringLiteral("step")).toString()
                        == QLatin1String("teardown");
            }), 1);
        captureLock.unlock();

        const int beforeReload = m_fake->requestPaths().size();
        m_fake->answerMe(200, accountBody());
        client.reloadAccount();
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->requestPaths().size() > beforeReload, 15000);
        QVERIFY2(m_fake->traceparents().at(beforeReload).isEmpty(),
                 "a failed teardown must not leak its trace id onto a later request");

        client.session()->attachSession(nullptr);
        delete engine;
    }

    void signOutClearsTheTraceIdToo()
    {
        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());

        m_fake->answerPlay(402, playRefusalBody(QStringLiteral("Not enough credit."),
                                                 QStringLiteral("SH-3K2XQ1")));
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.homeStatus(), QStringLiteral("refused"), 15000);

        // The refused Play itself already clears its own id (the test above); set a fresh one
        // directly so this test is not just re-proving that one.
        client.controlPlane()->setTraceId(QStringLiteral("abcdefabcdefabcdefabcdefabcdefab"));

        client.signOut();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("signed_out"), 15000);

        m_fake->answerLogin(200, QByteArrayLiteral("{\"access_token\":\"sb_at_after_signout\"}"));
        client.signInWithPassword(QStringLiteral("lina@example.com"), QStringLiteral("a password"));
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/auth/login")), 15000);
        QVERIFY2(m_fake->traceparents().last().isEmpty(),
                 "signing out must not leave a Play's trace id on the next sign-in's own requests");
    }

    // --- CR-02: a quality report is never lost to teardown or a sign-out (06.3-REVIEW-fork.md) ----

    void aTeardownPostFailureStoresThePendingQualityReportUnderItsOwnSessionAndAccount()
    {
        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());

        auto* engine = new FakeEngineSession;
        client.session()->attachSession(engine);
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));

        client.beginSession(QStringLiteral("aaaaaaaa-3333-3333-3333-333333333333"));
        emit engine->connectionStarted();
        QVERIFY2(!client.attachedSessionId().isEmpty(), "a session that streamed stays attached");

        emitStatsBlock(42.0);

        // The exact CR-02 scenario: a dropped connection at stream end.
        m_fake->answerEnd(0);
        QSignalSpy failed(client.teardown(), &TeardownController::teardownFailed);
        emit engine->readyForDeletion();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 15000);

        const QString outboxPath =
            QDir(m_dir->path()).filePath(QStringLiteral("quality-outbox/aaaaaaaa-3333-3333-3333-333333333333.json"));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(outboxPath), 15000);

        QFile file(outboxPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject envelope = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(envelope.value(QStringLiteral("account_id")).toString(),
                 QStringLiteral("6f1c6f5e-3a1e-4b1e-9f2e-0f1a2b3c4d5e"));
        QCOMPARE(envelope.value(QStringLiteral("report")).toObject()
                     .value(QStringLiteral("rendered_fps")).toDouble(),
                 42.0);
        // Never attempted a second POST for a connection that just failed to reach the control
        // plane once already.
        QVERIFY2(!m_fake->requestPaths().contains(QStringLiteral("/api/sessions/aaaaaaaa-3333-3333-3333-333333333333/quality")),
                 "handleTeardownFailed() must store straight to the outbox, not post again");

        client.session()->attachSession(nullptr);
        delete engine;
    }

    void signOutWithAPendingQualityReportStoresItUnderTheSigningOutAccount()
    {
        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());

        auto* engine = new FakeEngineSession;
        client.session()->attachSession(engine);
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));

        client.beginSession(QStringLiteral("aaaaaaaa-4444-4444-4444-444444444444"));
        emit engine->connectionStarted();
        QVERIFY2(!client.attachedSessionId().isEmpty(), "a session that streamed stays attached");

        emitStatsBlock(55.0);

        // Signed out before teardown ever ran (before the engine's own readyForDeletion arrived) -
        // the report is still only held in memory at this point.
        client.signOut();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("signed_out"), 15000);

        const QString outboxPath =
            QDir(m_dir->path()).filePath(QStringLiteral("quality-outbox/aaaaaaaa-4444-4444-4444-444444444444.json"));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(outboxPath), 15000);
        QFile file(outboxPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject envelope = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(envelope.value(QStringLiteral("account_id")).toString(),
                 QStringLiteral("6f1c6f5e-3a1e-4b1e-9f2e-0f1a2b3c4d5e"));

        client.session()->attachSession(nullptr);
        delete engine;
    }

    // --- CR-03: a drain in flight is never sent under a different account's token -----------------

    void aSignOutMidDrainNeverSendsTheRemainingFileUnderTheNextAccountsToken()
    {
        SeatHubClient client;
        isolateStore(client);
        client.controlPlane()->setBaseUrl(QStringLiteral("https://control.invalid"));
        armControlPlane(client);

        const QString accountA = QStringLiteral("6f1c6f5e-3a1e-4b1e-9f2e-0f1a2b3c4d5e"); // accountBody()
        const QString accountB = QStringLiteral("77777777-7777-7777-7777-777777777777");
        const QString sessionA1 = QStringLiteral("11111111-1111-1111-1111-111111111111");
        const QString sessionA2 = QStringLiteral("22222222-2222-2222-2222-222222222222");
        const QString sessionB1 = QStringLiteral("33333333-3333-3333-3333-333333333333");

        QDir(m_dir->path()).mkpath(QStringLiteral("quality-outbox"));
        auto writeOutboxFile = [&](const QString& sessionId, const QString& accountId) {
            QJsonObject envelope;
            envelope.insert(QStringLiteral("account_id"), accountId);
            QJsonObject report;
            report.insert(QStringLiteral("rendered_fps"), 30.0);
            envelope.insert(QStringLiteral("report"), report);
            QFile file(QDir(m_dir->path())
                           .filePath(QStringLiteral("quality-outbox/%1.json").arg(sessionId)));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
        };
        writeOutboxFile(sessionA1, accountA);
        writeOutboxFile(sessionA2, accountA);
        writeOutboxFile(sessionB1, accountB);

        // Held open long enough to sign out and sign back in as a different account while the
        // first file's own send is still in flight.
        m_fake->answerQuality(200);
        m_fake->delayQuality(2000);

        m_fake->answerMe(200, accountBody());
        m_fake->answerWallet(200, walletBody(90));
        QVERIFY(client.credentialStore()->storeToken(TokenStore::accessTokenName(),
                                                      QString::fromLatin1(kAccessToken)));
        client.restoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);

        // The first file's send is in flight (held open by delayQuality); the second must not have
        // been attempted yet.
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(
                QStringLiteral("/api/sessions/%1/quality").arg(sessionA1)),
            15000);
        QVERIFY2(!m_fake->requestPaths().contains(
                     QStringLiteral("/api/sessions/%1/quality").arg(sessionA2)),
                 "the second file must not be sent while the first is still in flight");

        client.signOut();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("signed_out"), 15000);

        // Sign in as B while A's held-open reply is still pending.
        QByteArray bAccountBody = QByteArrayLiteral(
            "{\"id\":\"77777777-7777-7777-7777-777777777777\",\"display_name\":\"Omar\","
            "\"username\":\"omar\",\"email\":\"omar@example.com\",\"phone_e164\":\"+962790000001\","
            "\"role\":\"customer\",\"signup_stage\":\"complete\",\"email_verified\":true,"
            "\"phone_verified\":true,\"created_at\":\"2026-09-01T00:00:00Z\"}");
        m_fake->answerMe(200, bAccountBody);
        m_fake->answerWallet(200, walletBody(45));
        m_fake->answerLogin(200, QByteArrayLiteral("{\"access_token\":\"sb_at_for_b\"}"));
        client.signInWithPassword(QStringLiteral("omar@example.com"), QStringLiteral("irrelevant"));
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);

        // B's own queued report is sent during B's sign-in.
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(
                QStringLiteral("/api/sessions/%1/quality").arg(sessionB1)),
            15000);
        QCOMPARE(m_fake->authorizationFor(QStringLiteral("/api/sessions/%1/quality").arg(sessionB1)),
                 QByteArrayLiteral("Bearer sb_at_for_b"));

        // A's held-open reply finally lands; give the event loop time to process it.
        QTest::qWait(2200);

        // CR-03's own guarantee: A's second (never-yet-attempted) file was never POSTed at all
        // once a different account had signed in - neither under A's stale token (the drain that
        // queued it aborted, via the epoch check, without ever calling postFn for it) nor under
        // B's (IN-05's own, separately-decided design deletes a foreign-tagged file locally,
        // before it ever reaches postFn, the moment B's own drain scans across it - see the WARN
        // line this produces). Either way, no HTTP request for this session ever went out under
        // the wrong account's credential, which is what T-06.3-51 asks for.
        QVERIFY2(!m_fake->requestPaths().contains(
                     QStringLiteral("/api/sessions/%1/quality").arg(sessionA2)),
                 "A's remaining file must never be sent once a different account has signed in");
        // Likewise A's first (in-flight) file: its own reply, once it lands, is treated as
        // Outcome::Aborted by the stale generation check rather than acted on - it must not have
        // been re-sent a second time under B's token either.
        QCOMPARE(m_fake->requestPaths().count(
                     QStringLiteral("/api/sessions/%1/quality").arg(sessionA1)),
                 1);
    }

    // --- WR-06: a fresh sign-in's own first session is never dropped for lack of an account id ----

    void aFreshSignInsFirstSessionQualityReportIsStoredUnderTheRealAccountId()
    {
        SeatHubClient client;
        isolateStore(client);
        client.controlPlane()->setBaseUrl(QStringLiteral("https://control.invalid"));
        armControlPlane(client);
        m_fake->answerMe(200, accountBody());
        m_fake->answerWallet(200, walletBody(90));

        // A fresh sign-in, first time - the outbox is empty, so the pre-WR-06 code never even
        // asked for the account id.
        m_fake->answerLogin(200, QByteArrayLiteral("{\"access_token\":\"sb_at_for_first\"}"));
        client.signInWithPassword(QStringLiteral("+962790000000"), QStringLiteral("a password"),
                                  QStringLiteral("JO"));
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->requestPaths().contains(QStringLiteral("/api/me")), 15000);

        auto* engine = new FakeEngineSession;
        client.session()->attachSession(engine);
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));
        client.beginSession(QStringLiteral("aaaaaaaa-5555-5555-5555-555555555555"));
        emit engine->connectionStarted();
        QVERIFY2(!client.attachedSessionId().isEmpty(), "a session that streamed stays attached");

        emitStatsBlock(48.0);
        m_fake->answerQuality(503); // Retryable - kept, never dropped
        QSignalSpy completed(client.teardown(), &TeardownController::teardownCompleted);
        emit engine->readyForDeletion();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 15000);

        const QString outboxPath =
            QDir(m_dir->path()).filePath(QStringLiteral("quality-outbox/aaaaaaaa-5555-5555-5555-555555555555.json"));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(outboxPath), 15000);
        QFile file(outboxPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject envelope = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(envelope.value(QStringLiteral("account_id")).toString(),
                 QStringLiteral("6f1c6f5e-3a1e-4b1e-9f2e-0f1a2b3c4d5e"));

        client.session()->attachSession(nullptr);
        delete engine;
    }

    // --- CR-04 (code review 06.3-REVIEW-fork.md, second review pass): a late reply to an
    // untagged report is never re-tagged under whichever epoch happens to be current when it
    // lands -----------------------------------------------------------------------------------

    // The WR-06 fix's own regression: A's `/api/me` fails at sign-in (`m_accountId` stays empty
    // for A's whole session, WR-06's own held-not-dropped path), A's `/quality` POST stalls past
    // both a sign-out AND a different account's sign-in, and only THEN lands. Before this fix the
    // stalled reply's handler read `m_authEpoch` at REPLY time (whatever is current then, B's),
    // not at the moment the report was actually posted (A's own, already gone) - so a later
    // `reloadAccount()` tagged and sent A's report as B's, under B's bearer token, reaching the
    // same threat (T-06.3-51) CR-03 already closed for the outbox's own drain, by a different
    // path. RED on the pre-fix code: this test's own `2>1` occurrence count and outbox-existence
    // assertions below both fail on `2cf61ab4` (a second POST for A's session, under B's token,
    // and an outbox file for A's session tagged `account_id: B`) and pass once the epoch is
    // captured at post time instead.
    void aLateReplyToAnUntaggedReportIsDroppedNeverReTaggedUnderALaterAccount()
    {
        SeatHubClient client;
        isolateStore(client);
        client.controlPlane()->setBaseUrl(QStringLiteral("https://control.invalid"));
        armControlPlane(client);

        const QString sessionA = QStringLiteral("aaaaaaaa-7777-7777-7777-777777777777");
        const QString qualityPathA =
            QStringLiteral("/api/sessions/%1/quality").arg(sessionA);

        // A signs in fresh; `/api/me` (the `fetchMe` `adoptSignIn()` always runs, WR-06) fails,
        // so `m_accountId` stays empty for the whole of A's session - CR-04's own precondition
        // (the finding's own "Failed /api/me at sign-in" case).
        m_fake->answerMe(503, QByteArray());
        m_fake->answerWallet(200, walletBody(90));
        m_fake->answerLogin(200, QByteArrayLiteral("{\"access_token\":\"sb_at_for_a\"}"));
        client.signInWithPassword(QStringLiteral("+962790000002"), QStringLiteral("a password"),
                                  QStringLiteral("JO"));
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->requestPaths().contains(QStringLiteral("/api/me")), 15000);

        auto* engine = new FakeEngineSession;
        client.session()->attachSession(engine);
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));
        client.beginSession(sessionA);
        emit engine->connectionStarted();
        QVERIFY2(!client.attachedSessionId().isEmpty(), "a session that streamed stays attached");

        emitStatsBlock(48.0);

        // The reply stays in flight (delayed) well past the sign-out and the next sign-in below -
        // exactly the window CR-04's own finding names ("the time A's `/quality` request stays in
        // flight ... a stalled POST can outlast a sign-out and a sign-in").
        m_fake->answerQuality(503); // Retryable once it finally answers
        m_fake->delayQuality(2500);
        QSignalSpy completed(client.teardown(), &TeardownController::teardownCompleted);
        emit engine->readyForDeletion();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 15000);

        // The POST itself already went out, under A's own token, before any of what follows.
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->requestPaths().contains(qualityPathA), 15000);
        QCOMPARE(m_fake->requestPaths().count(qualityPathA), 1);

        client.signOut();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("signed_out"), 15000);

        // B signs in - a distinct, distinguishable bearer token (unlike OTP verify, which always
        // answers the same fixed token - see `FakeControlPlane::createRequest()`), the same way
        // `aSignOutMidDrainNeverSendsTheRemainingFileUnderTheNextAccountsToken` (CR-03) does.
        QByteArray bAccountBody = QByteArrayLiteral(
            "{\"id\":\"77777777-7777-7777-7777-777777777777\",\"display_name\":\"Omar\","
            "\"username\":\"omar\",\"email\":\"omar@example.com\",\"phone_e164\":\"+962790000001\","
            "\"role\":\"customer\",\"signup_stage\":\"complete\",\"email_verified\":true,"
            "\"phone_verified\":true,\"created_at\":\"2026-09-01T00:00:00Z\"}");
        m_fake->answerMe(200, bAccountBody);
        m_fake->answerWallet(200, walletBody(45));
        m_fake->answerLogin(200, QByteArrayLiteral("{\"access_token\":\"sb_at_for_b\"}"));
        client.signInWithPassword(QStringLiteral("omar@example.com"), QStringLiteral("irrelevant"));
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->requestPaths().count(QStringLiteral("/api/me")) >= 2, 15000);
        // B's own `fetchMe` reply is undelayed; give its callback a moment to fill `m_accountId`
        // before A's own delayed reply (below) lands.
        QTest::qWait(200);

        // A's held-open reply finally lands, well after both the sign-out and B's own sign-in.
        QTest::qWait(2700);

        // The trigger the finding's own fix steps name: a later account read that flushes
        // whatever is still held in `m_untaggedQualityReports`.
        client.reloadAccount();
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->requestPaths().count(QStringLiteral("/api/me")) >= 3, 15000);
        // Give `setAccount()`'s own `flushUntaggedQualityReports()`/`drainQualityOutbox()` call a
        // moment to run (or, pre-fix, to write and send the mis-tagged file).
        QTest::qWait(300);

        // CR-04's own guarantee: A's report is dropped once the epoch that posted it is no
        // longer current - never re-tagged and sent as B's. Exactly one attempt for A's session
        // ever happened (the original, under A's own token); no second one, under B's, followed
        // the reload.
        QCOMPARE(m_fake->requestPaths().count(qualityPathA), 1);
        QVERIFY2(m_fake->authorizationFor(qualityPathA) != QByteArrayLiteral("Bearer sb_at_for_b"),
                 "A's session must never be sent under B's bearer token");

        const QString outboxPathA =
            QDir(m_dir->path()).filePath(QStringLiteral("quality-outbox/%1.json").arg(sessionA));
        QVERIFY2(!QFile::exists(outboxPathA),
                 "a dropped, never-re-tagged report must not be written to the outbox under a "
                 "later account either");

        client.session()->attachSession(nullptr);
        delete engine;
    }

    // --- WR-04: a decoder recreation mid-session aggregates, rather than overwrites ----------------

    void aDecoderRecreationMidSessionAggregatesBothSegmentsRatherThanReportingOnlyTheLast()
    {
        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());

        auto* engine = new FakeEngineSession;
        client.session()->attachSession(engine);
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));
        client.beginSession(QStringLiteral("aaaaaaaa-6666-6666-6666-666666666666"));

        // The moment the stream truly begins - starts the aggregator's clock, and (C1/C5) is what
        // makes this session live.
        emit client.session()->connectionStarted();
        QVERIFY2(!client.attachedSessionId().isEmpty(), "a session that streamed stays attached");

        QThread::msleep(50);
        emitStatsBlock(30.0); // segment 1 (a decoder that ran ~50 ms)
        QThread::msleep(150);
        emitStatsBlock(60.0); // segment 2, after a fullscreen toggle recreated the decoder

        QSignalSpy completed(client.teardown(), &TeardownController::teardownCompleted);
        emit engine->readyForDeletion();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 15000);

        // The quality report is its own request and may reach the fake after the teardown completes,
        // so wait for a body that carries the measurement before reading it: read too early, the
        // frame rate parses as 0 under load.
        const QString qualityPath =
            QStringLiteral("/api/sessions/aaaaaaaa-6666-6666-6666-666666666666/quality");
        QTRY_VERIFY_WITH_TIMEOUT(
            QJsonDocument::fromJson(m_fake->bodyFor(qualityPath)).object().contains(
                QStringLiteral("rendered_fps")),
            15000);
        const QByteArray body = m_fake->bodyFor(qualityPath);
        const double renderedFps =
            QJsonDocument::fromJson(body).object().value(QStringLiteral("rendered_fps")).toDouble();
        QVERIFY2(renderedFps > 30.0 && renderedFps < 60.0,
                 qPrintable(QStringLiteral(
                                "rendered_fps was %1, expected strictly between 30 and 60 - "
                                "today's code posts exactly the last segment's 60")
                                .arg(renderedFps)));

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

    void aServerEndedFailureStillPostsOneEndWithTheStep()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());

        QTemporaryDir spoolDir;
        QVERIFY(spoolDir.isValid());
        QMutex captureMutex;
        QList<ShippedLine> captured;
        LogShipper::instance().setSpoolDirectoryForTests(spoolDir.path());
        LogShipper::instance().start([&captureMutex, &captured](const ShippedLine& line) {
            QMutexLocker lock(&captureMutex);
            captured.append(line);
            return true;
        });
        LogShipper::instance().setCanShip(true);
        StopShipperOnScopeExit stopGuard;

        report(client, sessionIn(QStringLiteral("PREPARING")));
        client.teardown()->setVerifyIntervalMs(1);

        SessionInfo over = sessionIn(QStringLiteral("FAILED"));
        over.endReason = QStringLiteral("READINESS_TIMEOUT");
        report(client, over);

        QTRY_VERIFY_WITH_TIMEOUT(
            std::any_of(captured.cbegin(), captured.cend(), [](const ShippedLine& line) {
                return line.body == QLatin1String("play.step");
            }), 15000);

        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-stages/end")), 15000);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-stages/end")), 1);
        const QJsonObject body = QJsonDocument::fromJson(
            m_fake->bodyFor(QStringLiteral("/api/sessions/s-stages/end"))).object();
        QCOMPARE(body.value(QStringLiteral("failed")).toBool(), true);
        QCOMPARE(body.value(QStringLiteral("attempt_step")).toString(), QStringLiteral("rig_wait"));

        LogShipper::instance().stop();
        QMutexLocker lock(&captureMutex);
        int playSteps = 0;
        const ShippedLine* record = nullptr;
        for (const ShippedLine& line : captured) {
            if (line.body == QLatin1String("play.step")) {
                ++playSteps;
                record = &line;
            }
        }
        QCOMPARE(playSteps, 1);
        QVERIFY(record != nullptr);
        QCOMPARE(record->attrs.value(QStringLiteral("step")).toString(),
                 QStringLiteral("rig_wait"));
        QCOMPARE(record->attrs.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("server_ended"));
        QCOMPARE(record->attrs.value(QStringLiteral("end_reason")).toString(),
                 QStringLiteral("READINESS_TIMEOUT"));
        lock.unlock();

        report(client, over);
        QTest::qWait(25);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-stages/end")), 1);
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
        QTest::newRow("the customer's new Play replaced it, one minute")
            << "READY" << "COMPLETED" << "REPLACED_BY_PLAY" << 1 << stream
            << "Ended by your new Play. You were charged for 1 minute."
            << QStringLiteral("Ended by your new Play. You were charged for %1 minute.")
                   .arg(mono.arg(1));
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

        QTemporaryDir spoolDir;
        QVERIFY(spoolDir.isValid());
        QMutex captureMutex;
        QList<ShippedLine> captured;
        LogShipper::instance().setSpoolDirectoryForTests(spoolDir.path());
        LogShipper::instance().start([&captureMutex, &captured](const ShippedLine& line) {
            QMutexLocker lock(&captureMutex);
            captured.append(line);
            return true;
        });
        LogShipper::instance().setCanShip(true);
        StopShipperOnScopeExit stopGuard;
        const auto playStepCount = [&captureMutex, &captured]() {
            QMutexLocker lock(&captureMutex);
            return static_cast<int>(std::count_if(captured.cbegin(), captured.cend(),
                [](const ShippedLine& line) { return line.body == QLatin1String("play.step"); }));
        };

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
        QTRY_COMPARE_WITH_TIMEOUT(playStepCount(), 1, 15000);
        QCOMPARE(client.appState(), QStringLiteral("connecting"));
        // Nothing was read about the session, so it stopped at the stage every session begins at.
        QCOMPARE(client.stalledStepText(), QStringLiteral("Stopped at: Preparing the rig"));
        // The client's own sentence, from the copy it already had; no reference, because none exists.
        QCOMPARE(client.stalledReasonText(), QStringLiteral("The rig didn't finish connecting. Try again."));
        QVERIFY(client.reference().isEmpty());
        // D-05/C2: the client's own pairing deadline ends the session at once - it is never left
        // for Try again to find.
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-stages/end")), 15000);
        QVERIFY(m_fake->bodyFor(QStringLiteral("/api/sessions/s-stages/end"))
                    .contains(QByteArrayLiteral("\"attempt_step\":\"pair_authorize\"")));
        LogShipper::instance().stop();
        QMutexLocker lock(&captureMutex);
        const auto record = std::find_if(captured.cbegin(), captured.cend(),
            [](const ShippedLine& line) { return line.body == QLatin1String("play.step"); });
        QVERIFY(record != captured.cend());
        QCOMPARE(record->attrs.value(QStringLiteral("step")).toString(),
                 QStringLiteral("pair_authorize"));
        QCOMPARE(record->attrs.value(QStringLiteral("outcome")).toString(), QStringLiteral("failed"));
        QCOMPARE(record->attrs.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("unreachable_deadline"));
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

    // A-68 / D-06 reversal, contract 3.3.0 (ADR-0064): a Play sends no quality field at all, and
    // an authorization that carries one anyway is parsed and ignored - the customer's saved
    // Settings are the only decider of stream quality.
    void playSendsNoQualityAndAppliesNone()
    {
        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());

        // The customer's saved Settings, deliberately not any of "1080p120"'s own values, so an
        // accidental application would be unmistakable.
        QVERIFY(client.settings()->setValue(QStringLiteral("width"), 2560));
        QVERIFY(client.settings()->setValue(QStringLiteral("height"), 1440));
        QVERIFY(client.settings()->setValue(QStringLiteral("fps"), 30));

        // An authorization that carries a quality profile - parsed, and never applied. `pairing_pin`
        // is left absent so the controller keeps polling rather than reaching for an engine seam
        // this test never sets up (matching `theClientsOwnPairingDeadlineIsAStallToo`'s own
        // technique for the same reason).
        QJsonObject authBody;
        authBody.insert(QStringLiteral("session_id"), QStringLiteral("s-quality"));
        authBody.insert(QStringLiteral("quality_profile"), QStringLiteral("1080p120"));
        m_fake->answerPairing(200, QJsonDocument(authBody).toJson(QJsonDocument::Compact));
        m_fake->answerPlay(201, QByteArrayLiteral("{\"id\":\"s-quality\"}"));

        client.start();

        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions")), 15000);
        QCOMPARE(m_fake->bodyFor(QStringLiteral("/api/sessions")), QByteArrayLiteral("{}"));

        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-quality/pairing")), 15000);

        // The saved values stand, exactly as set - nothing applied "1080p120".
        QCOMPARE(client.settings()->getValue(QStringLiteral("width")).toInt(), 2560);
        QCOMPARE(client.settings()->getValue(QStringLiteral("height")).toInt(), 1440);
        QCOMPARE(client.settings()->getValue(QStringLiteral("fps")).toInt(), 30);
    }

    // Plan 09 correction, ADR-0072 item 2 / `docs/spec/client.md` "Play diagnostics - Rules": a
    // pairing that succeeds only after the rig's 409 was cleared emits ONE `play.step` at INFO
    // with `step=pair_handshake outcome=ok failure_class=in_progress attempt=2` - produced here by
    // the real production route (the seam's clear-and-retry, the controller's completion, the
    // facade's own emission into the shipper), never constructed by the test.
    void aRecoveredPairingEmitsExactlyOneInfoPlayStepWithTheFrozenRecord()
    {
        g_recoveredHandshakeArmed = true;
        g_handshakeCalls = 0;
        auto disarm = qScopeGuard([]() {
            g_recoveredHandshakeArmed = false;
            g_handshakeCalls = 0;
        });

        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());

        auto engine = std::make_unique<FakeEngineSession>();
        g_engineCreateReason = EngineLaunchReason::Started;
        g_engineFactoryHook = [&client, enginePtr = engine.get()]() {
            client.session()->attachSession(enginePtr);
        };
        auto clearFactoryHook = qScopeGuard([]() {
            g_engineFactoryHook = {};
            g_engineCreateReason = EngineLaunchReason::CreateFailed;
        });

        // This suite never installs the LogTee and never calls `SeatHubTelemetry::start()`, so a
        // shipper started here sees exactly one kind of line: the diagnostics the facade itself
        // emits. Started AFTER `reachHome()`, whose identity changes run `updateCanShip()` -
        // `setCanShip(true)` afterwards is what stands for the rest of this test.
        QTemporaryDir spoolDir;
        QVERIFY(spoolDir.isValid());
        QMutex captureMutex;
        QList<ShippedLine> captured;
        LogShipper::instance().setSpoolDirectoryForTests(spoolDir.path());
        LogShipper::instance().start([&captureMutex, &captured](const ShippedLine& line) {
            QMutexLocker lock(&captureMutex);
            captured.append(line);
            return true;
        });
        LogShipper::instance().setCanShip(true);
        StopShipperOnScopeExit stopGuard;

        const auto capturedBodyCount = [&](const QString& body) {
            QMutexLocker lock(&captureMutex);
            int count = 0;
            for (const ShippedLine& line : captured) {
                if (line.body == body) {
                    ++count;
                }
            }
            return count;
        };

        // A real authorization, so the controller reaches the production seam - whose first
        // handshake run is the rig's 409 (see the armed stub above). 127.0.0.1 keeps the seam's
        // rig-side clear on the loopback, where nothing answers and the call returns at once.
        QJsonObject ports;
        ports.insert(QStringLiteral("https"), 47984);
        ports.insert(QStringLiteral("control"), 47989);
        ports.insert(QStringLiteral("rtsp"), 48010);
        QJsonObject authBody;
        authBody.insert(QStringLiteral("session_id"), QStringLiteral("s-recovered"));
        authBody.insert(QStringLiteral("pairing_pin"), QStringLiteral("4821"));
        authBody.insert(QStringLiteral("host_address"), QStringLiteral("127.0.0.1"));
        authBody.insert(QStringLiteral("ports"), ports);
        authBody.insert(QStringLiteral("state"), QStringLiteral("READY"));
        m_fake->answerPairing(200, QJsonDocument(authBody).toJson(QJsonDocument::Compact));
        m_fake->answerPlay(201, QByteArrayLiteral("{\"id\":\"s-recovered\"}"));

        client.beginSession(QStringLiteral("s-recovered"));

        // The record arrives through the real route.
        QTRY_VERIFY_WITH_TIMEOUT(capturedBodyCount(QStringLiteral("play.step")) == 1, 20000);

        // Stop the worker before reading the capture: the final assertions must not race it.
        LogShipper::instance().stop();

        // ONE line this run DIAGNOSED - `LogShipper::start()` installs the LogTee, so the
        // capture also holds this suite's ordinary local log lines, and those are not emissions
        // of the record under test.
        QMutexLocker lock(&captureMutex);
        int diagnostics = 0;
        const ShippedLine* record = nullptr;
        int playSteps = 0;
        for (const ShippedLine& line : captured) {
            if (line.diagnostic) {
                ++diagnostics;
                record = &line;
            }
            if (line.body == QLatin1String("play.step")) {
                ++playSteps;
            }
        }
        QCOMPARE(diagnostics, 1);
        QVERIFY(record != nullptr);
        QVERIFY2(!record->body.isEmpty(), "the one diagnostic must be the play.step record");

        // Exactly ONE `play.step`, and it is the ADR's frozen record - not a WARN, not a second
        // emission of any kind (the recovery makes this one line and nothing else).
        QCOMPARE(playSteps, 1);
        QCOMPARE(record->body, QStringLiteral("play.step"));
        QCOMPARE(record->level, LogLevel::Info);
        QCOMPARE(record->attrs.value(QStringLiteral("step")).toString(),
                 QStringLiteral("pair_handshake"));
        QCOMPARE(record->attrs.value(QStringLiteral("outcome")).toString(), QStringLiteral("ok"));
        QCOMPARE(record->attrs.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("in_progress"));
        QCOMPARE(record->attrs.value(QStringLiteral("attempt")).toInt(), 2);

        // No PIN, no address and no diagnostic text anywhere in what was emitted
        // (ADR-0072 item 2, T-06.7-35): the 409 sentence and the PIN stay where they were.
        const QByteArray serialised = QJsonDocument(record->attrs).toJson(QJsonDocument::Compact);
        QVERIFY2(!serialised.contains("4821"), qPrintable(QString::fromUtf8(serialised)));
        QVERIFY2(!serialised.contains("409"),
                 qPrintable(QStringLiteral("the local diagnostic leaked: %1")
                                .arg(QString::fromUtf8(serialised))));
        QVERIFY(!record->body.contains(QStringLiteral("4821")));
        lock.unlock();

        // Let the successful fake engine complete so ordinary teardown runs. The recovered
        // pairing itself must not fail the Play or add an attempt step to /end.
        emit engine->readyForDeletion();

        // Nothing extra went to the server for the recovery (ADR-0072 item 2: "nothing extra is
        // sent ... on a recovered pairing"): the successful stream's ordinary `/end` carries no
        // `attempt_step` - a step the Play did NOT fail at never travels.
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-recovered/end")), 15000);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-recovered/end")), 1);
        QVERIFY2(!m_fake->bodyFor(QStringLiteral("/api/sessions/s-recovered/end"))
                      .contains("attempt_step"),
                  "a recovered pairing must add nothing to the /end body");
    }

    void aFailedPairingRetryEmitsOneWarnWithTheLastClassAndEndsAtPairing()
    {
        g_failedRetryHandshakeArmed = true;
        g_handshakeCalls = 0;
        auto disarm = qScopeGuard([]() {
            g_failedRetryHandshakeArmed = false;
            g_handshakeCalls = 0;
        });

        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());

        QTemporaryDir spoolDir;
        QVERIFY(spoolDir.isValid());
        QMutex captureMutex;
        QList<ShippedLine> captured;
        LogShipper::instance().setSpoolDirectoryForTests(spoolDir.path());
        LogShipper::instance().start([&captureMutex, &captured](const ShippedLine& line) {
            QMutexLocker lock(&captureMutex);
            captured.append(line);
            return true;
        });
        LogShipper::instance().setCanShip(true);
        StopShipperOnScopeExit stopGuard;

        QJsonObject ports;
        ports.insert(QStringLiteral("https"), 47984);
        ports.insert(QStringLiteral("control"), 47989);
        ports.insert(QStringLiteral("rtsp"), 48010);
        QJsonObject authBody;
        authBody.insert(QStringLiteral("session_id"), QStringLiteral("s-retry-failed"));
        authBody.insert(QStringLiteral("pairing_pin"), QStringLiteral("4821"));
        authBody.insert(QStringLiteral("host_address"), QStringLiteral("127.0.0.1"));
        authBody.insert(QStringLiteral("ports"), ports);
        authBody.insert(QStringLiteral("state"), QStringLiteral("READY"));
        m_fake->answerPairing(200, QJsonDocument(authBody).toJson(QJsonDocument::Compact));
        m_fake->answerPlay(201, QByteArrayLiteral("{\"id\":\"s-retry-failed\"}"));

        client.beginSession(QStringLiteral("s-retry-failed"));
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-retry-failed/end")),
            20000);
        const auto diagnosticCount = [&captureMutex, &captured]() {
            QMutexLocker lock(&captureMutex);
            return std::count_if(captured.cbegin(), captured.cend(),
                [](const ShippedLine& line) { return line.body == QLatin1String("play.step"); });
        };
        QTRY_COMPARE_WITH_TIMEOUT(diagnosticCount(), 1, 15000);
        LogShipper::instance().stop();

        QMutexLocker lock(&captureMutex);
        const auto record = std::find_if(captured.cbegin(), captured.cend(),
            [](const ShippedLine& line) { return line.body == QLatin1String("play.step"); });
        QVERIFY(record != captured.cend());
        QCOMPARE(record->level, LogLevel::Warning);
        QCOMPARE(record->attrs.value(QStringLiteral("step")).toString(),
                 QStringLiteral("pair_handshake"));
        QCOMPARE(record->attrs.value(QStringLiteral("outcome")).toString(),
                 QStringLiteral("failed"));
        QCOMPARE(record->attrs.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("pin_rejected"));
        QCOMPARE(record->attrs.value(QStringLiteral("attempt")).toInt(), 2);
        const QByteArray attrs = QJsonDocument(record->attrs).toJson(QJsonDocument::Compact);
        QVERIFY(!attrs.contains("4821"));
        QVERIFY(!attrs.contains("fixture retry rejection text"));
        const QJsonObject endBody = QJsonDocument::fromJson(
            m_fake->bodyFor(QStringLiteral("/api/sessions/s-retry-failed/end"))).object();
        QCOMPARE(endBody.value(QStringLiteral("failed")).toBool(), true);
        QCOMPARE(endBody.value(QStringLiteral("attempt_step")).toString(),
                 QStringLiteral("pair_handshake"));
    }

    // T093 (SC18/SC13): a first pairing failure and a retry that is still blocked when the customer
    // cancels reach the evidence capsule BEFORE the Play closes it, on every production path that
    // cancels pairing. The seam is the real one; only upstream's handshake is the fixture above.
    void cancelBeforeJournalCloseRetainsFirstFailure_data()
    {
        QTest::addColumn<int>("path");
        QTest::newRow("customer cancel") << 0;
        QTest::newRow("sign-out") << 1;
        QTest::newRow("server-ended while connecting") << 2;
    }

    void cancelBeforeJournalCloseRetainsFirstFailure()
    {
        QFETCH(int, path);
        // 503 keeps the capsule on disk after its delivery attempt, so it can be read back.
        m_evidenceStatusAtArm = 503;
        const QString sessionId = QStringLiteral("s-cancel-journal");
        g_blockedRetryHandshakeArmed = true;
        g_handshakeCalls = 0;
        g_blockedRetryEntered.acquire(g_blockedRetryEntered.available());
        g_blockedRetryGate.acquire(g_blockedRetryGate.available());
        auto disarm = qScopeGuard([]() {
            g_blockedRetryHandshakeArmed = false;
            g_handshakeCalls = 0;
        });
        // Declared before the client, so it runs after the client is gone: a replacement Play's
        // worker must not outlive this row and reach the next row's armed fixture.
        auto drainWorkers = qScopeGuard([]() { QThreadPool::globalInstance()->waitForDone(30000); });

        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());
        // Declared after the client, so the blocked worker is freed before the client is destroyed.
        auto freeWorker = qScopeGuard([]() { g_blockedRetryGate.release(10); });

        QJsonObject ports;
        ports.insert(QStringLiteral("https"), 47984);
        ports.insert(QStringLiteral("control"), 47989);
        ports.insert(QStringLiteral("rtsp"), 48010);
        QJsonObject authBody;
        authBody.insert(QStringLiteral("session_id"), sessionId);
        authBody.insert(QStringLiteral("pairing_pin"), QStringLiteral("4821"));
        authBody.insert(QStringLiteral("host_address"), QStringLiteral("127.0.0.1"));
        authBody.insert(QStringLiteral("ports"), ports);
        authBody.insert(QStringLiteral("state"), QStringLiteral("READY"));
        m_fake->answerPairing(200, QJsonDocument(authBody).toJson(QJsonDocument::Compact));
        m_fake->answerPlay(201, QByteArrayLiteral("{\"id\":\"s-cancel-journal\"}"));

        client.beginSession(sessionId);
        QVERIFY2(g_blockedRetryEntered.tryAcquire(1, 20000), "the retry must be entered and blocked");
        QVERIFY(evidenceEntriesFor(sessionId, QStringLiteral("pairing_attempt")).isEmpty());

        switch (path) {
        case 0: client.interrupt(); break;
        case 1: client.signOut(); break;
        case 2: {
            SessionInfo over = sessionIn(QStringLiteral("COMPLETED"), sessionId);
            over.endReason = QStringLiteral("CUSTOMER_ENDED");
            report(client, over);
            break;
        }
        }

        // The cancel has returned, and the capsule the Play closed already holds the first failure.
        const auto assertFirstFailureKeptOnce = [&]() {
            const QList<QJsonObject> attempts =
                evidenceEntriesFor(sessionId, QStringLiteral("pairing_attempt"));
            int first = 0;
            for (const QJsonObject& entry : attempts) {
                if (entry.value(QStringLiteral("attempt")).toInt() != 1) continue;
                ++first;
                QCOMPARE(entry.value(QStringLiteral("outcome")).toString(), QStringLiteral("failure"));
                QCOMPARE(entry.value(QStringLiteral("engine_step")).toString(),
                         QStringLiteral("pair_server_info"));
                QCOMPARE(entry.value(QStringLiteral("engine_class")).toString(),
                         QStringLiteral("http_4xx"));
                QVERIFY(entry.value(QStringLiteral("retried")).toBool());
                QVERIFY(!entry.value(QStringLiteral("retry_reason")).toString().isEmpty());
            }
            // The cancelled retry never finished, so the first failure is the whole record: once,
            // and no invented second attempt.
            QCOMPARE(first, 1);
            QCOMPARE(attempts.size(), 1);
        };
        assertFirstFailureKeptOnce();
        if (QTest::currentTestFailed()) return;

        // The same record went out with the capsule's delivery.
        const QString evidencePath = QStringLiteral("/api/sessions/%1/evidence").arg(sessionId);
        QTRY_VERIFY_WITH_TIMEOUT(!m_fake->bodiesFor(evidencePath).isEmpty(), 15000);
        QVERIFY(m_fake->bodiesFor(evidencePath).last().contains("http_4xx"));

        // The visible result stays neutral: Home (or the signed-out state), and no retry or reconnect.
        if (path == 1) {
            QVERIFY(!client.signedIn());
        } else {
            QCOMPARE(client.appState(), QStringLiteral("home"));
        }

        // The late worker finishes: nothing more reaches the record, and it never retries again.
        g_blockedRetryGate.release();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(15000));
        QTest::qWait(100);
        QCOMPARE(g_handshakeCalls, 2);
        assertFirstFailureKeptOnce();
        if (QTest::currentTestFailed()) return;

        // A replacement Play does not touch the closed capsule either.
        if (path != 1) {
            g_blockedRetryHandshakeArmed = false;
            client.beginSession(QStringLiteral("s-cancel-journal-2"));
            QTest::qWait(200);
            assertFirstFailureKeptOnce();
            QVERIFY(evidenceEntriesFor(QStringLiteral("s-cancel-journal-2"),
                                       QStringLiteral("pairing_attempt")).size() <= 1);
        }
    }

    void aNoLaunchableAppEmitsOneClassifiedStepAndEndsWithThatStep()
    {
        g_noAppHandshakeArmed = true;
        g_engineCreateReason = EngineLaunchReason::NoApp;
        auto disarm = qScopeGuard([]() {
            g_noAppHandshakeArmed = false;
            g_engineCreateReason = EngineLaunchReason::CreateFailed;
        });

        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());

        QTemporaryDir spoolDir;
        QVERIFY(spoolDir.isValid());
        QMutex captureMutex;
        QList<ShippedLine> captured;
        LogShipper::instance().setSpoolDirectoryForTests(spoolDir.path());
        LogShipper::instance().start([&captureMutex, &captured](const ShippedLine& line) {
            QMutexLocker lock(&captureMutex);
            captured.append(line);
            return true;
        });
        LogShipper::instance().setCanShip(true);
        StopShipperOnScopeExit stopGuard;

        QJsonObject ports;
        ports.insert(QStringLiteral("https"), 47984);
        ports.insert(QStringLiteral("control"), 47989);
        ports.insert(QStringLiteral("rtsp"), 48010);
        QJsonObject authBody;
        authBody.insert(QStringLiteral("session_id"), QStringLiteral("s-no-app"));
        authBody.insert(QStringLiteral("pairing_pin"), QStringLiteral("4821"));
        authBody.insert(QStringLiteral("host_address"), QStringLiteral("127.0.0.1"));
        authBody.insert(QStringLiteral("ports"), ports);
        authBody.insert(QStringLiteral("state"), QStringLiteral("READY"));
        m_fake->answerPairing(200, QJsonDocument(authBody).toJson(QJsonDocument::Compact));
        m_fake->answerPlay(201, QByteArrayLiteral("{\"id\":\"s-no-app\"}"));

        client.beginSession(QStringLiteral("s-no-app"));

        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-no-app/end")),
            20000);
        LogShipper::instance().stop();

        QMutexLocker lock(&captureMutex);
        int diagnostics = 0;
        int playSteps = 0;
        int summaries = 0;
        const ShippedLine* record = nullptr;
        for (const ShippedLine& line : captured) {
            if (line.diagnostic) {
                ++diagnostics;
            }
            if (line.body == QLatin1String("play.step")) {
                ++playSteps;
                record = &line;
            }
            if (line.body == QLatin1String("play.summary")) {
                ++summaries;
                QCOMPARE(line.attrs.value("exit_path").toString(), QStringLiteral("start_refused"));
                QCOMPARE(line.attrs.value("failure_class").toString(), QStringLiteral("no_app"));
            }
        }
        QCOMPARE(diagnostics, 2);
        QCOMPARE(playSteps, 1);
        QCOMPARE(summaries, 1);
        QVERIFY(record != nullptr);
        QCOMPARE(record->level, LogLevel::Warning);
        QCOMPARE(record->attrs.value(QStringLiteral("step")).toString(),
                 QStringLiteral("engine_prepare"));
        QCOMPARE(record->attrs.value(QStringLiteral("outcome")).toString(),
                 QStringLiteral("failed"));
        QCOMPARE(record->attrs.value(QStringLiteral("failure_class")).toString(),
                 QStringLiteral("no_app"));
        lock.unlock();

        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-no-app/end")),
            15000);
        const QJsonObject endBody = QJsonDocument::fromJson(
            m_fake->bodyFor(QStringLiteral("/api/sessions/s-no-app/end"))).object();
        QCOMPARE(endBody.value(QStringLiteral("failed")).toBool(), true);
        QCOMPARE(endBody.value(QStringLiteral("attempt_step")).toString(),
                 QStringLiteral("engine_prepare"));
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-no-app/end")), 1);
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
            // ADR-0091: the engine's own stage text is the sentence, as Moonlight composes it.
            QCOMPARE(client.stalledReasonText(), QStringLiteral("Starting RTSP handshake failed: Error -1"));

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

    // --- D-05/D-23: every pre-stream failure ends the session, once (06.6-17) ----------------------

    void pairingFailureEndsTheSession()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"));

        emit client.pairing()->pairingFailed(
            SeatHubFailure::local(QStringLiteral("The rig didn't finish connecting. Try again.")));

        QVERIFY(client.connectFailed());
        QCOMPARE(client.appState(), QStringLiteral("connecting"));

        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-stages/end")), 15000);
        // 06.1's ADR item 4 / J-22: the end body now also carries the stage this failed at -
        // `aPairingFailureReportsItsStage()` is the dedicated test for that; this only needs
        // `failed` still there, unaffected by the addition.
        const QJsonObject endBody = QJsonDocument::fromJson(
            m_fake->bodyFor(QStringLiteral("/api/sessions/s-stages/end"))).object();
        QCOMPARE(endBody.value(QStringLiteral("failed")).toBool(), true);

        // The connecting failure view stays on screen: the teardown completing does not carry the
        // customer away from what they are reading (C2's own contract).
        QTRY_VERIFY_WITH_TIMEOUT(client.attachedSessionId().isEmpty(), 15000);
        QCOMPARE(client.appState(), QStringLiteral("connecting"));
        QVERIFY(client.connectFailed());
    }

    void aNoEngineStartEndsTheSession()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"));

        // No engine is attached (`beginStagedSession` never attaches one), so pairing completing
        // finds nothing to start a stream with - the no-engine branch of `handlePairingCompleted`.
        emit client.pairing()->pairingCompleted(QStringLiteral("aa:bb:cc:dd:ee:ff"));

        QCOMPARE(client.appState(), QStringLiteral("error"));
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-stages/end")), 15000);
        // 06.1's ADR item 4 / J-22: the end body now also carries the stage this failed at
        // ("connecting" - pairing itself succeeded); `failed` is unaffected by the addition.
        const QJsonObject endBody = QJsonDocument::fromJson(
            m_fake->bodyFor(QStringLiteral("/api/sessions/s-stages/end"))).object();
        QCOMPARE(endBody.value(QStringLiteral("failed")).toBool(), true);
        QTRY_VERIFY_WITH_TIMEOUT(client.attachedSessionId().isEmpty(), 15000);
    }

    // --- 06.1's ADR item 4 / J-22: the report goes up, the blame comes back ---------------------

    void aPairingFailureReportsItsStage()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"));

        emit client.pairing()->pairingFailed(
            SeatHubFailure::local(QStringLiteral("The rig didn't finish connecting. Try again.")));

        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-stages/end")), 15000);
        const QJsonObject sent = QJsonDocument::fromJson(
            m_fake->bodyFor(QStringLiteral("/api/sessions/s-stages/end"))).object();
        QCOMPARE(sent.value(QStringLiteral("failed")).toBool(), true);
        QCOMPARE(sent.value(QStringLiteral("stage")).toString(), QStringLiteral("pairing"));
    }

    // --- T026 / E20: a pre-stream End that the network drops is retried, not lost -------------------

    void aPreStreamEndIsRetriedUntilTheServerConfirmsIt()
    {
        const QString endPath = QStringLiteral("/api/sessions/s-stages/end");
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"));
        m_fake->answerEnd(0);

        emit client.pairing()->pairingFailed(
            SeatHubFailure::local(QStringLiteral("The rig didn't finish connecting. Try again.")));

        // The blip: every attempt fails, and the End stays on disk and is posted again.
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->bodiesFor(endPath).size() >= 2, 30000);
        QVERIFY(client.hasSavedEnd());

        m_fake->answerEnd(200);
        QTRY_VERIFY_WITH_TIMEOUT(!client.hasSavedEnd(), 30000);

        // Every attempt carried the identical payload: a retry is a re-send, never a new End.
        const QList<QByteArray> bodies = m_fake->bodiesFor(endPath);
        for (const QByteArray& body : bodies) {
            QCOMPARE(body, bodies.first());
        }
        const QJsonObject sent = QJsonDocument::fromJson(bodies.first()).object();
        QCOMPARE(sent.value(QStringLiteral("failed")).toBool(), true);
        QCOMPARE(sent.value(QStringLiteral("stage")).toString(), QStringLiteral("pairing"));

        // Confirmed: nothing is posted again.
        QTest::qWait(300);
        const int confirmed = m_fake->bodiesFor(endPath).size();
        QTest::qWait(3000);
        QCOMPARE(m_fake->bodiesFor(endPath).size(), confirmed);
    }

    // --- T026 / ADR-0096: only observed evidence reaches the Play summary or the End text -----------

    void aStreamedPlayKeepsOnlyTheEvidenceItObserved_data()
    {
        QTest::addColumn<QStringList>("steps");
        QTest::addColumn<QString>("presentation");
        QTest::addColumn<bool>("manualObserved");
        QTest::addColumn<int>("nativeCode");

        constexpr int none = std::numeric_limits<int>::min();
        QTest::newRow("generic quit, nothing observed")
            << QStringList() << "unknown" << false << none;
        QTest::newRow("graceful termination code only")
            << QStringList{"code:0"} << "unknown" << false << 0;
        QTest::newRow("native no-video-traffic")
            << QStringList{"code:-100"} << "connection_lost" << false << -100;
        QTest::newRow("native early termination alone is not supported loss evidence")
            << QStringList{"code:-102"} << "unknown" << false << -102;
        QTest::newRow("native no-video-frame")
            << QStringList{"code:-101"} << "connection_lost" << false << -101;
        QTest::newRow("native fault then manual End keeps the fault")
            << QStringList{"code:-100", "manual"} << "connection_lost" << true << -100;
        QTest::newRow("native early termination then manual End stays unknown")
            << QStringList{"code:-102", "manual"} << "unknown" << true << -102;
        QTest::newRow("manual End then a later code is not a fault")
            << QStringList{"manual", "code:-102"} << "unknown" << true << none;
        QTest::newRow("manual End only")
            << QStringList{"manual"} << "unknown" << true << none;
        QTest::newRow("protected content is not a lost connection")
            << QStringList{"code:-103"} << "unknown" << false << -103;
    }

    void aStreamedPlayKeepsOnlyTheEvidenceItObserved()
    {
        QFETCH(QStringList, steps);
        QFETCH(QString, presentation);
        QFETCH(bool, manualObserved);
        QFETCH(int, nativeCode);
        constexpr int none = std::numeric_limits<int>::min();
        const QString endPath = QStringLiteral("/api/sessions/s-stages/end");

        auto* engine = new FakeEngineSession;
        {
            SeatHubClient client;
            beginStagedSession(client);
            QVERIFY(!QTest::currentTestFailed());
            client.teardown()->setVerifyIntervalMs(1);
            // Started after sign-in: the shipper is only open to a Play once the client is home.
            PlayCapture capture;
            client.session()->attachSession(engine);
            client.session()->start(nullptr);
            // The poll's own read must not end the Play before the scripted evidence arrives.
            PairingController* pairing = client.pairing();
            QMetaObject::invokeMethod(
                pairing, [pairing]() { pairing->cancel(); }, Qt::BlockingQueuedConnection);
            emit engine->connectionStarted();
            QCOMPARE(client.appState(), QStringLiteral("streaming"));

            m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("COMPLETED"),
                                  QStringLiteral("CUSTOMER_ENDED"), 15);
            for (const QString& step : steps) {
                if (step == QLatin1String("manual")) {
                    client.interrupt();
                }
                else {
                    emitConnectionTerminated(step.mid(5).toInt());
                }
            }

            QSignalSpy completed(client.teardown(), &TeardownController::teardownCompleted);
            emit engine->readyForDeletion();
            emit engine->readyForDeletion();  // repeated cleanup
            QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 15000);
            QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);
            QTRY_COMPARE_WITH_TIMEOUT(capture.named(QStringLiteral("play.summary")).size(), 1, 10000);

            const ShippedLine summary = capture.named(QStringLiteral("play.summary")).first();
            QCOMPARE(summary.attrs.value("end_presentation").toString(), presentation);
            QCOMPARE(summary.attrs.contains("manual_end_observed"), manualObserved);
            if (nativeCode == none) {
                QVERIFY(!summary.attrs.contains("native_fault_code"));
            }
            else {
                QCOMPARE(summary.attrs.value("native_fault_code").toInt(), nativeCode);
            }

            // The End the server saw is the unchanged one: no new field carries the presentation or
            // an intent, and cleanup answering 200 is not evidence of anything.
            QTRY_COMPARE_WITH_TIMEOUT(m_fake->bodiesFor(endPath).size(), 1, 15000);
            const QJsonObject sent = QJsonDocument::fromJson(m_fake->bodyFor(endPath)).object();
            QVERIFY(!sent.contains(QStringLiteral("end_presentation")));
            QVERIFY(!sent.contains(QStringLiteral("manual")));
            QVERIFY(!sent.contains(QStringLiteral("intent")));
            QCOMPARE(sent.value(QStringLiteral("failed")).toBool(), false);

            if (presentation == QLatin1String("connection_lost")) {
                QTRY_VERIFY_WITH_TIMEOUT(
                    client.endReasonText().startsWith(
                        QStringLiteral("Your connection was lost, so the session ended.")),
                    10000);
            }
            else {
                // A raw CUSTOMER_ENDED never claims who ended it. The text is set by the async
                // session read after teardown, so wait on that effect itself.
                QTRY_COMPARE_WITH_TIMEOUT(
                    client.endReasonText(),
                    QStringLiteral("The session ended. Unused minutes stay in your account."),
                    10000);
            }
            QCOMPARE(capture.named(QStringLiteral("play.summary")).size(), 1);

            client.session()->attachSession(nullptr);
        }
        delete engine;
    }

    // T075 / F5: the shared counterexample of the server's
    // `test_absent_ambiguous_or_generic_evidence_is_unknown_never_manual` (tests/test_end_outcomes.py):
    // completed CUSTOMER_ENDED, stage streaming, engine_error -102 and no independent supported loss
    // evidence is `unknown`. ADR-0096 / timeline.py limit native loss to {-100, -101}, so the client
    // must not present -102 alone as `connection_lost`.
    void nativeMinus102MatchesServerUnknown()
    {
        const QString endPath = QStringLiteral("/api/sessions/s-stages/end");

        auto* engine = new FakeEngineSession;
        {
            SeatHubClient client;
            beginStagedSession(client);
            QVERIFY(!QTest::currentTestFailed());
            client.teardown()->setVerifyIntervalMs(1);
            PlayCapture capture;
            client.session()->attachSession(engine);
            client.session()->start(nullptr);
            PairingController* pairing = client.pairing();
            QMetaObject::invokeMethod(
                pairing, [pairing]() { pairing->cancel(); }, Qt::BlockingQueuedConnection);
            emit engine->connectionStarted();
            QCOMPARE(client.appState(), QStringLiteral("streaming"));

            m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("COMPLETED"),
                                  QStringLiteral("CUSTOMER_ENDED"), 15);
            emitConnectionTerminated(-102);

            QSignalSpy completed(client.teardown(), &TeardownController::teardownCompleted);
            emit engine->readyForDeletion();
            QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 15000);
            QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);
            QTRY_COMPARE_WITH_TIMEOUT(capture.named(QStringLiteral("play.summary")).size(), 1, 10000);

            // The observed native code is still recorded as the first native evidence, but it is not
            // supported loss evidence, so the presentation is unknown.
            const ShippedLine summary = capture.named(QStringLiteral("play.summary")).first();
            QCOMPARE(summary.attrs.value("end_presentation").toString(), QStringLiteral("unknown"));
            QCOMPARE(summary.attrs.value("native_fault_code").toInt(), -102);
            QVERIFY(!summary.attrs.contains("manual_end_observed"));

            // The final text never claims a lost connection.
            QTRY_COMPARE_WITH_TIMEOUT(
                client.endReasonText(),
                QStringLiteral("The session ended. Unused minutes stay in your account."), 10000);

            // The outbound End is the unchanged ordinary one: sent once, not failed, no new field.
            QTRY_COMPARE_WITH_TIMEOUT(m_fake->bodiesFor(endPath).size(), 1, 15000);
            const QJsonObject sent = QJsonDocument::fromJson(m_fake->bodyFor(endPath)).object();
            QVERIFY(!sent.contains(QStringLiteral("end_presentation")));
            QVERIFY(!sent.contains(QStringLiteral("manual")));
            QVERIFY(!sent.contains(QStringLiteral("intent")));
            QVERIFY(!sent.contains(QStringLiteral("engine_error")));
            QCOMPARE(sent.value(QStringLiteral("failed")).toBool(), false);
            QCOMPARE(capture.named(QStringLiteral("play.summary")).size(), 1);

            client.session()->attachSession(nullptr);
        }
        delete engine;
    }

    void aSecondPlayDoesNotInheritTheFirstPlaysEvidence()
    {
        auto* engine = new FakeEngineSession;
        {
            SeatHubClient client;
            beginStagedSession(client);
            QVERIFY(!QTest::currentTestFailed());
            client.teardown()->setVerifyIntervalMs(1);
            PlayCapture capture;
            client.session()->attachSession(engine);
            client.session()->start(nullptr);
            PairingController* pairing = client.pairing();
            QMetaObject::invokeMethod(
                pairing, [pairing]() { pairing->cancel(); }, Qt::BlockingQueuedConnection);
            emit engine->connectionStarted();
            emitConnectionTerminated(-102);
            m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("COMPLETED"),
                                  QStringLiteral("CUSTOMER_ENDED"), 15);
            QSignalSpy completed(client.teardown(), &TeardownController::teardownCompleted);
            emit engine->readyForDeletion();
            QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 15000);
            QTRY_COMPARE_WITH_TIMEOUT(capture.named(QStringLiteral("play.summary")).size(), 1, 10000);
            client.session()->attachSession(nullptr);

            // The next Play, outside-cancelled while connecting, carries none of the first one's code.
            m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                       QStringLiteral("SH-2K2XQ1")));
            client.beginSession(QStringLiteral("s-second"));
            QCOMPARE(client.appState(), QStringLiteral("connecting"));
            SessionInfo over = sessionIn(QStringLiteral("COMPLETED"), QStringLiteral("s-second"));
            over.endReason = QStringLiteral("CUSTOMER_ENDED");
            report(client, over);
            QTRY_COMPARE_WITH_TIMEOUT(capture.named(QStringLiteral("play.summary")).size(), 2, 10000);

            const ShippedLine second = capture.named(QStringLiteral("play.summary")).last();
            QCOMPARE(second.attrs.value("end_presentation").toString(), QStringLiteral("cancelled"));
            QVERIFY(!second.attrs.contains("native_fault_code"));
            QVERIFY(!second.attrs.contains("manual_end_observed"));
        }
        delete engine;
    }

    // --- T026 / E16: an outside cancel while connecting is neutral, never the failure screen --------

    void anOutsideCancelWhileConnectingIsNeutral()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        PlayCapture capture;
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"),
                              QStringLiteral("CUSTOMER_ENDED"));

        SessionInfo over = sessionIn(QStringLiteral("COMPLETED"));
        over.endReason = QStringLiteral("CUSTOMER_ENDED");
        report(client, over);

        QVERIFY(!client.connectFailed());
        QCOMPARE(client.appState(), QStringLiteral("home"));
        QCOMPARE(client.endReasonText(), QStringLiteral("Cancelled. You were not charged."));

        QTRY_COMPARE_WITH_TIMEOUT(capture.named(QStringLiteral("play.summary")).size(), 1, 10000);
        const ShippedLine summary = capture.named(QStringLiteral("play.summary")).first();
        QCOMPARE(summary.attrs.value("outcome").toString(), QStringLiteral("cancelled"));
        QCOMPARE(summary.attrs.value("end_presentation").toString(), QStringLiteral("cancelled"));
        QVERIFY(!summary.attrs.contains("manual_end_observed"));
        QVERIFY(!summary.attrs.contains("native_fault_code"));

        // The End still goes up once, as the failed-start End it always was.
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->bodiesFor(QStringLiteral("/api/sessions/s-stages/end")).size() >= 1, 15000);
        const QJsonObject sent = QJsonDocument::fromJson(
            m_fake->bodyFor(QStringLiteral("/api/sessions/s-stages/end"))).object();
        QCOMPARE(sent.value(QStringLiteral("failed")).toBool(), true);
        QCOMPARE(sent.value(QStringLiteral("stage")).toString(), QStringLiteral("connecting"));
    }

    void aStageFailureReportsItsEngineCodeAndPorts()
    {
        auto* engine = new FakeEngineSession;
        {
            SeatHubClient client;
            client.session()->attachSession(engine);
            beginStagedSession(client);
            QVERIFY(!QTest::currentTestFailed());
            client.teardown()->setVerifyIntervalMs(1);
            m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"));

            QTemporaryDir spoolDir;
            QVERIFY(spoolDir.isValid());
            QMutex captureMutex;
            QList<ShippedLine> captured;
            LogShipper::instance().setSpoolDirectoryForTests(spoolDir.path());
            LogShipper::instance().start([&captureMutex, &captured](const ShippedLine& line) {
                QMutexLocker lock(&captureMutex);
                captured.append(line);
                return true;
            });
            LogShipper::instance().setCanShip(true);
            StopShipperOnScopeExit stopGuard;

            emit engine->stageStarting(QStringLiteral("RTSP handshake"));
            emit engine->stageFailed(QStringLiteral("RTSP handshake"), -102,
                                     QStringLiteral("UDP 47998"));

            QVERIFY(client.connectFailed());
            QTRY_VERIFY_WITH_TIMEOUT(
                m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-stages/end")), 15000);
            const QJsonObject sent = QJsonDocument::fromJson(
                m_fake->bodyFor(QStringLiteral("/api/sessions/s-stages/end"))).object();
            QCOMPARE(sent.value(QStringLiteral("failed")).toBool(), true);
            // D-11: the stage is `connecting` here, not the engine's own stage name - pairing
            // already succeeded for the engine to have started at all.
            QCOMPARE(sent.value(QStringLiteral("stage")).toString(), QStringLiteral("connecting"));
            QCOMPARE(sent.value(QStringLiteral("engine_stage")).toString(),
                     QStringLiteral("RTSP handshake"));
            QCOMPARE(sent.value(QStringLiteral("engine_error")).toInt(), -102);
            QCOMPARE(sent.value(QStringLiteral("failing_ports")).toString(),
                     QStringLiteral("UDP 47998"));
            // ADR-0104: no port test was relayed for this Play, so the End carries no `port_test`.
            QVERIFY(!sent.contains(QStringLiteral("port_test")));
            const auto playStepCount = [&captureMutex, &captured]() {
                QMutexLocker lock(&captureMutex);
                return std::count_if(captured.cbegin(), captured.cend(),
                    [](const ShippedLine& line) {
                        return line.body == QLatin1String("play.step");
                    });
            };
            QTRY_COMPARE_WITH_TIMEOUT(playStepCount(), 1, 15000);
            LogShipper::instance().stop();
            QMutexLocker lock(&captureMutex);
            const auto record = std::find_if(captured.cbegin(), captured.cend(),
                [](const ShippedLine& line) { return line.body == QLatin1String("play.step"); });
            QVERIFY(record != captured.cend());
            QCOMPARE(std::count_if(captured.cbegin(), captured.cend(),
                [](const ShippedLine& line) { return line.body == QLatin1String("play.step"); }), 1);
            QCOMPARE(record->attrs.value(QStringLiteral("step")).toString(),
                     QStringLiteral("engine_connect"));
            QCOMPARE(record->attrs.value(QStringLiteral("failure_class")).toString(),
                     QStringLiteral("rtsp_handshake"));
            QCOMPARE(sent.value(QStringLiteral("attempt_step")).toString(),
                     QStringLiteral("engine_connect"));
            // ADR-0091 supersedes D-51 for the sentence: Moonlight's own stage text is shown as
            // given, and the stage name also rides the diagnostic end body the server sees.
            QVERIFY(client.stalledReasonPlain());

            client.session()->attachSession(nullptr);
        }
        delete engine;
    }

    void aNoVideoFrameFailureEmitsTheStreamStepClass()
    {
        auto* engine = new FakeEngineSession;
        {
            SeatHubClient client;
            client.session()->attachSession(engine);
            beginStagedSession(client);
            QVERIFY(!QTest::currentTestFailed());
            emit engine->connectionStarted();
            QVERIFY(!client.attachedSessionId().isEmpty());

            QTemporaryDir spoolDir;
            QVERIFY(spoolDir.isValid());
            QMutex captureMutex;
            QList<ShippedLine> captured;
            LogShipper::instance().setSpoolDirectoryForTests(spoolDir.path());
            LogShipper::instance().start([&captureMutex, &captured](const ShippedLine& line) {
                QMutexLocker lock(&captureMutex);
                captured.append(line);
                return true;
            });
            LogShipper::instance().setCanShip(true);
            StopShipperOnScopeExit stopGuard;

            emitConnectionTerminated(-101);
            emit engine->displayLaunchError(QStringLiteral("fixture-only engine error text"));

            const auto playStepCount = [&captureMutex, &captured]() {
                QMutexLocker lock(&captureMutex);
                return std::count_if(captured.cbegin(), captured.cend(),
                    [](const ShippedLine& line) {
                        return line.body == QLatin1String("play.step");
                    });
            };
            QTRY_COMPARE_WITH_TIMEOUT(playStepCount(), 1, 15000);

            QMutexLocker lock(&captureMutex);
            const auto record = std::find_if(captured.cbegin(), captured.cend(),
                [](const ShippedLine& line) { return line.body == QLatin1String("play.step"); });
            QVERIFY(record != captured.cend());
            QCOMPARE(record->attrs.value(QStringLiteral("step")).toString(),
                     QStringLiteral("stream"));
            QCOMPARE(record->attrs.value(QStringLiteral("failure_class")).toString(),
                     QStringLiteral("no_video_frame"));
        }
        delete engine;
    }

    void theFailureScreenShowsTheServersBlameAndReference()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);

        // The client's own best guess, shown at once when connecting stopped.
        emit client.pairing()->pairingFailed(
            SeatHubFailure::local(QStringLiteral("The rig didn't finish connecting. Try again.")));
        QVERIFY(client.connectFailed());

        // The server's verdict, arriving on the terminal read teardown ends on: this exact
        // CONNECT_FAILED ending, blamed on the connection, with the server's own reference.
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"),
                             QStringLiteral("CONNECT_FAILED"), 0, QStringLiteral("connection"),
                             QStringLiteral("SH-4F7KQ2"));

        QTRY_COMPARE_WITH_TIMEOUT(
            client.failure().value(QStringLiteral("error")).toString(),
            QStringLiteral("Your connection couldn't reach the rig, so the stream didn't start. "
                           "You were not charged."),
            15000);
        QCOMPARE(client.reference(), QStringLiteral("SH-4F7KQ2"));
        // The view still reads the same two properties `ConnectingScreen.qml` renders.
        QCOMPARE(client.stalledReasonText(),
                 QStringLiteral("Your connection couldn't reach the rig, so the stream didn't start. "
                                "You were not charged."));
    }

    void aSecondFailureDoesNotEndTwice()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        // Kept ENDING (never terminal) for the whole test: the guard - not a lucky race against
        // the verify poll reaching a terminal read - is what keeps the second failure from
        // starting its own `/end`.
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("ENDING"));

        emit client.pairing()->pairingFailed(SeatHubFailure::local(QStringLiteral("first")));
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-stages/end")), 15000);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-stages/end")), 1);

        emit client.pairing()->pairingFailed(SeatHubFailure::local(QStringLiteral("second")));
        QTest::qWait(200);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-stages/end")), 1);
    }

    // --- D-05/C4a: a late pairing result cannot hijack the customer's next session (06.6-18) --------

    void aLateHostFromACancelledSessionIsIgnored()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());

        // "Cancelled": the same D-05/C3 path Cancel-before-the-engine uses (06.6-23). `m_sessionId`
        // is left attached to `s-stages` by `interrupt()` itself (it never clears it) until a fresh
        // Play attaches a different one, exactly the window a late host from the old handshake would
        // otherwise land in.
        client.interrupt();
        client.beginSession(QStringLiteral("s-fresh"));

        const int before = g_engineCreateCalls;
        const bool invoked = QMetaObject::invokeMethod(
            &client, "handleHostResolved", Q_ARG(QString, QStringLiteral("s-stages")),
            Q_ARG(PairedHostPtr, std::make_shared<PairedHost>()));
        QVERIFY2(invoked, "handleHostResolved must accept the session id it is resolving for");

        // The stale session's own late host never reaches the engine seam at all - it is dropped
        // before `MoonlightEngineSession::create()` is ever called, not merely failed closed the
        // ordinary way once there.
        QCOMPARE(g_engineCreateCalls, before);
    }

    // 06.6-REVIEW CR-01: a late host from a session Cancel already ended, with NO fresh Play in
    // between, must be dropped too. Unlike `aLateHostFromACancelledSessionIsIgnored()` above -
    // where a fresh `beginSession("s-fresh")` changes `m_sessionId` and the id check alone catches
    // the stale host - `interrupt()`'s pre-engine Cancel branch never clears `m_sessionId` at all
    // (only `setAttachedSessionEnded(true)` runs). A late `hostResolved` for that SAME session id
    // must still be refused, or a live engine attaches to a session the server has already been
    // told is over (CR-01).
    void aLateHostFromTheSameCancelledSessionIsIgnored()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());

        client.interrupt();
        QCOMPARE(client.appState(), QStringLiteral("home"));

        const int before = g_engineCreateCalls;
        const bool invoked = QMetaObject::invokeMethod(
            &client, "handleHostResolved", Q_ARG(QString, QStringLiteral("s-stages")),
            Q_ARG(PairedHostPtr, std::make_shared<PairedHost>()));
        QVERIFY2(invoked, "handleHostResolved must accept the session id it is resolving for");

        // Still the same session id as before Cancel - the id check alone would let this through.
        // `m_sessionEnded` is what must drop it: no engine is ever built for it.
        QCOMPARE(g_engineCreateCalls, before);
    }

    void aHostForTheAttachedSessionIsUsed()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());

        const int before = g_engineCreateCalls;
        const bool invoked = QMetaObject::invokeMethod(
            &client, "handleHostResolved", Q_ARG(QString, QStringLiteral("s-stages")),
            Q_ARG(PairedHostPtr, std::make_shared<PairedHost>()));
        QVERIFY2(invoked, "handleHostResolved must accept the session id it is resolving for");

        // A host resolved for the session actually attached is used as today: the facade goes on to
        // try building the engine from it.
        QCOMPARE(g_engineCreateCalls, before + 1);
    }

    // --- D-05/C3: Cancel before the engine ends the session and goes Home (06.6-23) ----------------

    void cancelBeforeEngineEndsTheSession()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"));

        client.interrupt();

        // Cancel is not a failure: it returns Home at once, without waiting on the network - a
        // customer stuck on this screen while the server answers is exactly what T-06.6-49 exists
        // to close (`screens.md` §24).
        QCOMPARE(client.appState(), QStringLiteral("home"));
        QVERIFY(!client.connectFailed());

        // The pairing poll actually stops - it does not keep failing quietly in the background.
        QTRY_COMPARE_WITH_TIMEOUT(client.pairing()->state(), QStringLiteral("idle"), 15000);

        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-stages/end")), 15000);
        QCOMPARE(m_fake->bodyFor(QStringLiteral("/api/sessions/s-stages/end")),
                 QByteArrayLiteral("{}"));
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-stages/end")), 1);
    }

    void cancelWhileStreamingIsUnchanged()
    {
        auto* engine = new FakeEngineSession;
        {
            SeatHubClient client;
            client.session()->attachSession(engine);

            // The local path (no access token) is the one place this suite can make
            // `SessionLifecycle::start()` genuinely run the attached engine, so `session()->active()`
            // is really true here - not the `emit engine->connectionStarted()` shortcut most
            // streaming-state tests use, which never touches `m_active` at all.
            client.start();
            QCOMPARE(client.session()->active(), true);
            emit engine->connectionStarted();
            QCOMPARE(client.appState(), QStringLiteral("streaming"));

            client.interrupt();

            // D-05/C3: the engine is active, so Cancel keeps today's quit-key path - it pushes the
            // interrupt and posts nothing itself; the appState only moves once the engine's own
            // teardown sequence (`readyForDeletion`) says so, never fast-forwarded to Home the way
            // the not-yet-active branch above is.
            QCOMPARE(engine->interrupts.load(), 1);
            QCOMPARE(client.appState(), QStringLiteral("streaming"));

            emit engine->sessionFinished(0);
            emit engine->readyForDeletion();
            QCOMPARE(client.appState(), QStringLiteral("signed_out"));

            client.session()->attachSession(nullptr);
        }
        delete engine;
    }

    // --- D-05/C4/C5: Try again is always a fresh Play; Resume only after the stream started --------

    void tryAgainIsAFreshPlay()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        // The old session's own teardown (from the pairing failure, C2) must still be in flight
        // when `retry()` runs, or this never observes `retryBusy` - it would just find the session
        // already gone.
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("ENDING"));

        emit client.pairing()->pairingFailed(SeatHubFailure::local(QStringLiteral("failed")));
        QVERIFY(client.connectFailed());

        client.retry();
        QVERIFY2(client.retryBusy(), "Try again waits for the old session's own teardown (C4)");
        QVERIFY(!client.connectFailed());

        // While the old session is still ENDING, no fresh Play has gone out.
        QTest::qWait(100);
        QVERIFY(client.retryBusy());
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions")), 0);

        // A second Play pressed during the wait must not race the one `retry()` already promised -
        // it would otherwise post a second `POST /api/sessions`, which the old session's own
        // teardown (arriving after) would then tear back down as though it had never streamed.
        client.start();
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions")), 0);
        QVERIFY(client.retryBusy());

        // The old session reaches CANCELLED (D-05/D-23): the wait ends and a fresh Play goes out.
        m_fake->answerPlay(201, QByteArrayLiteral("{\"id\":\"s-fresh\"}"));
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"));

        QTRY_VERIFY_WITH_TIMEOUT(!client.retryBusy(), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(m_fake->requestPaths().count(QStringLiteral("/api/sessions")), 1,
                                  15000);
        // The request is counted before its reply moves the app on, so wait on the state itself.
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("connecting"), 15000);
        // Pairing polls the NEW session, never the old one again - proof this was a fresh Play,
        // never a reattachment (which would have addressed "s-stages").
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-fresh/pairing")), 15000);
    }

    // --- D-05/C4/C7: Play ends a never-streamed attached session first, same as Try again ---------

    void playAfterANeverStreamedSessionEndsItFirst()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);

        // C2's own end attempt cannot reach the server (C7): the session stays attached and its
        // teardown claim is released, ready for the next attempt to ask again.
        m_fake->answerEnd(0);
        QSignalSpy failed(client.teardown(), &TeardownController::teardownFailed);
        emit client.pairing()->pairingFailed(SeatHubFailure::local(QStringLiteral("failed")));
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-stages/end")), 15000);
        // The claim is released by the facade's handler of this failure on this thread. That
        // connection is older than the spy's, so its queued call is posted before the spy counts;
        // run it, or `start()` below can find the claim still spent and never ask a second time.
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 15000);
        QCoreApplication::processEvents();
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-stages/end")), 1);
        QVERIFY(!client.retryBusy());

        // Back to home, then Play - not Try again. This is C4's second half.
        client.dismissError();
        QCOMPARE(client.appState(), QStringLiteral("home"));

        m_fake->answerEnd(200);
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("ENDING"));
        client.start();
        QVERIFY2(client.retryBusy(), "Play ends the never-streamed session first (C4/C7)");
        QTRY_COMPARE_WITH_TIMEOUT(
            m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-stages/end")), 2, 15000);

        // While the old session is still ENDING, no fresh Play has gone out.
        QTest::qWait(100);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions")), 0);

        // The old session reaches CANCELLED: the wait ends and a fresh Play goes out.
        m_fake->answerPlay(201, QByteArrayLiteral("{\"id\":\"s-fresh\"}"));
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"));

        QTRY_VERIFY_WITH_TIMEOUT(!client.retryBusy(), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(m_fake->requestPaths().count(QStringLiteral("/api/sessions")), 1,
                                  15000);
        QCOMPARE(client.appState(), QStringLiteral("connecting"));
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-fresh/pairing")), 15000);
    }

    void tryAgainShowsBusyUntilTheOldSessionEnds()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("ENDING"));

        emit client.pairing()->pairingFailed(SeatHubFailure::local(QStringLiteral("failed")));
        QVERIFY(client.connectFailed());
        // C2's own end for the pairing failure must actually have reached the network thread
        // before `retry()` runs, or the count below observes zero requests by pure timing luck
        // rather than proving anything about `retry()` itself.
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-stages/end")), 15000);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-stages/end")), 1);

        client.retry();
        QVERIFY2(client.retryBusy(), "Try again shows busy from the very first press");
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-stages/end")), 1);

        // A second Try again press while the old session is still ending must post nothing.
        client.retry();
        QVERIFY(client.retryBusy());
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-stages/end")), 1);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions")), 0);

        // The old session reaches its terminal read: the busy state clears and a fresh Play goes out.
        m_fake->answerPlay(201, QByteArrayLiteral("{\"id\":\"s-fresh-2\"}"));
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"));

        QTRY_VERIFY_WITH_TIMEOUT(!client.retryBusy(), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(m_fake->requestPaths().count(QStringLiteral("/api/sessions")), 1,
                                  15000);
    }

    // `TeardownController::cancel()` (what `signOut()` calls) emits neither `teardownCompleted` nor
    // `teardownFailed` - a `retry()` wait abandoned by a sign-out must still clear `retryBusy`
    // itself, or the NEXT, unrelated session's ordinary teardown would find it still set and fire
    // an unrequested fresh Play for a customer who is no longer signed in.
    void signOutDuringTheWaitClearsRetryBusy()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("ENDING"));

        emit client.pairing()->pairingFailed(SeatHubFailure::local(QStringLiteral("failed")));
        client.retry();
        QVERIFY(client.retryBusy());

        client.signOut();
        QVERIFY2(!client.retryBusy(), "a sign-out mid-wait must not leave the flag stuck");
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("signed_out"), 15000);
    }

    // UD-04 R14, RS01 (SC2): there is no customer Resume. A process that dies mid-session leaves the
    // session with the server; the relaunched app, signed in again, offers Play only, reattaches
    // nothing, and a Play it makes meets the server's own refusal for the session still held.
    void noCustomerResumeAfterRestart()
    {
        const QString sentence = QStringLiteral("The server's own sentence about the session it holds.");

        // Process one: the stream starts, then the process dies before anything ends the session.
        {
            auto* engine = new FakeEngineSession;
            {
                SeatHubClient first;
                first.session()->attachSession(engine);
                beginStagedSession(first);
                QVERIFY(!QTest::currentTestFailed());
                emit engine->connectionStarted();
                QCOMPARE(first.appState(), QStringLiteral("streaming"));
                QCOMPARE(first.attachedSessionId(), QStringLiteral("s-stages"));
                first.session()->attachSession(nullptr);
            }
            delete engine;
            m_fake = nullptr;
        }

        // Process two: the same customer signs in again (a stored credential restores it).
        SeatHubClient second;
        reachHome(second, 90);
        QVERIFY(!QTest::currentTestFailed());
        QCOMPARE(second.appState(), QStringLiteral("home"));
        QVERIFY2(second.attachedSessionId().isEmpty(), "a restart reattaches no session");

        // Play is the only way on, and the server still holds the earlier session.
        m_fake->answerPlay(409, playRefusalBody(sentence, QStringLiteral("SH-4F7KQ2"),
                                                QStringLiteral("USER_HAS_NONTERMINAL_SESSION")));
        second.start();
        QTRY_COMPARE_WITH_TIMEOUT(second.homeStatus(), QStringLiteral("refused"), 15000);
        QCOMPARE(second.appState(), QStringLiteral("home"));

        // LK01: the server's own sentence and reference, unchanged, on Home.
        QCOMPARE(second.failure().value(QStringLiteral("error")).toString(), sentence);
        QCOMPARE(second.reference(), QStringLiteral("SH-4F7KQ2"));

        // A NEW session was asked for, once. Nothing addressed the earlier one: no pairing, no read,
        // no end - a session route would show here, and none does.
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions")), 1);
        for (const QString& path : m_fake->requestPaths()) {
            // T063 / ADR-0105: the dead process left its evidence capsule on disk, and the next
            // sign-in delivers it. That upload only hands the server the earlier run's record; it
            // addresses no pairing, read or end of the session and attaches nothing. It is the one
            // session route allowed here, and only for exactly that session.
            if (path == QStringLiteral("/api/sessions/s-stages/evidence")) {
                continue;
            }
            QVERIFY2(!path.startsWith(QStringLiteral("/api/sessions/")), qPrintable(path));
        }
        QVERIFY(second.attachedSessionId().isEmpty());
    }

    // The same rule inside one process: a streamed session whose end could not reach the server is
    // still held there, and Play asks for a new session instead of picking it up again.
    void playNeverReattachesAStreamedSessionTheServerStillHolds()
    {
        const QString sentence = QStringLiteral("The server's own sentence about the session it holds.");
        auto* engine = new FakeEngineSession;
        {
            SeatHubClient client;
            client.session()->attachSession(engine);
            beginStagedSession(client);
            QVERIFY(!QTest::currentTestFailed());
            client.teardown()->setVerifyIntervalMs(1);
            emit engine->connectionStarted();
            QCOMPARE(client.appState(), QStringLiteral("streaming"));

            // The stream ends and the end cannot reach the server: the session stays held there.
            m_fake->answerEnd(0);
            QSignalSpy failed(client.teardown(), &TeardownController::teardownFailed);
            emit engine->readyForDeletion();
            QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 15000);
            QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);
            // C-ENDING: the End could not be confirmed, so the durable record stands and Home shows
            // the ending state (Play not ready) instead of an error screen.
            QCOMPARE(client.homeStatus(), QStringLiteral("ending"));
            // C-ENDING: Play is not ready while the End is unconfirmed. Once the server confirms it,
            // Play asks for a new session of its own.
            m_fake->answerEnd(200);
            QTRY_VERIFY_WITH_TIMEOUT(!client.hasSavedEnd(), 15000);
            client.dismissError();
            QCOMPARE(client.appState(), QStringLiteral("home"));
            QCOMPARE(client.attachedSessionId(), QStringLiteral("s-stages"));

            m_fake->answerPlay(409, playRefusalBody(sentence, QStringLiteral("SH-4F7KQ2"),
                                                    QStringLiteral("USER_HAS_NONTERMINAL_SESSION")));
            client.start();
            QTRY_COMPARE_WITH_TIMEOUT(client.homeStatus(), QStringLiteral("refused"), 15000);

            // Home, with the server's sentence: a pick-up of the held session would be `connecting`.
            QCOMPARE(client.appState(), QStringLiteral("home"));
            QCOMPARE(client.failure().value(QStringLiteral("error")).toString(), sentence);
            QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions")), 1);

            client.session()->attachSession(nullptr);
        }
        delete engine;
    }

    // UD-04 O01: a customer's own quit is not a reported failure. The server's ending a session as
    // `CONNECT_FAILED` blames a rig, so a quit - which blames nothing - must not look like one.
    void theInStreamQuitShortcutBeforeActiveIsNotReportedFailed()
    {
        auto* engine = new FakeEngineSession;
        {
            SeatHubClient client;
            client.session()->attachSession(engine);
            beginStagedSession(client);
            QVERIFY(!QTest::currentTestFailed());
            client.teardown()->setVerifyIntervalMs(1);
            m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"));

            // The rig was still being prepared when the session was last read, which records the
            // progress marker `rig_wait`; the stream then starts arriving, and the customer quits
            // with the in-stream shortcut before the server marks the session ACTIVE.
            report(client, sessionIn(QStringLiteral("PREPARING")));
            emit engine->connectionStarted();
            QCOMPARE(client.appState(), QStringLiteral("streaming"));
            emit engine->readyForDeletion();

            QTRY_VERIFY_WITH_TIMEOUT(
                m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-stages/end")),
                15000);
            const QJsonObject sent = QJsonDocument::fromJson(
                m_fake->bodyFor(QStringLiteral("/api/sessions/s-stages/end"))).object();
            QVERIFY2(!sent.value(QStringLiteral("failed")).toBool(),
                     "a quit once the stream arrived must not be reported as a failure");
            QVERIFY(!sent.contains(QStringLiteral("attempt_step")));

            client.session()->attachSession(nullptr);
        }
        delete engine;
    }

    void aCancelWhileTheEngineConnectsIsNotReportedFailed()
    {
        auto* engine = new FakeEngineSession;
        {
            SeatHubClient client;
            client.session()->attachSession(engine);
            beginStagedSession(client);
            QVERIFY(!QTest::currentTestFailed());
            client.teardown()->setVerifyIntervalMs(1);
            m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"));

            // The engine is running (its connect stage), and the customer presses Cancel. The
            // engine's interrupted connection then surfaces as a stage failure - which here is the
            // customer's own quit, not a connection failure.
            QVERIFY(client.session()->start(nullptr));
            client.interrupt();
            QCOMPARE(engine->interrupts.load(), 1);
            emit engine->stageFailed(QStringLiteral("RTSP handshake"), -1, QString());

            QTRY_VERIFY_WITH_TIMEOUT(
                m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-stages/end")),
                15000);
            const QJsonObject sent = QJsonDocument::fromJson(
                m_fake->bodyFor(QStringLiteral("/api/sessions/s-stages/end"))).object();
            QVERIFY2(!sent.value(QStringLiteral("failed")).toBool(),
                     "a Cancel must not be reported as a connection failure");

            emit engine->readyForDeletion();
            client.session()->attachSession(nullptr);
        }
        delete engine;
    }

    // R06: the teardown of a session that has ended finishes after a newer session was attached. It
    // must not stop, detach or end anything of the newer one.
    void aLateTeardownOfAnEndedSessionNeverActsOnTheNewerSession()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        QCOMPARE(client.attachedSessionId(), QStringLiteral("s-stages"));

        // A newer session is attached (its pairing polls run), then the older one's teardown ends.
        client.beginSession(QStringLiteral("s-newer"));
        QCOMPARE(client.attachedSessionId(), QStringLiteral("s-newer"));
        QCOMPARE(client.appState(), QStringLiteral("connecting"));

        SessionInfo old = sessionIn(QStringLiteral("COMPLETED"), QStringLiteral("s-stages"));
        old.endReason = QStringLiteral("CUSTOMER_ENDED");
        emit client.teardown()->teardownCompleted(old, QStringLiteral("s-stages"));
        emit client.teardown()->teardownFailed(SeatHubFailure::local(QStringLiteral("late")),
                                               QStringLiteral("s-stages"));
        QTest::qWait(30);

        QCOMPARE(client.attachedSessionId(), QStringLiteral("s-newer"));
        QCOMPARE(client.appState(), QStringLiteral("connecting"));
        QVERIFY(client.failure().isEmpty());
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
        // ADR-0096: with no observed evidence the line makes no claim about who ended it.
        QTest::newRow("the session ended, nobody observed")
            << "CUSTOMER_ENDED" << 15
            << "The session ended. Unused minutes stay in your account.";
        // A sentence with a number in it takes the session's own billed minutes, in mono.
        QTest::newRow("the rig was lost, with a number")
            << "HOST_LOST" << 23
            << QStringLiteral("We lost contact with this rig, so the session ended. You were charged "
                              "for %1 minutes.").arg(mono.arg(23));
        // C-SAMEPLAY: the customer's own Play ended the held session.
        QTest::newRow("the customer's new Play replaced it")
            << "REPLACED_BY_PLAY" << 23
            << QStringLiteral("Ended by your new Play. You were charged for %1 minutes.")
                   .arg(mono.arg(23));
        // Every `You were charged for {minutes}.` sentence says `1 minute` in the singular.
        QTest::newRow("the new Play replaced it, one minute")
            << "REPLACED_BY_PLAY" << 1
            << QStringLiteral("Ended by your new Play. You were charged for %1 minute.")
                   .arg(mono.arg(1));
        QTest::newRow("the rig was lost, one minute")
            << "HOST_LOST" << 1
            << QStringLiteral("We lost contact with this rig, so the session ended. You were charged "
                              "for %1 minute.").arg(mono.arg(1));
        QTest::newRow("the connection dropped too often, one minute")
            << "RECONNECT_LIMIT" << 1
            << QStringLiteral("Your connection dropped too many times, so the session ended. You were "
                              "charged for %1 minute.").arg(mono.arg(1));
        QTest::newRow("support ended it, one minute")
            << "OPERATOR_FORCED" << 1
            << QStringLiteral("Support ended this session. You were charged for %1 minute.")
                   .arg(mono.arg(1));
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
            // The text arrives with the session read, which can land just after Home does.
            QTRY_COMPARE_WITH_TIMEOUT(client.endReasonText(), expected, 15000);
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

    // --- D-23: the CONNECT_FAILED sentence on Home (06.6-18) -----------------------------------------

    void connectFailedSaysTheStreamDidNotStart()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());

        SessionInfo over = sessionIn(QStringLiteral("FAILED"));
        over.endReason = QStringLiteral("CONNECT_FAILED");
        report(client, over);

        QCOMPARE(client.endReasonText(),
                 QStringLiteral("The stream didn't start. You were not charged."));
    }

    void theFacadeHandsTheScreenTheBundledCountriesAndARegionThatIsOneOfThem()
    {
        SeatHubClient client;
        QCOMPARE(client.countries().size(), SeatHubCountries::all().size());
        QVERIFY(client.countries().size() > 0);
        QVERIFY(SeatHubCountries::contains(client.defaultCountryCode()));
        // C-SIGNIN: Jordan is the default for every sign-in, whatever region this machine is set to.
        QCOMPARE(client.defaultCountryCode(), QStringLiteral("JO"));
        QCOMPARE(SeatHubCountries::find(client.defaultCountryCode())
                     .value(QStringLiteral("dial")).toString(),
                 QStringLiteral("+962"));
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
        QCOMPARE(client.balanceMinutes(), qint64(0));
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

    void settingsThenProfileFromTheMenuShowsProfileAtOnce()
    {
        // F-LIVE-2 / R6: Settings open, the menu's Profile replaces it instead of staying hidden under it.
        SeatHubClient client;
        reachHome(client, 45);
        QVERIFY(!QTest::currentTestFailed());

        client.openSettings();
        QVERIFY(client.inSettings());
        QVERIFY(!client.inProfile());

        client.openProfile();
        QVERIFY(client.inProfile());
        QVERIFY(!client.inSettings());
        QCOMPARE(client.appState(), QStringLiteral("home"));

        // Back from Profile returns to Home, not to a Settings page left underneath.
        client.closeProfile();
        QVERIFY(!client.inProfile());
        QVERIFY(!client.inSettings());
        QCOMPARE(client.appState(), QStringLiteral("home"));
    }

    void profileThenSettingsFromTheMenuShowsSettingsAndBackReturnsHome()
    {
        // R6: Profile open, the menu's Settings replaces it, and Back from Settings is Home, not Profile.
        SeatHubClient client;
        reachHome(client, 45);
        QVERIFY(!QTest::currentTestFailed());

        client.openProfile();
        QVERIFY(client.inProfile());
        QVERIFY(!client.inSettings());

        client.openSettings();
        QVERIFY(client.inSettings());
        QVERIFY(!client.inProfile());
        QCOMPARE(client.appState(), QStringLiteral("home"));
        // The Profile visit is over: nothing of it is left to show as current later.
        QCOMPARE(client.totalsStatus(), QStringLiteral("idle"));

        client.closeSettings();
        QVERIFY(!client.inSettings());
        QVERIFY(!client.inProfile());
        QCOMPARE(client.appState(), QStringLiteral("home"));

        // Alternating repeatedly never leaves both set.
        for (int i = 0; i < 3; ++i) {
            client.openSettings();
            client.openProfile();
            QVERIFY(client.inProfile() && !client.inSettings());
            client.openSettings();
            QVERIFY(client.inSettings() && !client.inProfile());
        }
        client.closeSettings();
        QVERIFY(!client.inSettings() && !client.inProfile());
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

    void aSessionRowEndedByANewPlaySaysSoInShortWords()
    {
        SeatHubClient client;
        reachProfile(client);
        m_fake->answerList(
            QStringLiteral("/api/sessions"), QString(), 200,
            pageBody("sessions",
                     { sessionRow(QStringLiteral("a"), 12, QStringLiteral("REPLACED_BY_PLAY")),
                       sessionRow(QStringLiteral("b"), 1, QStringLiteral("REPLACED_BY_PLAY")) },
                     QString()));

        SessionListModel* list = client.sessionHistory();
        client.loadFirstPage(QStringLiteral("sessions"));
        QTRY_COMPARE_WITH_TIMEOUT(list->status(), QStringLiteral("ready"), 15000);
        QCOMPARE(list->count(), 2);
        QCOMPARE(cell(list, 0, CustomerListModel::KindRole), QStringLiteral("Ended by your new Play"));
        QCOMPARE(cell(list, 1, CustomerListModel::KindRole), QStringLiteral("Ended by your new Play"));
        QCOMPARE(cell(list, 0, CustomerListModel::AmountRole), QStringLiteral("12 min"));
        QCOMPARE(cell(list, 1, CustomerListModel::AmountRole), QStringLiteral("1 min"));
    }

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
                       ledgerRowJson(QStringLiteral("l6"), QStringLiteral("shortfall"), -5),
                       // A-LC / L11: the extra minutes a held rental returns, in its own row.
                       ledgerRowJson(QStringLiteral("l7"), QStringLiteral("held_return"), 7) },
                     QString()));

        CreditHistoryModel* list = client.creditHistory();
        client.loadFirstPage(QStringLiteral("credit"));
        QTRY_COMPARE_WITH_TIMEOUT(list->status(), QStringLiteral("ready"), 15000);
        QCOMPARE(list->count(), 7);

        const QStringList kinds = { QStringLiteral("Top-up"), QStringLiteral("First top-up bonus"),
                                    QStringLiteral("Played"), QStringLiteral("Refund"),
                                    QStringLiteral("Adjustment by support"),
                                    QStringLiteral("Unpaid minutes"),
                                    QStringLiteral("Minutes returned") };
        const QStringList amounts = { QStringLiteral("+5 h 00 min"), QStringLiteral("+1 h 00 min"),
                                      QStringLiteral("-1 min"), QStringLiteral("+45 min"),
                                      QStringLiteral("-1 h 30 min"), QStringLiteral("-5 min"),
                                      QStringLiteral("+7 min") };
        for (int row = 0; row < 7; ++row) {
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
        QTest::newRow("REPLACED_BY_PLAY") << "REPLACED_BY_PLAY" << "Ended by your new Play";
        QTest::newRow("CONNECT_FAILED") << "CONNECT_FAILED" << "Didn't start, not charged";
        QTest::newRow("BALANCE_EXHAUSTED") << "BALANCE_EXHAUSTED" << "Balance ran out";
        QTest::newRow("HOST_LOST") << "HOST_LOST" << "Lost contact with the rig";
        QTest::newRow("CLIENT_SILENT") << "CLIENT_SILENT" << "Lost contact with your device";
        QTest::newRow("CONNECT_TIMEOUT") << "CONNECT_TIMEOUT" << "Not started in time, not charged";
        QTest::newRow("READINESS_TIMEOUT") << "READINESS_TIMEOUT" << "Rig wasn't ready, not charged";
        QTest::newRow("MODE_BOOT_TIMEOUT") << "MODE_BOOT_TIMEOUT" << "Rig didn't come back, not charged";
        QTest::newRow("GRACE_EXPIRED") << "GRACE_EXPIRED" << "Couldn't reconnect";
        QTest::newRow("RECONNECT_LIMIT") << "RECONNECT_LIMIT" << "Dropped too often";
        QTest::newRow("CLIENT_ABSENT") << "CLIENT_ABSENT" << "Lost contact before start, not charged";
        QTest::newRow("OWNER_RESERVATION") << "OWNER_RESERVATION" << "Owner reserved the rig";
        QTest::newRow("TEARDOWN_TIMEOUT") << "TEARDOWN_TIMEOUT" << "Closed after a problem";
        QTest::newRow("OPERATOR_FORCED") << "OPERATOR_FORCED" << "Ended by support";
        QTest::newRow("a reason the deck has no form for") << "SESSION_LOST" << "Ended";
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
        QTest::newRow("held_return") << "held_return" << "Minutes returned";
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

        QCOMPARE(client.websiteUrl(QStringLiteral("home")), QStringLiteral("https://rentapc.online"));

        QList<QUrl> opened;
        client.setUrlOpener([&opened](const QUrl& url) {
            opened.append(url);
            return true;
        });
        QVERIFY(client.openWebsite(QStringLiteral("home")));
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened.at(0).toString(), QStringLiteral("https://rentapc.online"));
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
        // The whole of this rests on this: nothing in the client knew the balance during a stream,
        // and the one thread that is stopped for the stream is the facade's. The reads are made and
        // Time left is fed on the network thread; this test never runs its own event loop, so a
        // design that hopped to the facade's thread to update it would leave the value at the seed.
        SeatHubClient client;
        auto* engine = new FakeEngineSession;
        WalletTicks ticks;
        beginStreaming(client, engine, &ticks, /*home*/ 40, /*wallet*/ 7);
        QVERIFY(!QTest::currentTestFailed());

        // At the first frame: the seed Home read is on the HUD (D-16: never shown, but recorded).
        QVERIFY(client.hud()->creditMinutes() >= 0);

        // A liveness tick inside the stream reads the wallet; the server's answer replaces the seed
        // and, at seven minutes, arms the D-11 reminder. The session's opening read (made at
        // `beginSession()`, before the HUD's session began) may or may not have reached the HUD, so
        // one tick is driven and waited for (`settleIntoTheStream`). The HUD's own direct connection
        // was made in the facade's constructor, ahead of the counter this waits on, so the value is
        // on the HUD by the time the wait returns - read here with no event loop having run.
        QVERIFY2(settleIntoTheStream(client, &ticks), "no wallet answer came back on the tick");
        QCOMPARE(client.hud()->creditMinutes(), qint64(7));
        client.hud()->tick();
        QVERIFY2(!client.hud()->compositor().composedBottom().isNull(),
                 "seven minutes must show Time left (D-11)");

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

        // D-11: the stage moving to "streaming" inside `beginStreaming` (`handleConnectionStarted`)
        // reports at once, the same way any stage change does, and that report carries no wallet
        // read. It is queued to the network thread with nothing ordering it against the opening
        // read's answer, so the totals are captured only once it has certainly been sent: after
        // `settleIntoTheStream`, whose tick runs behind it. Sampled earlier, it could land between
        // the two samples and be counted as a third report with no read (06.3 test-race fix).
        QVERIFY(settleIntoTheStream(client, &ticks));
        const int reportsBeforeTicks = m_fake->countOfPathEndingWith(QStringLiteral("/liveness"));
        const int readsBeforeTicks = ticks.read.load() + ticks.failed.load();
        const int walletGetsBeforeTicks = m_fake->countOfPathEndingWith(QStringLiteral("/api/wallet"));

        // Two more ticks, and each is one liveness report and one wallet read: the read has no
        // cadence of its own. A tick sends its report in the same call that sends its read, before
        // either can be answered, so both counts are final once the read's answer is in. The read is
        // counted twice over: as the reporter's own answer, and as a `GET /api/wallet` on the wire -
        // the second is what would see a read that came from anywhere but a tick.
        QVERIFY(tickAndWait(client, &ticks));
        QVERIFY(tickAndWait(client, &ticks));
        const int reports =
            m_fake->countOfPathEndingWith(QStringLiteral("/liveness")) - reportsBeforeTicks;
        const int reads = ticks.read.load() + ticks.failed.load() - readsBeforeTicks;
        const int walletGets =
            m_fake->countOfPathEndingWith(QStringLiteral("/api/wallet")) - walletGetsBeforeTicks;
        QCOMPARE(reports, 2);
        QCOMPARE(reads, reports);
        QCOMPARE(walletGets, reports);

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
        // A read inside the stream first, so the HUD's last value is the server's 30 and not the
        // seed, and no answer is in flight to be counted against the failures below.
        QVERIFY(settleIntoTheStream(client, &ticks));
        QCOMPARE(client.hud()->creditMinutes(), qint64(30));

        struct Failure { int status; QByteArray body; const char* what; };
        const Failure failures[] = {
            { 0, QByteArray(), "the control plane is unreachable" },
            { 500, errorJson(QStringLiteral("Something went wrong."), QStringLiteral("SH-9K2XQ1")),
              "a server error" },
            { 200, QByteArrayLiteral("{}"), "an answer with no balance in it" },
            { 401, refusedBody(), "a rejected credential" },
        };
        // The start hint (SC13) shares the bar for its first ten seconds; this test is about Time
        // left, so the hint is cleared first.
        client.hud()->compositor().showStartHint(false);
        for (const Failure& failure : failures) {
            m_fake->answerWallet(failure.status, failure.body);
            const int failedBefore = ticks.failed.load();
            QVERIFY2(tickAndWait(client, &ticks), failure.what);
            QCOMPARE(ticks.failed.load(), failedBefore + 1);

            // Nothing changed: the last value stays, and Time left shows nothing (thirty is above
            // the reminder).
            QCOMPARE(client.hud()->creditMinutes(), qint64(30));
            client.hud()->tick();
            QVERIFY(client.hud()->compositor().composedBottom().isNull());
        }

        // Not even a rejected credential ends the stream or signs anybody out from a wallet read.
        QCOMPARE(client.appState(), QStringLiteral("streaming"));
        QVERIFY(client.signedIn());

        // A later read that succeeds is taken up as if nothing had happened.
        m_fake->answerWallet(200, walletBody(9));
        QVERIFY(tickAndWait(client, &ticks));
        QCOMPARE(client.hud()->creditMinutes(), qint64(9));
        client.hud()->tick();
        QVERIFY2(!client.hud()->compositor().composedBottom().isNull(),
                 "nine minutes must show Time left (D-11)");

        endStreaming(client, engine);
    }

    // [Renamed from `eachThresholdFiresExactlyOnceAcrossASession` (D-12/D-21): the CUST-15 once-
    // per-session cards this test used to pin are retired by Plan 22's D-11 rule, whose reminder
    // is armed once per top-up-or-session, not once per session outright - the top-up assertion at
    // the end is the rule change, not a bent test.]
    void theReminderArmsAtTenCriticalHoldsFromFiveAndATopUpReArms()
    {
        SeatHubClient client;
        auto* engine = new FakeEngineSession;
        WalletTicks ticks;
        beginStreaming(client, engine, &ticks, 90, 90);
        QVERIFY(!QTest::currentTestFailed());
        // The seed is also 90, so the HUD's value alone cannot say a read has landed; the settle's
        // tick can, and it leaves no earlier answer in flight to arrive one step late in the loop.
        QVERIFY(settleIntoTheStream(client, &ticks));
        QCOMPARE(client.hud()->creditMinutes(), qint64(90));
        client.hud()->compositor().showStartHint(false); // the start hint (SC13) is not under test
        client.hud()->tick();
        QVERIFY2(client.hud()->compositor().composedBottom().isNull(), "ninety minutes shows nothing");

        // One minute at a time, past both D-11 thresholds and to zero, and a read repeated at each.
        for (int minutes = 15; minutes >= 0; --minutes) {
            m_fake->answerWallet(200, walletBody(minutes));
            QVERIFY(tickAndWait(client, &ticks));
            QVERIFY(tickAndWait(client, &ticks));   // the same balance again changes nothing
            QCOMPARE(client.hud()->creditMinutes(), qint64(minutes));
            client.hud()->tick();

            // Robust assertion points only: the reminder's own one-minute window runs on the real
            // monotonic clock here (the facade injects none), so only the boundary readings - the
            // reminder's own arming point, and everything at or below the final threshold - are
            // guaranteed regardless of how much real time this loop takes.
            if (minutes == 10) {
                QVERIFY2(!client.hud()->compositor().composedBottom().isNull(),
                         "ten minutes arms the reminder");
            }
            if (minutes <= 5) {
                QVERIFY2(!client.hud()->compositor().composedBottom().isNull(),
                         qPrintable(QStringLiteral("%1 min must show Time left (D-11 final)")
                                        .arg(minutes)));
            }
        }

        // A top-up mid-stream re-arms the rule (D-21): still above ten (sixty) hides it, and the
        // descent to eight - inside the 6-10 band - shows the reminder again, unlike the retired
        // once-per-session cards this test used to assert.
        m_fake->answerWallet(200, walletBody(60));
        QVERIFY(tickAndWait(client, &ticks));
        client.hud()->tick();
        QVERIFY2(client.hud()->compositor().composedBottom().isNull(), "a top-up above ten hides it");
        m_fake->answerWallet(200, walletBody(8));
        QVERIFY(tickAndWait(client, &ticks));
        client.hud()->tick();
        QVERIFY2(!client.hud()->compositor().composedBottom().isNull(),
                 "D-21: a top-up into 6-10 re-arms the reminder");

        endStreaming(client, engine);
    }

    // [Renamed from `aSessionThatBeginsUnderTwoMinutesFiresOnlyTheLowerWarning` (D-11): the
    // once-per-session "lower warning" this test used to pin is retired - the state machine has no
    // "warnings" to fire independently any more, only Critical from any state.]
    void aSessionThatBeginsAtOrBelowFiveShowsCriticalAtOnce()
    {
        SeatHubClient client;
        auto* engine = new FakeEngineSession;
        WalletTicks ticks;
        beginStreaming(client, engine, &ticks, 90, 1);
        QVERIFY(!QTest::currentTestFailed());
        // The opening read may have landed before the HUD's session began (and been dropped); the
        // settle's tick is a read inside it.
        QVERIFY(settleIntoTheStream(client, &ticks));
        QCOMPARE(client.hud()->creditMinutes(), qint64(1));

        client.hud()->tick();
        QVERIFY2(!client.hud()->compositor().composedBottom().isNull(),
                 "one minute shows Time left at once (D-11 Critical)");

        endStreaming(client, engine);
    }

    // T057 / ADR-0080: a warm-return session may now start below the 15-minute Play floor, so the
    // first in-stream read at 6-10 minutes must arm the white reminder at once - through the real
    // client path (liveness report -> wallet read -> HUD), never from the pre-stream balance. The
    // one-minute window and the red final run on the HUD's injected clock (`tst_hud_bitmap`).
    void aWarmReturnThatBeginsBetweenSixAndTenShowsTheReminderAtOnce()
    {
        SeatHubClient client;
        auto* engine = new FakeEngineSession;
        WalletTicks ticks;
        // Home showed forty minutes before the stream; the first in-stream read says eight.
        beginStreaming(client, engine, &ticks, 40, 8);
        QVERIFY(!QTest::currentTestFailed());
        QVERIFY(settleIntoTheStream(client, &ticks));
        QCOMPARE(client.hud()->creditMinutes(), qint64(8));

        client.hud()->tick();
        const QImage frame = client.hud()->compositor().composedBottom();
        QVERIFY2(!frame.isNull(), "an eight-minute first read shows Time left at once");

        int white = 0;
        int red = 0;
        for (int y = 0; y < frame.height(); ++y) {
            for (int x = 0; x < frame.width(); ++x) {
                const QRgb pixel = frame.pixel(x, y);
                if (qAlpha(pixel) != 255) {
                    continue;
                }
                white += (pixel == kOsdValue) ? 1 : 0;
                red += (pixel == kOsdDestructive) ? 1 : 0;
            }
        }
        QVERIFY2(white > 0, "the reminder is white (osdValueDefault)");
        QCOMPARE(red, 0);   // not Critical: eight is above the five-minute final threshold

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
        // The opening read fails at `beginSession()`, which may be before the HUD's session began and
        // so prove nothing about the HUD. The settle's tick is a failed read inside the stream.
        QVERIFY(settleIntoTheStream(client, &ticks));
        QCOMPARE(ticks.failed.load(), 2);

        QCOMPARE(client.hud()->creditMinutes(), qint64(1));
        client.hud()->compositor().showStartHint(false); // the start hint (SC13) is not under test
        client.hud()->tick();
        QVERIFY2(client.hud()->compositor().composedBottom().isNull(),
                 "the seed alone must not show Time left (D-16) - a failed read fires nothing");

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

    // --- Plan 15 (Task 2): the facade's own telemetry identity wiring --------------------------

    void identityAfterInteractiveSignInSetsUserAndIssuesOneTelemetryRequest()
    {
        SeatHubClient client;
        isolateStore(client);
        armControlPlane(client);
        m_fake->answerLogin(200, QByteArrayLiteral("{\"access_token\":\"sb_at_identity\"}"));
        m_fake->answerMe(200, accountBody());
        m_fake->answerWallet(200, walletBody(10));
        const int telemetryBefore = m_fake->countOfPathEndingWith(QStringLiteral("/api/me/telemetry"));

        QSignalSpy accepted(&client, &SeatHubClient::passwordSignInAccepted);
        client.signInWithPassword(QStringLiteral("lina@example.com"), QStringLiteral("hunter22hunter"));
        QTRY_COMPARE_WITH_TIMEOUT(accepted.count(), 1, 15000);

        QTRY_COMPARE_WITH_TIMEOUT(SeatHubTelemetry::currentUserId(),
                                  QStringLiteral("6f1c6f5e-3a1e-4b1e-9f2e-0f1a2b3c4d5e"), 15000);
        QVERIFY(SeatHubTelemetry::signedIn());
        QTRY_COMPARE_WITH_TIMEOUT(
            m_fake->countOfPathEndingWith(QStringLiteral("/api/me/telemetry")),
            telemetryBefore + 1, 15000);
    }

    void identityAfterRestoreSetsUserAndIssuesOneTelemetryRequest()
    {
        SeatHubClient client;
        isolateStore(client);
        const int telemetryBefore = m_fake ? m_fake->countOfPathEndingWith(QStringLiteral("/api/me/telemetry")) : 0;

        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());

        QCOMPARE(SeatHubTelemetry::currentUserId(),
                 QStringLiteral("6f1c6f5e-3a1e-4b1e-9f2e-0f1a2b3c4d5e"));
        QVERIFY(SeatHubTelemetry::signedIn());
        QTRY_COMPARE_WITH_TIMEOUT(
            m_fake->countOfPathEndingWith(QStringLiteral("/api/me/telemetry")),
            telemetryBefore + 1, 15000);
    }

    void restore401ClearsUser()
    {
        SeatHubClient client;
        QString tokenPath;
        QVERIFY(storeACredential(client, &tokenPath));
        armControlPlane(client);
        m_fake->answerMe(401, refusedBody());

        // A user identity first, the way a previous process's own restore would have left it -
        // otherwise "cleared" is indistinguishable from "never set".
        SeatHubTelemetry::setUser(QStringLiteral("stale-account-id"));

        client.restoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("signed_out"), 15000);

        QVERIFY(SeatHubTelemetry::currentUserId().isEmpty());
        QVERIFY(!SeatHubTelemetry::signedIn());
    }

    void beginSessionSetsSessionIdAndSessionStateAddsHostId()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());

        QCOMPARE(SeatHubTelemetry::currentSessionId(), QStringLiteral("s-stages"));
        QVERIFY(SeatHubTelemetry::currentHostId().isEmpty());

        SessionInfo info = sessionIn(QStringLiteral("PREPARING"));
        info.hostId = QStringLiteral("host-42");
        report(client, info);

        QCOMPARE(SeatHubTelemetry::currentHostId(), QStringLiteral("host-42"));
        QCOMPARE(SeatHubTelemetry::currentSessionId(), QStringLiteral("s-stages"));
    }

    void beginPlayRequestSetsTraceIdAndIssuesOneTelemetryRequest()
    {
        SeatHubClient client;
        auto* engine = new FakeEngineSession;
        client.session()->attachSession(engine);
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());

        const int telemetryBefore = m_fake->countOfPathEndingWith(QStringLiteral("/api/me/telemetry"));
        const int beforePlay = m_fake->requestPaths().size();

        // The rig stays "not ready" (409) rather than failing pairing outright (the default 404):
        // this Play's trace id must still be in place a moment later, which a failed pairing
        // would already have cleared (`clearThisPlaysTraceIdAfterAnyQueuedLivenessReport()`).
        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));
        m_fake->answerPlay(201, QByteArrayLiteral("{\"id\":\"s-telemetry-play\"}"));
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("connecting"), 15000);
        emit engine->connectionStarted();
        QVERIFY2(!client.attachedSessionId().isEmpty(), "a session that streamed stays attached");

        QVERIFY(!SeatHubTelemetry::currentTraceId().isEmpty());
        // The session create is still the first request a Play makes (the existing trace-id tests
        // index requests the same way) - the telemetry refresh rides right after it.
        QVERIFY(m_fake->requestPaths().size() > beforePlay);
        QCOMPARE(m_fake->requestPaths().at(beforePlay), QStringLiteral("/api/sessions"));
        QTRY_COMPARE_WITH_TIMEOUT(
            m_fake->countOfPathEndingWith(QStringLiteral("/api/me/telemetry")),
            telemetryBefore + 1, 15000);
    }

    void signOutClearsUserSessionAndTrace()
    {
        SeatHubClient client;
        auto* engine = new FakeEngineSession;
        client.session()->attachSession(engine);
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());

        // Kept pending (409), not failed (the default 404): the trace id this Play minted must
        // still be current when sign-out clears it, not already cleared by a failed pairing.
        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));
        m_fake->answerPlay(201, QByteArrayLiteral("{\"id\":\"s-signout\"}"));
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("connecting"), 15000);
        emit engine->connectionStarted();
        QVERIFY2(!client.attachedSessionId().isEmpty(), "a session that streamed stays attached");

        QVERIFY(!SeatHubTelemetry::currentUserId().isEmpty());
        QCOMPARE(SeatHubTelemetry::currentSessionId(), QStringLiteral("s-signout"));
        QVERIFY(!SeatHubTelemetry::currentTraceId().isEmpty());
        QVERIFY(SeatHubTelemetry::signedIn());

        client.signOut();

        QVERIFY(SeatHubTelemetry::currentUserId().isEmpty());
        QVERIFY(SeatHubTelemetry::currentSessionId().isEmpty());
        QVERIFY(SeatHubTelemetry::currentTraceId().isEmpty());
        QVERIFY(!SeatHubTelemetry::signedIn());
    }

    void teardownClearsSessionAndTrace()
    {
        SeatHubClient client;
        auto* engine = new FakeEngineSession;
        client.session()->attachSession(engine);
        client.controlPlane()->setBaseUrl(QStringLiteral("https://control.invalid"));
        client.controlPlane()->setAccessToken(QString::fromLatin1(kAccessToken));
        client.teardown()->setVerifyIntervalMs(1);

        QString tokenPath;
        QVERIFY(storeACredential(client, &tokenPath));

        client.beginSession(QStringLiteral("session-td"));
        SeatHubTelemetry::setTrace(QStringLiteral("11111111111111111111111111111111"));
        armControlPlane(client);

        QSignalSpy completed(client.teardown(), &TeardownController::teardownCompleted);
        emit engine->readyForDeletion();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 15000);

        // The clearing is an effect of the teardown finishing, not of the signal itself: wait on it.
        QTRY_VERIFY_WITH_TIMEOUT(SeatHubTelemetry::currentSessionId().isEmpty(), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(SeatHubTelemetry::currentHostId().isEmpty(), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(SeatHubTelemetry::currentTraceId().isEmpty(), 15000);
    }

    // --- 06.1-19/J-07: a dropped stream comes back on the same session ---------------------------

    void aGracefulQuitStillEnds()
    {
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        WalletTicks ticks;
        beginStreaming(client, engine, &ticks, 90, 80);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-live"), QStringLiteral("COMPLETED"),
                              QStringLiteral("CUSTOMER_ENDED"));

        emitConnectionTerminated(0); // ML_ERROR_GRACEFUL_TERMINATION - a real code, not "no error"
        emit engine->displayLaunchError(QStringLiteral("Connection terminated"));
        QCOMPARE(client.appState(), QStringLiteral("streaming")); // Moonlight's own message stays; no SeatHub error view

        emit engine->readyForDeletion();
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-live/end")), 15000);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-live/end")), 1);
    }

    void anOfflineEndIsSavedAndResentUntilConfirmed()
    {
        FakeEngineSession engine;
        WalletTicks ticks;
        SeatHubClient client;
        client.setReconnectJitterProvider([]() { return 0.0; });
        beginStreaming(client, &engine, &ticks, 90, 80);
        client.teardown()->setVerifyIntervalMs(1);

        const QString outboxFile =
            QDir(m_dir->path()).filePath(QStringLiteral("end-outbox/s-live.json"));
        QVERIFY(!QFile::exists(outboxFile));

        // The control plane is unreachable for this End.
        m_fake->answerEnd(0);

        emitConnectionTerminated(0);
        emit engine.displayLaunchError(QStringLiteral("Connection terminated"));
        emit engine.readyForDeletion();

        // The End is on disk, tagged to the account, even though no POST could be answered.
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(outboxFile), 5000);
        QVERIFY(client.hasSavedEnd());
        QCOMPARE(client.homeStatus(), QStringLiteral("ending"));
        {
            QFile file(outboxFile);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QJsonObject entry = QJsonDocument::fromJson(file.readAll()).object();
            QCOMPARE(entry.value(QStringLiteral("account_id")).toString(),
                     QStringLiteral("6f1c6f5e-3a1e-4b1e-9f2e-0f1a2b3c4d5e"));
            QCOMPARE(entry.value(QStringLiteral("session_id")).toString(), QStringLiteral("s-live"));
        }

        // The server answers 2xx; the next attempt drops the record and Home is ready again.
        m_fake->answerEnd(200);
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(outboxFile), 10000);
        QVERIFY(!client.hasSavedEnd());
        QCOMPARE(client.homeStatus(), QStringLiteral("ready"));
    }

    void aSavedEndSurvivesARestartAndIsSentBeforePlay()
    {
        // Exactly the record a crashed run leaves behind on disk - written before this run starts.
        const QString outboxDir = QDir(m_dir->path()).filePath(QStringLiteral("end-outbox"));
        const QString outboxFile = QDir(outboxDir).filePath(QStringLiteral("s-live.json"));
        QVERIFY(QDir().mkpath(outboxDir));
        {
            QFile file(outboxFile);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(QJsonDocument(QJsonObject{
                { QStringLiteral("account_id"),
                  QStringLiteral("6f1c6f5e-3a1e-4b1e-9f2e-0f1a2b3c4d5e") },
                { QStringLiteral("session_id"), QStringLiteral("s-live") },
                { QStringLiteral("failed"), false },
            }).toJson(QJsonDocument::Compact));
        }

        // The restart: a fresh client signs in and finds the saved End on the way in.
        SeatHubClient client;
        client.setReconnectJitterProvider([]() { return 0.0; });
        reachHome(client, 90);

        // It was re-sent and confirmed as part of signing in, before any Play.
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(outboxFile), 10000);
        QVERIFY(!client.hasSavedEnd());
        QVERIFY(m_fake->countOfPathEndingWith(QStringLiteral("/end")) >= 1);

        // Nothing is re-sent at Play: the record was already confirmed before it.
        const int endsBeforePlay = m_fake->countOfPathEndingWith(QStringLiteral("/end"));
        client.start();
        QTest::qWait(200);
        QCOMPARE(m_fake->countOfPathEndingWith(QStringLiteral("/end")), endsBeforePlay);
    }

    void aRestoredEndBlocksPlayAndFollowsTheOutboxAnswers()
    {
        SeatHubClient client;
        client.setReconnectJitterProvider([]() { return 0.0; });
        reachHome(client, 90);

        // A record a crashed run left behind, standing in for the restart (written after sign-in so
        // the server can withhold its answer).
        const QString outboxDir = QDir(m_dir->path()).filePath(QStringLiteral("end-outbox"));
        const QString outboxFile = QDir(outboxDir).filePath(QStringLiteral("s-live.json"));
        QVERIFY(QDir().mkpath(outboxDir));
        {
            QFile file(outboxFile);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(QJsonDocument(QJsonObject{
                { QStringLiteral("account_id"),
                  QStringLiteral("6f1c6f5e-3a1e-4b1e-9f2e-0f1a2b3c4d5e") },
                { QStringLiteral("session_id"), QStringLiteral("s-live") },
                { QStringLiteral("failed"), false },
            }).toJson(QJsonDocument::Compact));
        }

        // The server withholds its answer; a network return starts the send at once, not after the
        // backoff the cadence would otherwise have reached.
        m_fake->answerEnd(0);
        QMetaObject::invokeMethod(&client, "onNetworkBack");
        QTRY_VERIFY_WITH_TIMEOUT(client.hasSavedEnd(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(client.homeStatus(), QStringLiteral("ending"), 10000);

        // Play while the End is unconfirmed asks the server for nothing.
        const int playsBefore = m_fake->countOfPathEndingWith(QStringLiteral("/api/sessions"));
        client.start();
        QTest::qWait(200);
        QCOMPARE(m_fake->countOfPathEndingWith(QStringLiteral("/api/sessions")), playsBefore);
        QCOMPARE(client.homeStatus(), QStringLiteral("ending"));

        // 401 is not a confirmation: the record stays and Home stays ending.
        m_fake->answerEnd(401);
        QTest::qWait(1500);
        QVERIFY(client.hasSavedEnd());
        QCOMPARE(client.homeStatus(), QStringLiteral("ending"));

        // 404 says the server holds no such session, so the record is done and Home is ready.
        m_fake->answerEnd(404);
        const int endsBefore = m_fake->countOfPathEndingWith(QStringLiteral("/end"));
        QMetaObject::invokeMethod(&client, "onNetworkBack");
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->countOfPathEndingWith(QStringLiteral("/end")) > endsBefore, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!client.hasSavedEnd(), 15000);
        QCOMPARE(client.homeStatus(), QStringLiteral("ready"));
    }

    void anEndIsOnDiskBeforeTheFirstEndRequestIsSent()
    {
        // V-OUTBOX: the durable record exists on disk before the first End POST leaves the machine.
        // Play ends a never-streamed session before allocating, and that caller writes the record
        // first too - the probe reads the disk as the request arrives, before any answer.
        SeatHubClient client;
        auto* engine = new FakeEngineSession;
        client.session()->attachSession(engine);
        client.setReconnectJitterProvider([]() { return 0.0; });
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());

        // Keep pairing from resolving, and let the pre-stream teardown time out, so the session stays
        // attached on Home: the Play below is the caller that must end it before it allocates.
        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("ACTIVE"));
        client.teardown()->setVerifyIntervalMs(1);
        client.teardown()->setTeardownGraceMs(1);
        client.beginSession(QStringLiteral("s-stages"));
        emit engine->displayLaunchError(QStringLiteral("boom"));
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("error"), 15000);
        client.dismissError();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(client.attachedSessionId(), QStringLiteral("s-stages"), 15000);

        const QString outboxFile =
            QDir(m_dir->path()).filePath(QStringLiteral("end-outbox/s-stages.json"));
        m_fake->setEndProbe([outboxFile]() { return QFile::exists(outboxFile); });
        m_fake->answerEnd(0);  // the server never answers; the probe is what this test reads

        const int endsBefore = m_fake->countOfPathEndingWith(QStringLiteral("/end"));
        client.start();
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->countOfPathEndingWith(QStringLiteral("/end")) > endsBefore, 15000);
        QVERIFY(m_fake->endOutboxExistedAtFirstEnd());
        QVERIFY(QFile::exists(outboxFile));
        QVERIFY(client.hasSavedEnd());
        // Nothing was allocated while the End is unconfirmed.
        QCOMPARE(client.homeStatus(), QStringLiteral("ending"));
        QCOMPARE(m_fake->countOfPathEndingWith(QStringLiteral("/api/sessions")), 0);
    }

    void anUnwritableEndOutboxKeepsTheEndUnconfirmedAndBlocksPlay()
    {
        // If the End cannot be written to disk the app must not claim it saved, and Play must not
        // allocate while the session is un-ended: the failure reaches the customer through the
        // ordinary failure screen, never a ready Home.
        SeatHubClient client;
        auto* engine = new FakeEngineSession;
        client.session()->attachSession(engine);
        client.setReconnectJitterProvider([]() { return 0.0; });
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());

        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("ACTIVE"));
        client.teardown()->setVerifyIntervalMs(1);
        client.teardown()->setTeardownGraceMs(1);
        client.beginSession(QStringLiteral("s-stages"));
        emit engine->displayLaunchError(QStringLiteral("boom"));
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("error"), 15000);
        client.dismissError();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(client.attachedSessionId(), QStringLiteral("s-stages"), 15000);

        // A regular file where the outbox directory should be: nothing can be written under it.
        const QString blocker = QDir(m_dir->path()).filePath(QStringLiteral("blocked"));
        {
            QFile file(blocker);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write("not a directory");
        }
        client.setEndOutboxDirectory(QDir(blocker).filePath(QStringLiteral("end-outbox")));
        m_fake->answerEnd(0);

        const int playsBefore = m_fake->countOfPathEndingWith(QStringLiteral("/api/sessions"));
        client.start();

        // The teardown raises the failure the customer sees; Home is never ready.
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("error"), 15000);
        QVERIFY(!client.hasSavedEnd());
        QVERIFY(client.homeStatus() != QStringLiteral("ready"));
        QTest::qWait(200);
        QCOMPARE(m_fake->countOfPathEndingWith(QStringLiteral("/api/sessions")), playsBefore);
    }

    void anEndAnswerOnlyClearsTheRecordItAnswers()
    {
        // The acknowledgement is scoped to the session it names: a late answer for another session
        // (or another account's record) must not clear the End this account is still waiting on.
        SeatHubClient client;
        client.setReconnectJitterProvider([]() { return 0.0; });
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());

        const QString outboxDir = QDir(m_dir->path()).filePath(QStringLiteral("end-outbox"));
        QVERIFY(QDir().mkpath(outboxDir));
        {
            QFile file(QDir(outboxDir).filePath(QStringLiteral("s-live.json")));
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(QJsonDocument(QJsonObject{
                { QStringLiteral("account_id"),
                  QStringLiteral("6f1c6f5e-3a1e-4b1e-9f2e-0f1a2b3c4d5e") },
                { QStringLiteral("session_id"), QStringLiteral("s-live") },
                { QStringLiteral("failed"), false },
            }).toJson(QJsonDocument::Compact));
        }

        m_fake->answerEnd(0);
        QMetaObject::invokeMethod(&client, "onNetworkBack");
        QTRY_VERIFY_WITH_TIMEOUT(client.hasSavedEnd(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(client.homeStatus(), QStringLiteral("ending"), 10000);

        // An answer for a different session leaves this account's record and Home alone.
        QMetaObject::invokeMethod(&client, "handleTeardownEndAnswered",
                                  Q_ARG(QString, QStringLiteral("s-other")), Q_ARG(int, 200));
        QVERIFY(client.hasSavedEnd());
        QCOMPARE(client.homeStatus(), QStringLiteral("ending"));

        // The answer for this session is the one that finishes it. Home's own return to ready is the
        // teardown's confirmation of the session's end, which this direct ack does not stand in for.
        QMetaObject::invokeMethod(&client, "handleTeardownEndAnswered",
                                  Q_ARG(QString, QStringLiteral("s-live")), Q_ARG(int, 200));
        QVERIFY(!client.hasSavedEnd());
    }

    void anUnwritableEndOutboxOnAStreamedSessionKeepsPlayBlocked()
    {
        // V-OUTBOX/C-ENDING (AC3): the ordinary streamed quit path. When the End cannot be written
        // to disk it is remembered for this run only; the failed send surfaces the ordinary failure
        // screen, and dismissing it must not make Play ready over a session the server may still
        // hold. A repaired destination makes the End durable again and unblocks Home on the answer.
        SeatHubClient client;
        auto* engine = new FakeEngineSession;
        client.setReconnectJitterProvider([]() { return 0.0; });
        WalletTicks ticks;
        beginStreaming(client, engine, &ticks, 90, 80);
        QVERIFY(!QTest::currentTestFailed());
        QCOMPARE(client.appState(), QStringLiteral("streaming"));

        // A regular file where the outbox directory should be: nothing can be written under it.
        const QString blocker = QDir(m_dir->path()).filePath(QStringLiteral("blocked-streamed"));
        {
            QFile file(blocker);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write("not a directory");
        }
        client.setEndOutboxDirectory(QDir(blocker).filePath(QStringLiteral("end-outbox")));
        m_fake->answerEnd(0);

        emitConnectionTerminated(0);
        emit engine->displayLaunchError(QStringLiteral("Connection terminated"));
        emit engine->readyForDeletion();

        // The End never reached the server, so the teardown failed and the customer sees the ordinary
        // failure screen - and no durable record exists, only the in-memory intent.
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("error"), 15000);
        QVERIFY(!client.hasSavedEnd());

        client.dismissError();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);
        QCOMPARE(client.homeStatus(), QStringLiteral("ending"));

        // Play is pressed over and over while the End is unconfirmed: no new session is allocated.
        const int playsBefore = m_fake->countOfPathEndingWith(QStringLiteral("/api/sessions"));
        client.start();
        QTest::qWait(300);
        client.start();
        QTest::qWait(300);
        QCOMPARE(m_fake->countOfPathEndingWith(QStringLiteral("/api/sessions")), playsBefore);
        QVERIFY(client.appState() != QStringLiteral("connecting"));
        QVERIFY(client.homeStatus() != QStringLiteral("ready"));

        // The destination is repaired and the network returns: the intent becomes durable, is sent,
        // and the server's 200 confirms it - only then is Home ready again.
        client.setEndOutboxDirectory(QDir(m_dir->path()).filePath(QStringLiteral("end-outbox")));
        const int endsBefore = m_fake->countOfPathEndingWith(QStringLiteral("/end"));
        m_fake->answerEnd(200);
        QMetaObject::invokeMethod(&client, "onNetworkBack");
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->countOfPathEndingWith(QStringLiteral("/end")) > endsBefore, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!client.hasSavedEnd(), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(client.homeStatus(), QStringLiteral("ready"), 15000);
    }

    void aHostSideCodeStillEnds()
    {
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        WalletTicks ticks;
        beginStreaming(client, engine, &ticks, 90, 80);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-live"), QStringLiteral("COMPLETED"),
                              QStringLiteral("HOST_LOST"));

        emitConnectionTerminated(-102); // ML_ERROR_UNEXPECTED_EARLY_TERMINATION - host-side
        emit engine->displayLaunchError(QStringLiteral("Connection terminated"));
        QCOMPARE(client.appState(), QStringLiteral("streaming")); // Moonlight's own message stays; no SeatHub error view

        emit engine->readyForDeletion();
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-live/end")), 15000);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-live/end")), 1);
    }

    // --- 06.4-15 Task 1: the SseClient's lifecycle and account.state wiring ------------------------

    void theStreamStartsAfterSignInAndStopsAtSignOut()
    {
        SeatHubClient client;
        reachHome(client, 90);

        // 06.4/ADR-0067, D-01: opened the moment sign-in (here, a confirmed restore) is confirmed.
        QTRY_VERIFY_WITH_TIMEOUT(client.sse()->state() != QStringLiteral("idle"), 15000);

        client.signOut();

        // `stop()` sets the state to "idle" and aborts any open reply - no stream request survives
        // a sign-out.
        QTRY_COMPARE_WITH_TIMEOUT(client.sse()->state(), QStringLiteral("idle"), 15000);
    }

    void aPushedAccountStateUpdatesTheBalance()
    {
        SeatHubClient client;
        reachHome(client, 90);

        QSignalSpy balanceSpy(&client, &SeatHubClient::balanceChanged);

        // `feed()` is the test seam and the production entry point alike (sse_client.h's own
        // header comment): a whole `account.state` frame, fed directly.
        client.sse()->feed(QByteArrayLiteral(
            "event: account.state\n"
            "data: {\"topic\":\"account:acc-1\",\"at\":\"2026-09-27T00:00:00Z\","
            "\"account\":{\"wallet\":{\"balance_minutes\":125},\"live_session_id\":\"\","
            "\"open_topup_notice\":null,\"signup_stage\":\"complete\"}}\n"
            "\n"));

        QTRY_COMPARE_WITH_TIMEOUT(client.balanceMinutes(), qint64(125), 15000);
        QCOMPARE(client.balanceText(), QStringLiteral("2 h 05 min"));
        QVERIFY(!client.balanceStale());
        QCOMPARE(balanceSpy.count(), 1);
    }

    void signedBalanceRendersMinus()
    {
        // INTENT SC25, ADR-0075: a balance below zero is a real number, drawn with a leading minus
        // in the same hours-and-minutes shape as a positive one - never `0 min`, never "unknown".
        SeatHubClient client;

        // Before any read: unknown, which reads 0 with no text.
        QCOMPARE(client.balanceMinutes(), qint64(0));
        QVERIFY(client.balanceText().isEmpty());

        // A real -1 is a balance, not the unknown state (the old -1 sentinel is gone).
        reachHome(client, -1);
        QVERIFY(!QTest::currentTestFailed());
        QCOMPARE(client.balanceMinutes(), qint64(-1));
        QCOMPARE(client.balanceText(), QStringLiteral("-1 min"));
        QVERIFY(!client.balanceStale());

        // Wallet reads: -3, -75, 0 and 27 minutes.
        struct Case { int minutes; const char* text; };
        const Case cases[] = { { -3, "-3 min" }, { -75, "-1 h 15 min" }, { 0, "0 min" },
                               { 27, "27 min" } };
        for (const Case& c : cases) {
            m_fake->answerWallet(200, walletBody(c.minutes));
            client.refreshBalance();
            QTRY_COMPARE_WITH_TIMEOUT(client.balanceText(), QString::fromLatin1(c.text), 15000);
            QCOMPARE(client.balanceMinutes(), qint64(c.minutes));
            QVERIFY(!client.balanceStale());
        }

        // A pushed `account.state` carries the negative balance unclamped.
        client.sse()->feed(QByteArrayLiteral(
            "event: account.state\n"
            "data: {\"topic\":\"account:acc-1\",\"at\":\"2026-09-27T00:00:00Z\","
            "\"account\":{\"wallet\":{\"balance_minutes\":-3},\"live_session_id\":\"\","
            "\"open_topup_notice\":null,\"signup_stage\":\"complete\"}}\n"
            "\n"));
        QTRY_COMPARE_WITH_TIMEOUT(client.balanceText(), QStringLiteral("-3 min"), 15000);
        QCOMPARE(client.balanceMinutes(), qint64(-3));

        // So does the fallback `GET /api/account-state` read.
        client.setAccountFallbackMs(50);
        m_fake->answerAccountState(200, QByteArrayLiteral(
            "{\"topic\":\"account:acc-1\",\"at\":\"2026-09-27T00:00:00Z\","
            "\"account\":{\"wallet\":{\"balance_minutes\":-75},\"live_session_id\":\"\","
            "\"open_topup_notice\":null,\"signup_stage\":\"complete\"}}"));
        emit client.sse()->pausedChanged(true);
        QTRY_COMPARE_WITH_TIMEOUT(client.balanceText(), QStringLiteral("-1 h 15 min"), 15000);
        QCOMPARE(client.balanceMinutes(), qint64(-75));
        emit client.sse()->pausedChanged(false);

        // Signing out forgets it: the next customer never sees this one's debt.
        client.signOut();
        QCOMPARE(client.balanceMinutes(), qint64(0));
        QVERIFY(client.balanceText().isEmpty());
    }

    void aChangedTopupNoticeReloadsTheProfileLists()
    {
        SeatHubClient client;
        reachProfile(client, 90);

        const int topupsBefore =
            m_fake->countOfPathEndingWith(QStringLiteral("/api/topup-notices"));
        const int usageBefore = m_fake->countOfPathEndingWith(QStringLiteral("/api/usage"));

        const QByteArray noticeOpen = QByteArrayLiteral(
            "event: account.state\n"
            "data: {\"topic\":\"account:acc-1\",\"at\":\"2026-09-27T00:00:00Z\","
            "\"account\":{\"wallet\":{\"balance_minutes\":90},\"live_session_id\":\"\","
            "\"open_topup_notice\":{\"id\":\"notice-1\"},\"signup_stage\":\"complete\"}}\n"
            "\n");
        client.sse()->feed(noticeOpen);

        QTRY_COMPARE_WITH_TIMEOUT(
            m_fake->countOfPathEndingWith(QStringLiteral("/api/topup-notices")),
            topupsBefore + 1, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(
            m_fake->countOfPathEndingWith(QStringLiteral("/api/usage")), usageBefore + 1, 15000);

        // The identical notice again reloads nothing a second time.
        client.sse()->feed(noticeOpen);
        QTest::qWait(50);
        QCOMPARE(m_fake->countOfPathEndingWith(QStringLiteral("/api/topup-notices")),
                 topupsBefore + 1);
        QCOMPARE(m_fake->countOfPathEndingWith(QStringLiteral("/api/usage")), usageBefore + 1);
    }

    // --- 06.4-15 Task 2: the connecting wake-up, the paused fallback read, and the revoked -------
    // --- notice-then-sign-out (deferred during a stream) -------------------------------------------

    void aSessionStateWakesThePairingPollAtOnce()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());

        // The first poll (from `start()`) has already landed; a very long interval means only
        // `pollNow()` could produce another one inside this test's own window.
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-stages/pairing")),
            15000);
        client.pairing()->setPollIntervalMs(60000);
        const int pairingBefore = m_fake->countOfPathEndingWith(QStringLiteral("/pairing"));

        // A session.state for a different session does not wake it.
        emit client.sse()->sessionState(
            sessionIn(QStringLiteral("PREPARING"), QStringLiteral("not-s-stages")));
        QTest::qWait(100);
        QCOMPARE(m_fake->countOfPathEndingWith(QStringLiteral("/pairing")), pairingBefore);

        // A session.state for the session being connected wakes it at once.
        emit client.sse()->sessionState(
            sessionIn(QStringLiteral("PREPARING"), QStringLiteral("s-stages")));
        QTRY_COMPARE_WITH_TIMEOUT(m_fake->countOfPathEndingWith(QStringLiteral("/pairing")),
                                  pairingBefore + 1, 15000);
    }

    void aPausedStreamReadsTheAccountEveryTenSecondsWithTheMarker()
    {
        SeatHubClient client;
        reachHome(client, 90);
        client.setAccountFallbackMs(50);

        QSignalSpy pausedSpy(&client, &SeatHubClient::liveUpdatesPausedChanged);

        m_fake->answerAccountState(200, QByteArrayLiteral(
            "{\"topic\":\"account:acc-1\",\"at\":\"2026-09-27T00:00:00Z\","
            "\"account\":{\"wallet\":{\"balance_minutes\":77},\"live_session_id\":\"\","
            "\"open_topup_notice\":null,\"signup_stage\":\"complete\"}}"));

        // The fallback read runs on the channel's L5 trigger without showing the indicator (R7).
        emit client.sse()->pausedChanged(true);
        QCOMPARE(pausedSpy.count(), 0);
        QVERIFY(!client.liveUpdatesPaused());

        QTRY_COMPARE_WITH_TIMEOUT(client.balanceMinutes(), qint64(77), 15000);
        QCOMPARE(m_fake->streamFallbackHeaderFor(QStringLiteral("/api/account-state")),
                 QByteArrayLiteral("seathub"));

        // The indicator follows the channel's own "no hello" loss signal, not the fallback trigger.
        emit client.sse()->liveUpdatesLostChanged(true);
        QCOMPARE(pausedSpy.count(), 1);
        QVERIFY(client.liveUpdatesPaused());
        emit client.sse()->liveUpdatesLostChanged(false);
        QCOMPARE(pausedSpy.count(), 2);
        QVERIFY(!client.liveUpdatesPaused());

        // Live again: the reads stop.
        const int readsBeforeLive =
            m_fake->countOfPathEndingWith(QStringLiteral("/api/account-state"));
        emit client.sse()->pausedChanged(false);
        QTest::qWait(150);
        QCOMPARE(m_fake->countOfPathEndingWith(QStringLiteral("/api/account-state")),
                 readsBeforeLive);

        // A 401 on the fallback read starts the signed-out sequence, not an immediate sign-out.
        client.setSignedOutNoticeMs(60000);
        m_fake->answerAccountState(401, refusedBody());
        emit client.sse()->pausedChanged(true);
        QTRY_VERIFY_WITH_TIMEOUT(client.signedOutNotice(), 15000);
        QVERIFY2(client.signedIn(), "the 401 must start the notice, not sign out at once");
    }

    void aRevokedStreamShowsTheNoticeThenSignsOut()
    {
        SeatHubClient client;
        reachHome(client, 90);
        client.setSignedOutNoticeMs(200);

        QSignalSpy noticeSpy(&client, &SeatHubClient::signedOutNoticeChanged);
        emit client.sse()->revoked();

        QVERIFY2(client.signedOutNotice(), "the notice must show right after revoked()");
        QVERIFY2(client.signedIn(), "the client must still be signed in during the notice");

        // A second revoked() during the wait starts no second sign-out.
        emit client.sse()->revoked();
        QVERIFY(client.signedIn());

        QTRY_VERIFY_WITH_TIMEOUT(!client.signedIn(), 15000);
        QVERIFY(!client.signedOutNotice());
        QCOMPARE(client.appState(), QStringLiteral("signed_out"));
        // The notice's own property goes true once, then false once - never a second true.
        QCOMPARE(noticeSpy.count(), 2);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/auth/logout")), 1);
    }

    void aRevokedStreamDuringAStreamWaitsForItsEnd()
    {
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        WalletTicks ticks;
        beginStreaming(client, engine, &ticks, 90, 80);
        QVERIFY(!QTest::currentTestFailed());
        client.setSignedOutNoticeMs(200);
        m_fake->answerSession(QStringLiteral("s-live"), QStringLiteral("CANCELLED"),
                              QStringLiteral("CUSTOMER_ENDED"));

        emit client.sse()->revoked();
        QTest::qWait(100);
        QVERIFY2(!client.signedOutNotice(), "the notice must wait for the stream to end");
        QVERIFY2(client.signedIn(), "revoked() must not sign out mid-stream");

        client.interrupt();
        emit engine->sessionFinished(0);
        emit engine->readyForDeletion();

        // The same delayed sequence now runs once: the notice, then - after L9 - the sign-out.
        // Never immediately, even once the stream has ended.
        QVERIFY2(client.signedIn(), "must not sign out the instant the stream ends either");
        QTRY_VERIFY_WITH_TIMEOUT(!client.signedIn(), 15000);
        QCOMPARE(client.appState(), QStringLiteral("signed_out"));
    }

    void aSignOutDuringTheNoticeCancelsIt()
    {
        SeatHubClient client;
        reachHome(client, 90);
        client.setSignedOutNoticeMs(200);

        emit client.sse()->revoked();
        QVERIFY(client.signedOutNotice());

        const int logoutsBefore =
            m_fake->requestPaths().count(QStringLiteral("/api/auth/logout"));
        client.signOut();

        QVERIFY(!client.signedOutNotice());
        QCOMPARE(client.appState(), QStringLiteral("signed_out"));

        // Past where the notice would have elapsed on its own: no second sign-out follows.
        QTest::qWait(400);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/auth/logout")),
                 logoutsBefore + 1);
    }

    // --- ADR-0074 X02: the Play request names the client certificate ---------------------------

    void thePlayRequestNamesTheClientCertificateTheAppPairsWith()
    {
        g_ownCertFingerprint = QStringLiteral("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
        const auto reset = qScopeGuard([]() { g_ownCertFingerprint.clear(); });

        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());
        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));
        m_fake->answerPlay(201, QByteArrayLiteral("{\"id\":\"s-x02\"}"));

        client.start();
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->requestPaths().contains(QStringLiteral("/api/sessions")), 15000);

        const QJsonObject sent =
            QJsonDocument::fromJson(m_fake->bodyFor(QStringLiteral("/api/sessions"))).object();
        QCOMPARE(sent.value(QStringLiteral("client_cert_sha256")).toString(), g_ownCertFingerprint);
        QCOMPARE(sent.size(), 1); // no quality, no rig, nothing else
    }

    // --- ADR-0074 W03: the warm return ---------------------------------------------------------

    void aWarmReturnAuthorizationSkipsTheHandshakeAndStillStartsTheStream()
    {
        g_warmCheckArmed = true;
        g_noAppHandshakeArmed = true; // if the normal handshake runs, it too pairs: the counters tell
        g_warmChecks = 0;
        g_plainHandshakeRuns = 0;
        const auto disarm = qScopeGuard([]() {
            g_warmCheckArmed = false;
            g_noAppHandshakeArmed = false;
            g_warmChecks = 0;
            g_plainHandshakeRuns = 0;
            HeldRigTrust::clear();
        });
        HeldRigTrust::remember(QByteArrayLiteral("pinned earlier in this run"));

        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());
        auto engine = std::make_unique<FakeEngineSession>();
        g_engineCreateReason = EngineLaunchReason::Started;
        g_engineFactoryHook = [&client, enginePtr = engine.get()]() {
            client.session()->attachSession(enginePtr);
        };
        const auto clearFactoryHook = qScopeGuard([]() {
            g_engineFactoryHook = {};
            g_engineCreateReason = EngineLaunchReason::CreateFailed;
        });

        QJsonObject ports;
        ports.insert(QStringLiteral("https"), 47984);
        ports.insert(QStringLiteral("control"), 47989);
        ports.insert(QStringLiteral("rtsp"), 48010);
        QJsonObject authBody;
        authBody.insert(QStringLiteral("session_id"), QStringLiteral("s-warm"));
        authBody.insert(QStringLiteral("pairing_pin"), QStringLiteral("4821"));
        authBody.insert(QStringLiteral("host_address"), QStringLiteral("127.0.0.1"));
        authBody.insert(QStringLiteral("ports"), ports);
        authBody.insert(QStringLiteral("state"), QStringLiteral("READY"));
        authBody.insert(QStringLiteral("warm_return"), true);
        m_fake->answerPairing(200, QJsonDocument(authBody).toJson(QJsonDocument::Compact));
        m_fake->answerPlay(201, QByteArrayLiteral("{\"id\":\"s-warm\"}"));

        client.beginSession(QStringLiteral("s-warm"));

        // The stream starts (the fake engine was run), through the warm path alone.
        QTRY_VERIFY_WITH_TIMEOUT(!engine->runs.isEmpty(), 20000);
        QCOMPARE(g_warmChecks, 1);
        QCOMPARE(g_plainHandshakeRuns, 0);

        // The fake engine finishes, so the ordinary teardown runs and the lifecycle lets go of it.
        emit engine->readyForDeletion();
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-warm/end")), 15000);
    }

    void aWarmReturnIndicationWithNoHeldTrustPairsNormally()
    {
        g_warmCheckArmed = true;
        g_noAppHandshakeArmed = true;
        g_warmChecks = 0;
        g_plainHandshakeRuns = 0;
        const auto disarm = qScopeGuard([]() {
            g_warmCheckArmed = false;
            g_noAppHandshakeArmed = false;
            g_warmChecks = 0;
            g_plainHandshakeRuns = 0;
        });
        HeldRigTrust::clear(); // a restart, or a sign-out, left nothing

        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());
        auto engine = std::make_unique<FakeEngineSession>();
        g_engineCreateReason = EngineLaunchReason::Started;
        g_engineFactoryHook = [&client, enginePtr = engine.get()]() {
            client.session()->attachSession(enginePtr);
        };
        const auto clearFactoryHook = qScopeGuard([]() {
            g_engineFactoryHook = {};
            g_engineCreateReason = EngineLaunchReason::CreateFailed;
        });

        QJsonObject ports;
        ports.insert(QStringLiteral("https"), 47984);
        ports.insert(QStringLiteral("control"), 47989);
        ports.insert(QStringLiteral("rtsp"), 48010);
        QJsonObject authBody;
        authBody.insert(QStringLiteral("session_id"), QStringLiteral("s-cold"));
        authBody.insert(QStringLiteral("pairing_pin"), QStringLiteral("4821"));
        authBody.insert(QStringLiteral("host_address"), QStringLiteral("127.0.0.1"));
        authBody.insert(QStringLiteral("ports"), ports);
        authBody.insert(QStringLiteral("state"), QStringLiteral("READY"));
        authBody.insert(QStringLiteral("warm_return"), true);
        m_fake->answerPairing(200, QJsonDocument(authBody).toJson(QJsonDocument::Compact));
        m_fake->answerPlay(201, QByteArrayLiteral("{\"id\":\"s-cold\"}"));

        client.beginSession(QStringLiteral("s-cold"));

        QTRY_VERIFY_WITH_TIMEOUT(!engine->runs.isEmpty(), 20000);
        QCOMPARE(g_warmChecks, 0);       // nothing held, so the rig is not even asked
        QCOMPARE(g_plainHandshakeRuns, 1);

        emit engine->readyForDeletion();
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-cold/end")), 15000);
    }

    void aSignOutForgetsTheHeldRigTrustSoTheNextPlayPairsNormally()
    {
        const auto cleanup = qScopeGuard([]() { HeldRigTrust::clear(); });
        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());

        HeldRigTrust::remember(QByteArrayLiteral("pinned earlier in this run"));
        client.signOut();
        QVERIFY2(HeldRigTrust::held().isEmpty(), "the held pin must not outlive the sign-in");
    }

    void aRefusedRestoreForgetsTheHeldRigTrust()
    {
        const auto cleanup = qScopeGuard([]() { HeldRigTrust::clear(); });
        SeatHubClient client;
        isolateStore(client);
        client.controlPlane()->setBaseUrl(QStringLiteral("https://control.invalid"));
        armControlPlane(client);
        m_fake->answerMe(401, refusedBody());
        QVERIFY(client.credentialStore()->storeToken(TokenStore::accessTokenName(),
                                                     QString::fromLatin1(kAccessToken)));
        HeldRigTrust::remember(QByteArrayLiteral("pinned earlier in this run"));

        client.restoreSession();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("signed_out"), 15000);
        QVERIFY(HeldRigTrust::held().isEmpty());
    }

    // --- ADR-0074 HI02 / HI06: delivered stream time -------------------------------------------

    void aStreamedSessionDeliversItsOwnMeasuredTimeToItsOwnSessionAtTeardown()
    {
        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());

        auto* engine = new FakeEngineSession;
        client.session()->attachSession(engine);
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));

        const QString session = QStringLiteral("aaaaaaaa-8888-8888-8888-888888888888");
        const QString path = QStringLiteral("/api/sessions/%1/stream-time").arg(session);
        client.beginSession(session);
        emit engine->connectionStarted();
        QTest::qWait(150);
        // The engine's own quit line, through the real LogTee: the segment ends here, with no
        // statistics overlay anywhere in the picture.
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Quit event received");

        QSignalSpy completed(client.teardown(), &TeardownController::teardownCompleted);
        emit engine->readyForDeletion();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->requestPaths().contains(path), 15000);

        // Every report for this stream went to this session's own route, under the customer token,
        // and says the same closed segment 1: a resend adds nothing.
        const QList<QByteArray> bodies = m_fake->bodiesFor(path);
        QVERIFY(!bodies.isEmpty());
        qint64 firstDelivered = -1;
        for (const QByteArray& raw : bodies) {
            const QJsonObject sent = QJsonDocument::fromJson(raw).object();
            QCOMPARE(sent.size(), 3);
            QCOMPARE(sent.value(QStringLiteral("segment_no")).toInt(), 1);
            const qint64 delivered = sent.value(QStringLiteral("delivered_ms")).toInteger();
            if (sent.value(QStringLiteral("closed")).toBool()) {
                if (firstDelivered < 0) {
                    firstDelivered = delivered;
                }
                QCOMPARE(delivered, firstDelivered);
            }
        }
        QVERIFY2(firstDelivered >= 100 && firstDelivered < 10000, qPrintable(QString::number(firstDelivered)));
        QCOMPARE(m_fake->authorizationFor(path),
                 QByteArray("Bearer ") + QByteArray(kAccessToken));
        QVERIFY2(!m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-newer/stream-time")),
                 "no other session was named");

        // The control plane took it, so nothing is left in the record.
        const QString recordPath =
            QDir(m_dir->path()).filePath(QStringLiteral("stream-time-outbox/%1.json").arg(session));
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(recordPath), 15000);

        client.session()->attachSession(nullptr);
        delete engine;
    }

    void aLostConnectionTakesTheEnginesTenSecondWaitOffTheDeliveredTime()
    {
        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());

        auto* engine = new FakeEngineSession;
        client.session()->attachSession(engine);
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));

        const QString session = QStringLiteral("aaaaaaaa-9999-9999-9999-999999999999");
        const QString path = QStringLiteral("/api/sessions/%1/stream-time").arg(session);
        client.beginSession(session);
        emit engine->connectionStarted();
        QTest::qWait(120);
        // The engine's own account of a lost connection, through the real LogTee: the disconnect
        // line, then the termination, then the quit event. The stream lasted far less than the
        // engine's 10 s wait, so none of it was delivered.
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Control stream received unexpected disconnect event");
        emitConnectionTerminated(-1);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Quit event received");

        QSignalSpy completed(client.teardown(), &TeardownController::teardownCompleted);
        emit engine->readyForDeletion();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->requestPaths().contains(path), 15000);

        for (const QByteArray& raw : m_fake->bodiesFor(path)) {
            const QJsonObject sent = QJsonDocument::fromJson(raw).object();
            if (sent.value(QStringLiteral("closed")).toBool()) {
                QCOMPARE(sent.value(QStringLiteral("delivered_ms")).toInteger(), qint64(0));
            }
        }

        client.session()->attachSession(nullptr);
        delete engine;
    }

    void aKilledRunsRecordedStreamTimeIsReplayedAfterRelaunchToTheSameSessionAndAccountOnly()
    {
        // What a killed run left on disk: a record for one session of THIS account, and one for a
        // session of another account on the same PC.
        const QString dir = QDir(m_dir->path()).filePath(QStringLiteral("stream-time-outbox"));
        const QString ownSession = QStringLiteral("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa");
        const QString otherSession = QStringLiteral("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb");
        const QString ownAccount = QStringLiteral("6f1c6f5e-3a1e-4b1e-9f2e-0f1a2b3c4d5e"); // accountBody()'s id
        {
            StreamTimeOutbox left(dir);
            left.put(ownSession, ownAccount, StreamSegmentReport{ 1, 20000, false });
            left.put(otherSession, QStringLiteral("someone-else"), StreamSegmentReport{ 1, 99000, true });
        }
        const QString ownRecord = QDir(dir).filePath(ownSession + QStringLiteral(".json"));
        const QString otherRecord = QDir(dir).filePath(otherSession + QStringLiteral(".json"));
        QVERIFY(QFile::exists(ownRecord));
        QVERIFY(QFile::exists(otherRecord));

        SeatHubClient client;
        reachHome(client, 90); // the relaunch: restore, then `setAccount()` drains
        QVERIFY(!QTest::currentTestFailed());

        const QString ownPath = QStringLiteral("/api/sessions/%1/stream-time").arg(ownSession);
        const QString otherPath = QStringLiteral("/api/sessions/%1/stream-time").arg(otherSession);
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->requestPaths().contains(ownPath), 15000);

        const QJsonObject sent =
            QJsonDocument::fromJson(m_fake->bodiesFor(ownPath).first()).object();
        QCOMPARE(sent.value(QStringLiteral("segment_no")).toInt(), 1);
        QCOMPARE(sent.value(QStringLiteral("delivered_ms")).toInteger(), qint64(20000));
        QCOMPARE(sent.value(QStringLiteral("closed")).toBool(), false); // replayed as recorded
        QCOMPARE(m_fake->authorizationFor(ownPath), QByteArray("Bearer ") + QByteArray(kAccessToken));

        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(ownRecord), 15000);
        // Another account's record is neither sent under this token nor deleted.
        QTest::qWait(200);
        QVERIFY2(!m_fake->requestPaths().contains(otherPath), "another account's record was sent");
        QVERIFY(QFile::exists(otherRecord));
        QCOMPARE(m_fake->requestPaths().count(ownPath), 1);
    }

    void aSignOutForgetsTheDeliveredTimeClocksSession()
    {
        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());

        auto* engine = new FakeEngineSession;
        client.session()->attachSession(engine);
        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));
        const QString session = QStringLiteral("aaaaaaaa-cccc-cccc-cccc-cccccccccccc");
        client.beginSession(session);
        emit engine->connectionStarted();
        QVERIFY(client.streamTime()->hasOpenSegment());

        client.signOut();
        QVERIFY2(client.streamTime()->sessionId().isEmpty(),
                 "the clock must not carry a signed-out account's session");
        QVERIFY(!client.streamTime()->hasOpenSegment());

        client.session()->attachSession(nullptr);
        delete engine;
    }

    // --- R06 (carried from wave 3): a late teardown posts only its own session's report -------------

    void aLateTeardownPostsOnlyItsOwnSessionsQualityReportNeverTheNewerAttemptsOne()
    {
        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());
        auto* engine = new FakeEngineSession;
        client.session()->attachSession(engine);
        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));

        const QString oldSession = QStringLiteral("aaaaaaaa-d1d1-d1d1-d1d1-d1d1d1d1d1d1");
        const QString newSession = QStringLiteral("aaaaaaaa-e2e2-e2e2-e2e2-e2e2e2e2e2e2");
        const QString oldQuality = QStringLiteral("/api/sessions/%1/quality").arg(oldSession);
        const QString newQuality = QStringLiteral("/api/sessions/%1/quality").arg(newSession);

        // The old session streamed and its stats block is held; then a newer attempt is attached and
        // ITS stats block replaces the held one (the report is always the latest parsed).
        client.beginSession(oldSession);
        emit engine->connectionStarted();
        client.beginSession(newSession);
        emit engine->connectionStarted();
        emitStatsBlock(44.0); // bound to the newer session, the one attached now

        SessionInfo old = sessionIn(QStringLiteral("COMPLETED"), oldSession);
        old.endReason = QStringLiteral("CUSTOMER_ENDED");

        // The OLD session's teardown finishes late: it is not the newer attempt's to post for.
        emit client.teardown()->teardownCompleted(old, oldSession);
        emit client.teardown()->teardownFailed(SeatHubFailure::local(QStringLiteral("late")), oldSession);
        QTest::qWait(300);
        QVERIFY2(!m_fake->requestPaths().contains(newQuality),
                 "a late teardown of an ended session posted the newer attempt's quality report");
        QVERIFY(!m_fake->requestPaths().contains(oldQuality));
        // The old session has no report left behind, and the old teardown did not drain newSession's write-ahead report
        QVERIFY(!QFile::exists(QDir(m_dir->path()).filePath(
            QStringLiteral("quality-outbox/%1.json").arg(oldSession))));

        // The newer attempt's own teardown still posts it, once.
        SessionInfo current = sessionIn(QStringLiteral("COMPLETED"), newSession);
        emit client.teardown()->teardownCompleted(current, newSession);
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->requestPaths().contains(newQuality), 15000);
        QCOMPARE(m_fake->requestPaths().count(newQuality), 1);

        client.session()->attachSession(nullptr);
        delete engine;
    }

    void aLateTeardownStillPostsTheEndedSessionsOwnQualityReport()
    {
        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());
        auto* engine = new FakeEngineSession;
        client.session()->attachSession(engine);
        m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                   QStringLiteral("SH-2K2XQ1")));

        const QString oldSession = QStringLiteral("aaaaaaaa-f3f3-f3f3-f3f3-f3f3f3f3f3f3");
        const QString newSession = QStringLiteral("aaaaaaaa-a4a4-a4a4-a4a4-a4a4a4a4a4a4");
        const QString oldQuality = QStringLiteral("/api/sessions/%1/quality").arg(oldSession);

        client.beginSession(oldSession);
        emit engine->connectionStarted();
        emitStatsBlock(47.0); // bound to the old session
        client.beginSession(newSession);

        SessionInfo old = sessionIn(QStringLiteral("COMPLETED"), oldSession);
        emit client.teardown()->teardownCompleted(old, oldSession);
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->requestPaths().contains(oldQuality), 15000);
        QCOMPARE(m_fake->requestPaths().count(oldQuality), 1);

        client.session()->attachSession(nullptr);
        delete engine;
    }

    // --- Moonlight ends the stream (SC30, ADR-0090) ----------------------------------------------
    // SeatHub starts no reconnect, draws no text of its own, and ends the rental once through the
    // existing End path.

    void aMoonlightEndedStreamQueuesOneEndAndReturnsToPlay()
    {
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        WalletTicks ticks;
        beginStreaming(client, engine, &ticks, 90, 80);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-live"), QStringLiteral("COMPLETED"),
                              QStringLiteral("HOST_LOST"));

        emitConnectionTerminated(-1); // ENet's own "unexpected disconnect"
        emit engine->displayLaunchError(QStringLiteral("Connection terminated"));
        emit engine->readyForDeletion();

        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-live/end")), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-live/end")), 1);
        // No reconnect was entered: no second pairing round for the same session.
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-live/pairing")), 1);
    }

    void aStartOfStreamNoVideoErrorEndsThroughTheEndPath()
    {
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"));
        client.session()->attachSession(engine);
        client.session()->start(nullptr);

        emitConnectionTerminated(-100); // ML_ERROR_NO_VIDEO_TRAFFIC, before the first frame
        emit engine->displayLaunchError(QStringLiteral("No video received from host."));
        emit engine->readyForDeletion();

        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-stages/end")), 15000);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-stages/end")), 1);
    }

    void moonlightsOwnEndMessageIsNotReplacedBySeatHubText()
    {
        const QString text = QStringLiteral(
            "Your network connection isn't performing well. Reduce your video bitrate setting or "
            "try a faster connection.");
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        WalletTicks ticks;
        beginStreaming(client, engine, &ticks, 90, 80);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-live"), QStringLiteral("COMPLETED"),
                              QStringLiteral("HOST_LOST"));
        const QString endPath = QStringLiteral("/api/sessions/s-live/end");
        const QString outboxFile =
            QDir(m_dir->path()).filePath(QStringLiteral("end-outbox/s-live.json"));
        m_fake->setEndProbe([outboxFile]() { return QFile::exists(outboxFile); });

        emitConnectionTerminated(-101); // ML_ERROR_NO_VIDEO_FRAME
        emit engine->displayLaunchError(text);

        // SeatHub raises no error view and no failure text of its own; Moonlight's message is held.
        QVERIFY(client.failure().isEmpty());
        QVERIFY(!client.connectFailed());
        QCOMPARE(client.engineEndMessage(), text);
        emit engine->readyForDeletion();

        QTRY_VERIFY_WITH_TIMEOUT(m_fake->requestPaths().contains(endPath), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);
        QCOMPARE(m_fake->requestPaths().count(endPath), 1);
        QVERIFY(m_fake->endOutboxExistedAtFirstEnd());
        QCOMPARE(client.engineEndMessage(), text);
        QVERIFY(client.failure().isEmpty());
    }

    void aStartOfStreamNoVideoAfterConnectionStartedShowsMoonlightsTextAndQueuesOneEnd()
    {
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        WalletTicks ticks;
        beginStreaming(client, engine, &ticks, 90, 80);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-live"), QStringLiteral("COMPLETED"),
                              QStringLiteral("HOST_LOST"));

        emitConnectionTerminated(-100); // ML_ERROR_NO_VIDEO_TRAFFIC
        emit engine->displayLaunchError(QStringLiteral("No video received from host."));
        emit engine->readyForDeletion();

        // Home is set synchronously on readyForDeletion, while the End is posted from the network
        // thread, so wait for the request before counting it (as the sibling test above does).
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-live/end")), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);
        QCOMPARE(client.engineEndMessage(), QStringLiteral("No video received from host."));
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-live/end")), 1);
    }

    void aPortTestResultDoesNotReplaceMoonlightsEndMessage()
    {
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        WalletTicks ticks;
        beginStreaming(client, engine, &ticks, 90, 80);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-live"), QStringLiteral("COMPLETED"),
                              QStringLiteral("HOST_LOST"));

        emitConnectionTerminated(-100);
        emit engine->displayLaunchError(QStringLiteral("No video received from host."));
        emit engine->sessionFinished(1); // the engine's port test found a blocked port
        emit engine->readyForDeletion();

        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);
        QVERIFY(client.failure().isEmpty());
        QCOMPARE(client.engineEndMessage(), QStringLiteral("No video received from host."));
    }

    void aDeliberateQuitShowsNoEngineEndMessage()
    {
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        WalletTicks ticks;
        beginStreaming(client, engine, &ticks, 90, 80);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-live"), QStringLiteral("COMPLETED"),
                              QStringLiteral("CUSTOMER_ENDED"));

        emitConnectionTerminated(0);
        emit engine->readyForDeletion();

        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);
        QVERIFY(client.engineEndMessage().isEmpty());
    }

    void aNewSessionClearsTheEngineEndMessage()
    {
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        WalletTicks ticks;
        beginStreaming(client, engine, &ticks, 90, 80);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-live"), QStringLiteral("COMPLETED"),
                              QStringLiteral("HOST_LOST"));
        emitConnectionTerminated(-100);
        emit engine->displayLaunchError(QStringLiteral("No video received from host."));
        emit engine->readyForDeletion();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);
        QVERIFY(!client.engineEndMessage().isEmpty());

        client.beginSession(QStringLiteral("s-next"));
        QVERIFY(client.engineEndMessage().isEmpty());
    }

    void aPreStreamEngineLaunchErrorShowsMoonlightsTextVerbatim()
    {
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        client.session()->attachSession(engine);
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"),
                              QStringLiteral("CONNECT_FAILED"), 0, QStringLiteral("connection"),
                              QStringLiteral("SH-4F7KQ2"));

        emit engine->displayLaunchError(QStringLiteral("Host returned error: 503"));

        QVERIFY(client.connectFailed());
        QCOMPARE(client.stalledReasonText(), QStringLiteral("Host returned error: 503"));
        QVERIFY(client.stalledReasonPlain());
        QTRY_COMPARE_WITH_TIMEOUT(client.reference(), QStringLiteral("SH-4F7KQ2"), 15000);
        // The server's verdict supplied the reference only; the sentence is still Moonlight's.
        QCOMPARE(client.stalledReasonText(), QStringLiteral("Host returned error: 503"));
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-stages/end")), 15000);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-stages/end")), 1);
        client.session()->attachSession(nullptr);
        delete engine;
    }

    void aPreStreamEngineStageFailureShowsMoonlightsStageText()
    {
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        client.session()->attachSession(engine);
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"));

        // ADR-0104: the engine's connection thread relays the conclusive port-test result of this
        // Play (flag 1 requested, mask 0x10 failed) before it emits the stage failure.
        seathub::PortTestRelay::publish(1u, 0x10u);
        emit engine->stageStarting(QStringLiteral("RTSP handshake"));
        emit engine->stageFailed(QStringLiteral("RTSP handshake"), 10060, QStringLiteral("TCP 48010"));

        QVERIFY(client.connectFailed());
        QCOMPARE(client.stalledReasonText(),
                 QStringLiteral("Starting RTSP handshake failed: Error 10060\n\nCheck your firewall "
                                "and port forwarding rules for port(s): TCP 48010"));
        QVERIFY(client.stalledReasonPlain());
        client.session()->attachSession(nullptr);
        delete engine;
    }

    void aPreStreamStageFailureWithoutAConclusiveTestShowsNoFirewallSentence()
    {
        // ADR-0104: a clean or inconclusive port test, or none at all, must not blame the firewall.
        struct Case { const char* name; bool publish; unsigned flags; unsigned result; };
        const Case cases[] = {
            { "not run", false, 0u, 0u },
            { "clean", true, 1u, 0u },
            { "inconclusive", true, 1u, seathub::kPortTestInconclusive },
        };
        for (const Case& c : cases) {
            QTest::qWait(0);
            auto* engine = new FakeEngineSession;
            SeatHubClient client;
            client.session()->attachSession(engine);
            beginStagedSession(client);
            QVERIFY2(!QTest::currentTestFailed(), c.name);
            client.teardown()->setVerifyIntervalMs(1);
            m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"));
            if (c.publish) {
                seathub::PortTestRelay::publish(c.flags, c.result);
            }
            emit engine->stageFailed(QStringLiteral("RTSP handshake"), 10060,
                                     QStringLiteral("TCP 48010"));
            QVERIFY2(client.connectFailed(), c.name);
            QCOMPARE(client.stalledReasonText(),
                     QStringLiteral("Starting RTSP handshake failed: Error 10060"));
            client.session()->attachSession(nullptr);
            delete engine;
        }
    }

    void aStageFailureSendsItsPlaysPortTestOnEndAndLiveness()
    {
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        client.session()->attachSession(engine);
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"));

        seathub::PortTestRelay::publish(1u, 0x10u);
        emit engine->stageFailed(QStringLiteral("RTSP handshake"), 10060, QStringLiteral("TCP 48010"));

        const QJsonObject expected {
            { QStringLiteral("outcome"), QStringLiteral("failed") },
            { QStringLiteral("failed_mask"), 16 },
        };
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-stages/end")), 15000);
        const QJsonObject end = QJsonDocument::fromJson(
            m_fake->bodyFor(QStringLiteral("/api/sessions/s-stages/end"))).object();
        QCOMPARE(end.value(QStringLiteral("port_test")).toObject(), expected);
        QCOMPARE(end.value(QStringLiteral("failing_ports")).toString(), QStringLiteral("TCP 48010"));

        // The liveness failure report of the same Play carries the same result.
        const QList<QByteArray> reports =
            m_fake->bodiesFor(QStringLiteral("/api/sessions/s-stages/liveness"));
        bool sawFailureReport = false;
        for (const QByteArray& body : reports) {
            const QJsonObject report = QJsonDocument::fromJson(body).object();
            if (report.value(QStringLiteral("stage")).toString() == QLatin1String("failed")) {
                sawFailureReport = true;
                QCOMPARE(report.value(QStringLiteral("port_test")).toObject(), expected);
            }
        }
        QVERIFY(sawFailureReport);
        client.session()->attachSession(nullptr);
        delete engine;
    }

    void anEarlierPlaysPortTestIsNeverAttachedToALaterPlay()
    {
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        client.session()->attachSession(engine);
        // A result left behind by an earlier Play: beginSession of the next Play must discard it.
        seathub::PortTestRelay::publish(1u, 0x10u);
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"));

        emit engine->stageFailed(QStringLiteral("RTSP handshake"), 10060, QStringLiteral("TCP 48010"));

        QVERIFY(client.connectFailed());
        QCOMPARE(client.stalledReasonText(),
                 QStringLiteral("Starting RTSP handshake failed: Error 10060"));
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-stages/end")), 15000);
        const QJsonObject end = QJsonDocument::fromJson(
            m_fake->bodyFor(QStringLiteral("/api/sessions/s-stages/end"))).object();
        QVERIFY(!end.contains(QStringLiteral("port_test")));
        client.session()->attachSession(nullptr);
        delete engine;
    }

    void aRetriedPreStreamEndCarriesTheSamePortTest()
    {
        // ADR-0104: the End's first POST fails, so it is retried from the disk outbox - by the same
        // client, and by a fresh client after a restart - and every attempt carries the same result.
        const QString outboxFile =
            QDir(m_dir->path()).filePath(QStringLiteral("end-outbox/s-stages.json"));
        const QJsonObject expected {
            { QStringLiteral("outcome"), QStringLiteral("failed") },
            { QStringLiteral("failed_mask"), 16 },
        };
        const QString endPath = QStringLiteral("/api/sessions/s-stages/end");

        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        client.session()->attachSession(engine);
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        client.teardown()->setTeardownGraceMs(1);
        // The server has not seen the session end, so only the saved End can confirm it.
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("ACTIVE"));
        m_fake->answerEnd(0);  // the control plane is unreachable for every attempt below

        seathub::PortTestRelay::publish(1u, 0x10u);
        emit engine->stageFailed(QStringLiteral("RTSP handshake"), 10060,
                                 QStringLiteral("TCP 48010"));

        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(outboxFile), 15000);
        // The same client retries it from the outbox: wait for a second attempt.
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->bodiesFor(endPath).size() >= 2, 20000);
        QVERIFY(client.hasSavedEnd());
        for (const QByteArray& body : m_fake->bodiesFor(endPath)) {
            QCOMPARE(QJsonDocument::fromJson(body).object()
                         .value(QStringLiteral("port_test")).toObject(),
                     expected);
        }
        {
            QFile file(outboxFile);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QJsonObject report = QJsonDocument::fromJson(file.readAll()).object()
                                           .value(QStringLiteral("report")).toObject();
            QCOMPARE(report.value(QStringLiteral("port_test")).toObject(), expected);
        }

        // The restart: a fresh client on the same outbox directory re-sends it with the same result.
        // The first client stays alive so no retry is in flight against a destroyed object.
        m_fake->answerEnd(200);
        SeatHubClient restarted;
        restarted.setReconnectJitterProvider([]() { return 0.0; });
        reachHome(restarted, 90);
        QVERIFY(!QTest::currentTestFailed());
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(outboxFile), 15000);
        const QList<QByteArray> bodies = m_fake->bodiesFor(endPath);
        // reachHome() armed the restarted client with a fresh fake control plane, so this history
        // holds only what the restarted client sent: the re-sent End.
        QVERIFY(!bodies.isEmpty());
        for (const QByteArray& body : bodies) {
            QCOMPARE(QJsonDocument::fromJson(body).object()
                         .value(QStringLiteral("port_test")).toObject(),
                     expected);
        }
        client.session()->attachSession(nullptr);
        delete engine;
    }

    void aRetriedEndWithoutAPortTestIsRetriedWithoutTheKey()
    {
        // An End whose Play ran no port test, and an old outbox entry written before ADR-0104, both
        // read back and are re-sent with no `port_test` key at all.
        const QString outboxDir = QDir(m_dir->path()).filePath(QStringLiteral("end-outbox"));
        const QString outboxFile = QDir(outboxDir).filePath(QStringLiteral("s-old.json"));
        QVERIFY(QDir().mkpath(outboxDir));
        {
            QFile file(outboxFile);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(QJsonDocument(QJsonObject{
                { QStringLiteral("account_id"),
                  QStringLiteral("6f1c6f5e-3a1e-4b1e-9f2e-0f1a2b3c4d5e") },
                { QStringLiteral("session_id"), QStringLiteral("s-old") },
                { QStringLiteral("failed"), true },
                { QStringLiteral("report"), QJsonObject{
                    { QStringLiteral("stage"), QStringLiteral("connecting") },
                    { QStringLiteral("failing_ports"), QStringLiteral("TCP 48010") },
                    { QStringLiteral("attempt_step"), QStringLiteral("engine_connect") },
                } },
            }).toJson(QJsonDocument::Compact));
        }

        SeatHubClient client;
        client.setReconnectJitterProvider([]() { return 0.0; });
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(outboxFile), 15000);
        const QJsonObject end = QJsonDocument::fromJson(
            m_fake->bodyFor(QStringLiteral("/api/sessions/s-old/end"))).object();
        QCOMPARE(end.value(QStringLiteral("failed")).toBool(), true);
        QCOMPARE(end.value(QStringLiteral("failing_ports")).toString(), QStringLiteral("TCP 48010"));
        QVERIFY(!end.contains(QStringLiteral("port_test")));
    }

    void aSeatHubOwnPreStreamFailureKeepsSeatHubCopy()
    {
        SeatHubClient client;
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);

        emit client.pairing()->pairingFailed(
            SeatHubFailure::local(QStringLiteral("The rig didn't finish connecting. Try again.")));
        QVERIFY(client.connectFailed());
        QVERIFY(!client.stalledReasonPlain());
        QVERIFY(!client.stalledReasonText().isEmpty());

        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"),
                              QStringLiteral("CONNECT_FAILED"), 0, QStringLiteral("rig"),
                              QStringLiteral("SH-4F7KQ2"));
        const QString rigSentence = mapEndBlame(QStringLiteral("rig"), QString()).error;
        QTRY_COMPARE_WITH_TIMEOUT(client.stalledReasonText(), rigSentence.toHtmlEscaped(), 15000);
        QVERIFY(!client.stalledReasonPlain());
    }

    void aDeliberateQuitStillEndsWithoutAnError()
    {
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        WalletTicks ticks;
        beginStreaming(client, engine, &ticks, 90, 80);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-live"), QStringLiteral("COMPLETED"),
                              QStringLiteral("CUSTOMER_ENDED"));

        emitConnectionTerminated(0); // ML_ERROR_GRACEFUL_TERMINATION: Ctrl+Alt+Shift+Q
        emit engine->readyForDeletion();

        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-live/end")), 15000);
        QVERIFY(client.failure().isEmpty());
        QVERIFY(!client.connectFailed());
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-live/end")), 1);
    }

    void aSecondEndSourceAfterAnEngineEndQueuesNoSecondIntent()
    {
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        WalletTicks ticks;
        beginStreaming(client, engine, &ticks, 90, 80);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-live"), QStringLiteral("COMPLETED"),
                              QStringLiteral("HOST_LOST"));

        emitConnectionTerminated(-102);
        emit engine->displayLaunchError(QStringLiteral("Connection terminated"));
        emit engine->readyForDeletion();
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-live/end")), 15000);

        // A server-terminal read and a repeat ready-for-deletion arrive after the engine's end.
        SessionInfo over = sessionIn(QStringLiteral("COMPLETED"), QStringLiteral("s-live"));
        over.endReason = QStringLiteral("HOST_LOST");
        report(client, over);
        emit engine->readyForDeletion();
        QTest::qWait(300);
        QCOMPARE(m_fake->requestPaths().count(QStringLiteral("/api/sessions/s-live/end")), 1);
    }

    void aFrozenPictureStartsNoSeatHubTimerOrEnd()
    {
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        WalletTicks ticks;
        beginStreaming(client, engine, &ticks, 90, 80);
        QVERIFY(!QTest::currentTestFailed());

        // No frame statistics ever arrive; SeatHub owns no freeze deadline, so nothing interrupts
        // the engine, ends the session or starts a reconnect.
        QTest::qWait(500);
        QCOMPARE(engine->interrupts.load(), 0);
        QCOMPARE(client.appState(), QStringLiteral("streaming"));
        QVERIFY(!m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-live/end")));
        emit engine->readyForDeletion();
    }

    // --- the start-of-stream hint (SC13) ---------------------------------------------------------

    void theStartHintShowsFromConnectionStartedAndExpiresThroughTheHud()
    {
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        WalletTicks ticks;
        std::atomic<qint64> now{1000000};
        std::atomic<int> published{0};
        client.hud()->compositor().setClock([&now]() { return now.load(); });
        client.hud()->setPublisher([&published](SDL_Surface* surface) {
            if (surface) {
                ++published;
                SDL_FreeSurface(surface);
            }
            return true;
        });
        beginStreaming(client, engine, &ticks, 90, 80);
        QVERIFY(!QTest::currentTestFailed());

        QVERIFY(client.hud()->compositor().isStartHintActive());
        client.hud()->tick();
        QVERIFY(published.load() >= 1);
        const QImage withHint = client.hud()->compositor().composedBottom();
        QVERIFY(!withHint.isNull());

        now += 9999;
        QVERIFY(client.hud()->compositor().isStartHintActive());
        now += 2;
        QVERIFY(!client.hud()->compositor().isStartHintActive());
        client.hud()->tick();
        QVERIFY(client.hud()->compositor().composedBottom() != withHint);

        client.session()->attachSession(nullptr);
        delete engine;
    }

    void theStartHintIsClearedWhenTheSessionFinishes()
    {
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        WalletTicks ticks;
        beginStreaming(client, engine, &ticks, 90, 80);
        QVERIFY(!QTest::currentTestFailed());
        QVERIFY(client.hud()->compositor().isStartHintActive());

        emit engine->sessionFinished(0);
        QVERIFY(!client.hud()->compositor().isStartHintActive());

        client.session()->attachSession(nullptr);
        delete engine;
    }

    // -----------------------------------------------------------------------------------------
    // T063 / ADR-0105: the stall window rides the sampler's thread, never the facade's.
    // -----------------------------------------------------------------------------------------

    void emptyWindowsThenARollupRecordOneStallWhileTheMainThreadIsHeld()
    {
        // 503 keeps the capsule on disk, so the entry can be read back after the delivery attempt.
        m_evidenceStatusAtArm = 503;
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        WalletTicks ticks;
        client.setSamplerWindowMsForTests(40);
        beginStreaming(client, engine, &ticks, 90, 80);
        QVERIFY(!QTest::currentTestFailed());
        QVERIFY(settleIntoTheStream(client, &ticks));
        const int before = m_fake->countOfPathEndingWith(QStringLiteral("/evidence"));

        // From here this thread runs no event loop - the state it is in during a real stream - so
        // only the sampler's own thread can record the stall and start the delivery.
        QThread::msleep(220); // several full windows with no sample
        feedOneSample(client); // the first non-empty rollup follows
        QVERIFY2(waitWithoutTheEventLoop([&]() {
                     return !evidenceEntries(QStringLiteral("stall_window")).isEmpty()
                         && m_fake->countOfPathEndingWith(QStringLiteral("/evidence")) > before;
                 }),
                 "the stall must be on disk and delivered while the main thread is held");

        const QList<QJsonObject> stalls = evidenceEntries(QStringLiteral("stall_window"));
        QCOMPARE(stalls.size(), 1);
        const int windows = stalls.first().value(QStringLiteral("windows")).toInt();
        QVERIFY2(windows >= 2, "consecutive empty windows coalesce into one entry");
        QCOMPARE(stalls.first().value(QStringLiteral("duration_ms")).toInt(), windows * 40);

        QThread::msleep(100);
        QCOMPARE(m_fake->countOfPathEndingWith(QStringLiteral("/evidence")), before + 1);
        const QList<QByteArray> sent =
            m_fake->bodiesFor(QStringLiteral("/api/sessions/s-live/evidence"));
        QVERIFY(!sent.isEmpty());
        QVERIFY(sent.last().contains("stall_window"));

        endStreaming(client, engine);
    }

    void stoppingAfterEmptyWindowsOnlyKeepsThePendingStallInThatPlaysCapsule()
    {
        m_evidenceStatusAtArm = 503;
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        WalletTicks ticks;
        client.setSamplerWindowMsForTests(40);
        beginStreaming(client, engine, &ticks, 90, 80);
        QVERIFY(!QTest::currentTestFailed());
        QVERIFY(settleIntoTheStream(client, &ticks));

        QThread::msleep(220); // empty windows only: no non-empty rollup ever follows
        QVERIFY(evidenceEntries(QStringLiteral("stall_window")).isEmpty());

        emit engine->sessionFinished(0);
        emit engine->readyForDeletion();

        // finishPlay recorded the pending stall before it closed the capsule.
        const QList<QJsonObject> stalls = evidenceEntries(QStringLiteral("stall_window"));
        QCOMPARE(stalls.size(), 1);
        const int windows = stalls.first().value(QStringLiteral("windows")).toInt();
        QVERIFY(windows >= 2);
        QCOMPARE(stalls.first().value(QStringLiteral("duration_ms")).toInt(), windows * 40);

        endStreaming(client, engine);
    }

    void anEarlierPlaysStallNeverAppearsInALaterPlaysCapsule()
    {
        m_evidenceStatusAtArm = 503;
        auto* first = new FakeEngineSession;
        SeatHubClient client;
        WalletTicks ticks;
        client.setSamplerWindowMsForTests(40);
        beginStreaming(client, first, &ticks, 90, 80);
        QVERIFY(!QTest::currentTestFailed());
        QVERIFY(settleIntoTheStream(client, &ticks));
        QThread::msleep(220);
        emit first->sessionFinished(0);
        emit first->readyForDeletion();
        QCOMPARE(evidenceEntries(QStringLiteral("stall_window")).size(), 1);
        endStreaming(client, first);

        // A second Play, with a window too long to end during the test: no empty window, no stall.
        client.setSamplerWindowMsForTests(60000);
        auto* second = new FakeEngineSession;
        client.session()->attachSession(second);
        client.beginSession(QStringLiteral("s-live-2"));
        emit second->connectionStarted();
        QCOMPARE(client.appState(), QStringLiteral("streaming"));
        QThread::msleep(150);
        emit second->sessionFinished(0);
        emit second->readyForDeletion();

        // Read each capsule apart: the stall is in the first Play's capsule and not in the second's.
        QMap<QString, QStringList> kindsBySession;
        const QDir dir(m_dir->path() + QStringLiteral("/evidence-journal"));
        for (const QString& name : dir.entryList({QStringLiteral("evj-*.json")}, QDir::Files)) {
            QFile file(dir.filePath(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QJsonObject capsule = QJsonDocument::fromJson(file.readAll()).object();
            for (const QJsonValue& entry : capsule.value(QStringLiteral("entries")).toArray()) {
                kindsBySession[capsule.value(QStringLiteral("session_id")).toString()]
                    << entry.toObject().value(QStringLiteral("kind")).toString();
            }
        }
        QVERIFY2(kindsBySession.contains(QStringLiteral("s-live-2")),
                 "the later Play has a capsule, so the absence below is not vacuous");
        QVERIFY(kindsBySession.value(QStringLiteral("s-live")).contains(QStringLiteral("stall_window")));
        QVERIFY(!kindsBySession.value(QStringLiteral("s-live-2")).contains(QStringLiteral("stall_window")));

        endStreaming(client, second);
    }

    // T063 / ADR-0105: a capsule exists only while a Play is open.
    void nothingIsRecordedWhileIdle()
    {
        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());
        QThread::msleep(100);
        const QDir dir(m_dir->path() + QStringLiteral("/evidence-journal"));
        QVERIFY2(dir.entryList({QStringLiteral("evj-*.json")}, QDir::Files).isEmpty(),
                 "no Play is open, so no capsule may exist");
    }

    // T063 / ADR-0105: a conclusive port test and the engine stage failure reach the capsule as
    // closed values only; the address, port text and a sentinel path never do.
    void aPreStreamFailureRecordsClosedEvidenceAndNoSentinel()
    {
        m_evidenceStatusAtArm = 503;
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        client.session()->attachSession(engine);
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"));

        seathub::PortTestRelay::publish(1u, 0x10u);
        emit engine->stageStarting(QStringLiteral("RTSP handshake"));
        emit engine->stageFailed(QStringLiteral("RTSP handshake"), 10060,
                                 QStringLiteral("TCP 48010 at 203.0.113.9 C:\\Users\\sentinel"));

        QVERIFY(!evidenceEntries(QStringLiteral("engine_failure")).isEmpty());
        QVERIFY(!evidenceEntries(QStringLiteral("port_test")).isEmpty());

        const QDir dir(m_dir->path() + QStringLiteral("/evidence-journal"));
        for (const QString& name : dir.entryList({QStringLiteral("evj-*.json")}, QDir::Files)) {
            QFile file(dir.filePath(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QByteArray raw = file.readAll();
            QVERIFY(!raw.contains("203.0.113.9"));
            QVERIFY(!raw.contains("sentinel"));
            QVERIFY(!raw.contains("48010"));
        }
        client.session()->attachSession(nullptr);
        delete engine;
    }

    // T063 / ADR-0105: an offline delivery keeps the capsule; it never reaches the customer.
    void anOfflineDeliveryKeepsTheCapsule()
    {
        m_evidenceStatusAtArm = 503;
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        client.session()->attachSession(engine);
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"));
        emit engine->stageStarting(QStringLiteral("RTSP handshake"));
        emit engine->stageFailed(QStringLiteral("RTSP handshake"), 10060, QString());

        QTRY_VERIFY_WITH_TIMEOUT(m_fake->countOfPathEndingWith(QStringLiteral("/evidence")) >= 1, 10000);
        QVERIFY2(!evidenceEntries().isEmpty(), "a 503 answer keeps the entries on disk");
        client.session()->attachSession(nullptr);
        delete engine;
    }

    // T063 / ADR-0105: a capsule belongs to the account that played it. Another account's capsule is
    // neither sent nor dropped by the current account, and goes out when its own account is current.
    void aCapsuleOfAnotherAccountWaitsForThatAccount()
    {
        SeatHubClient client;
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());
        plantCapsule(QStringLiteral("s-old-a"), accountAId());
        plantCapsule(QStringLiteral("s-old-b"), accountBId());
        QVERIFY(!QTest::currentTestFailed());
        QVERIFY(capsuleOnDisk(QStringLiteral("s-old-a")));
        QVERIFY(capsuleOnDisk(QStringLiteral("s-old-b")));

        QMetaObject::invokeMethod(&client, "onNetworkBack");
        QTRY_VERIFY_WITH_TIMEOUT(!capsuleOnDisk(QStringLiteral("s-old-a")), 15000);
        QTest::qWait(500);
        QVERIFY(m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-old-a/evidence")));
        QVERIFY2(!m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-old-b/evidence")),
                 "account A must never send account B's capsule");
        QVERIFY2(capsuleOnDisk(QStringLiteral("s-old-b")), "account A must never drop B's capsule");
        QCOMPARE(evidenceEntries(QStringLiteral("stall_window")).size(), 1);

        // B signs in: its own capsule is now the one that goes out, under B's own token.
        client.signOut();
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("signed_out"), 15000);
        m_fake->answerMe(200, QByteArrayLiteral(
            "{\"id\":\"77777777-7777-7777-7777-777777777777\",\"display_name\":\"Omar\","
            "\"username\":\"omar\",\"email\":\"omar@example.com\",\"phone_e164\":\"+962790000001\","
            "\"role\":\"customer\",\"signup_stage\":\"complete\",\"email_verified\":true,"
            "\"phone_verified\":true,\"created_at\":\"2026-09-01T00:00:00Z\"}"));
        m_fake->answerWallet(200, walletBody(45));
        m_fake->answerLogin(200, QByteArrayLiteral("{\"access_token\":\"sb_at_for_b\"}"));
        client.signInWithPassword(QStringLiteral("omar@example.com"), QStringLiteral("irrelevant"));
        QTRY_COMPARE_WITH_TIMEOUT(client.appState(), QStringLiteral("home"), 15000);

        QTRY_VERIFY_WITH_TIMEOUT(!capsuleOnDisk(QStringLiteral("s-old-b")), 15000);
        QVERIFY(m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-old-b/evidence")));
        QCOMPARE(m_fake->authorizationFor(QStringLiteral("/api/sessions/s-old-b/evidence")),
                 QByteArrayLiteral("Bearer sb_at_for_b"));
    }

    // T063 / ADR-0105: an offline delivery keeps the capsule; a network return then sends it once.
    void aNetworkReturnDrainsAnOfflineCapsuleExactlyOnce()
    {
        const QString path = QStringLiteral("/api/sessions/s-offline/evidence");
        SeatHubClient client;
        client.setReconnectJitterProvider([]() { return 0.0; });
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());
        m_fake->answerEvidence(0, QByteArray()); // transport failure
        plantCapsule(QStringLiteral("s-offline"), accountAId());
        QVERIFY(!QTest::currentTestFailed());

        QMetaObject::invokeMethod(&client, "onNetworkBack");
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->requestPaths().count(path) >= 1, 15000);
        QTest::qWait(200);
        QVERIFY2(capsuleOnDisk(QStringLiteral("s-offline")), "a transport failure keeps the capsule");

        m_fake->answerEvidence(200);
        QMetaObject::invokeMethod(&client, "onNetworkBack");
        QTRY_VERIFY_WITH_TIMEOUT(!capsuleOnDisk(QStringLiteral("s-offline")), 15000);

        // Delivered: nothing is sent again, not by a second trigger and not by the retry timer.
        const int delivered = m_fake->requestPaths().count(path);
        QMetaObject::invokeMethod(&client, "onNetworkBack");
        QTest::qWait(3500);
        QCOMPARE(m_fake->requestPaths().count(path), delivered);
    }

    // T063 / ADR-0105: with no network-return hint at all, the End outbox's paced timer drains it.
    void theRetryTimerDrainsAnOfflineCapsuleExactlyOnce()
    {
        const QString path = QStringLiteral("/api/sessions/s-timer/evidence");
        SeatHubClient client;
        client.setReconnectJitterProvider([]() { return 0.0; });
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());
        m_fake->answerEvidence(503);
        plantCapsule(QStringLiteral("s-timer"), accountAId());
        QVERIFY(!QTest::currentTestFailed());

        QMetaObject::invokeMethod(&client, "onNetworkBack");
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->requestPaths().count(path) >= 1, 15000);
        QTest::qWait(200);
        QVERIFY2(capsuleOnDisk(QStringLiteral("s-timer")), "a 5xx answer keeps the capsule");

        m_fake->answerEvidence(200);
        QTRY_VERIFY_WITH_TIMEOUT(!capsuleOnDisk(QStringLiteral("s-timer")), 20000);
        const int delivered = m_fake->requestPaths().count(path);
        QTest::qWait(3500);
        QCOMPARE(m_fake->requestPaths().count(path), delivered);
    }

    // ADR-0107: evidence capsules are a covered upload. With reporting off nothing is posted and the
    // capsule stays pending; sign-in (a functional call) is unaffected; switching it on sends it.
    void aCapsuleStaysPendingWhileReportingIsOffAndIsSentWhenItIsSwitchedOn()
    {
        const QString path = QStringLiteral("/api/sessions/s-consent/evidence");
        reporting_consent_store::Record record;
        record.recorded = true;
        record.send = false;
        record.noticeVersion = reporting_consent_store::kCurrentNoticeVersion;
        ReportingConsent::setStateForTests(true, record);

        SeatHubClient client;
        client.setReconnectJitterProvider([]() { return 0.0; });
        reachHome(client, 90); // sign-in, /api/me and the wallet still work with reporting off
        QVERIFY(!QTest::currentTestFailed());
        QVERIFY(m_fake->requestPaths().contains(QStringLiteral("/api/me")));
        m_fake->answerEvidence(200);
        plantCapsule(QStringLiteral("s-consent"), accountAId());
        QVERIFY(!QTest::currentTestFailed());

        QMetaObject::invokeMethod(&client, "onNetworkBack");
        QTest::qWait(500);
        QCOMPARE(m_fake->requestPaths().count(path), 0);
        QVERIFY2(capsuleOnDisk(QStringLiteral("s-consent")), "a capsule stays pending while off");

        record.send = true;
        ReportingConsent::setStateForTests(true, record);
        QMetaObject::invokeMethod(&client, "onNetworkBack");
        // The answer deletes the capsule on the network thread within a millisecond or two. Polling
        // capsuleOnDisk() opens that file, and Windows refuses the delete while it is open, which
        // left the capsule behind and made the client send it a second time. Let the post and its
        // answer settle before the first poll.
        QTRY_COMPARE_WITH_TIMEOUT(m_fake->requestPaths().count(path), 1, 15000);
        QTest::qWait(300);
        QTRY_VERIFY_WITH_TIMEOUT(!capsuleOnDisk(QStringLiteral("s-consent")), 15000);
        QCOMPARE(m_fake->requestPaths().count(path), 1);
    }

    // ADR-0107: the quality outbox sends nothing while off, keeps its files, and a switch-off in the
    // middle of a drain stops the rest of the chain at once.
    void theQualityOutboxSendsNothingWhileReportingIsOffAndKeepsItsFiles()
    {
        const QString account = QStringLiteral("6f1c6f5e-3a1e-4b1e-9f2e-0f1a2b3c4d5e");
        QualityOutbox outbox;
        outbox.setDirectory(m_dir->path() + QStringLiteral("/quality-consent"));
        outbox.put(QStringLiteral("aaaaaaaa-1111-1111-1111-111111111111"), account,
                   QJsonObject{ { QStringLiteral("rtt_ms"), 12 } });
        outbox.put(QStringLiteral("bbbbbbbb-2222-2222-2222-222222222222"), account,
                   QJsonObject{ { QStringLiteral("rtt_ms"), 14 } });

        reporting_consent_store::Record record;
        record.recorded = true;
        record.send = false;
        record.noticeVersion = reporting_consent_store::kCurrentNoticeVersion;
        ReportingConsent::setStateForTests(true, record);

        int posts = 0;
        bool switchOffAfterFirst = false;
        const auto post = [&](const QString&, const QJsonObject&, ControlPlaneClient::Callback callback) {
            ++posts;
            if (switchOffAfterFirst) {
                reporting_consent_store::Record off = record;
                off.send = false;
                ReportingConsent::setStateForTests(true, off);
            }
            ControlPlaneResult delivered;
            delivered.ok = true;
            delivered.statusCode = 200;
            callback(delivered);
        };

        outbox.drain(QStringLiteral("gen-1"), account, post);
        QCOMPARE(posts, 0);
        QVERIFY2(outbox.hasQueuedReports(), "off: the files stay queued");

        record.send = true;
        ReportingConsent::setStateForTests(true, record);
        switchOffAfterFirst = true;
        outbox.drain(QStringLiteral("gen-1"), account, post);
        QCOMPARE(posts, 1); // the second file is not sent once the choice flips mid-drain
        QVERIFY2(outbox.hasQueuedReports(), "the unsent file is still queued");

        switchOffAfterFirst = false;
        record.send = true;
        ReportingConsent::setStateForTests(true, record);
        outbox.drain(QStringLiteral("gen-1"), account, post);
        QCOMPARE(posts, 2);
        QVERIFY(!outbox.hasQueuedReports());
    }

    // T073 / ADR-0107 + ADR-0101: the held quality report is a covered upload at the IMMEDIATE send
    // boundary too. With reporting off, or no choice recorded, a Play whose stream produced stats and
    // whose End/teardown completed sends zero quality requests, yet the account-bound write-ahead file
    // stays on disk; switching reporting on sends it exactly once and only then removes the file.
    void qualityTeardownHonorsConsentOffAndNoChoice()
    {
        for (int pass = 0; pass < 2; ++pass) {
            const bool choiceRecorded = (pass == 1); // pass 0: no choice recorded, pass 1: recorded Off
            const QString sessionId = QStringLiteral("aaaaaaaa-c0c0-c0c0-c0c0-c0c0c0c0c0c%1").arg(pass);
            const QString qualityPath = QStringLiteral("/api/sessions/%1/quality").arg(sessionId);
            const QString endPath = QStringLiteral("/api/sessions/%1/end").arg(sessionId);
            const QString outboxPath =
                QDir(m_dir->path()).filePath(QStringLiteral("quality-outbox/%1.json").arg(sessionId));

            reporting_consent_store::Record record;
            record.recorded = choiceRecorded;
            record.send = false;
            record.noticeVersion = reporting_consent_store::kCurrentNoticeVersion;
            ReportingConsent::setStateForTests(true, record);

            SeatHubClient client;
            reachHome(client, 90);
            QVERIFY(!QTest::currentTestFailed());
            auto* engine = new FakeEngineSession;
            client.session()->attachSession(engine);
            client.teardown()->setVerifyIntervalMs(1);
            m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                       QStringLiteral("SH-2K2XQ1")));
            client.beginSession(sessionId);
            emit engine->connectionStarted();
            emitStatsBlock(42.0);

            QSignalSpy completed(client.teardown(), &TeardownController::teardownCompleted);
            emit engine->readyForDeletion();
            QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 15000);
            QTRY_VERIFY_WITH_TIMEOUT(m_fake->requestPaths().contains(endPath), 15000);
            QTest::qWait(500);
            QCOMPARE(m_fake->requestPaths().count(qualityPath), 0);

            QVERIFY2(QFile::exists(outboxPath), "the write-ahead report stays durable while off");
            {
                QFile file(outboxPath);
                QVERIFY(file.open(QIODevice::ReadOnly));
                const QJsonObject envelope = QJsonDocument::fromJson(file.readAll()).object();
                QCOMPARE(envelope.value(QStringLiteral("account_id")).toString(), accountAId());
                QCOMPARE(envelope.value(QStringLiteral("report")).toObject()
                             .value(QStringLiteral("rendered_fps")).toDouble(),
                         42.0);
            }

            record.recorded = true;
            record.send = true;
            ReportingConsent::setStateForTests(true, record);
            QMetaObject::invokeMethod(&client, "onNetworkBack");
            QTRY_COMPARE_WITH_TIMEOUT(m_fake->requestPaths().count(qualityPath), 1, 15000);
            QTest::qWait(300);
            QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(outboxPath), 15000);
            QTest::qWait(500);
            QCOMPARE(m_fake->requestPaths().count(qualityPath), 1);

            client.session()->attachSession(nullptr);
            delete engine;
        }
    }

    // T073 / ADR-0101 amendment 2026-10-19: the six stream-start fields (width, height, target_fps,
    // codec, hdr, configured_bitrate_kbps) survive parse, write-ahead, teardown, restart and replay
    // under their own Play and account; a Play with no start lines keeps them null (no borrowing from
    // the Play before it); a legacy file without them still drains; another account's file never does.
    void streamStartQualitySurvivesTeardownAndRestart()
    {
        const QString playA = QStringLiteral("aaaaaaaa-d1d1-d1d1-d1d1-d1d1d1d1d1d1");
        const QString playB = QStringLiteral("aaaaaaaa-d2d2-d2d2-d2d2-d2d2d2d2d2d2");
        const QString legacy = QStringLiteral("aaaaaaaa-d3d3-d3d3-d3d3-d3d3d3d3d3d3");
        const QString foreign = QStringLiteral("aaaaaaaa-d4d4-d4d4-d4d4-d4d4d4d4d4d4");
        const auto qualityPath = [](const QString& id) {
            return QStringLiteral("/api/sessions/%1/quality").arg(id);
        };
        const auto outboxPath = [this](const QString& id) {
            return QDir(m_dir->path()).filePath(QStringLiteral("quality-outbox/%1.json").arg(id));
        };
        const auto readEnvelope = [](const QString& path) {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object()
                                                  : QJsonObject();
        };
        const QStringList startKeys{ QStringLiteral("width"), QStringLiteral("height"),
                                     QStringLiteral("target_fps"), QStringLiteral("codec"),
                                     QStringLiteral("hdr"), QStringLiteral("configured_bitrate_kbps") };

        reporting_consent_store::Record record;
        record.recorded = true;
        record.send = false;
        record.noticeVersion = reporting_consent_store::kCurrentNoticeVersion;
        ReportingConsent::setStateForTests(true, record);

        {
            SeatHubClient client;
            reachHome(client, 90);
            QVERIFY(!QTest::currentTestFailed());
            auto* engine = new FakeEngineSession;
            client.session()->attachSession(engine);
            client.teardown()->setVerifyIntervalMs(1);
            m_fake->answerPairing(409, playRefusalBody(QStringLiteral("The rig is not ready yet."),
                                                       QStringLiteral("SH-2K2XQ1")));

            // Play A: 1920x1080 at 60 FPS, HEVC 8-bit (0x0100), 20000 kbps configured.
            client.beginSession(playA);
            emit engine->connectionStarted();
            emitVideoStreamLine(1920, 1080, 60, 0x0100);
            emitVideoBitrateLine(20000);
            emitStatsBlock(55.0);

            // Play B replaces it and prints no start lines at all: nothing is borrowed from A.
            client.beginSession(playB);
            emitStatsBlock(33.0);

            emit client.teardown()->teardownCompleted(sessionIn(QStringLiteral("COMPLETED"), playA), playA);
            emit client.teardown()->teardownCompleted(sessionIn(QStringLiteral("COMPLETED"), playB), playB);
            QTest::qWait(500);
            QCOMPARE(m_fake->requestPaths().count(qualityPath(playA)), 0);
            QCOMPARE(m_fake->requestPaths().count(qualityPath(playB)), 0);

            QVERIFY(QFile::exists(outboxPath(playA)));
            QVERIFY(QFile::exists(outboxPath(playB)));
            const QJsonObject a = readEnvelope(outboxPath(playA));
            QCOMPARE(a.value(QStringLiteral("account_id")).toString(), accountAId());
            const QJsonObject reportA = a.value(QStringLiteral("report")).toObject();
            QCOMPARE(reportA.value(QStringLiteral("rendered_fps")).toDouble(), 55.0);
            QCOMPARE(reportA.value(QStringLiteral("width")).toInt(), 1920);
            QCOMPARE(reportA.value(QStringLiteral("height")).toInt(), 1080);
            QCOMPARE(reportA.value(QStringLiteral("target_fps")).toDouble(), 60.0);
            QCOMPARE(reportA.value(QStringLiteral("codec")).toString(), QStringLiteral("h265"));
            QVERIFY(reportA.value(QStringLiteral("hdr")).isBool());
            QCOMPARE(reportA.value(QStringLiteral("hdr")).toBool(), false);
            QCOMPARE(reportA.value(QStringLiteral("configured_bitrate_kbps")).toInt(), 20000);

            const QJsonObject reportB = readEnvelope(outboxPath(playB)).value(QStringLiteral("report")).toObject();
            QCOMPARE(reportB.value(QStringLiteral("rendered_fps")).toDouble(), 33.0);
            for (const QString& key : startKeys)
                QVERIFY2(reportB.value(key).isNull(), qPrintable(key + " must stay null for a Play with no start lines"));

            client.session()->attachSession(nullptr);
            delete engine;
        }

        // A legacy file (no start fields) and another account's file are planted beside the two.
        QualityOutbox planted;
        planted.setDirectory(QDir(m_dir->path()).filePath(QStringLiteral("quality-outbox")));
        planted.put(legacy, accountAId(), QJsonObject{ { QStringLiteral("rendered_fps"), 41.0 } });
        planted.put(foreign, QStringLiteral("99999999-9999-9999-9999-999999999999"),
                    QJsonObject{ { QStringLiteral("rendered_fps"), 22.0 } });

        // Restart: a new client, reporting now on. Each file is sent once, with its own payload.
        record.send = true;
        ReportingConsent::setStateForTests(true, record);
        SeatHubClient restarted;
        reachHome(restarted, 90);
        QVERIFY(!QTest::currentTestFailed());

        QTRY_COMPARE_WITH_TIMEOUT(m_fake->requestPaths().count(qualityPath(playA)), 1, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(m_fake->requestPaths().count(qualityPath(playB)), 1, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(m_fake->requestPaths().count(qualityPath(legacy)), 1, 15000);
        const QJsonObject sentA = QJsonDocument::fromJson(m_fake->bodyFor(qualityPath(playA))).object();
        QCOMPARE(sentA.value(QStringLiteral("width")).toInt(), 1920);
        QCOMPARE(sentA.value(QStringLiteral("height")).toInt(), 1080);
        QCOMPARE(sentA.value(QStringLiteral("target_fps")).toDouble(), 60.0);
        QCOMPARE(sentA.value(QStringLiteral("codec")).toString(), QStringLiteral("h265"));
        QCOMPARE(sentA.value(QStringLiteral("hdr")).toBool(true), false);
        QCOMPARE(sentA.value(QStringLiteral("configured_bitrate_kbps")).toInt(), 20000);
        const QJsonObject sentB = QJsonDocument::fromJson(m_fake->bodyFor(qualityPath(playB))).object();
        const QJsonObject sentLegacy = QJsonDocument::fromJson(m_fake->bodyFor(qualityPath(legacy))).object();
        for (const QString& key : startKeys) {
            QVERIFY2(sentB.value(key).isNull(), qPrintable(key + " must stay null in Play B's wire body"));
            // A legacy file never had the start keys: ADR-0101 treats omitted and null alike, and
            // either way no value may be borrowed from another Play.
            QVERIFY2(!sentLegacy.contains(key) || sentLegacy.value(key).isNull(),
                     qPrintable(key + " must stay unobserved for a legacy file"));
        }
        QCOMPARE(sentLegacy.value(QStringLiteral("rendered_fps")).toDouble(), 41.0);

        QTest::qWait(300);
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(outboxPath(playA)), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(outboxPath(playB)), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(outboxPath(legacy)), 15000);
        QTest::qWait(500);
        QCOMPARE(m_fake->requestPaths().count(qualityPath(playA)), 1);
        QCOMPARE(m_fake->requestPaths().count(qualityPath(playB)), 1);
        QCOMPARE(m_fake->requestPaths().count(qualityPath(legacy)), 1);
        QCOMPARE(m_fake->requestPaths().count(qualityPath(foreign)), 0);
    }

    // T063 / ADR-0105: every End-outbox step is one closed log line; the payload, the retry and the
    // idempotence of the End itself are what they were.
    void theEndOutboxLogsPutDrainKeptAndDroppedAsClosedLines()
    {
        LogLineCapture log;
        const QString endPath = QStringLiteral("/api/sessions/s-stages/end");
        SeatHubClient client;
        client.setReconnectJitterProvider([]() { return 0.0; });
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"));
        m_fake->answerEnd(0);

        emit client.pairing()->pairingFailed(
            SeatHubFailure::local(QStringLiteral("The rig didn't finish connecting. Try again.")));
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->bodiesFor(endPath).size() >= 3, 30000);
        QVERIFY(client.hasSavedEnd());

        m_fake->answerEnd(404); // the server holds no such session: the record is done
        QTRY_VERIFY_WITH_TIMEOUT(!client.hasSavedEnd(), 30000);
        QTest::qWait(300);

        const QStringList lines = log.texts(QStringLiteral("seathub.end_outbox"));
        QVERIFY(!lines.isEmpty());
        for (const QString& line : lines) {
            QVERIFY2(outboxLineIsClosed(QStringLiteral("end_outbox"), line), qPrintable(line));
        }
        QVERIFY(lines.contains(QStringLiteral("end_outbox event=put answer=none session=s-stages count=1")));
        QVERIFY(lines.contains(QStringLiteral("end_outbox event=drain answer=none session=- count=1")));
        QVERIFY(lines.contains(
            QStringLiteral("end_outbox event=kept answer=transport session=s-stages count=1")));
        QVERIFY(lines.contains(
            QStringLiteral("end_outbox event=dropped answer=404 session=s-stages count=1")));

        // Unchanged: every attempt carried the identical payload, and a finished End is never re-sent.
        const QList<QByteArray> bodies = m_fake->bodiesFor(endPath);
        for (const QByteArray& body : bodies) {
            QCOMPARE(body, bodies.first());
        }
        const int lineCount = lines.size();
        const int sends = bodies.size();
        QTest::qWait(3000);
        QCOMPARE(m_fake->bodiesFor(endPath).size(), sends);
        QCOMPARE(log.texts(QStringLiteral("seathub.end_outbox")).size(), lineCount);
    }

    void theEndOutboxLogsDeliveredAndAnUnreadableFileAsClosedLines()
    {
        LogLineCapture log;
        const QString endPath = QStringLiteral("/api/sessions/s-live/end");
        SeatHubClient client;
        client.setReconnectJitterProvider([]() { return 0.0; });
        reachHome(client, 90);
        QVERIFY(!QTest::currentTestFailed());

        const QString outboxDir = QDir(m_dir->path()).filePath(QStringLiteral("end-outbox"));
        QVERIFY(QDir().mkpath(outboxDir));
        {
            QFile file(QDir(outboxDir).filePath(QStringLiteral("s-live.json")));
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(QJsonDocument(QJsonObject{
                { QStringLiteral("account_id"), accountAId() },
                { QStringLiteral("session_id"), QStringLiteral("s-live") },
                { QStringLiteral("failed"), false },
            }).toJson(QJsonDocument::Compact));
        }
        const QString junk = QDir(outboxDir).filePath(QStringLiteral("junk.json"));
        {
            QFile file(junk);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write("{not json");
        }
        m_fake->answerEnd(200);
        QMetaObject::invokeMethod(&client, "onNetworkBack");
        QTRY_VERIFY_WITH_TIMEOUT(!client.hasSavedEnd(), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(client.homeStatus(), QStringLiteral("ready"), 15000);
        QTest::qWait(300);

        const QStringList lines = log.texts(QStringLiteral("seathub.end_outbox"));
        for (const QString& line : lines) {
            QVERIFY2(outboxLineIsClosed(QStringLiteral("end_outbox"), line), qPrintable(line));
        }
        QVERIFY(lines.contains(
            QStringLiteral("end_outbox event=delivered answer=2xx session=s-live count=1")));
        QVERIFY(lines.contains(QStringLiteral("end_outbox event=unreadable answer=none session=- count=1")));
        QVERIFY2(QFile::exists(junk), "an unreadable file is counted and logged, never deleted");
        QCOMPARE(m_fake->bodiesFor(endPath).size(), 1);
    }

    // T063 / ADR-0105: no log line the outbox, the quality outbox, the live-updates stream or the
    // facade writes carries an address, a Wi-Fi name, a path, a user name or a PIN, even when the
    // engine's own failure text is full of them.
    void noOutboxLogLineCarriesAnAddressNetworkNamePathUserOrPin()
    {
        LogLineCapture log;
        const QString endPath = QStringLiteral("/api/sessions/s-stages/end");
        m_evidenceStatusAtArm = 503;
        auto* engine = new FakeEngineSession;
        SeatHubClient client;
        client.setReconnectJitterProvider([]() { return 0.0; });
        client.session()->attachSession(engine);
        beginStagedSession(client);
        QVERIFY(!QTest::currentTestFailed());
        client.teardown()->setVerifyIntervalMs(1);
        m_fake->answerSession(QStringLiteral("s-stages"), QStringLiteral("CANCELLED"));
        m_fake->answerEnd(0);
        m_fake->answerQuality(503);

        // A quality report and an unreadable file waiting in the quality outbox.
        QVERIFY(QDir(m_dir->path()).mkpath(QStringLiteral("quality-outbox")));
        {
            QJsonObject envelope;
            envelope.insert(QStringLiteral("account_id"), accountAId());
            envelope.insert(QStringLiteral("report"), QJsonObject{{QStringLiteral("rendered_fps"), 30.0}});
            QFile file(QDir(m_dir->path()).filePath(QStringLiteral("quality-outbox/s-q1.json")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
        }
        {
            QFile file(QDir(m_dir->path()).filePath(QStringLiteral("quality-outbox/junk.json")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("{not json");
        }

        seathub::PortTestRelay::publish(1u, 0x10u);
        emit engine->stageStarting(QStringLiteral("RTSP handshake"));
        emit engine->stageFailed(
            QStringLiteral("RTSP handshake"), 10060,
            QStringLiteral("TCP 48010 at 203.0.113.9 on SentinelWifi-5G C:\\Users\\sentinel-user "
                           "PIN 4821 for lina lina@example.com +962790000000"));
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->bodiesFor(endPath).size() >= 2, 30000);
        QMetaObject::invokeMethod(&client, "onNetworkBack");
        QTRY_VERIFY_WITH_TIMEOUT(
            m_fake->requestPaths().contains(QStringLiteral("/api/sessions/s-q1/quality")), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->countOfPathEndingWith(QStringLiteral("/evidence")) >= 1, 15000);
        QTest::qWait(300);

        const QStringList quality = log.texts(QStringLiteral("seathub.quality_outbox"));
        QVERIFY(!quality.isEmpty());
        for (const QString& line : quality) {
            QVERIFY2(outboxLineIsClosed(QStringLiteral("quality_outbox"), line), qPrintable(line));
        }
        const QStringList sentinels = {
            QStringLiteral("203.0.113.9"), QStringLiteral("sentinelwifi"), QStringLiteral("c:\\users"),
            QStringLiteral("sentinel"), QStringLiteral("4821"), QStringLiteral("48010"),
            QStringLiteral("lina"), QStringLiteral("example.com"), QStringLiteral("962790000"),
        };
        int checked = 0;
        for (const LogLineCapture::Line& line : log.all()) {
            if (!line.category.startsWith(QLatin1String("seathub."))) continue;
            ++checked;
            for (const QString& sentinel : sentinels) {
                QVERIFY2(!line.text.toLower().contains(sentinel),
                         qPrintable(line.category + QLatin1String(": ") + line.text));
            }
        }
        QVERIFY2(checked > 0, "the sentinel scan must have read log lines");

        client.session()->attachSession(nullptr);
        delete engine;
    }
};

QTEST_MAIN(TstFacadeWiring)

#include "tst_facade_wiring.moc"
