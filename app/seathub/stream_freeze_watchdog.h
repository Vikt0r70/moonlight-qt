#pragma once

#include <QObject>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

/// Frozen picture watchdog (SC30 under ADR-0087 / V-STREAM).
///
/// If 10 seconds elapse without a new frame statistics tap WHILE streaming with the stats overlay
/// shown, the watchdog sets its stall flag and invokes the freeze callback, triggering SeatHub's
/// own reconnect logic without billing changes.
///
/// The deadline starts when the watchdog becomes armed (streaming and stats shown), not at the
/// first tap, so a stream that never delivers a single tap is caught too. Taps arrive on the
/// engine's decoder thread, and the facade thread is suspended inside the engine's SDL loop for
/// the whole stream, so the clock runs on the watchdog's own thread and every method is safe to
/// call from any thread. The freeze callback runs on that thread (or on the caller of
/// `forceTimeout()`).
///
/// The watchdog is disarmed when stats are hidden or when the stream is stopped.
class StreamFreezeWatchdog : public QObject
{
public:
    static constexpr int kFreezeTimeoutMs = 10000;

    explicit StreamFreezeWatchdog(QObject* parent = nullptr);
    ~StreamFreezeWatchdog() override;

    using TimeoutFn = std::function<void()>;
    void onTimeout(TimeoutFn callback);

    /// Test seam only: production keeps `kFreezeTimeoutMs`.
    void setTimeoutMs(int ms);

    void setStreaming(bool streaming);
    bool isStreaming() const;

    /// Mirrors the engine's real Debug overlay state (shown or hidden). Off until the stream's
    /// start-up reads the preference or the engine reports the overlay shown.
    void setStatsVisible(bool visible);
    bool isStatsVisible() const;

    void noteFrameStatsTap();

    bool isArmed() const;
    bool isStalled() const;
    void clearStall();

    void forceTimeout();

    /// Stops and joins the clock thread. The watchdog can be re-armed afterwards.
    void shutdown();

private:
    bool armedLocked() const { return m_streaming && m_statsVisible; }
    void rearmLocked();
    void run();
    void fire(std::unique_lock<std::mutex>& lock);

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_thread;
    bool m_stop = false;

    bool m_streaming = false;
    bool m_statsVisible = false;
    bool m_stalled = false;
    bool m_hasDeadline = false;
    std::chrono::milliseconds m_timeout{kFreezeTimeoutMs};
    std::chrono::steady_clock::time_point m_deadline;
    TimeoutFn m_callback;
};
