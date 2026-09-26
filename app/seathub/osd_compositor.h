#pragma once

// SeatHub's compositor behind the engine's one text-rasteriser hook (D-09, D-13; ADR-0045 amended
// 2026-09-26; Plan 06's `osd_renderer.*`).
//
// `OsdCompositor::rasterize` is the `Overlay::OverlayManager::TextRasterizer` callback (Plan 10's
// own hook in `overlaymanager.{h,cpp}`). Register it once, before the stream runs (Plan 14):
//
//     manager.setTextRasterizer(&OsdCompositor::rasterize, &compositor);
//
// For `OverlayStatusUpdate` it records the engine's own status line and colour, then returns
// `renderOsdBottom()`'s composition (Moonlight's message plus Time left, D-13) as an owned
// ARGB8888 surface, or `nullptr` when nothing is drawn. For `OverlayDebug` (Plan 16, D-10) it
// parses the engine's raw stats text, builds one `OsdStatsRow` per ticked label - in the order
// RES, FPS, LATENCY, then the rest in `stats_catalogue.h`'s own order - and returns
// `renderOsdStats()`'s image as an owned surface, or `nullptr` when nothing parsed or every row
// is off (OD-04, now SeatHub's own rule).
//
// Reentrant by design (`06.6-RESEARCH-FORK.md` §1.3 "Threads"): every mutable field lives behind
// one mutex, a call copies what it needs and renders outside the lock with a fresh `QImage`
// (`renderOsdBottom()`, Plan 06), and no SDL window function is ever called from here (Pitfall
// 11) - `windowWidth`/`windowHeight` are only ever set by `setWindowSize()`, never read from SDL.

#include "osd_renderer.h"
#include "streaming/video/overlaymanager.h"

#include <QMutex>
#include <QString>
#include <QStringList>

#include <SDL.h>

class OsdCompositor
{
public:
    OsdCompositor();

    /// Learns the stream window's client size (D-14). Defaults to 1920x1080 until this is
    /// called - the same base `docs/spec/ui.md` §7 sizes from.
    void setWindowSize(int width, int height);

    /// Time left's own state (D-11/D-16/D-20/D-21), fed by Plans 14 and 22.
    void setTimeLeft(const OsdTimeLeft& timeLeft);

    /// The customer's ticked stats labels (D-26): stored now, set by Plan 14, drawn by Plan 16.
    void setEnabledStatsLabels(const QStringList& labels);

    /// The `Overlay::OverlayManager::TextRasterizer` callback. `context` must be the
    /// `OsdCompositor` instance `setTextRasterizer()` was given.
    static SDL_Surface* rasterize(Overlay::OverlayType type, const char* text, bool enabled,
                                   SDL_Color color, void* context);

    /// The same bottom composition `rasterize()` publishes for `OverlayStatusUpdate`, for the
    /// HUD's own publish path (Plan 14) - both draw from the one recorded state.
    QImage composedBottom() const;

private:
    struct State
    {
        int windowWidth = 1920;
        int windowHeight = 1080;
        QString engineText;
        QRgb engineColor = qRgba(0, 0, 0, 0);
        OsdTimeLeft timeLeft{ false, 0, false };
        QStringList enabledStatsLabels;
    };

    SDL_Surface* rasterizeInstance(Overlay::OverlayType type, const char* text, bool enabled,
                                    SDL_Color color);
    /// The `OverlayDebug` half of `rasterizeInstance()` (Plan 16, D-10): parses `text` and draws
    /// the ticked stats rows, or returns `nullptr`.
    SDL_Surface* rasterizeStats(const char* text, bool enabled) const;
    State snapshot() const;

    mutable QMutex m_mutex;
    State m_state;
};
