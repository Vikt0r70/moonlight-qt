#pragma once

// The one place the client turns minutes into words.
//
// `docs/spec/copy.md` §5 (owner answer OD-01, Phase 5 D-18): a duration is hours and minutes and
// never a raw minute count - `2 h 15 min`, `45 min`, `0 min`, and a whole hour still shows its
// minutes as `3 h 00 min`. A ledger row is signed: `+1 h 00 min`, `-45 min`. The result is
// rendered in the mono family by the caller, is never translated, and covers everything the client
// draws as a duration: the balance element, the HUD's `Credit left`, a session's length, the
// profile totals and every credit row (FLOW-09).
//
// Every client surface calls these two functions. A second formatter anywhere in `app/seathub/`
// or `app/gui/` is a defect: two of them is how "35 min" and "0 h 35 min" end up on one screen.
// The input is always a wallet or ledger fact the server sent; nothing here does arithmetic on it
// beyond splitting it into hours and minutes.

#include <QString>
#include <QtGlobal>

/// `minutes` as `2 h 15 min` / `45 min` / `0 min` / `3 h 00 min`. A negative value is printed
/// with a leading `-` and the same shape (`-45 min`, `-1 h 05 min`).
QString durationText(qint64 minutes);

/// The ledger-row form: a leading `+` for a credit, `-` for a debit, and a plain `0 min` for
/// zero (`+1 h 00 min`, `-45 min`, `0 min`).
QString signedDurationText(qint64 minutes);
