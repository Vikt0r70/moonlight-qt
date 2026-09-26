#pragma once

// The in-stream text overlay's pure renderer (D-08 through D-17, D-20; `docs/spec/ui.md` §7).
//
// Every function here is a pure transform: given sizes, colours and strings, it returns a
// `QImage` and touches nothing else - no SDL, no `Session`, no `OverlayManager`. Plan 10 is what
// hands the returned bitmaps to the engine's overlay hook; this file exists so the whole look can
// be built and tested with no host, no Sunshine and no stream (D-17's picture sheet is the same
// idea one step further).
//
// `registerOsdFonts()` is called once from `SeatHubClient`'s constructor (Plan 14) - never from
// `app/main.cpp`, which 06.3.1 edits (RESEARCH-FORK.md §3, Pitfall 12).

#include <QFont>
#include <QImage>
#include <QList>
#include <QString>
#include <QtGlobal>

// Colour constants (`docs/spec/ui.md` §3.1). Each name is also the `Tokens.qml` property it must
// stay pinned to (Plan 20 checks the generated file mechanically, the same way
// `tst_hud_bitmap.cpp`'s `hudUsesTheGeneratedDesignTokens()` already does for the HUD's own
// colours).
constexpr QRgb kOsdLabel = 0xFFFF9A2E;       // Tokens.osdLabelDefault
constexpr QRgb kOsdLabelMedia = 0xFF2DD4BF;  // Tokens.osdLabelMediaDefault
constexpr QRgb kOsdValue = 0xFFFFFFFF;       // Tokens.osdValueDefault
constexpr QRgb kOsdOutline = 0xD9000000;     // Tokens.osdOutlineDefault (black, 0.85 alpha)
constexpr QRgb kOsdDestructive = 0xFFEF4444; // Tokens.destructiveDefault

/// Registers the bundled Open Sans SemiBold (`:/seathub/fonts/OpenSans-SemiBold.ttf`, SIL OFL
/// 1.1 - see the licence bundled beside it at `:/seathub/fonts/OFL.txt`) with Qt's font database.
/// Idempotent: a second call is a no-op that returns the same result as the first. Logs one
/// warning and returns `false` on failure; `osdFont()` still returns a usable font in that case
/// (Qt's own fallback face), it is simply not Open Sans.
bool registerOsdFonts();

/// A `QFont` for the OSD face at `pixelSize`. Uses the family `QFontDatabase::applicationFontFamilies()`
/// reported for the bundled file - never a hard-coded `"Open Sans"` literal
/// (RESEARCH-FORK.md Pitfall 7: that name silently falls back to Segoe UI on Windows when no font
/// registered under it exists). The SemiBold weight is pinned explicitly so a system-installed
/// "Open Sans" family cannot be matched at a lighter weight instead, and digits are given the
/// `tnum` OpenType feature so they never jitter (`ui.md` §4).
QFont osdFont(int pixelSize);

/// The OSD's whole size table, derived from the stream window's client height in pixels (D-14):
/// `px(base) = max(floor, round(base * H / 1080))`. See `docs/spec/ui.md` §7 and
/// `06.6-RESEARCH-FORK.md` §2.
struct OsdSizes
{
    int valuePx;
    int labelPx;
    int unitPx;
    int rowHeight;
    int gap;
    int statsMargin;
    int timeLeftInset;
    int statusPx;
    qreal outlinePx;
};

/// Computes `OsdSizes` for a stream window whose client height is `windowHeight` pixels.
OsdSizes osdSizesFor(int windowHeight);

/// One value in a stats row: an optional sub-label (for a grouped row such as LATENCY's `NET`/
/// `TOTAL`), the value text and its unit suffix. A plain aggregate - no constructors - so a
/// brace-initializer binds straight to a `const&` parameter.
struct OsdValuePart
{
    QString subLabel;
    QString value;
    QString unit;
};

/// One row of the top-left stats block: its label (`FPS`, `RES`, `LATENCY`, ...), whether it is
/// the resolution row (drawn in `kOsdLabelMedia` instead of `kOsdLabel`), and its value parts.
struct OsdStatsRow
{
    QString label;
    bool mediaLabel;
    QList<OsdValuePart> parts;
};

/// Time left's own state (D-11, D-16, D-20, D-21): whether it is visible at all, the whole
/// minutes remaining, and whether it is in its critical (red) state.
struct OsdTimeLeft
{
    bool visible;
    qint64 minutes;
    bool critical;
};

/// The stats block's label column width (`docs/spec/ui.md` §7, A-73,
/// `06.6-DECISION-OSD-LABEL-COLUMN.md`): it has no base of its own. It is as wide as the widest
/// label in `rows`, measured uppercase in the face and size it is drawn in
/// (`osdFont(osdSizesFor(windowHeight).labelPx)`), plus the gap - so every value starts at one x
/// and no label reaches it at any `windowHeight`. An empty `rows` list returns 0.
int osdLabelColumn(const QList<OsdStatsRow>& rows, int windowHeight);

/// Renders the top-left stats block for `rows` at the size class `windowHeight` selects. A null
/// image with an empty `rows` list (OD-04: nothing is drawn once every row is off).
QImage renderOsdStats(const QList<OsdStatsRow>& rows, int windowHeight);

/// Renders the bottom overlay: Moonlight's own status line (`engineText`, drawn in `engineColor`,
/// bottom-left, with no outline - D-13) and Time left (bottom-right, outlined, D-11/D-16/D-20),
/// composed into one image as wide as the window (the ADR-0045 padding technique). A null image
/// when neither is drawn.
QImage renderOsdBottom(int windowWidth, int windowHeight, const QString& engineText,
                       QRgb engineColor, const OsdTimeLeft& timeLeft);
