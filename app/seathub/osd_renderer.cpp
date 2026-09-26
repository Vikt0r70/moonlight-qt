#include "osd_renderer.h"

#include "duration_text.h"

#include <QColor>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QLoggingCategory>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointF>
#include <QStringList>

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(seathubOsd, "seathub.osd")

namespace {

// Set once by a successful `registerOsdFonts()` call; read by every `osdFont()` call after that.
// `-1` / empty mean "not registered yet" - `osdFont()` still returns a usable (if not Open Sans)
// font in that case, matching `registerOsdFonts()`'s own "log and fall back" contract.
int g_osdFontId = -1;
QString g_osdFamily;

// `px(base) = max(floor, round(base * u))` (D-14). `u` is the window's client height in pixels
// over 1080.
int scaledPx(qreal base, qreal u, int floorPx)
{
    return std::max(floorPx, static_cast<int>(std::lround(base * u)));
}

// A "derived" size (`06.6-RESEARCH-FORK.md` §2): scales the same way but has no named floor of
// its own beyond staying at least one pixel.
int derivedPx(qreal base, qreal u)
{
    return std::max(1, static_cast<int>(std::lround(base * u)));
}

// Paints one text run at `baseline` (the path's own baseline origin, as `QPainterPath::addText`
// takes it): with `outlined`, a (+1,+1) shadow fill, then a round-joined stroke, both in
// `kOsdOutline`, then the fill in `fillColor` (`ui.md` §7: "Each glyph gets a shadow ... and a
// stroke ... then the fill"). Without `outlined` (Moonlight's own status lines, D-13), only the
// fill is drawn. A no-op for an empty run so a caller never has to guard blank sub-labels itself.
void paintTextRun(QPainter& painter, const QPointF& baseline, const QFont& font,
                   const QString& text, QRgb fillColor, qreal outlinePx, bool outlined)
{
    if (text.isEmpty()) {
        return;
    }

    QPainterPath path;
    path.addText(baseline, font, text);

    if (outlined) {
        QPainterPath shadowPath;
        shadowPath.addText(baseline + QPointF(1.0, 1.0), font, text);
        painter.fillPath(shadowPath, QColor::fromRgba(kOsdOutline));

        QPen outlinePen(QColor::fromRgba(kOsdOutline), 2.0 * outlinePx, Qt::SolidLine,
                        Qt::RoundCap, Qt::RoundJoin);
        painter.strokePath(path, outlinePen);
    }

    painter.fillPath(path, QColor::fromRgba(fillColor));
}

} // namespace

bool registerOsdFonts()
{
    if (g_osdFontId >= 0) {
        // Idempotent: Qt would happily load the same file a second time under a second id, so
        // the guard is this function's own, not something `QFontDatabase` gives for free.
        return true;
    }

    const int id = QFontDatabase::addApplicationFont(
        QStringLiteral(":/seathub/fonts/OpenSans-SemiBold.ttf"));
    if (id < 0) {
        qCWarning(seathubOsd) << "could not register the bundled Open Sans SemiBold font;"
                                 " the OSD will draw with Qt's fallback face";
        return false;
    }

    const QStringList families = QFontDatabase::applicationFontFamilies(id);
    if (families.isEmpty()) {
        qCWarning(seathubOsd) << "Open Sans SemiBold registered but reported no font family";
        return false;
    }

    g_osdFontId = id;
    g_osdFamily = families.first();
    return true;
}

QFont osdFont(int pixelSize)
{
    // `g_osdFamily` is only ever the family Qt itself returned for the bundled file
    // (RESEARCH-FORK.md Pitfall 7); the literal here is reached only if a caller asks for a font
    // before `registerOsdFonts()` has run at all, which is a caller bug this still degrades
    // gracefully from.
    QFont font(g_osdFamily.isEmpty() ? QStringLiteral("Open Sans SemiBold") : g_osdFamily);
    font.setPixelSize(pixelSize);
    font.setWeight(QFont::DemiBold);
    // `ui.md` §4: "Digits have equal advances (tabular figures)". Set explicitly rather than
    // assumed, so the OSD does not depend on whichever figures a given build of Open Sans
    // defaults to.
    font.setFeature(QFont::Tag("tnum"), 1);
    return font;
}

OsdSizes osdSizesFor(int windowHeight)
{
    const qreal u = windowHeight > 0 ? (static_cast<qreal>(windowHeight) / 1080.0) : 0.0;

    OsdSizes sizes;
    sizes.valuePx = scaledPx(15.0, u, 13);
    sizes.labelPx = scaledPx(15.0, u, 13);
    // "unit suffix: round(0.6 * value px), raised to the cap height" - the 0.6 factor keeps it
    // smaller than the value at every size in range (D-14's "units are smaller than values").
    sizes.unitPx = std::max(1, static_cast<int>(std::lround(0.6 * sizes.valuePx)));
    sizes.rowHeight = derivedPx(18.0, u);
    sizes.labelColumn = derivedPx(56.0, u);
    sizes.gap = derivedPx(8.0, u);
    sizes.statsMargin = derivedPx(16.0, u);
    sizes.timeLeftInset = derivedPx(24.0, u);
    sizes.statusPx = scaledPx(36.0, u, 13);
    // A pen width, not a rounded pixel count - the outline floor is 1.0px exactly (D-14), not the
    // rounded-then-floored integer form the text sizes use.
    sizes.outlinePx = std::max(1.0, 1.5 * u);
    return sizes;
}

QImage renderOsdStats(const QList<OsdStatsRow>& rows, int windowHeight)
{
    if (rows.isEmpty()) {
        // OD-04 / `ui.md` §7: "A row is drawn only when its Settings toggle is on; with every row
        // off nothing is drawn, even when the stats hotkey shows the slot." The caller is the one
        // that turns "every toggle off" into an empty list; this is the null-image half of that
        // contract.
        return QImage();
    }

    const OsdSizes sizes = osdSizesFor(windowHeight);
    const QFont labelFont = osdFont(sizes.labelPx);
    const QFont valueFont = osdFont(sizes.valuePx);
    const QFont unitFont = osdFont(sizes.unitPx);
    const QFontMetricsF labelMetrics(labelFont);
    const QFontMetricsF valueMetrics(valueFont);
    const QFontMetricsF unitMetrics(unitFont);

    // Measure every row so the image is exactly as wide as its widest row needs - a fixed width
    // would either clip a long LATENCY row or waste space around a single-value FPS row.
    qreal maxPartsWidth = 0.0;
    for (const OsdStatsRow& row : rows) {
        qreal partsWidth = 0.0;
        for (const OsdValuePart& part : row.parts) {
            if (!part.subLabel.isEmpty()) {
                partsWidth += labelMetrics.horizontalAdvance(part.subLabel) + sizes.gap;
            }
            partsWidth += valueMetrics.horizontalAdvance(part.value) + sizes.gap
                + unitMetrics.horizontalAdvance(part.unit) + (sizes.gap * 2);
        }
        maxPartsWidth = std::max(maxPartsWidth, partsWidth);
    }

    const int width = static_cast<int>(
        std::ceil((sizes.statsMargin * 2) + sizes.labelColumn + maxPartsWidth));
    const int height = (sizes.statsMargin * 2) + (static_cast<int>(rows.size()) * sizes.rowHeight);

    QImage image(std::max(1, width), std::max(1, height), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    qreal rowTop = sizes.statsMargin;
    for (const OsdStatsRow& row : rows) {
        const qreal centreY = rowTop + (sizes.rowHeight / 2.0);
        const QRgb labelColour = row.mediaLabel ? kOsdLabelMedia : kOsdLabel;
        const qreal labelBaseline = centreY + ((labelMetrics.ascent() - labelMetrics.descent()) / 2.0);

        paintTextRun(painter, QPointF(sizes.statsMargin, labelBaseline), labelFont,
                     row.label.toUpper(), labelColour, sizes.outlinePx, true);

        qreal x = sizes.statsMargin + sizes.labelColumn;
        for (const OsdValuePart& part : row.parts) {
            if (!part.subLabel.isEmpty()) {
                paintTextRun(painter, QPointF(x, labelBaseline), unitFont, part.subLabel,
                             labelColour, sizes.outlinePx, true);
                x += unitMetrics.horizontalAdvance(part.subLabel) + sizes.gap;
            }

            const qreal valueBaseline = centreY + ((valueMetrics.ascent() - valueMetrics.descent()) / 2.0);
            paintTextRun(painter, QPointF(x, valueBaseline), valueFont, part.value, kOsdValue,
                         sizes.outlinePx, true);
            x += valueMetrics.horizontalAdvance(part.value) + sizes.gap;

            // "raised to the cap height": the unit's cap sits level with the value's cap, rather
            // than sharing the value's baseline (which would sink a smaller face visibly below
            // the value it qualifies).
            const qreal unitBaseline = valueBaseline - (valueMetrics.capHeight() - unitMetrics.capHeight());
            paintTextRun(painter, QPointF(x, unitBaseline), unitFont, part.unit, kOsdValue,
                         sizes.outlinePx, true);
            x += unitMetrics.horizontalAdvance(part.unit) + (sizes.gap * 2);
        }

        rowTop += sizes.rowHeight;
    }

    painter.end();
    return image.convertToFormat(QImage::Format_ARGB32);
}

QImage renderOsdBottom(int windowWidth, int windowHeight, const QString& engineText,
                       QRgb engineColor, const OsdTimeLeft& timeLeft)
{
    const bool drawEngine = !engineText.isEmpty();
    const bool drawTimeLeft = timeLeft.visible;
    if (!drawEngine && !drawTimeLeft) {
        return QImage();
    }

    const OsdSizes sizes = osdSizesFor(windowHeight);
    const QFont engineFont = osdFont(sizes.statusPx);
    const QFont valueFont = osdFont(sizes.valuePx);
    const QFont unitFont = osdFont(sizes.unitPx);
    const QFontMetricsF engineMetrics(engineFont);
    const QFontMetricsF valueMetrics(valueFont);
    const QFontMetricsF unitMetrics(unitFont);

    // Moonlight's own line breaks are kept (D-13); the block grows with the number of lines.
    const QStringList engineLines = drawEngine ? engineText.split(QLatin1Char('\n')) : QStringList();
    const qreal engineLineHeight = engineMetrics.height();
    const qreal engineBlockHeight = static_cast<qreal>(engineLines.size()) * engineLineHeight;

    // `durationText()` is the one formatter every client surface uses (FLOW-09) - Time left splits
    // its `"9 min"` into the number (value style) and the trailing `min` (unit style, D-08).
    QString timeLeftNumber;
    QString timeLeftUnit;
    qreal timeLeftWidth = 0.0;
    qreal timeLeftHeight = 0.0;
    if (drawTimeLeft) {
        const QString text = durationText(timeLeft.minutes);
        const int spaceIndex = text.indexOf(QLatin1Char(' '));
        timeLeftNumber = spaceIndex >= 0 ? text.left(spaceIndex) : text;
        timeLeftUnit = spaceIndex >= 0 ? text.mid(spaceIndex + 1) : QString();
        timeLeftWidth = valueMetrics.horizontalAdvance(timeLeftNumber) + sizes.gap
            + unitMetrics.horizontalAdvance(timeLeftUnit);
        timeLeftHeight = valueMetrics.height();
    }

    // ADR-0045's padding technique: the image is as wide as the window, so it composes at a fixed
    // (0, displayHeight - image.height()) origin whether or not Time left is present, exactly the
    // way `hud_overlay.cpp`'s warning card already pads out to a card's own x offset.
    const qreal timeLeftBlockHeight = drawTimeLeft ? (timeLeftHeight + sizes.timeLeftInset) : 0.0;
    const int height = static_cast<int>(std::ceil(std::max(engineBlockHeight, timeLeftBlockHeight)));
    if (height <= 0) {
        return QImage();
    }

    QImage image(std::max(1, windowWidth), height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    if (drawEngine) {
        // Flush to the bottom-left, exactly where the engine's own slot drew it before this hook
        // existed (`overlaymanager.cpp`'s `renderRect.x = 0`) - a font-only change (D-13).
        qreal y = static_cast<qreal>(height) - engineBlockHeight + engineMetrics.ascent();
        for (const QString& line : engineLines) {
            paintTextRun(painter, QPointF(0.0, y), engineFont, line, engineColor, sizes.outlinePx,
                         false);
            y += engineLineHeight;
        }
    }

    if (drawTimeLeft) {
        const qreal baselineY = static_cast<qreal>(height) - sizes.timeLeftInset - valueMetrics.descent();
        qreal x = static_cast<qreal>(windowWidth) - sizes.timeLeftInset - timeLeftWidth;
        const QRgb colour = timeLeft.critical ? kOsdDestructive : kOsdValue;

        paintTextRun(painter, QPointF(x, baselineY), valueFont, timeLeftNumber, colour,
                     sizes.outlinePx, true);
        x += valueMetrics.horizontalAdvance(timeLeftNumber) + sizes.gap;

        const qreal unitBaseline = baselineY - (valueMetrics.capHeight() - unitMetrics.capHeight());
        paintTextRun(painter, QPointF(x, unitBaseline), unitFont, timeLeftUnit, colour,
                     sizes.outlinePx, true);
    }

    painter.end();
    return image.convertToFormat(QImage::Format_ARGB32);
}
