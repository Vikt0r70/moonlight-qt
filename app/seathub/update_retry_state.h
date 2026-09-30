#pragma once

#include <QDateTime>
#include <QStringList>

class UpdateRetryState
{
public:
    int v = 1;
    QString target, lastClass;
    int attemptsFailed = 0, declines = 0;
    QDateTime firstFailedAt, lastFailedAt, nextAllowedAt;
    bool exhaustedReported = false, launched = false;
    QStringList reportedIds;

    static QString defaultPath();
    static UpdateRetryState load(const QString& path);
    bool save(const QString& path) const;
    void rememberReport(const QString& id);
};
