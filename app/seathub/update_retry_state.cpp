#include "update_retry_state.h"
#include <QStandardPaths>

QString UpdateRetryState::defaultPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/update/retry-state.json");
}
UpdateRetryState UpdateRetryState::load(const QString&) { return {}; }
bool UpdateRetryState::save(const QString&) const { return false; }
void UpdateRetryState::rememberReport(const QString&) {}
