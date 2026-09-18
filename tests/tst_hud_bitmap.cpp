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
// Build (nothing is on PATH machine-wide - Qt and MSVC are both absolute):
//   call "<VS BuildTools>\VC\Auxiliary\Build\vcvars64.bat"
//   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
//   cd tests && qmake tst_hud_bitmap.pro && jom && tst_hud_bitmap.exe -o out.txt,txt

#include <QtTest>

#include <QImage>

// Don't let SDL hook the main function Qt Test provides (see `app/main.cpp`), or `QTEST_MAIN`
// expands to `SDL_main` and the link fails with "unresolved external symbol main".
#define SDL_MAIN_HANDLED
#include <SDL.h>

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
};

QTEST_MAIN(TestHudBitmap)

#include "tst_hud_bitmap.moc"
