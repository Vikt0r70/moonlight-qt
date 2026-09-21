#include "duration_text.h"

namespace {

// `qint64` magnitude without the undefined behaviour of `-INT64_MIN`.
quint64 magnitude(qint64 value)
{
    return value < 0 ? static_cast<quint64>(0) - static_cast<quint64>(value)
                     : static_cast<quint64>(value);
}

QString body(quint64 minutes)
{
    const quint64 hours = minutes / 60;
    const quint64 rest = minutes % 60;

    // Under an hour there is no hours part at all: `45 min`, not `0 h 45 min`. From one hour up
    // the minutes are always two digits, so a whole hour reads `3 h 00 min`.
    if (hours == 0) {
        return QStringLiteral("%1 min").arg(rest);
    }
    return QStringLiteral("%1 h %2 min").arg(hours).arg(rest, 2, 10, QLatin1Char('0'));
}

} // namespace

QString durationText(qint64 minutes)
{
    const QString text = body(magnitude(minutes));
    return minutes < 0 ? QLatin1Char('-') + text : text;
}

QString signedDurationText(qint64 minutes)
{
    if (minutes == 0) {
        return body(0);
    }
    return (minutes > 0 ? QLatin1Char('+') : QLatin1Char('-')) + body(magnitude(minutes));
}
