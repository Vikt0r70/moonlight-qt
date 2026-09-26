#include "telemetry.h"

#include "log_tee.h"
#include "path.h"
#include "seathub_version.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

// `NOMINMAX` before <windows.h>: the Win32 headers define `min`/`max` as macros, which breaks
// every `std::min`/`std::max` in a translation unit that includes them after Qt (same guard as
// `token_store.cpp`).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <sentry.h>

// This is the ONE translation unit in this fork that includes `sentry.h` (see the header's own
// comment). Everything below is what `06.3.1-RESEARCH-SPIKE-CRASHPAD.md` proved by running it.

namespace {

bool s_started = false;
bool s_lastRunCrashed = false;
QString s_lastCrashEventId;

/// Runs inside crashpad's first-chance filter, in the crashing process, on the crashing thread
/// (SPIKE Q5, `on_crash` fires before `before_send` for a crash - `sentry_backend_crashpad.cpp`
/// only calls one or the other, never both). It allocates nothing and takes no lock beyond what
/// `sentry_value_t` itself does, and returns the event unchanged: SeatHub adds nothing to a crash
/// event here.
sentry_value_t seatHubOnCrash(const sentry_ucontext_t*, sentry_value_t event, sentry_hint_t*, void*)
{
    return event;
}

/// D-11: only unexpected errors reach Sentry - unhandled exceptions, 5xx, panics and crashes. A
/// crash event's level is always `"fatal"`; anything else that reaches this callback (a captured
/// message, a non-fatal event this SDK does not currently send at all) is dropped rather than
/// uploaded. Per `on_crash`/`before_send` mutual exclusion (SPIKE Q5), a real crash never reaches
/// this function - `seatHubOnCrash` above already handled it - so this is defence in depth, not
/// the crash path.
sentry_value_t seatHubBeforeSend(sentry_value_t event, sentry_hint_t*, void*)
{
    sentry_value_t level = sentry_value_get_by_key(event, "level");
    const char* levelStr = sentry_value_as_string(level);
    if (levelStr && QString::fromUtf8(levelStr) == QLatin1String("fatal")) {
        return event;
    }
    sentry_value_decref(event);
    return sentry_value_new_null();
}

/// Registered on every init, but the facts it deposits are only meaningful after the FIRST init of
/// a process (SPIKE T7b: a re-init's `sentry_get_crashed_last_run()` reports 0, because the crash
/// marker was already consumed by the first init that read it - 0.17.1's `sentry_init()` "consumes
/// `<db>/last_crash` after caching its value"). `start()`/`startWith()` are only ever called once
/// per process in this plan (Plan 15 adds the one re-init, once a DSN arrives), so there is no
/// "first vs. later" distinction to make here yet.
void seatHubOnCrashedLastRun(const sentry_envelope_t* envelope, void*)
{
    sentry_value_t event = sentry_envelope_get_event(envelope);
    sentry_value_t eventId = sentry_value_get_by_key(event, "event_id");
    const char* idStr = sentry_value_as_string(eventId);
    s_lastRunCrashed = true;
    s_lastCrashEventId = idStr ? QString::fromUtf8(idStr) : QString();
}

/// `GetModuleFileNameW(nullptr, ...)` resolves the running executable's own path - the interface
/// `06.3.1-08-PLAN.md` names explicitly, because no `QCoreApplication` exists yet at the
/// `app/main.cpp` call site (`QCoreApplication::applicationDirPath()` needs one).
QString runningExecutableDir()
{
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        // Never expected in practice (MAX_PATH covers every real install path); an empty result
        // makes startWith()'s handler-path check fail loudly instead of guessing a wrong directory.
        return QString();
    }
    const QString exePath = QString::fromWCharArray(buffer, static_cast<int>(length));
    return QFileInfo(exePath).absolutePath();
}

} // namespace

namespace SeatHubTelemetry {

bool started()
{
    return s_started;
}

bool lastRunCrashed()
{
    return s_lastRunCrashed;
}

QString lastCrashEventId()
{
    return s_lastCrashEventId;
}

void removeLegacyDumps(const QString& dir)
{
    // D-05: every SeatHub build through 0.1.19 writes heap memory into these
    // (`MiniDumpWithIndirectlyReferencedMemory`, `app/main.cpp`'s `UnhandledExceptionHandler`).
    // Deleted, never uploaded - they predate this plan and are not crashpad reports.
    QDir logDir(dir);
    const QStringList legacy = logDir.entryList(QStringList(QStringLiteral("SeatHub-*.dmp")), QDir::Files);
    for (const QString& name : legacy) {
        QFile::remove(logDir.filePath(name));
    }
}

bool startWith(const Options& options)
{
    sentry_options_t* sentryOptions = sentry_options_new();

    // Explicit every time, even when empty: no environment variable (SENTRY_DSN or otherwise) is
    // ever allowed to inject a DSN behind this call's back (D-01: the DSN comes from the control
    // plane at run time, never compiled in and never read from the environment).
    const QByteArray dsnUtf8 = options.dsn.toUtf8();
    sentry_options_set_dsn(sentryOptions, dsnUtf8.constData());

    sentry_options_set_database_pathw(sentryOptions,
                                       reinterpret_cast<const wchar_t*>(options.databaseDir.utf16()));
    sentry_options_set_handler_pathw(sentryOptions,
                                      reinterpret_cast<const wchar_t*>(options.handlerPath.utf16()));

    const QByteArray releaseUtf8 = options.release.toUtf8();
    sentry_options_set_release(sentryOptions, releaseUtf8.constData());
    const QByteArray environmentUtf8 = options.environment.toUtf8();
    sentry_options_set_environment(sentryOptions, environmentUtf8.constData());

    // No `cache_keep` (Pitfall 4, SPIKE C.1 fact 4: it swaps the prune for `cache_max_*` instead of
    // the 2-day/8 MB default this design relies on to bound the customer's disk footprint). No
    // `require_user_consent` (SPIKE C.1 fact 3: consent defaults to "given", so leaving it unset
    // keeps every report eligible for upload - there is no consent flow to wire it to). No Qt
    // integration (build-time `SENTRY_INTEGRATION_QT=OFF`). No debug logging (`sentry_options_set_debug`
    // is never called).
    sentry_options_set_max_breadcrumbs(sentryOptions, 0);
    sentry_options_set_http_retry(sentryOptions, 1);
    sentry_options_set_on_crash(sentryOptions, seatHubOnCrash, nullptr);
    sentry_options_set_before_send(sentryOptions, seatHubBeforeSend, nullptr);
    sentry_options_set_on_crashed_last_run(sentryOptions, seatHubOnCrashedLastRun, nullptr);

    const int rv = sentry_init(sentryOptions);
    s_started = (rv == 0);
    return s_started;
}

bool start()
{
    Options options;
    options.databaseDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/crash-db");
    options.handlerPath = QDir(runningExecutableDir()).filePath(QStringLiteral("crashpad_handler.exe"));
    options.release = QStringLiteral("seathub@" SEATHUB_VERSION);
    options.environment = QStringLiteral("production");
    options.dsn = QString(); // D-10.1 is empty here; Plan 15 adds the cached-DSN read.

    // D-10: nothing here shows the customer anything, on success or failure.
    LogTee::install();
    removeLegacyDumps(Path::getLogDir());

    return startWith(options);
}

} // namespace SeatHubTelemetry
