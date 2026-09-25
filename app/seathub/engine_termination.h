#pragma once

// engine_termination (D-11, A-51, Plan 15 Task 2): the pure matcher for upstream's own
// termination log line.
//
// `Session::clConnectionTerminated(int errorCode)` (`app/streaming/session.cpp:96-146`,
// read-only, unchanged since `2394dfe8` of 2020-02-24 and present at both the pinned `v6.1.0` tag
// and upstream `master`) is the ONLY place the engine's `ML_ERROR_*` integer
// (`moonlight-common-c/moonlight-common-c/src/Limelight.h:404-436`, read-only) is available at
// all: no `EngineSession` signal carries it (`engine_session.h`'s eight signals are all
// upstream's own `Session` signals, none of which pass an error code through), and the sentence
// `displayLaunchError()` shows the customer is lossy on purpose - `ML_ERROR_PROTECTED_CONTENT`
// and `ML_ERROR_UNEXPECTED_EARLY_TERMINATION` share one sentence, and
// `ML_ERROR_GRACEFUL_TERMINATION` (0, a REAL code, not "no error") shows nothing at all. The one
// place the integer survives, unconditionally, on every code path including 0, is
// `SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Connection terminated: %d", errorCode)`
// (`session.cpp:144-146`), logged just before the engine pushes `SDL_QUIT`. Reading that line
// through the tee (`log_tee.h`) is the one route that reaches the code without editing
// `session.cpp` a second time - `ADR-0044`'s one permitted engine-file edit there is the window
// title literal (`ADR-0046`), and this plan does not widen that exception.

#include <QtGlobal>

/// Matches upstream's exact `"Connection terminated: %d"` line: `SDL_LOG_CATEGORY_APPLICATION`,
/// `SDL_LOG_PRIORITY_ERROR`, the fixed prefix `"Connection terminated: "`, then an optional `-`
/// and one or more decimal digits to the end of the string, with a value outside `int`'s range
/// rejected. `*code` is left untouched on a non-match (a different category, a different
/// priority, a different prefix - including a same-text-different-case one - no digits, trailing
/// characters after the digits, or overflow).
bool parseConnectionTerminated(int category, int priority, const char* message, int* code);
