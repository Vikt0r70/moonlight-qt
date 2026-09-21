#pragma once

// Where the sign-in field's country tag starts (Phase 5 D-02, `screens.md` §22): the machine's own
// region, then the system locale's territory, then the default the website uses. It answers a
// question the customer is never asked, and it returns a country code and nothing else.
//
// The machine's region is the Windows "Country or region" setting, which is a different setting
// from the display language: a Jordanian on an English Windows still reads `JO`. A code that is not
// a row in the bundled list (`countries.h`) is treated as no answer, so the tag can never start on a
// country the picker does not know.

#include <QString>

namespace SeatHubRegion {

/// The first of `machineRegion`, `localeRegion` that is a row in the bundled list (compared
/// case-insensitively, returned upper-case); the default country when neither is. Pure, so the
/// order is asserted without touching the machine.
QString pick(const QString& machineRegion, const QString& localeRegion);

/// The Windows "Country or region" as an ISO 3166-1 alpha-2 code, or empty when the machine does not
/// report one (or reports a region that is not a country, such as `001`).
QString machineRegion();

/// The system locale's territory as an ISO 3166-1 alpha-2 code, or empty.
QString localeRegion();

/// `pick(machineRegion(), localeRegion())`: the country code the tag starts on.
QString initialCountryCode();

} // namespace SeatHubRegion
