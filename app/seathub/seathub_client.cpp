#include "seathub_client.h"

#include <QLoggingCategory>
#include <QWindow>

#include "session_lifecycle.h"
#include "settings_bridge.h"

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

} // namespace

SeatHubClient::SeatHubClient(QObject* parent)
    : QObject(parent),
      m_appState(QString::fromLatin1(kStateSignedOut)),
      m_session(new SessionLifecycle(this)),
      m_settings(new SettingsBridge(this))
{
    connect(m_session, &SessionLifecycle::stageStarting, this, &SeatHubClient::handleStageStarting);
    connect(m_session, &SessionLifecycle::stageFailed, this, &SeatHubClient::handleStageFailed);
    connect(m_session, &SessionLifecycle::connectionStarted, this, &SeatHubClient::handleConnectionStarted);
    connect(m_session, &SessionLifecycle::displayLaunchError, this, &SeatHubClient::handleDisplayLaunchError);
    connect(m_session, &SessionLifecycle::displayLaunchWarning, this, &SeatHubClient::handleDisplayLaunchWarning);
    connect(m_session, &SessionLifecycle::quitStarting, this, &SeatHubClient::handleQuitStarting);
    connect(m_session, &SessionLifecycle::sessionFinished, this, &SeatHubClient::handleSessionFinished);
    connect(m_session, &SessionLifecycle::readyForDeletion, this, &SeatHubClient::handleReadyForDeletion);
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
        m_settings->setStreamingActive(isStreaming);
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
    m_session->interrupt();
}

void SeatHubClient::requestOtp(const QString& phoneE164)
{
    // Stubbed control plane (Plan 03-03 owns the real `POST /api/auth/otp` call). The
    // phone number is never stored on this object beyond the call itself.
    if (phoneE164.trimmed().isEmpty()) {
        emit otpRejected(QString::fromLatin1(kOtpMismatch), QString());
        return;
    }
    emit otpRequested(phoneE164);
}

void SeatHubClient::verifyOtp(const QString& phoneE164, const QString& code)
{
    // Stubbed control plane (Plan 03-03). A six-digit code is accepted; nothing is
    // authenticated, and no token exists anywhere on this object.
    if (code.size() != 6 || code.toInt() == 0) {
        emit otpRejected(QString::fromLatin1(kOtpMismatch), QString());
        return;
    }

    m_identity = phoneE164;
    emit identityChanged();
    emit otpAccepted();
    setAppState(QString::fromLatin1(kStateHome));
}

void SeatHubClient::signOut()
{
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

    // D-37 / D-14: the launch's negotiated results and its in-memory overrides are over. The
    // saved preferences were never touched, so the settings page goes back to showing them.
    m_settings->noteSessionFinished();
}

void SeatHubClient::openSettings()
{
    setInSettings(true);
}

void SeatHubClient::closeSettings()
{
    setInSettings(false);
}
