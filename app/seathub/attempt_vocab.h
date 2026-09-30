#pragma once

// attempt_vocab (ADR-0072 item 1, Plan 09 Task 1): the attempt vocabulary - the 11 frozen
// `attempt_step` tokens and the mappers that turn an enum integer, an HTTP status bucket or an
// engine-stage index into a closed `failure_class`.
//
// Pure and sentry-free, in the style of `engine_termination.h`: no Qt network, no `app/backend/`,
// no telemetry include, so any suite can link it. The class sets stay in Sentry; only the step
// travels to the server (D-12, `attempt_step` on `POST /api/sessions/{id}/end`).
//
// The redaction rule this module exists to hold (ADR-0072 item 2, `docs/spec/client.md`
// "Play diagnostics - Rules"): a class is computed ONLY from an enum integer
// (`QNetworkReply::NetworkError`, `NvPairingManager::PairState`, the engine stage index), an
// HTTP status bucket or a closed server string - never from `what()`, `toQString()`,
// `errorString()`, `PairingHandshakeResult::engineError` or `SeatHubFailure::diagnostic`. Every
// mapper's default arm is a vocabulary token, never text.
//
// The token block below is read mechanically: `scripts/check_attempt_vocabulary.py --require
// client,fork --fork <path>` takes the string literals between the `attempt-steps-begin` and
// `attempt-steps-end` markers and asserts they equal `docs/spec/client.md` "Attempt steps", in
// order (ADR-0072 "Frozen names").

#include <QString>
#include <QStringList>

/// The 11 `attempt_step` tokens in the frozen order (ADR-0072 item 1). One literal per line
/// between the two markers, because the vocabulary script reads this block and nothing else.
// attempt-steps-begin
constexpr const char* const kAttemptStepTokens[] = {
    "allocate",
    "rig_wait",
    "pair_authorize",
    "pair_server_info",
    "pair_handshake",
    "engine_prepare",
    "engine_connect",
    "first_frame",
    "stream",
    "reconnect",
    "teardown",
};
// attempt-steps-end

/// How many tokens `kAttemptStepTokens` carries.
constexpr int kAttemptStepCount =
    static_cast<int>(sizeof(kAttemptStepTokens) / sizeof(kAttemptStepTokens[0]));

/// The 11 attempt-step tokens as strings, in the frozen order.
QStringList attemptStepTokens();

/// `value` when it is already one of the 11 tokens, `"other"` otherwise - the client's coercion
/// rule (the server coerces to `unknown`; ADR-0072 item 1).
QString coerceStep(const QString& value);

/// `QNetworkReply::NetworkError` (as an integer, so this header stays network-free) to its class:
/// 1 `net_refused`, 2 `net_closed`, 3 `net_host_not_found`, 4 `net_timeout`, 6 `net_tls`,
/// 101-105 `net_proxy`, anything else `net_other` (the documented default).
QString classForNetworkError(int code);

/// An HTTP status to its bucket: 400-499 `http_4xx`, 500-599 `http_5xx`, anything else
/// `net_other` - the family's default, because `pair_server_info`'s closed set has no `other`.
QString classForHttpStatus(int status);

/// `NvPairingManager::PairState` (as an integer) to its class: `PIN_WRONG` `pin_rejected`,
/// `ALREADY_IN_PROGRESS` `in_progress`, `FAILED` `failed`, everything else `other`. The mapper is
/// only ever called on a failure; `PAIRED` has no failure class.
QString classForPairState(int state);

/// The engine-stage index (moonlight-common-c `Connection.c`, 1-based: 1 `platform_init` ...
/// 11 `input_start`) to its slug; every other value - stage `none`, a negative index, an index
/// past the last stage - is `launch_error`, the set's only bucket for "no stage had started".
QString classForEngineStage(int stage);
