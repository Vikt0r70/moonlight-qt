#include "log_tee.h"

#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <utility>
#include <vector>

namespace {

bool s_installed = false;

QtMessageHandler s_previousQtHandler = nullptr;
SDL_LogOutputFunction s_previousSdlHandler = nullptr;
void* s_previousSdlUserdata = nullptr;

QMutex s_sinksMutex;
std::vector<std::pair<LogTee::SinkHandle, LogTee::Sink>> s_sinks;
LogTee::SinkHandle s_nextHandle = 1;

// Same-thread re-entry guard (see the header comment). `thread_local` rather than a mutex-guarded
// flag: two DIFFERENT threads logging at the same moment are two independent, legitimate
// dispatches, and only a thread walking back into its OWN dispatch (via a sink's or the previous
// handler's own logging) is the recursion this exists to stop.
thread_local bool s_inDispatch = false;

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
    QMutexLocker locker(&s_sinksMutex);
    const SinkHandle handle = s_nextHandle++;
    s_sinks.emplace_back(handle, std::move(sink));
    return handle;
}

void LogTee::removeSink(SinkHandle handle)
{
    if (handle == 0) {
        return;
    }
    QMutexLocker locker(&s_sinksMutex);
    s_sinks.erase(std::remove_if(s_sinks.begin(), s_sinks.end(),
                                 [handle](const std::pair<SinkHandle, Sink>& entry) {
                                     return entry.first == handle;
                                 }),
                 s_sinks.end());
}

void LogTee::clearSinksForTests()
{
    QMutexLocker locker(&s_sinksMutex);
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

    std::vector<Sink> sinksCopy;
    {
        QMutexLocker locker(&s_sinksMutex);
        sinksCopy.reserve(s_sinks.size());
        for (const std::pair<SinkHandle, Sink>& entry : s_sinks) {
            sinksCopy.push_back(entry.second);
        }
    }

    for (const Sink& sink : sinksCopy) {
        if (sink) {
            sink(level, category, priority, text);
        }
    }

    s_inDispatch = false;
}
