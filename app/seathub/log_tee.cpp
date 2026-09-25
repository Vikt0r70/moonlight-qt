#include "log_tee.h"

#include <QReadLocker>
#include <QReadWriteLock>
#include <QWriteLocker>

#include <algorithm>
#include <atomic>
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
// WR-10 (code review 06.3-REVIEW-fork.md, second review pass): `QReadWriteLock` is
// non-recursive (Qt's own documentation) - a thread that already holds it for reading, as every
// thread running `dispatch()` below does for the whole sink-calling loop, cannot also take it
// for writing without deadlocking itself. `addSink()`/`removeSink()` called from inside a sink
// therefore never take this lock at all on that thread; see `s_deferredSinkOps` below.
QReadWriteLock s_sinksLock;
std::vector<std::pair<LogTee::SinkHandle, LogTee::Sink>> s_sinks;

// WR-10: allocated independently of `s_sinksLock` (a plain increment under no lock at all would
// race two threads calling `addSink()` at the same moment; `std::atomic` costs nothing here and
// means a handle is always valid the instant `addSink()` returns, on the reentrant path exactly
// as much as the ordinary one - `s_deferredAdds` below needs a real handle to hand back before
// the entry it names has actually been inserted into `s_sinks`).
std::atomic<LogTee::SinkHandle> s_nextHandle{ 1 };

// Same-thread re-entry guard (see the header comment). `thread_local` rather than a mutex-guarded
// flag: two DIFFERENT threads logging at the same moment are two independent, legitimate
// dispatches, and only a thread walking back into its OWN dispatch (via a sink's or the previous
// handler's own logging) is the recursion this exists to stop.
thread_local bool s_inDispatch = false;

// WR-10: one `addSink()`/`removeSink()` call made from inside a sink running on THIS thread,
// while `s_inDispatch` is true here - queued rather than applied immediately (see both
// functions' header comments for why), and drained by `dispatch()` itself, on this same thread,
// in the order the calls were made, the moment it releases the read lock the deferred write
// would otherwise deadlock against. One ordered queue, not a separate list per operation kind:
// a sink that both adds and removes (or adds a handle it then removes) within the same dispatch
// must see those two calls applied in the order it made them, not one kind before the other.
// `thread_local`: each thread's own in-flight dispatch (if it has one at all - most threads
// never call either function reentrantly) only ever needs to remember its own deferred requests.
struct DeferredSinkOp
{
    enum class Kind { Add, Remove } kind;
    LogTee::SinkHandle handle = 0;
    LogTee::Sink sink; // only meaningful for Kind::Add
};
thread_local std::vector<DeferredSinkOp> s_deferredSinkOps;

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
    const SinkHandle handle = s_nextHandle.fetch_add(1);
    if (s_inDispatch) {
        // WR-10: this thread is inside `dispatch()` right now, holding the read lock for the
        // whole sink-calling loop below - taking the write lock here would deadlock this thread
        // against itself. Deferred instead: applied by `dispatch()`, on this same thread, the
        // moment it releases that read lock.
        s_deferredSinkOps.push_back(
            DeferredSinkOp{ DeferredSinkOp::Kind::Add, handle, std::move(sink) });
        return handle;
    }
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
        // WR-10: same reasoning as `addSink()` above - deferred, not applied here, and not a
        // wait either (unlike the cross-thread case below, this thread cannot be waiting on
        // itself). `dispatch()` applies it once it releases the read lock this thread is
        // currently holding.
        s_deferredSinkOps.push_back(DeferredSinkOp{ DeferredSinkOp::Kind::Remove, handle, Sink() });
        return;
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
    s_inDispatch = true;

    {
        // WR-07: the read lock is held for the whole block, across the sink calls below and not
        // only around the copy - `removeSink()`'s write lock therefore cannot return while this
        // dispatch (on whichever thread it is running) is still calling a sink. Two different
        // threads dispatching at the same moment both hold the read lock concurrently, which
        // `QReadWriteLock` allows; a `removeSink()` call queues behind both and lets neither in
        // until it has the write lock to itself.
        //
        // WR-10: scoped to this block, ending before the deferred-op block below, on purpose - a
        // sink that calls `addSink()`/`removeSink()` on ITS OWN thread while this loop is
        // running never takes this lock at all (a non-recursive `QReadWriteLock` would deadlock
        // that thread against its own read lock here); it defers instead, and the deferred queue
        // is only safe to apply once this read lock has actually been released.
        QReadLocker locker(&s_sinksLock);

        std::vector<Sink> sinksCopy;
        sinksCopy.reserve(s_sinks.size());
        for (const std::pair<SinkHandle, Sink>& entry : s_sinks) {
            sinksCopy.push_back(entry.second);
        }

        for (const Sink& sink : sinksCopy) {
            if (sink) {
                sink(level, category, priority, text);
            }
        }
    }

    // WR-10 (code review 06.3-REVIEW-fork.md, second review pass): whatever this thread's own
    // sinks asked `addSink()`/`removeSink()` for, reentrantly, while the read lock above was
    // held - applied now, in the order the calls were made, on this same thread, now that the
    // read lock is released and taking the write lock here cannot deadlock against it.
    if (!s_deferredSinkOps.empty()) {
        std::vector<DeferredSinkOp> ops;
        ops.swap(s_deferredSinkOps);
        QWriteLocker locker(&s_sinksLock);
        for (DeferredSinkOp& op : ops) {
            if (op.kind == DeferredSinkOp::Kind::Remove) {
                s_sinks.erase(std::remove_if(s_sinks.begin(), s_sinks.end(),
                                             [&op](const std::pair<SinkHandle, Sink>& entry) {
                                                 return entry.first == op.handle;
                                             }),
                             s_sinks.end());
            }
            else {
                s_sinks.emplace_back(op.handle, std::move(op.sink));
            }
        }
    }

    s_inDispatch = false;
}
