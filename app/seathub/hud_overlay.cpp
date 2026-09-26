#include "hud_overlay.h"

#include <QLoggingCategory>

#include <cstring>

Q_LOGGING_CATEGORY(seathubHud, "seathub.hud")

namespace {

constexpr int kTickMs = 1000;

// CR-03 (ADR-0045): the surface owns its pixels - a fresh allocation and a byte-for-byte row
// copy, never a view onto `image`'s own buffer. `OsdCompositor`'s own private
// `toOwnedArgbSurface()` is the same pattern, kept here rather than exposed because this class
// already owned this exact code before the compositor existed.
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

    const int rowBytes = image.width() * 4;
    SDL_LockSurface(surface);
    for (int y = 0; y < image.height(); ++y) {
        memcpy(static_cast<uchar*>(surface->pixels) + (y * surface->pitch),
               image.constScanLine(y), static_cast<size_t>(rowBytes));
    }
    SDL_UnlockSurface(surface);
    return surface;
}

} // namespace

HudOverlay::HudOverlay()
{
    // The default monotonic clock (`nowMs()`, with no `setClock()` injected) - started once here,
    // never restarted, so `elapsed()` is a stable "ms since this object was constructed" for the
    // whole process lifetime.
    m_monotonicClock.start();
}

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

qint64 HudOverlay::nowMs() const
{
    if (m_clock) {
        return m_clock();
    }
    // D-11/T-06.6-61: monotonic, not wall time (see the header's own comment on `m_monotonicClock`).
    return m_monotonicClock.elapsed();
}

void HudOverlay::publishComposedFrame(const QImage& composed)
{
    if (!m_publisher || composed.isNull()) {
        return;
    }

    // CR-03: the surface owns its pixels, and the pixels are copied out of `composed` here. The
    // renderer contract (`OverlayManager::updateOverlaySurface`) is that delivery is asynchronous
    // and ownership transfers in every case, so the buffer handed over has to stay valid until
    // whatever consumes it has consumed it - or refused it and freed it. `QImage::Format_ARGB32`
    // and `SDL_PIXELFORMAT_ARGB8888` are both 0xAARRGGBB words with straight (non-premultiplied)
    // alpha, so each row copies exactly, byte for byte.
    SDL_Surface* surface = toOwnedArgbSurface(composed);
    if (surface == nullptr) {
        qCWarning(seathubHud) << "could not allocate the OSD compositor bitmap:" << SDL_GetError();
        return;
    }

    m_publisher(surface);
}

void HudOverlay::publishNow()
{
    // See this class's own header comment: whatever the compositor has to draw - Time left, an
    // engine status line the hook recorded, or both - is republished every tick; with nothing to
    // draw, the slot is hidden exactly once (`m_publishedVisible` guards the repeat).
    const QImage composed = m_compositor.composedBottom();
    if (!composed.isNull()) {
        publishComposedFrame(composed);
        m_publishedVisible.store(true);
        return;
    }

    if (m_publishedVisible.exchange(false)) {
        hidePublished();
    }
}

void HudOverlay::hidePublished()
{
    if (m_publisher) {
        m_publisher(nullptr);
    }
}

void HudOverlay::beginSession()
{
    // A session that starts while one is already running must not inherit the previous state.
    endSession();

    {
        // D-11/D-21: a new session has no previous fresh read, so the first one this session can
        // never be read as a top-up, and its own reminder can fire fresh.
        std::lock_guard<std::mutex> lock(m_warnMutex);
        m_credit = -1;
        m_timeLeftState = TimeLeftState::Hidden;
        m_reminderFired = false;
        m_reminderStartedMs = 0;
        m_prevReading = -1;
    }
    // A new session starts with Time left invisible too (D-11: the pre-stream floor is 15
    // minutes, A-33 #24, so a real session never starts with it visible; reset defensively so a
    // reused `HudOverlay` cannot carry a previous session's Time left into this one).
    m_compositor.setTimeLeft({false, 0, false});
    m_sessionActive.store(true);

    // The window-size event watch (D-14): a best-effort improvement over `tick()`'s own
    // `SDL_GetWindowSize()` fallback (`syncCompositorWindowSize()`), never required for
    // correctness - only for how soon a resize reaches the compositor's own sizing.
    if (SDL_WasInit(SDL_INIT_EVENTS) != 0) {
        SDL_AddEventWatch(&HudOverlay::watchEvents, this);
        m_watchingEvents = true;
    }
    else {
        qCWarning(seathubHud) << "SDL's events subsystem is not initialised, so Time left's size"
                                 " can only follow the tick's own fallback poll, not window-resize"
                                 " events";
    }

    // SDL's timer thread, not a QTimer: the streaming loop suspends Qt processing for the whole
    // session, so a Qt timer would never fire. Same precondition as the engine's own
    // SDL_AddTimer users in `app/streaming/input/`. Failure is reported, not fatal: without ticks
    // Time left simply stays as it was last published.
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

    if (m_publishedVisible.exchange(false)) {
        hidePublished();
    }

    {
        // Time left (Critical) stays "until the session ends" (D-11); this is that end.
        std::lock_guard<std::mutex> lock(m_warnMutex);
        m_timeLeftState = TimeLeftState::Hidden;
        m_reminderFired = false;
        m_reminderStartedMs = 0;
        m_prevReading = -1;
    }
    m_compositor.setTimeLeft({false, 0, false});
}

void HudOverlay::seedCreditMinutes(qint64 minutes)
{
    if (minutes < 0) {
        return;
    }
    // D-11/D-16: the seed never reaches the compositor - only a fresh read
    // (`noteCreditMinutes()`, via `applyTimeLeftReading()`) does - so there is nothing to publish
    // here; only the last-known balance moves.
    std::lock_guard<std::mutex> lock(m_warnMutex);
    m_credit = minutes;
}

void HudOverlay::applyTimeLeftReading(qint64 minutes)
{
    // The whole compute-and-publish operation runs under one lock (the class's own Threading
    // note): `tick()`'s reminder-expiry branch does the same, so whichever of the two writers
    // acquires the lock second always publishes from the freshest state - never a stale decision
    // computed before the other writer's own update.
    std::lock_guard<std::mutex> lock(m_warnMutex);

    // D-21: a fresh read above the previous one is a top-up. Time left goes away, and both rules
    // below then run again on this same read (the re-run) - the only way "goes away" is
    // observable, since the rule is stated on the balance alone. The very first fresh read of a
    // session (`m_prevReading < 0`) is never a top-up: there is nothing yet to have risen above.
    if (m_prevReading >= 0 && minutes > m_prevReading) {
        m_timeLeftState = TimeLeftState::Hidden;
        m_reminderFired = false;
    }
    m_prevReading = minutes;

    if (minutes <= kStaysMinutes) {
        // D-11/D-20: Critical from any state, cutting a running reminder short (11 straight to 4
        // goes red at once; a top-up landing at or below five shows red at once too).
        m_timeLeftState = TimeLeftState::Critical;
        m_reminderFired = true;
    }
    else if (minutes <= kReminderMinutes && !m_reminderFired) {
        // D-11: the first fresh read at or below ten minutes, once per armed period.
        m_timeLeftState = TimeLeftState::Reminder;
        m_reminderFired = true;
        m_reminderStartedMs = nowMs();
    }
    // Otherwise the state stays exactly as it was: `Hidden` above the reminder with nothing fired
    // yet, or `Reminder`/`Rested` already decided by an earlier read this armed period.

    const bool visible = m_timeLeftState == TimeLeftState::Reminder
        || m_timeLeftState == TimeLeftState::Critical;
    const bool critical = m_timeLeftState == TimeLeftState::Critical;

    // In `Reminder`/`Critical` the number updates to this read; hidden otherwise (D-16: whole
    // minutes only, so 0 rather than the stale value is the only sane thing to publish while
    // nothing is shown).
    m_compositor.setTimeLeft({visible, visible ? minutes : 0, critical});
}

void HudOverlay::noteCreditMinutes(qint64 minutes)
{
    // Not a balance, or not a session: nothing to show. A read that lands after the stream ended
    // must not leave anything armed for the next one.
    if (minutes < 0 || !m_sessionActive.load()) {
        return;
    }

    // D-11/D-16/D-20/D-21: the owner's full Time-left appear/disappear rule, evaluated on every
    // fresh read - `seedCreditMinutes()` never reaches here, so the pre-stream seed never shows it
    // (D-16). The result surfaces on the next `tick()`, not immediately from here - `tick()` is
    // what decides whether the compositor owns the slot each cycle.
    applyTimeLeftReading(minutes);

    std::lock_guard<std::mutex> lock(m_warnMutex);
    m_credit = minutes;
}

qint64 HudOverlay::creditMinutes() const
{
    std::lock_guard<std::mutex> lock(m_warnMutex);
    return m_credit;
}

void HudOverlay::syncCompositorWindowSize()
{
    // See this method's own header comment: the fallback path, for whenever nothing has told the
    // compositor yet. The focused SDL window is the stream window (nothing else in the process is
    // an SDL window while streaming), the same `SDL_GetWindowSize` the D3D11 renderer sizes its
    // swapchain from.
    SDL_Window* window = SDL_GetKeyboardFocus();
    if (window == nullptr) {
        window = SDL_GetMouseFocus();
    }
    if (window == nullptr) {
        return;
    }

    int w = 0;
    int h = 0;
    SDL_GetWindowSize(window, &w, &h);
    if (w > 0 && h > 0) {
        m_compositor.setWindowSize(w, h);
    }
}

void HudOverlay::tick()
{
    if (!m_sessionActive.load()) {
        return;
    }

    const qint64 now = nowMs();

    // D-14: kept current every tick, ahead of the compositor's own read below - `watchEvents()`'s
    // window-size branch is the preferred, event-driven path and normally keeps this current
    // already; this call is the fallback for whenever nothing has told the compositor yet.
    syncCompositorWindowSize();

    // D-11: the Reminder window closes on the monotonic tick, not on the next fresh read - a
    // customer whose balance stops changing (no further liveness read arrives, or it repeats the
    // same value) must still see the one-minute reminder disappear on schedule. See
    // `applyTimeLeftReading()`'s own comment: the state check and the publish share one lock.
    {
        std::lock_guard<std::mutex> lock(m_warnMutex);
        if (m_timeLeftState == TimeLeftState::Reminder
            && (now - m_reminderStartedMs) >= kReminderMs) {
            m_timeLeftState = TimeLeftState::Rested;
            m_compositor.setTimeLeft({false, 0, false});
        }
    }

    // Re-published every tick, not only when the second changes: the renderer keeps the last
    // texture it was handed, so one publish a second is what keeps Time left on screen current,
    // and re-publishing also repairs a texture lost to a swapchain recreation (ADR-0045 risk 3).
    publishNow();
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
    if (event->type == SDL_WINDOWEVENT
        && (event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED
            || event->window.event == SDL_WINDOWEVENT_RESIZED)) {
        // D-14: the compositor's own window size, learned on the correct thread (the event watch
        // runs synchronously from whichever thread pumps SDL's event queue, never the engine's
        // decoder thread the compositor's own `rasterizeInstance()` must query nothing from).
        if (event->window.data1 > 0 && event->window.data2 > 0) {
            self->m_compositor.setWindowSize(event->window.data1, event->window.data2);
        }
    }

    // The return value of an event watch is ignored; this is an observer, not a filter, and it
    // must never consume an event - the engine's input handling owns them.
    return 0;
}
