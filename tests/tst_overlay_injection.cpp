// Proof (b) of Plan 03-05's overlay spike (ADR-0045): the semantics of the bitmap-injection
// method the fork adds to `app/streaming/video/overlaymanager.h` - the one file under
// `app/streaming/` the D-28 diff gate names as an exception.
//
// A real `OverlayManager` and a mock `Overlay::IOverlayRenderer` are used, so this tests the
// production class rather than a stand-in. It is deliberately a pure unit test: no window, no
// SDL video subsystem, no D3D11 device, no engine `Session` (whose `getOverlayManager()` is
// the only thing the HUD producer needs, and which does not exist in this build until Plan
// 03-03 attaches one).
//
// Every test builds its own manager and renderer rather than sharing members, so a test's
// outcome cannot depend on how many tests ran before it or on what they published.
//
// The font path is intentionally not exercised: `ModeSeven.ttf` lives in the application's
// resource bundle (`app/qml.qrc`), which this test project does not link, so
// `OverlayManager::notifyOverlayUpdated()`'s TTF rasteriser has nothing to load here. That is
// the text overlay's path, not the bitmap path under test.
//
// Build (nothing is on PATH machine-wide - Qt and MSVC are both absolute):
//   call "<VS BuildTools>\VC\Auxiliary\Build\vcvars64.bat"
//   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
//   cd tests && qmake tst_overlay_injection.pro && jom && tst_overlay_injection.exe -o out.txt,txt

#include <QtTest>

#include <atomic>
#include <thread>

// Don't let SDL hook our main function: Qt Test already provides one (the same reason and the
// same fix as `app/main.cpp`). Without this, `QTEST_MAIN` expands to `SDL_main` and the link
// fails with "unresolved external symbol main".
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "streaming/video/overlaymanager.h"
#include "seathub/osd_compositor.h"

namespace {

// The smallest faithful stand-in for the renderer the engine registers
// (`ffmpeg.cpp:633 setOverlayRenderer(m_FrontendRenderer)`). All the production code does with
// it is "tell it the content for this overlay changed, once".
class MockOverlayRenderer : public Overlay::IOverlayRenderer
{
public:
    void notifyOverlayUpdated(Overlay::OverlayType type) override
    {
        ++calls;
        lastType = type;
    }

    int calls = 0;
    Overlay::OverlayType lastType = Overlay::OverlayDebug;
};

constexpr Overlay::OverlayType kType = Overlay::OverlayStatusUpdate;

// A bitmap in the format every renderer's upload path accepts and asserts on.
SDL_Surface* makeBitmap(int width, int height, Uint32 argb)
{
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32,
                                                          SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) {
        return nullptr;
    }
    SDL_FillRect(surface, nullptr, argb);
    return surface;
}

Uint32 pixelAt(const SDL_Surface* surface, int x, int y)
{
    const auto* row = reinterpret_cast<const Uint32*>(
        static_cast<const Uint8*>(surface->pixels) + y * surface->pitch);
    return row[x];
}

// Plan 10 (D-09, ADR-0045 amended 2026-09-26): tolerates anti-aliasing at a glyph's edges - "is
// this colour present" is a nearness test, not an equality test, the same rule
// `tst_osd_render.cpp`'s own `regionHasPixelNear()` documents.
bool surfaceRegionHasPixelNear(const SDL_Surface* surface, int x0, int y0, int x1, int y1,
                               Uint8 r, Uint8 g, Uint8 b, int tolerance = 24)
{
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Uint32 pixel = pixelAt(surface, x, y);
            const Uint8 a = static_cast<Uint8>((pixel >> 24) & 0xFF);
            if (a == 0) {
                continue;
            }
            const Uint8 pr = static_cast<Uint8>((pixel >> 16) & 0xFF);
            const Uint8 pg = static_cast<Uint8>((pixel >> 8) & 0xFF);
            const Uint8 pb = static_cast<Uint8>(pixel & 0xFF);
            if (qAbs(int(pr) - int(r)) <= tolerance && qAbs(int(pg) - int(g)) <= tolerance
                && qAbs(int(pb) - int(b)) <= tolerance) {
                return true;
            }
        }
    }
    return false;
}

// Plan 10, Task 1: a rasteriser that records exactly what it was called with, so
// `rasterizerReceivesRawTextEnabledAndColour` can assert on the hook's own arguments directly -
// independent of `OsdCompositor`, which is exercised separately below.
struct RecordedRasterizerCall
{
    Overlay::OverlayType type;
    QByteArray text;
    bool enabled;
    SDL_Color color;
};

QList<RecordedRasterizerCall> g_recordedRasterizerCalls;

SDL_Surface* recordingRasterizer(Overlay::OverlayType type, const char* text, bool enabled,
                                 SDL_Color color, void* context)
{
    Q_UNUSED(context);
    g_recordedRasterizerCalls.append(
        RecordedRasterizerCall{ type, QByteArray(text != nullptr ? text : ""), enabled, color });
    return nullptr;
}

// Plan 10, Task 2: a rasteriser that always returns a valid surface in a pixel format
// `OverlayManager` does not accept, so the hook's own refusal rule (not `OsdCompositor`'s) is
// what is under test.
SDL_Surface* foreignFormatRasterizer(Overlay::OverlayType, const char*, bool, SDL_Color, void*)
{
    return SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 32, SDL_PIXELFORMAT_RGBA8888);
}

// Plan 10, Task 2: a rasteriser that always returns a valid ARGB8888 surface - proves that a
// disabled slot is refused by the hook even when the rasteriser itself returns something the
// renderer would otherwise accept.
SDL_Surface* fixedArgbRasterizer(Overlay::OverlayType, const char*, bool, SDL_Color, void*)
{
    return SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 32, SDL_PIXELFORMAT_ARGB8888);
}

// Phase 5 plan 12 (CUST-17/D-23/D-26): the eleven lines Moonlight 6.1.0's own
// `stringifyVideoStats()` writes (`git show v6.1.0:app/streaming/video/ffmpeg.cpp`), verbatim
// including each line's own trailing colon, in the engine's own output order - the "single list
// the test pins" the plan's own verify checks against the live upstream source, so a rename here
// is caught there. `settings_bridge.cpp`'s `kStatsToggles` carries the same eleven labels (without
// the colon, D-26); this is the compositor test's own copy for building realistic raw-text
// fixtures, not a second source of truth for what the labels mean.
constexpr const char* kAllDebugLinesRaw =
    "Video stream: 1920x1080 60.00 FPS (Codec: H.264)\n"
    "Incoming frame rate from network: 60.00 FPS\n"
    "Decoding frame rate: 60.00 FPS\n"
    "Rendering frame rate: 60.00 FPS\n"
    "Host processing latency min/max/average: 1.0/2.0/1.5 ms\n"
    "Frames dropped by your network connection: 0.10%\n"
    "Frames dropped due to network jitter: 0.05%\n"
    "Average network latency: 12 ms (variance: 1 ms)\n"
    "Average decoding time: 3.20 ms\n"
    "Average frame queue delay: 0.80 ms\n"
    "Average rendering time (including monitor V-sync latency): 4.10 ms\n";

// The same eleven labels (no colon), matching `settings_bridge.cpp`'s `kStatsToggles` exactly -
// what a real caller (`SettingsBridge::enabledStatsLabels()`) would ever pass to
// `setDebugLineFilter()` in production.
const QStringList kAllPinnedLabels = {
    QStringLiteral("Video stream"),
    QStringLiteral("Incoming frame rate from network"),
    QStringLiteral("Decoding frame rate"),
    QStringLiteral("Rendering frame rate"),
    QStringLiteral("Host processing latency min/max/average"),
    QStringLiteral("Frames dropped by your network connection"),
    QStringLiteral("Frames dropped due to network jitter"),
    QStringLiteral("Average network latency"),
    QStringLiteral("Average decoding time"),
    QStringLiteral("Average frame queue delay"),
    QStringLiteral("Average rendering time (including monitor V-sync latency)"),
};

} // namespace

class TestOverlayInjection : public QObject
{
    Q_OBJECT

private slots:
    void publishesTheBitmapAndNotifiesExactlyOnce()
    {
        Overlay::OverlayManager manager;
        MockOverlayRenderer renderer;
        manager.setOverlayRenderer(&renderer);

        // Enabling the overlay notifies the renderer only through the *text* path, and that
        // path needs the bundled TTF this project does not link - hence zero. It is worth
        // pinning: it is exactly why the bitmap path calls the renderer directly instead of
        // funnelling through the TTF rasteriser, so a HUD still composites on a build where
        // the font resource is missing or failed to load.
        manager.setOverlayState(kType, true);
        const int afterEnable = renderer.calls;
        QCOMPARE(afterEnable, 0);

        SDL_Surface* bitmap = makeBitmap(64, 24, 0xFF112233);
        QVERIFY(bitmap != nullptr);

        QVERIFY(manager.updateOverlaySurface(kType, bitmap));

        // Exactly one further notification: the renderer re-uploads on its next frame, so a
        // second notification would be redundant work on a render thread.
        QCOMPARE(renderer.calls, afterEnable + 1);
        QCOMPARE(static_cast<int>(renderer.lastType), static_cast<int>(kType));

        // Delivery is asynchronous and the manager does not copy: the very surface handed in
        // is the one the renderer will take.
        SDL_Surface* taken = manager.getUpdatedOverlaySurface(kType);
        QCOMPARE(taken, bitmap);
        QCOMPARE(taken->w, 64);
        QCOMPARE(taken->h, 24);
        QCOMPARE(taken->format->format, SDL_PIXELFORMAT_ARGB8888);
        QCOMPARE(pixelAt(taken, 5, 5), 0xFF112233u);

        SDL_FreeSurface(taken);
        QCOMPARE(manager.getUpdatedOverlaySurface(kType), nullptr);
    }

    void refusesAForeignPixelFormat()
    {
        Overlay::OverlayManager manager;
        MockOverlayRenderer renderer;
        manager.setOverlayRenderer(&renderer);
        manager.setOverlayState(kType, true);
        const int afterEnable = renderer.calls;

        // RGBA8888 is a different memory order and is not what the renderer's upload path
        // accepts, so it is refused rather than passed to an assert on a render thread.
        SDL_Surface* rgba = SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 32, SDL_PIXELFORMAT_RGBA8888);
        QVERIFY(rgba != nullptr);

        QVERIFY(!manager.updateOverlaySurface(kType, rgba));

        QCOMPARE(renderer.calls, afterEnable);
        QCOMPARE(manager.getUpdatedOverlaySurface(kType), nullptr);
    }

    void refusesWhileTheOverlayIsDisabled()
    {
        Overlay::OverlayManager manager;
        MockOverlayRenderer renderer;
        manager.setOverlayRenderer(&renderer);

        QVERIFY(!manager.isOverlayEnabled(kType));

        SDL_Surface* bitmap = makeBitmap(16, 16, 0xFF000000);
        QVERIFY(bitmap != nullptr);

        QVERIFY(!manager.updateOverlaySurface(kType, bitmap));

        QCOMPARE(renderer.calls, 0);
        QCOMPARE(manager.getUpdatedOverlaySurface(kType), nullptr);
    }

    void replacesAndFreesThePreviousBitmap()
    {
        Overlay::OverlayManager manager;
        MockOverlayRenderer renderer;
        manager.setOverlayRenderer(&renderer);
        manager.setOverlayState(kType, true);
        const int afterEnable = renderer.calls;

        SDL_Surface* first = makeBitmap(32, 16, 0xFF010203);
        QVERIFY(first != nullptr);
        QVERIFY(manager.updateOverlaySurface(kType, first));

        SDL_Surface* second = makeBitmap(48, 16, 0xFF040506);
        QVERIFY(second != nullptr);
        QVERIFY(manager.updateOverlaySurface(kType, second));

        // The earlier bitmap is never handed out again: the manager freed it during the swap.
        // (Asserting the free itself would mean reading freed memory; the observable contract
        // is that the slot holds exactly the newest surface and the old one is gone.)
        SDL_Surface* taken = manager.getUpdatedOverlaySurface(kType);
        QCOMPARE(taken, second);
        QCOMPARE(taken->w, 48);
        QCOMPARE(pixelAt(taken, 1, 1), 0xFF040506u);
        SDL_FreeSurface(taken);

        QCOMPARE(manager.getUpdatedOverlaySurface(kType), nullptr);
        QCOMPARE(renderer.calls, afterEnable + 2);
    }

    void toleratesARendererThatHasNotRegisteredYet()
    {
        // The overlay manager outlives an individual decoder: the engine sets the renderer to
        // nullptr while a decoder is torn down (`ffmpeg.cpp:283`) and the HUD publishes on its
        // own cadence regardless. A publish must not depend on a renderer existing.
        Overlay::OverlayManager manager;
        manager.setOverlayState(kType, true);

        SDL_Surface* bitmap = makeBitmap(16, 16, 0xFF0A0B0C);
        QVERIFY(bitmap != nullptr);

        QVERIFY(manager.updateOverlaySurface(kType, bitmap));

        SDL_Surface* taken = manager.getUpdatedOverlaySurface(kType);
        QVERIFY(taken != nullptr);
        QCOMPARE(pixelAt(taken, 0, 0), 0xFF0A0B0Cu);
        SDL_FreeSurface(taken);
    }

    void refusesANullSurface()
    {
        Overlay::OverlayManager manager;
        MockOverlayRenderer renderer;
        manager.setOverlayRenderer(&renderer);
        manager.setOverlayState(kType, true);
        const int afterEnable = renderer.calls;

        QVERIFY(!manager.updateOverlaySurface(kType, nullptr));
        QCOMPARE(renderer.calls, afterEnable);
    }

    // Phase 5 plan 10 (T-05-41, RESEARCH Pitfall 2): the whole point of the re-assert path. The
    // engine writes its own status text to the same overlay while the SeatHub bitmap is enabled -
    // the exact sequence `session.cpp`'s `clConnectionStatusUpdate(CONN_STATUS_POOR)` makes
    // (`updateOverlayText()` then, already enabled, nothing else needed to trigger the text path).
    void aPublishedSeatHubSurfaceSurvivesAnEngineTextUpdate()
    {
        Overlay::OverlayManager manager;
        MockOverlayRenderer renderer;
        manager.setOverlayRenderer(&renderer);
        manager.setOverlayState(kType, true);

        SDL_Surface* bitmap = makeBitmap(64, 24, 0xFF112233);
        QVERIFY(bitmap != nullptr);
        QVERIFY(manager.updateOverlaySurface(kType, bitmap));
        const int afterBitmap = renderer.calls;

        manager.updateOverlayText(kType, "Poor connection to PC");

        // Re-asserted, not overwritten by the text: the slot still holds the SeatHub bitmap's own
        // pixels, and the renderer is notified again so it actually re-uploads them.
        QCOMPARE(renderer.calls, afterBitmap + 1);
        SDL_Surface* taken = manager.getUpdatedOverlaySurface(kType);
        QVERIFY(taken != nullptr);
        QCOMPARE(taken->w, 64);
        QCOMPARE(taken->h, 24);
        QCOMPARE(pixelAt(taken, 5, 5), 0xFF112233u);
        SDL_FreeSurface(taken);
    }

    // The other half of Pitfall 2: `CONN_STATUS_OKAY` disables the overlay outright
    // (`setOverlayState(type, false)`), which blanks whatever was showing; the HUD is not asked to
    // publish again just because the connection recovered. The bitmap has to come back on its own
    // when the overlay is next enabled - by the engine, not by a fresh `updateOverlaySurface()`.
    void aPublishedSeatHubSurfaceSurvivesTheEngineDisablingAndReEnablingTheOverlay()
    {
        Overlay::OverlayManager manager;
        MockOverlayRenderer renderer;
        manager.setOverlayRenderer(&renderer);
        manager.setOverlayState(kType, true);

        SDL_Surface* bitmap = makeBitmap(48, 16, 0xFF040506);
        QVERIFY(bitmap != nullptr);
        QVERIFY(manager.updateOverlaySurface(kType, bitmap));

        // The engine's own toggle - not the SeatHub hide path (`clearSeatHubSurface()`), which a
        // real caller only reaches by publishing a null surface. Visibility while disabled is the
        // renderer's own `isOverlayEnabled()` check at draw time (untouched by this plan), not a
        // property of this manager's surface slot, so it is not asserted here.
        manager.setOverlayState(kType, false);

        manager.setOverlayState(kType, true);
        SDL_Surface* taken = manager.getUpdatedOverlaySurface(kType);
        QVERIFY2(taken != nullptr,
                 "the SeatHub bitmap should reappear once the overlay is re-enabled");
        QCOMPARE(taken->w, 48);
        QCOMPARE(taken->h, 16);
        QCOMPARE(pixelAt(taken, 1, 1), 0xFF040506u);
        SDL_FreeSurface(taken);
    }

    // The third case the plan names: with no SeatHub bitmap ever published on this slot, the text
    // path is not diverted into the reassert branch at all - the guard is `seatHubSurface !=
    // nullptr`, and this proves the false side of it. (This test project links no TTF font
    // resource - see the file header - so `notifyOverlayUpdated()`'s rasteriser returns before
    // calling the renderer, exactly as `publishesTheBitmapAndNotifiesExactlyOnce` already
    // documents for `setOverlayState(true)` alone; the assertion here is that this text write
    // does not behave any differently from that pre-existing, unaffected path.)
    void theEnginesOwnTextStillWorksWhenSeatHubHasPublishedNothing()
    {
        Overlay::OverlayManager manager;
        MockOverlayRenderer renderer;
        manager.setOverlayRenderer(&renderer);
        manager.setOverlayState(kType, true);
        const int afterEnable = renderer.calls;

        manager.updateOverlayText(kType, "Poor connection to PC");

        QCOMPARE(renderer.calls, afterEnable);
        QVERIFY(manager.getUpdatedOverlaySurface(kType) == nullptr);
    }

    void notifiesTheRequestedOverlayOnly()
    {
        Overlay::OverlayManager manager;
        MockOverlayRenderer renderer;
        manager.setOverlayRenderer(&renderer);
        manager.setOverlayState(Overlay::OverlayDebug, true);
        const int afterEnable = renderer.calls;

        SDL_Surface* bitmap = makeBitmap(8, 8, 0xFF00FF00);
        QVERIFY(bitmap != nullptr);
        QVERIFY(manager.updateOverlaySurface(Overlay::OverlayDebug, bitmap));

        QCOMPARE(renderer.calls, afterEnable + 1);
        QCOMPARE(static_cast<int>(renderer.lastType), static_cast<int>(Overlay::OverlayDebug));

        // The other overlay's slot is untouched.
        QCOMPARE(manager.getUpdatedOverlaySurface(kType), nullptr);

        SDL_Surface* taken = manager.getUpdatedOverlaySurface(Overlay::OverlayDebug);
        QCOMPARE(taken, bitmap);
        SDL_FreeSurface(taken);
    }

    // ------------------------------------------------------------------------------------------
    // Phase 5 plan 12 (CUST-17/D-23/D-26): the per-line OverlayDebug filter. `filteredDebugText()`
    // is exercised directly rather than through the TTF-rasterised surface, because this test
    // project links no font resource (see this file's own header) - `notifyOverlayUpdated()`
    // returns before reaching the filter for exactly the same reason `publishesTheBitmapAnd
    // NotifiesExactlyOnce` already documents for the pre-existing text path. `filteredDebugText()`
    // is the same computation that path uses once a font is linked (production).
    // ------------------------------------------------------------------------------------------

    void withNoFilterEverSetNothingIsDrawn()
    {
        // The safe default before any wiring runs (Plan 12 Task 2's own job): nothing enabled
        // means nothing drawn, the same as an explicit empty list.
        Overlay::OverlayManager manager;
        manager.updateOverlayText(Overlay::OverlayDebug, kAllDebugLinesRaw);

        QVERIFY(manager.filteredDebugText().isEmpty());
    }

    void onlyEnabledLinesRenderInTheEnginesOwnOrder()
    {
        Overlay::OverlayManager manager;
        manager.updateOverlayText(Overlay::OverlayDebug, kAllDebugLinesRaw);

        // The filter list itself is NOT in the engine's order; the output must be.
        manager.setDebugLineFilter({ QStringLiteral("Rendering frame rate"),
                                     QStringLiteral("Video stream"),
                                     QStringLiteral("Average network latency") });

        QCOMPARE(manager.filteredDebugText(),
                 QByteArray("Video stream: 1920x1080 60.00 FPS (Codec: H.264)\n"
                            "Rendering frame rate: 60.00 FPS\n"
                            "Average network latency: 12 ms (variance: 1 ms)\n"));
    }

    void aDisabledLineInTheMiddleLeavesNoGap()
    {
        // "Decoding frame rate" sits between these two in the engine's own text and is disabled.
        Overlay::OverlayManager manager;
        manager.updateOverlayText(Overlay::OverlayDebug, kAllDebugLinesRaw);
        manager.setDebugLineFilter({ QStringLiteral("Incoming frame rate from network"),
                                     QStringLiteral("Rendering frame rate") });

        QCOMPARE(manager.filteredDebugText(),
                 QByteArray("Incoming frame rate from network: 60.00 FPS\n"
                            "Rendering frame rate: 60.00 FPS\n"));
    }

    void twoAdjacentEnabledLinesRenderWithNothingBetweenThem()
    {
        // CUST-17 adjacency edge: two lines the engine already writes next to each other stay
        // next to each other when both are enabled.
        Overlay::OverlayManager manager;
        manager.updateOverlayText(Overlay::OverlayDebug, kAllDebugLinesRaw);
        manager.setDebugLineFilter({ QStringLiteral("Decoding frame rate"),
                                     QStringLiteral("Rendering frame rate") });

        QCOMPARE(manager.filteredDebugText(),
                 QByteArray("Decoding frame rate: 60.00 FPS\n"
                            "Rendering frame rate: 60.00 FPS\n"));
    }

    void nothingRendersWithEveryLineDisabled()
    {
        // CUST-17 empty edge: no empty box, no heading - nothing at all.
        Overlay::OverlayManager manager;
        manager.updateOverlayText(Overlay::OverlayDebug, kAllDebugLinesRaw);
        manager.setDebugLineFilter({});

        QVERIFY(manager.filteredDebugText().isEmpty());
    }

    void anEngineLineWithNoMatchingLabelIsNeverDrawnEvenWhenEverythingElseIsEnabled()
    {
        // T-05-49: simulates an upstream rename/addition. Even with every one of the eleven known
        // labels enabled, a line whose label is not among them is dropped, not defaulted to
        // visible - there is no "draw unknown content" branch anywhere in the filter.
        Overlay::OverlayManager manager;
        manager.updateOverlayText(Overlay::OverlayDebug,
                                  QByteArray(kAllDebugLinesRaw) + "Host public address: 10.0.0.5\n");
        manager.setDebugLineFilter(kAllPinnedLabels);

        const QByteArray filtered = manager.filteredDebugText();
        QVERIFY(!filtered.contains("Host public address"));
        QCOMPARE(filtered, QByteArray(kAllDebugLinesRaw));
    }

    void changingTheFilterAfterTheTextWasSetIsReflectedImmediately()
    {
        // The filter is read fresh on every call, never cached at the point the raw text was
        // written (T-05-52).
        Overlay::OverlayManager manager;
        manager.updateOverlayText(Overlay::OverlayDebug, kAllDebugLinesRaw);

        manager.setDebugLineFilter({ QStringLiteral("Video stream") });
        QCOMPARE(manager.filteredDebugText(),
                 QByteArray("Video stream: 1920x1080 60.00 FPS (Codec: H.264)\n"));

        manager.setDebugLineFilter({ QStringLiteral("Decoding frame rate") });
        QCOMPARE(manager.filteredDebugText(), QByteArray("Decoding frame rate: 60.00 FPS\n"));
    }

    // ------------------------------------------------------------------------------------------
    // Phase 5 plan 12 Task 2 (OD-04): Moonlight's own stats hotkey (Ctrl+Alt+Shift+S, or the
    // gamepad chord) can enable OverlayDebug mid-stream on its own, with no SeatHub code path in
    // between (`session.cpp`'s own keyboard/gamepad handling; not edited by this fork). The owner
    // chose "the filter keeps applying" over "the hotkey shows everything" - there is only ever
    // the one filtered read, so these two are about what happens when something OTHER than a
    // connect-time write toggles the overlay on.
    // ------------------------------------------------------------------------------------------

    void theFilterKeepsApplyingWhicheverThingTurnedTheOverlayOn()
    {
        Overlay::OverlayManager manager;
        MockOverlayRenderer renderer;
        manager.setOverlayRenderer(&renderer);

        // The filter is set once, as if at connect time, before the overlay is ever enabled.
        manager.setDebugLineFilter({ QStringLiteral("Video stream") });
        manager.updateOverlayText(Overlay::OverlayDebug, kAllDebugLinesRaw);
        QVERIFY(!manager.isOverlayEnabled(Overlay::OverlayDebug));

        // The hotkey's own effect, from the filter's point of view, is just this: the overlay
        // becomes enabled with no SeatHub call in between.
        manager.setOverlayState(Overlay::OverlayDebug, true);

        QCOMPARE(manager.filteredDebugText(),
                 QByteArray("Video stream: 1920x1080 60.00 FPS (Codec: H.264)\n"));
    }

    void theHotkeyDrawsNothingWithEveryLineTurnedOff()
    {
        Overlay::OverlayManager manager;
        manager.updateOverlayText(Overlay::OverlayDebug, kAllDebugLinesRaw);
        manager.setDebugLineFilter({});

        manager.setOverlayState(Overlay::OverlayDebug, true);

        QVERIFY(manager.filteredDebugText().isEmpty());
    }

    // ------------------------------------------------------------------------------------------
    // Plan 10 (D-09, D-13, ADR-0045 amended 2026-09-26): the pluggable text rasteriser this plan
    // adds to `overlaymanager.{h,cpp}`, and SeatHub's `OsdCompositor` behind it.
    // ------------------------------------------------------------------------------------------

    void rasterizerReceivesRawTextEnabledAndColour()
    {
        g_recordedRasterizerCalls.clear();

        Overlay::OverlayManager manager;
        MockOverlayRenderer renderer;
        manager.setOverlayRenderer(&renderer);
        manager.setTextRasterizer(&recordingRasterizer, nullptr);

        // A filter that excludes everything: if the hook handed the FILTERED text to the
        // rasteriser (as the SDL_ttf path's `filteredDebugText()` would), the recorded text below
        // would be empty. It must be the raw text instead (D-10's total latency needs lines the
        // customer may not have ticked; see `filteredDebugText()`'s own header comment).
        manager.setDebugLineFilter({});
        manager.setOverlayState(Overlay::OverlayDebug, true);
        manager.updateOverlayText(Overlay::OverlayDebug, kAllDebugLinesRaw);

        QVERIFY(!g_recordedRasterizerCalls.isEmpty());
        const RecordedRasterizerCall debugCall = g_recordedRasterizerCalls.last();
        QCOMPARE(static_cast<int>(debugCall.type), static_cast<int>(Overlay::OverlayDebug));
        QCOMPARE(debugCall.text, QByteArray(kAllDebugLinesRaw));
        QVERIFY(debugCall.enabled);
        QCOMPARE(debugCall.color.r, static_cast<Uint8>(0xD0));
        QCOMPARE(debugCall.color.g, static_cast<Uint8>(0xD0));
        QCOMPARE(debugCall.color.b, static_cast<Uint8>(0x00));
        QCOMPARE(debugCall.color.a, static_cast<Uint8>(0xFF));

        g_recordedRasterizerCalls.clear();
        manager.setOverlayState(kType, true);
        manager.updateOverlayText(kType, "Poor connection to PC");

        QVERIFY(!g_recordedRasterizerCalls.isEmpty());
        const RecordedRasterizerCall statusCall = g_recordedRasterizerCalls.last();
        QCOMPARE(static_cast<int>(statusCall.type), static_cast<int>(kType));
        QCOMPARE(statusCall.text, QByteArray("Poor connection to PC"));
        QVERIFY(statusCall.enabled);
        QCOMPARE(statusCall.color.r, static_cast<Uint8>(0xCC));
        QCOMPARE(statusCall.color.g, static_cast<Uint8>(0x00));
        QCOMPARE(statusCall.color.b, static_cast<Uint8>(0x00));
        QCOMPARE(statusCall.color.a, static_cast<Uint8>(0xFF));
    }

    // RESEARCH-FORK.md §1.3 "Why the hook goes before the re-assert branch": that branch
    // (`:321` before this plan) returns early whenever a SeatHub bitmap is remembered, which
    // would otherwise hide D-13's message for as long as Time left stays published. With the
    // rasteriser called first, the engine's own text always reaches it.
    void engineTextReachesTheHookWhileASeatHubBitmapIsRemembered()
    {
        g_recordedRasterizerCalls.clear();

        Overlay::OverlayManager manager;
        MockOverlayRenderer renderer;
        manager.setOverlayRenderer(&renderer);
        manager.setTextRasterizer(&recordingRasterizer, nullptr);
        manager.setOverlayState(kType, true);

        SDL_Surface* bitmap = makeBitmap(64, 24, 0xFF112233);
        QVERIFY(bitmap != nullptr);
        QVERIFY(manager.updateOverlaySurface(kType, bitmap));

        g_recordedRasterizerCalls.clear();
        manager.updateOverlayText(kType, "Poor connection to PC");

        QVERIFY(!g_recordedRasterizerCalls.isEmpty());
        QCOMPARE(g_recordedRasterizerCalls.last().text, QByteArray("Poor connection to PC"));
    }

    // D-13: Moonlight's own status line, drawn by SeatHub in the slot's own colour, bottom-left.
    void compositorDrawsTheEngineLineInItsSlotColour()
    {
        OsdCompositor compositor;
        const SDL_Color red{ 0xCC, 0x00, 0x00, 0xFF };

        SDL_Surface* surface = OsdCompositor::rasterize(kType, "Poor connection to PC", true, red,
                                                         &compositor);
        QVERIFY(surface != nullptr);
        QCOMPARE(surface->format->format, static_cast<Uint32>(SDL_PIXELFORMAT_ARGB8888));
        QVERIFY(!SDL_MUSTLOCK(surface));

        QVERIFY(surfaceRegionHasPixelNear(surface, 0, 0, surface->w / 2, surface->h,
                                          0xCC, 0x00, 0x00));

        SDL_FreeSurface(surface);
    }

    // With no `setTextRasterizer()` call ever made on this manager, the text path behaves exactly
    // as it did before this plan added the hook - the no-font-linked early return every other test
    // in this file already relies on (see this file's own header).
    void noHookKeepsTodaysPath()
    {
        Overlay::OverlayManager manager;
        MockOverlayRenderer renderer;
        manager.setOverlayRenderer(&renderer);
        manager.setOverlayState(kType, true);
        const int afterEnable = renderer.calls;

        manager.updateOverlayText(kType, "Poor connection to PC");

        QCOMPARE(renderer.calls, afterEnable);
        QVERIFY(manager.getUpdatedOverlaySurface(kType) == nullptr);
    }

    // Plan 10, Task 2: a returned surface in a pixel format the renderer cannot take, or a valid
    // surface for a slot that is disabled, is refused (freed, nothing published) - the same rule
    // `updateOverlaySurface()` already applies, now also enforced on the rasteriser's own path.
    void rasterizerRefusesAForeignSurface()
    {
        {
            Overlay::OverlayManager manager;
            MockOverlayRenderer renderer;
            manager.setOverlayRenderer(&renderer);
            manager.setTextRasterizer(&foreignFormatRasterizer, nullptr);
            manager.setOverlayState(kType, true);

            manager.updateOverlayText(kType, "Poor connection to PC");

            QVERIFY(manager.getUpdatedOverlaySurface(kType) == nullptr);
        }
        {
            Overlay::OverlayManager manager;
            MockOverlayRenderer renderer;
            manager.setOverlayRenderer(&renderer);
            manager.setTextRasterizer(&fixedArgbRasterizer, nullptr);
            manager.setOverlayState(kType, true);

            // Disabling calls notifyOverlayUpdated() (state changed) with `enabled == false`; the
            // rasteriser still returns a valid ARGB8888 surface, and the hook must refuse it.
            manager.setOverlayState(kType, false);

            QVERIFY(manager.getUpdatedOverlaySurface(kType) == nullptr);
        }
    }

    // This test project links no ModeSeven.ttf/SDL_ttf font resource at all (see this file's own
    // header) - the rasteriser path never touches TTF, so publishing succeeds regardless.
    void rasterizerRunsWithoutFontData()
    {
        OsdCompositor compositor;
        Overlay::OverlayManager manager;
        MockOverlayRenderer renderer;
        manager.setOverlayRenderer(&renderer);
        manager.setTextRasterizer(&OsdCompositor::rasterize, &compositor);
        manager.setOverlayState(kType, true);

        manager.updateOverlayText(kType, "Poor connection to PC");

        SDL_Surface* taken = manager.getUpdatedOverlaySurface(kType);
        QVERIFY(taken != nullptr);
        SDL_FreeSurface(taken);
    }

    // The engine clears `text` to empty before calling this class's own text path on disable
    // (`setOverlayState(false)`); the compositor's own recorded engine text must reflect that, so
    // a later composition draws Time left alone rather than the stale line.
    void disablingTheSlotClearsTheRecordedEngineText()
    {
        OsdCompositor compositor;
        Overlay::OverlayManager manager;
        MockOverlayRenderer renderer;
        manager.setOverlayRenderer(&renderer);
        manager.setTextRasterizer(&OsdCompositor::rasterize, &compositor);
        manager.setOverlayState(kType, true);
        manager.updateOverlayText(kType, "Poor connection to PC");

        SDL_Surface* withEngineText = manager.getUpdatedOverlaySurface(kType);
        QVERIFY(withEngineText != nullptr);
        SDL_FreeSurface(withEngineText);

        manager.setOverlayState(kType, false);

        QVERIFY(compositor.composedBottom().isNull());

        const OsdTimeLeft timeLeft{ true, 4, false };
        compositor.setTimeLeft(timeLeft);
        const QImage composed = compositor.composedBottom();
        const QImage expected = renderOsdBottom(1920, 1080, QString(), 0, timeLeft);
        QCOMPARE(composed, expected);
    }

    // Reentrancy (RESEARCH-FORK.md §1.3 "Threads"): the hook runs on the decoder thread, the SDL
    // main thread and moonlight-common-c's connection-status callback thread. `rasterize()` must
    // be safe called concurrently from several threads while `setTimeLeft()`/`setWindowSize()`
    // mutate state on another - every surface returned is well-formed or null, and nothing
    // crashes.
    void compositorIsReentrant()
    {
        OsdCompositor compositor;
        std::atomic<bool> sawBadSurface{ false };

        auto rasterizeWorker = [&]() {
            for (int i = 0; i < 200; ++i) {
                const SDL_Color colours[] = { { 0xCC, 0x00, 0x00, 0xFF }, { 0xD0, 0xD0, 0x00, 0xFF } };
                SDL_Surface* s = OsdCompositor::rasterize(kType, "Poor connection to PC", true,
                                                          colours[i % 2], &compositor);
                if (s != nullptr) {
                    if (s->format->format != SDL_PIXELFORMAT_ARGB8888 || SDL_MUSTLOCK(s)) {
                        sawBadSurface.store(true);
                    }
                    SDL_FreeSurface(s);
                }
            }
        };

        auto stateWorker = [&]() {
            for (int i = 0; i < 200; ++i) {
                compositor.setTimeLeft(OsdTimeLeft{ (i % 2) == 0, i % 20, (i % 3) == 0 });
                compositor.setWindowSize(1280 + (i % 4) * 100, 720 + (i % 4) * 100);
            }
        };

        std::thread t1(rasterizeWorker);
        std::thread t2(rasterizeWorker);
        std::thread t3(rasterizeWorker);
        std::thread t4(stateWorker);

        t1.join();
        t2.join();
        t3.join();
        t4.join();

        QVERIFY(!sawBadSurface.load());
    }
};

QTEST_MAIN(TestOverlayInjection)

#include "tst_overlay_injection.moc"
