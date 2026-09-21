#pragma once

// The country list the sign-in field's tag and picker read (Phase 5 D-02, `screens.md` §22).
//
// The data is `countries.json`, a copy of the website's list (`seathub-web/src/lib/countries.ts`)
// compiled into the binary as a Qt resource: the picker fetches nothing and works offline. The rows
// are the website's, sorted by name the way its picker shows them. A row is `iso` (ISO 3166-1
// alpha-2, the row's identity: dial codes are shared, ISO codes are not), `name` and `dial` (a
// leading `+` and the digits a number from there starts with; where a territory has its own area
// code inside the North American plan the dial code carries it, so `dial + national digits` is the
// whole E.164 number).
//
// Nothing here is a spec value: `docs/spec/README.md` records the list's contents and the Jordan
// default as the owner's to confirm, and the client stays in step with the website by copy. The test
// suite pins the row count and the default row so the two cannot drift silently.

#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace SeatHubCountries {

/// Every country, as `{iso, name, dial}` maps, in the order the picker lists them. Read once from
/// the bundled resource. Empty only if the resource is missing, which the test suite treats as a
/// failure.
QVariantList all();

/// The service area, and so the country the tag starts on when the machine says nothing usable:
/// Jordan, the same default as the website's picker.
QString defaultIso();

/// True when `iso` (any case) names a row in the list.
bool contains(const QString& iso);

/// The row for `iso`, or an empty map.
QVariantMap find(const QString& iso);

} // namespace SeatHubCountries
