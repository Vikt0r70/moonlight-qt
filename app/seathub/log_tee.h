#pragma once

// LogTee (ADR-0044, Plan 15 Task 1): the one place in this fork that installs the Qt message
// handler and the SDL log output function.
//
// It exists because this plan needs TWO independent readers of the engine's own log traffic -
// the end-of-stream video-stats block (`stream_stats.h`) and the termination code's log line
// (`engine_termination.h`, A-51, Task 2) - and `SDL_LogSetOutputFunction()` /
// `qInstallMessageHandler()` each REPLACE whatever was installed before them; neither chains
// automatically, so two independent call sites racing to install their own handler would leave
// only the last one in effect. `LogTee` is the single installation point: every SeatHub sink
// subscribes through `addSink()` instead of calling either function itself. The header comment on
// `install()` below says this is a hard rule, not a preference - `06.3-15-PLAN.md`'s own
// `must_haves` truth names it explicitly.
//
// It chains upstream's own handlers first, unchanged: `SDL_LogGetOutputFunction()` and
// `qInstallMessageHandler()`'s return value are whatever was installed before `install()` ran -
// `app/main.cpp`'s `sdlLogToDiskHandler`/`qtLogToDiskHandler` in production, since nothing else in
// this fork installs a competing handler (`FORK-CHANGES.md`). Calling those first, before any sink
// runs, means `SeatHub-<epoch>.log` is exactly what it was before this file existed.
//
// A `thread_local` re-entry guard stops a sink's own logging (or the previous handler's) from
// recursing back into `dispatch()`: SDL and Qt logging calls can happen on any thread, including
// moonlight-common-c's own connection thread, and a sink that logs while it is running would
// otherwise walk straight back into this same dispatch on the same thread. The previous handler
// still runs for the re-entrant message (so the disk log is unaffected); only the second,
// recursive pass through the sink list is skipped.

#include <QMessageLogContext>
#include <QString>
#include <QtGlobal>

#include <functional>

#include <SDL.h>

/// A tee'd message's normalised severity, shared between Qt and SDL's own vocabularies. For an
/// SDL message the raw `category`/`priority` `dispatch()` also carries are SDL's own enum values
/// (`SDL_LogCategory`/`SDL_LogPriority`) - `engine_termination.h`'s parser matches those exactly,
/// not this enum. For a Qt message, `category` and `priority` are both the sentinel `-1`: Qt has
/// no such concept, and `-1` cannot collide with a real SDL value (SDL's own category and
/// priority enums both start counting at 0 and 1 respectively).
enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error,
    Critical,
};

class LogTee
{
public:
    using Sink = std::function<void(LogLevel level, int category, int priority, const QString& text)>;
    /// Opaque handle from `addSink()`. Never dereferenced; pass it to `removeSink()` to
    /// unregister. `0` is never a live handle (`addSink()` starts counting at 1), so a
    /// default-constructed member can safely mean "nothing registered".
    using SinkHandle = quint64;

    /// Installs the tee as the one Qt message handler and the one SDL log output function for
    /// this process. A second call is a no-op: after the first call, `install()`'s own functions
    /// ARE "the previous handler" as far as Qt/SDL are concerned, and re-installing over them
    /// would capture that and chain the tee to itself, double-firing every future message.
    static void install();

    /// Subscribes `sink` to every future message, from any thread, until `removeSink()` is
    /// called with the returned handle. A `Sink` that captures a pointer to an object with a
    /// shorter lifetime than the process MUST be paired with a `removeSink()` call before that
    /// object is destroyed - `LogTee`'s own sink list is process-global and outlives any one
    /// caller, exactly the way `SeatHubClient`'s destructor does it.
    ///
    /// WR-10 (code review 06.3-REVIEW-fork.md, second review pass): called from inside a sink
    /// that is itself running, on the thread that is dispatching it - the same re-entry
    /// `dispatch()`'s own `thread_local` guard already detects for the Qt/SDL handler chain -
    /// this is DEFERRED rather than applied immediately. Taking the same write lock `dispatch()`
    /// is holding for reading, on the SAME thread, would deadlock that thread against itself
    /// (`QReadWriteLock` is non-recursive: a thread already holding it for reading cannot also
    /// take it for writing - Qt's own `QReadWriteLock::RecursionMode` documentation). The handle
    /// returned here is still valid immediately (allocated up front, independent of the sink
    /// list itself), and the sink is registered the moment the dispatch that is currently running
    /// on this thread returns - see `dispatch()`'s own comment for exactly where.
    static SinkHandle addSink(Sink sink);

    /// Unregisters the sink `handle` named, and - WR-07, code review 06.3-REVIEW-fork.md - waits
    /// for any dispatch already calling it on another thread to finish first. `dispatch()` holds a
    /// read lock for the whole time it is calling sinks (not only while it copies the list), and
    /// this takes the same lock for writing, which blocks until every reader has released it. The
    /// header used to promise this and not keep it: `dispatch()` copied the sink list under a
    /// plain mutex, released it, and then called the copies - a thread that had already made its
    /// copy just before this ran could still call a sink whose owner this call is meant to make
    /// safe to destroy. A handle already removed, or `0`, is a no-op.
    ///
    /// WR-10 (code review 06.3-REVIEW-fork.md, second review pass): calling this from inside a
    /// sink that is itself running, on the thread dispatching it, does NOT wait and does NOT
    /// deadlock - it is deferred instead, exactly like `addSink()` above, and applied the moment
    /// that dispatch returns. A previous revision of this comment claimed the same-thread
    /// re-entry guard "stops that from reaching here at all"; it did not - that guard is checked
    /// only inside `qtHandler()`/`sdlHandler()`, before either one ever calls `dispatch()`, and
    /// neither `addSink()` nor `removeSink()` read it at all before this fix. Nothing in this
    /// tree calls either function from inside a sink today (this defer path is currently dead
    /// code, exercised only by its own test), but a future sink now gets a real guarantee instead
    /// of a silent hang with no diagnostic.
    static void removeSink(SinkHandle handle);

    /// Test seam: forgets every registered sink. Does not touch the installed Qt/SDL handler
    /// chain - `install()` stays installed for the life of the process (see its own comment).
    static void clearSinksForTests();

private:
    static void qtHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg);
    static void sdlHandler(void* userdata, int category, SDL_LogPriority priority, const char* message);
    static void dispatch(LogLevel level, int category, int priority, const QString& text);
};
