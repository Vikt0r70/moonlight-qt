#include "overlaymanager.h"
#include "path.h"

#include <QList>
#include <QSet>

using namespace Overlay;

namespace {

// SeatHub: D-28 exception, see FORK-CHANGES.md. CUST-17/D-23/D-26: the shared filtering logic
// `OverlayManager::setDebugLineFilter()`/`filteredDebugText()` and `notifyOverlayUpdated()` all
// rely on. Matches each `\n`-terminated line's label (the text up to, not including, its first
// ':') against `enabledLabelsJoined` (itself `\n`-joined, as `setDebugLineFilter()` stores it).
// See `setDebugLineFilter()`'s own header comment for the full contract this implements: engine
// order preserved, no gap left by a skipped line, no "draw by default" case for an unmatched
// label.
QByteArray filterDebugLines(const char* rawText, const QByteArray& enabledLabelsJoined)
{
    if (rawText == nullptr || rawText[0] == '\0') {
        return QByteArray();
    }

    QSet<QByteArray> enabledLabels;
    if (!enabledLabelsJoined.isEmpty()) {
        const QList<QByteArray> parts = enabledLabelsJoined.split('\n');
        for (const QByteArray& label : parts) {
            if (!label.isEmpty()) {
                enabledLabels.insert(label);
            }
        }
    }

    if (enabledLabels.isEmpty()) {
        return QByteArray();
    }

    QByteArray filtered;
    const QList<QByteArray> lines = QByteArray(rawText).split('\n');
    for (const QByteArray& line : lines) {
        if (line.isEmpty()) {
            continue;
        }

        const int colon = line.indexOf(':');
        if (colon < 0 || !enabledLabels.contains(line.left(colon))) {
            // No colon at all (not one of the engine's own lines), or a label this filter was
            // not told is enabled - disabled on purpose, or unrecognised because the engine
            // renamed or added it (T-05-49). Either way: skipped, with no gap left behind.
            continue;
        }

        filtered += line;
        filtered += '\n';
    }
    return filtered;
}

} // namespace

OverlayManager::OverlayManager() :
    m_Renderer(nullptr),
    m_FontData(Path::readDataFile("ModeSeven.ttf")),
    m_DebugLineFilter(nullptr),
    m_TextRasterizer(nullptr),
    m_TextRasterizerContext(nullptr)
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

    // SeatHub: D-28 exception, see FORK-CHANGES.md.
    delete static_cast<QByteArray*>(m_DebugLineFilter);

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

// SeatHub: D-28 exception, see FORK-CHANGES.md and ADR-0045. See the header's own comment on
// this method for the full contract.
void OverlayManager::setDebugLineFilter(const QStringList& enabledLabels)
{
    QByteArray* buffer = new QByteArray(enabledLabels.join(QLatin1Char('\n')).toUtf8());
    void* old = SDL_AtomicSetPtr(&m_DebugLineFilter, buffer);
    delete static_cast<QByteArray*>(old);
}

// SeatHub: D-28 exception, see FORK-CHANGES.md. See the header's own comment on this method.
QByteArray OverlayManager::filteredDebugText() const
{
    const QByteArray* filter = static_cast<QByteArray*>(SDL_AtomicGetPtr(&m_DebugLineFilter));
    const QByteArray filterCopy = (filter != nullptr) ? *filter : QByteArray();
    return filterDebugLines(m_Overlays[OverlayType::OverlayDebug].text, filterCopy);
}

// SeatHub: D-28 exception (ADR-0045 amendment, 2026-09-26), see FORK-CHANGES.md. See the header's
// own comment on this method for the full contract.
void OverlayManager::setTextRasterizer(TextRasterizer rasterizer, void* context)
{
    SDL_AtomicSetPtr(&m_TextRasterizerContext, context);
    SDL_AtomicSetPtr(&m_TextRasterizer, (void*)rasterizer);
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

    // SeatHub: D-28 exception (ADR-0045 amendment, 2026-09-26), see FORK-CHANGES.md.
    //
    // Placed before the seatHubSurface re-assert branch below: that branch returns early whenever
    // a SeatHub bitmap is remembered, and a rasteriser call placed after it would mean the
    // engine's own status text (the poor-connection warning, the gamepad mouse-mode hint) never
    // reaches SeatHub while a bitmap (Time left) is remembered - hiding D-13's message for as long
    // as that bitmap stays published. With the rasteriser first, SeatHub composes both itself.
    TextRasterizer rasterize = (TextRasterizer)SDL_AtomicGetPtr(&m_TextRasterizer);
    if (rasterize != nullptr) {
        SDL_Surface* oldSurface = (SDL_Surface*)SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, nullptr);
        if (oldSurface != nullptr) {
            SDL_FreeSurface(oldSurface);
        }

        // Called even while disabled, so SeatHub learns the engine's text went away
        // (`setOverlayState(false)` clears `text` to empty just before this runs).
        SDL_Surface* s = rasterize(type, m_Overlays[type].text, m_Overlays[type].enabled,
                                   m_Overlays[type].color,
                                   SDL_AtomicGetPtr(&m_TextRasterizerContext));
        if (s != nullptr && (!m_Overlays[type].enabled
                             || s->format->format != SDL_PIXELFORMAT_ARGB8888 || SDL_MUSTLOCK(s))) {
            // Same refusal rule updateOverlaySurface() applies (:161-177): a foreign format or a
            // surface that needs locking would trip an assert on a render thread, and a disabled
            // slot has nothing that would draw it.
            SDL_FreeSurface(s);
            s = nullptr;
        }

        SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, s);
        m_Renderer->notifyOverlayUpdated(type);
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
        if (type == OverlayType::OverlayDebug) {
            // SeatHub: D-28 exception, see FORK-CHANGES.md. CUST-17/D-23/D-26: draw only the
            // per-line subset the customer chose (`setDebugLineFilter()`/`filteredDebugText()`'s
            // own comments have the full contract, including OD-04's hotkey case). The engine's
            // own `text` field is untouched by this - `ffmpeg.cpp`'s writer still writes its
            // whole text every window; only what gets rasterised here changes, and it is
            // recomputed fresh on every call rather than cached, so a filter change before this
            // session's first window is what applies (T-05-52).
            const QByteArray filteredText = filteredDebugText();
            if (!filteredText.isEmpty()) {
                SDL_Surface* surface = TTF_RenderText_Blended_Wrapped(m_Overlays[type].font,
                                                                      filteredText.constData(),
                                                                      m_Overlays[type].color,
                                                                      1024);
                SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, surface);
            }
            // else: nothing enabled - the surface slot was already cleared above (CUST-17 empty
            // edge: nothing drawn at all, no empty box, no heading).
        }
        else {
            // The _Wrapped variant is required for line breaks to work
            SDL_Surface* surface = TTF_RenderText_Blended_Wrapped(m_Overlays[type].font,
                                                                  m_Overlays[type].text,
                                                                  m_Overlays[type].color,
                                                                  1024);
            SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, surface);
        }
    }

    // Notify the renderer
    m_Renderer->notifyOverlayUpdated(type);
}
