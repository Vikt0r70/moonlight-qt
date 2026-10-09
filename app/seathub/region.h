#pragma once

// Where the sign-in field's country tag starts (`screens.md` §22, C-SIGNIN): Jordan (`JO +962`) for
// every sign-in, whatever the machine's own region or locale says. Any other country is still
// selectable in the picker. It answers a question the customer is never asked, and it returns a
// country code and nothing else.
//
// The machine's "Country or region" and the system locale used to come first (Phase 5 D-02). They
// no longer do: C-SIGNIN fixes the default, so the tag cannot start on a country the customer did
// not choose or the spec did not name.

#include <QString>

namespace SeatHubRegion {

/// The country code the tag starts on: Jordan, the one default (`SeatHubCountries::defaultIso()`).
QString initialCountryCode();

} // namespace SeatHubRegion

namespace SeatHubSystem {

/// False when Windows' "Animation effects" setting is off (Settings > Accessibility > Visual
/// effects), in which case the sign-in field's country tag appears and hides without a transition
/// (`screens.md` §22). True on every other platform and whenever the setting cannot be read.
bool animationEffectsEnabled();

} // namespace SeatHubSystem
