#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

struct InstallJournalRecord
{
    qint64 v = 0;
    QString attempt, from, to, mode, outcome, step, failureClass, rollback, state;
    QDateTime started, ended;
    qint32 code = 0;
    QJsonObject ms;
    bool elevated = false;
    QJsonObject toJson() const;
};

class InstallJournal
{
public:
    static QList<InstallJournalRecord> adopt(const QString& folder, const QDateTime& now);
};
