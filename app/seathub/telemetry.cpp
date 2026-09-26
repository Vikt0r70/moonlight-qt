#include "telemetry.h"

#include "log_shipper.h"
#include "log_tee.h"
#include "path.h"
#include "seathub_version.h"
#include "token_store.h"

#include <QByteArray>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <atomic>

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

Q_LOGGING_CATEGORY(seathubTelemetry, "seathub.telemetry")

namespace {

bool s_started = false;
bool s_lastRunCrashed = false;
QString s_lastCrashEventId;

// --- Plan 15: rotation/re-init bookkeeping and the log-shipper kill switch ---------------------

/// The `Options` the process's very first `startWith()` call used. `applyHandout()`'s one re-init
/// (`adoptFirstDsn`, below) rebuilds from this rather than from scratch, so the database and
/// handler paths, release and callbacks never drift between the first init and the re-init
/// (SEATHUB § C.2's code sketch: "the same options plus the DSN").
SeatHubTelemetry::Options s_lastOptions;
/// True when this process's `startWith()` ran with an empty `dsn`. Only such a process ever
/// re-inits (C.3 rule 2); a process that started with a cached DSN keeps it for the whole run.
bool s_startedWithNoDsn = false;
/// True once this process has adopted a first DSN in-process. Guards the one re-init so a second,
/// later `applyHandout()` in the same run only rewrites the cache (C.3 rule 2).
bool s_adoptedFirstDsn = false;
/// Test-only counters, exposed through `initCallCount()`/`closeCallCount()`.
int s_initCount = 0;
int s_closeCount = 0;
/// `false` with no DSN in use, or the last handout's `logs` was `false`. Plan 21's shipper worker
/// reads this from its own thread while `applyHandout()` may run on the client thread.
std::atomic<bool> s_logsEnabled{ false };

// --- identity (D-09, D-13, G.4): read and written only from `SeatHubClient`'s own (client)
// thread - see `setUser()`'s own comment - so plain statics need no synchronization here, unlike
// `s_logsEnabled` above.
QString s_currentUserId;
QString s_currentSessionId;
QString s_currentHostId;
QString s_currentTraceId;
bool s_signedIn = false;

/// Task 3 (D-04, D-10): `SEATHUB_TEST_CRASH` is read once per process - the FIRST call to
/// `maybeTestCrash()` decides everything; every later call, from the same or a later
/// `beginSession()`, is a no-op regardless of what the variable now holds.
bool s_testCrashChecked = false;

/// Plan 21 (D-14): recomputes `LogShipper::setCanShip()` from the two facts that decide it -
/// signed in (`s_signedIn`, read from the client thread, same as every other identity read here)
/// and `s_logsEnabled` (a DSN in use and the handout's `logs` flag - `s_logsEnabled` is already
/// `false` whenever there is no DSN in use, see its own comment above). Called from `setUser()`,
/// `clearUser()` and `applyHandout()` - every site that changes either input.
void updateCanShip()
{
    LogShipper::instance().setCanShip(s_signedIn && s_logsEnabled.load());
}

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

/// Plan 21 (D-14): the exact `sentry_level_t` a `LogLevel` maps to. Logs and events use
/// independent level enums in sentry-native - this mapping has no bearing on `before_send`'s own
/// fatal-only event filter above, which never sees a log at all (`before_send_log` is separate).
sentry_level_t seatHubLogLevel(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug:
        return SENTRY_LEVEL_DEBUG; // never reached in practice - LogShipper filters debug (D-14).
    case LogLevel::Info:
        return SENTRY_LEVEL_INFO;
    case LogLevel::Warning:
        return SENTRY_LEVEL_WARNING;
    case LogLevel::Error:
        return SENTRY_LEVEL_ERROR;
    case LogLevel::Critical:
        return SENTRY_LEVEL_FATAL;
    }
    return SENTRY_LEVEL_INFO;
}

/// Plan 21/Pitfall 5: reads the ORIGINAL capture time this line's own attribute carries
/// (`seathub.logged_at`, set by `logShipperHandOff()` below) and copies it into the log's own
/// `timestamp` - `apply_attributes()` (sentry_logs.c) has already stamped `timestamp` with NOW by
/// the time this callback runs, which is the replay time for anything that waited in the spool,
/// not when it was actually logged. Also re-runs `LogShipper::scrub()` over the body (SEATHUB §
/// G.3: a defensive second pass, after the worker's own primary scrub) - `LogShipper::scrub()` is
/// static and pure, so calling it twice costs nothing but a second regex pass.
sentry_value_t seatHubRestoreTimestampAndScrubLog(sentry_value_t log, void*)
{
    const sentry_value_t attributes = sentry_value_get_by_key(log, "attributes");
    const sentry_value_t loggedAtAttribute
        = sentry_value_get_by_key(attributes, "seathub.logged_at");
    const sentry_value_t loggedAtValue = sentry_value_get_by_key(loggedAtAttribute, "value");
    if (!sentry_value_is_null(loggedAtValue)) {
        sentry_value_set_by_key(
            log, "timestamp", sentry_value_new_double(sentry_value_as_double(loggedAtValue)));
    }

    const char* bodyUtf8 = sentry_value_as_string(sentry_value_get_by_key(log, "body"));
    const QString scrubbed
        = LogShipper::scrub(QString::fromUtf8(bodyUtf8 ? bodyUtf8 : ""));
    const QByteArray scrubbedUtf8 = scrubbed.toUtf8();
    sentry_value_set_by_key(log, "body", sentry_value_new_string(scrubbedUtf8.constData()));

    return log;
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

/// The one place every `sentry_init()` call in this process goes through: `startWith()`'s first
/// call and the one re-init inside `adoptFirstDsn()`, below. `s_initCount` therefore counts every
/// call regardless of caller (Task 1's rotation test reads it through `initCallCount()`).
bool initSentry(const SeatHubTelemetry::Options& options)
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
    // Plan 21/D-14: restores the line's own original time and re-scrubs the body.
    sentry_options_set_before_send_log(sentryOptions, seatHubRestoreTimestampAndScrubLog, nullptr);

    const int rv = sentry_init(sentryOptions);
    ++s_initCount;
    return rv == 0;
}

/// Plan 15 (C.2/C.3 rule 2): the ONE re-init this process ever performs, called from
/// `applyHandout()` only when this process started with no DSN and no handout has been adopted
/// yet. Reuses every option `start()`/`startWith()` was first called with - only `dsn` and
/// `environment` change - so the database/handler paths, release and callbacks stay identical
/// between the first init and this one (SEATHUB § C.2's code sketch: "the same options plus the
/// DSN"; `environment` also follows the handout here, per RESEARCH.md § C.1: "the environment
/// ... always from the handout, for every client").
void adoptFirstDsn(const QString& dsn, const QString& environment)
{
    sentry_close();
    ++s_closeCount;
    SeatHubTelemetry::Options options = s_lastOptions;
    options.dsn = dsn;
    options.environment = environment;
    s_started = initSentry(options);
    s_lastOptions = options;

    // CR-01 fix: `sentry_close()`'s own scope cleanup (verified against the vendored
    // `sentry_scope.c`'s `sentry__scope_cleanup()`) wipes every `sentry_set_user`/
    // `sentry_set_tag`/`sentry_set_attribute`/`sentry_set_trace` call made before this re-init.
    // The `s_current*` statics were never touched by that wipe - only the SDK's own copy was - so
    // replaying them onto the freshly-initialised scope is enough to make a crash or log line
    // captured after this point still carry the account, session, host and trace id that were set
    // before the re-init (D-09, D-03). A process with none of these set yet (no identity before
    // its first Play) replays nothing, matching what a fresh sentry_init() would already look
    // like.
    if (s_started) {
        if (s_signedIn) {
            SeatHubTelemetry::setUser(s_currentUserId);
        }
        if (!s_currentSessionId.isEmpty() || !s_currentHostId.isEmpty()) {
            SeatHubTelemetry::setSession(s_currentSessionId, s_currentHostId);
        }
        if (!s_currentTraceId.isEmpty()) {
            SeatHubTelemetry::setTrace(s_currentTraceId);
        }
    }
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

HandOff logShipperHandOff()
{
    return [](const ShippedLine& line) {
        sentry_value_t attrs = sentry_value_new_object();
        sentry_value_set_by_key(attrs, "seathub.logged_at",
            sentry_value_new_attribute(sentry_value_new_double(line.loggedAt), nullptr));
        if (!line.sessionId.isEmpty()) {
            const QByteArray sessionUtf8 = line.sessionId.toUtf8();
            sentry_value_set_by_key(attrs, "session_id",
                sentry_value_new_attribute(
                    sentry_value_new_string(sessionUtf8.constData()), nullptr));
        }
        if (!line.hostId.isEmpty()) {
            const QByteArray hostUtf8 = line.hostId.toUtf8();
            sentry_value_set_by_key(attrs, "host_id",
                sentry_value_new_attribute(sentry_value_new_string(hostUtf8.constData()), nullptr));
        }
        if (!line.traceId.isEmpty()) {
            // A `seathub.`-prefixed attribute, never the SDK's own `trace_id` field
            // (`sentry_set_trace`, G.4's scope-level trace) - `sentry__scope_apply_to_telemetry`
            // only fills a log's `trace_id` in when it is still null, so this is purely an extra
            // attribute and never shadows the scope's live trace.
            const QByteArray traceUtf8 = line.traceId.toUtf8();
            sentry_value_set_by_key(attrs, "seathub.trace_id",
                sentry_value_new_attribute(
                    sentry_value_new_string(traceUtf8.constData()), nullptr));
        }

        // Never a `sentry_log_*` printf-style variant (Pitfall 6) - `line.body` is
        // customer/engine-produced text that may contain a literal `%`.
        const QByteArray bodyUtf8 = line.body.toUtf8();
        const log_return_value_t rv
            = sentry_log(seatHubLogLevel(line.level), bodyUtf8.constData(), attrs);
        return rv != SENTRY_LOG_RETURN_FAILED && rv != SENTRY_LOG_RETURN_DISABLED;
    };
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
    s_lastOptions = options;
    s_startedWithNoDsn = options.dsn.isEmpty();
    s_adoptedFirstDsn = false;
    s_started = initSentry(options);
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
    options.dsn = QString(); // Plan 15's cached-DSN read, right below, may replace both of these.

    // Plan 15 (C.2/C.3): a cached handout whose DSN still passes `acceptDsn()` (defence in depth -
    // a tampered or stale cache file is never trusted blind) starts this run with it, so a crash
    // before this run's own first handout still uploads. `environment` follows the same cache
    // entry - it never comes from anywhere else (RESEARCH.md § C.1). A cache with no usable DSN
    // (absent, rejected, or explicitly "off") leaves the defaults above in place.
    if (const std::optional<Handout> cached = readCache()) {
        if (!cached->dsn.isEmpty() && acceptDsn(cached->dsn)) {
            options.dsn = cached->dsn;
            options.environment = cached->environment;
            s_logsEnabled = cached->logs;
        }
    }

    // D-10: nothing here shows the customer anything, on success or failure.
    LogTee::install();
    // Plan 21 (D-14): the one process-lifetime registration - `logShipperHandOff()` is the only
    // real `HandOff` this fork ever builds. `canShip` starts false regardless of the cached
    // `s_logsEnabled` value just above - nobody is signed in yet this early (`updateCanShip()`
    // below folds both facts together once sign-in happens).
    LogShipper::instance().start(logShipperHandOff());
    updateCanShip();
    removeLegacyDumps(Path::getLogDir());

    return startWith(options);
}

// ---------------------------------------------------------------------------------------------
// Plan 15: the DSN handout, its cache, identity and the test crash switch.

std::optional<Handout> parseHandout(const QJsonObject& body)
{
    // C.3 rule 3: `environment` and `logs` must both be present, or this handout is malformed and
    // changes nothing already cached/applied.
    if (!body.contains(QStringLiteral("environment")) || !body.contains(QStringLiteral("logs"))) {
        return std::nullopt;
    }

    const QJsonValue environmentValue = body.value(QStringLiteral("environment"));
    const QJsonValue logsValue = body.value(QStringLiteral("logs"));
    if (!environmentValue.isString() || !logsValue.isBool()) {
        return std::nullopt;
    }

    Handout handout;
    handout.environment = environmentValue.toString();
    handout.logs = logsValue.toBool();

    // `dsn` absent, JSON `null`, or an empty string all read as the same empty QString (C.3 rule
    // 3: "off" and "an older server that never sent the field" are never told apart, on purpose).
    const QJsonValue dsnValue = body.value(QStringLiteral("dsn"));
    handout.dsn = dsnValue.isString() ? dsnValue.toString() : QString();

    return handout;
}

bool acceptDsn(const QString& dsn)
{
    const QUrl url(dsn);
    if (!url.isValid() || url.host().isEmpty()) {
        return false;
    }

#ifdef SEATHUB_TEST_ALLOW_LOOPBACK_DSN
    // Compile-time only, and only in tst_telemetry.pro (never app.pro): widens this rule to also
    // accept unencrypted loopback, which is what a test's own fake Sentry listener has to use.
    // T-06.3.1-41's mitigation is unaffected in any build that ships - this branch does not exist
    // in one.
    if (url.scheme() == QLatin1String("http") && url.host() == QLatin1String("127.0.0.1")) {
        return true;
    }
#endif

    return url.scheme() == QLatin1String("https")
        && url.host().endsWith(QLatin1String(".ingest.de.sentry.io"));
}

QString cachePath()
{
    // The same directory the token store's DPAPI blobs live in (`token_store.cpp`'s own comment:
    // the installer's upgrade-preserve step already carries that directory across a version bump).
    // A plain JSON file, not DPAPI-wrapped - a DSN is not a secret (D-18 F-1) - and
    // `TokenStore::clearAll()` only ever sweeps its own `*.dpapi` suffix, so it never touches this.
    return QDir(TokenStore::defaultDirectory()).filePath(QStringLiteral("telemetry.json"));
}

std::optional<Handout> readCache()
{
    QFile file(cachePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }

    return parseHandout(document.object());
}

void writeCache(const Handout& handout)
{
    const QString path = cachePath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QJsonObject object;
    object.insert(QStringLiteral("dsn"), handout.dsn);
    object.insert(QStringLiteral("environment"), handout.environment);
    object.insert(QStringLiteral("logs"), handout.logs);

    // QSaveFile: a temp file, committed with a rename - the same primitive `token_store.cpp` uses
    // for its own blobs, so a crash mid-write never leaves a half-written cache behind.
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

void deleteCache()
{
    QFile::remove(cachePath());
}

void applyHandout(const Handout& handout)
{
    if (!handout.dsn.isEmpty() && !acceptDsn(handout.dsn)) {
        // Never the DSN itself (T-06.3.1-41) - only the fact that one was rejected.
        qCWarning(seathubTelemetry) << "rejected a telemetry handout: the DSN failed the "
                                       "acceptance rule (scheme/host check)";
        return;
    }

    if (handout.dsn.isEmpty()) {
        // C.3 rule 3: off, cached as off. `sentry_close()` is never called here (C.1 fact 7) -
        // crash capture keeps whatever URL this run already has.
        deleteCache();
        s_logsEnabled = false;
        updateCanShip();
        return;
    }

    writeCache(handout);

    // C.3 rule 2: only a process that started with no DSN ever re-inits, and only once, on the
    // very first handout it ever applies. A later handout in the same run (a rotation, or this
    // same handout re-delivered) only rewrote the cache above - it takes effect at the next
    // launch, via `start()`'s cache read.
    if (started() && s_startedWithNoDsn && !s_adoptedFirstDsn) {
        // Plan 21 (SEATHUB § E.3 step 5): the worker must never hand a line to an SDK instance
        // that is mid-`sentry_close()`/mid-`sentry_init()` - `pauseHandOff()` forces every line
        // through this window into the spool instead, and is only released once the re-init has
        // finished AND `s_logsEnabled` (which feeds `canShip`, via `updateCanShip()`) already
        // reflects this handout. Flipping `s_logsEnabled` before the pause (the previous version
        // of this function did) left a window where `canShip` could already read `true` while the
        // OLD, no-DSN SDK instance was still the one `sentry_log()` would reach.
        LogShipper::instance().pauseHandOff();
        s_logsEnabled = handout.logs;
        adoptFirstDsn(handout.dsn, handout.environment);
        s_adoptedFirstDsn = true;
        updateCanShip();
        LogShipper::instance().resumeHandOff();
    }
    else {
        s_logsEnabled = handout.logs;
        updateCanShip();
    }
}

bool logsEnabled()
{
    return s_logsEnabled.load();
}

int initCallCount()
{
    return s_initCount;
}

int closeCallCount()
{
    return s_closeCount;
}

#ifdef SEATHUB_TEST_ALLOW_LOOPBACK_DSN
void captureTestMessageForTests(const QString& message)
{
    const QByteArray messageUtf8 = message.toUtf8();
    sentry_value_t event
        = sentry_value_new_message_event(SENTRY_LEVEL_FATAL, nullptr, messageUtf8.constData());
    sentry_capture_event(event);
}
#endif

// --- identity (D-09, D-13, G.4) ------------------------------------------------------------
//
// Every setter/getter below is read and written only from `SeatHubClient`'s own (client) thread
// (`seathub_client.cpp`'s call sites), so the plain `QString`/`bool` statics above need no
// synchronization - unlike `s_logsEnabled`. The `sentry_*` calls are safe to make even when this
// process never called `start()`/`startWith()` (as `tst_facade_wiring` never does): sentry-native
// keeps its scope (`sentry_scope.c`'s `g_scope`) as a lazily-initialised global, independent of
// `sentry_init()`, so every call here is a real, harmless write to that scope regardless of
// whether a client/handler exists to ever flush it anywhere.

void setUser(const QString& accountId)
{
    s_currentUserId = accountId;
    s_signedIn = true;
    const QByteArray idUtf8 = accountId.toUtf8();
    sentry_set_user(sentry_value_new_user(idUtf8.constData(), nullptr, nullptr, nullptr));
    sentry_set_tag("signed_in", "true");
    updateCanShip();
}

void clearUser()
{
    s_currentUserId.clear();
    s_signedIn = false;
    sentry_remove_user();
    sentry_set_tag("signed_in", "false");
    updateCanShip();
}

void setSession(const QString& sessionId, const QString& hostId)
{
    s_currentSessionId = sessionId;
    s_currentHostId = hostId;

    const QByteArray sessionUtf8 = sessionId.toUtf8();
    sentry_set_tag("session_id", sessionUtf8.constData());
    sentry_set_attribute("session_id",
                          sentry_value_new_attribute(sentry_value_new_string(sessionUtf8.constData()), nullptr));

    if (hostId.isEmpty()) {
        // `beginSession()` calls this before the session names a rig - nothing to set yet, and
        // nothing stale from an earlier session should linger either.
        sentry_remove_tag("host_id");
        sentry_remove_attribute("host_id");
    }
    else {
        const QByteArray hostUtf8 = hostId.toUtf8();
        sentry_set_tag("host_id", hostUtf8.constData());
        sentry_set_attribute("host_id",
                              sentry_value_new_attribute(sentry_value_new_string(hostUtf8.constData()), nullptr));
    }

    // Plan 21: publishes the snapshot every future logged line captures - never read from the
    // `s_current*` statics above directly (documented client-thread-only; `LogShipper`'s sink
    // runs on ARBITRARY threads, so that read would be a data race).
    LogShipper::instance().publishIds(s_currentSessionId, s_currentHostId, s_currentTraceId);
}

void clearSession()
{
    s_currentSessionId.clear();
    s_currentHostId.clear();
    sentry_remove_tag("session_id");
    sentry_remove_attribute("session_id");
    sentry_remove_tag("host_id");
    sentry_remove_attribute("host_id");
    LogShipper::instance().publishIds(s_currentSessionId, s_currentHostId, s_currentTraceId);
}

void setTrace(const QString& traceId)
{
    s_currentTraceId = traceId;
    const QByteArray traceUtf8 = traceId.toUtf8();
    sentry_set_trace(traceUtf8.constData(), nullptr);
    LogShipper::instance().publishIds(s_currentSessionId, s_currentHostId, s_currentTraceId);
}

void clearTrace()
{
    s_currentTraceId.clear();
    sentry_start_new_trace();
    LogShipper::instance().publishIds(s_currentSessionId, s_currentHostId, s_currentTraceId);
}

QString currentUserId()
{
    return s_currentUserId;
}

QString currentSessionId()
{
    return s_currentSessionId;
}

QString currentHostId()
{
    return s_currentHostId;
}

QString currentTraceId()
{
    return s_currentTraceId;
}

bool signedIn()
{
    return s_signedIn;
}

void maybeTestCrash()
{
    if (s_testCrashChecked) {
        return;
    }
    s_testCrashChecked = true;

    const QByteArray value = qgetenv("SEATHUB_TEST_CRASH");
    if (value == "1") {
        sentry_crash();
    }
    else if (value == "fastfail") {
        __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    }
    else if (value == "qfatal") {
        qFatal("SeatHub test crash (SEATHUB_TEST_CRASH=qfatal)");
    }
    // else: unset, empty, or any other value - ignored (D-10: nothing shown to the customer,
    // and nothing here is a reason to crash a real customer's run).
}

} // namespace SeatHubTelemetry
