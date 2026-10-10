#pragma once

#include <QObject>
#include <QTimer>
#include <functional>

/// Frozen picture watchdog (SC30 under ADR-0087 / V-STREAM).
///
/// If 10 seconds elapse without a new frame statistics tap WHILE streaming,
/// the watchdog sets its stall flag and invokes the freeze callback, triggering
/// SeatHub's own reconnect logic without billing changes.
///
/// The watchdog is disarmed when stats are hidden or when the stream is stopped.
class StreamFreezeWatchdog : public QObject
{
    Q_OBJECT

public:
    static constexpr int kFreezeTimeoutMs = 10000;

    explicit StreamFreezeWatchdog(QObject* parent = nullptr);

    using TimeoutFn = std::function<void()>;
    void onTimeout(TimeoutFn callback);

    void setStreaming(bool streaming);
    bool isStreaming() const { return m_streaming; }

    void setStatsVisible(bool visible);
    bool isStatsVisible() const { return m_statsVisible; }

    void noteFrameStatsTap();

    bool isArmed() const { return m_streaming && m_statsVisible; }
    bool isStalled() const { return m_stalled; }
    void clearStall() { m_stalled = false; }

    QTimer* timer() { return &m_timer; }
    void forceTimeout();

private slots:
    void handleTimeout();

private:
    void reevaluateArmed();

    bool m_streaming = false;
    bool m_statsVisible = true;
    bool m_stalled = false;
    QTimer m_timer;
    TimeoutFn m_callback;
};
