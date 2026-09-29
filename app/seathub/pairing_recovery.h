#pragma once

// Rig-side pairing-session recovery (G-06.2-2, Plan 06.2-12 Task 1).
//
// What broke: Sunshine keys a pending pairing session by the uniqueid the CLIENT supplies, and
// upstream's own client code hard-codes one shared value for that uniqueid
// (`app/backend/nvhttp.cpp:482`). Every SeatHub install on earth pairs with the same key, and a
// half-open pending session from one attempt collides with the next attempt's getservercert step:
// Sunshine answers HTTP 409 "A pairing session with this uniqueid already exists"
// (`src/nvhttp.cpp:596-620, 975-997`), the fork treated that as fatal, the rig-blame strike
// counter quarantined the rig, and failures self-perpetuated until something restarted the rig.
//
// The way out that upstream itself offers: `GET /pair?uniqueid=<id>&phrase=cancel` clears the rig's
// pending pairing session (Sunshine's `pair()` reads the `phrase` parameter and fails the session
// with "Invalid pairing request", whose handler erases the map entry - `src/nvhttp.cpp:705-713`).
// There is no dedicated /unpair route in this Sunshine build (v2026.914.233613, `src/nvhttp.cpp:
// 1691-1712`), so this request is the only client-reachable clear.
//
// This module holds the two facts every recovery path needs - the shared uniqueid and the cancel
// request - and nothing else. It is deliberately Qt-Network-only with no `app/backend/` include:
// it is linked into the pairing tests, which must build without the engine, and the one upstream
// value it needs (the control HTTP port) is pinned below with its citation.

#include <QString>

/// The pairing identity upstream sends on every pairing request. Byte-for-byte the value
/// `app/backend/nvhttp.cpp:482` hard-codes - the parity test in `tests/tst_pairing.cpp` opens
/// that file and asserts this constant still matches it, so a future change to one side without
/// the other cannot ship silently.
extern const char* const kSharedPairingUniqueId;

/// The plain-HTTP port pairing requests ride. 47989, upstream's `DEFAULT_HTTP_PORT`
/// (`app/backend/nvaddress.h:5`) and the contract's `ports.control`
/// (`docs/spec/openapi.yaml:1804-1806`). A constant here rather than `app/backend/nvaddress.h`'s
/// own because this module must not include `app/backend/` (see the file comment above).
inline constexpr int kPairingControlPort = 47989;

/// One best-effort clear of the rig's pending pairing session for `kSharedPairingUniqueId`.
///
/// Blocking: a local event loop waits up to `timeoutMs` for the rig to answer (default 3 s - a
/// clear that outlived the pairing deadline it serves would be a bug; see T-06.2-12-02 in
/// Plan 06.2-12's threat model). Any HTTP answer at all counts as success: Sunshine answers
/// "Invalid uniqueid" (HTTP 400) for the no-op clear and that is exactly what a clear after a
/// session already expired should look like. A timeout or a transport failure is a best-effort
/// miss, reported through the log only - never raised.
///
/// The address never appears in a log line, and neither does the PIN (there is no PIN on this
/// request at all): the diagnostics contract for this module is that a reader of the log learns
/// that a clear was attempted and what became of it, and nothing else (T-06.2-12-01).
///
/// Thread-safe: it builds its own `QNetworkAccessManager` and event loop on the calling thread,
/// which is a pool thread in production and the test thread in tests - the same shape upstream's
/// own pairing requests use (`app/backend/nvhttp.cpp:510-511`).
bool sendPairingCancelRequest(const QString& hostAddress, int port = kPairingControlPort,
                              int timeoutMs = 3000);
