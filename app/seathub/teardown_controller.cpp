#include "teardown_controller.h"

#include <QLoggingCategory>
#include <QThread>

Q_LOGGING_CATEGORY(seathubTeardown, "seathub.teardown")

// The rig-side sequence this controller waits on, in the order STREAM-10 and D-10 require -
//  1. disable   POST /api/clients/update {uuid, enabled:false}   Node Agent
//  2. unpair    POST /api/clients/unpair {uuid}                  Node Agent
//  3. verify    GET  /api/clients/list  (the `listClients` call)  Node Agent
// None of those three is on the control-plane contract (`docs/spec/openapi.yaml` has no
// `/api/clients/*` path), and this client holds no Sunshine admin credential, so it calls none
// of them - `03-RESEARCH.md` §Teardown order: "The client must not call Sunshine admin endpoints
// directly because the specification forbids exposing those credentials; Node Agent owns those
// calls." The order still binds this client, because it is why the client waits: a session
// reaches a terminal state only after the rig side has finished the three steps in that order,
// so "the session is terminal" is this client's evidence that they happened. Waiting on the
// control plane instead of doing the work is what keeps the client outside the admin boundary.

namespace {

const char* kStateIdle = "idle";
const char* kStateDisabling = "disabling";
const char* kStateUnpairing = "unpairing";
const char* kStateVerifying = "verifying";
const char* kStateClearing = "clearing";
const char* kStateDone = "done";
const char* kStateFailed = "failed";

// `EndReason.TEARDOWN_TIMEOUT` (`openapi.yaml` §EndReason): the rig side did not finish inside
// the grace window. Reported as the reason, with the client's own reference (ADR-0008).
const char* kTeardownTimeout = "TEARDOWN_TIMEOUT";
const char* kTeardownTimeoutSentence = "The rig didn't finish closing the session. Support can see it.";
const char* kTeardownFailedSentence = "We couldn't close the session cleanly. Support can see it.";

QString stateForStage(TeardownStage stage)
{
    switch (stage) {
    case TeardownStage::Disable:
        return QString::fromLatin1(kStateDisabling);
    case TeardownStage::Unpair:
        return QString::fromLatin1(kStateUnpairing);
    case TeardownStage::Verify:
        return QString::fromLatin1(kStateVerifying);
    case TeardownStage::Clear:
        return QString::fromLatin1(kStateClearing);
    case TeardownStage::Done:
        return QString::fromLatin1(kStateDone);
    case TeardownStage::Failed:
        return QString::fromLatin1(kStateFailed);
    case TeardownStage::Idle:
        break;
    }
    return QString::fromLatin1(kStateIdle);
}

} // namespace

TeardownController::TeardownController(QObject* parent)
    : QObject(parent),
      m_state(QString::fromLatin1(kStateIdle)),
      m_verifyTimer(new QTimer(this))
{
    m_verifyTimer->setSingleShot(true);
    connect(m_verifyTimer, &QTimer::timeout, this, &TeardownController::verifySession);
}

void TeardownController::setControlPlane(ControlPlaneClient* client)
{
    m_client = client;
}

bool TeardownController::onOwnThread() const
{
    // `thread()` goes null only when the QThread that owned this object has been destroyed, in
    // which case there is no queue left to hand work to and running here is the only option.
    QThread* owner = thread();
    return owner == nullptr || owner == QThread::currentThread();
}

void TeardownController::setTeardownGraceMs(int milliseconds)
{
    m_teardownGraceMs = qMax(1, milliseconds);
}

void TeardownController::setVerifyIntervalMs(int milliseconds)
{
    m_verifyIntervalMs = qMax(1, milliseconds);
}

bool TeardownController::isOrdered(TeardownStage from, TeardownStage to)
{
    // Strictly forward. Re-entering a stage is not progress, and a failed teardown may not
    // resume at an earlier one - the class of mistake this prevents is exactly Pitfall 5.
    return static_cast<int>(to) > static_cast<int>(from);
}

void TeardownController::setState(const QString& state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged();
}

bool TeardownController::advanceTo(TeardownStage stage)
{
    if (!isOrdered(m_stage, stage)) {
        // Refused, and refused loudly: a caller trying to move backwards is either a bug or an
        // attempt to remove a pairing before stopping the stream that is using it.
        qCWarning(seathubTeardown) << "refusing out-of-order teardown transition from"
                                   << static_cast<int>(m_stage) << "to"
                                   << static_cast<int>(stage);
        return false;
    }

    m_stage = stage;
    setState(stateForStage(stage));
    emit stageEntered(stage);
    return true;
}

void TeardownController::teardown(const QString& sessionId, const QString& clientUuid, bool failed)
{
    if (!onOwnThread()) {
        // HR-01: `m_verifyTimer` is a child of this object, and this object is moved to the
        // network thread so its work runs while the stream has the main thread suspended. A call
        // from the main thread has to be handed over: `QTimer::start()` from a foreign thread is
        // refused by Qt with a warning, which used to strand teardown before `Clear` - the store
        // was never cleared and `teardownCompleted()` never fired. `failed` is captured and
        // forwarded here too - a queued call that dropped it would silently re-derive the
        // parameterless default (`false`) on the owning thread, telling the server "customer
        // ended" for what was actually a connect failure (D-05/D-23).
        QMetaObject::invokeMethod(this, [this, sessionId, clientUuid, failed]() {
            teardown(sessionId, clientUuid, failed);
        }, Qt::QueuedConnection);
        return;
    }

    if (sessionId.isEmpty()) {
        fail(SeatHubFailure::local(QString::fromLatin1(kTeardownFailedSentence)));
        return;
    }

    m_sessionId = sessionId;
    m_clientUuid = clientUuid;
    m_finished = false;
    m_cleared = false;
    m_stage = TeardownStage::Idle;
    m_clock.start();

    qCInfo(seathubTeardown) << "teardown started for session" << sessionId;

    if (!advanceTo(TeardownStage::Disable)) {
        return;
    }

    if (m_client == nullptr) {
        fail(SeatHubFailure::local(QString::fromLatin1(kTeardownFailedSentence)));
        return;
    }

    // Step 1 on the rig, requested through the only route this client has. `POST /end` is
    // idempotent, so pressing End twice is not an error.
    m_client->endSession(sessionId, failed, [this](const ControlPlaneResult& result) {
        handleEndResult(result);
    });
}

void TeardownController::cancel()
{
    if (!onOwnThread()) {
        QMetaObject::invokeMethod(this, [this]() { cancel(); }, Qt::QueuedConnection);
        return;
    }

    m_verifyTimer->stop();
    m_finished = true;
    m_clock.invalidate();
    m_sessionId.clear();
    m_stage = TeardownStage::Idle;
    setState(QString::fromLatin1(kStateIdle));
}

void TeardownController::handleEndResult(const ControlPlaneResult& result)
{
    if (m_finished) {
        return;
    }

    if (!result.ok) {
        fail(result.toFailure());
        return;
    }

    // The rig is now disabling the client and removing the pairing record. Those are steps 2
    // and 3, and this client's only way to see them is the session's own progress, so it moves
    // to observing them rather than performing them.
    if (!advanceTo(TeardownStage::Unpair)) {
        return;
    }
    if (!advanceTo(TeardownStage::Verify)) {
        return;
    }

    scheduleVerify();
}

void TeardownController::scheduleVerify()
{
    m_verifyTimer->start(m_verifyIntervalMs);
}

void TeardownController::verifySession()
{
    if (!onOwnThread()) {
        QMetaObject::invokeMethod(this, [this]() { verifySession(); }, Qt::QueuedConnection);
        return;
    }

    if (m_finished || m_sessionId.isEmpty()) {
        return;
    }

    if (m_clock.elapsed() >= m_teardownGraceMs) {
        // `timing.md`: a session still `ENDING` past the deadline fails `TEARDOWN_TIMEOUT`.
        // Reporting it is the honest outcome - the pairing may or may not be gone on the rig,
        // and claiming success would tell the customer the opposite of what is true.
        fail(SeatHubFailure::local(QString::fromLatin1(kTeardownTimeoutSentence)),
             QString::fromLatin1(kTeardownTimeout));
        return;
    }

    if (m_client == nullptr) {
        fail(SeatHubFailure::local(QString::fromLatin1(kTeardownFailedSentence)));
        return;
    }

    m_client->fetchSession(m_sessionId, [this](const ControlPlaneResult& result) {
        handleVerifyResult(result);
    });
}

void TeardownController::handleVerifyResult(const ControlPlaneResult& result)
{
    if (m_finished) {
        return;
    }

    if (!result.ok) {
        if (result.statusCode == 0) {
            // Unreachable control plane. Keep waiting inside the grace window rather than
            // declaring a result this client cannot actually know.
            scheduleVerify();
            return;
        }
        fail(result.toFailure());
        return;
    }

    SessionInfo info;
    if (!SessionInfo::parse(result.body, &info)) {
        fail(SeatHubFailure::local(QString::fromLatin1(kTeardownFailedSentence)));
        return;
    }

    if (!info.isTerminal()) {
        // Still ENDING: the rig side has not finished. Wait.
        scheduleVerify();
        return;
    }

    // Step 4: leave nothing behind. STREAM-10 - "the client keeps no stored rig, address or
    // pairing of its own" - is what this stage stands for, and it holds by construction: the
    // client's only stored data is the customer's sign-in credential (`TokenStore`), the rig, its
    // address and the pairing live in the engine's session and are dropped with it, and nothing of
    // them is ever written to disk here.
    //
    // This stage used to sweep the token store as well ("every credential this client holds is
    // removed"). That was written before ADR-0050 made sessions permanent, and it means a customer
    // who plays once is signed out at the next launch - the opposite of CUST-08. The credential is
    // the customer's sign-in, not a session artifact, so it is deliberately NOT touched here: only
    // sign-out (`SeatHubClient::signOut()`) removes it (Phase 5 plan 02, D-06).
    if (!advanceTo(TeardownStage::Clear)) {
        return;
    }

    m_cleared = true;
    m_verifyTimer->stop();
    m_finished = true;

    if (!advanceTo(TeardownStage::Done)) {
        return;
    }

    qCInfo(seathubTeardown) << "teardown complete for session" << m_sessionId;
    emit teardownCompleted(info);
}

void TeardownController::fail(const SeatHubFailure& failure, const QString& failureCode)
{
    m_verifyTimer->stop();
    m_finished = true;
    m_stage = TeardownStage::Failed;
    setState(QString::fromLatin1(kStateFailed));

    SeatHubFailure reported = failure;
    if (!failureCode.isEmpty()) {
        reported.failure = failureCode;
    }

    qCWarning(seathubTeardown) << "teardown failed; reference" << reported.reference
                               << "failure" << reported.failure;

    emit teardownFailed(reported);
}
