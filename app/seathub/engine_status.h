#pragma once

// engine_status (D-11, ADR-0072, Plan 13 Task 2): the pure matcher for Moonlight's
// "Connection status update: N" log line.
//
// `Session::clConnectionStatusUpdate` logs the status at APPLICATION/INFO
// (app/streaming/session.cpp:180-184, read-only). ControlStream.c calls that callback.
// The integer is Moonlight's own CONN_STATUS_* value:
//   0 = CONN_STATUS_OKAY
//   1 = CONN_STATUS_POOR
//
// The tee reads this without modifying the streaming engine.

/// Matches upstream's exact "Connection status update: %d" line:
/// `SDL_LOG_CATEGORY_APPLICATION`, `SDL_LOG_PRIORITY_INFO`, the fixed prefix
/// "Connection status update: ", then 0 or 1 as a decimal integer. Any other
/// category, priority, prefix, or integer value returns false and leaves *status
/// untouched.
bool parseConnectionStatusUpdate(int category, int priority, const char* message,
                                 int* status);
