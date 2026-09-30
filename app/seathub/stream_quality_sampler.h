#pragma once
#include "stream_stats.h"
#include <QElapsedTimer>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QTimer>
#include <atomic>
#include <functional>

// ADR-0072: bounded render-thread feed; timer/emission on the network thread.
class StreamQualitySampler : public QObject
{
    Q_OBJECT
public:
    static constexpr int kWindowMs = 60000;
    static constexpr int kMaxSamplesPerWindow = 120;
    static constexpr int kRecentSamples = 10;
    static constexpr double kRttThresholdMs = 80.0;
    static constexpr int kRttInCount = 5;
    static constexpr int kRttOutCount = 10;
    static constexpr int kBadCapPerPlay = 20;
    static constexpr int kBadSpacingMs = 30000;
    using Clock = std::function<qint64()>;
    explicit StreamQualitySampler(QObject* parent = nullptr, Clock clock = {});
    void setWindowMs(int ms);
    // The same Play may restart its stream after reconnect; caps and totals survive that gap.
    void start(const QString& playId);
    void finish();
    void noteConnectionStatus(int status);
    void feed(const VideoStats& stats);
    // Snapshot and detach under the mutex; aggregation happens outside it.
    QJsonObject takeWindow(bool partial);
    int rollups() const { return m_rollups.load(); }
    int badEpisodes() const { return m_badEpisodes.load(); }
    double badSeconds() const;
    double samplingGapS() const;
signals:
    void rollupEmitted();
private:
    bool onOwnThread() const;
    void tick();
    void emitWindow(bool partial);
    qint64 nowMs() const;
    void noteRttState(bool poor);
    void updateQuality(const QString& reason);
    void closeBadEpisode();
    Clock m_clock;
    QString m_playId;
    bool m_hasPlay = false;
    QElapsedTimer m_playClock;
    qint64 m_clockOrigin = 0;
    std::atomic<qint64> m_finishedMs{0};
    std::atomic<quint64> m_generation{0};
    QTimer* m_timer;
    int m_windowMs = kWindowMs;
    std::atomic<qint64> m_windowStartMs{0};
    QMutex m_mutex;
    QVector<VideoStats> m_buffer;
    QVector<VideoStats> m_recent;
    int m_rttIn = 0;
    int m_rttOut = 0;
    bool m_feedRttPoor = false;
    bool m_enginePoor = false;
    bool m_rttPoor = false;
    QString m_badReason;
    int m_badLines = 0;
    qint64 m_lastBadLineMs = -kBadSpacingMs;
    std::atomic<int> m_badEpisodes{0};
    std::atomic<double> m_badSeconds{0};
    std::atomic<qint64> m_badSinceMs{-1};
    std::atomic<qint64> m_samples{0};
    std::atomic<int> m_dropped{0};
    std::atomic<bool> m_running{false};
    std::atomic<int> m_rollups{0};
};
