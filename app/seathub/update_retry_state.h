#pragma once

#include <QDateTime>
#include <QStringList>
#include <QRandomGenerator>
#include <QVariantMap>
#include <functional>

class UpdateRetryState
{
public:
    // V34 / ADR-0070 item 5: design values, not vendor defaults.
    static constexpr int kMaxAttempts = 4; // V34 design value: total automatic attempts per target.
    static constexpr qint64 kBase = 3600; // V34 design value: one hour, seconds.
    static constexpr qint64 kCap = 86400; // V34 design value: 24 hours, seconds.
    int v = 1;
    QString target, lastClass;
    int attemptsFailed = 0, declines = 0;
    QDateTime firstFailedAt, lastFailedAt, nextAllowedAt;
    bool exhaustedReported = false, launched = false;
    // Keep manual Try again free across a process exit before its result arrives.
    bool launchedManual = false;
    QStringList reportedIds;
    QVariantMap lastOffer;

    static QString defaultPath();
    static UpdateRetryState load(const QString& path);
    bool save(const QString& path) const;
    void rememberReport(const QString& id);
    void setClock(std::function<QDateTime()> clock) { m_clock = std::move(clock); }
    void setRandomGenerator(QRandomGenerator* random) { m_random = random; }
    QDateTime now() const { return m_clock(); }
    qint64 nextDelayFor(int k) const;
    void noteFailure(const QString& failureClass, bool permanent = false, bool manual = false,
                     bool previousLaunch = false);
    void noteDecline();
    void noteSuccess();
    bool automaticAttemptAllowed(const QDateTime& now, bool streaming) const;
    bool exhausted() const;
    bool reset(const QString& nextTarget, const QString& installed);
private:
    std::function<QDateTime()> m_clock = [] { return QDateTime::currentDateTimeUtc(); };
    QRandomGenerator* m_random = QRandomGenerator::global();
    bool m_declinedThisLaunch = false, m_firstFailureThisLaunch = false;
};
