#include "install_journal.h"

QList<InstallJournalRecord> InstallJournal::adopt(const QString&, const QDateTime&)
{
    return {};
}

QJsonObject InstallJournalRecord::toJson() const
{
    return {};
}
