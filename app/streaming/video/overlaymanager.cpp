#include "overlaymanager.h"
#include "path.h"

using namespace Overlay;

OverlayManager::OverlayManager() :
    m_Renderer(nullptr),
    m_FontData(Path::readDataFile("ModeSeven.ttf"))
{
    memset(m_Overlays, 0, sizeof(m_Overlays));

    m_Overlays[OverlayType::OverlayDebug].color = {0xD0, 0xD0, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayDebug].fontSize = 20;

    m_Overlays[OverlayType::OverlayStatusUpdate].color = {0xCC, 0x00, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayStatusUpdate].fontSize = 36;

    // While TTF will usually not be initialized here, it is valid for that not to
    // be the case, since Session destruction is deferred and could overlap with
    // the lifetime of a new Session object.
    //SDL_assert(TTF_WasInit() == 0);

    if (TTF_Init() != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TTF_Init() failed: %s",
                    TTF_GetError());
        return;
    }
}

OverlayManager::~OverlayManager()
{
    for (int i = 0; i < OverlayType::OverlayMax; i++) {
        if (m_Overlays[i].surface != nullptr) {
            SDL_FreeSurface(m_Overlays[i].surface);
        }
        // SeatHub: D-28 exception, see FORK-CHANGES.md and ADR-0045.
        if (m_Overlays[i].seatHubSurface != nullptr) {
            SDL_FreeSurface(m_Overlays[i].seatHubSurface);
        }
        if (m_Overlays[i].font != nullptr) {
            TTF_CloseFont(m_Overlays[i].font);
        }
    }

    TTF_Quit();

    // For similar reasons to the comment in the constructor, this will usually,
    // but not always, deinitialize TTF. In the cases where Session objects overlap
    // in lifetime, there may be an additional reference on TTF for the new Session
    // that means it will not be cleaned up here.
    //SDL_assert(TTF_WasInit() == 0);
}

bool OverlayManager::isOverlayEnabled(OverlayType type)
{
    return m_Overlays[type].enabled;
}

char* OverlayManager::getOverlayText(OverlayType type)
{
    return m_Overlays[type].text;
}

void OverlayManager::updateOverlayText(OverlayType type, const char* text)
{
    strncpy(m_Overlays[type].text, text, sizeof(m_Overlays[0].text));
    m_Overlays[type].text[getOverlayMaxTextLength() - 1] = '\0';

    setOverlayTextUpdated(type);
}

int OverlayManager::getOverlayMaxTextLength()
{
    return sizeof(m_Overlays[0].text);
}

int OverlayManager::getOverlayFontSize(OverlayType type)
{
    return m_Overlays[type].fontSize;
}

SDL_Surface* OverlayManager::getUpdatedOverlaySurface(OverlayType type)
{
    // If a new surface is available, return it. If not, return nullptr.
    // Caller must free the surface on success.
    return (SDL_Surface*)SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, nullptr);
}

// SeatHub: D-28 exception, see FORK-CHANGES.md and ADR-0045. Publishes a caller-rendered
// bitmap through the same swap-and-notify path the text overlays use.
bool OverlayManager::updateOverlaySurface(OverlayType type, SDL_Surface* surface)
{
    if (surface == nullptr) {
        return false;
    }

    // Every renderer's upload path asserts both of these before it hands the pixels to the
    // GPU (see d3d11va.cpp's notifyOverlayUpdated, which rejects anything that is not
    // SDL_PIXELFORMAT_ARGB8888 and asserts !SDL_MUSTLOCK). Refusing here keeps that contract
    // instead of tripping an assert inside a render thread.
    if (surface->format->format != SDL_PIXELFORMAT_ARGB8888 || SDL_MUSTLOCK(surface)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SeatHub overlay: refused a bitmap for overlay %d (format %s, must lock %d)",
                    (int)type,
                    SDL_GetPixelFormatName(surface->format->format),
                    SDL_MUSTLOCK(surface));
        SDL_FreeSurface(surface);
        return false;
    }

    if (!m_Overlays[type].enabled) {
        // Nothing would draw it, so publishing would only hand the renderer a surface it
        // immediately frees (or, worse, leave a bitmap in the slot the text path assumes is
        // its own). `setOverlayState(type, true)` re-enables and notifies the renderer.
        SDL_FreeSurface(surface);
        return false;
    }

    // SeatHub: D-28 exception, see FORK-CHANGES.md and ADR-0045. Remembers a private copy of
    // this bitmap (see the `seatHubSurface` field comment) so `notifyOverlayUpdated()` can
    // re-publish it if the engine writes its own text or toggles this slot's enabled state
    // before the HUD publishes again. A copy, because `surface` itself is handed to the
    // renderer below and is not this class's to keep past that hand-off. A failed duplicate
    // (out of memory) only degrades to pre-05-10 behaviour - the engine's text can blank this
    // publish until the next one succeeds - so it is logged, not treated as a refusal.
    SDL_Surface* remembered = SDL_ConvertSurface(surface, surface->format, 0);
    if (remembered == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SeatHub overlay: could not remember a bitmap for overlay %d: %s",
                    (int)type, SDL_GetError());
    }
    if (m_Overlays[type].seatHubSurface != nullptr) {
        SDL_FreeSurface(m_Overlays[type].seatHubSurface);
    }
    m_Overlays[type].seatHubSurface = remembered;

    SDL_Surface* oldSurface = (SDL_Surface*)SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, surface);

    // Free the surface this bitmap replaced, exactly as the text path does.
    if (oldSurface != nullptr) {
        SDL_FreeSurface(oldSurface);
    }

    // Deliberately NOT setOverlayTextUpdated()/notifyOverlayUpdated(): those rasterise the
    // text field and would publish a text surface over the bitmap set one line above.
    if (m_Renderer != nullptr) {
        m_Renderer->notifyOverlayUpdated(type);
    }

    return true;
}

// SeatHub: D-28 exception, see FORK-CHANGES.md and ADR-0045.
void OverlayManager::clearSeatHubSurface(OverlayType type)
{
    if (m_Overlays[type].seatHubSurface != nullptr) {
        SDL_FreeSurface(m_Overlays[type].seatHubSurface);
        m_Overlays[type].seatHubSurface = nullptr;
    }
}

void OverlayManager::setOverlayTextUpdated(OverlayType type)
{
    // Only update the overlay state if it's enabled. If it's not enabled,
    // the renderer has already been notified by setOverlayState().
    if (m_Overlays[type].enabled) {
        notifyOverlayUpdated(type);
    }
}

void OverlayManager::setOverlayState(OverlayType type, bool enabled)
{
    bool stateChanged = m_Overlays[type].enabled != enabled;

    m_Overlays[type].enabled = enabled;

    if (stateChanged) {
        if (!enabled) {
            // Set the text to empty string on disable
            m_Overlays[type].text[0] = 0;
        }

        notifyOverlayUpdated(type);
    }
}

SDL_Color OverlayManager::getOverlayColor(OverlayType type)
{
    return m_Overlays[type].color;
}

void OverlayManager::setOverlayRenderer(IOverlayRenderer* renderer)
{
    m_Renderer = renderer;
}

// SeatHub: D-28 exception, see FORK-CHANGES.md and ADR-0045.
void OverlayManager::reassertPublishedSurface(OverlayType type)
{
    SDL_Surface* source = m_Overlays[type].seatHubSurface;
    if (source == nullptr) {
        return;
    }

    // A fresh copy each time: whatever is swapped into `m_Overlays[type].surface` below is taken
    // and freed by the renderer (`getUpdatedOverlaySurface()`'s documented contract), so the
    // remembered copy itself must never be handed out directly - it has to survive to be
    // re-published again the next time this runs.
    SDL_Surface* copy = SDL_ConvertSurface(source, source->format, 0);
    if (copy == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SeatHub overlay: could not re-assert the bitmap for overlay %d: %s",
                    (int)type, SDL_GetError());
        return;
    }

    SDL_Surface* oldSurface = (SDL_Surface*)SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, copy);
    if (oldSurface != nullptr) {
        SDL_FreeSurface(oldSurface);
    }

    if (m_Renderer != nullptr) {
        m_Renderer->notifyOverlayUpdated(type);
    }
}

void OverlayManager::notifyOverlayUpdated(OverlayType type)
{
    if (m_Renderer == nullptr) {
        return;
    }

    // SeatHub: D-28 exception, see FORK-CHANGES.md and ADR-0045.
    //
    // A remembered SeatHub bitmap is authoritative for this slot for as long as the overlay stays
    // enabled: an engine text write (`updateOverlayText()` -> `setOverlayTextUpdated()`) and a
    // state toggle (`setOverlayState()`) both call this function, and both would otherwise
    // rasterise `text` over exactly the frame a low-balance warning needs to stay on screen for
    // (RESEARCH Pitfall 2) - the first case is the poor-connection message overwriting it while
    // still enabled, the second is the overlay coming back enabled with nothing published since.
    // `clearSeatHubSurface()` is the only way out of this branch; disabling alone does not clear
    // it, which is what lets the bitmap reappear on re-enable rather than being replaced by
    // whatever the engine last wrote to `text`.
    if (m_Overlays[type].enabled && m_Overlays[type].seatHubSurface != nullptr) {
        reassertPublishedSurface(type);
        return;
    }

    // Construct the required font to render the overlay
    if (m_Overlays[type].font == nullptr) {
        if (m_FontData.isEmpty()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL overlay font failed to load");
            return;
        }

        // m_FontData must stay around until the font is closed
        m_Overlays[type].font = TTF_OpenFontRW(SDL_RWFromConstMem(m_FontData.constData(), m_FontData.size()),
                                               1,
                                               m_Overlays[type].fontSize);
        if (m_Overlays[type].font == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "TTF_OpenFont() failed: %s",
                        TTF_GetError());

            // Can't proceed without a font
            return;
        }
    }

    SDL_Surface* oldSurface = (SDL_Surface*)SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, nullptr);

    // Free the old surface
    if (oldSurface != nullptr) {
        SDL_FreeSurface(oldSurface);
    }

    if (m_Overlays[type].enabled) {
        // The _Wrapped variant is required for line breaks to work
        SDL_Surface* surface = TTF_RenderText_Blended_Wrapped(m_Overlays[type].font,
                                                              m_Overlays[type].text,
                                                              m_Overlays[type].color,
                                                              1024);
        SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, surface);
    }

    // Notify the renderer
    m_Renderer->notifyOverlayUpdated(type);
}
