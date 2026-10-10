#include "osd_compositor.h"

#include "stats_catalogue.h"
#include "stream_stats.h"

#include <QDateTime>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>

#include <cmath>
#include <cstring>

namespace {

static void paintHintText(QPainter& painter, const QPointF& baseline, const QFont& font,
                          const QString& text, QRgb fillColor, qreal outlinePx)
{
    if (text.isEmpty()) {
        return;
    }

    QPainterPath path;
    path.addText(baseline, font, text);

    QPainterPath shadowPath;
    shadowPath.addText(baseline + QPointF(1.0, 1.0), font, text);
    painter.fillPath(shadowPath, QColor::fromRgba(kOsdOutline));

    QPen outlinePen(QColor::fromRgba(kOsdOutline), 2.0 * outlinePx, Qt::SolidLine,
                    Qt::RoundCap, Qt::RoundJoin);
    painter.strokePath(path, outlinePen);

    painter.fillPath(path, QColor::fromRgba(fillColor));
}

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

void OsdCompositor::setStatsTap(StatsTapFn tap)
{
    QMutexLocker locker(&m_mutex);
    m_statsTap = std::move(tap);
}

SDL_Surface* OsdCompositor::rasterize(Overlay::OverlayType type, const char* text, bool enabled,
                                       SDL_Color color, void* context)
{
    if (context == nullptr) {
        return nullptr;
    }
    return static_cast<OsdCompositor*>(context)->rasterizeInstance(type, text, enabled, color);
}

void OsdCompositor::showStartHint(bool show)
{
    QMutexLocker locker(&m_mutex);
    m_state.startHintActive = show;
    if (show) {
        m_state.startHintTimestamp = QDateTime::currentMSecsSinceEpoch();
    } else {
        m_state.startHintTimestamp = 0;
    }
}

bool OsdCompositor::isStartHintActive() const
{
    QMutexLocker locker(&m_mutex);
    if (!m_state.startHintActive) {
        return false;
    }
    if (m_state.startHintTimestamp > 0) {
        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - m_state.startHintTimestamp;
        if (elapsed >= 10000) {
            return false;
        }
    }
    return true;
}

QImage OsdCompositor::renderBottom(const State& current) const
{
    const bool hintActive = isStartHintActive();
    if (!hintActive) {
        return renderOsdBottom(current.windowWidth, current.windowHeight, current.engineText,
                               current.engineColor, current.timeLeft);
    }

    // SC13 / ADR-0087: start-of-stream hint renders in the Time left slot (bottom-right)
    // beside Moonlight's status text without hiding it.
    OsdTimeLeft noTimeLeft = current.timeLeft;
    noTimeLeft.visible = false;

    QImage base = renderOsdBottom(current.windowWidth, current.windowHeight, current.engineText,
                                  current.engineColor, noTimeLeft);

    const OsdSizes sizes = osdSizesFor(current.windowHeight);
    const QFont hintFont = osdFont(sizes.valuePx);
    const QFontMetricsF hintMetrics(hintFont);
    const QString hintText = QStringLiteral("Ctrl+Alt+Shift+Q ends the session. Ctrl+Alt+Shift+S shows stats.");
    const qreal hintWidth = hintMetrics.horizontalAdvance(hintText);
    const qreal hintHeight = hintMetrics.height();
    const qreal hintBlockHeight = hintHeight + sizes.timeLeftInset;

    const int targetHeight = static_cast<int>(std::ceil(std::max(
        base.isNull() ? 0.0 : static_cast<qreal>(base.height()), hintBlockHeight)));

    if (targetHeight <= 0) {
        return QImage();
    }

    QImage image(std::max(1, current.windowWidth), targetHeight, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    if (!base.isNull()) {
        painter.drawImage(0, targetHeight - base.height(), base);
    }

    const qreal baselineY = static_cast<qreal>(targetHeight) - sizes.timeLeftInset - hintMetrics.descent();
    const qreal x = static_cast<qreal>(current.windowWidth) - sizes.timeLeftInset - hintWidth;

    paintHintText(painter, QPointF(x, baselineY), hintFont, hintText, kOsdValue, sizes.outlinePx);

    return image;
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

    if (!enabled && !isStartHintActive()) {
        // Nothing would draw it, and `OverlayManager`'s own refusal rule would discard whatever
        // this returned anyway (the same rule `updateOverlaySurface()` applies) - returning
        // nullptr here up front avoids the wasted render.
        return nullptr;
    }

    const QImage image = renderBottom(current);
    if (image.isNull()) {
        return nullptr;
    }
    return toOwnedArgbSurface(image);
}

SDL_Surface* OsdCompositor::rasterizeStats(const char* text, bool enabled) const
{
    if (!enabled) {
        // OD-04 (now SeatHub's own rule): a disabled slot draws nothing, whatever is ticked.
        return nullptr;
    }

    VideoStats stats;
    const QString block = QString::fromUtf8(text != nullptr ? text : "");
    if (!parseVideoStatsBlock(block, &stats)) {
        return nullptr;
    }

    // D-11, Plan 13 Task 1: feed the sampler BEFORE the labels-empty OD-04 gate so the
    // sampler always receives data even when no rows are visible to the customer. The tap
    // is a sentry-free callback, installed by the facade via setStatsTap().
    // No sentry.h, no facade include crosses the compositor boundary.
    {
        StatsTapFn tap;
        {
            QMutexLocker locker(&m_mutex);
            tap = m_statsTap;
        }
        if (tap) {
            tap(stats);
        }
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

    const QList<OsdStatsRow> rows = buildStatsRows(labels, stats);
    if (rows.isEmpty()) {
        return nullptr;
    }

    return toOwnedArgbSurface(renderOsdStats(rows, windowHeight));
}

QImage OsdCompositor::composedBottom() const
{
    const State current = snapshot();
    return renderBottom(current);
}
