#include "attempt_vocab.h"

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
