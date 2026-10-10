#include "stream_freeze_watchdog.h"

StreamFreezeWatchdog::StreamFreezeWatchdog(QObject* parent)
    : QObject(parent)
{
    m_timer.setInterval(kFreezeTimeoutMs);
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &StreamFreezeWatchdog::handleTimeout);
}

void StreamFreezeWatchdog::onTimeout(TimeoutFn callback)
{
    m_callback = std::move(callback);
}

void StreamFreezeWatchdog::setStreaming(bool streaming)
{
    if (m_streaming == streaming) {
        return;
    }
    m_streaming = streaming;
    reevaluateArmed();
}

void StreamFreezeWatchdog::setStatsVisible(bool visible)
{
    if (m_statsVisible == visible) {
        return;
    }
    m_statsVisible = visible;
    reevaluateArmed();
}

void StreamFreezeWatchdog::noteFrameStatsTap()
{
    if (isArmed()) {
        m_timer.start();
    }
}

void StreamFreezeWatchdog::forceTimeout()
{
    if (isArmed()) {
        handleTimeout();
    }
}

void StreamFreezeWatchdog::handleTimeout()
{
    if (!isArmed()) {
        return;
    }
    m_stalled = true;
    if (m_callback) {
        m_callback();
    }
}

void StreamFreezeWatchdog::reevaluateArmed()
{
    if (!isArmed()) {
        m_timer.stop();
    }
}
