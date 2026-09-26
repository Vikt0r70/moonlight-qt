#pragma once

// SDL must not redefine `main` (see `app/main.cpp`). The fork's other bridge headers avoid
// `SDL.h` entirely for that reason; this one needs `SDL_Surface` in its public signature, so it
// protects itself instead: whatever includes it gets the define before SDL is pulled in.
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>

#include <QElapsedTimer>
#include <QImage>
#include <QString>

#include <atomic>
#include <functional>
#include <mutex>

// The compositor this class owns and publishes through (D-09/D-11/D-13, Plan 14).
#include "osd_compositor.h"

// The SeatHub in-session overlay. Since Plan 22 this is exactly the D-11 Time-left rule (bottom
// right, numbers only, the owner's appear/disappear rule) composited by `OsdCompositor` (Plan
// 14): the D-56 strip (session duration, the End-session hint), the D-04 auto-hide and the
// CUST-15 low-balance warning cards this class carried through Phase 5 and Plan 14's transitional
// one-publish-per-slot design are gone (D-12). Connection quality is Moonlight's own stats text
// (D-13); it reaches the screen through the same compositor via the engine's own text-rasteriser
// hook (`SeatHubClient` registers `OsdCompositor::rasterize`), a path this class never calls into
// directly.
//
// This class owns one `OsdCompositor` (`compositor()`) and republishes whatever it has to draw -
// Time left, an engine status line the hook recorded, or both - once a second on its own
// heartbeat, so a wallet-driven change (the engine's own `OverlayManager` never publishes on its
// own for one) still reaches the screen, and a texture lost to a swapchain recreation is repaired
// within the second (ADR-0045 risk 3).
//
// The overlay is not QML. `ADR-0045` records why: the Qt window is hidden for the whole session
// (D-01), so `QQuickItem::grabToImage()` is structurally impossible, and the engine already
// composites overlays into the stream's own swapchain (`D3D11VARenderer::renderFrame()` draws
// them immediately before `Present()`). Drawing itself lives in `osd_renderer.cpp`/
// `osd_compositor.cpp`; this class hands the compositor's `QImage` to an injected publisher,
// wrapped as an `SDL_PIXELFORMAT_ARGB8888` surface. Production injects the publisher that
// composites through `OverlayManager` - `SeatHubClient` builds it, so this file stays free of the
// engine header and can be linked into a test with no engine in it. One window, no second
// swapchain, no overlay thread, which is what keeps `STREAM-01` true.
//
// ## Threading
//
// The heartbeat is an `SDL_AddTimer` timer, not a `QTimer`, because the streaming loop suspends
// Qt processing for the whole session ("we want to suspend all Qt processing until the stream is
// over", `session.cpp:1966`). The engine's own input timers use `SDL_AddTimer` for the same
// reason. Its callback runs on SDL's timer thread, so the injected publisher must be safe to call
// from an arbitrary thread (`OverlayManager::updateOverlaySurface()` hands the surface over
// atomically with its renderer notification, explicitly "callable on an arbitrary thread").
//
// Time left is written from a third thread: the liveness timer's, which is the control-plane
// thread and the only one still running a Qt event loop during a stream. A wallet read reaches
// `noteCreditMinutes()` straight from that thread - it must never be marshalled through the
// facade's thread, which is suspended for the whole stream and would leave Time left frozen and
// never shown. That state (the balance, the reminder/critical machinery) sits behind one small
// mutex rather than in separate atomics, and every write to it publishes the compositor's result
// from inside the same lock (`applyTimeLeftReading()`, the tick's own reminder-expiry branch) so
// a concurrent reader-then-writer race can never publish a stale decision after a fresher one.
//
// `setClock()` and `setPublisher()` are configuration, not state: set them once before the first
// `beginSession()`, never while a session is running.
class HudOverlay
{
public:
    // `docs/spec/timing.md`: "In-stream Time left, reminder" (a fresh read at or below 10 minutes,
    // shown white for one minute), "In-stream Time left, final" (a fresh read at or below 5
    // minutes, red to the end) and the top-up row (D-11, D-16, D-20, D-21). `tst_hud_bitmap` pins
    // them here so a change of the spec row has to change this file too.
    static constexpr qint64 kReminderMinutes = 10;
    static constexpr qint64 kStaysMinutes = 5;
    static constexpr qint64 kReminderMs = 60000;

    // Milliseconds. Injectable so a test can advance time instead of waiting for it: one minute
    // of the reminder window is one minute of real time otherwise.
    using Clock = std::function<qint64()>;

    // Composites one frame - Time left, an engine status line, or both. A null surface means
    // "hide": nothing to draw. A non-null surface is `SDL_PIXELFORMAT_ARGB8888`, owns its own
    // pixels, and stays valid until the renderer consumes it - delivery is asynchronous
    // (`OverlayManager::updateOverlaySurface`), so the publisher must not hold on to it and must
    // not free it. Ownership transfers in every case, success or failure - the contract of
    // `OverlayManager::updateOverlaySurface()`.
    using Publisher = std::function<bool(SDL_Surface*)>;

    HudOverlay();
    ~HudOverlay();

    HudOverlay(const HudOverlay&) = delete;
    HudOverlay& operator=(const HudOverlay&) = delete;

    void setClock(Clock clock);
    void setPublisher(Publisher publisher);

    // Wired to the session lifecycle: `connectionStarted` starts the clock, `sessionFinished`
    // (and `readyForDeletion`, defensively) stops it. A new session starts with Time left hidden
    // and no balance known.
    void beginSession();
    void endSession();

    // The heartbeat. Public so a caller - or a test without SDL's timer subsystem - can pump it
    // deterministically instead of waiting for the timer thread.
    void tick();

    // What the customer has left, in whole minutes, as the server's wallet says it (`client.md`
    // Wallet authority): never derived from the lease's advisory horizon and never computed here.
    //
    // `seedCreditMinutes` is the last balance read before the stream and never reaches Time left
    // (D-16: that value can be old, and showing it would arm the reminder on a balance the real
    // one has not reached). `noteCreditMinutes` is a fresh read - one arrives on every liveness
    // report - and is what drives Time left's appear/disappear rule (D-11/D-16/D-20/D-21). A read
    // that fails is simply not reported: nothing here moves. Negative values are not balances and
    // are ignored. Any thread may call either.
    void seedCreditMinutes(qint64 minutes);
    void noteCreditMinutes(qint64 minutes);
    /// -1 until a value is known: the last value `seedCreditMinutes`/`noteCreditMinutes` was
    /// given, whichever came later - not necessarily what Time left is currently showing.
    qint64 creditMinutes() const;

    /// D-09/D-11/D-13 (Plan 14): the compositor `SeatHubClient` registers as the engine's text
    /// rasteriser (`&OsdCompositor::rasterize`, with this object's address as its context) and
    /// feeds window-size and stats-label state to.
    OsdCompositor& compositor() { return m_compositor; }

private:
    /// D-11/D-16/D-20/D-21, driven by fresh reads (`noteCreditMinutes()`) and the monotonic tick
    /// (`tick()`): `Hidden` (above the reminder), `Reminder` (white, for `kReminderMs`), `Rested`
    /// (reminder spent, still above `kStaysMinutes`) and `Critical` (red, from `kStaysMinutes` to
    /// the end of the session). See `applyTimeLeftReading()`.
    enum class TimeLeftState { Hidden, Reminder, Rested, Critical };

    /// Evaluates one fresh read against the state machine above (`06.6-RESEARCH-FORK.md` §6's
    /// transition table) and publishes the result to `m_compositor.setTimeLeft()` from inside the
    /// same lock the state mutation happens under (see the class's own Threading note). Called
    /// from `noteCreditMinutes()` only - `seedCreditMinutes()` never reaches here (D-16).
    void applyTimeLeftReading(qint64 minutes);

    /// The compositor's own owned-surface publish (CR-03: a fresh allocation, never a view onto
    /// `composed`'s own buffer), for whichever caller decided `composed` is what should be on
    /// screen this cycle.
    void publishComposedFrame(const QImage& composed);
    /// The one publish decision `tick()` routes through: publish whatever the compositor has to
    /// draw, or hide once (`m_publishedVisible`) when it has nothing.
    void publishNow();
    void hidePublished();
    qint64 nowMs() const;
    /// D-14: keeps the compositor's own window size current. `watchEvents()`'s window-size branch
    /// below is the preferred, event-driven path (the correct thread, no query needed); this is
    /// the fallback for whenever nothing has told the compositor yet - a session's first tick,
    /// before any resize, or a window that never took focus. `SDL_GetWindowSize()` on the SDL
    /// thread only (never from the compositor's own `rasterizeInstance()`, which runs on the
    /// engine's decoder thread and must query nothing - `osd_compositor.h`'s own header comment,
    /// Pitfall 11).
    void syncCompositorWindowSize();

    static Uint32 SDLCALL onTimer(Uint32 interval, void* param);
    static int SDLCALL watchEvents(void* userdata, SDL_Event* event);

    Clock m_clock;
    Publisher m_publisher;
    SDL_TimerID m_timer = 0;
    bool m_watchingEvents = false;
    std::atomic<bool> m_sessionActive{false};
    std::atomic<bool> m_publishedVisible{false};

    // The credit and Time-left state, which change together (see the class's own Threading note).
    mutable std::mutex m_warnMutex;
    qint64 m_credit = -1;

    // D-11/D-16/D-20/D-21: Time left's own state, guarded by `m_warnMutex` for the same reason
    // `m_credit` is - it changes on a fresh read from the liveness thread and is read from
    // `tick()` on the timer thread.
    TimeLeftState m_timeLeftState = TimeLeftState::Hidden;
    bool m_reminderFired = false;
    qint64 m_reminderStartedMs = 0;
    /// The last fresh read this session, or -1 before the first one (never set by
    /// `seedCreditMinutes()` - only a fresh read can be "the previous one" for the top-up rule).
    qint64 m_prevReading = -1;

    // The default clock (`nowMs()`, with none injected): a monotonic clock, not wall time, so a
    // system clock change cannot shorten or stretch the one-minute reminder window (T-06.6-61).
    // Started once, at construction; a test that cares injects its own clock via `setClock()`.
    QElapsedTimer m_monotonicClock;

    // D-09/D-11/D-13 (Plan 14): thread-safe on its own (a mutex-guarded `State`, per its own
    // header comment) - no extra locking needed here.
    OsdCompositor m_compositor;
};
