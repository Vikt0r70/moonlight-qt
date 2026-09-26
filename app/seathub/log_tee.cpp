#include "log_tee.h"

#include <QByteArray>
#include <QReadLocker>
#include <QReadWriteLock>
#include <QWriteLocker>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace {

bool s_installed = false;

QtMessageHandler s_previousQtHandler = nullptr;
SDL_LogOutputFunction s_previousSdlHandler = nullptr;
void* s_previousSdlUserdata = nullptr;

// WR-07: a non-recursive reader/writer lock. `dispatch()` takes it for reading around the whole
// sink-calling loop below (not only around the copy), so `removeSink()`'s write lock genuinely
// waits for every in-flight dispatch to finish before it returns - which is what the header
// promises. Two different threads logging at the same moment are two concurrent readers, which a
// `QReadWriteLock` allows; only a writer (`addSink`/`removeSink`/`clearSinksForTests`) excludes
// every reader.
//
// D-16 (06.3.1): `addSink()`/`removeSink()` called from inside a sink's own dispatch, on the
// thread dispatching it, no longer takes this lock at all - not because it would deadlock (it
// still would; `QReadWriteLock` is non-recursive, Qt's own documentation), but because that call
// is refused outright before it ever reaches here. See both functions' definitions below.
QReadWriteLock s_sinksLock;
std::vector<std::pair<LogTee::SinkHandle, LogTee::Sink>> s_sinks;

// Allocated independently of `s_sinksLock` (a plain increment under no lock at all would race two
// threads calling `addSink()` at the same moment; `std::atomic` costs nothing here).
std::atomic<LogTee::SinkHandle> s_nextHandle{ 1 };

// Same-thread re-entry guard (see the header comment). `thread_local` rather than a mutex-guarded
// flag: two DIFFERENT threads logging at the same moment are two independent, legitimate
// dispatches, and only a thread walking back into its OWN dispatch (via a sink's or the previous
// handler's own logging) is the recursion this exists to stop.
//
// D-16 (06.3.1, area 1 of the design-review trigger): also the flag `addSink()`/`removeSink()`
// check to refuse an in-dispatch call. Set and reset only through `InDispatchGuard` below now -
// IN-11 (code review 06.3-REVIEW-FIXDIFF2-fork.md): the plain `s_inDispatch = true;`/`= false;`
// statements this replaced left the flag set forever if a sink threw between the two, so every
// later `addSink()`/`removeSink()` call on that thread would refuse (or, before this change,
// defer) forever, with no diagnostic pointing at the throw that caused it. The guard's destructor
// runs on every exit path out of `dispatch()`, including one unwinding past a sink's exception,
// so the flag always comes back down.
thread_local bool s_inDispatch = false;

// D-16 (06.3.1): RAII guard for `s_inDispatch`. `dispatch()` constructs one at the top of its
// body instead of assigning the flag directly, so a sink's exception unwinding through the
// try/catch below still runs this guard's destructor on the way out.
class InDispatchGuard
{
public:
    InDispatchGuard() { s_inDispatch = true; }
    ~InDispatchGuard() { s_inDispatch = false; }
    InDispatchGuard(const InDispatchGuard&) = delete;
    InDispatchGuard& operator=(const InDispatchGuard&) = delete;
};

// D-16 (06.3.1, area 1 of the design-review trigger, SEATHUB verdict F): replaces WR-10's
// deferral queue. An `addSink()`/`removeSink()` call from inside a sink's own dispatch, on the
// thread dispatching it, is a programming error, not a case this tee accepts and applies later.
// `Q_ASSERT_X` fires first, so a debug build stops here with a precise stack; it is a no-op once
// `QT_NO_DEBUG` is defined (every suite in this tree, and every production build, are built with
// `CONFIG -= debug`), so a release build falls straight through to the lines below regardless.
// The reason line is written through whatever handler was installed before `LogTee::install()`
// ran - so it still reaches the disk log, exactly like an ordinary message would - AND straight
// to stderr, so it is visible even with no disk log configured (this is how `tst_log_tee.cpp`'s
// death tests read it back), before `std::abort()` ends the process. `std::abort()`, not
// `qFatal()`: `qFatal()` is a fast-fail Windows path crashpad's filter never sees without the WER
// module (`06.3.1-RESEARCH-SPIKE-CRASHPAD.md` T15); `abort()` raises `SIGABRT`, which crashpad's
// own handler does catch (T10, T10b) once `SeatHubTelemetry::start()` installs it - so a misuse
// in a shipped build still reaches Sentry with a precise stack, instead of the silent,
// latent use-after-free WR-12 and IN-12 left open.
void refuseReentrantMutation(const char* functionName)
{
    const QByteArray reason = QByteArrayLiteral("LogTee: ") + functionName
        + QByteArrayLiteral("() called from inside a sink's dispatch");

    Q_ASSERT_X(false, "LogTee", reason.constData());

    if (s_previousQtHandler != nullptr) {
        QMessageLogContext context;
        s_previousQtHandler(QtCriticalMsg, context, QString::fromUtf8(reason));
    }
    std::fprintf(stderr, "%s\n", reason.constData());
    std::fflush(stderr);
    std::abort();
}

} // namespace

void LogTee::install()
{
    if (s_installed) {
        return;
    }
    s_installed = true;

    s_previousQtHandler = qInstallMessageHandler(&LogTee::qtHandler);

    SDL_LogOutputFunction previousFn = nullptr;
    void* previousUserdata = nullptr;
    SDL_LogGetOutputFunction(&previousFn, &previousUserdata);
    s_previousSdlHandler = previousFn;
    s_previousSdlUserdata = previousUserdata;
    SDL_LogSetOutputFunction(&LogTee::sdlHandler, nullptr);
}

LogTee::SinkHandle LogTee::addSink(Sink sink)
{
    if (s_inDispatch) {
        // D-16: this thread is inside `dispatch()` right now - a programming error, refused
        // loudly instead of deferred (the header's own comment on `addSink()` explains why).
        // `refuseReentrantMutation()` never returns.
        refuseReentrantMutation("addSink");
    }
    const SinkHandle handle = s_nextHandle.fetch_add(1);
    QWriteLocker locker(&s_sinksLock);
    s_sinks.emplace_back(handle, std::move(sink));
    return handle;
}

void LogTee::removeSink(SinkHandle handle)
{
    if (handle == 0) {
        return;
    }
    if (s_inDispatch) {
        // D-16: same reasoning as `addSink()` above - refused loudly, not deferred.
        // `refuseReentrantMutation()` never returns.
        refuseReentrantMutation("removeSink");
    }
    // WR-07: the write lock blocks until every `dispatch()` currently holding the read lock -
    // including one calling this very `handle`'s sink on another thread right now - has finished
    // and released it. By the time `removeSink()` returns, no dispatch that started before it was
    // called is still running.
    QWriteLocker locker(&s_sinksLock);
    s_sinks.erase(std::remove_if(s_sinks.begin(), s_sinks.end(),
                                 [handle](const std::pair<SinkHandle, Sink>& entry) {
                                     return entry.first == handle;
                                 }),
                 s_sinks.end());
}

void LogTee::clearSinksForTests()
{
    QWriteLocker locker(&s_sinksLock);
    s_sinks.clear();
}

void LogTee::qtHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
    Q_UNUSED(context);

    if (s_previousQtHandler != nullptr) {
        s_previousQtHandler(type, context, msg);
    }

    if (s_inDispatch) {
        // A sink's own Qt logging (or the previous handler's) re-entering this function on this
        // same thread. The previous handler above already ran - the disk log still gets the
        // line - but no sink runs a second time for it.
        return;
    }

    LogLevel level;
    switch (type) {
    case QtDebugMsg:
        level = LogLevel::Debug;
        break;
    case QtInfoMsg:
        level = LogLevel::Info;
        break;
    case QtWarningMsg:
        level = LogLevel::Warning;
        break;
    case QtCriticalMsg:
        level = LogLevel::Critical;
        break;
    case QtFatalMsg:
        level = LogLevel::Critical;
        break;
    default:
        level = LogLevel::Info;
        break;
    }

    dispatch(level, -1, -1, msg);
}

void LogTee::sdlHandler(void* userdata, int category, SDL_LogPriority priority, const char* message)
{
    Q_UNUSED(userdata);

    if (s_previousSdlHandler != nullptr) {
        s_previousSdlHandler(s_previousSdlUserdata, category, priority, message);
    }

    if (s_inDispatch) {
        return;
    }

    LogLevel level;
    switch (priority) {
    case SDL_LOG_PRIORITY_VERBOSE:
    case SDL_LOG_PRIORITY_DEBUG:
        level = LogLevel::Debug;
        break;
    case SDL_LOG_PRIORITY_INFO:
        level = LogLevel::Info;
        break;
    case SDL_LOG_PRIORITY_WARN:
        level = LogLevel::Warning;
        break;
    case SDL_LOG_PRIORITY_ERROR:
        level = LogLevel::Error;
        break;
    case SDL_LOG_PRIORITY_CRITICAL:
    default:
        level = LogLevel::Critical;
        break;
    }

    dispatch(level, category, static_cast<int>(priority), QString::fromUtf8(message));
}

void LogTee::dispatch(LogLevel level, int category, int priority, const QString& text)
{
    // D-16 (06.3.1): RAII, not a plain assignment - see `InDispatchGuard`'s own comment above for
    // why (IN-11).
    InDispatchGuard guard;

    // WR-07: the read lock is held for the whole function, across the sink calls below and not
    // only around the copy - `removeSink()`'s write lock therefore cannot return while this
    // dispatch (on whichever thread it is running) is still calling a sink. Two different
    // threads dispatching at the same moment both hold the read lock concurrently, which
    // `QReadWriteLock` allows; a `removeSink()` call queues behind both and lets neither in
    // until it has the write lock to itself.
    QReadLocker locker(&s_sinksLock);

    std::vector<Sink> sinksCopy;
    sinksCopy.reserve(s_sinks.size());
    for (const std::pair<SinkHandle, Sink>& entry : s_sinks) {
        sinksCopy.push_back(entry.second);
    }

    for (const Sink& sink : sinksCopy) {
        if (!sink) {
            continue;
        }
        try {
            sink(level, category, priority, text);
        }
        catch (...) {
            // D-16 / IN-11: a sink's contract (header comment on `addSink()`) says it never
            // throws. This tee has no way to know what a caught exception here means for
            // whatever the sink's own caller expected, so there is nothing safe to do with it
            // beyond making sure it never crosses back out of this function - into SDL's C
            // callback, or into whatever Qt-internal code called `qtHandler()`, either of which
            // is undefined behaviour under `/EHsc` (the review's own note). `guard` above still
            // resets `s_inDispatch` when `dispatch()` returns regardless of how it returns, and
            // every OTHER sink for this message, and every later message, still get called
            // normally.
        }
    }
}
