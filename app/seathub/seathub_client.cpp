#include "seathub_client.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QStringList>
#include <QThread>
#include <QWindow>

#include <SDL.h>

#include "agent_config.h"
#include "countries.h"
#include "duration_text.h"
#include "engine_termination.h"
#include "log_shipper.h"
#include "log_tee.h"
#include "osd_compositor.h"
#include "quality_outbox.h"
#include "region.h"
#include "session_lifecycle.h"
#include "settings_bridge.h"
#include "stream_stats.h"
#include "telemetry.h"
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

// 06.1-19/J-07: the reconnect backoff (D-01's own reference points - ENet's ~10 s connection
// timeout, Sunshine's 10 s `ping_timeout` - not a multiple of them).
const int kReconnectInitialBackoffMs = 2000;
const int kReconnectMaxBackoffMs = 15000;

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
const char* kEndConnectFailed = "The stream didn't start. You were not charged.";
const char* kEndHostLost =
    "We lost contact with this rig, so the session ended. You were charged for %1 minutes.";
const char* kEndClientSilent =
    "We lost contact with your device, so the session ended. You were charged for %1 minutes.";
const char* kEndConnectTimeout =
    "You didn't start streaming in time, so the session was released. You were not charged.";
const char* kEndReadinessTimeout = "This rig didn't come back in time. You were not charged.";
const char* kEndGraceExpired =
    "We couldn't reconnect, so the session ended. You were charged for %1 minutes.";
// 06.1's ADR (D-03/copy.md §Reconnect and §Session end reasons): the sixth disconnect after five
// successful reconnects, and a client the server never heard from before the stream started.
const char* kEndReconnectLimit =
    "Your connection dropped too many times, so the session ended. You were charged for %1 "
    "minutes.";
const char* kEndClientAbsent =
    "We lost contact with your device before the stream started. You were not charged.";
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
        { "CONNECT_FAILED", kEndConnectFailed },
        { "HOST_LOST", kEndHostLost },
        { "CLIENT_SILENT", kEndClientSilent },
        { "CONNECT_TIMEOUT", kEndConnectTimeout },
        { "READINESS_TIMEOUT", kEndReadinessTimeout },
        // copy.md: MODE_BOOT_TIMEOUT is "the same line as READINESS_TIMEOUT above".
        { "MODE_BOOT_TIMEOUT", kEndReadinessTimeout },
        { "GRACE_EXPIRED", kEndGraceExpired },
        { "RECONNECT_LIMIT", kEndReconnectLimit },
        { "CLIENT_ABSENT", kEndClientAbsent },
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

// D-27: 16 bytes of `QRandomGenerator::system()`, hex-encoded - a fresh W3C trace id, 32
// lowercase hex characters, minted once per Play (`beginPlayRequest`).
QString randomTraceId()
{
    auto* generator = QRandomGenerator::system();
    const QString high = QString::number(generator->generate64(), 16).rightJustified(16, QLatin1Char('0'));
    const QString low = QString::number(generator->generate64(), 16).rightJustified(16, QLatin1Char('0'));
    return high + low;
}

/// Applies a `GET /api/me/telemetry` answer (D-01, D-18 SV-C3): a successful body is parsed and
/// handed to `SeatHubTelemetry::applyHandout()`; a 401, a transport failure, or a malformed body
/// changes nothing - telemetry is best-effort and never a reason to disrupt anything else. Called
/// (Plan 15) after the interactive sign-in's `fetchMe`, after restore's `fetchMe`, and at
/// `beginPlayRequest()`. A free function, not a member: `seathub_client.h` is not one of this
/// plan's files, and this needs no class state.
void applyTelemetryResult(const ControlPlaneResult& result)
{
    if (!result.ok) {
        return;
    }
    if (const std::optional<SeatHubTelemetry::Handout> handout =
            SeatHubTelemetry::parseHandout(result.body)) {
        SeatHubTelemetry::applyHandout(*handout);
    }
}

bool isClosedEndReason(const QString& endReason)
{
    static const QStringList closedEndReasons{
        QStringLiteral("RECONNECT_LIMIT"), QStringLiteral("HOST_LOST"),
        QStringLiteral("SESSION_LOST"), QStringLiteral("LEASE_GUARD_LOST"),
        QStringLiteral("OWNER_RESERVATION"), QStringLiteral("MODE_BOOT_TIMEOUT"),
        QStringLiteral("READINESS_TIMEOUT"), QStringLiteral("CONNECT_TIMEOUT"),
        QStringLiteral("GRACE_EXPIRED"), QStringLiteral("CUSTOMER_ENDED"),
        QStringLiteral("WALLET_EMPTY"), QStringLiteral("OPERATOR_FORCED"),
        QStringLiteral("CLIENT_SILENT"), QStringLiteral("BALANCE_EXHAUSTED"),
        QStringLiteral("TEARDOWN_TIMEOUT"), QStringLiteral("CONNECT_FAILED"),
        QStringLiteral("CLIENT_ABSENT")};
    return closedEndReasons.contains(endReason);
}

// 06.1's ADR item 4 / J-22: the guard-claim-and-teardown sequence
// `SeatHubClient::endAttachedSessionBeforeStream()` already runs for a pre-stream failure with no
// report to add, now carrying one - shared by `handlePairingFailed()`, the no-engine branch of
// `handlePairingCompleted()`, and `handleStageFailed()`'s pre-stream branch. A free function taking
// the exact pieces `endAttachedSessionBeforeStream()` itself reads (`seathub_client.h` is not part
// of this plan's touched files, so this is not a new declared member): `TeardownController` and
// `SessionTeardownGuard` already expose the public API this needs.
void endPreStreamSessionWithReport(SessionTeardownGuard& guard, TeardownController* teardown,
                                   const QString& sessionId, const QString& clientUuid,
                                   const EndReport& report)
{
    if (guard.markStarted()) {
        teardown->teardown(sessionId, clientUuid, true, report);
    }
    // else: this session's teardown was already claimed - by `handleReadyForDeletion()` or by an
    // earlier call here for the same session - and is already running (T-06.6-50); see
    // `endAttachedSessionBeforeStream()`'s own comment.
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
      // 06.4/ADR-0067: the account push channel, moved with the others in `startNetworkThreads()`.
      m_sse(new SseClient(nullptr)),
      // `m_pairing` has to travel with the seam and its poll timer: a `QTimer` fires only on the
      // thread its object lives on, and the main thread is suspended for the whole stream.
      m_pairing(new PairingController(nullptr)),
      m_pairingSeam(new ProductionPairingSeam(nullptr)),
      m_teardown(new TeardownController(nullptr)),
      m_liveness(new LivenessTimer(nullptr)),
      m_horizon(new AuthorizedThroughTimer(nullptr))
{
    // D-17/A-51, Plan 15: the one log tee for this process, installed before anything else in
    // this constructor could log. A second call from a later `SeatHubClient` constructed in the
    // same process (as `tst_facade_wiring.cpp` does, once per test) is a no-op
    // (`LogTee::install()`'s own header comment).
    LogTee::install();

    // D-09/D-15 (Plan 14): the bundled Open Sans SemiBold, registered once here - never from
    // `app/main.cpp`, which 06.3.1 edits (RESEARCH-FORK.md §3, Pitfall 12) - so every path that
    // constructs a facade (production, and `tst_facade_wiring.cpp`, once per test) has the font
    // registered before `handleHostResolved()` ever sets the compositor as the engine's
    // rasteriser. Idempotent (`registerOsdFonts()`'s own header comment); a failure only degrades
    // to Qt's own fallback face, logged there, not fatal here.
    registerOsdFonts();

    // A-51: the engine's own termination code, read from `Session::clConnectionTerminated`'s log
    // line (`engine_termination.h`'s own header comment says why this is the one route to it)
    // and handed to the liveness timer. Runs on whichever thread logged the line -
    // moonlight-common-c's own connection thread in production - and only parses and hands off:
    // `LivenessTimer::noteTermination()` marshals itself onto its own thread, the same way
    // `stop()` already does, so this lambda never touches timer state directly.
    m_terminationSinkHandle = LogTee::addSink(
        [this](LogLevel level, int category, int priority, const QString& text) {
            Q_UNUSED(level);
            int code = 0;
            const QByteArray utf8 = text.toUtf8();
            if (parseConnectionTerminated(category, priority, utf8.constData(), &code)) {
                m_liveness->noteTermination(code);
                // 06.1-19/J-07/T-06.1-56: the same line, the same call - no new thread crossing.
                // `handleDisplayLaunchError()` reads this (relaxed load) on the facade thread to
                // decide whether this termination is a reconnect-eligible one; a plain getter on
                // `m_liveness` from a different thread than the one that owns it is exactly the
                // unsynchronised cross-thread read WR-01 already fixed once elsewhere.
                m_lastTerminationCode.store(code, std::memory_order_relaxed);
            }
        });

    // 06.1-19/J-07: the reconnect backoff. Parented to `this` (never moved to the network
    // thread, unlike `m_liveness`/`m_pairing`): no engine runs between a drop and a successful
    // reconnect, so the facade's own event loop is not suspended for the backoff to wait through.
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &SeatHubClient::resumeAfterReconnect);

    // 06.4/ADR-0067, D-04/D-06: the paused-fallback read (L5). Repeating, parented to `this` for
    // the same reason `m_reconnectTimer` is (its class comment above).
    m_accountFallbackTimer = new QTimer(this);
    connect(m_accountFallbackTimer, &QTimer::timeout, this,
            &SeatHubClient::fetchAccountStateFallback);

    // 06.4/ADR-0067, D-07: the notice shows for L9 before this client signs out.
    m_signedOutNoticeTimer = new QTimer(this);
    m_signedOutNoticeTimer->setSingleShot(true);
    connect(m_signedOutNoticeTimer, &QTimer::timeout, this, [this]() {
        m_signedOutNotice = false;
        emit signedOutNoticeChanged();
        handleCredentialRefused();
    });

    // D-17: the end-of-stream video-stats block, read the same way and held until
    // `handleTeardownCompleted()` posts it (`handleVideoStatsParsed()`).
    m_statsWatcher = new StatsWatcher(this);
    m_statsSinkHandle = LogTee::addSink(m_statsWatcher->sink());
    connect(m_statsWatcher, &StatsWatcher::videoStatsParsed,
            this, &SeatHubClient::handleVideoStatsParsed);

    // D-11, Plan 13 Task 1 + Task 2: the 60s roll-up sampler and the connection-status sink.
    // The sampler lives on the network thread (moved in `startNetworkThreads()`). The status
    // sink only parses and marshals onto that thread via queued invoke — it never touches
    // timer state directly (same discipline as the termination sink above).
    m_sampler = new StreamQualitySampler(nullptr);
    m_statusSinkHandle = LogTee::addSink([sampler = m_sampler](LogLevel, int category, int priority,
                                                            const QString& message) {
        int status;
        const QByteArray text = message.toUtf8();
        if (parseConnectionStatusUpdate(category, priority, text.constData(), &status)) {
            QMetaObject::invokeMethod(sampler, [sampler, status]() {
                sampler->noteConnectionStatus(status);
            }, Qt::QueuedConnection);
        }
    });

    // The sign-in field's data: read from the binary, never fetched (Phase 5 D-02).
    qRegisterMetaType<SessionInfo>("SessionInfo");
    qRegisterMetaType<AccountStateInfo>("AccountStateInfo");
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
    m_sse->setControlPlane(m_controlPlane);

    connect(m_sessionChannel, &SessionWebSocket::sessionStateReceived,
            this, &SeatHubClient::handleSessionState);
    connect(m_sessionChannel, &SessionWebSocket::sessionBillingReceived,
            this, &SeatHubClient::handleSessionBilling);
    connect(m_sessionChannel, &SessionWebSocket::sessionWarningReceived,
            this, &SeatHubClient::handleSessionWarning);

    // Audit F12: the connection-state stage. The control plane's `DISCONNECTED` warning is one
    // trigger (see `handleSessionWarning`); the channel dropping is the other, and it is the one
    // that fires when the customer's own connection goes away and no frame can arrive at all. The
    // HUD's own "Connection lost. Reconnecting..." line is dead code Plan 22 removes (D-12; the
    // reconnect message moves to SeatHub's own window in 06.1, #21) - only the liveness stage
    // moves here now.
    connect(m_sessionChannel, &SessionWebSocket::opened, this, [this]() {
        m_liveness->setStage(QStringLiteral("streaming"));
    });
    connect(m_sessionChannel, &SessionWebSocket::dropped, this, [this](int, int) {
        m_liveness->setStage(QStringLiteral("reconnecting"));
    });

    // The session as the control plane reports it, read on the pairing poll's own tick (ADR-0055):
    // the connecting stages come from here. The session channel above is never opened, so this is the
    // only thing that feeds `handleSessionState` while a session is connecting.
    connect(m_pairing, &PairingController::sessionRead,
            this, &SeatHubClient::handleSessionState);

    // 06.4/ADR-0067, D-01: Home and Profile had no live update of their own before this
    // (RESEARCH-FORK.md) - a pushed `account.state` now updates the balance the same way a
    // successful `refreshBalance()` answer does, and reloads the Top-ups list and the totals when
    // the open top-up notice changed while the profile is open.
    connect(m_sse, &SseClient::accountState, this, &SeatHubClient::handleAccountState);

    // 06.4/ADR-0067: a `session.state` for the session being connected wakes the pairing poll at
    // once, so connecting speeds up on push; the poll keeps running as the fallback and the
    // source of truth (Task 2).
    connect(m_sse, &SseClient::sessionState, this, &SeatHubClient::handleSseSessionState);

    // 06.4/ADR-0067, D-04/D-06 (L5): the fallback read starts and stops with the channel's own
    // paused state.
    connect(m_sse, &SseClient::pausedChanged, this, &SeatHubClient::handleSsePaused);

    // 06.4/ADR-0067, D-07 (the owner's design A-87): a revoked stream starts the signed-out
    // sequence, deferred while the engine's stream is running (Phase 3 D-33).
    connect(m_sse, &SseClient::revoked, this, &SeatHubClient::handleSseRevoked);

    connect(m_pairing, &PairingController::pairingCompleted,
            this, &SeatHubClient::handlePairingCompleted);
    connect(m_pairing, &PairingController::pairingFailed,
            this, &SeatHubClient::handlePairingFailed);
    connect(m_pairing, &PairingController::stepFailed, this,
            [this](const QString& step, const QString& failureClass, qint64 elapsedMs) {
                if (m_reconnecting) {
                    m_lastFailedStep = QStringLiteral("reconnect");
                    noteStepOutcome(m_lastFailedStep, QStringLiteral("failed"),
                                    QStringLiteral("attempt_pair_failed"), elapsedMs, 1);
                } else {
                    m_lastFailedStep = step;
                    noteStepOutcome(step, QStringLiteral("failed"), failureClass, elapsedMs, 1);
                }
            });

    // A-68 / D-06 reversal: carries no quality profile any more (nothing applies one). Kept as
    // the D-11 signal that a real authorization arrived, ahead of pairing.
    connect(m_pairing, &PairingController::authorizationGranted,
            this, &SeatHubClient::handleAuthorizationGranted);

    // The host the handshake resolved, in the same emission as the completion above and always
    // ahead of it (see `ProductionPairingSeam::finish()`), so `handlePairingCompleted()` finds an
    // engine session already attached. Queued: the seam lives on the network thread.
    connect(m_pairingSeam, &ProductionPairingSeam::hostResolved,
            this, &SeatHubClient::handleHostResolved);

    // The classification for the result the seam is about to report (ADR-0072, Plan 09), emitted
    // by the seam immediately before it reports that result - hence always ahead of the
    // controller's `pairingFailed` / `pairingCompleted` for the same run, on either connection
    // type. Both handlers consume it: `handlePairingFailed()` reads the step and the class for a
    // failure, `handlePairingCompleted()` for the frozen record of a pairing that recovered
    // through the clear-and-retry. The seam's own `engineError` is deliberately not part of it.
    connect(m_pairingSeam, &ProductionPairingSeam::handshakeClassified, this,
            [this](const PairingHandshakeResult& result) {
                m_pairingClassification = result;
            });

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
    const bool summarisedAtQuit = m_playSummaryPending;
    finishPlay(QStringLiteral("app_quit"), QStringLiteral("cancelled"));
    m_hud.compositor().setStatsTap(nullptr);
    if (m_statusSinkHandle != 0) LogTee::removeSink(m_statusSinkHandle);
    m_statusSinkHandle = 0;
    // D-17/A-51: unregister both `LogTee` sinks FIRST, before anything either lambda captures -
    // `this` (the termination sink calls `m_liveness->noteTermination()`) and `m_statsWatcher`
    // (owned by the stats sink) - is destroyed. `LogTee`'s sink list is process-global and
    // outlives any one `SeatHubClient`; skipping this would leave a dangling-lambda call
    // registered for the rest of the process every time a `SeatHubClient` is destroyed, which
    // `tst_facade_wiring.cpp` does once per test (`log_tee.h`'s own `addSink()` header comment).
    LogTee::removeSink(m_terminationSinkHandle);
    LogTee::removeSink(m_statsSinkHandle);
    m_terminationSinkHandle = 0;
    m_statsSinkHandle = 0;

    // Plan 21 (D-14, SEATHUB § E.3 step 6): "at quit" for this fork is this destructor - one
    // `SeatHubClient` for the life of the production app. Queues whatever is still in memory
    // straight to the spool (never attempting a hand-off this late) and joins the worker thread -
    // required before process exit: a joinable `std::thread` still attached to the `LogShipper`
    // singleton at its own static destruction would call `std::terminate()`. A no-op, cheaply,
    // when `LogShipper` was never started (`tst_facade_wiring.cpp` constructs and destroys a
    // `SeatHubClient` once per test and never calls `SeatHubTelemetry::start()`).
    // Plan 09 (ADR-0072 item 9, V31) inserts the quit-time flush between a drain and that stop:
    // `drainBeforeSignOut()` first, so every line this run captured has reached the SDK or the
    // spool, then `flush(2000)` - `sentry_flush()` only, never `sentry_close()` (SPIKE T11: a
    // crash after close is captured without its fatal event, hooks or marker) - then the stop
    // takes the hand-off path away. Both the drain and the flush are cheap no-ops in a process
    // that never started telemetry: `drainBeforeSignOut()` returns when no worker was ever
    // started, and `sentry_flush()` finds no options and returns at once (`tst_facade_wiring`
    // constructs and destroys a `SeatHubClient` per test without ever starting telemetry).
    LogShipper::instance().drainBeforeSignOut();
    if (!summarisedAtQuit) SeatHubTelemetry::flush(2000);
    LogShipper::instance().stop();

    // Plan 09 (ADR-0072 item 5): the last act of a CLEAN quit - this destructor is the only thing
    // that ever reaches it. The next launch reads `clean` because this line ran, `crash` or
    // `unknown` because it never did (a crash, a kill and a power loss all leave the marker).
    SeatHubTelemetry::markCleanExit();

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
            const QList<QObject*> workers = { m_pairingSeam, m_pairing, m_liveness, m_sampler,
                                              m_horizon, m_sessionChannel, m_teardown, m_sse };
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
    delete m_sampler;  // D-11, Plan 13 Task 1
    delete m_horizon;
    delete m_sessionChannel;
    delete m_teardown;
    delete m_sse;

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
    m_sampler->moveToThread(networkThread);  // D-11, Plan 13: sampler lives on the network thread alongside liveness
    m_horizon->moveToThread(networkThread);
    m_sessionChannel->moveToThread(networkThread);
    // 06.4/ADR-0067: `SseClient::start()`'s own `openAccountStream()` call requires being on this
    // thread (its header comment).
    m_sse->moveToThread(networkThread);

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
        // No session to report against - `m_liveness` has no session attached yet either
        // (`reportFailure` no-ops with no session, the same guard `tick()` uses), so this call is
        // for completeness with the other pre-engine failure paths (RESEARCH Q1) rather than
        // because it posts anything here.
        m_liveness->reportFailure(QStringLiteral("preparing_rig"));
        raiseFailure(SeatHubFailure::local(QStringLiteral("We couldn't start this session.")));
        return;
    }

    setAttachedSession(sessionId);
    setAttachedSessionEnded(false);
    m_lastFailedStep.clear();
    m_engineLaunchReason = EngineLaunchReason::Started;
    if (!m_playSummaryPending) {
        m_stepTimings.clear();
        m_playElapsed.start();
        m_samplerPlayId = randomTraceId();
        m_playSummaryPending = true;
        m_firstFrameMs.store(-1);
        m_playGeneration.fetch_add(1);
        m_streamStartMs = -1;
        m_lastFailureClass.clear();
        m_controlPlane->setTraceId(m_samplerPlayId);
        SeatHubTelemetry::setTrace(m_samplerPlayId);
    }
    m_playConfiguration = {
        {"cfg_res", QStringLiteral("%1x%2").arg(m_settings->getValue(QStringLiteral("width")).toInt())
                                            .arg(m_settings->getValue(QStringLiteral("height")).toInt())},
        {"cfg_fps", m_settings->getValue(QStringLiteral("fps")).toInt()},
        {"cfg_bitrate_kbps", m_settings->getValue(QStringLiteral("bitrate")).toInt()}
    };
    m_clientUuid.clear();
    // A new session's teardown has not been asked for yet. Without this the second and later
    // sessions in one run never tear down (defect F-9; `teardown_guard.h`).
    m_teardownGuard.reset();

    // 06.1-19/J-07: a fresh Play or Resume starts with no termination code known yet for the
    // session it is about to attach, and no reconnect episode of its own - a code (or an ended
    // reconnect) left over from whatever this client streamed before must never leak into this
    // one's own first `handleDisplayLaunchError()`.
    m_lastTerminationCode.store(kNoTerminationCode, std::memory_order_relaxed);
    if (m_reconnecting) {
        m_reconnectTimer->stop();
        m_reconnecting = false;
        emit reconnectingChanged();
    }
    m_reconnectBackoffMs = 0;
    if (m_reconnectEnded) {
        m_reconnectEnded = false;
        emit connectFailedChanged();
    }
    if (m_reconnectStillTrying) {
        m_reconnectStillTrying = false;
        emit reconnectStillTryingChanged();
    }
    if (m_reconnectCount != 0) {
        m_reconnectCount = 0;
        emit reconnectCountChanged();
    }
    if (m_graceMinutesLeft != -1) {
        m_graceMinutesLeft = -1;
        emit graceMinutesLeftChanged();
    }

    // D-09/D-13: `hostId` is not known yet at this call - only `handleSessionState()`'s own
    // `SessionInfo` names it, once the server answers - so this starts the tag/attribute with an
    // empty host, which `handleSessionState()` fills in as soon as the session names one.
    SeatHubTelemetry::setSession(sessionId, QString());

    // D-04, D-10: the owner's live test switch. Read once per process (Task 3); every session
    // this process ever begins after the first checks nothing further.
    SeatHubTelemetry::maybeTestCrash();

    // A new session clears the previous one's end reason: the home screen shows the outcome of
    // the session that just ended, never a stale one (audit E10).
    setEndReasonText(QString());
    setHomeStatus(QString::fromLatin1(kHomeReady));

    clearFailure();
    resetConnecting();
    // D-25/T-05-45: correct any forced setting back to its fixed value before the engine reads
    // preferences for this connection (`settings_bridge.h`, RESEARCH Q4).
    m_settings->prepareForSession();
    setAppState(QString::fromLatin1(kStateConnecting));

    startNetworkThreads();

    // D-11: liveness starts here, at session begin, not at the stream's first frame - a
    // pre-engine failure is invisible to the server otherwise (`questions.md` #2). The first tick
    // reports stage `preparing_rig`. `start()` re-invokes itself on its own thread when called
    // from another one (the same pattern `stop()` and every other entry point use), so this is
    // called directly rather than marshalled through `onClientThread()`.
    m_liveness->start(sessionId);

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
    // C1: a genuinely new session id starts with no stream yet. Re-attaching the SAME id - the
    // Resume branch of `start()` calling `beginSession(m_sessionId)` - is not a change and must
    // not clear a flag `handleConnectionStarted()` already set for it.
    if (sessionId != m_sessionId) {
        m_streamStarted = false;
    }
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
    // C5: Resume is offered only for a session that reached ACTIVE on this client's side - a
    // pre-stream session is never one Home resumes, even before anything has ended it.
    const bool live = !m_sessionId.isEmpty() && !m_sessionEnded && m_streamStarted;
    if (m_liveSession == live) {
        return;
    }
    qCInfo(seathubClient) << "live session" << m_liveSession << "->" << live;
    m_liveSession = live;
    emit liveSessionChanged();
}

void SeatHubClient::setRetryBusy(bool busy)
{
    if (m_retryBusy == busy) {
        return;
    }
    m_retryBusy = busy;
    emit retryBusyChanged();
}

void SeatHubClient::endAttachedSessionBeforeStream(bool failed)
{
    // A local (no access token) attempt has no control-plane session to end.
    if (!inControlPlaneSession()) {
        return;
    }
    setAttachedSessionEnded(true);
    if (m_teardownGuard.markStarted()) {
        m_teardown->teardown(m_sessionId, m_clientUuid, failed);
    }
    // else: this session's teardown was already claimed - by `handleReadyForDeletion()` or by an
    // earlier call here for the same session - and is already running. A second failure while it
    // is in flight must not start a second one (T-06.6-50).
}

// ---------------------------------------------------------------------------
// 06.1-19/J-07: the reconnect state (D-03, screens.md §24)
// ---------------------------------------------------------------------------

bool SeatHubClient::isReconnectEligible(int code)
{
    if (code == kNoTerminationCode) {
        return false;
    }
    switch (code) {
    case 0:    // ML_ERROR_GRACEFUL_TERMINATION - a deliberate stop (a quit, or the host ending it)
    case -102: // ML_ERROR_UNEXPECTED_EARLY_TERMINATION - host-side
    case -103: // ML_ERROR_PROTECTED_CONTENT - host-side
    case -104: // ML_ERROR_FRAME_CONVERSION - host-side
        return false;
    default:
        // Every other code, including ENet's `-1` (the actual code a network blip produces) and
        // `ML_ERROR_NO_VIDEO_TRAFFIC`/`ML_ERROR_NO_VIDEO_FRAME` - the network-loss family FORK
        // research §1.2 narrows this to.
        return true;
    }
}

void SeatHubClient::enterReconnect()
{
    m_reconnecting = true;
    m_reconnectBackoffMs = kReconnectInitialBackoffMs;
    // WR-15: this is the header's own documented reset point ("Reset in `enterReconnect()`") -
    // without it, once any attempt in an earlier episode of this same session ever reached
    // `retryReconnectOrGiveUp()` once, every later episode's own first attempt would show "Still
    // trying" instead of "Connection lost. Reconnecting... (n of 5)".
    if (m_reconnectStillTrying) {
        m_reconnectStillTrying = false;
        emit reconnectStillTryingChanged();
    }
    // `setAppState()` itself would unlock Settings/the updater here (leaving "streaming"); this
    // relocks them at once, for the whole episode (T-06.1-57).
    setAppState(QString::fromLatin1(kStateConnecting));
    setReconnectLock(true);
    m_liveness->setStage(QStringLiteral("reconnecting"));
    emit reconnectingChanged();
}

void SeatHubClient::resumeAfterReconnect()
{
    // A no-op once the episode is no longer reconnecting: superseded by a give-up
    // (`handleReconnectEnded()`) or by the customer's own `endHeldSession()` while this timeout
    // was already queued.
    if (!m_reconnecting || m_sessionId.isEmpty()) {
        return;
    }
    m_clientUuid.clear();
    // D-25/T-05-45: the same correction `beginSession()` makes, before this attempt's engine
    // reads preferences.
    m_settings->prepareForSession();
    startNetworkThreads();
    // STREAM-10: the client keeps no stored pairing of its own, so this is a full, fresh
    // handshake against the same session id - the same thing `beginSession()`'s own Resume branch
    // runs, just without resetting `appState`, the stepper or `m_liveness` (none of those stopped
    // across the drop).
    const QString sessionId = m_sessionId;
    onClientThread(m_pairing, [this, sessionId]() { m_pairing->start(sessionId); });
}

void SeatHubClient::retryReconnectOrGiveUp()
{
    if (!m_reconnectStillTrying) {
        m_reconnectStillTrying = true;
        emit reconnectStillTryingChanged();
    }
    m_reconnectBackoffMs = qMin(m_reconnectBackoffMs * 2, kReconnectMaxBackoffMs);

    // No poll runs during the backoff gap between attempts (the pairing controller has already
    // failed or finished for this attempt): fetched once here so a server-side ending reached
    // while nothing was polling is still caught before the next attempt starts.
    const QString sessionId = m_sessionId;
    m_controlPlane->fetchSession(sessionId, [this, sessionId](const ControlPlaneResult& result) {
        onClientThread(this, [this, sessionId, result]() {
            if (!m_reconnecting || sessionId != m_sessionId) {
                // Superseded (ended, or reconnected already) while this read was in flight.
                return;
            }
            SessionInfo session;
            if (result.ok && SessionInfo::parse(result.body, &session)) {
                if (session.isTerminal()) {
                    handleReconnectEnded(session);
                    return;
                }
                applyReconnectSessionFields(session);
            }
            m_reconnectTimer->start(m_reconnectBackoffMs);
        });
    });
}

void SeatHubClient::handleReconnectEnded(const SessionInfo& session)
{
    m_reconnectTimer->stop();
    m_reconnecting = false;
    m_reconnectBackoffMs = 0;
    m_reconnectEnded = true;
    setReconnectLock(false);
    setAttachedSessionEnded(true);
    m_liveness->stop();
    onClientThread(m_pairing, [this]() { m_pairing->cancel(); });
    const QString reconnectClass = session.endReason == QLatin1String("GRACE_EXPIRED")
        ? QStringLiteral("grace_expired")
        : (session.endReason == QLatin1String("RECONNECT_LIMIT")
               ? QStringLiteral("reconnect_limit") : QString());
    if (!reconnectClass.isEmpty()) {
        m_lastFailedStep = QStringLiteral("reconnect");
        noteStepOutcome(m_lastFailedStep, QStringLiteral("failed"), reconnectClass,
                        m_playElapsed.isValid() ? m_playElapsed.elapsed() : -1, 1);
    }
    finishPlay(QStringLiteral("reconnect_ended"), QStringLiteral("failed"),
               QStringLiteral("reconnect"), reconnectClass);
    // CR-06: this IS an ending - the server gave up on this episode, not the customer or a locally-
    // running engine - so it must run the same D-10/STREAM-10 disable/remove/verify sequence every
    // other ending path runs (`endAttachedSessionBeforeStream()`, `endPreStreamSessionWithReport()`,
    // `handleReadyForDeletion()`'s ordinary branch). Without this, the dead Play's D-27 trace id
    // leaks onto the next request, `m_horizon` stays armed, and `m_sessionId` stays attached to a
    // session that is over.
    if (inControlPlaneSession() && m_teardownGuard.markStarted()) {
        EndReport report;
        report.attemptStep = m_lastFailedStep;
        m_teardown->teardown(m_sessionId, m_clientUuid, !report.attemptStep.isEmpty(), report);
    }
    emit reconnectingChanged();
    // The existing connecting-failure path: the stage that was active (stage 3, still active
    // throughout the whole episode) turns destructive, and `session`'s own end reason - here
    // `GRACE_EXPIRED` or `RECONNECT_LIMIT` - is the sentence under it. `reconnectEnded` (which
    // shares this call's own `connectFailedChanged`) is what tells the screen to offer only
    // `Back to home`.
    raiseConnectFailure(SeatHubFailure::generic(), session.endReason, session.minutesBilled);
}

void SeatHubClient::setReconnectLock(bool locked)
{
    m_settings->setStreamingActive(locked);
    m_updates->setStreamingActive(locked);
}

void SeatHubClient::applyReconnectSessionFields(const SessionInfo& session)
{
    if (m_reconnectCount != session.reconnectCount) {
        m_reconnectCount = session.reconnectCount;
        emit reconnectCountChanged();
    }
    const int minutesLeft = computeGraceMinutesLeft(session.graceDeadlineAt);
    if (m_graceMinutesLeft != minutesLeft) {
        m_graceMinutesLeft = minutesLeft;
        emit graceMinutesLeftChanged();
    }
}

int SeatHubClient::computeGraceMinutesLeft(const QString& graceDeadlineAt)
{
    if (graceDeadlineAt.isEmpty()) {
        return -1;
    }
    const QDateTime deadline = QDateTime::fromString(graceDeadlineAt, Qt::ISODate);
    if (!deadline.isValid()) {
        return -1;
    }
    // Whole minutes, floored, computed once against this client's own clock at the moment the
    // read that named this deadline is applied - never a client-owned countdown ticking on its
    // own (screens.md §24: "never a client timer").
    const qint64 msLeft = QDateTime::currentDateTimeUtc().msecsTo(deadline);
    if (msLeft <= 0) {
        return 0;
    }
    return static_cast<int>(msLeft / 60000);
}

void SeatHubClient::endHeldSession()
{
    if (!m_reconnecting) {
        return;
    }
    finishPlay(QStringLiteral("ended_held"), QStringLiteral("cancelled"), QStringLiteral("reconnect"));
    m_reconnectTimer->stop();
    m_reconnecting = false;
    m_reconnectBackoffMs = 0;
    setReconnectLock(false);
    emit reconnectingChanged();

    if (m_session->active()) {
        // A retry's own engine is mid-launch. Its own quit keystroke does the stopping; the
        // control-plane teardown runs from `handleReadyForDeletion()` once it does - the same
        // order `interrupt()` documents for the ordinary in-stream case - and `m_reconnecting` is
        // already false here, so that handler takes the ordinary (non-reconnect) path and posts
        // the ordinary `/end`.
        m_session->interrupt();
        return;
    }

    // No engine is running for this attempt (the backoff gap, or still re-pairing): the same
    // pre-engine sequence `interrupt()` uses, adapted for a session that already streamed once.
    onClientThread(m_pairing, [this]() { m_pairing->cancel(); });
    m_liveness->stop();
    endAttachedSessionBeforeStream(false);
    m_sessionChannel->close();
    resetConnecting();
    setHomeStatus(QString::fromLatin1(kHomeReady));
    setAppState(!m_signedIn ? QString::fromLatin1(kStateSignedOut)
                            : QString::fromLatin1(kStateHome));
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

    // Raw diagnostic text stays in the local failure object, never in the shipping tee.
    if (!failure.diagnostic.isEmpty()) {
        qCWarning(seathubClient) << "SeatHub connect failure" << coerceStep(m_lastFailedStep)
            << (isClosedFailureClass(m_lastFailedStep, m_lastFailureClass) ? m_lastFailureClass : QString());
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
        qCWarning(seathubClient) << "SeatHub engine failure" << coerceStep(m_lastFailedStep)
            << (isClosedFailureClass(m_lastFailedStep, m_lastFailureClass) ? m_lastFailureClass : QString());
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

    // D-05/C4: `retry()`'s own end-then-fresh-Play sequence is in flight (the old session is still
    // being torn down). Home's own view stays "ready" for the wait's duration - it is not
    // "connecting" or "streaming" yet - so without this guard a second Play pressed during the
    // wait would post its own `POST /api/sessions` and `beginSession()` a second session, which
    // the old session's own `handleTeardownCompleted()` (arriving after) would then tear back down
    // as if it were the one that never streamed. `retryBusy` is exactly "a fresh Play is already
    // promised"; a second one here would only race it.
    if (m_retryBusy) {
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

    // D-05/C4/C7: a session still attached here, that never streamed, is ended first - whether
    // this Play is Try again (a session that failed before it streamed) or a fresh Play from
    // Home itself (C7: the same session's earlier `/end` could not reach the server offline, so
    // the server still considers it live). Idempotent with C2's own end attempt; the wait this
    // starts is what keeps the POST below from meeting the "one nonterminal session per
    // customer" refusal (409 `USER_HAS_NONTERMINAL_SESSION`, `openapi.yaml`).
    if (endAttachedSessionBeforePlay()) {
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

bool SeatHubClient::endAttachedSessionBeforePlay()
{
    if (m_sessionId.isEmpty() || m_streamStarted) {
        return false;
    }
    setRetryBusy(true);
    if (m_teardownGuard.markStarted()) {
        m_teardown->teardown(m_sessionId, m_clientUuid, false);
    }
    // else: this session's end was already claimed - C2's own pre-stream failure path, or an
    // earlier call here for the same session - and is already running; just wait for it.
    // `handleTeardownCompleted()`/`handleTeardownFailed()` notice `m_retryBusy` and continue (or
    // stop) from there.
    return true;
}

void SeatHubClient::beginLocalAttempt()
{
    m_samplerPlayId = randomTraceId();
    m_playSummaryPending = true;
    m_stepTimings.clear();
    m_playElapsed.start();
    m_firstFrameMs.store(-1);
    m_playGeneration.fetch_add(1);
    m_streamStartMs = -1;
    m_lastFailureClass.clear();
    m_playConfiguration = {};
    SeatHubTelemetry::setTrace(m_samplerPlayId);
    clearFailure();
    setEndReasonText(QString());
    setHomeStatus(QString::fromLatin1(kHomeReady));
    resetConnecting();
    // D-25/T-05-45: same correction as `beginSession()`, before the local engine starts.
    m_settings->prepareForSession();
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

    // D-27: one trace id per Play, minted here so it covers this very request - the session
    // create - and every later request of the same Play, through to teardown
    // (`handleTeardownCompleted` clears it).
    const QString playTraceId = randomTraceId();
    m_samplerPlayId = playTraceId;
    m_playSummaryPending = true;
    m_stepTimings.clear();
    m_playElapsed.start();
    m_firstFrameMs.store(-1);
    m_playGeneration.fetch_add(1);
    m_streamStartMs = -1;
    m_lastFailureClass.clear();
    m_playConfiguration = {};
    m_controlPlane->setTraceId(playTraceId);
    SeatHubTelemetry::setTrace(playTraceId);

    // The session create goes out first - existing trace-id tests index requests relative to this
    // being the first one a Play makes - the telemetry refresh right after it (still unguarded by
    // epoch, like `requestSession()`'s own callback: applying a slightly stale handout after a
    // sign-out is harmless, D-18 F-1). No quality field is sent (A-68, D-06 reversal): the
    // customer's saved Settings are the only decider of stream quality.
    m_controlPlane->requestSession([this](const ControlPlaneResult& result) {
        onClientThread(this, [this, result]() {
            if (!result.ok) {
                applyPlayFailure(result);
                return;
            }

            const QString sessionId = result.body.value(QStringLiteral("id")).toString();
            if (sessionId.isEmpty()) {
                noteStepOutcome(QStringLiteral("allocate"), QStringLiteral("failed"),
                                QStringLiteral("bad_response"), m_playElapsed.elapsed());
                finishPlay(QStringLiteral("refused"), QStringLiteral("failed"),
                           QStringLiteral("allocate"), QStringLiteral("bad_response"));
                // A 2xx that carries no session is a contract violation, not a home state.
                setHomeStatus(QString::fromLatin1(kHomeReady));
                raiseFailure(SeatHubFailure::generic());
                return;
            }

            setHomeStatus(QString::fromLatin1(kHomeReady));
            beginSession(sessionId);
            noteStepOutcome(QStringLiteral("allocate"), QStringLiteral("ok"), {}, m_playElapsed.elapsed());
        });
    });

    // Plan 15 (D-01, D-18 SV-C3): one of the three moments SeatHub refreshes its DSN handout.
    m_controlPlane->fetchTelemetry([this](const ControlPlaneResult& result) {
        onClientThread(this, [this, result]() { applyTelemetryResult(result); });
    });
}

void SeatHubClient::applyPlayFailure(const ControlPlaneResult& result)
{
    QString allocationClass;
    if (result.failure == QLatin1String("NO_HOST_AVAILABLE")) {
        allocationClass = QStringLiteral("no_host");
    }
    else if (result.statusCode == 402) {
        allocationClass = QStringLiteral("refused_balance");
    }
    else if (result.statusCode == 409) {
        allocationClass = QStringLiteral("refused_state");
    }
    else if (result.statusCode == 0) {
        allocationClass = QStringLiteral("unreachable");
    }
    else if (result.statusCode >= 500 && result.statusCode <= 599) {
        allocationClass = QStringLiteral("server_error");
    }
    else {
        allocationClass = QStringLiteral("bad_response");
    }
    m_lastFailedStep = QStringLiteral("allocate");
    noteStepOutcome(m_lastFailedStep, QStringLiteral("failed"), allocationClass,
                    m_playElapsed.isValid() ? m_playElapsed.elapsed() : -1, 1);
    finishPlay(QStringLiteral("refused"), QStringLiteral("failed"), m_lastFailedStep, allocationClass);

    // WR-01: a refused Play (a balance floor, a duplicate session, or an unreachable control
    // plane) is a Play end too - the trace id `beginPlayRequest()` minted must not leak onto the
    // customer's next request (another Play, a wallet poll, or another customer's sign-in on a
    // shared PC). Before this fix only `handleTeardownCompleted()` ever cleared it, so every
    // refused Play left its id in place.
    m_controlPlane->clearTraceId();
    SeatHubTelemetry::clearTrace();

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

    // D-05/C4/C7: Try again is always a fresh Play, never a Resume of a session that did not
    // stream - `start()` itself now ends a session left attached here first
    // (`endAttachedSessionBeforePlay()`), the same one sequence C4's second half asks Play from
    // Home to run too.
    start();
}

void SeatHubClient::interrupt()
{
    // D-05/C3: before the engine runs - the rig being prepared, or pairing -
    // `SessionLifecycle::interrupt()` is a no-op: there is no active engine to push the quit key
    // to (`session_lifecycle.cpp:131-140`). Without this branch a Cancel press here did nothing
    // until an engine eventually attached, which is exactly the "stuck behind a dead session"
    // shape T-06.6-49 exists to close. Cancel is not a failure, so this never raises one: it
    // cancels pairing, stops liveness, ends the session through C2's one path with `failed:
    // false` (C6: no charge either way, the server's call to make), and returns Home at once -
    // not once the network answers (`screens.md` §24).
    if (!m_session->active()) {
        finishPlay(QStringLiteral("cancelled"), QStringLiteral("cancelled"), m_lastFailedStep);
        onClientThread(m_pairing, [this]() { m_pairing->cancel(); });
        m_liveness->stop();
        endAttachedSessionBeforeStream(false);
        m_sessionChannel->close();
        resetConnecting();
        setHomeStatus(QString::fromLatin1(kHomeReady));
        setAppState(!m_signedIn ? QString::fromLatin1(kStateSignedOut)
                                : QString::fromLatin1(kStateHome));
        return;
    }

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

    // 06.4/ADR-0067, D-01: the account push channel opens the moment there is a token to open it
    // with - the same one `applyRestoreResult()` opens for a confirmed restore. Marshalled: this
    // runs on the facade thread, `m_sse` lives on the network thread.
    onClientThread(m_sse, [this]() { m_sse->start(); });

    // 06.4/ADR-0067, D-07: a successful sign-in stops the notice timer and clears any signed-out
    // notice left over from the credential this one replaces.
    clearSignedOutNotice();

    // WR-06 (code review 06.3-REVIEW-fork.md): a fresh sign-in used to read the account's real id
    // only when the outbox already held a file (`hasQueuedReports()`), so a customer who signed in
    // for the first time and played straight away had `m_accountId` still empty at their own
    // teardown - and any `Retryable`/`AuthFailed` outcome for that very first session was then
    // dropped, "has no signed-in account to tag it with", which undid the outbox for exactly the
    // sessions it matters most for (a new customer's first one). `fetchMe` now always runs
    // (guarded by `epoch` the same way it already was), whether or not the outbox has anything
    // queued yet - it also fills `m_accountId` before this session's own teardown can need it, not
    // only before an earlier process's leftover report can be drained. If `fetchMe` itself fails,
    // or simply has not answered yet by the time a report needs a tag, that report is held rather
    // than dropped (`m_untaggedQualityReports`) and is tagged the moment `m_accountId` is next
    // filled, this epoch - by this same callback, or by a later `openProfile()`/restore.
    m_qualityOutbox.setDirectory(qualityOutboxDirectory());
    const quint64 epoch = m_authEpoch;
    m_controlPlane->fetchMe([this, epoch](const ControlPlaneResult& result) {
        onClientThread(this, [this, epoch, result]() {
            if (epoch != m_authEpoch) {
                return;
            }
            AccountInfo account;
            if (result.ok && AccountInfo::parse(result.body, &account)) {
                m_accountId = account.id;
                SeatHubTelemetry::setUser(account.id);
                flushUntaggedQualityReports();
                drainQualityOutbox();
            }
        });
    });

    // Plan 15 (D-01, D-18 SV-C3): one of the three moments SeatHub refreshes its DSN handout -
    // right after the interactive sign-in's own `fetchMe`, epoch-guarded the same way.
    m_controlPlane->fetchTelemetry([this, epoch](const ControlPlaneResult& result) {
        onClientThread(this, [this, epoch, result]() {
            if (epoch != m_authEpoch) {
                return;
            }
            applyTelemetryResult(result);
        });
    });

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
    finishPlay(QStringLiteral("signed_out"), QStringLiteral("cancelled"));
    // CR-02: a report still pending when the customer signs out (mid-teardown, or between
    // `handleVideoStatsParsed()` and a teardown outcome) is written to the outbox under the
    // account that is about to be cleared below - never dropped just because nobody is waiting
    // for it any more, and never left to be posted after `m_accountId` and the access token are
    // gone.
    storeQualityReportToOutbox();

    // WR-06: an untagged report (parsed before `m_accountId` was ever resolved this epoch) can
    // never be tagged now - the epoch it was held for is about to be over, and dropping it here,
    // once, is the one legitimate drop this outbox has left (see `flushUntaggedQualityReports()`'s
    // own header comment).
    m_untaggedQualityReports.clear();

    // CR-03: whatever `drainQualityOutbox()` call is in flight for this account must not still be
    // running by the time a different account signs in - see `QualityOutbox::cancel()`'s own
    // comment for what would otherwise happen to the new account's own drain.
    m_qualityOutbox.cancel();

    // WR-01: whatever Play this covered is over.
    m_controlPlane->clearTraceId();
    SeatHubTelemetry::clearTrace();
    // D-09, D-18 SV-C4: no session or account survives a sign-out into the next crash's tags.
    SeatHubTelemetry::clearSession();
    // Plan 21 (D-14, Pitfall 9): every line captured before this point ships - or lands back in
    // the spool - under THIS account before it is cleared below. Without this, a line from the
    // last instant of this session could still be sitting in the in-memory queue when
    // `clearUser()` runs, and ship moments later carrying no account id (or the next one).
    LogShipper::instance().drainBeforeSignOut();
    SeatHubTelemetry::clearUser();

    // Signing out leaves no stored pairing and no usable credential: not on disk, not in memory
    // here, and not valid on the server (ADR-0050, D-06). The channel and both timers stop before
    // the store is swept.
    m_sessionChannel->close();
    m_liveness->stop();
    m_horizon->disarm();
    // 06.4/ADR-0067: the account push channel closes with everything else, before the credential
    // it was opened with is cleared below.
    onClientThread(m_sse, [this]() { m_sse->stop(); });
    // 06.4/ADR-0067, D-07: a manual sign-out inside the notice's own wait stops the timer and
    // clears it, so no second sign-out follows once it would otherwise have elapsed.
    clearSignedOutNotice();
    onClientThread(m_pairing, [this]() { m_pairing->cancel(); });
    m_teardown->cancel();
    // D-05/C4: `TeardownController::cancel()` emits neither `teardownCompleted` nor
    // `teardownFailed`, so a `retry()` wait abandoned by a sign-out would otherwise never clear -
    // leaving the NEXT, unrelated session's ordinary teardown to find the flag still set and fire
    // an unrequested `beginPlayRequest()` for a customer who is no longer signed in.
    setRetryBusy(false);
    // WR-14: signing out mid-episode (e.g. during the backoff gap) stops and clears every other
    // piece of reconnect state above, but left `m_reconnectTimer`, `m_reconnecting` and the
    // Settings/updater lock stuck exactly as `enterReconnect()` set them - nobody signed in to
    // notice, until the next `beginSession()`'s own lock cycle happened to touch it.
    if (m_reconnecting) {
        m_reconnectTimer->stop();
        m_reconnecting = false;
        setReconnectLock(false);
        emit reconnectingChanged();
    }

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
    // D-17/Plan 30: the next sign-in's reports are never tagged with a previous customer's id on
    // a shared PC.
    m_accountId.clear();
    emit identityChanged();
    resetBalance();
    // 06.4/ADR-0067: the next customer's first account.state push must be compared against
    // nothing, never against a previous sign-in's notice.
    m_lastOpenTopupNoticeId.clear();
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

            // 06.4/ADR-0067, D-01: opened the moment a restore is confirmed - the same channel
            // `adoptSignIn()` opens for an interactive sign-in.
            onClientThread(m_sse, [this]() { m_sse->start(); });

            // Plan 15 (D-01, D-18 SV-C3): one of the three moments SeatHub refreshes its DSN
            // handout - right after restore's own `fetchMe` is confirmed. Guarded against the
            // epoch THIS restore just established above (not the pre-restore value the `fetchMe`
            // call at `restoreSession()` captured) - `applyRestoreResult()` bumps `m_authEpoch`
            // itself on every branch, so guarding against the pre-bump value would race that same
            // bump and drop this read whenever `fetchMe`'s own reply happens to land first.
            const quint64 telemetryEpoch = m_authEpoch;
            m_controlPlane->fetchTelemetry([this, telemetryEpoch](const ControlPlaneResult& telemetryResult) {
                onClientThread(this, [this, telemetryEpoch, telemetryResult]() {
                    if (telemetryEpoch != m_authEpoch) {
                        return;
                    }
                    applyTelemetryResult(telemetryResult);
                });
            });
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
        // D-17/Plan 30: no account is signed in past this point; a stale id from a process that
        // never signed out cleanly must not tag a later report.
        m_accountId.clear();
        // D-18 SV-C4: a crash after this point carries no account id.
        SeatHubTelemetry::clearUser();
        // WR-06: an untagged report held for this (now-refused) epoch can never be tagged now.
        m_untaggedQualityReports.clear();
        // CR-03: a `drainQualityOutbox()` call this credential started must not still be running
        // once it has been refused - see `QualityOutbox::cancel()`'s own comment.
        m_qualityOutbox.cancel();
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

    // D-17/Plan 30: the real, stable tag a stored quality report is written and checked against -
    // `m_identity` above is the typed phone/email/username and can differ across two sign-ins of
    // the same account, which would tag two reports for one customer as "different accounts".
    m_accountId = account.id;
    SeatHubTelemetry::setUser(account.id);
    // WR-06: any report parsed before this account id was known (this session's own, or an
    // earlier one this process never got a chance to tag) is tagged and stored now.
    flushUntaggedQualityReports();
    drainQualityOutbox();
}

QString SeatHubClient::qualityOutboxDirectory() const
{
    return QDir(m_tokenStore->directory()).filePath(QStringLiteral("quality-outbox"));
}

void SeatHubClient::drainQualityOutbox()
{
    if (m_accountId.isEmpty() || !m_controlPlane->hasAccessToken()) {
        return;
    }

    m_qualityOutbox.setDirectory(qualityOutboxDirectory());

    // D-35: the real bearer token never leaves `ControlPlaneClient`. `m_authEpoch` already
    // uniquely names "which sign-in/restore is current" (bumped by every sign-in, restore result
    // and sign-out), so its string form is the opaque `tokenGeneration` `QualityOutbox::drain()`
    // needs to tell "the same refused token" from "a new one" - without ever holding the secret
    // itself.
    const QString tokenGeneration = QString::number(m_authEpoch);

    // CR-03: captured once, here, and checked again both before issuing each file's send and
    // again in its reply continuation - `drain()` walks its files as a chain of callbacks that can
    // span several event-loop turns, and a sign-out (or a sign-in as a different account) can land
    // in the middle of that chain. Before this fix the loop kept going under this (now stale)
    // account id and whatever bearer token `ControlPlaneClient` currently held - which, after a
    // different customer signed in, was THEIR token - and a 404 from the server then deleted that
    // file for good (T-06.3-51).
    const quint64 epoch = m_authEpoch;

    m_qualityOutbox.drain(
        tokenGeneration, m_accountId,
        [this, epoch](const QString& sessionId, const QJsonObject& report,
               ControlPlaneClient::Callback callback) {
            if (epoch != m_authEpoch) {
                // Signed out, or signed in as someone else, since this drain started: never issue
                // the send at all under a credential that is no longer this account's.
                callback(ControlPlaneResult::aborted());
                return;
            }
            m_controlPlane->postSessionQuality(
                sessionId, report, [this, epoch, callback](const ControlPlaneResult& result) {
                    // `postSessionQuality`'s callback runs on the control-plane's own (network)
                    // thread; `QualityOutbox`'s own state (`m_draining`,
                    // `m_blockedTokenGeneration`) is only ever touched from this facade's thread
                    // (`drain()` is called from here, on `setAccount()`), so the continuation
                    // `callback` runs is marshalled back before it touches any of it. The epoch is
                    // checked again here too: the account can have changed while this particular
                    // request was in flight.
                    onClientThread(this, [this, epoch, callback, result]() {
                        callback(epoch == m_authEpoch ? result : ControlPlaneResult::aborted());
                    });
                });
        });
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
    // (D-51), so nothing of it is read beyond the fact that it began - `engine_stage` is diagnostic
    // data for the server (D-11), never rendered here.
    m_liveness->setEngineStage(stage);
    advanceConnectStage(kStageStream);
}

void SeatHubClient::handleStageFailed(const QString& stage, int errorCode, const QString& failingPorts)
{
    if (m_reconnecting) {
        // A retry's own engine failed inside its second stage - the same class of failure as a
        // pairing failure during reconnect (below): schedule the next attempt, and leave the
        // liveness stage exactly where it already is (`reconnecting`) rather than reporting
        // `failed` for an attempt, not the session.
        m_lastFailedStep = QStringLiteral("reconnect");
        noteStepOutcome(m_lastFailedStep, QStringLiteral("failed"),
                        QStringLiteral("attempt_engine_failed"),
                        m_playElapsed.isValid() ? m_playElapsed.elapsed() : -1, 1);
        retryReconnectOrGiveUp();
        return;
    }

    // D-11: the server sees exactly what the customer's screen is about to say, before liveness
    // stops - the engine's own stage, its code and the ports it named, whether this failure is
    // pre-stream or (rarely) mid-stream.
    m_liveness->reportFailure(stage, errorCode, failingPorts);

    if (m_appState == QLatin1String(kStateStreaming)) {
        m_lastFailedStep = QStringLiteral("stream");
        noteStepOutcome(m_lastFailedStep, QStringLiteral("failed"),
                        classForStreamError(errorCode),
                        m_playElapsed.isValid() ? m_playElapsed.elapsed() : -1, 1);
        // Mid-stream failures keep the session's own end-reason copy; the engine's stage
        // failure is diagnostic detail from here on.
        raiseFailure(mapLaunchError(stage));
        return;
    }
    const QString stageClass = classForEngineStageName(stage);
    m_lastFailedStep = QStringLiteral("engine_connect");
    noteStepOutcome(m_lastFailedStep, QStringLiteral("failed"), stageClass,
                    m_playElapsed.isValid() ? m_playElapsed.elapsed() : -1, 1);
    finishPlay(QStringLiteral("stage_failed"), QStringLiteral("failed"), m_lastFailedStep, stageClass);
    // Before the stream started the customer is on the connecting view: it stopped there, at the stage
    // the engine was in, and says so in the deck's words rather than sending them to the error view.
    if (connectingSession()) {
        raiseConnectFailure(mapStageFailure(stage, errorCode, failingPorts));
        // 06.1's ADR item 4 / J-22: the server needs what the engine just told us, now - not only
        // at `handleReadyForDeletion()`'s later, report-less teardown - so it can attribute blame.
        // `report.stage` is `connecting` (D-11: this is a stage failure inside stage 2, never
        // `pairing`, which already succeeded for the engine to have started at all); `engineStage`
        // is moonlight-common-c's own stage name, exactly as `m_liveness->reportFailure()` above
        // just reported it - `hasEngineError` is set unconditionally the same way, because this
        // 3-argument overload is only ever called with a real platform code.
        if (inControlPlaneSession()) {
            setAttachedSessionEnded(true);
            EndReport report;
            report.stage = QStringLiteral("connecting");
            report.engineStage = stage;
            report.hasEngineError = true;
            report.engineError = errorCode;
            report.failingPorts = failingPorts;
            report.attemptStep = m_lastFailedStep;
            endPreStreamSessionWithReport(m_teardownGuard, m_teardown, m_sessionId, m_clientUuid,
                                          report);
        }
        return;
    }
    raiseFailure(mapStageFailure(stage, errorCode, failingPorts));
}

void SeatHubClient::handleConnectionStarted()
{
    const bool firstConnection = m_streamStartMs < 0;
    if (firstConnection) m_streamStartMs = m_playElapsed.elapsed();
    const auto generation = m_playGeneration.load();
    const QElapsedTimer clock = m_playElapsed;
    m_hud.compositor().setStatsTap([this, generation, clock](const VideoStats& stats) {
        if (generation != m_playGeneration.load()) return;
        m_sampler->feed(stats);
        qint64 absent = -1;
        if (stats.renderedFps.present && stats.renderedFps.value > 0
            && m_firstFrameMs.compare_exchange_strong(absent, clock.elapsed())) {
            const double elapsed = static_cast<double>(m_firstFrameMs.load());
            QMetaObject::invokeMethod(m_sampler, [elapsed]() {
                SeatHubTelemetry::emitStepMetric(QStringLiteral("first_frame"), QStringLiteral("ok"),
                                                QStringLiteral("ok"), elapsed);
            }, Qt::QueuedConnection);
        }
    });
    // 06.1-19/J-07: the reconnect (if this was one) just succeeded - the stream is back before
    // anything else here runs.
    if (m_reconnecting) {
        noteStepOutcome(QStringLiteral("reconnect"), QStringLiteral("ok"), QStringLiteral("ok"),
                        m_playElapsed.isValid() ? m_playElapsed.elapsed() : -1,
                        qMax(1, m_reconnectCount));
        m_reconnecting = false;
        m_reconnectBackoffMs = 0;
        m_lastFailedStep.clear();
        emit reconnectingChanged();
    }

    // D-14: from here on the settings page can report what the session actually settled on,
    // rather than what was asked for.
    m_settings->noteConnectionStarted();
    advanceConnectStage(kStageStreamingNow);
    setAppState(QString::fromLatin1(kStateStreaming));

    // C1 (D-05): the session reached ACTIVE on this client's side - from here Home may offer
    // Resume (C5), and Try again must never treat this session as one to end and replace (C4).
    m_streamStarted = true;
    updateLiveSession();

    // The HUD is begun before liveness starts, because the reads liveness makes feed it:
    // `beginSession()` resets Time left's own state (D-11), so a wallet read that beat it would
    // be wiped. This fires before the engine creates its SDL window (D-01), so the first publish
    // may arrive before the renderer has registered; the 1 Hz heartbeat re-publishes and the
    // stream picks Time left up on its first eligible read.
    m_hud.beginSession();

    // WR-04: the moment the stream truly begins is the right anchor for the first decoder
    // segment's own duration - not `beginSession()`, which can run well before the first frame
    // while pairing and connecting happen and would otherwise overstate it.
    if (firstConnection) m_statsAggregator.start();

    // The last balance read before the stream (D-16: the seed never shows Time left, however low
    // it is - it can be old, so it arms no reminder; the report below reads the wallet at once and
    // that read is the first thing that can).
    if (m_balanceMinutes >= 0) {
        m_hud.seedCreditMinutes(m_balanceMinutes);
    }

    // D-11, Plan 13 Task 1: the sampler's 60s roll-up window starts when the stream truly begins
    // (same anchor as `m_statsAggregator.start()` above). The sampler already lives on the
    // network thread; `start()` re-invokes itself queued when called from another thread.
    m_sampler->start(m_samplerPlayId);

    // D-31/D-34/D-11: liveness itself started at `beginSession()` (session begin, not the stream's
    // first frame - RESEARCH Q1); from here it keeps reporting every 10 s, now with `state`
    // "streaming" (ADR-0041's meaning is unchanged, only the moment `start()` itself runs has
    // moved) and stage `streaming`, and it keeps reading the wallet on the same tick (CUST-15).
    if (inControlPlaneSession()) {
        m_liveness->setReportedState(QStringLiteral("streaming"));
        m_liveness->setStage(QStringLiteral("streaming"));
    }
}

void SeatHubClient::handleDisplayLaunchError(const QString& text)
{
    // 06.1-19/J-07: this reconnect attempt's own engine failed before it reached streaming again
    // (or dropped again immediately after) - schedule the next attempt rather than ending a
    // session J-07 exists to keep held. Checked before `connectingSession()`: `appState` stays
    // "connecting" throughout a reconnect (no new appState value - `main.qml`'s
    // `componentForState`/`showsHeader` switches are not this plan's to edit), so that check
    // alone cannot tell a reconnect's own attempt apart from an ordinary Play.
    if (m_reconnecting) {
        m_lastFailedStep = QStringLiteral("reconnect");
        noteStepOutcome(m_lastFailedStep, QStringLiteral("failed"),
                        QStringLiteral("attempt_engine_failed"),
                        m_playElapsed.isValid() ? m_playElapsed.elapsed() : -1, 1);
        retryReconnectOrGiveUp();
        return;
    }

    // Never shown verbatim (T-03-05). `mapLaunchError` keeps `text` as diagnostic only.
    if (connectingSession()) {
        m_lastFailedStep = QStringLiteral("engine_connect");
        noteStepOutcome(m_lastFailedStep, QStringLiteral("failed"),
                        QStringLiteral("launch_error"),
                        m_playElapsed.isValid() ? m_playElapsed.elapsed() : -1, 1);
        finishPlay(QStringLiteral("launch_failed"), QStringLiteral("failed"),
                   m_lastFailedStep, QStringLiteral("launch_error"));
        if (inControlPlaneSession()) {
            EndReport report;
            report.stage = QStringLiteral("connecting");
            report.attemptStep = m_lastFailedStep;
            endPreStreamSessionWithReport(m_teardownGuard, m_teardown, m_sessionId,
                                          m_clientUuid, report);
        }
        raiseConnectFailure(mapLaunchError(text));
        return;
    }

    // 06.1-19/J-07: a stream that had started ended on a connection-class engine code - not a
    // deliberate stop and not a host-side fault (FORK research §1.2) - while the server still
    // holds the session. SeatHub keeps it and relaunches, instead of tearing down below.
    const int terminationCode = m_lastTerminationCode.load(std::memory_order_relaxed);
    if (m_streamStarted) {
        m_lastFailedStep = QStringLiteral("stream");
        noteStepOutcome(m_lastFailedStep, QStringLiteral("failed"),
                        classForStreamError(terminationCode),
                        m_playElapsed.isValid() ? m_playElapsed.elapsed() : -1, 1);
    }
    if (m_streamStarted && inControlPlaneSession()
            && isReconnectEligible(terminationCode)) {
        enterReconnect();
        return;
    }

    finishPlay(QStringLiteral("terminated"), QStringLiteral("failed"),
               QStringLiteral("stream"), classForStreamError(terminationCode));
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
    m_hud.compositor().setStatsTap(nullptr);
    m_sampler->finish();
    // D-56: the duration timer stops here, which is the interval the plan specifies.
    m_hud.endSession();

    // 06.1-19/J-07: upstream runs its own port test on every session end, including a connection
    // drop this reconnect is about to keep - `raiseFailure()` here would flip `appState` to
    // "error" out from under `enterReconnect()`'s own "connecting" (T-06.1-57). No fake in this
    // suite emits this signal, so no test slot depends on the ordinary path running while
    // reconnecting.
    if (!m_reconnecting && portTestResult != 0 && portTestResult != -1 && m_failure.isEmpty()) {
        raiseFailure(mapPortTestFailure(portTestResult));
    }
}

void SeatHubClient::handleReadyForDeletion()
{
    if (m_reconnecting) {
        // 06.1-19/J-07: the engine that just tore itself down was this reconnect's own attempt
        // (the drop that started it, or a retry's own engine dropping again). This is not an
        // ending: `m_liveness` keeps running (it never stopped across the drop - the server must
        // still see `reconnecting` and hold the grace) and the session's own teardown never runs.
        // Only the local engine object goes away, and the next attempt starts after the backoff.
        releaseEngineSession();
        m_settings->noteSessionFinished();
        m_reconnectTimer->start(m_reconnectBackoffMs);
        return;
    }

    finishPlay(m_streamStarted ? QStringLiteral("ended") : QStringLiteral("cancelled"),
               m_streamStarted ? QStringLiteral("ok") : QStringLiteral("cancelled"),
               m_streamStarted ? QStringLiteral("stream") : m_lastFailedStep,
               m_streamStarted ? QStringLiteral("graceful") : QString());
    // SDL destruction is proven by the time this arrives (D-03), so the Qt window may come
    // back and the appState may leave "streaming". SessionSegue.qml performs the actual
    // `window.visible = true`.
    if (m_appState == QLatin1String(kStateStreaming)) {
        setAppState(!m_signedIn ? QString::fromLatin1(kStateSignedOut)
                                : QString::fromLatin1(kStateHome));
    }

    // 06.4/ADR-0067, D-07/D-33: a signed-out notice deferred while this stream was running (a
    // revoked stream, or the fallback read's own 401) now shows on whatever screen the stream's
    // end just landed on. `startSignedOutSequence()`'s own running-stream check is false here -
    // the engine has already finished - so this runs the ordinary notice-then-sign-out sequence.
    if (m_pendingSignedOutNotice) {
        m_pendingSignedOutNotice = false;
        startSignedOutSequence();
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
        EndReport report;
        report.attemptStep = m_lastFailedStep;
        m_teardown->teardown(m_sessionId, m_clientUuid, !report.attemptStep.isEmpty(), report);
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

    // 06.1-19/J-07: the reconnect screen's own count and grace countdown, cached from whichever
    // read reaches the facade first - the pairing poll's own `sessionRead` while an attempt is
    // actively re-pairing, or `retryReconnectOrGiveUp()`'s explicit fetch during the backoff gap
    // between attempts (no poll runs there).
    if (m_reconnecting) {
        applyReconnectSessionFields(session);
    }

    // D-09/D-13: `beginSession()` does not know the rig yet - this is the first (and only) place a
    // `SessionInfo` names one, so this is where the crash tags/attributes actually gain `host_id`.
    if (!session.hostId.isEmpty()) {
        SeatHubTelemetry::setSession(m_sessionId, session.hostId);
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

    if (session.state == QLatin1String("REQUESTED") || session.state == QLatin1String("ALLOCATED")
        || session.state == QLatin1String("PREPARING")) {
        m_lastFailedStep = QStringLiteral("rig_wait");
    }

    if (session.isTerminal()) {
        const bool wasAlreadyEnded = m_sessionEnded;
        // The server considers this session over - the wallet ran out, the owner's reservation
        // arrived, or an operator ended it. Home no longer offers to resume it, even while its
        // teardown is still running. Stop locally; teardown follows from `handleReadyForDeletion()`.
        if (session.id.isEmpty() || session.id == m_sessionId) {
            setAttachedSessionEnded(true);
        }
        m_liveness->stop();
        if (m_session->active() || m_appState == QLatin1String(kStateStreaming)) {
            finishPlay(QStringLiteral("server_ended"), QStringLiteral("ok"), QStringLiteral("stream"));
            // 06.1-19/J-07: if this engine belongs to a reconnect attempt that just resumed
            // streaming, this same terminal read still wins - clear `m_reconnecting` first, so
            // `handleReadyForDeletion()`, once this interrupt reaches it, takes the ordinary end
            // path instead of scheduling another attempt for a session that is already over.
            // WR-14: only when a retry's engine is still genuinely `m_reconnecting` - the ordinary
            // already-`"streaming"` sub-case (`m_reconnecting` already false here) is unlocked by
            // `setAppState()`'s own toggle once `handleReadyForDeletion()` leaves "streaming", and
            // releasing the Settings/updater lock again here would double-unlock it.
            if (m_reconnecting) {
                m_reconnecting = false;
                setReconnectLock(false);
                emit reconnectingChanged();
            }
            // The engine is running: it stops it, and its own end path carries the customer home.
            m_session->interrupt();
        }
        else if (m_reconnecting) {
            handleReconnectEnded(session);
        }
        else if (connectingSession()) {
            // No engine has started, so there is nothing to interrupt and the customer would sit on a
            // stepper that looks alive for a session that is over. Connecting stops here, at the stage
            // it had reached, in the server's own words for why.
            onClientThread(m_pairing, [this]() { m_pairing->cancel(); });
            if (!wasAlreadyEnded && inControlPlaneSession()) {
                m_lastFailedStep = QStringLiteral("rig_wait");
                noteStepOutcome(m_lastFailedStep, QStringLiteral("failed"),
                                QStringLiteral("server_ended"), m_playElapsed.elapsed(), 0,
                                session.endReason);
                finishPlay(QStringLiteral("server_ended_pre"), QStringLiteral("failed"),
                           m_lastFailedStep, QStringLiteral("server_ended"));
                EndReport report;
                report.stage = QStringLiteral("connecting");
                report.attemptStep = m_lastFailedStep;
                endPreStreamSessionWithReport(m_teardownGuard, m_teardown, m_sessionId,
                                              m_clientUuid, report);
            }
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

void SeatHubClient::noteStepOutcome(const QString& step, const QString& outcome,
                                   const QString& failureClass, qint64 elapsedMs, int attempt,
                                   const QString& endReason)
{
    const QString closedStep = coerceStep(step);
    if (closedStep == QLatin1String("other")
        || (outcome != QLatin1String("ok") && outcome != QLatin1String("failed")
            && outcome != QLatin1String("cancelled"))
        || !isClosedFailureClass(closedStep, failureClass)) {
        return;
    }

    QVariantMap timing;
    timing.insert(QStringLiteral("step"), closedStep);
    const qint64 nowMs = m_playElapsed.isValid() ? m_playElapsed.elapsed() : 0;
    timing.insert(QStringLiteral("start_ms"),
                  elapsedMs >= 0 ? qMax<qint64>(0, nowMs - elapsedMs) : nowMs);
    if (elapsedMs >= 0) {
        timing.insert(QStringLiteral("elapsed_ms"), elapsedMs);
    }
    m_stepTimings.insert(closedStep, timing);
    if (outcome == QLatin1String("failed")) m_lastFailureClass = failureClass;

    const QString closedClass = failureClass;
    SeatHubTelemetry::emitStepMetric(closedStep, outcome, closedClass,
                                     elapsedMs >= 0 ? static_cast<double>(elapsedMs) : 0.0);

    const bool emitLine = outcome == QLatin1String("failed")
        || outcome == QLatin1String("cancelled")
        || (outcome == QLatin1String("ok") && attempt == 2 && !closedClass.isEmpty());
    if (!emitLine) {
        return;
    }

    QJsonObject attributes;
    attributes.insert(QStringLiteral("step"), closedStep);
    attributes.insert(QStringLiteral("outcome"), outcome);
    if (!closedClass.isEmpty()) {
        attributes.insert(QStringLiteral("failure_class"), closedClass);
    }
    if (elapsedMs >= 0) {
        attributes.insert(QStringLiteral("elapsed_ms"), static_cast<double>(elapsedMs));
    }
    if (attempt > 0) {
        attributes.insert(QStringLiteral("attempt"), attempt);
    }
    if (isClosedEndReason(endReason)) {
        attributes.insert(QStringLiteral("end_reason"), endReason);
    }
    SeatHubTelemetry::emitDiagnostic(QStringLiteral("play.step"),
                                     outcome == QLatin1String("failed") ? LogLevel::Warning
                                                                         : LogLevel::Info,
                                     attributes);
}

void SeatHubClient::finishPlay(const QString& exitPath, const QString& outcome,
                              const QString& lastStep, const QString& failureClass)
{
    if (!m_playSummaryPending) return;
    m_playSummaryPending = false;
    // The renderer only feeds/captures the first-frame timestamp. Final aggregation, shipping
    // and the bounded SDK flush never run on that hot thread.
    m_hud.compositor().setStatsTap(nullptr);
    QJsonObject attributes = m_playConfiguration;
    attributes.insert("exit_path", exitPath);
    attributes.insert("outcome", outcome);
    QString reached = lastStep.isEmpty()
        ? (m_lastFailedStep.isEmpty() && m_streamStartMs >= 0 ? QStringLiteral("stream") : m_lastFailedStep)
        : lastStep;
    QString classified = failureClass.isEmpty() ? m_lastFailureClass : failureClass;
    const qint64 firstFrame = m_firstFrameMs.load();
    if (m_streamStartMs >= 0 && firstFrame < 0) {
        reached = QStringLiteral("first_frame");
        classified = QStringLiteral("no_first_frame");
    }
    if (coerceStep(reached) != QLatin1String("other")) attributes.insert("last_step", reached);
    if (!classified.isEmpty() && isClosedFailureClass(reached, classified)) {
        attributes.insert("failure_class", classified);
    }
    for (auto it = m_stepTimings.cbegin(); it != m_stepTimings.cend(); ++it) {
        if (it.value().contains(QStringLiteral("elapsed_ms"))) {
            attributes.insert(QStringLiteral("t_%1_ms").arg(it.key()),
                              QJsonValue::fromVariant(it.value().value(QStringLiteral("elapsed_ms"))));
        }
    }
    if (firstFrame >= 0) attributes.insert("t_first_frame_ms", static_cast<double>(firstFrame));
    auto finishSampler = [this, &attributes]() {
        m_sampler->finish();
        if (m_streamStartMs < 0) return;
        attributes.insert("rollups", m_sampler->rollups());
        attributes.insert("bad_episodes", m_sampler->badEpisodes());
        attributes.insert("bad_seconds", m_sampler->badSeconds());
        attributes.insert("sampling_gap_s", m_sampler->samplingGapS());
    };
    if (m_sampler->thread() == QThread::currentThread()) finishSampler();
    else QMetaObject::invokeMethod(m_sampler, finishSampler, Qt::BlockingQueuedConnection);
    if (m_streamStartMs >= 0) {
        attributes.insert("stream_s", (m_playElapsed.elapsed() - m_streamStartMs) / 1000.0);
        attributes.insert("reconnects", m_reconnectCount);
        if (m_statsAggregator.hasSegments()) {
            const auto quality = m_statsAggregator.aggregate();
            struct Field { const char* name; OptionalMetric VideoStats::* member; };
            const Field fields[] = {
                {"fps_avg", &VideoStats::renderedFps}, {"net_drop_avg", &VideoStats::networkDroppedFramePct},
                {"jitter_drop_avg", &VideoStats::jitterDroppedFramePct}, {"rtt_avg", &VideoStats::rttMs},
                {"decode_avg", &VideoStats::decodeTimeMs}, {"queue_avg", &VideoStats::queueTimeMs},
                {"render_avg", &VideoStats::renderTimeMs}, {"host_avg", &VideoStats::hostProcessingAvgMs}
            };
            for (const auto& field : fields) {
                const auto value = quality.*field.member;
                if (value.present) attributes.insert(QString::fromLatin1(field.name), value.value);
            }
        }
    }
    SeatHubTelemetry::emitDiagnostic(QStringLiteral("play.summary"),
                                     outcome == QLatin1String("failed") ? LogLevel::Warning : LogLevel::Info,
                                     attributes);
    LogShipper::instance().drainBeforeSignOut();
    SeatHubTelemetry::flush(2000);
}

void SeatHubClient::handleAccountState(const AccountStateInfo& account)
{
    // 06.4/ADR-0067, D-01: the same properties, the same signal, a successful refreshBalance()
    // answer sets - Home and Profile, which had no refresh of their own (RESEARCH-FORK.md), go
    // live the moment a push frame carries a new figure. Not this account's push (signed out, or
    // signed in as someone else, since the channel this frame arrived on was opened) is not shown.
    if (m_signedIn) {
        setBalance(account.balanceMinutes);
    }

    // The profile's Top-ups tab and its two totals only reload when the open notice actually
    // changed and the profile is the view showing it - an unrelated account.state push (a balance
    // change with no notice) must not thrash a list nobody asked to reload.
    if (account.openTopupNoticeId != m_lastOpenTopupNoticeId) {
        if (m_inProfile) {
            reloadList(QStringLiteral("topups"));
            reloadTotals();
        }
        m_lastOpenTopupNoticeId = account.openTopupNoticeId;
    }
}

void SeatHubClient::handleSseSessionState(const SessionInfo& session)
{
    // Only the session currently being connected: a state for a different (stale, or already
    // superseded by a fresh Play) session must not wake a poll that no longer concerns it. Mirrors
    // `handleSessionState()`'s own guard.
    if (m_sessionId.isEmpty() || (!session.id.isEmpty() && session.id != m_sessionId)) {
        return;
    }
    onClientThread(m_pairing, [this]() { m_pairing->pollNow(); });
}

void SeatHubClient::handleSsePaused(bool paused)
{
    if (m_liveUpdatesPaused == paused) {
        return;
    }
    m_liveUpdatesPaused = paused;
    emit liveUpdatesPausedChanged();

    if (paused) {
        m_accountFallbackTimer->start(m_accountFallbackMs);
    }
    else {
        m_accountFallbackTimer->stop();
    }
}

void SeatHubClient::fetchAccountStateFallback()
{
    if (!m_signedIn || !m_controlPlane->hasAccessToken()) {
        return;
    }

    const quint64 epoch = m_authEpoch;
    m_controlPlane->fetchAccountState(true, [this, epoch](const ControlPlaneResult& result) {
        onClientThread(this, [this, epoch, result]() {
            // Signed out, or signed in as someone else, since this read was issued: not this
            // read's to show.
            if (epoch != m_authEpoch || !m_signedIn) {
                return;
            }

            if (!result.ok && result.statusCode == 401) {
                // L9 (`timing.md`): SeatHub's fallback-read 401 starts the same signed-out
                // sequence a revoked stream does - never an immediate sign-out.
                startSignedOutSequence();
                return;
            }

            // `GET /api/account-state` answers the same `AccountStateEvent` shape the pushed
            // frame's `data` carries (`topic`/`at`/`account`) - the account object itself is
            // nested exactly like `SseClient::dispatchFrame()` reads it, never the response body
            // directly.
            AccountStateInfo account;
            if (result.ok
                && AccountStateInfo::parse(result.body.value(QStringLiteral("account")).toObject(),
                                           &account)) {
                setBalance(account.balanceMinutes);
            }
        });
    });
}

void SeatHubClient::startSignedOutSequence()
{
    // Phase 3 D-33: a control-plane channel never ends a paid, running stream - the server ends it
    // through the liveness grace. While the engine's stream is running, only remember it; the same
    // sequence runs once where the stream's end is handled (`handleReadyForDeletion()`), so the
    // notice shows on whatever screen the stream's end lands on.
    if (m_session->active() || m_appState == QLatin1String(kStateStreaming)) {
        m_pendingSignedOutNotice = true;
        return;
    }

    if (m_signedOutNoticeTimer->isActive()) {
        // Already showing the notice and waiting it out: a second `revoked()` (or fallback-read
        // 401) during the wait starts no second sign-out.
        return;
    }

    m_signedOutNotice = true;
    emit signedOutNoticeChanged();
    m_signedOutNoticeTimer->start(m_signedOutNoticeMs);
}

void SeatHubClient::clearSignedOutNotice()
{
    m_signedOutNoticeTimer->stop();
    m_pendingSignedOutNotice = false;
    if (m_signedOutNotice) {
        m_signedOutNotice = false;
        emit signedOutNoticeChanged();
    }
}

void SeatHubClient::setAccountFallbackMs(int milliseconds)
{
    m_accountFallbackMs = qMax(1, milliseconds);
}

void SeatHubClient::setSignedOutNoticeMs(int milliseconds)
{
    m_signedOutNoticeMs = qMax(1, milliseconds);
}

void SeatHubClient::handleSseRevoked()
{
    startSignedOutSequence();
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

    // Audit F12: `DISCONNECTED` is the server saying this client stopped reporting. The HUD's own
    // reconnect line this used to also flip is dead code Plan 22 removes (D-12); only the
    // liveness stage moves here now.
    const bool reconnecting = warning == QLatin1String("DISCONNECTED");
    m_liveness->setStage(reconnecting ? QStringLiteral("reconnecting") : QStringLiteral("streaming"));
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
    finishPlay(QStringLiteral("horizon"), QStringLiteral("ok"),
               m_streamStarted ? QStringLiteral("stream") : m_lastFailedStep);
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
    // Plan 09 (ADR-0072 item 2): the classification the seam reported for THIS handshake,
    // consumed here - one handshake, one classification, so neither handler can hand the other a
    // stale step. A pairing that succeeded after the rig's 409 was cleared carries the ADR's
    // frozen record and gets its ONE `play.step` here; an ordinary success carries no step and
    // emits nothing. INFO, never WARN: the step SUCCEEDED, after exactly one bounded retry - and
    // this is the only emission the recovery makes (no second line, no WARN, and no `/end`
    // change: "nothing extra is sent to the server on a recovered pairing"). The body is the
    // fixed token and the attributes are allow-listed vocabulary - the 409 sentence, the PIN and
    // the rig address were never part of the classification and never reach this call.
    const PairingHandshakeResult classification = m_pairingClassification;
    m_pairingClassification = PairingHandshakeResult();
    if (!classification.attemptStep.isEmpty()) {
        noteStepOutcome(classification.attemptStep, QStringLiteral("ok"), classification.stepClass,
                        -1, classification.attempts);
    }

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
        if (m_reconnecting) {
            // 06.1-19/J-07: this retry paired but had nothing to attach to start with - the same
            // class of failure as a pairing or stage failure during reconnect: schedule the next
            // attempt rather than ending the held session.
            retryReconnectOrGiveUp();
            return;
        }
        const EngineLaunchReason launchReason = m_engineLaunchReason == EngineLaunchReason::Started
            ? EngineLaunchReason::StartRefused : m_engineLaunchReason;
        m_lastFailedStep = QStringLiteral("engine_prepare");
        noteStepOutcome(m_lastFailedStep, QStringLiteral("failed"),
                        classForLaunchReason(launchReason), m_playElapsed.elapsed(), 1);
        finishPlay(QStringLiteral("start_refused"), QStringLiteral("failed"),
                   m_lastFailedStep, classForLaunchReason(launchReason));
        // Pairing itself succeeded, but nothing was attached to start with - connecting never
        // truly began, so the stage this stopped at is still `pairing` (D-11).
        m_liveness->reportFailure(QStringLiteral("pairing"));
        // WR-02: nothing after this point will ever stop the timer otherwise - `beginSession()`
        // already started it, and this Play never reaches a stream to run `handleReadyForDeletion()`
        // or a teardown. Without this it kept reporting `stage: failed` (and reading the wallet)
        // every 10 s until the next `beginSession()` or sign-out.
        m_liveness->stop();
        // D-05/C2: paired, but nothing could be attached to stream with - end the session now,
        // with `failed: true`, rather than leaving it for Try again to discover. Queued onto the
        // network thread right after the report/stop above (both of which self-marshal there too,
        // being called from this - the facade - thread), so the server records the pairing-stage
        // report before it sees the cancel. 06.1's ADR item 4 / J-22: connecting is what actually
        // failed here (pairing itself succeeded), so the report tells the server that, the same
        // stage `m_liveness->setStage()` would have moved to next.
        if (inControlPlaneSession()) {
            setAttachedSessionEnded(true);
            EndReport report;
            report.stage = QStringLiteral("connecting");
            report.attemptStep = m_lastFailedStep;
            endPreStreamSessionWithReport(m_teardownGuard, m_teardown, m_sessionId, m_clientUuid,
                                          report);
        }
        // WR-01: this Play is over here, at the start refusal - the next one mints its own id.
        // `reportFailure()` above re-invoked itself onto `m_liveness`'s own (network) thread,
        // because this handler runs on the facade thread; a plain, synchronous `clearTraceId()`
        // here would therefore very likely run BEFORE that queued failure report ever reaches
        // `send()` on the network thread, emptying the id it is supposed to carry. Clearing here
        // is marshalled the same way, onto the id this Play actually minted, and only takes effect
        // if that is still the current id - see `clearTraceIdIfEquals()`'s own comment for why a
        // plain queued clear (with no comparison) is not enough either.
        clearThisPlaysTraceIdAfterAnyQueuedLivenessReport();
        raiseFailure(SeatHubFailure::generic());
        return;
    }

    // The engine session is attached and about to start: connecting is now under way (D-11).
    // 06.1-19/J-07: unless this is a reconnect's own re-pairing, in which case the stage stays
    // `reconnecting` (`aReconnectKeepsTheReconnectingStageUntilTheStreamIsBack`) - the server
    // learns the stream is actually back only from `handleConnectionStarted()`'s own `streaming`.
    if (!m_reconnecting) {
        m_liveness->setStage(QStringLiteral("connecting"));
    }
}

void SeatHubClient::handleHostResolved(const QString& sessionId, const PairedHostPtr& host)
{
    // T-06.6-52: a host that resolved for a session that is no longer attached - cancelled, or
    // superseded by a fresh Play/Try again while this handshake was still running - must never
    // attach an engine (and therefore start a stream) for the wrong session. Checked before
    // anything else here runs, including `releaseEngineSession()`, so a stale result never so much
    // as touches whatever the actually-attached session has going. The id check alone only catches
    // "superseded by a different session id"; `m_sessionEnded` is what catches "cancelled" - Cancel
    // (`interrupt()`'s pre-engine branch, via `endAttachedSessionBeforeStream()`) marks the same
    // session ended without ever clearing `m_sessionId`, so a late handshake for that same id must
    // still be dropped here.
    if (sessionId != m_sessionId || m_sessionEnded) {
        qCInfo(seathubClient) << "dropping a paired host resolved for a session that is no longer "
                                 "attached or has already ended:" << sessionId;
        return;
    }

    releaseEngineSession();
    if (m_engineSession != nullptr) {
        // A previous launch is still running. One lifecycle drives one session, so a second engine
        // cannot be attached to it; the host is dropped and `handlePairingCompleted()` reports the
        // failure rather than quietly streaming the old session under the new one's billing.
        qCWarning(seathubClient) << "a session is still active; ignoring the paired host";
        return;
    }

    m_engineLaunchReason = EngineLaunchReason::CreateFailed;
    MoonlightEngineSession* engine = MoonlightEngineSession::create(host, nullptr,
                                                                    &m_engineLaunchReason);
    if (engine == nullptr) {
        // Fail closed. `MoonlightEngineSession::create()` logs which of the two reasons it was.
        return;
    }

    // CUST-17/D-23/D-26/OD-04: the compositor's per-line filter is set from the customer's
    // current choice here, at the engine session's one and only construction point - before
    // `run()` (started from `handlePairingCompleted()`) ever lets the engine write a stats line.
    // Moonlight's own stats hotkey (Ctrl+Alt+Shift+S, or the gamepad chord) can still enable the
    // overlay mid-stream on its own; the filter set here is what it draws through either way,
    // because the compositor consults it on every rasterise regardless of what turned the overlay
    // on (`overlaymanager.cpp`'s own comment on `notifyOverlayUpdated()`). There is only this one
    // filtered path - never a second "show everything" one - so with nothing chosen the hotkey
    // draws nothing, which is the owner's OD-04 answer (05-01-SUMMARY) over the alternative.
    engine->setDebugLineFilter(m_settings->enabledStatsLabels());

    // D-09/D-13 (Plan 14): the HUD's own `OsdCompositor` becomes the engine's text rasteriser
    // here, at the same one construction point `setDebugLineFilter()` above just used - before
    // `run()` (started from `handlePairingCompleted()`) ever lets the engine write a status line.
    // `setTextRasterizer()` is a thin forward (`MoonlightEngineSession`'s own header comment); the
    // instance is the same one `HudOverlay::tick()` publishes `composedBottom()` from
    // (`m_hud.compositor()`), so both draw from the one recorded state (`osd_compositor.h`'s own
    // header comment).
    engine->setTextRasterizer(&OsdCompositor::rasterize, &m_hud.compositor());

    // D-11, Plan 13 Task 1: install the sampler tap on the compositor so every successful
    // `parseVideoStatsBlock()` call feeds the 60-second roll-up window, even when no stats
    // rows are visible. The tap is removed at stream end (handleTeardownCompleted clears it
    // by passing nullptr). The compositor holds the function-pointer behind its own mutex.
    m_hud.compositor().setStatsTap([this](const VideoStats& stats) {
        m_sampler->feed(stats);
    });

    // D-26 (Plan 14): the compositor's own stats-label choice, set alongside the filter above.
    // Plan 16 is what actually draws the stats block through it. There is only this one call
    // site today - settings are read-only while a stream is active (T-05-52, same reason
    // `setDebugLineFilter()`'s own comment above gives), so nothing refreshes this mid-stream; a
    // future refresh site would set both together, the same as here.
    m_hud.compositor().setEnabledStatsLabels(m_settings->enabledStatsLabels());

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

void SeatHubClient::clearThisPlaysTraceIdAfterAnyQueuedLivenessReport()
{
    // WR-11 (code review 06.3-REVIEW-fork.md, second review pass): `controlPlane` is read from
    // the member ONCE, here, on the calling thread, and captured by value - never read again as
    // `m_controlPlane` inside the queued lambda below. `~SeatHubClient()` sets `m_controlPlane =
    // nullptr` on the main thread before its own blocking join with the control-plane thread
    // completes (see the destructor's own ordering comment); a lambda that instead read
    // `m_controlPlane` when IT ran, on the control-plane thread, would race that write - and,
    // since the queued call is already ahead of the join in the network thread's own event
    // queue, it can still run after the member has been nulled, dereferencing a null pointer
    // through `this`. Capturing the pointer by value here removes the member read entirely: the
    // lambda never touches `this` at all, only its own captured local, which cannot change
    // after this function returns.
    ControlPlaneClient* controlPlane = m_controlPlane;
    const QString thisPlaysTraceId = controlPlane->traceId();
    // Mirrors clearTraceIdIfEquals()'s own guard, synchronously, on this (client) thread: only
    // clear SeatHub's own trace identity if this Play's id is still the current one - an older
    // Play's late clear must never wipe a newer Play's trace (`setTrace()` in `beginPlayRequest()`
    // already moved on to the newer Play's id by the time this runs, if one has started).
    if (SeatHubTelemetry::currentTraceId() == thisPlaysTraceId) {
        SeatHubTelemetry::clearTrace();
    }
    onClientThread(controlPlane, [controlPlane, thisPlaysTraceId]() {
        controlPlane->clearTraceIdIfEquals(thisPlaysTraceId);
    });
}

void SeatHubClient::handlePairingFailed(const SeatHubFailure& failure)
{
    // Plan 09 (ADR-0072): the classification the seam reported for THIS failure, consumed here -
    // one failure, one classification, so a later failure from a run that never classified itself
    // can never inherit this run's step. Neither `failure.diagnostic` nor the seam's
    // `engineError` is read anywhere below: the class is a vocabulary token or it is nothing
    // (item 2), and the local diagnostic stays on the failure the error screen renders from.
    const PairingHandshakeResult classification = m_pairingClassification;
    m_pairingClassification = PairingHandshakeResult();

    if (m_reconnecting) {
        // 06.1-19/J-07: a pairing failure during a reconnect attempt - nothing streams on this
        // attempt, but the session is still the server's to end, not this client's: schedule the
        // next attempt instead of the ordinary pre-stream teardown below, which would end a
        // session J-07 exists to keep. The liveness stage stays `reconnecting` (no `reportFailure`
        // call here, unlike the ordinary path) for the same reason `handlePairingCompleted()`'s
        // own reconnect branch leaves it alone. No `play.step` either: the step a reconnect
        // attempt fails is `reconnect`, and every reconnect emission is plan 22's.
        if (classification.attemptStep == QLatin1String("pair_handshake")) {
            m_lastFailedStep = QStringLiteral("reconnect");
            noteStepOutcome(m_lastFailedStep, QStringLiteral("failed"),
                            QStringLiteral("attempt_pair_failed"),
                            m_playElapsed.isValid() ? m_playElapsed.elapsed() : -1,
                            classification.attempts);
        }
        retryReconnectOrGiveUp();
        return;
    }

    // Plan 09 (ADR-0072 item 1): one `play.step` per classified pairing failure, WARN because it
    // failed, with only allow-listed attributes. `elapsed_ms` rides "where known" (item 6) - the
    // pairing path does not time its step yet, so it is absent rather than invented. Emitted
    // through the shipper's diagnostic lane, so the three kill switches reach it like every other
    // line, and the body is the fixed token `play.step`, never customer or engine text.
    if (!classification.attemptStep.isEmpty()) {
        m_lastFailedStep = coerceStep(classification.attemptStep);
        noteStepOutcome(m_lastFailedStep, QStringLiteral("failed"), classification.stepClass, -1,
                        classification.attempts);
    }

    finishPlay(QStringLiteral("pair_failed"), QStringLiteral("failed"),
               m_lastFailedStep, m_lastFailureClass);
    // D-11: a pairing timeout or a control-plane refusal of the pairing read, neither with an
    // engine code - reported before liveness stops, so the server sees "SeatHub failed at
    // pairing" rather than a session that simply went quiet.
    m_liveness->reportFailure(QStringLiteral("pairing"));
    // WR-02: stops it, queued after the report above on the same (network) thread, so the failure
    // report still goes out first. Without this the timer kept reporting `stage: failed` (and
    // reading the wallet) every 10 s while the customer read the error screen and after returning
    // Home, until the next `beginSession()` or sign-out.
    m_liveness->stop();
    // D-05/C2: the client's own pairing deadline, or the control plane refusing the pairing read -
    // either way nothing will ever stream on this session, so end it now with `failed: true`
    // rather than leaving a dead session for Try again to discover. Queued onto the network
    // thread right after the report/stop above, so the server records the pairing-stage report
    // before it sees the cancel; this must run before the trace-id clear below, which is
    // marshalled onto the same thread and must not overtake it. 06.1's ADR item 4 / J-22: this is
    // exactly the stage `m_liveness->reportFailure()` above just reported, so the same stage name
    // rides the end body too.
    if (inControlPlaneSession()) {
        setAttachedSessionEnded(true);
        EndReport report;
        report.stage = QStringLiteral("pairing");
        // ADR-0072 item 1 / plan 09: the failing step rides `/end` (contract 3.8.0) - and only
        // the step. An unclassified pairing failure (the controller's own deadline, a
        // control-plane refusal) leaves this empty and the field absent from the body, because a
        // step the client never classified is a step it must not invent.
        report.attemptStep = m_lastFailedStep;
        endPreStreamSessionWithReport(m_teardownGuard, m_teardown, m_sessionId, m_clientUuid,
                                      report);
    }
    // WR-01: this Play is over here, at the pairing failure - the next one mints its own id. See
    // `clearThisPlaysTraceIdAfterAnyQueuedLivenessReport()`'s own comment for why this is not a
    // plain `clearTraceId()` call.
    clearThisPlaysTraceIdAfterAnyQueuedLivenessReport();

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

void SeatHubClient::handleAuthorizationGranted()
{
    // A-68 / D-06 reversal: the control plane's `quality_profile` is no longer applied to
    // anything - the customer's saved Settings are the only decider of stream quality. The
    // session-override mechanism this slot used to call into (`SettingsBridge`) is removed
    // entirely (06.6-19 Task 2).

    // D-11: a real authorization means the rig has a pairing target - the session has moved past
    // the 409 "not ready yet" polls that are `preparing_rig`, whether or not a PIN has arrived yet.
    // 06.1-19/J-07: unless this authorization belongs to a reconnect's own re-pairing, in which
    // case the stage stays `reconnecting` (see `handlePairingCompleted()`'s matching guard).
    if (!m_reconnecting) {
        m_liveness->setStage(QStringLiteral("pairing"));
    }
}

void SeatHubClient::handleVideoStatsParsed(VideoStats stats)
{
    // Pitfall 7: the engine logs this block during its own teardown, strictly before
    // `handleReadyForDeletion()` runs (D-03's own SDL-destruction guarantee) and therefore
    // strictly before `handleTeardownCompleted()`/`handleTeardownFailed()` (CR-02: teardown can
    // fail as well as succeed, and both paths dispose of whatever is pending here exactly once -
    // `postOrStoreQualityReport()`/`storeQualityReportToOutbox()`). `m_sessionId` is still the
    // ending session's here - `setAttachedSession(QString())` only runs inside
    // `handleTeardownCompleted()`, below. With no session attached at all (a local, non-control-
    // plane attempt, or a stats block that arrived with nothing to post it against) there is
    // nothing to hold it for.
    if (m_sessionId.isEmpty()) {
        return;
    }

    // WR-04: a decoder recreation mid-session (a fullscreen toggle, a display move/resize, a
    // renderer reset) logs a second block, and a third, and so on - each one is added here, and
    // the pending report always reflects every segment seen so far, not only the last one.
    m_statsAggregator.addSegment(stats);

    // CR-02: bound to the session it belongs to here, at parse time - not read from `m_sessionId`
    // again in the handler that eventually posts it, which may run after `m_sessionId` has
    // already moved on.
    m_pendingQualitySessionId = m_sessionId;
    m_pendingQualityReport = toQualityReport(m_statsAggregator.aggregate());
    m_hasPendingQualityReport = true;
}

void SeatHubClient::postOrStoreQualityReport()
{
    // D-17/CR-02: post the held stats block once, for the session it was bound to. Cleared
    // immediately after so a second call - a later session's teardown, or one with no stats block
    // at all - never re-sends an earlier session's numbers.
    if (!m_hasPendingQualityReport) {
        return;
    }
    const QString sessionId = m_pendingQualitySessionId;
    const QJsonObject report = m_pendingQualityReport;
    const QString accountId = m_accountId;
    // CR-04 (code review 06.3-REVIEW-fork.md, second review pass): captured here, at the
    // moment this report is POSTED - not read again from `m_authEpoch` when the asynchronous
    // reply below eventually lands. A sign-out (or a sign-in as someone else) can happen while
    // this POST is still in flight; without this capture the WR-06 hold below stamped a late
    // reply with whatever epoch happened to be current when the reply arrived, so a stalled
    // report of an untagged account A could be re-tagged and sent under a later account B's
    // token once B's own `m_accountId` was filled - the same threat (T-06.3-51) CR-03 already
    // closed for the outbox's own drain, reached here by a different path.
    const quint64 epochAtPost = m_authEpoch;
    m_hasPendingQualityReport = false;
    m_pendingQualityReport = QJsonObject();
    m_pendingQualitySessionId.clear();

    m_controlPlane->postSessionQuality(
        sessionId, report,
        [this, sessionId, report, accountId, epochAtPost](const ControlPlaneResult& result) {
            onClientThread(this, [this, sessionId, report, accountId, epochAtPost, result]() {
                switch (QualityOutbox::classify(result)) {
                case QualityOutbox::Outcome::Delivered:
                    break;
                case QualityOutbox::Outcome::Refused:
                    qCWarning(seathubClient) << "quality report for" << sessionId
                                              << "refused" << result.statusCode;
                    break;
                case QualityOutbox::Outcome::AuthFailed:
                case QualityOutbox::Outcome::Retryable:
                    // Rule 2: kept for a later attempt (the next sign-in or launch), rather
                    // than lost to a dropped connection or a momentarily refused credential -
                    // this plan's own write-trigger line names only the transport/5xx/408/429
                    // set, but a 401/403 that never gets a second chance is exactly the
                    // missing-critical-functionality this outbox exists to close.
                    if (accountId.isEmpty()) {
                        // WR-06: held, not dropped - `flushUntaggedQualityReports()` tags and
                        // stores it once `m_accountId` is next filled for this same epoch.
                        // CR-04: only if the epoch this report was POSTED under is still
                        // current - `signOut()`'s own epoch bump (and every sign-in's) means a
                        // reply landing after either one belongs to an account that has
                        // already signed out; it is dropped here, exactly once, rather than
                        // re-tagged under whichever epoch happens to be current when this late
                        // reply arrives.
                        if (epochAtPost == m_authEpoch) {
                            m_untaggedQualityReports.append(
                                PendingUntaggedQualityReport{ sessionId, report, epochAtPost });
                        }
                        // else: the account that owned it has signed out - drop, never re-tag
                        // under a later sign-in's epoch.
                    }
                    else {
                        m_qualityOutbox.setDirectory(qualityOutboxDirectory());
                        m_qualityOutbox.put(sessionId, accountId, report);
                    }
                    break;
                }
            });
        });
}

void SeatHubClient::storeQualityReportToOutbox()
{
    // CR-02: the teardown POST that would have carried this report already failed to reach the
    // control plane, so this stores it straight into the outbox rather than attempting a second
    // network call that is very likely to fail the same way. Cleared either way, so a later
    // session never inherits a stale pending report.
    if (!m_hasPendingQualityReport) {
        return;
    }
    const QString sessionId = m_pendingQualitySessionId;
    const QJsonObject report = m_pendingQualityReport;
    const QString accountId = m_accountId;
    m_hasPendingQualityReport = false;
    m_pendingQualityReport = QJsonObject();
    m_pendingQualitySessionId.clear();

    if (accountId.isEmpty()) {
        // WR-06: held, not dropped - see `postOrStoreQualityReport()`'s own comment.
        m_untaggedQualityReports.append(
            PendingUntaggedQualityReport{ sessionId, report, m_authEpoch });
        return;
    }
    m_qualityOutbox.setDirectory(qualityOutboxDirectory());
    m_qualityOutbox.put(sessionId, accountId, report);
}

void SeatHubClient::flushUntaggedQualityReports()
{
    if (m_accountId.isEmpty() || m_untaggedQualityReports.isEmpty()) {
        return;
    }
    m_qualityOutbox.setDirectory(qualityOutboxDirectory());
    for (int i = 0; i < m_untaggedQualityReports.size(); ++i) {
        const PendingUntaggedQualityReport& pending = m_untaggedQualityReports.at(i);
        if (pending.epoch == m_authEpoch) {
            m_qualityOutbox.put(pending.sessionId, m_accountId, pending.report);
        }
        // else: held for an epoch that is no longer current (a sign-out or another sign-in has
        // already happened) - the account it was held for is no longer the signed-in one, so it
        // is dropped rather than mistagged under this one (`signOut()`'s own comment names this).
    }
    m_untaggedQualityReports.clear();
}

void SeatHubClient::handleTeardownCompleted(const SessionInfo& finalSession)
{
    noteStepOutcome(QStringLiteral("teardown"), QStringLiteral("ok"), QStringLiteral("ok"),
                    m_playElapsed.isValid() ? m_playElapsed.elapsed() : -1);
    postOrStoreQualityReport();

    // Home says why the session ended, read from the session itself now that teardown has confirmed it
    // is over (CUST-15, D-21). The minute count is the server's own `minutes_billed`. A session the
    // server gave no reason for leaves the line empty: showing nothing is honest, inventing a sentence
    // is not.
    if (!finalSession.endReason.isEmpty()) {
        setEndReasonText(endReasonSentence(finalSession.endReason, finalSession.minutesBilled));
    }

    // 06.1's ADR item 4 / J-22: the connecting view already showed the client's own best guess the
    // moment connecting stopped (`raiseConnectFailure()`, at the stage or pairing failure itself) -
    // this terminal read is the server's verdict, arriving after. When it names a blame for this
    // exact CONNECT_FAILED ending, it replaces what the customer reads with copy.md's blame
    // sentence and the server's own SH- reference. `raiseConnectFailure()` itself would no-op here
    // (`m_connectFailed` is already true, and rightly so elsewhere - it is what stops a LATER
    // failure from overwriting the FIRST one the customer read), so this writes `m_failure` and
    // `m_stalledReasonText` directly instead. Any other end reason keeps whatever
    // `raiseConnectFailure()` already showed.
    if (m_connectFailed && finalSession.endReason == QLatin1String("CONNECT_FAILED")) {
        const SeatHubFailure blamed = mapEndBlame(finalSession.endBlame, finalSession.endReference);
        m_failure = blamed.toVariantMap();
        m_stalledReasonText = blamed.error.toHtmlEscaped();
        emit failureChanged();
        emit connectFailedChanged();
    }

    m_liveness->stop();
    // D-11, Plan 13 Task 1: flush any partial roll-up window and stop the sampler. Also clear
    // the compositor's stats tap so the render thread stops feeding into a stopped sampler.
    // `finish()` re-invokes itself queued onto the network thread, consistent with `stop()`.
    m_hud.compositor().setStatsTap(nullptr);
    m_sampler->finish();
    m_horizon->disarm();
    m_sessionChannel->close();
    // D-27: the Play this trace id covered is over; the next one (`beginPlayRequest`) mints its
    // own.
    m_controlPlane->clearTraceId();
    SeatHubTelemetry::clearTrace();
    // D-09: the session (and whatever rig it named) is over - a crash after this point carries
    // neither in its tags.
    SeatHubTelemetry::clearSession();

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

    // D-05/C4: `retry()`'s own end-then-fresh-Play sequence was waiting on exactly this teardown -
    // the old session is gone, so the fresh Play goes out now, instead of the ordinary "land back
    // on Home" handling below (which would otherwise treat this the way an ordinary finished
    // session is treated, when the customer never actually watched one happen). This must run
    // after every clear above (trace id, telemetry session, the attached session itself) so the
    // fresh Play's own trace id is not the one wiped.
    if (m_retryBusy) {
        setRetryBusy(false);
        beginPlayRequest();
        return;
    }

    // Only leave the view if the customer is not looking at a failure: a teardown that succeeded says
    // nothing about the failure the error screen - or a stalled connecting view - is already showing.
    if (m_appState != QLatin1String(kStateError) && !m_connectFailed) {
        setAppState(!m_signedIn ? QString::fromLatin1(kStateSignedOut)
                                : QString::fromLatin1(kStateHome));
    }
}

void SeatHubClient::handleTeardownFailed(const SeatHubFailure& failure)
{
    QString teardownClass;
    switch (failure.kind) {
    case FailureKind::Network:
        teardownClass = QStringLiteral("failed_net");
        break;
    case FailureKind::Auth:
        teardownClass = QStringLiteral("failed_auth");
        break;
    case FailureKind::Api:
        teardownClass = failure.statusCode == 401 ? QStringLiteral("failed_auth")
                                                   : QStringLiteral("failed_api");
        break;
    case FailureKind::Local:
    case FailureKind::Engine:
        teardownClass = QStringLiteral("failed_local");
        break;
    }
    m_lastFailedStep = QStringLiteral("teardown");
    noteStepOutcome(m_lastFailedStep, QStringLiteral("failed"), teardownClass,
                    m_playElapsed.isValid() ? m_playElapsed.elapsed() : -1);
    // CR-02: the exact case the outbox header promises to cover - a dropped connection at stream
    // end. `postSessionQuality()`'s own POST is never attempted here (the teardown POST that
    // would have carried it already failed to reach the control plane); the report is written
    // straight to the outbox and picked up at the next sign-in or launch.
    storeQualityReportToOutbox();

    // WR-01: the Play this trace id covered ended here, at a teardown failure - the next one
    // (`beginPlayRequest`) mints its own. Before this fix only `handleTeardownCompleted()` cleared
    // it, so a teardown failure left the id in place for every later request until the next Play,
    // including another customer's sign-in on a shared PC.
    m_controlPlane->clearTraceId();
    SeatHubTelemetry::clearTrace();
    // D-09: the session (and whatever rig it named) is over - a crash after this point carries
    // neither in its tags.
    SeatHubTelemetry::clearSession();

    // C7: no retry loop is added here, but the NEXT Try again (or Play) must still be able to ask
    // for a fresh `/end` rather than finding this session's teardown claim already spent - reset
    // the guard the same way a completed teardown does. `m_sessionId` itself is left attached: the
    // session is still the server's to end, and a customer who tries again gets exactly the "end
    // it, then a fresh Play" sequence C4 already describes.
    m_teardownGuard.reset();
    setRetryBusy(false);

    // STREAM-10: the rig-side disable/remove/verify did not complete, or something was left
    // stored on this PC. Reporting success would tell the customer the opposite of what is true,
    // so it is surfaced with a reason, a retry and a reference.
    raiseFailure(failure);
}
