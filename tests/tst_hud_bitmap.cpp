// Proof (a) of Plan 03-05's overlay spike (ADR-0045): the pixel-format contract between a
// Qt-rendered ARGB bitmap and the D3D11 overlay texture the engine builds from it.
//
// The chain under test is the whole one the HUD depends on:
//
//   QImage::Format_ARGB32            (what QPainter produces)
//     -> SDL_Surface, SDL_PIXELFORMAT_ARGB8888   (what the OverlayManager publishes)
//     -> ID3D11Texture2D, DXGI_FORMAT_B8G8R8A8_UNORM, IMMUTABLE,
//        pSysMem = surface->pixels, SysMemPitch = surface->pitch  (what the renderer uploads)
//     -> STAGING texture + CopyResource + Map    (how this test reads the GPU's own bytes back)
//
// The upload half is transcribed from `D3D11VARenderer::notifyOverlayUpdated`
// (app/streaming/video/ffmpeg-renderers/d3d11va.cpp:894-1014) field for field. It cannot be
// linked here: the method is private to a renderer class the fork's D-28 boundary forbids
// modifying. Reproducing it exactly is the point - if the engine's upload contract ever
// changes shape, this transcription is what has to change with it, and the test fails until
// it does.
//
// No window, no swapchain, no GPU and no video stream are involved: `D3D_DRIVER_TYPE_WARP`
// is the software rasteriser, so this runs on any Windows machine, including one with no
// Sunshine host anywhere near it.
//
// Proof (c) of Plan 03-05's overlay spike (ADR-0045): the SeatHub HUD producer itself - the
// bitmap, its layout, its alpha and its lifecycle. It shares this project with proof (a) because
// both answer the same question from opposite ends: (a) says the engine's upload path consumes
// exactly what a `QImage::Format_ARGB32` produces, (c) says the bytes put on that path are the
// HUD the design system asks for.
//
// The producer is deliberately linkable with no engine in it (`SeatHubClient` injects the
// publisher that reaches `OverlayManager`), so nothing here needs an SDL window, a D3D11 device
// or a streaming session.
//
// Build (nothing is on PATH machine-wide - Qt and MSVC are both absolute):
//   call "<VS BuildTools>\VC\Auxiliary\Build\vcvars64.bat"
//   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
//   cd tests && qmake tst_hud_bitmap.pro && jom && tst_hud_bitmap.exe -o out.txt,txt

#include <QtTest>

#include <QFile>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QMutexLocker>

#include <atomic>

// Don't let SDL hook the main function Qt Test provides (see `app/main.cpp`), or `QTEST_MAIN`
// expands to `SDL_main` and the link fails with "unresolved external symbol main".
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "seathub/hud_overlay.h"

#ifdef Q_OS_WIN32
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
#endif

namespace {

// A small image whose channels and alpha are all distinguishable, so a swapped channel, a
// premultiplied alpha or a stride error cannot pass by coincidence.
QImage makeTestImage()
{
    QImage image(64, 32, QImage::Format_ARGB32);
    image.fill(QColor(0, 0, 0, 0).rgba());

    for (int y = 0; y < image.height(); y++) {
        for (int x = 0; x < image.width(); x++) {
            if (x < 32) {
                // Straight 25% white: if anything premultiplies, this becomes 0x40404040 and
                // the byte comparison below sees it.
                image.setPixel(x, y, qRgba(0xFF, 0xFF, 0xFF, 0x40));
            }
            else {
                // Fully transparent, but with colour information left in the low bytes -
                // alpha must be the only thing that hides it.
                image.setPixel(x, y, qRgba(0x00, 0xFF, 0x00, 0x00));
            }
        }
    }

    // An opaque marker with all three colour channels different from each other.
    for (int y = 4; y < 12; y++) {
        for (int x = 4; x < 12; x++) {
            image.setPixel(x, y, qRgba(0xFF, 0x00, 0x00, 0xFF));
        }
    }

    return image;
}

// The idiom `session.cpp` already uses for the window icon (session.cpp:1892-1897), and the
// one ADR-0045 records as the way a QImage becomes an overlay surface.
SDL_Surface* wrapQImage(const QImage& image)
{
    return SDL_CreateRGBSurfaceWithFormatFrom(const_cast<uchar*>(image.constBits()),
                                              image.width(),
                                              image.height(),
                                              32,
                                              image.bytesPerLine(),
                                              SDL_PIXELFORMAT_ARGB8888);
}

#ifdef Q_OS_WIN32

struct D3D11Fixture {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

bool createWarpDevice(D3D11Fixture& fixture)
{
    D3D_FEATURE_LEVEL level;
    const HRESULT hr = D3D11CreateDevice(nullptr,
                                         D3D_DRIVER_TYPE_WARP,
                                         nullptr,
                                         0,
                                         nullptr,
                                         0,
                                         D3D11_SDK_VERSION,
                                         &fixture.device,
                                         &level,
                                         &fixture.context);
    return SUCCEEDED(hr) && fixture.device && fixture.context;
}

// Transcribed from D3D11VARenderer::notifyOverlayUpdated (d3d11va.cpp:917-956).
HRESULT uploadOverlaySurface(ID3D11Device* device, SDL_Surface* surface,
                             ComPtr<ID3D11Texture2D>& texture,
                             ComPtr<ID3D11ShaderResourceView>& srv)
{
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = surface->w;
    texDesc.Height = surface->h;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_IMMUTABLE;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = 0;

    D3D11_SUBRESOURCE_DATA texData = {};
    texData.pSysMem = surface->pixels;
    texData.SysMemPitch = surface->pitch;

    HRESULT hr = device->CreateTexture2D(&texDesc, &texData, &texture);
    if (FAILED(hr)) {
        return hr;
    }

    return device->CreateShaderResourceView(reinterpret_cast<ID3D11Resource*>(texture.Get()),
                                            nullptr, &srv);
}

// Reads the texture back through a STAGING copy, which is the only way to see what the GPU
// actually stored for a given input - the intermediate IMMUTABLE texture is not mappable.
bool readBackTexture(ID3D11Device* device, ID3D11DeviceContext* context,
                     ID3D11Texture2D* texture, QByteArray& bytes, UINT& rowPitch)
{
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))) {
        return false;
    }

    context->CopyResource(reinterpret_cast<ID3D11Resource*>(staging.Get()),
                          reinterpret_cast<ID3D11Resource*>(texture));

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(reinterpret_cast<ID3D11Resource*>(staging.Get()), 0,
                            D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }

    rowPitch = mapped.RowPitch;
    bytes.resize(static_cast<int>(rowPitch) * desc.Height);
    for (UINT y = 0; y < desc.Height; y++) {
        memcpy(bytes.data() + y * rowPitch,
               static_cast<const Uint8*>(mapped.pData) + y * rowPitch,
               rowPitch);
    }

    context->Unmap(reinterpret_cast<ID3D11Resource*>(staging.Get()), 0);
    return true;
}

#endif // Q_OS_WIN32

// Drives a real `HudOverlay` with a clock the test owns and a publisher that captures what the
// production publisher would hand to `OverlayManager`.
//
// It copies the pixels out immediately rather than keeping the surface: that is what a renderer
// which consumes the bitmap right away does (`D3D11VARenderer::notifyOverlayUpdated()` uploads it
// inside the call), and the copy also lets assertions run after the publisher has returned. The
// surface it copies from owns its own pixels since CR-03 - `HudOverlay::publishStrip()` allocates
// and fills a real `SDL_Surface` rather than wrapping the `QImage` it just drew into - so the
// transfer below (free after copying) is the ownership hand-over `updateOverlaySurface()`
// documents. `hudSurfaceOutlivesThePublisherCall` covers the other consumer, `SdlRenderer`, which
// takes the surface at its next frame instead.
class HudHarness
{
public:
    HudHarness()
    {
        m_hud.setClock([this]() { return m_nowMs.load(); });
        m_hud.setPublisher([this](SDL_Surface* surface) {
            if (surface == nullptr) {
                QMutexLocker locker(&m_mutex);
                ++m_hides;
                return true;
            }

            QImage copy(surface->w, surface->h, QImage::Format_ARGB32);
            for (int y = 0; y < surface->h; y++) {
                memcpy(copy.scanLine(y),
                       static_cast<const uchar*>(surface->pixels) + y * surface->pitch,
                       size_t(surface->w) * 4);
            }

            // Locked, and the clock above is atomic, because `beginSession()` starts a real
            // SDL_AddTimer heartbeat as soon as SDL's event subsystem is up: on the timer
            // thread the publisher and the clock are reachable concurrently with the test
            // thread. The product does not care - it only ever reads the surface inside the
            // publisher call - but an unlocked harness would be a data race in the test.
            QMutexLocker locker(&m_mutex);
            m_formats.append(int(surface->format->format));
            m_mustLock.append(SDL_MUSTLOCK(surface) != 0);
            m_frames.append(copy);

            SDL_FreeSurface(surface);
            return true;
        });
    }

    ~HudHarness()
    {
        // Stop the heartbeat before the capturing lambdas above go away.
        m_hud.endSession();
    }

    HudOverlay& hud() { return m_hud; }

    void advance(qint64 ms) { m_nowMs.store(m_nowMs.load() + ms); }

    int hides() const { QMutexLocker locker(&m_mutex); return m_hides; }
    const QList<QImage> frames() const { QMutexLocker locker(&m_mutex); return m_frames; }
    const QList<int> formats() const { QMutexLocker locker(&m_mutex); return m_formats; }
    const QList<bool> mustLock() const { QMutexLocker locker(&m_mutex); return m_mustLock; }

private:
    HudOverlay m_hud;
    std::atomic<qint64> m_nowMs{1000000};
    mutable QMutex m_mutex;
    int m_hides = 0;
    QList<QImage> m_frames;
    QList<int> m_formats;
    QList<bool> m_mustLock;
};

// Reads a file from the fork root (the path comes from the `FORK_ROOT` define in the .pro).
QString readForkFile(const QString& relativePath)
{
    QFile file(QStringLiteral(FORK_ROOT) + QLatin1Char('/') + relativePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

// Pixels of `image` that are `rgb` to within `tolerance` per channel and fully opaque. Fully
// opaque, so a partly covered edge (which is straight alpha and keeps its colour, but is not the
// point of these checks) cannot be counted twice or miscounted.
int countColour(const QImage& image, QRgb rgb, int tolerance = 6)
{
    int count = 0;
    for (int y = 0; y < image.height(); y++) {
        for (int x = 0; x < image.width(); x++) {
            const QRgb pixel = image.pixel(x, y);
            if (qAlpha(pixel) == 255 && qAbs(qRed(pixel) - qRed(rgb)) <= tolerance
                && qAbs(qGreen(pixel) - qGreen(rgb)) <= tolerance
                && qAbs(qBlue(pixel) - qBlue(rgb)) <= tolerance) {
                count++;
            }
        }
    }
    return count;
}

constexpr QRgb kWarnRgb = 0xFFF59E0B;        // Tokens.warnDefault
constexpr QRgb kDestructiveRgb = 0xFFEF4444; // Tokens.destructiveDefault

// The runs of non-transparent pixels along one row, as [first, last] column pairs. The strip and
// the card are each an opaque filled panel, so the row through their middle is two runs when they
// are apart and one when they touch.
QList<QPair<int, int>> opaqueRuns(const QImage& image, int y)
{
    QList<QPair<int, int>> runs;
    int start = -1;
    for (int x = 0; x < image.width(); x++) {
        const bool opaque = qAlpha(image.pixel(x, y)) > 0;
        if (opaque && start < 0) {
            start = x;
        }
        else if (!opaque && start >= 0) {
            runs.append(qMakePair(start, x - 1));
            start = -1;
        }
    }
    if (start >= 0) {
        runs.append(qMakePair(start, image.width() - 1));
    }
    return runs;
}

} // namespace

class TestHudBitmap : public QObject
{
    Q_OBJECT

private slots:
    // The format assumption the whole path rests on, asserted without any GPU involvement:
    // SDL_PIXELFORMAT_ARGB8888 is BGRA in memory on a little-endian machine, which is exactly
    // what QImage::Format_ARGB32 is.
    void sdlArgb8888MatchesQImageArgb32InMemory()
    {
        QImage image(2, 1, QImage::Format_ARGB32);
        image.setPixel(0, 0, qRgba(0xFF, 0x00, 0x00, 0x80));
        image.setPixel(1, 0, qRgba(0x12, 0x34, 0x56, 0x78));

        SDL_Surface* surface = wrapQImage(image);
        QVERIFY(surface != nullptr);
        QCOMPARE(surface->format->format, SDL_PIXELFORMAT_ARGB8888);

        // SDL's own decoder agrees with the alpha/colour split we wrote.
        Uint8 r = 0, g = 0, b = 0, a = 0;
        SDL_GetRGBA(*static_cast<const Uint32*>(surface->pixels), surface->format, &r, &g, &b, &a);
        QCOMPARE(r, Uint8(0xFF));
        QCOMPARE(g, Uint8(0x00));
        QCOMPARE(b, Uint8(0x00));
        QCOMPARE(a, Uint8(0x80));

        SDL_GetRGBA(*(reinterpret_cast<const Uint32*>(surface->pixels) + 1),
                    surface->format, &r, &g, &b, &a);
        QCOMPARE(r, Uint8(0x12));
        QCOMPARE(g, Uint8(0x34));
        QCOMPARE(b, Uint8(0x56));
        QCOMPARE(a, Uint8(0x78));

        // And the raw byte order is the one the upload path assumes: B, G, R, A.
        const Uint8* bytes = static_cast<const Uint8*>(surface->pixels);
        QCOMPARE(bytes[0], Uint8(0x00));   // blue
        QCOMPARE(bytes[1], Uint8(0x00));   // green
        QCOMPARE(bytes[2], Uint8(0xFF));   // red
        QCOMPARE(bytes[3], Uint8(0x80));   // alpha

        SDL_FreeSurface(surface);
    }

    // The guards `D3D11VARenderer::notifyOverlayUpdated` asserts before it touches a surface.
    void wrappedImageSatisfiesTheRenderersSurfaceGuards()
    {
        QImage image = makeTestImage();
        SDL_Surface* surface = wrapQImage(image);
        QVERIFY(surface != nullptr);

        QCOMPARE(surface->format->format, SDL_PIXELFORMAT_ARGB8888);
        QVERIFY(!SDL_MUSTLOCK(surface));
        QCOMPARE(surface->w, 64);
        QCOMPARE(surface->h, 32);
        QCOMPARE(surface->pitch, image.bytesPerLine());

        SDL_FreeSurface(surface);
    }

#ifdef Q_OS_WIN32
    void exactUploadPathRoundTripsChannelsAndStraightAlpha()
    {
        D3D11Fixture fixture;
        if (!createWarpDevice(fixture)) {
            QSKIP("No D3D11 device is available in this environment, not even WARP");
        }

        const QImage image = makeTestImage();
        SDL_Surface* surface = wrapQImage(image);
        QVERIFY(surface != nullptr);

        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> srv;
        const HRESULT hr = uploadOverlaySurface(fixture.device.Get(), surface, texture, srv);
        QVERIFY2(SUCCEEDED(hr), "the engine's exact CreateTexture2D/CreateShaderResourceView path failed");
        QVERIFY(texture && srv);

        D3D11_TEXTURE2D_DESC desc = {};
        texture->GetDesc(&desc);
        QCOMPARE(desc.Width, UINT(64));
        QCOMPARE(desc.Height, UINT(32));
        QCOMPARE(int(desc.Format), int(DXGI_FORMAT_B8G8R8A8_UNORM));
        QCOMPARE(int(desc.Usage), int(D3D11_USAGE_IMMUTABLE));
        QCOMPARE(int(desc.BindFlags), int(D3D11_BIND_SHADER_RESOURCE));

        QByteArray gpuBytes;
        UINT rowPitch = 0;
        QVERIFY(readBackTexture(fixture.device.Get(), fixture.context.Get(), texture.Get(),
                                gpuBytes, rowPitch));
        QVERIFY(rowPitch >= UINT(image.width() * 4));

        // Byte-for-byte: every channel, every row, and the row stride the engine passes.
        const int rowBytes = image.width() * 4;
        for (int y = 0; y < image.height(); y++) {
            QCOMPARE(memcmp(gpuBytes.constData() + y * rowPitch,
                            image.constScanLine(y),
                            rowBytes),
                     0);
        }

        SDL_FreeSurface(surface);
    }

    void premultipliedInputWouldBeDetectable()
    {
        // Guards the previous test's value: the 25%-white half is stored straight, so a
        // premultiplied input really would differ. This asserts the *expectation* the byte
        // comparison above relies on, so a future change that made both sides premultiplied
        // could not make the round trip pass vacuously.
        const QImage image = makeTestImage();
        SDL_Surface* surface = wrapQImage(image);
        QVERIFY(surface != nullptr);

        Uint8 r = 0, g = 0, b = 0, a = 0;
        SDL_GetRGBA(static_cast<const Uint32*>(surface->pixels)[0], surface->format, &r, &g, &b, &a);
        QCOMPARE(a, Uint8(0x40));
        QCOMPARE(r, Uint8(0xFF));
        QCOMPARE(g, Uint8(0xFF));
        QCOMPARE(b, Uint8(0xFF));
        QVERIFY2(!(r == 0x40 && g == 0x40 && b == 0x40),
                 "the fixture must not already be premultiplied");

        SDL_FreeSurface(surface);
    }
#endif // Q_OS_WIN32

    // ---------------------------------------------------------------------------------------
    // Proof (c): the HUD producer.
    // ---------------------------------------------------------------------------------------

    // The strip is a full-height HUD band, opaque only where the card is. Everything the renderer
    // blends with `SRC_ALPHA`/`INV_SRC_ALPHA` depends on that: transparent padding must really be
    // transparent, or the HUD would paint a bar across the video.
    void hudStripCoversTheCardAndNothingElse()
    {
        HudOverlay hud;
        const QImage strip = hud.renderStripAt(0, 0);

        QCOMPARE(strip.height(), HudOverlay::stripHeight());
        QCOMPARE(int(strip.format()), int(QImage::Format_ARGB32));
        QVERIFY2(strip.width() > 200 && strip.width() < 900,
                 "the card should be content-sized and fit a 720p stream window");

        // Corners are outside the card: fully transparent, no colour left behind.
        const QList<QPoint> corners = {QPoint(0, 0), QPoint(strip.width() - 1, 0),
                                       QPoint(0, strip.height() - 1),
                                       QPoint(strip.width() - 1, strip.height() - 1)};
        for (const QPoint& point : corners) {
            QCOMPARE(qAlpha(strip.pixel(point)), 0);
        }

        // The middle is the card: opaque.
        QCOMPARE(qAlpha(strip.pixel(strip.width() / 2, strip.height() / 2)), 255);

        int opaque = 0;
        for (int y = 0; y < strip.height(); y++) {
            for (int x = 0; x < strip.width(); x++) {
                if (qAlpha(strip.pixel(x, y)) == 255) {
                    opaque++;
                }
            }
        }
        QVERIFY2(opaque > 500, "the card should be a real surface, not a sliver");
    }

    // D-56's timer, at the boundaries where a duration display usually breaks.
    void hudTimerShowsHoursMinutesSeconds()
    {
        const QList<QPair<qint64, QString>> cases = {
            {0, QStringLiteral("00:00:00")},
            {59, QStringLiteral("00:00:59")},
            {60, QStringLiteral("00:01:00")},
            {3600, QStringLiteral("01:00:00")},
            {3661, QStringLiteral("01:01:01")},
            {86399, QStringLiteral("23:59:59")},
        };

        for (const auto& testCase : cases) {
            HudHarness harness;
            harness.hud().beginSession();
            harness.advance(testCase.first * 1000);
            harness.hud().tick();

            QCOMPARE(harness.hud().elapsedSeconds(), testCase.first);
            QCOMPARE(harness.hud().timerText(), testCase.second);
            harness.hud().endSession();
        }

        // Fixed width, so the card does not reflow when the first hour lands.
        QCOMPARE(HudOverlay::stripHeight(), 56);
        QCOMPARE(HudOverlay::autoHideMs(), 4000);
    }

    // The timer is actually drawn, not just tracked: an unrendered value would pass the test
    // above while showing a customer a frozen clock.
    void hudTimerTextChangesTheStripPixels()
    {
        HudOverlay hud;
        const QImage atZero = hud.renderStripAt(0, 0);
        const QImage atNinetyNine = hud.renderStripAt(99, 0);

        QCOMPARE(atZero.size(), atNinetyNine.size());

        int differing = 0;
        for (int y = 0; y < atZero.height(); y++) {
            for (int x = 0; x < atZero.width(); x++) {
                if (atZero.pixel(x, y) != atNinetyNine.pixel(x, y)) {
                    differing++;
                }
            }
        }
        QVERIFY2(differing > 20, "the timer did not reach the bitmap");
    }

    // ADR-0045's positioning technique: the overlay is drawn 1:1 in swapchain pixels from the
    // bottom-left anchor, so the card's position comes from the bytes, not from the surface size.
    // Padding the strip to a display width must therefore leave the card byte-identical - which
    // is what makes it safe to composite without knowing the stream window's size.
    void hudStripWidthFollowsTheRequestedDisplayWidth()
    {
        HudOverlay hud;
        const QImage content = hud.renderStripAt(0, 0);
        const int cardWidth = content.width();

        for (int displayWidth : {1280, 1920, 2560}) {
            const QImage padded = hud.renderStripAt(0, displayWidth);
            QCOMPARE(padded.width(), displayWidth);
            QCOMPARE(padded.height(), content.height());

            for (int y = 0; y < padded.height(); y++) {
                QCOMPARE(memcmp(padded.constScanLine(y), content.constScanLine(y),
                                size_t(cardWidth) * 4),
                         0);
            }

            // The padding is invisible, not black: it must not darken the video behind it.
            for (int x = cardWidth; x < padded.width(); x += 37) {
                QCOMPARE(qAlpha(padded.pixel(x, padded.height() / 2)), 0);
            }
        }
    }

    // The renderers blend with SRC_ALPHA/INV_SRC_ALPHA and a bare texture sample, so the bitmap
    // has to carry *straight* alpha. A premultiplied edge would double-darken against the video.
    void hudEdgePixelsCarryStraightAlpha()
    {
        HudOverlay hud;
        const QImage strip = hud.renderStripAt(0, 0);

        // Every colour the producer draws is a solid one, so with straight alpha a partially
        // covered pixel keeps its colour and only its alpha falls. Two legitimate sets exist and
        // nothing else may appear:
        //
        //  1. a light token - the six-colour palette's lighter entries - which an alpha-scaled
        //     (premultiplied) buffer would have darkened in proportion to its alpha;
        //  2. the card's own greys, anywhere between the fill #141414 and the border #404040,
        //     because the border is stroked over the fill and its antialiased edge is a mix of
        //     the two.
        //
        // The second set exists precisely because it is ambiguous at high alpha: a premultiplied
        // border pixel at alpha 222 reads #383838, which is inside the fill-to-border range. That
        // case is not what this test can catch - `hudStripCoversTheCardAndNothingElse` catches the
        // convention structurally instead, by requiring the image to be `QImage::Format_ARGB32`,
        // the straight format. What the colour walk below adds is the cases where premultiplication
        // is unmistakable: a light token at partial alpha. A #fafafa glyph edge at alpha 128 would
        // arrive as #7d7d7d, and the muted #a3a3a3 the same - neither is in either set.
        const QList<QColor> lightTokens = {QColor(0xfa, 0xfa, 0xfa),
                                           QColor(0xa3, 0xa3, 0xa3),
                                           QColor(0x10, 0xb9, 0x81)};
        const auto isLightToken = [&lightTokens](const QRgb pixel) {
            for (const QColor& colour : lightTokens) {
                if (qAbs(qRed(pixel) - colour.red()) <= 4
                    && qAbs(qGreen(pixel) - colour.green()) <= 4
                    && qAbs(qBlue(pixel) - colour.blue()) <= 4) {
                    return true;
                }
            }
            return false;
        };
        const auto isCardGrey = [](const QRgb pixel) {
            return qRed(pixel) >= 0x14 - 4 && qRed(pixel) <= 0x40 + 4
                   && qGreen(pixel) >= 0x14 - 4 && qGreen(pixel) <= 0x40 + 4
                   && qBlue(pixel) >= 0x14 - 4 && qBlue(pixel) <= 0x40 + 4;
        };

        int edgePixels = 0;
        int translucentEdges = 0;
        for (int y = 0; y < strip.height(); y++) {
            for (int x = 0; x < strip.width(); x++) {
                const QRgb pixel = strip.pixel(x, y);
                const int alpha = qAlpha(pixel);
                if (alpha == 0 || alpha == 255) {
                    continue;
                }

                edgePixels++;

                QVERIFY2(isLightToken(pixel) || isCardGrey(pixel),
                         qPrintable(QStringLiteral("edge pixel (%1,%2) alpha %3 has colour %4 -"
                                                   " premultiplied, not straight")
                                        .arg(x).arg(y).arg(alpha)
                                        .arg(QColor(pixel).name())));

                // The positive evidence, and the reason this walk is not vacuous: below alpha 64
                // an alpha-scaled card grey falls out of the range above entirely - a border
                // pixel premultiplied at alpha 27 reads #070707, and the fill at alpha 23 reads
                // #020202. Both are far below the #10 floor. A pixel this translucent that is
                // still the card's own colour proves the buffer is straight, because there is no
                // other way to produce it.
                if (alpha < 64) {
                    translucentEdges++;
                }
            }
        }

        QVERIFY2(edgePixels > 0, "the rounded card should be antialiased");
        QVERIFY2(translucentEdges > 0,
                 "no pixel was translucent enough for premultiplication to be distinguishable -"
                 " this walk proved nothing about the alpha convention");
    }

    // The HUD's colours, metrics and faces are not invented: every one of them is named in a
    // comment in `hud_overlay.cpp` as the token it mirrors, and this test reads the generated
    // token files to hold that claim. If a token file changes, this fails and the HUD has to
    // change with it.
    void hudUsesTheGeneratedDesignTokens()
    {
        const QString tokens = readForkFile(QStringLiteral("app/gui/Tokens.qml"));
        const QString metrics = readForkFile(QStringLiteral("app/gui/Metrics.qml"));
        QVERIFY2(!tokens.isEmpty(), "app/gui/Tokens.qml could not be read - is FORK_ROOT right?");
        QVERIFY2(!metrics.isEmpty(), "app/gui/Metrics.qml could not be read - is FORK_ROOT right?");

        const QList<QPair<QString, QString>> tokenValues = {
            {QStringLiteral("surface1Default"), QStringLiteral("#141414")},         // card fill
            {QStringLiteral("borderStrongDefault"), QStringLiteral("#404040")},     // card border
            {QStringLiteral("foregroundDefault"), QStringLiteral("#fafafa")},       // timer, labels
            {QStringLiteral("foregroundMutedDefault"), QStringLiteral("#a3a3a3")},  // eyebrow
            {QStringLiteral("successDefault"), QStringLiteral("#10b981")},          // live dot
            {QStringLiteral("borderDefault"), QStringLiteral("#262626")},           // hairline
            {QStringLiteral("warnDefault"), QStringLiteral("#f59e0b")},             // credit at 10, its card
            {QStringLiteral("destructiveDefault"), QStringLiteral("#ef4444")},      // credit at 2, its card
            {QStringLiteral("fontSansDefault"), QStringLiteral("Inter")},
            {QStringLiteral("fontMonoDefault"), QStringLiteral("Geist Mono")},
        };

        for (const auto& token : tokenValues) {
            const QString expected = token.first + QStringLiteral(": \"") + token.second
                                     + QLatin1Char('"');
            QVERIFY2(tokens.contains(expected),
                     qPrintable(QStringLiteral("Tokens.qml no longer declares %1").arg(expected)));
        }

        // The numeric companion: Metrics.qml derives its numbers from the generated tokens
        // rather than keeping a second copy of them (audit F15). This replaces the older
        // "the literals are still declared" check with the stronger one - a value that stops
        // being derived is exactly the drift this test exists to catch. `tst_ui_screens`
        // additionally loads the singleton and asserts the parsed numbers.
        for (const QString& derived : {QStringLiteral("s2: tokenPx(Tokens.step2)"),
                                       QStringLiteral("s4: tokenPx(Tokens.step4)"),
                                       QStringLiteral("s5: tokenPx(Tokens.step5)"),
                                       QStringLiteral("radiusLg: tokenPx(Tokens.lgDefault)"),
                                       QStringLiteral("fontLabel: tokenRem(Tokens.scaleLabelSize)"),
                                       QStringLiteral("fontSm: tokenRem(Tokens.scaleSmSize)")}) {
            QVERIFY2(metrics.contains(derived),
                     qPrintable(QStringLiteral("Metrics.qml no longer derives %1").arg(derived)));
        }
    }

    // The published surface is what `OverlayManager::updateOverlaySurface()` accepts and what
    // `notifyOverlayUpdated()` uploads without asserting - the same three guards proof (a)
    // asserts on a synthetic image, now asserted on the HUD's real output.
    void hudPublishesASurfaceTheRendererAccepts()
    {
        HudHarness harness;
        harness.hud().beginSession();

        QVERIFY(harness.frames().size() >= 1);
        QCOMPARE(harness.formats().last(), int(SDL_PIXELFORMAT_ARGB8888));
        QVERIFY2(!harness.mustLock().last(), "the renderer asserts a surface that needs no locking");

        const QImage expected = harness.hud().renderStripAt(0, 0);
        const QImage published = harness.frames().last();
        QCOMPARE(published.size(), expected.size());
        for (int y = 0; y < expected.height(); y++) {
            QCOMPARE(memcmp(published.constScanLine(y), expected.constScanLine(y),
                            size_t(expected.width()) * 4),
                     0);
        }

        harness.hud().endSession();
    }

    // CR-03: the surface has to stay valid until whatever consumes it has consumed it.
    //
    // `D3D11VARenderer` uploads inside `notifyOverlayUpdated()`, but `SdlRenderer` declares no
    // `notifyOverlayUpdated()` at all - it inherits the base class's no-op - and takes the bitmap
    // in `renderOverlay()` on its next frame. The old `publishStrip()` handed over a view onto the
    // `QImage` it had just drawn into and let that image die on return, so a software-decoding
    // session (`SdlRenderer`) read freed memory. This test is that renderer: it keeps the surface
    // the publisher returns, reads it afterwards, and only then frees it.
    void hudSurfaceOutlivesThePublisherCall()
    {
        SDL_Surface* handedOver = nullptr;
        int publishes = 0;

        HudOverlay hud;
        hud.setClock([]() { return qint64(1000000); });
        hud.setPublisher([&handedOver, &publishes](SDL_Surface* surface) {
            if (surface == nullptr) {
                return true;   // "hide": nothing to read later
            }
            if (handedOver != nullptr) {
                SDL_FreeSurface(handedOver);   // a republish replaces what this fake renderer holds
            }
            handedOver = surface;
            ++publishes;
            return true;
        });

        hud.beginSession();
        QVERIFY(publishes >= 1);
        QVERIFY(handedOver != nullptr);

        // The structural half of the guarantee: a real allocation, not a view onto a buffer the
        // HUD owns (SDL_PREALLOC is exactly "surface->pixels is not owned by this surface").
        QVERIFY2((handedOver->flags & SDL_PREALLOC) == 0,
                 "the HUD must hand over a surface that owns its pixels");

        // And the observable half: the pixels are still the strip, read after `publishStrip()`
        // returned and with the `QImage` it drew into long gone.
        const QImage expected = hud.renderStripAt(0, 0);
        QCOMPARE(handedOver->w, expected.width());
        QCOMPARE(handedOver->h, expected.height());
        QCOMPARE(handedOver->format->format, SDL_PIXELFORMAT_ARGB8888);
        QVERIFY2(!SDL_MUSTLOCK(handedOver), "the renderer asserts a surface that needs no locking");
        for (int y = 0; y < expected.height(); y++) {
            QCOMPARE(memcmp(static_cast<const uchar*>(handedOver->pixels) + y * handedOver->pitch,
                            expected.constScanLine(y), size_t(expected.width()) * 4),
                     0);
        }

        SDL_FreeSurface(handedOver);   // the late consumer's job, once it has consumed it
        handedOver = nullptr;
        hud.endSession();
    }

    // D-04/screens.md §25: auto-hide after 4 s, reappear on input. The heartbeat is what makes the
    // timer advance at all, and hiding must not disable the overlay in a way that blocks the
    // engine's own use of the same slot (the poor-connection warning).
    void hudAutoHidesAfterFourSecondsAndReturnsOnInput()
    {
        if (SDL_InitSubSystem(SDL_INIT_EVENTS) != 0) {
            QSKIP("SDL's events subsystem is unavailable, so the input watch cannot be installed");
        }
        QVERIFY(SDL_WasInit(SDL_INIT_EVENTS) != 0);

        HudHarness harness;
        harness.hud().beginSession();
        QVERIFY2(harness.hud().isVisible(), "the HUD must be up as soon as the session connects");

        // A tick inside the window keeps it up.
        harness.advance(1000);
        harness.hud().tick();
        QVERIFY(harness.hud().isVisible());
        QCOMPARE(harness.hides(), 0);

        // Past the window with no input, it hides.
        harness.advance(HudOverlay::autoHideMs());
        harness.hud().tick();
        QVERIFY2(!harness.hud().isVisible(), "the HUD should have auto-hidden");
        QCOMPARE(harness.hides(), 1);

        // Input brings it back.
        harness.hud().noteActivity();
        harness.hud().tick();
        QVERIFY2(harness.hud().isVisible(), "input should have brought the HUD back");
        QCOMPARE(harness.hides(), 1);

        // Ending the session hides it for good, and does not count as an auto-hide.
        harness.hud().endSession();
        QVERIFY(!harness.hud().isVisible());
        QVERIFY(harness.hides() >= 2);

        SDL_QuitSubSystem(SDL_INIT_EVENTS);
    }

    // ---------------------------------------------------------------------------------------
    // Phase 5 plan 10 (CUST-15): the remaining credit and the two low-balance warning cards.
    // ---------------------------------------------------------------------------------------

    // The numbers and the sentences are the spec's, and the two colours the HUD adds are tokens.
    // `timing.md` "Low-balance warnings: 10 min, then 2 min" and its two card-lifetime rows;
    // `copy.md` "In session" for the sentences, dash included (U+2014).
    void thresholdsLifetimesAndSentencesAreTheSpecs()
    {
        QCOMPARE(HudOverlay::kWarnMinutes, qint64(10));
        QCOMPARE(HudOverlay::kCriticalMinutes, qint64(2));
        QCOMPARE(HudOverlay::kTenMinuteCardMs, qint64(15000));

        QCOMPARE(HudOverlay::warningText(HudOverlay::Card::TenMinutes),
                 QString(QStringLiteral("10 minutes left.")));
        QCOMPARE(HudOverlay::warningText(HudOverlay::Card::TwoMinutes),
                 QString(QStringLiteral("2 minutes left ") + QChar(0x2014)
                         + QStringLiteral(" find a save point.")));
        QVERIFY(HudOverlay::warningText(HudOverlay::Card::None).isEmpty());

        // Nothing about topping up, and nothing that pressures: how much time is left, and no more.
        for (const HudOverlay::Card card : {HudOverlay::Card::TenMinutes,
                                            HudOverlay::Card::TwoMinutes}) {
            const QString text = HudOverlay::warningText(card).toLower();
            QVERIFY(!text.contains(QStringLiteral("top up")));
            QVERIFY(!text.contains(QStringLiteral("buy")));
            QVERIFY(!text.contains(QStringLiteral("hurry")));
        }
    }

    // The credit readout, at each side of each threshold. The value is the one duration format and
    // the colour and glyph come from the balance: nothing at 11, warn at 10 and 3, destructive at 2
    // and 0. Counting opaque pixels of a token colour proves the value was drawn in it, not that a
    // property was set.
    void creditReadoutChangesColourExactlyAtTheThresholds()
    {
        struct Case { qint64 minutes; bool warn; bool destructive; };
        const Case cases[] = { { 135, false, false }, { 61, false, false }, { 11, false, false },
                               { 10, true, false },   { 3, true, false },   { 2, false, true },
                               { 0, false, true } };

        for (const Case& c : cases) {
            HudOverlay hud;
            hud.seedCreditMinutes(c.minutes);
            const QImage strip = hud.renderStripAt(0, 0);

            const int warnPixels = countColour(strip, kWarnRgb);
            const int destructivePixels = countColour(strip, kDestructiveRgb);
            QVERIFY2((warnPixels > 20) == c.warn,
                     qPrintable(QStringLiteral("%1 min: warn pixels %2").arg(c.minutes).arg(warnPixels)));
            QVERIFY2((destructivePixels > 20) == c.destructive,
                     qPrintable(QStringLiteral("%1 min: destructive pixels %2")
                                    .arg(c.minutes).arg(destructivePixels)));
            // And exclusively: the strip never shows both treatments at once.
            QVERIFY(!(warnPixels > 20 && destructivePixels > 20));
        }
    }

    // A balance that is not known draws nothing for it - no label, no zero - and the strip is then
    // exactly what it was before Phase 5; one that is known makes the strip wider, and a different
    // value is a different picture.
    void creditReadoutIsDrawnOnlyOnceOneIsKnownAndFollowsItsValue()
    {
        HudOverlay unknown;
        HudOverlay known;
        known.seedCreditMinutes(135);
        HudOverlay other;
        other.seedCreditMinutes(136);

        const QImage without = unknown.renderStripAt(5, 0);
        const QImage with = known.renderStripAt(5, 0);
        QVERIFY2(with.width() > without.width(), "the credit group must add to the strip");
        QCOMPARE(with.height(), without.height());

        // Ignored, not zero: a negative is not a balance.
        HudOverlay negative;
        negative.seedCreditMinutes(-1);
        QCOMPARE(negative.creditMinutes(), qint64(-1));
        QCOMPARE(negative.renderStripAt(5, 0).width(), without.width());

        QCOMPARE(known.creditMinutes(), qint64(135));
        QCOMPARE(other.renderStripAt(5, 0).size(), with.size());
        int differing = 0;
        const QImage otherStrip = other.renderStripAt(5, 0);
        for (int y = 0; y < with.height(); y++) {
            for (int x = 0; x < with.width(); x++) {
                if (with.pixel(x, y) != otherStrip.pixel(x, y)) {
                    differing++;
                }
            }
        }
        QVERIFY2(differing > 10, "the credit value did not reach the bitmap");
    }

    // The ten-minute card: it appears when a read first reaches ten minutes, sits at the bottom
    // right of a frame as wide as the stream window, and goes after its 15 seconds - not 14.
    void theTenMinuteCardShowsForItsLifetimeAndThenGoes()
    {
        HudHarness harness;
        harness.hud().setDisplayWidth(1280);
        harness.hud().beginSession();
        harness.hud().noteCreditMinutes(11);
        QCOMPARE(int(harness.hud().activeCard()), int(HudOverlay::Card::None));

        harness.hud().noteCreditMinutes(10);
        QCOMPARE(int(harness.hud().activeCard()), int(HudOverlay::Card::TenMinutes));
        QCOMPARE(harness.hud().warningsFired(HudOverlay::Card::TenMinutes), 1);
        QVERIFY(harness.hud().isCardShown());

        // [Superseded by Plan 14 (D-11/D-12/D-20), decided by executor, owner to review]: the
        // legacy card's own state machine above is unchanged and still fires exactly as before -
        // Plan 22 is what removes it - but a ten-minute read is now ALSO a fresh Time-left read
        // (the same numeric threshold, D-11), so `publishNow()` routes every publish here to the
        // compositor instead of the legacy card's own frame (`hud_overlay.h`'s one-publish-per-
        // slot rule: no card sentence beside Time left, D-12). The card's own pixels are proven by
        // `renderFrame()` directly elsewhere (`theTwoCardsDifferInColourAndInGlyphAndNotInPlacement`
        // etc.); this test's own frame checks below assert the compositor owns the slot instead.
        QImage frame = harness.frames().last();
        QCOMPARE(frame.width(), 1920);
        QVERIFY(!harness.hud().compositor().composedBottom().isNull());

        // Still there just inside its lifetime (legacy state), and Time left stays visible with
        // it - the compositor has no lifetime of its own yet (Plan 22 applies the real
        // appear/disappear rule).
        harness.advance(HudOverlay::kTenMinuteCardMs - 1);
        harness.hud().tick();
        QCOMPARE(int(harness.hud().activeCard()), int(HudOverlay::Card::TenMinutes));
        frame = harness.frames().last();
        QCOMPARE(frame.width(), 1920);

        harness.advance(1);
        harness.hud().tick();
        QCOMPARE(int(harness.hud().activeCard()), int(HudOverlay::Card::None));
        QVERIFY(!harness.hud().isCardShown());
        // The legacy card's own lifetime ended, but the balance is still ten minutes: Time left
        // (a Plan 14/D-11 rule with no lifetime of its own yet) keeps the compositor on the slot,
        // not the bare strip the pre-Plan-14 assertion here expected.
        frame = harness.frames().last();
        QCOMPARE(frame.width(), 1920);
        QVERIFY(!harness.hud().compositor().composedBottom().isNull());

        // Once per session: reading ten again, or nine, does not bring the legacy card back.
        harness.hud().noteCreditMinutes(10);
        harness.hud().noteCreditMinutes(9);
        QCOMPARE(int(harness.hud().activeCard()), int(HudOverlay::Card::None));
        QCOMPARE(harness.hud().warningsFired(HudOverlay::Card::TenMinutes), 1);

        harness.hud().endSession();
    }

    // The two-minute card: it replaces the ten-minute one, stays however long the session runs,
    // and goes only when the session ends.
    void theTwoMinuteCardReplacesTheTenAndStaysUntilTheSessionEnds()
    {
        HudHarness harness;
        harness.hud().setDisplayWidth(1280);
        harness.hud().beginSession();

        harness.hud().noteCreditMinutes(10);
        QCOMPARE(int(harness.hud().activeCard()), int(HudOverlay::Card::TenMinutes));

        // Replaced, not stacked: one card at a time, and the ten-minute colours are gone from it.
        // [Superseded by Plan 14 (D-11/D-12/D-20), decided by executor, owner to review]: the
        // legacy card state above is unchanged, but a read at or below ten minutes is also a
        // fresh Time-left read, so `publishNow()` routes the publish to the compositor instead of
        // the legacy card's own frame - see `theTenMinuteCardShowsForItsLifetimeAndThenGoes`'s own
        // comment for the full rule.
        harness.hud().noteCreditMinutes(2);
        QCOMPARE(int(harness.hud().activeCard()), int(HudOverlay::Card::TwoMinutes));
        QCOMPARE(harness.hud().warningsFired(HudOverlay::Card::TwoMinutes), 1);
        QImage frame = harness.frames().last();
        QCOMPARE(frame.width(), 1920);
        QVERIFY(!harness.hud().compositor().composedBottom().isNull());
        // Two minutes is also at or below Time left's own critical threshold (five, D-20): red.
        QVERIFY(countColour(frame, kOsdDestructive, 0) > 0);

        // An hour on, a wallet read a minute later: still there. Not on a clock of its own.
        for (int i = 0; i < 6; ++i) {
            harness.advance(10 * 60 * 1000);
            harness.hud().tick();
            harness.hud().noteCreditMinutes(1);
            QCOMPARE(int(harness.hud().activeCard()), int(HudOverlay::Card::TwoMinutes));
        }
        QCOMPARE(harness.frames().last().width(), 1920);
        QCOMPARE(harness.hud().warningsFired(HudOverlay::Card::TwoMinutes), 1);
        QCOMPARE(harness.hud().warningsFired(HudOverlay::Card::TenMinutes), 1);

        // The session ends: the card goes with it.
        harness.hud().endSession();
        QCOMPARE(int(harness.hud().activeCard()), int(HudOverlay::Card::None));
        QVERIFY(harness.hides() >= 1);
    }

    // A session that begins with less than a threshold's worth of credit fires only the lower
    // card - and the higher one cannot fire later on the way down from a top-up.
    void aSessionStartingUnderTwoMinutesFiresOnlyTheLowerCard()
    {
        HudHarness harness;
        harness.hud().setDisplayWidth(1280);
        harness.hud().beginSession();

        harness.hud().noteCreditMinutes(1);
        QCOMPARE(int(harness.hud().activeCard()), int(HudOverlay::Card::TwoMinutes));
        QCOMPARE(harness.hud().warningsFired(HudOverlay::Card::TwoMinutes), 1);
        QCOMPARE(harness.hud().warningsFired(HudOverlay::Card::TenMinutes), 0);

        harness.hud().noteCreditMinutes(30);   // topped up mid-stream
        harness.hud().noteCreditMinutes(9);    // and down again
        QCOMPARE(harness.hud().warningsFired(HudOverlay::Card::TenMinutes), 0);
        QCOMPARE(harness.hud().warningsFired(HudOverlay::Card::TwoMinutes), 1);
        QCOMPARE(int(harness.hud().activeCard()), int(HudOverlay::Card::TwoMinutes));

        harness.hud().endSession();
    }

    // The same, starting between the two: the ten-minute card, and then the two-minute one later.
    void aSessionStartingBetweenTheThresholdsFiresBothInOrder()
    {
        HudHarness harness;
        harness.hud().setDisplayWidth(1280);
        harness.hud().beginSession();

        harness.hud().noteCreditMinutes(7);
        QCOMPARE(int(harness.hud().activeCard()), int(HudOverlay::Card::TenMinutes));
        harness.advance(20000);
        harness.hud().tick();
        QCOMPARE(int(harness.hud().activeCard()), int(HudOverlay::Card::None));
        harness.hud().noteCreditMinutes(2);
        QCOMPARE(int(harness.hud().activeCard()), int(HudOverlay::Card::TwoMinutes));
        QCOMPARE(harness.hud().warningsFired(HudOverlay::Card::TenMinutes), 1);
        QCOMPARE(harness.hud().warningsFired(HudOverlay::Card::TwoMinutes), 1);

        // A new session is a new set of thresholds.
        harness.hud().beginSession();
        QCOMPARE(int(harness.hud().activeCard()), int(HudOverlay::Card::None));
        QCOMPARE(harness.hud().warningsFired(HudOverlay::Card::TenMinutes), 0);
        QCOMPARE(harness.hud().warningsFired(HudOverlay::Card::TwoMinutes), 0);
        QCOMPARE(harness.hud().creditMinutes(), qint64(-1));
        harness.hud().noteCreditMinutes(7);
        QCOMPARE(int(harness.hud().activeCard()), int(HudOverlay::Card::TenMinutes));

        harness.hud().endSession();
    }

    // Missing data fires nothing (CUST-15 empty edge). The only thing a failed read does is not
    // arrive; what does arrive that is not a balance, or that is only the pre-stream seed, is
    // shown or ignored but never turned into a warning.
    void noWarningFiresOnMissingDataOrOnASeed()
    {
        HudHarness harness;
        harness.hud().setDisplayWidth(1280);
        harness.hud().beginSession();

        harness.hud().noteCreditMinutes(-1);
        QCOMPARE(harness.hud().creditMinutes(), qint64(-1));

        // The last balance read before the stream: on the strip, and no card, however low it is.
        harness.hud().seedCreditMinutes(1);
        QCOMPARE(harness.hud().creditMinutes(), qint64(1));
        QCOMPARE(int(harness.hud().activeCard()), int(HudOverlay::Card::None));
        QCOMPARE(harness.hud().warningsFired(HudOverlay::Card::TenMinutes), 0);
        QCOMPARE(harness.hud().warningsFired(HudOverlay::Card::TwoMinutes), 0);
        QVERIFY(harness.frames().last().width() < 1280);

        // A later tick with nothing new changes nothing either.
        harness.advance(10000);
        harness.hud().tick();
        QCOMPARE(int(harness.hud().activeCard()), int(HudOverlay::Card::None));

        // And the seed did not use up the threshold: the first real read of it still fires.
        harness.hud().noteCreditMinutes(1);
        QCOMPARE(int(harness.hud().activeCard()), int(HudOverlay::Card::TwoMinutes));

        harness.hud().endSession();
    }

    // A read that arrives outside a session arms nothing for the next one.
    void aReadOutsideASessionFiresNothing()
    {
        HudHarness harness;
        harness.hud().setDisplayWidth(1280);

        harness.hud().noteCreditMinutes(1);
        QCOMPARE(int(harness.hud().activeCard()), int(HudOverlay::Card::None));
        QCOMPARE(harness.hud().warningsFired(HudOverlay::Card::TwoMinutes), 0);
        QCOMPARE(harness.frames().size(), 0);

        harness.hud().beginSession();
        QCOMPARE(int(harness.hud().activeCard()), int(HudOverlay::Card::None));
        harness.hud().endSession();
    }

    // The cards do not auto-hide with the strip. With the strip gone after four seconds of no
    // input, a warning card is still published - alone, at the right, with nothing where the strip
    // was - and it is not hidden until the session ends.
    void aWarningCardOutlivesTheStripsAutoHide()
    {
        if (SDL_InitSubSystem(SDL_INIT_EVENTS) != 0) {
            QSKIP("SDL's events subsystem is unavailable, so the input watch cannot be installed");
        }

        HudHarness harness;
        harness.hud().setDisplayWidth(1280);
        harness.hud().beginSession();
        harness.hud().noteCreditMinutes(2);
        QCOMPARE(int(harness.hud().activeCard()), int(HudOverlay::Card::TwoMinutes));

        harness.advance(HudOverlay::autoHideMs());
        harness.hud().tick();
        QVERIFY2(!harness.hud().isVisible(), "the strip should have auto-hidden");
        QVERIFY(harness.hud().isCardShown());
        QCOMPARE(harness.hides(), 0);

        // [Superseded by Plan 14 (D-11/D-12/D-20), decided by executor, owner to review]: two
        // minutes is also a fresh Time-left read at or below the critical threshold (five, D-20),
        // so `publishNow()` routes the publish to the compositor instead of the legacy card's own
        // frame - see `theTenMinuteCardShowsForItsLifetimeAndThenGoes`'s own comment for the full
        // rule. The compositor has no auto-hide of its own yet (Plan 22), so it stays on screen
        // through the strip's own auto-hide exactly as the legacy card did.
        const QImage frame = harness.frames().last();
        QCOMPARE(frame.width(), 1920);
        QVERIFY(!harness.hud().compositor().composedBottom().isNull());

        // Input brings the strip back; the compositor is unaffected either way (D-11 has no input
        // dependency).
        harness.hud().noteActivity();
        harness.hud().tick();
        QVERIFY(harness.hud().isVisible());
        QCOMPARE(harness.frames().last().width(), 1920);

        harness.hud().endSession();
        QVERIFY(harness.hides() >= 1);
        QVERIFY(!harness.hud().isCardShown());
        SDL_QuitSubSystem(SDL_INIT_EVENTS);
    }

    // UI Considerations, "HUD warning card" overflow row: the two-minute card renders fully inside
    // the frame, bottom right, at a 1280x720 stream and at a 3840x2160 one, and does not run into
    // the strip. The frame is the window's width and one strip high: the card's bottom edge is the
    // frame's bottom edge and its right edge is the frame's right edge.
    void theTwoMinuteCardIsFullyInsideTheFrameAtBothStreamSizes()
    {
        for (const int width : {1280, 3840}) {
            HudOverlay hud;
            hud.seedCreditMinutes(2);
            const QImage frame = hud.renderFrame(37, width, true, HudOverlay::Card::TwoMinutes);

            QCOMPARE(frame.width(), width);
            QCOMPARE(frame.height(), HudOverlay::stripHeight());

            const int middle = frame.height() / 2;
            const auto runs = opaqueRuns(frame, middle);
            QCOMPARE(runs.size(), 2);                 // the strip, a gap, the card
            QCOMPARE(runs.first().first, 0);          // the strip at the left
            QCOMPARE(runs.last().second, width - 1);  // the card flush right, inside the frame
            QVERIFY(runs.last().first > runs.first().second + 8);

            // Fully inside vertically: the card's border reaches the last row and stays opaque
            // there, and the frame is no taller than one row of cards.
            const int cardMiddle = (runs.last().first + runs.last().second) / 2;
            QCOMPARE(qAlpha(frame.pixel(cardMiddle, 0)), 255);
            QCOMPARE(qAlpha(frame.pixel(cardMiddle, frame.height() - 1)), 255);

            // The card is the two-minute one: destructive border and glyph, and it fits its text
            // (about a third of the smaller window, not a slab).
            const QImage cardImage = frame.copy(runs.last().first, 0,
                                                runs.last().second - runs.last().first + 1,
                                                frame.height());
            QVERIFY(countColour(cardImage, kDestructiveRgb) > 100);
            QVERIFY(cardImage.width() > 200 && cardImage.width() < 500);
        }
    }

    // With no stream width known (nothing has told the HUD, no window is focused) a warning is
    // still on the screen: beside the strip, or alone at the left when the strip is hidden. A card
    // in the wrong place beats none.
    void withNoWidthKnownACardIsDrawnBesideTheStripNotDropped()
    {
        HudOverlay hud;
        hud.seedCreditMinutes(10);
        const QImage beside = hud.renderFrame(3, 0, true, HudOverlay::Card::TenMinutes);
        const QImage strip = hud.renderStripAt(3, 0);
        QVERIFY(beside.width() > strip.width());
        QCOMPARE(opaqueRuns(beside, beside.height() / 2).size(), 2);
        QCOMPARE(opaqueRuns(beside, beside.height() / 2).last().second, beside.width() - 1);

        const QImage alone = hud.renderFrame(3, 0, false, HudOverlay::Card::TenMinutes);
        QCOMPARE(opaqueRuns(alone, alone.height() / 2).size(), 1);
        QCOMPARE(opaqueRuns(alone, alone.height() / 2).first().first, 0);
        QVERIFY(countColour(alone, kWarnRgb) > 100);
    }

    // A frame that carries no card is the strip and nothing but: the two card levels differ from
    // each other only by their colour and glyph, and both are drawn from the copy deck's sentence.
    void theTwoCardsDifferInColourAndInGlyphAndNotInPlacement()
    {
        HudOverlay hud;
        const QImage ten = hud.renderFrame(0, 1280, false, HudOverlay::Card::TenMinutes);
        const QImage two = hud.renderFrame(0, 1280, false, HudOverlay::Card::TwoMinutes);

        QVERIFY(countColour(ten, kWarnRgb) > 100);
        QCOMPARE(countColour(ten, kDestructiveRgb), 0);
        QVERIFY(countColour(two, kDestructiveRgb) > 100);
        QCOMPARE(countColour(two, kWarnRgb), 0);

        // Both flush to the right edge and one row high.
        for (const QImage* image : {&ten, &two}) {
            QCOMPARE(image->width(), 1280);
            QCOMPARE(opaqueRuns(*image, 28).last().second, 1279);
        }

        // Different sentences, so different widths - the longer one is the two-minute line.
        QVERIFY(opaqueRuns(two, 28).last().first < opaqueRuns(ten, 28).last().first);
    }

    // ---------------------------------------------------------------------------------------
    // Plan 14 (D-09/D-11/D-13): the compositor Time left path. [Decided by executor, owner to
    // review]: `OverlayManager` holds one surface per slot, so the compositor owns it outright
    // once it has anything to draw - see `hud_overlay.h`'s own class comment for the full rule
    // these two tests are the RED/GREEN proof of.
    // ---------------------------------------------------------------------------------------

    // A fresh wallet read at or below ten minutes reaches the compositor and shows up through the
    // same publisher the legacy strip uses - on the next tick, not immediately
    // (`noteCreditMinutes()` only records the state; `tick()` decides the slot's frame each
    // cycle).
    void nineMinutesReadShowsTimeLeft()
    {
        HudHarness harness;
        harness.hud().beginSession();
        harness.hud().noteCreditMinutes(9);
        harness.hud().tick();

        QVERIFY2(!harness.hud().compositor().composedBottom().isNull(),
                 "a nine-minute fresh read must make the compositor visible");

        const QImage frame = harness.frames().last();
        // The compositor's own default window size (1920x1080, D-14) - not the legacy strip's
        // content-sized width - is what proves this frame came from `composedBottom()`, not the
        // superseded `renderFrame()`.
        QCOMPARE(frame.width(), 1920);

        // Tolerance 0, not `countColour()`'s default of 6: the legacy card's own sentence colour
        // (Tokens.foregroundDefault, 0xFAFAFA) is within a tolerance of 6 of pure white and would
        // pass this check vacuously if it accidentally matched the superseded card instead of the
        // compositor's own `kOsdValue` (0xFFFFFFFF exactly).
        const QImage rightHalf = frame.copy(frame.width() / 2, 0, frame.width() / 2, frame.height());
        QVERIFY2(countColour(rightHalf, qRgb(0xFF, 0xFF, 0xFF), 0) > 0,
                 "Time left's white value pixels must be on the right, drawn through the compositor");

        harness.hud().endSession();
    }

    // The pre-stream seed never shows Time left (D-16): `seedCreditMinutes()` never touches the
    // compositor - only a fresh read (`noteCreditMinutes()`) does.
    void seedNeverShows()
    {
        HudHarness harness;
        harness.hud().beginSession();
        harness.hud().seedCreditMinutes(9);
        harness.hud().tick();

        QVERIFY2(harness.hud().compositor().composedBottom().isNull(),
                 "the seed alone must not make the compositor visible");

        harness.hud().endSession();
    }
};

QTEST_MAIN(TestHudBitmap)

#include "tst_hud_bitmap.moc"
