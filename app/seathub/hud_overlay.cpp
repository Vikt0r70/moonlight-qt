#include "hud_overlay.h"

#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QFontMetricsF>
#include <QLoggingCategory>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QRectF>

#include <cmath>

Q_LOGGING_CATEGORY(seathubHud, "seathub.hud")

namespace {

// --- Design tokens ---------------------------------------------------------------------------
//
// The values and the names both come from the design authority (`docs/spec/ui.md` §3/§4) as
// emitted into `app/gui/Tokens.qml` and `app/gui/Metrics.qml`. `ui.md` defines no HUD-specific
// surface, so the card is composed from existing tokens rather than from new colour or size
// values; the four colours below are also checked against `Tokens.qml` mechanically by
// `tests/tst_hud_bitmap.cpp`, so they cannot drift from the token file silently.
constexpr QRgb kCardFill = 0xFF141414;    // Tokens.surface1Default
constexpr QRgb kCardBorder = 0xFF404040;  // Tokens.borderStrongDefault
constexpr QRgb kTextPrimary = 0xFFFAFAFA; // Tokens.foregroundDefault
constexpr QRgb kTextMuted = 0xFFA3A3A3;   // Tokens.foregroundMutedDefault
constexpr QRgb kLiveDot = 0xFF10B981;     // Tokens.successDefault
constexpr QRgb kHairline = 0xFF262626;    // Tokens.borderDefault

// Metrics mirror `app/gui/Metrics.qml`, which mirrors `Tokens.qml` at a 16px root.
constexpr int kStripHeight = 56;
constexpr int kPadX = 16;     // Metrics.s4
constexpr int kGap = 8;       // Metrics.s2
constexpr int kGroupGap = 20; // Metrics.s5
constexpr int kRadius = 20;   // Metrics.radiusLg
constexpr int kDotDiameter = 10;
constexpr int kGlyphSize = 16;
constexpr int kLabelPx = 12;  // Metrics.fontLabel
constexpr int kMonoPx = 14;   // Metrics.fontSm
constexpr int kTickMs = 1000;

// screens.md §25: the HUD "auto-hides after 4s, reappears on input".
constexpr qint64 kAutoHideMs = 4000;

// The End session affordance. `End session` is copy.md's own label (its glossary forbids "Quit"
// and "Stop"); the key combination is the D-02 binding stated as a binding, not as prose. The
// affordance is not a clickable control in Phase 3 - ADR-0045 records why (a click target inside
// the stream window would need either a wider D-28 exception or a second window). Neither string
// is final copy: D-55 gives Phase 5 the copy pass.
constexpr const char* kLiveLabel = "LIVE";
constexpr const char* kElapsedLabel = "Elapsed";
constexpr const char* kEndSessionLabel = "End session";
constexpr const char* kEndSessionKeys = "Ctrl+Alt+Shift+Q";

QFont labelFont(int weight)
{
    // Tokens.fontSansDefault. Neither Inter nor Geist Mono ships with the fork yet (only
    // ModeSeven.ttf, SDL_ttf's overlay font, and Qt cannot load the web build's WOFF2), so Qt
    // substitutes for now. Asking for the token family is still the right thing to do: when the
    // faces are bundled the HUD picks them up with no change here.
    QFont font(QStringLiteral("Inter"));
    font.setPixelSize(kLabelPx);
    font.setWeight(QFont::Weight(weight));
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.08 * kLabelPx); // ui.md §4 label tracking
    return font;
}

QFont monoFont(int pixelSize)
{
    QFont font(QStringLiteral("Geist Mono")); // Tokens.fontMonoDefault
    font.setPixelSize(pixelSize);
    font.setWeight(QFont::Medium);
    // ui.md §4: tabular figures on every numeric display, so the timer does not jitter as its
    // digits change.
    font.setFeature(QFont::Tag("tnum"), 1);
    return font;
}

QString formatDuration(qint64 seconds)
{
    if (seconds < 0) {
        seconds = 0;
    }
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds / 60) % 60;
    const qint64 secs = seconds % 60;

    // Always HH:MM:SS rather than dropping the hour below an hour: the card then has a fixed
    // width for the whole session instead of reflowing when the first hour rolls over.
    return QStringLiteral("%1:%2:%3")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'));
}

// Centres a line of one font on the card's vertical midline using that font's own metrics. Each
// font gets its own baseline: a shared baseline would sit the 12px label and the 14px mono timer
// at visibly different heights.
qreal baselineFor(const QFontMetricsF& metrics, qreal centreY)
{
    return centreY + (metrics.ascent() - metrics.descent()) / 2.0;
}

qreal drawLabel(QPainter& painter, const QFont& font, QRgb colour, const QString& text,
                qreal left, qreal centreY)
{
    const QFontMetricsF metrics(font);
    painter.setFont(font);
    painter.setPen(QColor::fromRgba(colour));
    painter.drawText(QPointF(left, baselineFor(metrics, centreY)), text);
    return left + metrics.horizontalAdvance(text);
}

// ui.md §8 names `power` as the end-session state glyph (Lucide, 1.5px stroke): a ring with a gap
// at the top and a tick down from the top edge.
void drawPowerGlyph(QPainter& painter, qreal left, qreal centreY, QRgb colour)
{
    const qreal radius = (kGlyphSize / 2.0) - 1.5;
    const QPointF centre(left + (kGlyphSize / 2.0), centreY);
    const QRectF ring(centre.x() - radius, centre.y() - radius, radius * 2.0, radius * 2.0);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor::fromRgba(colour), 1.5, Qt::SolidLine, Qt::RoundCap));

    // drawArc, not a QPainterPath: `arcTo()` on an empty path *connects the arc to the path's
    // current position, which is (0,0)*, so building the ring as a path drags a stray diagonal
    // line from the strip's top-left corner across the card to the glyph. QPainter::drawArc has
    // no such implicit connector. Angles are 1/16th of a degree, counter-clockwise from 3 o'clock;
    // 135° to 405° leaves the gap across the top.
    painter.drawArc(ring, 135 * 16, 270 * 16);

    painter.drawLine(QPointF(centre.x(), centre.y() - radius - 1.5),
                     QPointF(centre.x(), centre.y() - 0.5));
}

bool isUserInput(Uint32 type)
{
    switch (type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
    case SDL_MOUSEMOTION:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEWHEEL:
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
    case SDL_CONTROLLERAXISMOTION:
    case SDL_JOYBUTTONDOWN:
    case SDL_JOYAXISMOTION:
    case SDL_FINGERDOWN:
    case SDL_FINGERMOTION:
    case SDL_FINGERUP:
        return true;
    default:
        return false;
    }
}

} // namespace

HudOverlay::HudOverlay() = default;

HudOverlay::~HudOverlay()
{
    endSession();
}

void HudOverlay::setClock(Clock clock)
{
    if (clock) {
        m_clock = std::move(clock);
    }
}

void HudOverlay::setPublisher(Publisher publisher)
{
    m_publisher = std::move(publisher);
}

int HudOverlay::stripHeight()
{
    return kStripHeight;
}

qint64 HudOverlay::autoHideMs()
{
    return kAutoHideMs;
}

qint64 HudOverlay::nowMs() const
{
    if (m_clock) {
        return m_clock();
    }
    return QDateTime::currentMSecsSinceEpoch();
}

QString HudOverlay::timerText() const
{
    return formatDuration(m_elapsedSeconds.load());
}

QImage HudOverlay::renderStripAt(qint64 elapsedSeconds, int displayWidth) const
{
    const QString timer = formatDuration(elapsedSeconds);
    const QString live = QString::fromLatin1(kLiveLabel);
    const QString elapsedLabel = QString::fromLatin1(kElapsedLabel);
    const QString endSession = QString::fromLatin1(kEndSessionLabel);
    const QString keys = QString::fromLatin1(kEndSessionKeys);

    const QFont liveFont = labelFont(QFont::Medium);
    const QFont mutedFont = labelFont(QFont::Normal);
    const QFont timerFont = monoFont(kMonoPx);
    // The key combination is a technical identifier, so it takes the mono face - at the label
    // size, because it is a label. Two existing tokens composed, not a new size.
    const QFont keysFont = monoFont(kLabelPx);

    const QFontMetricsF liveMetrics(liveFont);
    const QFontMetricsF mutedMetrics(mutedFont);
    const QFontMetricsF timerMetrics(timerFont);
    const QFontMetricsF keysMetrics(keysFont);

    const qreal endSessionW = mutedMetrics.horizontalAdvance(endSession);
    const qreal contentWidth = kPadX + kDotDiameter + kGap + liveMetrics.horizontalAdvance(live)
                               + kGroupGap + mutedMetrics.horizontalAdvance(elapsedLabel) + kGap
                               + timerMetrics.horizontalAdvance(timer) + kGroupGap + kGlyphSize
                               + (kGap * 0.75) + endSessionW + kGap
                               + keysMetrics.horizontalAdvance(keys) + kPadX;

    const int cardWidth = int(std::ceil(contentWidth));
    const int width = std::max(cardWidth, displayWidth);
    const qreal centreY = kStripHeight / 2.0;

    // Painted premultiplied - Qt's native raster format and its fast path - then converted once
    // at the end. The renderers blend with SRC_ALPHA/INV_SRC_ALPHA and apply no alpha
    // manipulation, so the surface handed over must carry *straight* alpha.
    QImage image(width, kStripHeight, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);

        // Half-pixel inset so the 1px border lands inside the image rather than on its edge.
        painter.setPen(QPen(QColor::fromRgba(kCardBorder), 1.0));
        painter.setBrush(QColor::fromRgba(kCardFill));
        painter.drawRoundedRect(QRectF(0.5, 0.5, cardWidth - 1.0, kStripHeight - 1.0), kRadius,
                                kRadius);

        qreal x = kPadX;

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor::fromRgba(kLiveDot));
        painter.drawEllipse(QRectF(x, centreY - (kDotDiameter / 2.0), kDotDiameter, kDotDiameter));
        x += kDotDiameter + kGap;

        x = drawLabel(painter, liveFont, kTextPrimary, live, x, centreY);
        x += kGroupGap;

        x = drawLabel(painter, mutedFont, kTextMuted, elapsedLabel, x, centreY);
        x += kGap;

        x = drawLabel(painter, timerFont, kTextPrimary, timer, x, centreY);
        x += kGroupGap;

        drawPowerGlyph(painter, x, centreY, kTextMuted);
        x += kGlyphSize + (kGap * 0.75);

        const qreal endLeft = x;
        x = drawLabel(painter, mutedFont, kTextMuted, endSession, x, centreY);

        // Hairline between the affordance's label and its binding, so the two read as one
        // legend rather than as two unrelated strings.
        painter.setPen(QPen(QColor::fromRgba(kHairline), 1.0));
        painter.drawLine(QPointF(x + (kGap / 2.0), centreY - 11.0),
                         QPointF(x + (kGap / 2.0), centreY + 11.0));
        Q_ASSERT(x >= endLeft);

        x += kGap;
        drawLabel(painter, keysFont, kTextMuted, keys, x, centreY);
    }

    return image.convertToFormat(QImage::Format_ARGB32);
}

void HudOverlay::publishStrip()
{
    if (!m_publisher) {
        return;
    }

    // Production passes 0: the card is anchored at the overlay's bottom-left origin, where
    // overlay pixels are 1:1 with swapchain pixels, so no display size is needed. The padding
    // parameter exists because a card placed anywhere else would need the strip to reach that
    // x - ADR-0045 records the technique; Phase 3 does not use it.
    const QImage image = renderStripAt(m_elapsedSeconds.load(), 0);
    if (image.isNull()) {
        return;
    }

    // No copy: the surface points straight at the image's buffer, so the image must outlive the
    // publisher call. It does - the manager uploads the pixels into a D3D11 texture
    // synchronously inside that call before freeing the surface.
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(const_cast<uchar*>(image.constBits()),
                                                             image.width(), image.height(), 32,
                                                             image.bytesPerLine(),
                                                             SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) {
        qCWarning(seathubHud) << "could not wrap the HUD bitmap:" << SDL_GetError();
        return;
    }

    m_publisher(surface);
}

void HudOverlay::hideStrip()
{
    if (m_publisher) {
        m_publisher(nullptr);
    }
}

void HudOverlay::beginSession()
{
    // A session that starts while one is already running must not inherit the previous clock.
    endSession();

    const qint64 now = nowMs();
    m_sessionStartedMs.store(now);
    m_lastActivityMs.store(now);
    m_elapsedSeconds.store(0);
    m_publishedVisible.store(false);
    m_sessionActive.store(true);

    // Auto-hide needs to observe input, and event watches need SDL's *events* subsystem. If the
    // watch cannot be installed, the HUD stays up for the whole session instead of hiding itself
    // permanently four seconds in - D-05 makes the overlay a soft dependency, so degrading to
    // "always visible" is the right failure.
    //
    // The gate is `SDL_INIT_EVENTS`, not `SDL_INIT_TIMER` and not "SDL was initialised at all":
    // measured with this SDL2 build, `SDL_Init(SDL_INIT_VIDEO)` - what the engine does - leaves
    // `SDL_WasInit(SDL_INIT_EVENTS)` set but `SDL_WasInit(SDL_INIT_TIMER)` clear, even though
    // `SDL_AddTimer()` works perfectly afterwards. Gating the heartbeat on `SDL_INIT_TIMER`
    // would therefore have silently frozen the timer for every real session.
    if (SDL_WasInit(SDL_INIT_EVENTS) != 0) {
        SDL_AddEventWatch(&HudOverlay::watchEvents, this);
        m_watchingEvents = true;
        m_autoHide = true;
    }
    else {
        qCWarning(seathubHud) << "SDL's events subsystem is not initialised, so the HUD cannot"
                                 " observe input; it will stay visible for the whole session";
    }

    m_visible.store(true);
    publishStrip();
    m_publishedVisible.store(true);

    // SDL's timer thread, not a QTimer: the streaming loop suspends Qt processing for the whole
    // session, so a Qt timer would never fire. Same precondition as the engine's own
    // SDL_AddTimer users in `app/streaming/input/`. Failure is reported, not fatal: without ticks
    // the HUD simply stays as it was published.
    if (m_watchingEvents || SDL_WasInit(0) != 0) {
        m_timer = SDL_AddTimer(kTickMs, &HudOverlay::onTimer, this);
        if (m_timer == 0) {
            qCWarning(seathubHud) << "could not start the HUD heartbeat:" << SDL_GetError();
        }
    }
}

void HudOverlay::endSession()
{
    if (m_timer != 0) {
        SDL_RemoveTimer(m_timer);
        m_timer = 0;
    }

    if (m_watchingEvents) {
        SDL_DelEventWatch(&HudOverlay::watchEvents, this);
        m_watchingEvents = false;
    }

    m_sessionActive.store(false);

    if (m_publishedVisible.load()) {
        hideStrip();
        m_publishedVisible.store(false);
    }

    m_visible.store(false);
}

void HudOverlay::noteActivity()
{
    m_lastActivityMs.store(nowMs());
}

void HudOverlay::tick()
{
    if (!m_sessionActive.load()) {
        return;
    }

    const qint64 now = nowMs();
    m_elapsedSeconds.store((now - m_sessionStartedMs.load()) / 1000);

    const bool wanted = !m_autoHide || (now - m_lastActivityMs.load()) < kAutoHideMs;
    m_visible.store(wanted);

    if (wanted) {
        // Re-published every tick while visible, not only when the second changes: the renderer
        // keeps the last texture it was handed, so one publish a second is what keeps the timer
        // on screen current, and re-publishing also repairs a texture lost to a swapchain
        // recreation.
        publishStrip();
    }
    else if (m_publishedVisible.load()) {
        hideStrip();
    }

    m_publishedVisible.store(wanted);
}

Uint32 SDLCALL HudOverlay::onTimer(Uint32, void* param)
{
    auto* self = static_cast<HudOverlay*>(param);
    self->tick();

    // Stop the timer when the session is over; `endSession()` removes it too, this is only the
    // self-stopping path for a session that ended without it being called.
    return self->m_sessionActive.load() ? kTickMs : 0;
}

int SDLCALL HudOverlay::watchEvents(void* userdata, SDL_Event* event)
{
    auto* self = static_cast<HudOverlay*>(userdata);
    if (isUserInput(event->type)) {
        self->noteActivity();
    }

    // The return value of an event watch is ignored; this is an observer, not a filter, and it
    // must never consume an event - the engine's input handling owns them.
    return 0;
}
