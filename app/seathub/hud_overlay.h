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

// Plan 14 (D-09/D-11/D-13): the compositor this class now owns and publishes through.
#include "osd_compositor.h"

// The SeatHub in-session HUD: the D-56 Phase 3 subset (session duration timer + the End session
// affordance), with the D-04 auto-hide, and since Phase 5 (CUST-15, D-21) the customer's remaining
// credit and the two low-balance warning cards. Connection quality is Moonlight's own stats text,
// not drawn here.
//
// Since Plan 14 (D-09/D-11/D-13) this class also owns one `OsdCompositor` (`compositor()`),
// registered by `SeatHubClient` as the engine's text rasteriser. [Decided by executor, owner to
// review, Plan 14]: `OverlayManager` holds exactly one surface per overlay slot, so two publishes
// in the same cycle never coexist on screen - the second only replaces the first. Every publish
// site here (`publishNow()`) therefore checks the compositor first: whenever it has anything to
// draw (Time left visible, or an engine status line recorded, D-13), it owns the slot outright -
// no strip, no card, no sentence or End-session hint beside it (D-11/D-12/D-20) - and only when it
// has nothing does the legacy strip/card path below run. Plan 22 removes that legacy path and
// applies the owner's full Time-left appear/disappear rule; until then both paths exist together.
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
// The remaining credit and the warning cards (Phase 5) are written from a third thread: the
// liveness timer's, which is the control-plane thread and the only one still running a Qt event
// loop during a stream. A wallet read reaches `noteCreditMinutes()` straight from that thread - it
// must never be marshalled through the facade's thread, which is suspended for the whole stream and
// would leave the credit frozen and every warning unfired. That state is several fields that have
// to change together (the balance, which thresholds have fired, which card is up and since when),
// so it sits behind one small mutex rather than in separate atomics; nothing is done under it but
// reading and writing those fields, and the render and the publisher run outside it.
//
// `setClock()` and `setPublisher()` are configuration, not state: set them once before the first
// `beginSession()`, never while a session is running.
class HudOverlay
{
public:
    // `docs/spec/timing.md`: "In-stream Time left, reminder" (a fresh read at or below 10 minutes,
    // shown white for one minute), "In-stream Time left, final" (a fresh read at or below 5
    // minutes, red to the end) and the top-up row (D-11, D-16, D-20, D-21). Plan 22 retires the
    // CUST-15 low-balance-card thresholds this class used to carry under the names
    // `kWarnMinutes`/`kCriticalMinutes`/`kTenMinuteCardMs`, and Plan 14's own separate
    // `kTimeLeftThresholdMinutes`/`kTimeLeftCriticalMinutes` (`hud_overlay.cpp`), into these three:
    // one source for 10, 5 and 60000 from here on. `tst_hud_bitmap` pins them here so a change of
    // the spec row has to change this file too.
    static constexpr qint64 kReminderMinutes = 10;
    static constexpr qint64 kStaysMinutes = 5;
    static constexpr qint64 kReminderMs = 60000;

    /// Which low-balance card is on screen. At most one: the two-minute card replaces the
    /// ten-minute one.
    enum class Card { None, TenMinutes, TwoMinutes };

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
    // (and `readyForDeletion`, defensively) stops it - D-56's stated interval. A new session also
    // starts with no balance known, no threshold fired and no card up.
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

    // What the customer has left, in whole minutes, as the server's wallet says it (CUST-15,
    // `client.md` Wallet authority): never derived from the lease's advisory horizon and never
    // computed here.
    //
    // `seedCreditMinutes` shows the last balance read before the stream (`screens.md` 25: at
    // stream start the HUD shows it) and fires nothing: that value can be old, and a card fired on
    // an old value would use up a threshold the real balance has not reached. `noteCreditMinutes`
    // is a fresh read - one arrives on every liveness report - and is what drives the two cards. A
    // read that fails is simply not reported: the HUD keeps its last value, fires nothing and shows
    // no error text. Negative values are not balances and are ignored. Any thread may call either.
    void seedCreditMinutes(qint64 minutes);
    void noteCreditMinutes(qint64 minutes);
    /// -1 until a value is known.
    qint64 creditMinutes() const;

    /// The card on screen now, and how many times each has fired this session (each is 0 or 1;
    /// the count exists so a test can say "exactly once" instead of "at least once").
    Card activeCard() const;
    int warningsFired(Card card) const;

    /// The stream window's width in pixels, which the bottom-right placement needs (ADR-0045: the
    /// overlay is anchored bottom-left and pixels map 1:1, so a card at the right edge is drawn
    /// inside a bitmap as wide as the frame). Left alone it is learnt from SDL's own window events
    /// and, failing that, from the focused window - the same `SDL_GetWindowSize` the D3D11 renderer
    /// sizes its swapchain from. A test (or a caller that knows better) sets it directly; that also
    /// stops it being learnt. 0 means unknown, and a card is then drawn next to the strip instead.
    void setDisplayWidth(int pixels);
    int displayWidth() const { return m_displayWidth.load(); }

    // The strip as it would look with `elapsedSeconds` on the timer. `displayWidth` of 0 sizes
    // the strip to its content, which is what production uses: the card is anchored at the
    // overlay's own bottom-left origin and overlay pixels map 1:1 onto swapchain pixels, so the
    // card's position never depends on knowing the display size. A larger value pads the strip
    // with transparent pixels - the technique ADR-0045 records for placing the card anywhere
    // other than the left edge without editing a renderer file. Shows the remaining credit when
    // one is known, and never a warning card.
    QImage renderStripAt(qint64 elapsedSeconds, int displayWidth) const;

    // One published frame: the strip when `strip` is set, and `card` at the bottom right of a
    // bitmap `displayWidth` wide (or, with no width known, beside the strip). Everything else is
    // transparent. The two-minute card is drawn last, so where a very small window makes them meet
    // it wins.
    QImage renderFrame(qint64 elapsedSeconds, int displayWidth, bool strip, Card card) const;

    /// The sentence a card carries, exactly as `copy.md` "In session" has it.
    static QString warningText(Card card);

    /// True while the strip is up (it auto-hides). A warning card is separate: see `isCardShown`.
    bool isVisible() const { return m_visible.load(); }
    bool isCardShown() const { return activeCard() != Card::None; }
    qint64 elapsedSeconds() const { return m_elapsedSeconds.load(); }
    QString timerText() const; // HH:MM:SS

    static int stripHeight();
    static qint64 autoHideMs();

    /// D-09/D-11/D-13 (Plan 14): the compositor `SeatHubClient` registers as the engine's text
    /// rasteriser (`&OsdCompositor::rasterize`, with this object's address as its context) and
    /// feeds window-size and stats-label state to. See this class's own header comment for the
    /// one-publish-per-slot rule every publish site here follows once it has anything to draw.
    OsdCompositor& compositor() { return m_compositor; }

private:
    /// D-11/D-16/D-20/D-21 (Plan 22), driven by fresh reads (`noteCreditMinutes()`) and the
    /// monotonic tick (`tick()`): `Hidden` (above the reminder), `Reminder` (white, for
    /// `kReminderMs`), `Rested` (reminder spent, still above `kStaysMinutes`) and `Critical` (red,
    /// from `kStaysMinutes` to the end of the session). See `applyTimeLeftReading()`.
    enum class TimeLeftState { Hidden, Reminder, Rested, Critical };

    /// Evaluates one fresh read against the state machine above (`06.6-RESEARCH-FORK.md` §6's
    /// transition table) and publishes the result to `m_compositor.setTimeLeft()`. Called from
    /// `noteCreditMinutes()` only - `seedCreditMinutes()` never reaches here (D-16: the pre-stream
    /// seed never shows Time left).
    void applyTimeLeftReading(qint64 minutes);

    void publishFrame(bool strip);
    /// The compositor's own owned-surface publish (the same CR-03 pattern `publishFrame()` uses),
    /// for whichever caller decided `composed` is what should be on screen this cycle.
    void publishComposedFrame(const QImage& composed);
    /// The one publish decision every site (`tick()`, `noteCreditMinutes()`, `seedCreditMinutes()`,
    /// `setReconnecting()`) routes through: the compositor owns the slot outright when it has
    /// anything to draw, else the legacy strip/card frame with `wantStrip` as `renderFrame()`'s own
    /// `strip` argument.
    void publishNow(bool wantStrip);
    void hideStrip();
    qint64 nowMs() const;
    /// The width the next frame is placed against: the pinned one, else what the window events
    /// have said, else the focused SDL window's, else 0.
    int resolveDisplayWidth();
    /// D-14 (Plan 14 Task 2): keeps the compositor's own window size current, independent of the
    /// legacy `displayWidth` pin above (a separate concern - positioning a card, not sizing text).
    /// The window-size event branch in `watchEvents()` below is the preferred, event-driven path
    /// (the correct thread, no query needed); this is the fallback for whenever nothing has told
    /// the compositor yet - a session's first tick, before any resize, or a window that never took
    /// focus. `SDL_GetWindowSize()` on the SDL thread only (never from the compositor's own
    /// `rasterizeInstance()`, which runs on the engine's decoder thread and must query nothing -
    /// `osd_compositor.h`'s own header comment, Pitfall 11).
    void syncCompositorWindowSize();
    /// Ends the ten-minute card once its lifetime has passed. True when the card list changed.
    bool expireTenMinuteCard(qint64 now);

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

    // The stream window's width; see `setDisplayWidth`. Written by SDL's event watch (the main
    // thread) and by `setDisplayWidth`, read on whichever thread publishes.
    std::atomic<int> m_displayWidth{0};
    std::atomic<bool> m_displayWidthPinned{false};

    // The credit and the warning state, which change together (see the Threading note).
    mutable std::mutex m_warnMutex;
    qint64 m_credit = -1;
    bool m_tenFired = false;
    bool m_twoFired = false;
    int m_tenFiredCount = 0;
    int m_twoFiredCount = 0;
    Card m_card = Card::None;
    qint64 m_tenShownAtMs = 0;

    // D-11/D-16/D-20/D-21 (Plan 22): Time left's own state, guarded by `m_warnMutex` for the same
    // reason the legacy warning fields above are - it changes on a fresh read from the liveness
    // thread and is read from `tick()` on the timer thread.
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
