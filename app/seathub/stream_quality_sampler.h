#pragma once
#include "stream_stats.h"
#include <QElapsedTimer>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QTimer>
#include <atomic>

// ADR-0072: bounded render-thread feed; timer/emission on the network thread.
class StreamQualitySampler : public QObject
{
    Q_OBJECT
public:
    static constexpr int kWindowMs = 60000;
    static constexpr int kMaxSamplesPerWindow = 120;
    static constexpr double kRttThresholdMs = 80.0;
    static constexpr int kRttInCount = 5;
    static constexpr int kRttOutCount = 10;
    static constexpr int kBadCapPerPlay = 20;
    static constexpr int kBadSpacingMs = 30000;
    explicit StreamQualitySampler(QObject* parent = nullptr);
    void setWindowMs(int ms);
    void start(const QString& sessionId);
    void finish();
    void noteConnectionStatus(int status);
    void feed(const VideoStats& stats);
    // Snapshot and detach under the mutex; aggregation happens outside it.
    QJsonObject takeWindow(bool partial);
    int rollups() const { return m_rollups.load(); }
signals:
    void rollupEmitted();
private:
    bool onOwnThread() const;
    void tick();
    void emitWindow(bool partial);
    QTimer* m_timer;
    int m_windowMs = kWindowMs;
    QElapsedTimer m_windowClock;
    QMutex m_mutex;
    QVector<VideoStats> m_buffer;
    std::atomic<int> m_dropped{0};
    std::atomic<bool> m_running{false};
    std::atomic<int> m_rollups{0};
};
