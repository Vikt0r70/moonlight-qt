#pragma once

// The one place the client turns an instant into a date a customer reads.
//
// `docs/spec/copy.md` §5 (dates) and ADR-0029: a customer reads a date in Jordan time, as `Thu 12
// Sep, 21:40` - the weekday and the month in English, the day without a leading zero, a 24-hour
// clock and no year. The database and the wire hold UTC; nothing else in the client converts one to
// the other, so the profile's three lists cannot drift apart on what "21:40" means. It is the sibling
// of `duration_text.h`, which does the same job for minutes.
//
// The English day and month names do not depend on the machine's language: a customer whose Windows
// is set to another language still reads the same words the copy deck shows.

#include <QDateTime>
#include <QString>

/// `instant` (any time zone) as `Thu 12 Sep, 21:40` in Asia/Amman. An invalid instant yields an empty
/// string, so a row with no usable time draws a blank cell rather than a wrong one.
QString jordanDateTimeText(const QDateTime& instant);

/// The same for an RFC 3339 timestamp as the control plane sends it (`2026-09-12T18:40:00Z`, with or
/// without fractional seconds or an offset). Anything that is not one yields an empty string.
QString jordanDateTimeText(const QString& rfc3339);
