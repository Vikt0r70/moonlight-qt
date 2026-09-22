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

// Don't let SDL hook our main function: Qt Test already provides one (the same reason and the
// same fix as `app/main.cpp`). Without this, `QTEST_MAIN` expands to `SDL_main` and the link
// fails with "unresolved external symbol main".
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "streaming/video/overlaymanager.h"

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
};

QTEST_MAIN(TestOverlayInjection)

#include "tst_overlay_injection.moc"
