#pragma once

#include <QString>

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
    // Threading: same contract as the rest of this class. Safe from any thread; the surface
    // hand-off is the atomic swap the text path already uses, and the renderer notification
    // is documented as callable from an arbitrary thread. Delivery is asynchronous: the
    // renderer picks the bitmap up on its next frame.
    //
    // Returns true when the bitmap was published (the overlay is enabled and the surface was
    // accepted), false when it was refused.
    bool updateOverlaySurface(OverlayType type, SDL_Surface* surface);

    void setOverlayRenderer(IOverlayRenderer* renderer);

private:
    void notifyOverlayUpdated(OverlayType type);

    struct {
        bool enabled;
        int fontSize;
        SDL_Color color;
        char text[512];

        TTF_Font* font;
        SDL_Surface* surface;
    } m_Overlays[OverlayMax];
    IOverlayRenderer* m_Renderer;
    QByteArray m_FontData;
};

}
