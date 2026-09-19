#pragma once

// SDL must not redefine `main` (see `app/main.cpp`). The fork's other bridge headers avoid
// `SDL.h` entirely for that reason; this one needs `SDL_Surface` in its public signature, so it
// protects itself instead: whatever includes it gets the define before SDL is pulled in.
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>

#include <QImage>
#include <QString>

#include <atomic>
#include <functional>

// The SeatHub in-session HUD: the D-56 Phase 3 subset (session duration timer + the End session
// affordance), with the D-04 auto-hide. Nothing else - no remaining credit, no connection
// quality, no help. Those are Phase 5.
//
// The HUD is not QML. `ADR-0045` records why: the Qt window is hidden for the whole session
// (D-01), so `QQuickItem::grabToImage()` is structurally impossible, and the engine already
// composites overlays into the stream's own swapchain (`D3D11VARenderer::renderFrame()` draws
// them immediately before `Present()`). This class therefore renders the HUD with QPainter into
// a `QImage`, wraps it as an `SDL_PIXELFORMAT_ARGB8888` surface, and hands it to an injected
// publisher. Production injects the publisher that composites through `OverlayManager` -
// `SeatHubClient` builds it, so this file stays free of the engine header and can be linked into
// a test with no engine in it. One window, no second swapchain, no overlay thread, which is what
// keeps `STREAM-01` true.
//
// ## Threading
//
// The heartbeat is an `SDL_AddTimer` timer, not a `QTimer`, because the streaming loop suspends
// Qt processing for the whole session ("we want to suspend all Qt processing until the stream is
// over", `session.cpp:1966`). The engine's own input timers use `SDL_AddTimer` for the same
// reason. Its callback runs on SDL's timer thread, so every piece of state this class shares
// with the main thread is `std::atomic` (`m_autoHide` included - it is written by
// `beginSession()` on the main thread and read by `tick()` on the timer thread, ME-02), and the
// two things it calls out to - rendering a `QImage` with `QPainter`, and the injected publisher -
// are both safe from an arbitrary thread (Qt image painting needs no GUI thread, and
// `OverlayManager::updateOverlaySurface()` hands the surface over atomically with its renderer
// notification explicitly "callable on an arbitrary thread").
//
// `setClock()` and `setPublisher()` are configuration, not state: set them once before the first
// `beginSession()`, never while a session is running.
class HudOverlay
{
public:
    // Milliseconds. Injectable so a test can advance time instead of waiting for it: four
    // seconds of auto-hide is four seconds of real time otherwise.
    using Clock = std::function<qint64()>;

    // Composites one HUD frame. A null surface means "hide": the overlay is disabled and its
    // content dropped, which is what the engine's own `setOverlayState(type, false)` does. A
    // non-null surface is `SDL_PIXELFORMAT_ARGB8888`, owns its own pixels, and stays valid until
    // the renderer consumes it - delivery is asynchronous (`OverlayManager::updateOverlaySurface`),
    // so the publisher must not hold on to it and must not free it. Ownership transfers in every
    // case, success or failure - the contract of `OverlayManager::updateOverlaySurface()`.
    using Publisher = std::function<bool(SDL_Surface*)>;

    HudOverlay();
    ~HudOverlay();

    HudOverlay(const HudOverlay&) = delete;
    HudOverlay& operator=(const HudOverlay&) = delete;

    void setClock(Clock clock);
    void setPublisher(Publisher publisher);

    // Wired to the session lifecycle: `connectionStarted` starts the clock, `sessionFinished`
    // (and `readyForDeletion`, defensively) stops it - D-56's stated interval.
    void beginSession();
    void endSession();

    // Any user input: re-shows the HUD after it auto-hid (D-04, screens.md §25). Called from
    // SDL's event watch, so it only records a timestamp - `tick()` decides what is visible.
    void noteActivity();

    // True while the session's channel is down, so the strip can say so instead of counting
    // elapsed time the customer is not getting (audit F12; `copy.md` §In session, "Connection
    // lost. Reconnecting..."). The attempt counter the deck shows in parentheses is D-56-deferred
    // and is deliberately not rendered. Writes one atomic, so any thread may call it.
    void setReconnecting(bool reconnecting);

    // The heartbeat. Public so a caller - or a test without SDL's timer subsystem - can pump the
    // HUD deterministically instead of waiting for the timer thread.
    void tick();

    // The strip as it would look with `elapsedSeconds` on the timer. `displayWidth` of 0 sizes
    // the strip to its content, which is what production uses: the card is anchored at the
    // overlay's own bottom-left origin and overlay pixels map 1:1 onto swapchain pixels, so the
    // card's position never depends on knowing the display size. A larger value pads the strip
    // with transparent pixels - the technique ADR-0045 records for placing the card anywhere
    // other than the left edge without editing a renderer file.
    QImage renderStripAt(qint64 elapsedSeconds, int displayWidth) const;

    bool isVisible() const { return m_visible.load(); }
    qint64 elapsedSeconds() const { return m_elapsedSeconds.load(); }
    QString timerText() const; // HH:MM:SS

    static int stripHeight();
    static qint64 autoHideMs();

private:
    void publishStrip();
    void hideStrip();
    qint64 nowMs() const;

    static Uint32 SDLCALL onTimer(Uint32 interval, void* param);
    static int SDLCALL watchEvents(void* userdata, SDL_Event* event);

    Clock m_clock;
    Publisher m_publisher;
    SDL_TimerID m_timer = 0;
    bool m_watchingEvents = false;
    // Written on the main thread by `beginSession()`, read on SDL's timer thread by `tick()`:
    // atomic for the same reason every other shared member is (ME-02).
    std::atomic<bool> m_autoHide{false};
    std::atomic<bool> m_sessionActive{false};
    std::atomic<bool> m_visible{false};
    std::atomic<bool> m_publishedVisible{false};
    std::atomic<bool> m_reconnecting{false};
    std::atomic<qint64> m_sessionStartedMs{0};
    std::atomic<qint64> m_lastActivityMs{0};
    std::atomic<qint64> m_elapsedSeconds{0};
};
