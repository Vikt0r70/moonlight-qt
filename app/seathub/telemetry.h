#pragma once

// SeatHubTelemetry (06.3.1 D-02, ADR-0044 amendment): the one call site everything sentry-native
// touches lives behind.
//
// Every `sentry_*` call in this fork stays inside `telemetry.cpp`. No other translation unit
// includes `sentry.h`, which means every other suite that links `app/seathub/*.cpp` (for example
// `tst_facade_wiring`, which compiles `seathub_client.cpp`) never needs the sentry include path or
// its import library - only `tst_telemetry` does.
//
// `start()` is called from `app/main.cpp`, directly after upstream's own
// `SetUnhandledExceptionFilter(UnhandledExceptionHandler);` line, inside the same
// `#ifdef Q_OS_WIN32` block - see the two-line comment at that call site and
// `06.3.1-RESEARCH-SEATHUB.md` § B.4 for why the order is not optional (SPIKE T2/T3: the reverse
// order loses every SEH crash to crashpad). No `QCoreApplication` exists yet at that spot, which
// is why `start()` resolves the handler path with `GetModuleFileNameW` rather than
// `QCoreApplication::applicationDirPath()` - the interface `06.3.1-08-PLAN.md` names explicitly.
//
// `start()` returning `false` means crashpad's filter never came up (SPIKE T14: a handler file
// that exists but cannot launch otherwise eats every crash of that session, including upstream's
// own writer). The caller re-installs upstream's filter in that case; this header does not do it,
// because the caller already holds the exact call `SetUnhandledExceptionFilter` needs.
//
// Never call `sentry_close()` here, including at quit (SPIKE T11: a crash after close is captured
// but without the fatal event, hooks or marker - keep the SDK up for the whole process, per
// SEATHUB § conclusion 4). Plan 15 is the one place a re-init happens, when the first DSN arrives.
//
// Plan 21 (D-14): `start()` registers `logShipperHandOff()` with `LogShipper::instance()` - the
// only place this fork builds a real `HandOff` (`log_shipper.h`) that calls `sentry_log()`.
// `logShipperHandOff()` is exposed here, rather than kept file-static in `telemetry.cpp`, so a
// test that drives `LogShipper` itself against a real (loopback) DSN - `startWith()`, unlike
// `start()`, never touches `LogShipper` on its own (SPIKE-era tests use `startWith()` so they
// never hit the real crash-db path) - can wire the exact production hand-off rather than a stub
// standing in for `sentry_log()` itself. `log_shipper.h` knows nothing about `sentry.h`; including
// it here does not widen who touches the SDK.

#include "log_shipper.h"

#include <QJsonObject>
#include <QString>

#include <optional>

namespace SeatHubTelemetry {

/// What one `sentry_init` call needs. `start()` builds the production defaults; tests build their
/// own so they never touch the real `%LOCALAPPDATA%\Seven Hills\SeatHub\crash-db` or the real
/// `%TEMP%\SeatHub-*.dmp` legacy dumps.
struct Options
{
    /// The crashpad database directory. Per-user and writable - never the current working
    /// directory, which can be `Program Files` at the `main.cpp` call site and would make
    /// `sentry_init` fail outright (06.3.1-RESEARCH-SPIKE-CRASHPAD.md § 6, Pitfall 1).
    QString databaseDir;
    /// The full path to `crashpad_handler.exe`. A path that is not a file makes the backend refuse
    /// to start (`unable to start crashpad backend, invalid handler_path`); always set explicitly.
    QString handlerPath;
    /// Empty until Plan 15 adds the cached-DSN read. `sentry_init` with an empty DSN is not fatal:
    /// it starts the handler and keeps every crash pending until a later init supplies one
    /// (SPIKE T5a/T5b).
    QString dsn;
    /// `"production"` in `start()`'s defaults. Tests set their own so a test crash's scope is
    /// never mistaken for a customer's.
    QString environment;
    /// `seathub@<SEATHUB_VERSION>`, matching the release string the rest of the release feed uses.
    QString release;
};

/// Builds the production `Options` and calls `startWith()`:
/// - `databaseDir`: `QStandardPaths::AppLocalDataLocation` + `/crash-db`
///   (`%LOCALAPPDATA%\Seven Hills\SeatHub\crash-db`). Valid at the `main.cpp` call site because
///   `Path::initialize()` and the organisation/application names have already run by line 361.
/// - `handlerPath`: `crashpad_handler.exe` beside the running executable, from
///   `GetModuleFileNameW(nullptr, ...)`.
/// - `release`: `seathub@<SEATHUB_VERSION>`. `environment`: `"production"`, `dsn`: empty, UNLESS
///   `readCache()` finds a cached `Handout` whose `dsn` still passes `acceptDsn()` (Plan 15) - then
///   both `dsn` and `environment` come from the cache instead, so a crash before this run's own
///   first handout still uploads (C.2/C.3).
/// Also installs the log tee (`LogTee::install()`, idempotent - a second call from
/// `SeatHubClient`'s constructor is harmless) and deletes every legacy `SeatHub-*.dmp` in
/// `Path::getLogDir()` before `sentry_init` runs (D-05: every build through 0.1.19 wrote heap
/// memory into those dumps; they are deleted, never uploaded).
///
/// Returns `sentry_init(...) == 0`.
bool start();

/// The `sentry_init` call itself, with an already-built `Options`. Registers `on_crash` (returns
/// the event unchanged - it runs inside the crashing process's unhandled-exception filter, so it
/// allocates nothing and takes no lock), `before_send` (drops any event whose level is not
/// `"fatal"` - D-11: only unexpected errors reach Sentry), and `on_crashed_last_run` (copies the
/// previous run's crash event id into a static; read on the FIRST init of a process only - SPIKE
/// T7b: a re-init reports 0). No `cache_keep`, no `require_user_consent`, no Qt integration (that
/// is a build-time flag, `SENTRY_INTEGRATION_QT=OFF`), no debug logging.
///
/// Never calls `sentry_close()`. Never reads or writes any environment variable that could inject
/// a DSN - `sentry_options_set_dsn` is always called explicitly, with `options.dsn` (which is
/// empty when there is none), so no ambient `SENTRY_DSN` can reach a build that has no DSN yet.
///
/// Returns `sentry_init(...) == 0`.
bool startWith(const Options& options);

/// Deletes every `SeatHub-*.dmp` in `dir` (upstream's `UnhandledExceptionHandler`,
/// `app/main.cpp`) and nothing else. Called from `start()`, before `startWith()`, so a legacy dump
/// from the pre-sentry filter is never mistaken for a crashpad report and never uploaded (D-05).
void removeLegacyDumps(const QString& dir);

/// True once a `startWith()` call has returned `sentry_init(...) == 0`. False before any call, and
/// false if the only call so far failed.
bool started();

/// The `HandOff` `start()` registers with `LogShipper::instance()`: builds `seathub.logged_at`
/// (and, when set, `session_id`/`host_id`/`seathub.trace_id`) attributes from `line` and calls
/// `sentry_log(level, body, attrs)` - never a `sentry_log_*` printf-style variant (Pitfall 6:
/// user-provided text carrying a literal `%` would otherwise be read as a format string).
/// Returns `false` (keep the line in the spool) for `SENTRY_LOG_RETURN_FAILED` or
/// `SENTRY_LOG_RETURN_DISABLED`; `true` otherwise. Safe to call whether or not `sentry_init()` has
/// ever run in this process (`SENTRY_LOG_RETURN_DISABLED` is exactly what a no-DSN or never-inited
/// process reports).
HandOff logShipperHandOff();

/// True when the process that just started crashed on its previous run, per
/// `sentry_get_crashed_last_run()` read on the first init only. Stays false for a fast-fail crash
/// (`__fastfail`, `qFatal`) with no WER module (SPIKE T9b): the crash marker is written
/// in-process, and a fast-fail crash never reaches that code.
bool lastRunCrashed();

/// The previous run's crash event id, from `on_crashed_last_run`, when `lastRunCrashed()` is true.
/// Empty otherwise.
QString lastCrashEventId();

// ---------------------------------------------------------------------------------------------
// Plan 15 (D-01, D-09, D-10, D-13, D-18): the DSN handout, its cache, identity and the test crash
// switch. Everything below still funnels through `startWith()`/`adoptFirstDsn()` above - this is
// the ONE translation unit that touches `sentry.h` (see this header's own top comment).

/// What one `GET /api/me/telemetry` (or `AgentHeartbeatAck.telemetry`) answer carries
/// (`06.3.1-RESEARCH.md` § C.1). `logs` defaults to `true` only for a caller that builds one by
/// hand; every parsed handout always carries an explicit value (`parseHandout` rejects a body
/// that omits it).
struct Handout
{
    /// The caller's own Sentry project DSN, or empty when telemetry is off. Never null in memory -
    /// a JSON `null`, an absent field or an empty string all read as this same empty QString
    /// (`06.3.1-RESEARCH.md` § C.3 rule 3).
    QString dsn;
    /// The environment the events carry (`"production"`, `"development"`, ...). Always from the
    /// handout - no client stamps its own (`06.3.1-RESEARCH.md` § C.1).
    QString environment;
    /// Whether to ship log lines to Sentry Logs. Errors and crashes follow `dsn` only.
    bool logs = true;
};

/// Parses a `TelemetryConfig` body. Returns `std::nullopt` when `environment` or `logs` is
/// missing or the wrong JSON type - a malformed answer is never a reason to change anything
/// already cached. `dsn` absent, JSON `null`, or an empty string all parse as an empty
/// `Handout::dsn` (C.3 rule 3): "off" and "not sent yet" are read the same way, which is
/// deliberate - MAIN's contract only ever means "no DSN" by any of the three.
std::optional<Handout> parseHandout(const QJsonObject& body);

/// The DSN acceptance rule (T-06.3.1-41): scheme `https` and a host ending `.ingest.de.sentry.io`.
/// A DSN that fails this is never applied, cached, or used to re-init - the handout arrives over
/// TLS from our authenticated control plane, but this is the second gate, not a proxy for the
/// first. Never logs the DSN itself, only the fact that one was rejected.
bool acceptDsn(const QString& dsn);

/// `TokenStore::defaultDirectory() + "/telemetry.json"` - the same directory the token store's
/// DPAPI blobs live in, so the installer's existing upgrade-preserve step (`token_store.cpp`'s own
/// comment) carries this file across too. Unlike the token blobs, a DSN is not a secret (D-18
/// F-1), so this is a plain JSON file, not DPAPI-wrapped, and `TokenStore::clearAll()` - which
/// only ever sweeps its own `*.dpapi` suffix - never touches it.
QString cachePath();

/// Reads and parses the cache file. `std::nullopt` when the file is absent, unreadable, or does
/// not parse as a `Handout` (the same rejection rules as `parseHandout`).
std::optional<Handout> readCache();

/// Writes `handout` to `cachePath()` atomically (a temp file, then a rename - `QSaveFile`, the
/// same primitive `token_store.cpp` uses for its own blobs), so a crash mid-write never leaves a
/// half-written cache behind.
void writeCache(const Handout& handout);

/// Deletes the cache file. Called from `applyHandout()` when a handout turns telemetry off; never
/// called from `TokenStore::clearAll()` or sign-out (C.3 rule 1: the cache is what lets a crash
/// before the *next* sign-in still report, so a sign-out that deleted it would defeat the reason
/// it exists).
void deleteCache();

/// Applies one `Handout` (`06.3.1-RESEARCH.md` § C.2/C.3): the three states, and only these.
///
///   - A DSN that `acceptDsn()` refuses is ignored outright: cache untouched, `logsEnabled()`
///     untouched, one warning logged naming the reason - never the DSN itself.
///   - An accepted, non-empty DSN is written to `cachePath()` and `logsEnabled()` is set from
///     `handout.logs`. If this process's `start()`/`startWith()` ran with no DSN, and no handout
///     has been adopted in-process yet, the DSN (and the handout's `environment`) is adopted right
///     away: `sentry_close()` then a fresh `sentry_init()` with the same options otherwise, so any
///     report pending from before this handout uploads at once (C.1 facts 2-3, 7). Every later
///     handout in the same run only rewrites the cache - there is no second re-init (C.3 rule 2):
///     crashpad's upload URL is fixed for the life of a handler process, and closing the SDK a
///     second time would drop the in-process fatal event, `on_crash` hook and crash marker for any
///     crash between the close and the next init (SPIKE T11).
///   - An empty DSN (JSON `null` or an empty string) deletes the cache and turns `logsEnabled()`
///     false for the rest of this run. `sentry_close()` is never called for this case (C.1 fact 7):
///     crash capture keeps whatever URL this run already has.
void applyHandout(const Handout& handout);

/// `false` with no DSN in use, or the last applied handout's `logs` was `false`. Cross-thread safe
/// - Plan 21's log shipper worker reads this from its own thread while `applyHandout()` may run on
/// the client thread.
bool logsEnabled();

/// Test-only: how many times this process has called `sentry_init()` (`startWith()` plus, at
/// most, the one re-init inside `applyHandout()`'s first-DSN adoption). Task 1's rotation test
/// uses this to prove a second `applyHandout()` with a different DSN never triggers a second
/// re-init. Harmless to call in production - it only reports a counter.
int initCallCount();

/// Test-only: how many times this process has called `sentry_close()`. Never more than 1 in a
/// process's whole life (C.1 fact 7: SeatHub never closes the SDK except inside the one first-DSN
/// re-init and at quit, and quit never reaches this code).
int closeCallCount();

// --- identity (D-09, D-13, G.4): state SeatHub keeps itself, so a test can assert it without a
// live SDK, and so the same values are available to `logsEnabled()`'s caller and to a future
// crash's tags regardless of whether the SDK actually started in this process.

/// `sentry_set_user` with `id`, plus the tag `signed_in=true`. Called where `m_accountId` is set:
/// the interactive sign-in's `fetchMe` callback, and restore's `setAccount()`.
void setUser(const QString& accountId);
/// `sentry_remove_user()`, plus the tag `signed_in=false`. Called at sign-out and on a restore
/// 401 - after this, a crash carries no account id (D-18 SV-C4).
void clearUser();
/// `sentry_set_tag` and `sentry_set_attribute` for both `session_id` and `host_id` (Plan 21's
/// shipper reads the attributes). `hostId` may be empty - `beginSession()` does not know it yet;
/// the tags/attributes for a still-empty `host_id` are removed rather than set to `""`.
void setSession(const QString& sessionId, const QString& hostId);
/// Removes both tags and attributes. Called at a session's teardown (complete or failed) and at
/// sign-out.
void clearSession();
/// `sentry_set_trace(traceId, nullptr)`. Called beside `setTraceId(randomTraceId())` in
/// `beginPlayRequest()` (D-27: one trace id per Play).
void setTrace(const QString& traceId);
/// `sentry_start_new_trace()`. Called at every site that clears the control plane's own trace id.
void clearTrace();

QString currentUserId();
QString currentSessionId();
QString currentHostId();
QString currentTraceId();
/// True from `setUser()` until `clearUser()`. False before any sign-in in this process, so a
/// pre-sign-in crash's tag matches (D-18 SV-C4).
bool signedIn();

/// Reads `SEATHUB_TEST_CRASH` once per process (D-04, D-10): `"1"` calls `sentry_crash()`,
/// `"fastfail"` calls `__fastfail(FAST_FAIL_FATAL_APP_EXIT)`, `"qfatal"` calls `qFatal(...)`; any
/// other value (unset, empty, anything else) returns without doing anything. Only the owner sets
/// this, for the live test (T-06.3.1-44: accepted risk). Called from `beginSession()`.
void maybeTestCrash();

} // namespace SeatHubTelemetry
