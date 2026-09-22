#include "seathub_client.h"

#include <QDesktopServices>
#include <QLoggingCategory>
#include <QThread>
#include <QWindow>

#include <SDL.h>

#include "agent_config.h"
#include "countries.h"
#include "duration_text.h"
#include "region.h"
#include "session_lifecycle.h"
#include "settings_bridge.h"
#include "update_feed_client.h"
#include "web_origin.h"

// The production pairing handshake and the host record it resolves. Included here and nowhere
// else: these are the translation units that reach into `app/backend/`, which is how the rest of
// `app/seathub/` stays free of upstream includes (and how the seam stays testable with no engine
// in the test binary).
#include "pairing_handshake.h"

// `<SDL.h>` above is here for `SDL_Surface` and `SDL_FreeSurface` alone. The engine type itself is
// no longer named in this file: the HUD's publisher reaches the engine through the session object
// the lifecycle is driving, so `Session` appears exactly once in the fork - in
// `moonlight_engine_session.cpp` - and the publish path is assertable with a fake in its place.

Q_LOGGING_CATEGORY(seathubClient, "seathub.client")

namespace {

// appState values (D-35). QML switches views on these; they are part of the facade's
// contract, not an implementation detail.
const char* kStateRestoring = "restoring";
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
// `docs/spec/copy.md` § Sign in: said before anything is sent. The screen checks both first; the
// facade repeats them so no caller can spend a round trip on an empty field.
const char* kIdentifierMissing = "Enter your email or phone number.";
const char* kPasswordMissing = "Enter your password.";
// `docs/spec/copy.md` § Signup code errors: "When the server cannot be reached at all". Shown by
// every sign-in step when the request never reached the control plane.
const char* kOfflineSentence = "We couldn't reach SevenHills. Try again in a moment.";

// homeStatus values (audit F1). Five states of the one home view, from `screens.md` §23 and the
// copy deck: the populated state, the named loading state, the "no rig" empty state, the offline
// state and the server's own refusal, shown in its own words.
const char* kHomeReady = "ready";
const char* kHomeChecking = "checking";
const char* kHomeBusy = "busy";
const char* kHomeOffline = "offline";
const char* kHomeRefused = "refused";

// The quality profile Play asks for. `ADR-0011` fixes the vocabulary; `1080p60` is its base
// value and the one the control plane uses in its own examples.
const char* kDefaultQualityProfile = "1080p60";

// `docs/spec/copy.md` §Play flow: the three customer-visible stage lines (`screens.md` §24). The
// engine's own stage names (`LiGetStageName()`, e.g. "RTSP handshake") are internal and are never
// shown: the engine reaching any of its stages only says that the second stage is under way.
const char* kStagePreparingRig = "Preparing the rig";
const char* kStagePreparingStream = "Preparing the stream";
const char* kStageStreaming = "Streaming";

const int kStageRig = 1;
const int kStageStream = 2;
const int kStageStreamingNow = 3;

// The line of connecting stage `stage` (1 to 3), or empty for anything else.
QString stageLine(int stage)
{
    switch (stage) {
    case kStageRig:
        return QString::fromLatin1(kStagePreparingRig);
    case kStageStream:
        return QString::fromLatin1(kStagePreparingStream);
    case kStageStreamingNow:
        return QString::fromLatin1(kStageStreaming);
    default:
        return QString();
    }
}

// The connecting stage a session state belongs to, from the states `docs/spec/state-machines.md`
// lists: the rig is being found and prepared (stage 1), the rig is ready and this client pairs and
// starts the engine (stage 2), or the stream is running (stage 3). Every other state - a session that
// is ending or over, or a value this client does not know - belongs to no stage, so it moves nothing:
// what the customer reads is never a guess.
int connectStageForState(const QString& state)
{
    if (state == QLatin1String("REQUESTED") || state == QLatin1String("ALLOCATED")
            || state == QLatin1String("PREPARING")) {
        return kStageRig;
    }
    if (state == QLatin1String("READY")) {
        return kStageStream;
    }
    if (state == QLatin1String("ACTIVE")) {
        return kStageStreamingNow;
    }
    return 0;
}

// `docs/spec/copy.md` §Session end reasons, verbatim. The left column of the table is the
// internal key and never reaches a customer; these are the right column, which is what the home
// screen renders after a session ends (audit E10). `%1` is the session's real `minutes_billed` -
// copy.md: "the client substitutes the session's real `minutes_billed`".
const char* kEndCustomerEnded = "You ended the session. Unused minutes stay in your account.";
const char* kEndHostLost =
    "We lost contact with this rig, so the session ended. You were charged for %1 minutes.";
const char* kEndClientSilent =
    "We lost contact with your device, so the session ended. You were charged for %1 minutes.";
const char* kEndConnectTimeout =
    "You didn't start streaming in time, so the session was released. You were not charged.";
const char* kEndReadinessTimeout = "This rig didn't come back in time. You were not charged.";
const char* kEndGraceExpired =
    "We couldn't reconnect, so the session ended. You were charged for %1 minutes.";
const char* kEndOwnerReservation =
    "The rig's owner reserved it, so the session ended. You were charged for %1 minutes.";
const char* kEndBalanceExhausted = "Your balance ran out, so the session ended.";
const char* kEndTeardownTimeout =
    "Something went wrong ending this session, so we closed it. You were charged for the minutes "
    "you used.";
const char* kEndOperatorForced = "Support ended this session. You were charged for %1 minutes.";

// Every number with a unit is mono (copy.md §5), and the end-reason sentences are rendered as
// `Text.StyledText`, so the minute count is wrapped in the mono family. The family name is the
// `fontMonoDefault` token; C++ cannot read `Tokens.qml`, and `hud_overlay.cpp` requests the same
// family by name for the same reason.
QString monoMinutes(int minutesBilled)
{
    return QStringLiteral("<font face=\"Geist Mono\">%1</font>").arg(minutesBilled);
}

// `docs/spec/copy.md` §Session end reasons -> the sentence the customer reads. Returns an empty
// string for a reason the deck has no line for: `SESSION_LOST`, `LEASE_GUARD_LOST` and
// `RECONNECT_LIMIT` are documented to "fall back to the client's generic ended-session string",
// which copy.md never spells out - showing nothing is honest, inventing the sentence is not.
QString endReasonSentence(const QString& endReason, int minutesBilled, bool styled = true)
{
    struct ReasonLine {
        const char* reason;
        const char* sentence;
    };
    static const ReasonLine kLines[] = {
        { "CUSTOMER_ENDED", kEndCustomerEnded },
        { "HOST_LOST", kEndHostLost },
        { "CLIENT_SILENT", kEndClientSilent },
        { "CONNECT_TIMEOUT", kEndConnectTimeout },
        { "READINESS_TIMEOUT", kEndReadinessTimeout },
        // copy.md: MODE_BOOT_TIMEOUT is "the same line as READINESS_TIMEOUT above".
        { "MODE_BOOT_TIMEOUT", kEndReadinessTimeout },
        { "GRACE_EXPIRED", kEndGraceExpired },
        { "OWNER_RESERVATION", kEndOwnerReservation },
        { "BALANCE_EXHAUSTED", kEndBalanceExhausted },
        { "TEARDOWN_TIMEOUT", kEndTeardownTimeout },
        { "OPERATOR_FORCED", kEndOperatorForced },
    };

    for (const ReasonLine& line : kLines) {
        if (endReason == QLatin1String(line.reason)) {
            QString sentence = QString::fromUtf8(line.sentence);
            if (sentence.contains(QLatin1String("%1"))) {
                sentence = sentence.arg(styled ? monoMinutes(minutesBilled)
                                               : QString::number(minutesBilled));
            }
            return sentence;
        }
    }

    if (!endReason.isEmpty()) {
        qCWarning(seathubClient) << "no copy.md sentence for end reason" << endReason;
    }
    return QString();
}

// The HUD's publisher: the one place the SeatHub HUD reaches the engine's video output (ADR-0045).
//
// It composites through the overlay path the engine already has - `D3D11VARenderer::renderFrame()`
// draws the overlays into the stream's own swapchain immediately before `Present()` - so the HUD
// costs no second window and no second swapchain, and STREAM-01 holds by construction.
//
// It reaches the engine through the session object the lifecycle is driving, not through the
// engine's `Session::get()` global. That global is null on every path that is not a running engine
// session, so a publisher built on it silently did nothing whenever the session was a fake or had
// already been torn down - which, before the Plan 03-06 fix, was always, because no engine session
// was ever attached.
//
// Ownership (ADR-0045): a non-null surface is never the caller's afterwards. With a session
// attached the engine takes it in every case - it keeps an accepted one and frees a refused one -
// so nothing here releases anything. With no session there is no swapchain to composite into and no
// engine to take it, so it is released here and the HUD is told the publish did not land. That
// second branch is why this asks whether a session is attached at all rather than relying on the
// lifecycle's `false`: the two answers differ in what happens to the surface.
bool publishHudSurface(SessionLifecycle* lifecycle, SDL_Surface* surface)
{
    if (lifecycle == nullptr || lifecycle->upstreamSession() == nullptr) {
        // No active session to composite into: nothing is streaming, or the session already tore
        // down.
        if (surface != nullptr) {
            SDL_FreeSurface(surface);
        }
        return false;
    }

    return lifecycle->publishOverlaySurface(surface);
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
      // Entered before anything else is shown: the sign-in form must never flash while the stored
      // credential is being checked (`restoreSession()` resolves this to home or signed_out).
      m_appState(QString::fromLatin1(kStateRestoring)),
      m_homeStatus(QString::fromLatin1(kHomeReady)),
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
    // The sign-in field's data: read from the binary, never fetched (Phase 5 D-02).
    qRegisterMetaType<SessionInfo>("SessionInfo");
    m_countries = SeatHubCountries::all();
    m_defaultCountryCode = SeatHubRegion::initialCountryCode();
    m_animationEffects = SeatHubSystem::animationEffectsEnabled();
    m_urlOpener = [](const QUrl& url) { return QDesktopServices::openUrl(url); };

    // The profile's three histories. They live on this thread and ask the control-plane client for
    // their pages; that client marshals onto its own thread once a stream has begun, and each model
    // marshals the reply back. A 401 on any of them is a rejected credential (`screens.md` §27).
    m_sessionHistory = new SessionListModel(m_controlPlane, this);
    m_creditHistory = new CreditHistoryModel(m_controlPlane, this);
    m_topupHistory = new TopupListModel(m_controlPlane, this);
    for (CustomerListModel* list : { static_cast<CustomerListModel*>(m_sessionHistory),
                                     static_cast<CustomerListModel*>(m_creditHistory),
                                     static_cast<CustomerListModel*>(m_topupHistory) }) {
        connect(list, &CustomerListModel::credentialRefused,
                this, &SeatHubClient::handleCredentialRefused);
    }

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
    m_liveness->setControlPlane(m_controlPlane);

    connect(m_sessionChannel, &SessionWebSocket::sessionStateReceived,
            this, &SeatHubClient::handleSessionState);
    connect(m_sessionChannel, &SessionWebSocket::sessionBillingReceived,
            this, &SeatHubClient::handleSessionBilling);
    connect(m_sessionChannel, &SessionWebSocket::sessionWarningReceived,
            this, &SeatHubClient::handleSessionWarning);

    // Audit F12: the HUD's reconnect line. The control plane's `DISCONNECTED` warning is one
    // trigger (see `handleSessionWarning`); the channel dropping is the other, and it is the one
    // that fires when the customer's own connection goes away and no frame can arrive at all.
    // `setReconnecting` only stores an atomic the HUD's timer thread reads, so the socket's
    // thread may call it; this connection is queued to the facade's thread anyway.
    connect(m_sessionChannel, &SessionWebSocket::opened, this, [this]() {
        m_hud.setReconnecting(false);
    });
    connect(m_sessionChannel, &SessionWebSocket::dropped, this, [this](int, int) {
        m_hud.setReconnecting(true);
    });

    // The session as the control plane reports it, read on the pairing poll's own tick (ADR-0055):
    // the connecting stages come from here. The session channel above is never opened, so this is the
    // only thing that feeds `handleSessionState` while a session is connecting.
    connect(m_pairing, &PairingController::sessionRead,
            this, &SeatHubClient::handleSessionState);

    connect(m_pairing, &PairingController::pairingCompleted,
            this, &SeatHubClient::handlePairingCompleted);
    connect(m_pairing, &PairingController::pairingFailed,
            this, &SeatHubClient::handlePairingFailed);

    // D-37 / WR-05: the authorization's quality profile. Emitted before pairing starts, which is
    // what puts the override in place before the engine negotiates the stream.
    connect(m_pairing, &PairingController::authorizationGranted,
            this, &SeatHubClient::handleAuthorizationGranted);

    // The host the handshake resolved, in the same emission as the completion above and always
    // ahead of it (see `ProductionPairingSeam::finish()`), so `handlePairingCompleted()` finds an
    // engine session already attached. Queued: the seam lives on the network thread.
    connect(m_pairingSeam, &ProductionPairingSeam::hostResolved,
            this, &SeatHubClient::handleHostResolved);

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

    // CUST-15: the balance the liveness report's own wallet read produced. To the HUD it goes
    // straight from the network thread - a queued hop to this thread would wait out the whole
    // stream, because this thread is suspended for it (`session.cpp:1965-1966`), and the credit
    // would never move and no warning would ever fire. `HudOverlay::noteCreditMinutes` is built
    // for that call (a mutex-guarded state, a publisher safe from any thread). The facade's own
    // properties, which only the hidden Qt window reads, follow when this thread runs again.
    connect(m_liveness, &LivenessTimer::walletRead, this,
            [this](qint64 minutes) { m_hud.noteCreditMinutes(minutes); }, Qt::DirectConnection);
    connect(m_liveness, &LivenessTimer::walletRead, this,
            [this](qint64 minutes) { applyLiveBalance(minutes); }, Qt::QueuedConnection);

    // The HUD composites through the engine's own overlay path; wiring it here rather than in
    // the HUD keeps the engine reference in one place (and the HUD free of `Session`). The session
    // is read per call rather than captured: which engine session is attached changes per launch.
    m_hud.setPublisher([this](SDL_Surface* surface) {
        return publishHudSurface(m_session, surface);
    });
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

    // Last, and only when no stream is running. `run()` blocks inside this thread's event loop, so
    // a live session means this destructor is running *underneath* the engine's own frames - the
    // object cannot be destroyed there. An engine still streaming at shutdown is released by the
    // process, which is the only correct answer available at that point.
    if (m_engineSession != nullptr && !m_session->active()) {
        delete m_engineSession;
        m_engineSession = nullptr;
    }
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

    setAttachedSession(sessionId);
    setAttachedSessionEnded(false);
    m_clientUuid.clear();
    // A new session's teardown has not been asked for yet. Without this the second and later
    // sessions in one run never tear down (defect F-9; `teardown_guard.h`).
    m_teardownGuard.reset();

    // A new session clears the previous one's end reason: the home screen shows the outcome of
    // the session that just ended, never a stale one (audit E10).
    setEndReasonText(QString());
    setHomeStatus(QString::fromLatin1(kHomeReady));

    clearFailure();
    resetConnecting();
    setAppState(QString::fromLatin1(kStateConnecting));

    startNetworkThreads();

    // Pairing starts with the session and runs while the customer watches the connecting view; it
    // never asks them for anything (STREAM-03). The engine is started from `handlePairingCompleted()`
    // - pairing first, stream second, which is the order the protocol requires. The session's own
    // channel is NOT opened: the control plane serves no such route, so the client reads the session
    // on this same poll instead (ADR-0055).
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

    // The balance is read on every arrival at Home: after a restore, after a sign-in, and when a
    // session or a failure hands the customer back (CUST-06). A no-op unless signed in.
    if (state == QLatin1String(kStateHome)) {
        refreshBalance();
    }
}

void SeatHubClient::setSignedIn(bool signedIn)
{
    if (m_signedIn == signedIn) {
        return;
    }
    m_signedIn = signedIn;
    emit signedInChanged();
}

void SeatHubClient::setAttachedSession(const QString& sessionId)
{
    m_sessionId = sessionId;
    if (sessionId.isEmpty()) {
        m_sessionEnded = false;
    }
    updateLiveSession();
}

void SeatHubClient::setAttachedSessionEnded(bool ended)
{
    m_sessionEnded = ended;
    updateLiveSession();
}

void SeatHubClient::updateLiveSession()
{
    const bool live = !m_sessionId.isEmpty() && !m_sessionEnded;
    if (m_liveSession == live) {
        return;
    }
    qCInfo(seathubClient) << "live session" << m_liveSession << "->" << live;
    m_liveSession = live;
    emit liveSessionChanged();
}

void SeatHubClient::setInSettings(bool inSettings)
{
    if (m_inSettings == inSettings) {
        return;
    }
    m_inSettings = inSettings;
    emit inSettingsChanged();
}

void SeatHubClient::setInProfile(bool inProfile)
{
    if (m_inProfile == inProfile) {
        return;
    }
    m_inProfile = inProfile;
    emit inProfileChanged();
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

void SeatHubClient::advanceConnectStage(int stage)
{
    // Forward only, and silent when nothing changed: a late reply that reports an earlier state, or a
    // tick that reports the same one again, leaves the stepper exactly as it was. Once connecting has
    // stopped the stepper is frozen where it stopped.
    if (m_connectFailed || stage <= m_connectStage || stage > kStageStreamingNow) {
        return;
    }
    qCInfo(seathubClient) << "connecting stage" << m_connectStage << "->" << stage;
    m_connectStage = stage;
    setStageText(stageLine(stage));
    emit connectStageChanged();
}

bool SeatHubClient::connectingSession() const
{
    return m_appState == QLatin1String(kStateConnecting) && inControlPlaneSession();
}

void SeatHubClient::clearStall()
{
    if (!m_connectFailed && m_stalledStepText.isEmpty() && m_stalledReasonText.isEmpty()) {
        return;
    }
    m_connectFailed = false;
    m_stalledStepText.clear();
    m_stalledReasonText.clear();
    emit connectFailedChanged();
}

void SeatHubClient::raiseConnectFailure(const SeatHubFailure& failure, const QString& endReason,
                                        int minutesBilled)
{
    // The first thing that stopped connecting is what the customer reads; whatever else fails while
    // it is still showing is logged and not allowed to rewrite it.
    if (m_connectFailed) {
        qCInfo(seathubClient) << "connecting already stopped; not replacing what the customer reads";
        return;
    }

    // The engine's own text and the stage detail are logged for support and never shown (D-51).
    if (!failure.diagnostic.isEmpty()) {
        qWarning("SeatHub connect diagnostic (not shown to the customer, reference %s): %s",
                 qPrintable(failure.reference), qPrintable(failure.diagnostic));
    }

    // What the customer reads under the stage: the deck's sentence for what the server decided when
    // it named a reason the deck has a line for; otherwise the failure's own sentence (the server's,
    // verbatim, or the client's local one); and the generic sentence when there is none at all. The
    // sentence is drawn as styled text (the minute count is mono), so anything else is escaped.
    SeatHubFailure shown = failure;
    QString reasonText;
    const QString deckSentence = endReasonSentence(endReason, minutesBilled, true);
    if (!deckSentence.isEmpty()) {
        shown.error = endReasonSentence(endReason, minutesBilled, false);
        reasonText = deckSentence;
    }
    else {
        if (shown.error.isEmpty()) {
            shown.error = SeatHubFailure::generic().error;
        }
        reasonText = shown.error.toHtmlEscaped();
    }

    m_failure = shown.toVariantMap();
    // Stopped at the stage that was active. Before anything was read that is the first one: it is
    // where a session that exists begins.
    m_stalledStepText = QStringLiteral("Stopped at: %1").arg(stageLine(qMax(m_connectStage, kStageRig)));
    m_stalledReasonText = reasonText;
    m_connectFailed = true;
    qCInfo(seathubClient) << "connecting stopped at stage" << m_connectStage;

    m_sessionChannel->close();
    emit failureChanged();
    emit connectFailedChanged();
}

void SeatHubClient::resetConnecting()
{
    if (m_connectStage == 0 && m_stageText.isEmpty()) {
        return;
    }
    m_connectStage = 0;
    setStageText(QString());
    emit connectStageChanged();
}

void SeatHubClient::setHomeStatus(const QString& status)
{
    if (m_homeStatus == status) {
        return;
    }
    // Logged for the same reason the stage line is: "was the customer on the empty state or the
    // offline state" is not a question a screenshot of a machine nobody can see can answer.
    qCInfo(seathubClient) << "home status" << m_homeStatus << "->" << status;
    m_homeStatus = status;
    emit homeStatusChanged();
}

void SeatHubClient::setEndReasonText(const QString& text)
{
    if (m_endReasonText == text) {
        return;
    }
    m_endReasonText = text;
    emit endReasonTextChanged();
}

void SeatHubClient::raiseFailure(const SeatHubFailure& failure)
{
    // The error view replaces whatever connecting was showing, so a stall is over.
    clearStall();
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
    clearStall();
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

    // Play is the control plane's allocation (`POST /api/sessions`, D-35). Without an access
    // token there is nothing to allocate with, so there is nothing to stream: a stream is always
    // the object of an allocated session. This used to run the Plan 03-02 tracer instead, which is
    // how a build with no control plane produced a window and no stream; the lifecycle now refuses
    // to start with no engine session attached and this reports the failure.
    if (!m_controlPlane->hasAccessToken()) {
        beginLocalAttempt();
        return;
    }

    // A session this client already has, and the server has not reported over, is Resume session:
    // it is picked up again, not replaced by a request for another one - the control plane would
    // refuse that request anyway (a customer has at most one live session).
    if (m_liveSession) {
        qCInfo(seathubClient) << "resuming the attached session";
        beginSession(m_sessionId);
        return;
    }

    beginPlayRequest();
}

void SeatHubClient::beginLocalAttempt()
{
    clearFailure();
    setEndReasonText(QString());
    setHomeStatus(QString::fromLatin1(kHomeReady));
    resetConnecting();
    setAppState(QString::fromLatin1(kStateConnecting));

    if (!m_session->start(m_hostWindow)) {
        raiseFailure(SeatHubFailure::generic());
    }
}

void SeatHubClient::beginPlayRequest()
{
    clearFailure();
    setEndReasonText(QString());
    // The loading state belongs to the home view, so appState stays `home` until the control
    // plane answers: the customer sees the named loading line, not a connection screen for a
    // session that may never exist (`screens.md` §23, audit F1).
    setHomeStatus(QString::fromLatin1(kHomeChecking));

    startNetworkThreads();

    const QString profile = QString::fromLatin1(kDefaultQualityProfile);

    m_controlPlane->requestSession(profile, [this](const ControlPlaneResult& result) {
        onClientThread(this, [this, result]() {
            if (!result.ok) {
                applyPlayFailure(result);
                return;
            }

            const QString sessionId = result.body.value(QStringLiteral("id")).toString();
            if (sessionId.isEmpty()) {
                // A 2xx that carries no session is a contract violation, not a home state.
                setHomeStatus(QString::fromLatin1(kHomeReady));
                raiseFailure(SeatHubFailure::generic());
                return;
            }

            setHomeStatus(QString::fromLatin1(kHomeReady));
            beginSession(sessionId);
        });
    });
}

void SeatHubClient::applyPlayFailure(const ControlPlaneResult& result)
{
    // `NO_HOST_AVAILABLE` is the empty state, not an incident (copy.md §Play flow, "No rig"):
    // the right answer is the sentence and the next thing to do, not the error screen.
    if (result.failure == QLatin1String("NO_HOST_AVAILABLE")) {
        setHomeStatus(QString::fromLatin1(kHomeBusy));
        return;
    }

    // No HTTP status at all means the request never reached the control plane - `statusCode` is
    // 0 on that path (`ControlPlaneClient`). That is the offline state the copy deck's offline
    // sentence covers, and it keeps the customer on the home view instead of a failure screen
    // for what is very often a dropped Wi-Fi connection.
    if (result.statusCode == 0 && result.reference.isEmpty()) {
        setHomeStatus(QString::fromLatin1(kHomeOffline));
        return;
    }

    // The control plane answered and said no, in its own words: the balance floor (402), or a
    // refusal of the allocation itself that is not "nothing is free" (409, e.g. the customer
    // already has a session). These are the two responses `POST /api/sessions` documents besides
    // the unknown-profile 400. The customer stays on Home and reads that sentence and its reference
    // as the server wrote them, with a quiet way to top up beside it. The client applies no balance
    // rule of its own - it does not know the floor - so it is only ever the server that refuses.
    // Every other answer (400, 401, 5xx, a body that is not a refusal) is a real failure and keeps
    // the error screen.
    if (result.statusCode == 402 || result.statusCode == 409) {
        m_failure = result.toFailure().toVariantMap();
        emit failureChanged();
        setHomeStatus(QString::fromLatin1(kHomeRefused));
        return;
    }

    setHomeStatus(QString::fromLatin1(kHomeReady));
    raiseFailure(result.toFailure());
}

void SeatHubClient::retry()
{
    // Audit F21: retry re-runs the step, it does not just dismiss. With an identity the step
    // that failed was Play; without one there is nothing to retry but sign-in, so the sign-in
    // screen comes back.
    clearFailure();
    setInSettings(false);
    setInProfile(false);

    if (!m_signedIn) {
        setHomeStatus(QString::fromLatin1(kHomeReady));
        setAppState(QString::fromLatin1(kStateSignedOut));
        return;
    }

    setAppState(QString::fromLatin1(kStateHome));
    start();
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
                // what support searches on - the client does not substitute either. Only a request
                // that never arrived says the offline sentence.
                QString reference;
                const QString message = signInFailureText(result, &reference);
                emit otpRejected(message, reference);
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
                QString reference;
                const QString message = signInFailureText(result, &reference);
                emit otpRejected(message, reference);
                return;
            }

            AuthTokenPair pair;
            if (!AuthTokenPair::parse(result.body, &pair)) {
                const SeatHubFailure failure = SeatHubFailure::local(
                    QStringLiteral("Sign-in didn't finish. Try again."));
                emit otpRejected(failure.error, failure.reference);
                return;
            }

            QString failureText;
            if (!adoptSignIn(pair, phone, &failureText)) {
                emit otpRejected(failureText, QString());
                return;
            }
            emit otpAccepted();
            setAppState(QString::fromLatin1(kStateHome));
        });
    });
}

QString SeatHubClient::toE164(const QString& typed, const QString& dialCode) const
{
    return ControlPlaneClient::normalisePhoneE164(typed, dialCode);
}

QString SeatHubClient::signInFailureText(const ControlPlaneResult& result, QString* reference)
{
    // No HTTP status at all means the request never reached the control plane (`statusCode` is 0 on
    // that path). The deck has one sentence for that and it is the only one this screen adds.
    if (result.statusCode == 0) {
        if (reference) {
            reference->clear();
        }
        return QString::fromLatin1(kOfflineSentence);
    }
    const SeatHubFailure failure = result.toFailure();
    if (reference) {
        *reference = failure.reference;
    }
    return failure.error;
}

bool SeatHubClient::adoptSignIn(const AuthTokenPair& pair, const QString& identity,
                                QString* failureText)
{
    // D-30 / ADR-0050 D-10: the long-lived credential goes to disk only as a DPAPI blob.
    // Sessions are permanent, so the access token itself is the durable, non-expiring
    // credential and it lives in the access slot - the slot the launch-time restore
    // reads. `storeToken` writes no plaintext, and it is the only write path.
    if (!m_tokenStore->storeToken(TokenStore::accessTokenName(), pair.accessToken)) {
        if (failureText) {
            *failureText = SeatHubFailure::local(
                               QStringLiteral("This PC wouldn't let us save your sign-in.")).error;
        }
        return false;
    }
    // Whatever 0.1.x left in its own slot is now stale; two credentials at rest would let
    // a later launch restore the wrong one.
    m_tokenStore->clearToken(TokenStore::refreshTokenName());

    m_controlPlane->setAccessToken(pair.accessToken);

    ++m_authEpoch;
    setSignedIn(true);
    m_account.clear();
    m_identity = identity;
    emit identityChanged();
    return true;
}

void SeatHubClient::signInWithPassword(const QString& identifier, const QString& password)
{
    // Nothing is normalised: the server decides whether this is an email or a phone number and
    // which account it names. Surrounding space is not part of an identifier, so it is dropped.
    const QString who = identifier.trimmed();
    if (who.isEmpty()) {
        emit passwordSignInRejected(QString::fromLatin1(kIdentifierMissing), QString());
        return;
    }
    if (password.isEmpty()) {
        emit passwordSignInRejected(QString::fromLatin1(kPasswordMissing), QString());
        return;
    }

    startNetworkThreads();

    // The callback captures the identifier only. The password is handed to the request and is in no
    // closure, member or log line here (T-05-27).
    m_controlPlane->login(who, password, [this, who](const ControlPlaneResult& result) {
        onClientThread(this, [this, who, result]() {
            if (!result.ok) {
                // The server's own sentence, verbatim: the three refusals (`No account uses that
                // email or phone.`, `That password is wrong.`, `That account is disabled -
                // contact support.`) are distinct on purpose (ADR-0050 accepted-risk register).
                QString reference;
                const QString message = signInFailureText(result, &reference);
                emit passwordSignInRejected(message, reference);
                return;
            }

            AuthTokenPair pair;
            if (!AuthTokenPair::parse(result.body, &pair)) {
                const SeatHubFailure failure = SeatHubFailure::local(
                    QStringLiteral("Sign-in didn't finish. Try again."));
                emit passwordSignInRejected(failure.error, failure.reference);
                return;
            }

            QString failureText;
            if (!adoptSignIn(pair, who, &failureText)) {
                emit passwordSignInRejected(failureText, QString());
                return;
            }
            emit passwordSignInAccepted();
            setAppState(QString::fromLatin1(kStateHome));
        });
    });
}

QString SeatHubClient::websiteUrl(const QString& target) const
{
    // The profile's `Open the website` link: the origin itself, not one of the three paths (OD-11).
    if (target == QLatin1String("home")) {
        return SeatHubWeb::homeUrl().toString();
    }

    const char* path = nullptr;
    if (target == QLatin1String("signup")) {
        path = SeatHubWeb::kSignUpPath;
    }
    else if (target == QLatin1String("reset")) {
        path = SeatHubWeb::kResetPasswordPath;
    }
    else if (target == QLatin1String("topup")) {
        path = SeatHubWeb::kTopUpPath;
    }
    return path ? SeatHubWeb::url(path).toString() : QString();
}

bool SeatHubClient::openWebsite(const QString& target)
{
    const QString address = websiteUrl(target);
    if (address.isEmpty()) {
        return false;
    }
    // The address only: the log line names the target, never a credential (there is none in it).
    qCInfo(seathubClient) << "opening the website" << target;
    return m_urlOpener ? m_urlOpener(QUrl(address)) : false;
}

bool SeatHubClient::openTopUp()
{
    return openWebsite(QStringLiteral("topup"));
}

QVariantMap SeatHubClient::readAgentConfigFile(const QUrl& fileUrl)
{
    const QVariantMap described = AgentConfig::describe(fileUrl);

    // The path and whether a token was found are support facts. The token is not - it is not in
    // `described` and it is not logged here either.
    qCInfo(seathubClient) << "agent config file" << described.value(QStringLiteral("path")).toString()
                          << "token found" << described.value(QStringLiteral("ok")).toBool()
                          << described.value(QStringLiteral("error")).toString();

    return described;
}

void SeatHubClient::signOut()
{
    // Signing out leaves no stored pairing and no usable credential: not on disk, not in memory
    // here, and not valid on the server (ADR-0050, D-06). The channel and both timers stop before
    // the store is swept.
    m_sessionChannel->close();
    m_liveness->stop();
    m_horizon->disarm();
    onClientThread(m_pairing, [this]() { m_pairing->cancel(); });
    m_teardown->cancel();

    // The server's revoke goes out first - it is the only thing that makes "signed out" true for
    // anyone who has copied the credential. The request reads the credential when it runs, which
    // may be on the network thread a moment from now, so the in-memory copy is cleared when the
    // reply arrives (whatever it is) and not before.
    const quint64 epoch = ++m_authEpoch;
    if (m_controlPlane->hasAccessToken()) {
        startNetworkThreads();
        m_controlPlane->logout([this, epoch](const ControlPlaneResult& result) {
            onClientThread(this, [this, epoch, result]() {
                if (!result.ok) {
                    // Logged for support; the customer is already signed out here. An unreachable
                    // control plane leaves the server-side credential valid until the next
                    // reachable revoke - the local sweep below did not wait for it.
                    qCInfo(seathubClient) << "server revoke did not complete; status"
                                          << result.statusCode;
                }
                // A newer sign-in owns the in-memory credential now; leave it alone.
                if (m_authEpoch == epoch) {
                    m_controlPlane->setAccessToken(QString());
                }
            });
        });
    }

    // Unconditional, and never behind the network: an offline sign-out still removes the credential
    // from this machine (T-05-07).
    m_tokenStore->clearAll();

    setAttachedSession(QString());
    m_clientUuid.clear();

    setSignedIn(false);
    m_account.clear();
    m_identity.clear();
    emit identityChanged();
    resetBalance();
    clearFailure();
    resetConnecting();
    setInSettings(false);
    // Nothing of this customer's history, totals or identity rows is left for the next one.
    setInProfile(false);
    resetProfileData();
    setHomeStatus(QString::fromLatin1(kHomeReady));
    setEndReasonText(QString());
    setAppState(QString::fromLatin1(kStateSignedOut));
}

// ---------------------------------------------------------------------------
// Launch: restore the stored sign-in (CUST-08, D-06)
// ---------------------------------------------------------------------------

void SeatHubClient::restoreSession()
{
    if (m_restoreStarted) {
        return;
    }
    m_restoreStarted = true;

    // A credential written by 0.1.4, or parked by an update, is in the access slot after this
    // (WINDOWS #22). Outcomes are logged by the store; the credential never is.
    m_tokenStore->recoverAtStartup();

    const QString credential = m_tokenStore->retrieveToken(TokenStore::accessTokenName());
    if (credential.isEmpty()) {
        qCInfo(seathubClient) << "no stored credential; showing sign-in";
        setAppState(QString::fromLatin1(kStateSignedOut));
        return;
    }

    // Found - and that is all that is logged about it.
    qCInfo(seathubClient) << "stored credential found; confirming it with the control plane";
    m_controlPlane->setAccessToken(credential);
    startNetworkThreads();

    const quint64 epoch = m_authEpoch;
    m_controlPlane->fetchMe([this, epoch](const ControlPlaneResult& result) {
        onClientThread(this, [this, epoch, result]() {
            if (epoch != m_authEpoch) {
                return;
            }
            applyRestoreResult(result);
        });
    });
}

void SeatHubClient::applyRestoreResult(const ControlPlaneResult& result)
{
    if (result.ok) {
        AccountInfo account;
        if (AccountInfo::parse(result.body, &account)) {
            ++m_authEpoch;
            setSignedIn(true);
            setAccount(account);
            setHomeStatus(QString::fromLatin1(kHomeReady));
            setAppState(QString::fromLatin1(kStateHome));
            return;
        }
        // A 2xx that is not an account is a contract violation, not evidence the credential is
        // bad. Treated like an unreachable control plane below: keep the customer signed in.
        qCWarning(seathubClient) << "GET /api/me answered without an account";
    }
    else if (result.statusCode == 401) {
        // The control plane says the credential is no longer valid (revoked, or the account is
        // gone): the one case where a stored credential is discarded at launch.
        qCInfo(seathubClient) << "the stored credential was refused; clearing it";
        ++m_authEpoch;
        m_tokenStore->clearAll();
        m_controlPlane->setAccessToken(QString());
        setAppState(QString::fromLatin1(kStateSignedOut));
        return;
    }

    // Could not confirm it: no network, or the control plane had a bad moment. A lost connection
    // must never sign a customer out, so the credential stays and Home opens in its offline state
    // (copy.md, Support and errors). The next successful read of the wallet clears that state.
    qCInfo(seathubClient) << "could not confirm the stored credential (status" << result.statusCode
                          << "); staying signed in, offline";
    ++m_authEpoch;
    setSignedIn(true);
    setHomeStatus(QString::fromLatin1(kHomeOffline));
    setAppState(QString::fromLatin1(kStateHome));
}

void SeatHubClient::setAccount(const AccountInfo& account)
{
    m_account.clear();
    m_account.insert(QStringLiteral("display_name"), account.displayName);
    m_account.insert(QStringLiteral("username"), account.username);
    m_account.insert(QStringLiteral("email"), account.email);
    m_account.insert(QStringLiteral("phone"), account.phoneE164);
    m_account.insert(QStringLiteral("email_verified"), account.emailVerified);

    // What the customer is shown as: the first of the four the account has.
    m_identity.clear();
    const QStringList candidates = { account.phoneE164, account.email, account.username,
                                     account.displayName };
    for (const QString& candidate : candidates) {
        if (!candidate.isEmpty()) {
            m_identity = candidate;
            break;
        }
    }
    emit identityChanged();
}

// ---------------------------------------------------------------------------
// The balance (CUST-06)
// ---------------------------------------------------------------------------

void SeatHubClient::refreshBalance()
{
    if (!m_signedIn || !m_controlPlane->hasAccessToken()) {
        return;
    }

    startNetworkThreads();

    const quint64 epoch = m_authEpoch;
    m_controlPlane->fetchWallet([this, epoch](const ControlPlaneResult& result) {
        onClientThread(this, [this, epoch, result]() { applyWalletResult(epoch, result); });
    });
}

void SeatHubClient::applyWalletResult(quint64 epoch, const ControlPlaneResult& result)
{
    // Signed out, or signed in as someone else, since the read was issued: the answer is not this
    // session's to show.
    if (epoch != m_authEpoch || !m_signedIn) {
        return;
    }

    WalletInfo wallet;
    if (!result.ok || !WalletInfo::parse(result.body, &wallet)) {
        // A failed read changes nothing but the label: the last known value stays on screen, and
        // it never signs anybody out - not even on a 401, which a wallet read has no business
        // deciding (the launch-time restore is where a refused credential is handled).
        if (!m_balanceStale) {
            m_balanceStale = true;
            emit balanceChanged();
        }
        return;
    }

    setBalance(wallet.balanceMinutes);

    // The control plane is reachable, so an offline label from an unconfirmed restore (or a failed
    // Play) no longer describes the world.
    if (m_homeStatus == QLatin1String(kHomeOffline)) {
        setHomeStatus(QString::fromLatin1(kHomeReady));
    }
}

void SeatHubClient::setBalance(qint64 minutes)
{
    m_balanceMinutes = minutes;
    m_balanceText = durationText(minutes);
    m_balanceStale = false;
    emit balanceChanged();
}

void SeatHubClient::applyLiveBalance(qint64 minutes)
{
    // Signed out since the read was made (a queued answer can outlive the stream that asked): not
    // this session's to show. And nothing to say when it is what is already held.
    if (!m_signedIn || (m_balanceMinutes == minutes && !m_balanceStale)) {
        return;
    }
    setBalance(minutes);
}

void SeatHubClient::resetBalance()
{
    if (m_balanceMinutes == -1 && m_balanceText.isEmpty() && !m_balanceStale) {
        return;
    }
    m_balanceMinutes = -1;
    m_balanceText.clear();
    m_balanceStale = false;
    emit balanceChanged();
}

void SeatHubClient::dismissError()
{
    clearFailure();
    setInSettings(false);
    setInProfile(false);
    setHomeStatus(QString::fromLatin1(kHomeReady));
    setAppState(!m_signedIn ? QString::fromLatin1(kStateSignedOut)
                            : QString::fromLatin1(kStateHome));
}

// ---------------------------------------------------------------------------
// Engine lifecycle -> typed view state
// ---------------------------------------------------------------------------

void SeatHubClient::handleStageStarting(const QString& stage)
{
    // The engine only starts its own stages once the rig is ready and paired, so reaching any of them
    // means the second stage is under way. Its own name for the stage is internal and is not shown
    // (D-51), so nothing of it is read beyond the fact that it began.
    Q_UNUSED(stage);
    advanceConnectStage(kStageStream);
}

void SeatHubClient::handleStageFailed(const QString& stage, int errorCode, const QString& failingPorts)
{
    if (m_appState == QLatin1String(kStateStreaming)) {
        // Mid-stream failures keep the session's own end-reason copy; the engine's stage
        // failure is diagnostic detail from here on.
        raiseFailure(mapLaunchError(stage));
        return;
    }
    // Before the stream started the customer is on the connecting view: it stopped there, at the stage
    // the engine was in, and says so in the deck's words rather than sending them to the error view.
    if (connectingSession()) {
        raiseConnectFailure(mapStageFailure(stage, errorCode, failingPorts));
        return;
    }
    raiseFailure(mapStageFailure(stage, errorCode, failingPorts));
}

void SeatHubClient::handleConnectionStarted()
{
    // D-14: from here on the settings page can report what the session actually settled on,
    // rather than what was asked for.
    m_settings->noteConnectionStarted();
    advanceConnectStage(kStageStreamingNow);
    setAppState(QString::fromLatin1(kStateStreaming));

    // The HUD is begun before liveness starts, because the reads liveness makes feed it:
    // `beginSession()` resets the credit and the once-per-session warning state, so a wallet read
    // that beat it would be wiped.
    //
    // D-56: the duration timer starts here. This fires before the engine creates its SDL window
    // (D-01), so the first HUD publish may arrive before the renderer has registered; the 1 Hz
    // heartbeat re-publishes and the stream picks the HUD up on its first frame.
    m_hud.beginSession();

    // A stream that just started is by definition connected (audit F12): whatever the last
    // session's channel did, this one is live now.
    m_hud.setReconnecting(false);

    // `Credit left` from the first frame: the last balance read before the stream (`screens.md`
    // 25). Display only - it can be old, so it fires no warning; the report below reads the wallet
    // at once and that read is the first thing that can.
    if (m_balanceMinutes >= 0) {
        m_hud.seedCreditMinutes(m_balanceMinutes);
    }

    // D-31/D-34: liveness starts with the stream and reports every 10 s, including `state` and
    // `error_code`, and reads the wallet on the same tick (CUST-15). It runs on the network thread,
    // because the Qt main thread is inside SDL's event loop from here until the stream ends
    // (`session.cpp:1965-1966`).
    if (inControlPlaneSession()) {
        m_liveness->start(m_sessionId);
    }
}

void SeatHubClient::handleDisplayLaunchError(const QString& text)
{
    // Never shown verbatim (T-03-05). `mapLaunchError` keeps `text` as diagnostic only.
    if (connectingSession()) {
        raiseConnectFailure(mapLaunchError(text));
        return;
    }
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
        setAppState(!m_signedIn ? QString::fromLatin1(kStateSignedOut)
                                : QString::fromLatin1(kStateHome));
    }

    // The stream is over, so nothing more is reported to the control plane about it (D-31).
    m_liveness->stop();
    m_horizon->disarm();
    m_sessionChannel->close();

    // D-10/STREAM-10: disable -> remove -> verify, then leave no local state. This is the point
    // in the documented sequence where the server-side teardown runs: the SDL window has been
    // destroyed, so the stream that the rig is about to revoke no longer exists on this PC.
    //
    // The guard is this facade's own per-session flag, reset in `beginSession()`, and not
    // `TeardownController::stage()`. That stage is sticky - it reaches `Done` on the first
    // teardown and is cleared only by `cancel()`, which only `signOut()` calls - so guarding on it
    // ran teardown for the first session and silently skipped every session after it: no
    // `POST /api/sessions/{id}/end`, no rig-side disable or unpair, no local clear, and no
    // `teardownCompleted()`. The flag is also atomic, which removes the unsynchronised
    // cross-thread read the security re-audit flagged on the same line (defect F-9).
    if (inControlPlaneSession() && m_teardownGuard.markStarted()) {
        m_teardown->teardown(m_sessionId, m_clientUuid);
    }

    // The engine session is finished with - the signal that arrived here was its own. Released
    // now rather than after the teardown completes: teardown talks to the control plane and needs
    // nothing from the engine object, and holding it until then would keep an engine `Session`
    // alive across the whole teardown for no reason.
    //
    // This release is permitted because the lifecycle clears its own state *above* the emission
    // that runs this handler: `releaseEngineSession()` refuses while the lifecycle reports itself
    // active, and this is a direct connection. Before that ordering was fixed the refusal was
    // unconditional and the finished engine object survived until the next launch's
    // `handleHostResolved()` (verifier finding W1; `session_lifecycle.cpp` has the whole story).
    releaseEngineSession();

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
// The profile (CUST-08, CUST-14)
// ---------------------------------------------------------------------------

QString SeatHubClient::accountStatus() const
{
    // Whether there is anything to draw: an account we have read is "ready" whatever a later read
    // does, so a refresh that fails never blanks the customer's own details.
    if (!m_account.isEmpty()) {
        return QStringLiteral("ready");
    }
    return m_accountFailed ? QStringLiteral("error") : QStringLiteral("loading");
}

CustomerListModel* SeatHubClient::listNamed(const QString& list) const
{
    if (list == QLatin1String("sessions")) {
        return m_sessionHistory;
    }
    if (list == QLatin1String("credit")) {
        return m_creditHistory;
    }
    if (list == QLatin1String("topups")) {
        return m_topupHistory;
    }
    return nullptr;
}

void SeatHubClient::openProfile()
{
    // Only from Home: the menu is also on the connecting and error views, and a customer who is
    // connecting is not looking at their history.
    if (!m_signedIn || m_appState != QLatin1String(kStateHome)) {
        return;
    }

    // Each visit starts clean: nothing from an earlier one is shown as current, and a reply still on
    // its way from that visit is dropped.
    resetProfileData();
    setInProfile(true);

    // The balance in the header and the `Credit left` tile are two reads of the same number; asking
    // for the header's again keeps them from disagreeing for the length of a visit.
    refreshBalance();
    readTotals();
    readAccount();
}

void SeatHubClient::closeProfile()
{
    setInProfile(false);
    resetProfileData();
}

void SeatHubClient::loadFirstPage(const QString& list)
{
    CustomerListModel* model = listNamed(list);
    if (model == nullptr || !m_signedIn || !m_controlPlane->hasAccessToken()) {
        return;
    }
    startNetworkThreads();
    model->loadFirstPage();
}

void SeatHubClient::loadNextPage(const QString& list)
{
    CustomerListModel* model = listNamed(list);
    if (model == nullptr || !m_signedIn || !m_controlPlane->hasAccessToken()) {
        return;
    }
    startNetworkThreads();
    model->loadNextPage();
}

void SeatHubClient::reloadList(const QString& list)
{
    CustomerListModel* model = listNamed(list);
    if (model == nullptr || !m_signedIn || !m_controlPlane->hasAccessToken()) {
        return;
    }
    startNetworkThreads();
    model->reload();
}

void SeatHubClient::reloadTotals()
{
    readTotals();
}

void SeatHubClient::reloadAccount()
{
    readAccount();
}

void SeatHubClient::resetProfileData()
{
    m_sessionHistory->reset();
    m_creditHistory->reset();
    m_topupHistory->reset();

    // Replies asked for before this point belong to a visit that is over.
    ++m_totalsRead;
    ++m_accountRead;

    m_totalsStatus = QStringLiteral("idle");
    m_hoursPlayedText.clear();
    m_creditLeftText.clear();
    m_totalsError.clear();
    m_totalsErrorReference.clear();
    emit totalsChanged();

    m_accountFailed = false;
    m_accountError.clear();
    m_accountErrorReference.clear();
    emit accountStatusChanged();
}

void SeatHubClient::readTotals()
{
    if (!m_signedIn || !m_controlPlane->hasAccessToken()) {
        return;
    }
    startNetworkThreads();

    const quint64 epoch = m_authEpoch;
    const quint64 read = ++m_totalsRead;

    m_totalsStatus = QStringLiteral("loading");
    m_totalsError.clear();
    m_totalsErrorReference.clear();
    emit totalsChanged();

    m_controlPlane->fetchUsage([this, epoch, read](const ControlPlaneResult& result) {
        onClientThread(this, [this, epoch, read, result]() {
            // Signed out or in as someone else since, or asked again since: not this read's to show.
            if (epoch != m_authEpoch || read != m_totalsRead || !m_signedIn) {
                return;
            }

            UsageInfo usage;
            if (result.ok && UsageInfo::parse(result.body, &usage)) {
                // Both are the server's numbers, printed as they came.
                m_hoursPlayedText = durationText(usage.minutesPlayed);
                m_creditLeftText = durationText(usage.balanceMinutes);
                m_totalsStatus = QStringLiteral("ready");
                m_totalsError.clear();
                m_totalsErrorReference.clear();
                emit totalsChanged();
                return;
            }

            if (!result.ok && result.statusCode == 401) {
                handleCredentialRefused();
                return;
            }

            // A 2xx that is not the totals is a contract violation: the deck's generic sentence and
            // no reference, never a zero that says "you have played nothing".
            const SeatHubFailure failure = result.ok ? SeatHubFailure::generic() : result.toFailure();
            m_hoursPlayedText.clear();
            m_creditLeftText.clear();
            m_totalsStatus = QStringLiteral("error");
            m_totalsError = failure.error;
            m_totalsErrorReference = failure.reference;
            emit totalsChanged();
        });
    });
}

void SeatHubClient::readAccount()
{
    if (!m_signedIn || !m_controlPlane->hasAccessToken()) {
        return;
    }
    startNetworkThreads();

    const quint64 epoch = m_authEpoch;
    const quint64 read = ++m_accountRead;

    if (m_accountFailed) {
        m_accountFailed = false;
        m_accountError.clear();
        m_accountErrorReference.clear();
        emit accountStatusChanged();
    }

    m_controlPlane->fetchMe([this, epoch, read](const ControlPlaneResult& result) {
        onClientThread(this, [this, epoch, read, result]() {
            if (epoch != m_authEpoch || read != m_accountRead || !m_signedIn) {
                return;
            }

            AccountInfo account;
            if (result.ok && AccountInfo::parse(result.body, &account)) {
                m_accountFailed = false;
                m_accountError.clear();
                m_accountErrorReference.clear();
                setAccount(account);
                emit accountStatusChanged();
                return;
            }

            if (!result.ok && result.statusCode == 401) {
                handleCredentialRefused();
                return;
            }

            // Whatever was read before stays (the status is derived from it); only an identity that
            // was never read shows the error.
            const SeatHubFailure failure = result.ok ? SeatHubFailure::generic() : result.toFailure();
            m_accountFailed = true;
            m_accountError = failure.error;
            m_accountErrorReference = failure.reference;
            emit accountStatusChanged();
        });
    });
}

void SeatHubClient::handleCredentialRefused()
{
    // The same end as a sign-out: the local credential is swept whatever the network says, the
    // server-side revoke is asked for (it will simply refuse a credential it already refused), and
    // the customer lands on sign-in.
    qCInfo(seathubClient) << "a profile read was refused (401); signing out";
    signOut();
}

// ---------------------------------------------------------------------------
// The control plane's session channel
// ---------------------------------------------------------------------------

void SeatHubClient::handleSessionState(const SessionInfo& session)
{
    // An answer about another session (one that arrives late, after a new Play) or one that arrives
    // with no session attached is not this session's to speak for.
    if (m_sessionId.isEmpty() || (!session.id.isEmpty() && session.id != m_sessionId)) {
        return;
    }

    // The connecting stages are the session's own state, as the control plane reports it.
    if (m_appState == QLatin1String(kStateConnecting)) {
        advanceConnectStage(connectStageForState(session.state));
    }

    // D-33: `authorized_through` is the control plane's horizon, and it moves forward when the
    // lease is renewed. Arming from here is the extension path; nothing in this client invents a
    // deadline of its own.
    if (!session.authorizedThrough.isEmpty()) {
        m_horizon->arm(session.authorizedThrough);
    }

    if (session.isTerminal()) {
        // The server considers this session over - the wallet ran out, the owner's reservation
        // arrived, or an operator ended it. Home no longer offers to resume it, even while its
        // teardown is still running. Stop locally; teardown follows from `handleReadyForDeletion()`.
        if (session.id.isEmpty() || session.id == m_sessionId) {
            setAttachedSessionEnded(true);
        }
        m_liveness->stop();
        if (m_session->active() || m_appState == QLatin1String(kStateStreaming)) {
            // The engine is running: it stops it, and its own end path carries the customer home.
            m_session->interrupt();
        }
        else if (connectingSession()) {
            // No engine has started, so there is nothing to interrupt and the customer would sit on a
            // stepper that looks alive for a session that is over. Connecting stops here, at the stage
            // it had reached, in the server's own words for why.
            onClientThread(m_pairing, [this]() { m_pairing->cancel(); });
            raiseConnectFailure(SeatHubFailure::generic(), session.endReason, session.minutesBilled);
        }
    }

    if (!session.endReason.isEmpty()) {
        // The session's own outcome, in the deck's words, ready for the home screen the customer
        // lands on once teardown finishes (audit E10). The minute count is the server's own
        // `minutes_billed`, never arithmetic done here.
        setEndReasonText(endReasonSentence(session.endReason, session.minutesBilled));
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

    // Audit F12: `DISCONNECTED` is the server saying this client stopped reporting, which is
    // exactly the moment the HUD's strip leaves "Elapsed" and says what is happening. The
    // strip never shows the "(2 of 5)" attempt counter D-56 defers.
    m_hud.setReconnecting(warning == QLatin1String("DISCONNECTED"));
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

    // A pairing only completes against a rig the session has made ready, so the second stage is under
    // way whether or not a poll happened to read READY first.
    advanceConnectStage(kStageStream);

    // Pairing is done, so the stream may start. The engine session was attached by
    // `handleHostResolved()`, which the seam emitted ahead of this; when it could not be built -
    // a host that is not a paired record, or an application list that names no single application
    // to launch - nothing is attached and `start()` fails closed here rather than starting a
    // session against nothing. Before the Plan 03-06 fix this `start()` fell through to the 03-02
    // tracer, which is how the client paired for real and then streamed a fake.
    if (!m_session->start(m_hostWindow)) {
        raiseFailure(SeatHubFailure::generic());
    }
}

void SeatHubClient::handleHostResolved(const PairedHostPtr& host)
{
    releaseEngineSession();
    if (m_engineSession != nullptr) {
        // A previous launch is still running. One lifecycle drives one session, so a second engine
        // cannot be attached to it; the host is dropped and `handlePairingCompleted()` reports the
        // failure rather than quietly streaming the old session under the new one's billing.
        qCWarning(seathubClient) << "a session is still active; ignoring the paired host";
        return;
    }

    MoonlightEngineSession* engine = MoonlightEngineSession::create(host);
    if (engine == nullptr) {
        // Fail closed. `MoonlightEngineSession::create()` logs which of the two reasons it was.
        return;
    }

    m_engineSession = engine;
    m_session->attachSession(engine);
}

void SeatHubClient::releaseEngineSession()
{
    if (m_engineSession == nullptr) {
        return;
    }

    if (m_session->active()) {
        // The engine may be inside `run()` on this thread's event loop: deleting the object under
        // it would leave the next touch a use-after-free. This branch is reached while a session
        // is starting or running (`handleHostResolved()`); at the end of one the lifecycle has
        // already cleared `active` by the time `readyForDeletion` arrives, so
        // `handleReadyForDeletion()` releases the object instead of refusing here (W1).
        qCWarning(seathubClient) << "engine session is still active; not releasing it";
        return;
    }

    // Detach before destroying: the lifecycle must not be holding an object that is going away.
    m_session->attachSession(nullptr);
    m_engineSession->deleteLater();
    m_engineSession = nullptr;
}

void SeatHubClient::handlePairingFailed(const SeatHubFailure& failure)
{
    // Fail closed with a SeatHub error the error screen can render - a reason, a retry and an
    // `SH-` reference - never a Moonlight dialog (ADR-0008, D-51, STREAM-03).
    m_sessionChannel->close();
    if (connectingSession()) {
        // The client's own pairing deadline, or the control plane refusing the pairing read: the stage
        // that was active stays on screen, marked failed, with the sentence that came with it.
        raiseConnectFailure(failure);
        return;
    }
    raiseFailure(failure);
}

void SeatHubClient::handleAuthorizationGranted(const QString& qualityProfile)
{
    // D-37 / WR-05: the control plane's `quality_profile` becomes this launch's resolution and
    // frame rate, in memory, leaving every saved preference untouched (D-12, STREAM-02). It is
    // applied here - on authorization, ahead of pairing - so the override is in place before
    // `handleConnectionStarted()` reports what the session actually settled on (D-14), and it is
    // handed over here rather than in the controller so that the bridge keeps exactly one writer
    // from the session path.
    //
    // The controller is the only object that sees the authorization; it puts this one field on the
    // signal and keeps the rest, including `pairing_pin` (STREAM-03).
    m_settings->applySessionOverride(qualityProfile);
}

void SeatHubClient::handleTeardownCompleted(const SessionInfo& finalSession)
{
    // Home says why the session ended, read from the session itself now that teardown has confirmed it
    // is over (CUST-15, D-21). The minute count is the server's own `minutes_billed`. A session the
    // server gave no reason for leaves the line empty: showing nothing is honest, inventing a sentence
    // is not.
    if (!finalSession.endReason.isEmpty()) {
        setEndReasonText(endReasonSentence(finalSession.endReason, finalSession.minutesBilled));
    }

    m_liveness->stop();
    m_horizon->disarm();
    m_sessionChannel->close();

    setAttachedSession(QString());
    m_clientUuid.clear();
    // The session's teardown claim goes with its id. The next `beginSession()` resets it anyway;
    // clearing it here keeps "no session" and "no claim" the same state, which is what the guard
    // documents.
    m_teardownGuard.reset();
    m_billing.clear();
    m_sessionWarning.clear();
    emit billingChanged();
    emit sessionWarningChanged();

    // Only leave the view if the customer is not looking at a failure: a teardown that succeeded says
    // nothing about the failure the error screen - or a stalled connecting view - is already showing.
    if (m_appState != QLatin1String(kStateError) && !m_connectFailed) {
        setAppState(!m_signedIn ? QString::fromLatin1(kStateSignedOut)
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
