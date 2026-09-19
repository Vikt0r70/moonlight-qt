#include "seathub_client.h"

#include <QLoggingCategory>
#include <QWindow>

#include "session_lifecycle.h"
#include "settings_bridge.h"
#include "update_feed_client.h"

// The production pairing handshake. Included here and nowhere else: it is the only translation
// unit that reaches into `app/backend/`, which is how `app/seathub/` stays free of upstream
// includes (and how the seam stays testable with no engine in the test binary).
#include "pairing_handshake.h"

// Only this translation unit needs the engine type: the HUD's publisher below is the one place
// that reaches `Session::get()`, which is what keeps `app/seathub/hud_overlay.*` free of the
// engine and linkable into a test with no engine in it.
#include "streaming/session.h"

Q_LOGGING_CATEGORY(seathubClient, "seathub.client")

namespace {

// appState values (D-35). QML switches views on these; they are part of the facade's
// contract, not an implementation detail.
const char* kStateSignedOut = "signed_out";
const char* kStateHome = "home";
const char* kStateConnecting = "connecting";
const char* kStateStreaming = "streaming";
const char* kStateError = "error";

// `docs/spec/copy.md` §Sign in, path B.
const char* kOtpMismatch = "That code didn't match. Try again or resend.";
// `docs/spec/copy.md`, Sign-in Path B (phone): the string for a number that is not E.164 after
// normalisation. Documented copy, not invented here.
const char* kPhoneInvalid = "That doesn't look like a phone number.";

// `docs/spec/copy.md` §Play flow, the four customer-visible stage lines. The engine's own
// stage names (`LiGetStageName()`, e.g. "RTSP handshake") are internal and are never shown
// - each one is mapped onto one of these four sentences.
const char* kStageWaitingForRig = "Waiting for a free rig";
const char* kStagePreparingRig = "Preparing the rig";
const char* kStagePreparingStream = "Preparing the stream";
const char* kStageReady = "Ready";

const char* const kCopyDeckStageLines[] = {
    kStageWaitingForRig,
    kStagePreparingRig,
    kStagePreparingStream,
    kStageReady,
};

bool isCopyDeckStageLine(const QString& stage)
{
    for (const char* line : kCopyDeckStageLines) {
        if (stage == QLatin1String(line)) {
            return true;
        }
    }
    return false;
}

// Maps an engine stage name onto a `copy.md` §Play flow line. Anything unrecognised gets
// the middle line rather than the engine's own words.
QString stageLineFor(const QString& engineStage)
{
    if (isCopyDeckStageLine(engineStage)) {
        // Already a SeatHub line (the tracer's stubbed sequence emits these directly).
        return engineStage;
    }

    const QString s = engineStage.toLower();
    if (s.contains(QLatin1String("rtsp")) || s.contains(QLatin1String("handshake"))
            || s.contains(QLatin1String("control")) || s.contains(QLatin1String("video"))
            || s.contains(QLatin1String("audio")) || s.contains(QLatin1String("input"))) {
        return QString::fromLatin1(kStagePreparingStream);
    }
    if (s.contains(QLatin1String("platform")) || s.contains(QLatin1String("name"))) {
        return QString::fromLatin1(kStagePreparingRig);
    }
    if (s.contains(QLatin1String("start"))) {
        return QString::fromLatin1(kStageReady);
    }
    return QString::fromLatin1(kStagePreparingStream);
}

// The HUD's publisher: the one place that composites the SeatHub HUD into the engine's video
// output, and the one place `Session::get()` is reached for it (ADR-0045).
//
// It composites through the overlay path the engine already has - `D3D11VARenderer::renderFrame()`
// draws the overlays into the stream's own swapchain immediately before `Present()` - so the HUD
// costs no second window and no second swapchain, and STREAM-01 holds by construction.
//
// A null surface means "hide" and disables the overlay, which is what the engine itself does when
// a connection returns to healthy. A non-null surface is published and the manager's contract is
// that ownership transfers in every case, so this never leaks one.
bool publishHudSurface(SDL_Surface* surface)
{
    Session* session = Session::get();
    if (session == nullptr) {
        // No active session to composite into: the tracer path, or a session already torn down.
        if (surface != nullptr) {
            SDL_FreeSurface(surface);
        }
        return false;
    }

    Overlay::OverlayManager& manager = session->getOverlayManager();

    if (surface == nullptr) {
        manager.setOverlayState(Overlay::OverlayStatusUpdate, false);
        return true;
    }

    // Enable before publishing. The manager only notifies on a state change, so this is
    // idempotent, and it means a HUD published before the renderer has registered is still picked
    // up on the first frame after it does - the HUD's first publish happens on
    // `connectionStarted`, which is before the engine creates its stream window (D-01).
    manager.setOverlayState(Overlay::OverlayStatusUpdate, true);
    return manager.updateOverlaySurface(Overlay::OverlayStatusUpdate, surface);
}

} // namespace

// Control-plane callbacks arrive on the network thread once `startNetworkThreads()` has moved the
// client off the Qt main thread. Everything they touch - `appState`, `failure`, the settings
// bridge, the token store - belongs to the facade's own thread, so it is marshalled back before
// it is applied. Without this a stream's liveness callback would mutate QML-facing state from
// another thread.
template <typename Fn>
void onClientThread(QObject* owner, Fn fn)
{
    if (QThread::currentThread() == owner->thread()) {
        fn();
        return;
    }
    QMetaObject::invokeMethod(owner, std::move(fn), Qt::QueuedConnection);
}

SeatHubClient::SeatHubClient(QObject* parent)
    : QObject(parent),
      m_appState(QString::fromLatin1(kStateSignedOut)),
      m_session(new SessionLifecycle(this)),
      m_settings(new SettingsBridge(this)),
      m_updates(new UpdateFeedClient(this)),
      // No parent on these seven: `moveToThread()` refuses an object that has one, and they have
      // to leave the Qt main thread before a stream starts (see `startNetworkThreads`). The
      // destructor owns them instead. `m_teardown` is the one HR-01 added to this group - it was
      // parented to `this` and therefore never moved, so its verify timer was a main-thread child
      // while every control-plane callback arrives on the network thread.
      m_controlPlane(new ControlPlaneClient(nullptr)),
      m_tokenStore(new TokenStore(this)),
      m_sessionChannel(new SessionWebSocket(nullptr)),
      // `m_pairing` has to travel with the seam and its poll timer: a `QTimer` fires only on the
      // thread its object lives on, and the main thread is suspended for the whole stream.
      m_pairing(new PairingController(nullptr)),
      m_pairingSeam(new ProductionPairingSeam(nullptr)),
      m_teardown(new TeardownController(nullptr)),
      m_liveness(new LivenessTimer(nullptr)),
      m_horizon(new AuthorizedThroughTimer(nullptr))
{
    connect(m_session, &SessionLifecycle::stageStarting, this, &SeatHubClient::handleStageStarting);
    connect(m_session, &SessionLifecycle::stageFailed, this, &SeatHubClient::handleStageFailed);
    connect(m_session, &SessionLifecycle::connectionStarted, this, &SeatHubClient::handleConnectionStarted);
    connect(m_session, &SessionLifecycle::displayLaunchError, this, &SeatHubClient::handleDisplayLaunchError);
    connect(m_session, &SessionLifecycle::displayLaunchWarning, this, &SeatHubClient::handleDisplayLaunchWarning);
    connect(m_session, &SessionLifecycle::quitStarting, this, &SeatHubClient::handleQuitStarting);
    connect(m_session, &SessionLifecycle::sessionFinished, this, &SeatHubClient::handleSessionFinished);
    connect(m_session, &SessionLifecycle::readyForDeletion, this, &SeatHubClient::handleReadyForDeletion);

    // ---------------------------------------------------------------- the control-plane bridge
    //
    // One object owns each safety-critical sequence, and the facade owns the objects. The
    // connection topology is deliberately a fan-in to the facade: QML sees typed state
    // (`appState`, `stageText`, `failure`) and never a frame, a header or a token (D-35).
    // The seam gets upstream's real handshake; without it `PairingController` fails closed, which
    // is the state 03-03 shipped and the gap this wiring closes.
    m_pairingSeam->setHandshake(&runUpstreamPairingHandshake);
    m_pairing->setSeam(m_pairingSeam);
    m_pairing->setControlPlane(m_controlPlane);
    m_teardown->setControlPlane(m_controlPlane);
    m_teardown->setTokenStore(m_tokenStore);
    m_liveness->setControlPlane(m_controlPlane);

    connect(m_sessionChannel, &SessionWebSocket::sessionStateReceived,
            this, &SeatHubClient::handleSessionState);
    connect(m_sessionChannel, &SessionWebSocket::sessionBillingReceived,
            this, &SeatHubClient::handleSessionBilling);
    connect(m_sessionChannel, &SessionWebSocket::sessionWarningReceived,
            this, &SeatHubClient::handleSessionWarning);

    connect(m_pairing, &PairingController::pairingCompleted,
            this, &SeatHubClient::handlePairingCompleted);
    connect(m_pairing, &PairingController::pairingFailed,
            this, &SeatHubClient::handlePairingFailed);

    connect(m_teardown, &TeardownController::teardownCompleted,
            this, &SeatHubClient::handleTeardownCompleted);
    connect(m_teardown, &TeardownController::teardownFailed,
            this, &SeatHubClient::handleTeardownFailed);

    // D-33: the horizon is the only locally enforced end.
    connect(m_horizon, &AuthorizedThroughTimer::horizonReached,
            this, &SeatHubClient::handleHorizonReached);
    // D-33: a liveness failure after the grace window is a warning. It is wired to a warning
    // handler and not to `raiseFailure`, because ending a working stream over an undelivered
    // heartbeat is the failure mode this connection exists to prevent.
    connect(m_liveness, &LivenessTimer::livenessWarning, this, &SeatHubClient::onLivenessWarning);

    // The HUD composites through the engine's own overlay path; wiring it here rather than in
    // the HUD keeps the engine reference in one place (and the HUD free of `Session`).
    m_hud.setPublisher(&publishHudSurface);
}

SeatHubClient::~SeatHubClient()
{
    // Order matters. Everything that was moved to the control-plane thread is brought home *from
    // inside that thread* first, the thread is then stopped and joined, and only then is anything
    // destroyed here. Qt refuses both a cross-thread destruction and a cross-thread
    // `moveToThread()` - each is a warning, not an error - so the previous version of this
    // destructor silently left five objects belonging to a thread that had already stopped, one of
    // them owning a live `QWebSocket` and a running reconnect timer, and then deleted the control
    // plane a second time (CR-01: it now destroys itself on its own thread, inside
    // `stopOwnedThread()`).
    ControlPlaneClient* controlPlane = m_controlPlane;
    m_controlPlane = nullptr;
    if (controlPlane != nullptr) {
        const bool joinable = controlPlane->onOwnThread()
                              && QThread::currentThread() != controlPlane->thread();
        if (!joinable) {
            // Either it was never moved - this thread owns it and deletes it here - or, in the
            // degenerate case, this destructor is running on the network thread itself, where
            // joining would deadlock. Deleting it from its own thread is still thread-correct.
            delete controlPlane;
        }
        else {
            const QList<QObject*> workers = { m_pairingSeam, m_pairing, m_liveness, m_horizon,
                                              m_sessionChannel, m_teardown };
            const QThread* home = QThread::currentThread();
            // Blocking, and issued through an object that lives on the worker thread: a move is
            // only accepted when it comes from the object's own thread, and this is the last point
            // in the lifetime where that thread still runs.
            QMetaObject::invokeMethod(controlPlane, [workers, home]() {
                QThread* owner = QThread::currentThread();
                for (QObject* worker : workers) {
                    if (worker != nullptr && worker->thread() == owner) {
                        worker->moveToThread(const_cast<QThread*>(home));
                    }
                }
            }, Qt::BlockingQueuedConnection);

            // Stops the thread and destroys `controlPlane` on it, exactly once. No
            // `ControlPlaneClient*` may be touched after this line.
            controlPlane->stopOwnedThread();
        }
    }

    // The seam first: it holds the completion callback into the controller, so destroying it
    // first means no handshake result can be delivered to a half-destroyed controller.
    delete m_pairingSeam;
    delete m_pairing;
    delete m_liveness;
    delete m_horizon;
    delete m_sessionChannel;
    delete m_teardown;
}

bool SeatHubClient::inControlPlaneSession() const
{
    return !m_sessionId.isEmpty();
}

void SeatHubClient::startNetworkThreads()
{
    // Upstream hijacks the Qt main thread for the whole stream and suspends Qt processing
    // (`app/streaming/session.cpp:1965-1966`). Anything that must keep running while the
    // customer is streaming - the liveness timer, the horizon timer, the session channel -
    // therefore needs a thread with its own event loop, or it is silent for exactly the
    // interval it exists to cover.
    //
    // `moveToOwnThread()` is idempotent, so calling it per session is safe.
    m_controlPlane->moveToOwnThread();

    QThread* networkThread = m_controlPlane->thread();
    if (networkThread == nullptr || networkThread == QThread::currentThread()) {
        return;
    }

    m_liveness->moveToThread(networkThread);
    m_horizon->moveToThread(networkThread);
    m_sessionChannel->moveToThread(networkThread);

    // The seam's handshake blocks - upstream's first pairing request is issued with no client-side
    // timeout at all - so it runs on a pool thread and only its deadline timer lives here.
    m_pairingSeam->moveToThread(networkThread);
    m_pairing->moveToThread(networkThread);

    // HR-01: teardown's verify timer is the reason this object has to live here, and the reason it
    // is constructed without a parent (`moveToThread()` refuses a parented object). Every
    // control-plane callback - `endSession`, `fetchSession` - arrives on this thread, so a
    // main-thread `m_verifyTimer` would be refused its start and leave teardown stuck before
    // `Clear`, with the DPAPI blobs still on disk and `teardownCompleted()` never emitted.
    m_teardown->moveToThread(networkThread);
}

void SeatHubClient::beginSession(const QString& sessionId)
{
    if (sessionId.isEmpty()) {
        raiseFailure(SeatHubFailure::local(QStringLiteral("We couldn't start this session.")));
        return;
    }

    m_sessionId = sessionId;
    m_clientUuid.clear();

    clearFailure();
    setStageText(QString::fromLatin1(kStageWaitingForRig));
    setAppState(QString::fromLatin1(kStateConnecting));

    startNetworkThreads();

    // The channel and pairing start with the session; pairing runs while the customer watches
    // the connecting view and never asks them for anything (STREAM-03). The engine is started
    // from `handlePairingCompleted()` - pairing first, stream second, which is the order the
    // protocol requires.
    m_sessionChannel->setBaseUrl(m_controlPlane->baseUrl());
    m_sessionChannel->open(sessionId);
    // Marshalled, not called: the controller and its poll timer now live on the network thread,
    // and its authorization callbacks come back there too.
    onClientThread(m_pairing, [this, sessionId]() { m_pairing->start(sessionId); });
}

QString SeatHubClient::reference() const
{
    return m_failure.value(QStringLiteral("reference")).toString();
}

void SeatHubClient::setHostWindow(QWindow* window)
{
    m_hostWindow = window;
}

void SeatHubClient::setAppState(const QString& state)
{
    if (m_appState == state) {
        return;
    }
    // Logged so a run's own log records the whole UI lifecycle. Without this the only way to
    // tell how far the tracer reached is a screenshot, and "did the window hide before the
    // stream window existed" (D-01) is not something a screenshot can answer.
    qCInfo(seathubClient) << "app state" << m_appState << "->" << state;
    const bool wasStreaming = m_appState == QLatin1String(kStateStreaming);
    m_appState = state;
    const bool isStreaming = m_appState == QLatin1String(kStateStreaming);

    if (wasStreaming != isStreaming) {
        // Pitfall 6 / T-03-15: the settings page stops accepting writes for the duration.
        // Pitfall 8 / T-03-17: so does the updater - replacing the binary a live session is
        // running from is not something this client does (D-41).
        m_settings->setStreamingActive(isStreaming);
        m_updates->setStreamingActive(isStreaming);
    }

    emit appStateChanged();
}

void SeatHubClient::setInSettings(bool inSettings)
{
    if (m_inSettings == inSettings) {
        return;
    }
    m_inSettings = inSettings;
    emit inSettingsChanged();
}

void SeatHubClient::setStageText(const QString& text)
{
    if (m_stageText == text) {
        return;
    }
    qCInfo(seathubClient) << "stage line ->" << text;
    m_stageText = text;
    emit stageTextChanged();
}

void SeatHubClient::raiseFailure(const SeatHubFailure& failure)
{
    m_failure = failure.toVariantMap();
    // The raw engine text stays out of the view layer entirely: it is logged here for
    // support and dropped (D-51, T-03-05).
    if (!failure.diagnostic.isEmpty()) {
        qWarning("SeatHub engine diagnostic (not shown to the customer, reference %s): %s",
                 qPrintable(failure.reference), qPrintable(failure.diagnostic));
    }
    emit failureChanged();
    setAppState(QString::fromLatin1(kStateError));
}

void SeatHubClient::clearFailure()
{
    if (m_failure.isEmpty()) {
        return;
    }
    m_failure.clear();
    emit failureChanged();
}

void SeatHubClient::start()
{
    if (m_appState == QLatin1String(kStateConnecting)
            || m_appState == QLatin1String(kStateStreaming)) {
        return;
    }

    clearFailure();
    setStageText(QString::fromLatin1(kStageWaitingForRig));
    setAppState(QString::fromLatin1(kStateConnecting));

    if (!m_session->start(m_hostWindow)) {
        raiseFailure(SeatHubFailure::generic());
    }
}

void SeatHubClient::interrupt()
{
    // D-02: the local stop. The engine's own quit keystroke does the stopping; the control-plane
    // teardown runs from `handleReadyForDeletion()`, because the documented order is
    // stop the stream, destroy the SDL window, and only then close the session server-side.
    m_session->interrupt();
}

void SeatHubClient::requestOtp(const QString& phoneE164)
{
    // ME-03: one normalisation rule, in `ControlPlaneClient`, applied here and in `verifyOtp`.
    // Separators a person types are stripped, a leading `00` becomes `+`, and anything that is
    // not then `^\+[1-9][0-9]{7,14}$` (`docs/spec/openapi.yaml` `phone_e164`) is rejected
    // locally rather than spent as a round trip that can only come back refused.
    const QString phone = ControlPlaneClient::normalisePhoneE164(phoneE164);
    if (phone.isEmpty()) {
        emit otpRejected(QString::fromLatin1(kPhoneInvalid), QString());
        return;
    }

    startNetworkThreads();

    m_controlPlane->requestOtp(phone, [this, phone](const ControlPlaneResult& result) {
        onClientThread(this, [this, phone, result]() {
            if (!result.ok) {
                // This is where an HTTP 200 carrying `status:false` lands (Pitfall 4). The
                // control plane's own sentence is what the customer reads, and its `SH-` code is
                // what support searches on - the client does not substitute either.
                const SeatHubFailure failure = result.toFailure();
                emit otpRejected(failure.error, failure.reference);
                return;
            }
            emit otpRequested(phone);
        });
    });
}

void SeatHubClient::verifyOtp(const QString& phoneE164, const QString& code)
{
    // Shape only, locally: the control plane is the authority on whether the code is right, and
    // `OtpVerifyRequest.code` is documented as `^[0-9]{6}$`. A locally malformed code is not
    // worth a round trip.
    if (code.size() != 6) {
        emit otpRejected(QString::fromLatin1(kOtpMismatch), QString());
        return;
    }

    // ME-03: the same normalisation `requestOtp` applies, so the two calls cannot send two
    // spellings of one number - the control plane keys the pending code on what it was sent.
    const QString phone = ControlPlaneClient::normalisePhoneE164(phoneE164);
    if (phone.isEmpty()) {
        emit otpRejected(QString::fromLatin1(kPhoneInvalid), QString());
        return;
    }

    startNetworkThreads();

    m_controlPlane->verifyOtp(phone, code, [this, phone](const ControlPlaneResult& result) {
        onClientThread(this, [this, phone, result]() {
            if (!result.ok) {
                const SeatHubFailure failure = result.toFailure();
                emit otpRejected(failure.error, failure.reference);
                return;
            }

            AuthTokenPair pair;
            if (!AuthTokenPair::parse(result.body, &pair)) {
                const SeatHubFailure failure = SeatHubFailure::local(
                    QStringLiteral("Sign-in didn't finish. Try again."));
                emit otpRejected(failure.error, failure.reference);
                return;
            }

            // D-30: the long-lived credential goes to disk only as a DPAPI blob. The access
            // token stays in memory - it is short-lived, it is on every request, and it has no
            // reason to be at rest at all. `storeToken` is the only write path, and it writes no
            // plaintext.
            if (!m_tokenStore->storeToken(TokenStore::refreshTokenName(), pair.refreshToken)) {
                const SeatHubFailure failure = SeatHubFailure::local(
                    QStringLiteral("This PC wouldn't let us save your sign-in."));
                emit otpRejected(failure.error, failure.reference);
                return;
            }

            m_controlPlane->setAccessToken(pair.accessToken);

            m_identity = phone;
            emit identityChanged();
            emit otpAccepted();
            setAppState(QString::fromLatin1(kStateHome));
        });
    });
}

void SeatHubClient::signOut()
{
    // STREAM-10: signing out leaves no stored pairing and no stored credential. The channel and
    // both timers stop before the store is swept.
    m_sessionChannel->close();
    m_liveness->stop();
    m_horizon->disarm();
    onClientThread(m_pairing, [this]() { m_pairing->cancel(); });
    m_teardown->cancel();
    m_tokenStore->clearAll();

    m_sessionId.clear();
    m_clientUuid.clear();
    m_controlPlane->setAccessToken(QString());

    m_identity.clear();
    emit identityChanged();
    clearFailure();
    setInSettings(false);
    setAppState(QString::fromLatin1(kStateSignedOut));
}

void SeatHubClient::dismissError()
{
    clearFailure();
    setInSettings(false);
    setAppState(m_identity.isEmpty() ? QString::fromLatin1(kStateSignedOut)
                                     : QString::fromLatin1(kStateHome));
}

// ---------------------------------------------------------------------------
// Engine lifecycle -> typed view state
// ---------------------------------------------------------------------------

void SeatHubClient::handleStageStarting(const QString& stage)
{
    setStageText(stageLineFor(stage));
}

void SeatHubClient::handleStageFailed(const QString& stage, int errorCode, const QString& failingPorts)
{
    if (m_appState == QLatin1String(kStateStreaming)) {
        // Mid-stream failures keep the session's own end-reason copy; the engine's stage
        // failure is diagnostic detail from here on.
        raiseFailure(mapLaunchError(stage));
        return;
    }
    raiseFailure(mapStageFailure(stage, errorCode, failingPorts));
}

void SeatHubClient::handleConnectionStarted()
{
    // D-14: from here on the settings page can report what the session actually settled on,
    // rather than what was asked for.
    m_settings->noteConnectionStarted();
    setStageText(QString::fromLatin1(kStageReady));
    setAppState(QString::fromLatin1(kStateStreaming));

    // D-31/D-34: liveness starts with the stream and reports every 10 s, including `state` and
    // `error_code`. It runs on the network thread, because the Qt main thread is inside SDL's
    // event loop from here until the stream ends (`session.cpp:1965-1966`).
    if (inControlPlaneSession()) {
        m_liveness->start(m_sessionId);
    }

    // D-56: the duration timer starts here. This fires before the engine creates its SDL window
    // (D-01), so the first HUD publish may arrive before the renderer has registered; the 1 Hz
    // heartbeat re-publishes and the stream picks the HUD up on its first frame.
    m_hud.beginSession();
}

void SeatHubClient::handleDisplayLaunchError(const QString& text)
{
    // Never shown verbatim (T-03-05). `mapLaunchError` keeps `text` as diagnostic only.
    raiseFailure(mapLaunchError(text));
}

void SeatHubClient::handleDisplayLaunchWarning(const QString& text)
{
    // The engine's other public reporting seam: a saved setting it could not honour. The text is
    // engine wording, so it never reaches a screen; the bridge matches it to the setting and
    // produces SeatHub's own sentence, leaving the saved preference untouched (D-14, D-51).
    m_settings->noteLaunchWarning(text);
}

void SeatHubClient::handleQuitStarting()
{
    // Deliberately inert. `quitStarting` fires before deferred engine cleanup and before
    // SDL destroys its window, so restoring the Qt window here would put two windows on
    // screen at once (Pitfall 1). The restore happens on `readyForDeletion`.
}

void SeatHubClient::handleSessionFinished(int portTestResult)
{
    // D-56: the duration timer stops here, which is the interval the plan specifies.
    m_hud.endSession();

    if (portTestResult != 0 && portTestResult != -1 && m_failure.isEmpty()) {
        raiseFailure(mapPortTestFailure(portTestResult));
    }
}

void SeatHubClient::handleReadyForDeletion()
{
    // SDL destruction is proven by the time this arrives (D-03), so the Qt window may come
    // back and the appState may leave "streaming". SessionSegue.qml performs the actual
    // `window.visible = true`.
    if (m_appState == QLatin1String(kStateStreaming)) {
        setAppState(m_identity.isEmpty() ? QString::fromLatin1(kStateSignedOut)
                                         : QString::fromLatin1(kStateHome));
    }

    // The stream is over, so nothing more is reported to the control plane about it (D-31).
    m_liveness->stop();
    m_horizon->disarm();
    m_sessionChannel->close();

    // D-10/STREAM-10: disable -> remove -> verify, then leave no local state. This is the point
    // in the documented sequence where the server-side teardown runs: the SDL window has been
    // destroyed, so the stream that the rig is about to revoke no longer exists on this PC.
    if (inControlPlaneSession() && m_teardown->stage() != TeardownStage::Done) {
        m_teardown->teardown(m_sessionId, m_clientUuid);
    }

    // D-37 / D-14: the launch's negotiated results and its in-memory overrides are over. The
    // saved preferences were never touched, so the settings page goes back to showing them.
    m_settings->noteSessionFinished();

    // D-41: a release that arrived during the session is offered now, not during it.
    m_updates->sessionFinished();
}

void SeatHubClient::openSettings()
{
    setInSettings(true);
}

void SeatHubClient::closeSettings()
{
    setInSettings(false);
}

// ---------------------------------------------------------------------------
// The control plane's session channel
// ---------------------------------------------------------------------------

void SeatHubClient::handleSessionState(const SessionInfo& session)
{
    // D-33: `authorized_through` is the control plane's horizon, and it moves forward when the
    // lease is renewed. Arming from here is the extension path; nothing in this client invents a
    // deadline of its own.
    if (!session.authorizedThrough.isEmpty()) {
        m_horizon->arm(session.authorizedThrough);
    }

    if (session.isTerminal()) {
        // The server considers this session over - the wallet ran out, the owner's reservation
        // arrived, or an operator ended it. Stop locally; teardown follows from
        // `handleReadyForDeletion()`.
        m_liveness->stop();
        if (m_appState == QLatin1String(kStateStreaming)
                || m_appState == QLatin1String(kStateConnecting)) {
            m_session->interrupt();
        }
    }
}

void SeatHubClient::handleSessionBilling(const QString& sessionId, int minutesBilled,
                                         int balanceMinutes, int minuteIndex)
{
    // Wallet facts are the server's (`docs/spec/client.md` §Wallet authority). They are stored
    // for display and nothing else: no local arithmetic, no anticipation of the next charge.
    m_billing.insert(QStringLiteral("session_id"), sessionId);
    m_billing.insert(QStringLiteral("minutes_billed"), minutesBilled);
    m_billing.insert(QStringLiteral("balance_minutes"), balanceMinutes);
    m_billing.insert(QStringLiteral("minute_index"), minuteIndex);
    emit billingChanged();
}

void SeatHubClient::handleSessionWarning(const QString& sessionId, const QString& warning,
                                         const QString& deadlineAt)
{
    Q_UNUSED(sessionId);
    m_sessionWarning = warning;
    m_billing.insert(QStringLiteral("warning_deadline_at"), deadlineAt);
    emit sessionWarningChanged();
}

// ---------------------------------------------------------------------------
// D-33: the horizon is the only locally enforced end
// ---------------------------------------------------------------------------

void SeatHubClient::handleHorizonReached()
{
    if (!inControlPlaneSession()) {
        return;
    }
    if (m_appState != QLatin1String(kStateStreaming)
            && m_appState != QLatin1String(kStateConnecting)) {
        return;
    }

    // The authorization this session was given has run out and the control plane has not
    // extended it. Stop locally rather than keep streaming on credit the server never approved.
    m_liveness->stop();
    m_sessionChannel->close();
    m_session->interrupt();
}

void SeatHubClient::onLivenessWarning()
{
    // D-33, and the reason `LivenessTimer::livenessWarning` is wired here rather than to
    // `raiseFailure()`: the media path does not go through the control plane, so a report that
    // cannot be delivered says nothing about whether the customer's stream is working. The
    // session continues and the timer keeps retrying. Ending it here would turn a control-plane
    // outage into a customer-visible one.
    qCWarning(seathubClient) << "liveness grace window elapsed; session continues (D-33)";
}

// ---------------------------------------------------------------------------
// Pairing and teardown results
// ---------------------------------------------------------------------------

void SeatHubClient::handlePairingCompleted(const QString& clientUuid)
{
    // The exact Sunshine client UUID. It is the only identifier that ever refers to this client
    // - never the readable label, the address or a list index (Pitfall 3, D-07) - and teardown
    // needs it to verify the removal.
    m_clientUuid = clientUuid;

    // Pairing is done, so the stream may start. This is the same lifecycle entry the tracer uses,
    // which is what keeps the D-01/D-03 window sequence proven on the real path too.
    if (!m_session->start(m_hostWindow)) {
        raiseFailure(SeatHubFailure::generic());
    }
}

void SeatHubClient::handlePairingFailed(const SeatHubFailure& failure)
{
    // Fail closed with a SeatHub error the error screen can render - a reason, a retry and an
    // `SH-` reference - never a Moonlight dialog (ADR-0008, D-51, STREAM-03).
    m_sessionChannel->close();
    raiseFailure(failure);
}

void SeatHubClient::handleTeardownCompleted()
{
    m_liveness->stop();
    m_horizon->disarm();
    m_sessionChannel->close();

    m_sessionId.clear();
    m_clientUuid.clear();
    m_billing.clear();
    m_sessionWarning.clear();
    emit billingChanged();
    emit sessionWarningChanged();

    // Only leave the error view if the customer is not looking at one; a teardown that succeeded
    // says nothing about an unrelated failure the error screen is already showing.
    if (m_appState != QLatin1String(kStateError)) {
        setAppState(m_identity.isEmpty() ? QString::fromLatin1(kStateSignedOut)
                                         : QString::fromLatin1(kStateHome));
    }
}

void SeatHubClient::handleTeardownFailed(const SeatHubFailure& failure)
{
    // STREAM-10: the rig-side disable/remove/verify did not complete, or something was left
    // stored on this PC. Reporting success would tell the customer the opposite of what is true,
    // so it is surfaced with a reason, a retry and a reference.
    raiseFailure(failure);
}
