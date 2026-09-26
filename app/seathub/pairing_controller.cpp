#include "pairing_controller.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(seathubPairing, "seathub.pairing")

namespace {

const char* kStateIdle = "idle";
const char* kStateAuthorizing = "authorizing";
const char* kStatePairing = "pairing";
const char* kStateReady = "ready";
const char* kStateFailed = "failed";

// `docs/spec/copy.md` §Play flow. A pairing that does not resolve is a SeatHub failure with a
// reason and a retry, never a Moonlight error surface (ADR-0008, D-51). These are the local sentences;
// a control-plane refusal carries the control plane's own sentence and reference instead.
const char* kTimedOut = "The rig didn't finish connecting. Try again.";
const char* kNoAuthorization = "We couldn't get this session ready. Try again.";

// 409 from `GET /api/sessions/{id}/pairing` means the session is not in a state that has a
// pairing target yet - the host has not reached READY. That is a wait, not a failure.
const int kHttpConflict = 409;

} // namespace

PairingController::PairingController(QObject* parent)
    : QObject(parent),
      m_state(QString::fromLatin1(kStateIdle)),
      m_pollTimer(new QTimer(this))
{
    m_pollTimer->setSingleShot(true);
    connect(m_pollTimer, &QTimer::timeout, this, &PairingController::pollAuthorization);
}

void PairingController::setControlPlane(ControlPlaneClient* client)
{
    m_client = client;
}

void PairingController::setSeam(PairingSeam* seam)
{
    m_seam = seam;
}

void PairingController::setPollIntervalMs(int milliseconds)
{
    m_pollIntervalMs = qMax(1, milliseconds);
}

void PairingController::setDeadlineMs(int milliseconds)
{
    m_deadlineMs = qMax(1, milliseconds);
}

void PairingController::setState(const QString& state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged();
}

bool PairingController::deadlineExceeded() const
{
    return m_clock.isValid() && m_clock.elapsed() >= m_deadlineMs;
}

void PairingController::start(const QString& sessionId)
{
    if (sessionId.isEmpty()) {
        fail(SeatHubFailure::local(QString::fromLatin1(kNoAuthorization)));
        return;
    }

    m_sessionId = sessionId;
    m_clientUuid.clear();
    m_finished = false;
    m_polls = 0;
    m_conflictPolls = 0;
    m_clock.start();

    qCInfo(seathubPairing) << "silent pairing started for session" << sessionId;

    // No PIN surface is created here or anywhere below. The customer is never asked for one
    // (STREAM-03): the PIN exists only between the control plane and the engine's handshake.
    setState(QString::fromLatin1(kStateAuthorizing));
    pollAuthorization();
}

void PairingController::cancel()
{
    m_pollTimer->stop();
    m_finished = true;
    m_clock.invalidate();
    m_sessionId.clear();
    setState(QString::fromLatin1(kStateIdle));
}

void PairingController::scheduleNextPoll()
{
    m_pollTimer->start(m_pollIntervalMs);
}

void PairingController::pollAuthorization()
{
    if (m_finished || m_sessionId.isEmpty()) {
        return;
    }

    if (deadlineExceeded()) {
        // D-08: nothing resolved inside 90 s. Fail closed. The host side cancels its own
        // pending request at the same horizon; this client's job is to stop waiting and say so
        // rather than leave a half-open handshake behind.
        fail(SeatHubFailure::local(QString::fromLatin1(kTimedOut)));
        return;
    }

    if (m_client == nullptr) {
        fail(SeatHubFailure::local(QString::fromLatin1(kNoAuthorization)));
        return;
    }

    ++m_polls;
    m_client->fetchSessionAuthorization(
        m_sessionId,
        [this](const ControlPlaneResult& result) { handleAuthorization(result); });

    // The session itself, on the same tick (ADR-0055): the state the connecting stages come from.
    // Fire and forget - it never decides when the next poll is, so a slow or failed read cannot
    // slow pairing down or spend its deadline.
    const QString polledSession = m_sessionId;
    m_client->fetchSession(polledSession, [this, polledSession](const ControlPlaneResult& result) {
        handleSessionRead(polledSession, result);
    });
}

void PairingController::handleSessionRead(const QString& polledSession,
                                          const ControlPlaneResult& result)
{
    // After pairing has finished (or was cancelled) the engine owns what the customer sees, and a
    // reply for an earlier session must never speak for this one.
    if (m_finished || polledSession != m_sessionId || !result.ok) {
        return;
    }

    SessionInfo info;
    if (!SessionInfo::parse(result.body, &info)) {
        return;
    }
    emit sessionRead(info);
}

void PairingController::handleAuthorization(const ControlPlaneResult& result)
{
    if (m_finished) {
        return;
    }

    if (!result.ok) {
        if (result.statusCode == 0) {
            // The control plane is unreachable. Keep trying until the deadline: a brief network
            // gap is not a reason to throw away the session (D-33's posture, applied here).
            qCInfo(seathubPairing) << "authorization poll failed in transport; retrying";
            scheduleNextPoll();
            return;
        }

        if (result.statusCode == kHttpConflict) {
            // No pairing target yet. This is the expected state for the first polls while the
            // host is still being prepared, so it is not logged as a problem.
            //
            // Nor does it spend D-08's 90 s, which bounds a pairing resolving, not the rig getting
            // ready: preparation has its own server deadline, and a session that deadline fails
            // arrives here as a terminal session read (`SeatHubClient::handleSessionState`). The
            // clock restarts on every "not yet", so it counts from the last moment the rig could
            // not pair. A transport failure does not restart it, so an outage is still bounded.
            ++m_conflictPolls;
            m_clock.restart();
            scheduleNextPoll();
            return;
        }

        fail(result.toFailure());
        return;
    }

    SessionAuthorization authorization;
    if (!SessionAuthorization::parse(result.body, &authorization)) {
        fail(SeatHubFailure::local(QString::fromLatin1(kNoAuthorization)));
        return;
    }

    // A-68 / D-06 reversal: `authorization.qualityProfile` is parsed above but never forwarded -
    // nothing reads it any more.
    emit authorizationGranted();

    if (authorization.pairingPin.isEmpty()) {
        // `pairing_pin` is null until the host is ready to pair. Poll again rather than seating
        // an empty PIN into the engine.
        scheduleNextPoll();
        return;
    }

    if (m_seam == nullptr) {
        // No engine pairing seam was supplied. Failing closed is the only safe answer: silently
        // reporting success here would tell the caller a rig was paired when nothing paired.
        fail(SeatHubFailure::local(QString::fromLatin1(kNoAuthorization)));
        return;
    }

    setState(QString::fromLatin1(kStatePairing));

    PairingTarget target;
    target.sessionId = authorization.sessionId.isEmpty() ? m_sessionId : authorization.sessionId;
    target.hostAddress = authorization.hostAddress;
    target.httpsPort = authorization.httpsPort;
    target.pairingPin = authorization.pairingPin;

    // The engine seam gets the address and the PIN. Nothing else in this process does.
    m_seam->pair(target, [this](bool ok, const QString& uuid, const QString& engineError) {
        handleSeamResult(ok, uuid, engineError);
    });
}

void PairingController::handleSeamResult(bool ok, const QString& clientUuid,
                                         const QString& engineError)
{
    if (m_finished) {
        return;
    }

    m_pollTimer->stop();

    if (!ok) {
        // The engine's own sentence is diagnostic only - it is never what the customer reads
        // (D-51, T-03-05). `local()` keeps it out of the rendered reason.
        SeatHubFailure failure = SeatHubFailure::local(QString::fromLatin1(kTimedOut));
        failure.diagnostic = engineError;
        fail(failure);
        return;
    }

    if (clientUuid.isEmpty()) {
        // Upstream reported success without an exact client UUID. The UUID is the only thing
        // that identifies this client (Pitfall 3, D-07) and teardown cannot verify a removal
        // without it, so an unidentified success is a failure (Pitfall 3, fail-closed).
        fail(SeatHubFailure::local(QString::fromLatin1(kNoAuthorization)));
        return;
    }

    m_finished = true;
    m_clientUuid = clientUuid;
    setState(QString::fromLatin1(kStateReady));
    qCInfo(seathubPairing) << "silent pairing complete";
    emit pairingCompleted(clientUuid);
}

void PairingController::fail(const SeatHubFailure& failure)
{
    m_pollTimer->stop();
    m_finished = true;
    setState(QString::fromLatin1(kStateFailed));

    // Logged without the PIN and without the address. What support needs is the reference and
    // the fact that pairing did not resolve, not the credentials that were in flight.
    qCWarning(seathubPairing) << "silent pairing failed; reference" << failure.reference;

    emit pairingFailed(failure);
}
