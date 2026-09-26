#include "osd_compositor.h"

#include "stats_catalogue.h"
#include "stream_stats.h"

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

// A metric formatted to `decimals` places - one small helper so every stats row uses the same
// trailing-zero behaviour `QString::number` already gives.
QString formatMetric(double value, int decimals)
{
    return QString::number(value, 'f', decimals);
}

// D-10/D-26 (Plan 16): maps the customer's ticked Moonlight-long-label choices
// (`enabledStatsLabels`, `SettingsBridge::enabledStatsLabels()`'s own order) plus the raw parsed
// stats into the OSD's `OsdStatsRow`s - RES, FPS, LATENCY first (in that order, when ticked),
// then every other catalogue entry in `stats_catalogue.h`'s own order, never in the customer's
// own tick order (`docs/spec/screens.md` §25's own row order). A row whose data was not present
// in this parse (an absent conditional line) is skipped, not guessed (`06.6-16-PLAN.md`'s own
// action text: "a torn or unknown line is dropped, never guessed").
QList<OsdStatsRow> buildStatsRows(const QStringList& enabledLongLabels, const VideoStats& stats)
{
    QList<QString> orderedKeys;
    const char* const kPriorityKeys[] = { "statsVideoStream", "statsRenderingFrameRate",
                                          "statsNetworkLatency" };
    for (const char* key : kPriorityKeys) {
        orderedKeys.append(QString::fromLatin1(key));
    }
    for (const StatsCatalogueEntry& entry : kStatsCatalogue) {
        const QString key = QString::fromLatin1(entry.key);
        if (!orderedKeys.contains(key)) {
            orderedKeys.append(key);
        }
    }

    QList<OsdStatsRow> rows;
    for (const QString& key : orderedKeys) {
        const StatsCatalogueEntry* entry = findStatsCatalogueEntry(key);
        if (entry == nullptr || !enabledLongLabels.contains(QString::fromLatin1(entry->longLabel))) {
            continue;
        }

        const QString shortLabel = QString::fromLatin1(entry->shortLabel);
        const QString unit = QString::fromLatin1(entry->unit);

        if (key == QLatin1String("statsVideoStream")) {
            if (!stats.videoWidth.present || !stats.videoHeight.present) {
                continue;
            }
            const QString value = QStringLiteral("%1x%2")
                .arg(static_cast<int>(stats.videoWidth.value))
                .arg(static_cast<int>(stats.videoHeight.value));
            rows.append(OsdStatsRow{ shortLabel, true,
                                     { OsdValuePart{ QString(), value, QString() } } });
            continue;
        }

        if (key == QLatin1String("statsNetworkLatency")) {
            if (!stats.rttMs.present) {
                // No network figure: neither NET nor TOTAL can be drawn (`screens.md` §25: "With
                // no network figure there is no total").
                continue;
            }
            QList<OsdValuePart> parts;
            parts.append(
                OsdValuePart{ QStringLiteral("NET"), formatMetric(stats.rttMs.value, 0), unit });
            const OptionalMetric total = totalLatencyMs(stats);
            if (total.present) {
                parts.append(OsdValuePart{ QStringLiteral("TOTAL"), formatMetric(total.value, 0),
                                           unit });
            }
            rows.append(OsdStatsRow{ shortLabel, false, parts });
            continue;
        }

        OptionalMetric metric;
        int decimals = 2;
        if (key == QLatin1String("statsIncomingFrameRate")) {
            metric = stats.receivedFps;
            decimals = 1;
        } else if (key == QLatin1String("statsDecodingFrameRate")) {
            metric = stats.decodedFps;
            decimals = 1;
        } else if (key == QLatin1String("statsRenderingFrameRate")) {
            metric = stats.renderedFps;
            decimals = 1;
        } else if (key == QLatin1String("statsHostProcessingLatency")) {
            metric = stats.hostProcessingAvgMs;
            decimals = 1;
        } else if (key == QLatin1String("statsNetworkDroppedFrames")) {
            metric = stats.networkDroppedFramePct;
        } else if (key == QLatin1String("statsJitterDroppedFrames")) {
            metric = stats.jitterDroppedFramePct;
        } else if (key == QLatin1String("statsDecodingTime")) {
            metric = stats.decodeTimeMs;
        } else if (key == QLatin1String("statsFrameQueueDelay")) {
            metric = stats.queueTimeMs;
        } else if (key == QLatin1String("statsRenderingTime")) {
            metric = stats.renderTimeMs;
        }

        if (!metric.present) {
            continue;
        }
        rows.append(OsdStatsRow{
            shortLabel, false, { OsdValuePart{ QString(), formatMetric(metric.value, decimals), unit } } });
    }

    return rows;
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
                                               bool enabled, SDL_Color color)
{
    if (type == Overlay::OverlayDebug) {
        return rasterizeStats(text, enabled);
    }

    if (type != Overlay::OverlayStatusUpdate) {
        return nullptr;
    }

    State current;
    {
        QMutexLocker locker(&m_mutex);
        // Recorded regardless of `enabled`: the engine already cleared `text` to empty before
        // this call in the disabled case (`OverlayManager::setOverlayState()`), so this is how
        // the compositor learns the engine's own line went away (RESEARCH-FORK.md §1.3) - a
        // stale line from before disabling must not survive into the next composition.
        m_state.engineText = QString::fromUtf8(text != nullptr ? text : "");
        m_state.engineColor = qRgba(color.r, color.g, color.b, color.a);
        current = m_state;
    }

    if (!enabled) {
        // Nothing would draw it, and `OverlayManager`'s own refusal rule would discard whatever
        // this returned anyway (the same rule `updateOverlaySurface()` applies) - returning
        // nullptr here up front avoids the wasted render.
        return nullptr;
    }

    const QImage image = renderOsdBottom(current.windowWidth, current.windowHeight,
                                          current.engineText, current.engineColor,
                                          current.timeLeft);
    return toOwnedArgbSurface(image);
}

SDL_Surface* OsdCompositor::rasterizeStats(const char* text, bool enabled) const
{
    if (!enabled) {
        // OD-04 (now SeatHub's own rule): a disabled slot draws nothing, whatever is ticked.
        return nullptr;
    }

    QStringList labels;
    int windowHeight;
    {
        QMutexLocker locker(&m_mutex);
        labels = m_state.enabledStatsLabels;
        windowHeight = m_state.windowHeight;
    }
    if (labels.isEmpty()) {
        // OD-04: with every row off, nothing is drawn - even though the slot itself is enabled
        // and the raw text carries every line.
        return nullptr;
    }

    VideoStats stats;
    const QString block = QString::fromUtf8(text != nullptr ? text : "");
    if (!parseVideoStatsBlock(block, &stats)) {
        return nullptr;
    }

    const QList<OsdStatsRow> rows = buildStatsRows(labels, stats);
    if (rows.isEmpty()) {
        return nullptr;
    }

    return toOwnedArgbSurface(renderOsdStats(rows, windowHeight));
}

QImage OsdCompositor::composedBottom() const
{
    const State current = snapshot();
    return renderOsdBottom(current.windowWidth, current.windowHeight, current.engineText,
                            current.engineColor, current.timeLeft);
}
