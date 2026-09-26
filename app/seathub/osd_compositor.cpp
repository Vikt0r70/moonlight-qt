#include "osd_compositor.h"

#include <QMutexLocker>

#include <cstring>

namespace {

// CR-03 (`hud_overlay.cpp`'s own rule, ADR-0045): the surface owns its pixels - a fresh
// allocation and a byte-for-byte row copy, never a view onto `image`'s own buffer, because
// delivery through `OverlayManager` is asynchronous and some renderers (`SdlRenderer`) do not
// consume the surface until their own next frame.
SDL_Surface* toOwnedArgbSurface(const QImage& image)
{
    if (image.isNull()) {
        return nullptr;
    }

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, image.width(), image.height(), 32,
                                                          SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) {
        return nullptr;
    }

    const size_t rowBytes = static_cast<size_t>(image.width()) * 4;
    SDL_LockSurface(surface);
    for (int y = 0; y < image.height(); ++y) {
        memcpy(static_cast<Uint8*>(surface->pixels) + (y * surface->pitch),
               image.constScanLine(y), rowBytes);
    }
    SDL_UnlockSurface(surface);
    return surface;
}

} // namespace

OsdCompositor::OsdCompositor() = default;

void OsdCompositor::setWindowSize(int width, int height)
{
    QMutexLocker locker(&m_mutex);
    m_state.windowWidth = width;
    m_state.windowHeight = height;
}

void OsdCompositor::setTimeLeft(const OsdTimeLeft& timeLeft)
{
    QMutexLocker locker(&m_mutex);
    m_state.timeLeft = timeLeft;
}

void OsdCompositor::setEnabledStatsLabels(const QStringList& labels)
{
    QMutexLocker locker(&m_mutex);
    m_state.enabledStatsLabels = labels;
}

OsdCompositor::State OsdCompositor::snapshot() const
{
    QMutexLocker locker(&m_mutex);
    return m_state;
}

SDL_Surface* OsdCompositor::rasterize(Overlay::OverlayType type, const char* text, bool enabled,
                                       SDL_Color color, void* context)
{
    if (context == nullptr) {
        return nullptr;
    }
    return static_cast<OsdCompositor*>(context)->rasterizeInstance(type, text, enabled, color);
}

SDL_Surface* OsdCompositor::rasterizeInstance(Overlay::OverlayType type, const char* text,
                                               bool /*enabled*/, SDL_Color color)
{
    if (type != Overlay::OverlayStatusUpdate) {
        // Plan 16 adds the stats block for OverlayDebug; nothing to draw here yet.
        return nullptr;
    }

    State current;
    {
        QMutexLocker locker(&m_mutex);
        // Recorded even while the slot is disabled - the engine already cleared `text` to empty
        // before this call in that case (`OverlayManager::setOverlayState()`), so this is how
        // SeatHub learns the engine's own line went away (RESEARCH-FORK.md §1.3).
        m_state.engineText = QString::fromUtf8(text != nullptr ? text : "");
        m_state.engineColor = qRgba(color.r, color.g, color.b, color.a);
        current = m_state;
    }

    const QImage image = renderOsdBottom(current.windowWidth, current.windowHeight,
                                          current.engineText, current.engineColor,
                                          current.timeLeft);
    return toOwnedArgbSurface(image);
}

QImage OsdCompositor::composedBottom() const
{
    const State current = snapshot();
    return renderOsdBottom(current.windowWidth, current.windowHeight, current.engineText,
                            current.engineColor, current.timeLeft);
}
