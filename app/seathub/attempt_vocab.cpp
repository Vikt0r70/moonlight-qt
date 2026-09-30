#include "attempt_vocab.h"

#include <QHash>

// The vocabulary's own code, and nothing else: no Qt network, no `app/backend/`, no telemetry.
// Every answer below is a token from a closed set (ADR-0072 item 1) computed from an enum
// integer, an HTTP bucket or a stage index - never from `what()`, `toQString()`, `errorString()`,
// `PairingHandshakeResult::engineError` or `SeatHubFailure::diagnostic` (ADR-0072 item 2).

namespace {

// The default arm of the coercion rule and of every mapper that cannot name what it was given:
// a token, never a sentence (`docs/spec/client.md` "Play diagnostics - Rules").
const char* const kOther = "other";
const char* const kNetOther = "net_other";

// The engine-stage slugs, 1-based against moonlight-common-c's own stage list
// (`moonlight-common-c/src/Connection.c:40-53`, read-only): index 1 is "platform initialization"
// and index 11 is "input stream establishment". Index 0 is "none" - no stage had started - and a
// failure there (or anywhere outside 1..11) is `launch_error`, the set's only such bucket.
const char* const kEngineStageSlugs[] = {
    "platform_init", "name_resolution", "audio_init", "rtsp_handshake", "control_init",
    "video_init",     "input_init",      "control_start", "video_start",  "audio_start",
    "input_start",
};

constexpr int kEngineStageCount =
    static_cast<int>(sizeof(kEngineStageSlugs) / sizeof(kEngineStageSlugs[0]));

} // namespace

QStringList attemptStepTokens()
{
    QStringList tokens;
    tokens.reserve(kAttemptStepCount);
    for (int index = 0; index < kAttemptStepCount; ++index) {
        tokens.append(QString::fromLatin1(kAttemptStepTokens[index]));
    }
    return tokens;
}

QString coerceStep(const QString& value)
{
    for (int index = 0; index < kAttemptStepCount; ++index) {
        if (value == QLatin1String(kAttemptStepTokens[index])) {
            return value;
        }
    }
    return QLatin1String(kOther);
}

QString classForNetworkError(int code)
{
    // `QNetworkReply::NetworkError` as integers, so this module stays free of Qt network: the
    // enum's own values (Qt 6 `qnetworkreply.h`): 1 ConnectionRefused, 2 RemoteHostClosed,
    // 3 HostNotFound, 4 Timeout, 6 SslHandshakeFailed, and 101-105 the five proxy errors.
    switch (code) {
    case 1:
        return QStringLiteral("net_refused");
    case 2:
        return QStringLiteral("net_closed");
    case 3:
        return QStringLiteral("net_host_not_found");
    case 4:
        return QStringLiteral("net_timeout");
    case 6:
        return QStringLiteral("net_tls");
    case 101:
    case 102:
    case 103:
    case 104:
    case 105:
        return QStringLiteral("net_proxy");
    default:
        // The documented default arm: 0 (NoError), the unlisted errors and anything this build
        // has never seen all land here rather than on a sentence.
        return QLatin1String(kNetOther);
    }
}

QString classForHttpStatus(int status)
{
    if (status >= 400 && status <= 499) {
        return QStringLiteral("http_4xx");
    }
    if (status >= 500 && status <= 599) {
        return QStringLiteral("http_5xx");
    }
    // A redirect, a 2xx that carried no usable body, or a status outside the two buckets: the
    // family's default. `pair_server_info`'s closed set has no `other`, so it is `net_other`.
    return QLatin1String(kNetOther);
}

QString classForPairState(int state)
{
    // `NvPairingManager::PairState`'s own order (`app/backend/nvpairingmanager.h`): PAIRED 0,
    // PIN_WRONG 1, FAILED 2, ALREADY_IN_PROGRESS 3. The mapper runs only on a failure - a
    // PAIRED handshake returns before any class is computed - so 0 has no class of its own.
    switch (state) {
    case 1:
        return QStringLiteral("pin_rejected");
    case 3:
        return QStringLiteral("in_progress");
    case 2:
        return QStringLiteral("failed");
    default:
        return QLatin1String(kOther);
    }
}

QString classForEngineStage(int stage)
{
    if (stage >= 1 && stage <= kEngineStageCount) {
        return QString::fromLatin1(kEngineStageSlugs[stage - 1]);
    }
    // Stage `none`, an index past the last stage, a negative index: no stage had started, which
    // is what `launch_error` names (a `displayLaunchError` before the first stage).
    return QStringLiteral("launch_error");
}

QString classForEngineStageName(const QString& stageName)
{
    static const QStringList stageNames{
        QStringLiteral("Platform initialization"),
        QStringLiteral("Name resolution"),
        QStringLiteral("Audio stream initialization"),
        QStringLiteral("RTSP handshake"),
        QStringLiteral("Control stream initialization"),
        QStringLiteral("Video stream initialization"),
        QStringLiteral("Input stream initialization"),
        QStringLiteral("Control stream establishment"),
        QStringLiteral("Video stream establishment"),
        QStringLiteral("Audio stream establishment"),
        QStringLiteral("Input stream establishment")};
    for (qsizetype index = 0; index < stageNames.size(); ++index) {
        if (stageNames.at(index).compare(stageName, Qt::CaseInsensitive) == 0) {
            return classForEngineStage(static_cast<int>(index) + 1);
        }
    }
    return QStringLiteral("launch_error");
}

QString classForStreamError(int code)
{
    switch (code) {
    case 0:
        return QStringLiteral("graceful");
    case -100:
        return QStringLiteral("no_video_traffic");
    case -101:
        return QStringLiteral("no_video_frame");
    case -102:
        return QStringLiteral("early_termination");
    case -103:
        return QStringLiteral("protected_content");
    case -104:
        return QStringLiteral("frame_conversion");
    default:
        return QStringLiteral("net_other");
    }
}

QString classForLaunchReason(EngineLaunchReason reason)
{
    switch (reason) {
    case EngineLaunchReason::Started:
        return QString();
    case EngineLaunchReason::NoApp:
        return QStringLiteral("no_app");
    case EngineLaunchReason::AppCount:
        return QStringLiteral("app_count");
    case EngineLaunchReason::CreateFailed:
        return QStringLiteral("engine_create");
    case EngineLaunchReason::StartRefused:
        return QStringLiteral("start_refused");
    case EngineLaunchReason::AppListFailed:
        return QStringLiteral("app_list_failed");
    }
    return QStringLiteral("engine_create");
}

bool isClosedFailureClass(const QString& step, const QString& failureClass)
{
    if (failureClass.isEmpty()) {
        return true;
    }
    static const QHash<QString, QStringList> classes = {
        {QStringLiteral("allocate"), {QStringLiteral("no_host"), QStringLiteral("refused_balance"),
                                       QStringLiteral("refused_state"), QStringLiteral("unreachable"),
                                       QStringLiteral("server_error"), QStringLiteral("bad_response")}},
        {QStringLiteral("rig_wait"), {QStringLiteral("ok"), QStringLiteral("server_ended")}},
        {QStringLiteral("pair_authorize"), {QStringLiteral("deadline"), QStringLiteral("refused"),
                                            QStringLiteral("bad_response"), QStringLiteral("no_seam"),
                                            QStringLiteral("unreachable_deadline")}},
        {QStringLiteral("pair_server_info"), {QStringLiteral("no_address"), QStringLiteral("net_refused"),
                                              QStringLiteral("net_closed"), QStringLiteral("net_host_not_found"),
                                              QStringLiteral("net_timeout"), QStringLiteral("net_tls"),
                                              QStringLiteral("net_proxy"), QStringLiteral("net_other"),
                                              QStringLiteral("http_4xx"), QStringLiteral("http_5xx")}},
        {QStringLiteral("pair_handshake"), {QStringLiteral("pin_rejected"), QStringLiteral("in_progress"),
                                            QStringLiteral("failed"), QStringLiteral("net_refused"),
                                            QStringLiteral("net_closed"), QStringLiteral("net_host_not_found"),
                                            QStringLiteral("net_timeout"), QStringLiteral("net_tls"),
                                            QStringLiteral("net_proxy"), QStringLiteral("net_other"),
                                            QStringLiteral("http_4xx"), QStringLiteral("http_5xx"),
                                            QStringLiteral("crypto_init"), QStringLiteral("other")}},
        {QStringLiteral("engine_prepare"), {QStringLiteral("no_app"), QStringLiteral("app_count"),
                                            QStringLiteral("engine_create"), QStringLiteral("start_refused"),
                                            QStringLiteral("app_list_failed")}},
        {QStringLiteral("engine_connect"), {QStringLiteral("platform_init"), QStringLiteral("name_resolution"),
                                             QStringLiteral("audio_init"), QStringLiteral("rtsp_handshake"),
                                             QStringLiteral("control_init"), QStringLiteral("video_init"),
                                             QStringLiteral("input_init"), QStringLiteral("control_start"),
                                             QStringLiteral("video_start"), QStringLiteral("audio_start"),
                                             QStringLiteral("input_start"), QStringLiteral("launch_error")}},
        {QStringLiteral("first_frame"), {QStringLiteral("ok"), QStringLiteral("no_first_frame")}},
        {QStringLiteral("stream"), {QStringLiteral("graceful"), QStringLiteral("no_video_traffic"),
                                     QStringLiteral("no_video_frame"), QStringLiteral("early_termination"),
                                     QStringLiteral("protected_content"), QStringLiteral("frame_conversion"),
                                     QStringLiteral("net_other")}},
        {QStringLiteral("reconnect"), {QStringLiteral("attempt_pair_failed"),
                                        QStringLiteral("attempt_engine_failed"),
                                        QStringLiteral("grace_expired"), QStringLiteral("reconnect_limit"),
                                        QStringLiteral("ok")}},
        {QStringLiteral("teardown"), {QStringLiteral("failed_net"), QStringLiteral("failed_api"),
                                      QStringLiteral("failed_auth"), QStringLiteral("failed_local"),
                                      QStringLiteral("ok")}},
    };
    return classes.value(step).contains(failureClass);
}
