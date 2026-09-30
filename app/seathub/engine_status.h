#pragma once

// engine_status (D-11, ADR-0072, Plan 13 Task 2): the pure matcher for Moonlight's
// "Connection status update: N" log line.
//
// `ControlStream.c` calls `SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
// "Connection status update: %d", status)` (moonlight-common-c ControlStream.c:431-462,
// read-only). The integer is Moonlight's own CONN_STATUS_* value:
//   0 = CONN_STATUS_OKAY
//   1 = CONN_STATUS_POOR
//
// Reading this line through the tee is the only route that reaches the code without
// editing ControlStream.c.

#include <QtGlobal>

/// Matches upstream's exact "Connection status update: %d" line:
/// `SDL_LOG_CATEGORY_APPLICATION`, `SDL_LOG_PRIORITY_INFO`, the fixed prefix
/// "Connection status update: ", then 0 or 1 as a decimal integer. Any other
/// category, priority, prefix, or integer value returns false and leaves *status
/// untouched.
bool parseConnectionStatusUpdate(int category, int priority, const char* message,
                                 int* status);
