#include "countries.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(seathubCountries, "seathub.countries")

namespace {

// `ISO 3166-1 alpha-2`, matching the website's `DEFAULT_COUNTRY`.
const char* kDefaultIso = "JO";

QVariantList load()
{
    QVariantList rows;

    QFile file(QStringLiteral(":/seathub/countries.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(seathubCountries) << "the bundled country list is missing";
        return rows;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!doc.isArray()) {
        qCWarning(seathubCountries) << "the bundled country list is not a JSON array:"
                                    << parseError.errorString();
        return rows;
    }

    for (const QJsonValue& value : doc.array()) {
        const QJsonObject row = value.toObject();
        const QString iso = row.value(QStringLiteral("iso")).toString();
        const QString name = row.value(QStringLiteral("name")).toString();
        const QString dial = row.value(QStringLiteral("dial")).toString();
        if (iso.size() != 2 || name.isEmpty() || !dial.startsWith(QLatin1Char('+'))) {
            // A malformed row is skipped, not guessed at; the parity test counts the rows.
            continue;
        }
        rows.append(QVariantMap{{QStringLiteral("iso"), iso},
                                {QStringLiteral("name"), name},
                                {QStringLiteral("dial"), dial}});
    }
    return rows;
}

} // namespace

namespace SeatHubCountries {

QVariantList all()
{
    static const QVariantList rows = load();
    return rows;
}

QString defaultIso()
{
    return QString::fromLatin1(kDefaultIso);
}

QVariantMap find(const QString& iso)
{
    const QString wanted = iso.toUpper();
    for (const QVariant& row : all()) {
        const QVariantMap map = row.toMap();
        if (map.value(QStringLiteral("iso")).toString() == wanted) {
            return map;
        }
    }
    return QVariantMap();
}

bool contains(const QString& iso)
{
    return !find(iso).isEmpty();
}

} // namespace SeatHubCountries
