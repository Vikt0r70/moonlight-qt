#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <SDL.h>
#include <SDL_ttf.h>

namespace Overlay {

enum OverlayType {
    OverlayDebug,
    OverlayStatusUpdate,
    OverlayMax
};

class IOverlayRenderer
{
public:
    virtual ~IOverlayRenderer() = default;

    virtual void notifyOverlayUpdated(OverlayType type) = 0;
};

class OverlayManager
{
public:
    OverlayManager();
    ~OverlayManager();

    bool isOverlayEnabled(OverlayType type);
    char* getOverlayText(OverlayType type);
    void updateOverlayText(OverlayType type, const char* text);
    int getOverlayMaxTextLength();
    void setOverlayTextUpdated(OverlayType type);
    void setOverlayState(OverlayType type, bool enabled);
    SDL_Color getOverlayColor(OverlayType type);
    int getOverlayFontSize(OverlayType type);
    SDL_Surface* getUpdatedOverlaySurface(OverlayType type);

    // SeatHub: D-28 exception, see FORK-CHANGES.md and ADR-0045.
    //
    // Publishes a caller-rendered bitmap for `type` in place of the text this class
    // rasterises itself, so a surface the caller produced (the SeatHub HUD, D-05/D-56) goes
    // through the same renderer path the text overlays use. The existing text API and every
    // other method here are untouched.
    //
    // The surface must be SDL_PIXELFORMAT_ARGB8888 and must not need locking: every
    // renderer's upload path asserts exactly that before handing the pixels to the GPU
    // (d3d11va.cpp's `notifyOverlayUpdated` uploads them straight into an immutable
    // DXGI_FORMAT_B8G8R8A8_UNORM texture with `pSysMem = surface->pixels`). A surface that
    // fails either check is refused and freed here rather than tripping an assert on a
    // render thread. The alpha channel is used as-is through SRC_ALPHA/INV_SRC_ALPHA
    // blending, so the caller must supply **straight** (non-premultiplied) alpha.
    //
    // Ownership: `surface` is consumed in every case, including refusal - the caller must
    // not free it.
    //
    // Buffer lifetime (CR-03): because delivery is asynchronous (below), the pixels must stay
    // valid until the renderer has actually taken them. The surface must therefore own its
    // pixels - `SDL_CreateRGBSurfaceWithFormat()`, or an equivalent allocation - and must not
    // be a view onto a buffer whose lifetime the caller ends when this call returns.
    // `SDL_CreateRGBSurfaceWithFormatFrom()` over a caller-owned image is exactly the
    // mistake: `D3D11VARenderer` consumes the surface inside `notifyOverlayUpdated()`, but
    // `SdlRenderer` declares no `notifyOverlayUpdated()` at all (it inherits the no-op
    // default, `renderer.h`), so it picks the surface up on its next `renderOverlay()` and
    // would read a buffer that is already gone. A renderer that consumes later, or not at
    // all, is not a bug in the renderer: this API promises only asynchronous delivery.
    //
    // Threading: same contract as the rest of this class. Safe from any thread; the surface
    // hand-off is the atomic swap the text path already uses, and the renderer notification
    // is documented as callable from an arbitrary thread. Delivery is asynchronous: the
    // renderer picks the bitmap up on its next frame.
    //
    // Returns true when the bitmap was published (the overlay is enabled and the surface was
    // accepted), false when it was refused.
    bool updateOverlaySurface(OverlayType type, SDL_Surface* surface);

    // SeatHub: D-28 exception, see FORK-CHANGES.md and ADR-0045.
    //
    // Tells this slot its SeatHub content is genuinely gone (the HUD published nothing this
    // frame, not merely a state the engine happens to be toggling) - see the `seatHubSurface`
    // field comment for why that distinction matters. After this call the engine's own text is
    // free to draw here again the next time it writes one, exactly as it could before
    // `updateOverlaySurface()` ever ran. Idempotent; safe on any thread, same as the rest of
    // this class.
    void clearSeatHubSurface(OverlayType type);

    // SeatHub: D-28 exception, see FORK-CHANGES.md and ADR-0045.
    //
    // CUST-17/D-23/D-26: sets which of the OverlayDebug lines Moonlight's own stats writer
    // (`ffmpeg.cpp`'s `stringifyVideoStats()`, untouched) may be drawn. `enabledLabels` is the
    // caller's complete current choice - not additive with a previous call - matched against the
    // text up to (not including) the first ':' on each `\n`-terminated line the engine writes.
    // The full label catalogue lives in `SettingsBridge` (D-26, `settings_bridge.cpp`'s
    // `kStatsToggles`): this class carries no copy of it and simply draws whatever subset of
    // lines it is told is enabled, in the engine's own order, with no gap left where a disabled
    // line was. An empty list disables every line - nothing is drawn at all. A raw line whose
    // label is not present in `enabledLabels` - disabled on purpose, or unrecognised because the
    // engine renamed it - is never drawn either way (T-05-49): this filter has no "draw by
    // default" case.
    //
    // Applies only to `OverlayDebug`; `OverlayStatusUpdate` (the SeatHub HUD/warning surface) has
    // no per-line concept and this call does not touch it.
    //
    // OD-04: Moonlight's own stats hotkey (Ctrl+Alt+Shift+S, or the gamepad chord) can enable
    // `OverlayDebug` mid-stream on its own, with no SeatHub code path in between - this filter is
    // exactly what `notifyOverlayUpdated()` consults on every rasterise regardless of what turned
    // the overlay on, so there is only ever the one filtered read, never a separate "show
    // everything" path the hotkey could reach instead: with nothing enabled, the hotkey draws
    // nothing.
    //
    // Threading: same lock-free contract as the rest of this class (the atomic pointer swap the
    // bitmap path already uses). Settings are read-only while a stream is active (T-05-52), so
    // this is set once before the engine starts and read fresh on every OverlayDebug rasterise
    // afterwards - never cached in the surface.
    void setDebugLineFilter(const QStringList& enabledLabels);

    // SeatHub: D-28 exception, see FORK-CHANGES.md. CUST-17: the `OverlayDebug` text this class
    // would draw next - the engine's own raw text with `setDebugLineFilter()`'s current choice
    // applied, in the engine's own line order. The exact computation `notifyOverlayUpdated()`
    // uses to build the rasterised surface, exposed as its own read-only step because it needs no
    // TTF font to answer, and this fork's test project links none (see
    // `tests/tst_overlay_injection.cpp`'s own file header).
    QByteArray filteredDebugText() const;

    // SeatHub: D-28 exception (ADR-0045 amendment, 2026-09-26), see FORK-CHANGES.md.
    //
    // A pluggable text rasteriser. Wherever this class would rasterise a slot with SDL_ttf, it
    // instead hands the slot's RAW text (never `filteredDebugText()` - CUST-17's per-line filter
    // stays SeatHub's own concern), that slot's own `enabled` flag and its own `SDL_Color` to this
    // callback, and publishes whatever ARGB8888 straight-alpha surface it returns through the
    // same atomic swap `updateOverlaySurface()` uses. A returned surface is refused - freed here,
    // nothing published - under exactly the rules `updateOverlaySurface()` already applies: a
    // foreign pixel format, a surface that needs locking, or a publish while the slot is disabled.
    //
    // Called even while the slot is disabled, so the rasteriser learns the engine's text went
    // away (`setOverlayState()` clears `text` to empty on disable, same as always) - the returned
    // surface is still subject to the disabled-slot refusal above, so a disabled slot never
    // actually publishes through this path either.
    //
    // With no rasteriser set, this class behaves byte-for-byte as it always has: the SDL_ttf
    // path, `updateOverlaySurface()`, `clearSeatHubSurface()` and `setDebugLineFilter()` are
    // unchanged.
    typedef SDL_Surface* (*TextRasterizer)(OverlayType type, const char* text, bool enabled,
                                           SDL_Color color, void* context);

    // Sets the one rasteriser for every slot. Call once, before the stream runs; the same
    // lock-free atomic-swap idiom `setDebugLineFilter()` already uses. Passing `nullptr` restores
    // the SDL_ttf fallback path.
    void setTextRasterizer(TextRasterizer rasterizer, void* context);

    void setOverlayRenderer(IOverlayRenderer* renderer);

private:
    void notifyOverlayUpdated(OverlayType type);
    void reassertPublishedSurface(OverlayType type);

    struct {
        bool enabled;
        int fontSize;
        SDL_Color color;
        char text[512];

        TTF_Font* font;
        SDL_Surface* surface;

        // SeatHub: D-28 exception, see FORK-CHANGES.md and ADR-0045.
        //
        // Remembers the caller-rendered bitmap this slot last published (`updateOverlaySurface()`),
        // as a private copy the caller owes nothing to and this class owns for as long as it holds
        // it: `updateOverlaySurface()`'s surface argument is consumed on delivery, and the renderer
        // takes it too, so the same pointer cannot be kept without a second reference or a copy.
        // Existing to close a real gap: `notifyOverlayUpdated()` - the text path, run whenever the
        // engine calls `updateOverlayText()` / `setOverlayState()` - always rasterises `text` and
        // replaces whatever is in the slot, which took SeatHub's bitmap off screen exactly when
        // Moonlight's own status text (a poor-connection warning, or nothing at all on recovery)
        // wrote to the same overlay (RESEARCH Pitfall 2). With this set, the text path re-publishes
        // this copy immediately afterwards instead, so a low-balance warning survives it.
        SDL_Surface* seatHubSurface;
    } m_Overlays[OverlayMax];
    IOverlayRenderer* m_Renderer;
    QByteArray m_FontData;

    // SeatHub: D-28 exception, see FORK-CHANGES.md. CUST-17/D-23: the OverlayDebug line filter -
    // see `setDebugLineFilter()`'s own comment for the full contract. A `QByteArray*` (the
    // caller's enabled labels, `\n`-joined) swapped the same lock-free way `m_Overlays[type].
    // surface` already is; nullptr means "nothing enabled", the same as an explicit empty list.
    // `mutable` because `filteredDebugText()` is logically const (it changes no externally
    // visible state) but reads this through the same atomic API the setter writes it with.
    mutable void* m_DebugLineFilter;

    // SeatHub: D-28 exception (ADR-0045 amendment, 2026-09-26), see FORK-CHANGES.md. The
    // pluggable text rasteriser and its caller-owned context, swapped the same lock-free way
    // `m_DebugLineFilter` already is. `nullptr` on both means "no rasteriser set" - the SDL_ttf
    // path stays exactly as it always has.
    void* m_TextRasterizer;
    void* m_TextRasterizerContext;
};

}
