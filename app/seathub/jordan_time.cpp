#include "jordan_time.h"

#include <QLocale>
#include <QTimeZone>

namespace {

// Jordan's zone. Resolved once. Windows builds of Qt read the system's own zone data, which knows
// `Asia/Amman`; if a machine did not, the fallback below is Jordan's fixed offset since 28 October
// 2022 (UTC+3, no daylight saving), which is still Jordan time for every recent row, where the
// machine's own zone never would be.
QTimeZone jordanZone()
{
    static const QTimeZone zone = []() {
        const QTimeZone amman(QByteArrayLiteral("Asia/Amman"));
        return amman.isValid() ? amman : QTimeZone::fromSecondsAheadOfUtc(3 * 60 * 60);
    }();
    return zone;
}

} // namespace

QString jordanDateTimeText(const QDateTime& instant)
{
    if (!instant.isValid()) {
        return QString();
    }
    // The C locale, not the machine's: `ddd` and `MMM` are then always `Thu` and `Sep`.
    return QLocale::c().toString(instant.toTimeZone(jordanZone()),
                                 QStringLiteral("ddd d MMM, HH:mm"));
}

QString jordanDateTimeText(const QString& rfc3339)
{
    if (rfc3339.isEmpty()) {
        return QString();
    }
    QDateTime instant = QDateTime::fromString(rfc3339, Qt::ISODate);
    // A timestamp with no zone at all is not RFC 3339, but every instant the database holds is UTC
    // (AGENTS.md), so it is read as UTC; Qt would otherwise read it as the machine's local time.
    if (instant.isValid() && instant.timeSpec() == Qt::LocalTime) {
        instant.setTimeZone(QTimeZone::utc());
    }
    return jordanDateTimeText(instant);
}
