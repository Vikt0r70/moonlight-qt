#pragma once

// The name the engine titles the stream window with (Phase 5 owner answer OD-13, copy string C9;
// CUST-01, `ADR-0055` item 4).
//
// The engine builds the SDL window's title from the computer record it is handed: that record's
// `name` plus ` - SeatHub` (`ADR-0046`). The record is made from the host's own `serverinfo` reply,
// so left alone the name in the taskbar and the alt-tab switcher would be whatever the rig calls
// itself - the one thing a customer must never read (CUST-01). This client's own code replaces it
// before the engine ever sees the record, so the window is titled `Playing - SeatHub`. The engine's
// title code is not touched, and the exception that lets its title literal differ from upstream is
// not widened: if this bridge could not reach the name, that would need its own record shaped like
// `ADR-0046`, not a second edit of `session.cpp`.
//
// Header-only and free of the engine, so a test can prove the assignment against any record with a
// `name` - which is what "whatever the host reports" means - without linking the backend.

#include <QString>

namespace SeatHubStreamWindow {

/// The fixed word: the whole of what the window's name says.
inline QString neutralName()
{
    return QStringLiteral("Playing");
}

/// Sets the record's name to the neutral word, whatever the host reported. `Computer` is the engine's
/// `NvComputer` in production; anything with an assignable `name` will do.
template <typename Computer>
void applyNeutralName(Computer* computer)
{
    if (computer != nullptr) {
        computer->name = neutralName();
    }
}

} // namespace SeatHubStreamWindow
