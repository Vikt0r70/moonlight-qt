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

#include <QString>

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
/// - `release`: `seathub@<SEATHUB_VERSION>`. `environment`: `"production"`. `dsn`: empty (Plan 15).
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

/// True when the process that just started crashed on its previous run, per
/// `sentry_get_crashed_last_run()` read on the first init only. Stays false for a fast-fail crash
/// (`__fastfail`, `qFatal`) with no WER module (SPIKE T9b): the crash marker is written
/// in-process, and a fast-fail crash never reaches that code.
bool lastRunCrashed();

/// The previous run's crash event id, from `on_crashed_last_run`, when `lastRunCrashed()` is true.
/// Empty otherwise.
QString lastCrashEventId();

} // namespace SeatHubTelemetry
