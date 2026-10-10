#include "stream_freeze_watchdog.h"

StreamFreezeWatchdog::StreamFreezeWatchdog(QObject* parent)
    : QObject(parent)
{
}

StreamFreezeWatchdog::~StreamFreezeWatchdog()
{
    shutdown();
}

void StreamFreezeWatchdog::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
        m_hasDeadline = false;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stop = false;
}

void StreamFreezeWatchdog::onTimeout(TimeoutFn callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callback = std::move(callback);
}

void StreamFreezeWatchdog::setTimeoutMs(int ms)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_timeout = std::chrono::milliseconds(ms);
}

bool StreamFreezeWatchdog::isStreaming() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_streaming;
}

bool StreamFreezeWatchdog::isStatsVisible() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_statsVisible;
}

bool StreamFreezeWatchdog::isArmed() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return armedLocked();
}

bool StreamFreezeWatchdog::isStalled() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stalled;
}

void StreamFreezeWatchdog::clearStall()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stalled = false;
}

// Called with the mutex held after any change to the armed state: a newly armed watchdog gets a
// full timeout from now, a disarmed one has no deadline.
void StreamFreezeWatchdog::rearmLocked()
{
    if (!armedLocked()) {
        m_hasDeadline = false;
        return;
    }
    m_deadline = std::chrono::steady_clock::now() + m_timeout;
    m_hasDeadline = true;
    if (!m_thread.joinable()) {
        m_thread = std::thread(&StreamFreezeWatchdog::run, this);
    }
}

void StreamFreezeWatchdog::setStreaming(bool streaming)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_streaming == streaming) {
            return;
        }
        const bool wasArmed = armedLocked();
        m_streaming = streaming;
        if (wasArmed == armedLocked()) {
            return;
        }
        rearmLocked();
    }
    m_cv.notify_all();
}

void StreamFreezeWatchdog::setStatsVisible(bool visible)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_statsVisible == visible) {
            return;
        }
        const bool wasArmed = armedLocked();
        m_statsVisible = visible;
        if (wasArmed == armedLocked()) {
            return;
        }
        rearmLocked();
    }
    m_cv.notify_all();
}

void StreamFreezeWatchdog::noteFrameStatsTap()
{
    // The worker is already waiting on the older (earlier) deadline; it re-reads the pushed one
    // when it wakes, so no notification is needed here.
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!armedLocked() || m_stalled) {
        return;
    }
    m_deadline = std::chrono::steady_clock::now() + m_timeout;
    m_hasDeadline = true;
}

void StreamFreezeWatchdog::forceTimeout()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (armedLocked()) {
        fire(lock);
    }
}

// Mutex held on entry and on return; released around the callback.
void StreamFreezeWatchdog::fire(std::unique_lock<std::mutex>& lock)
{
    m_stalled = true;
    m_hasDeadline = false;
    const TimeoutFn callback = m_callback;
    lock.unlock();
    if (callback) {
        callback();
    }
    lock.lock();
}

void StreamFreezeWatchdog::run()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_stop) {
        if (!armedLocked() || !m_hasDeadline) {
            m_cv.wait(lock);
            continue;
        }
        const auto deadline = m_deadline;
        m_cv.wait_until(lock, deadline);
        if (m_stop) {
            break;
        }
        if (armedLocked() && m_hasDeadline && std::chrono::steady_clock::now() >= m_deadline) {
            fire(lock);
        }
    }
}
