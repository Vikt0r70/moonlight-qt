// Task 1 (06.6-06, RESEARCH-FORK.md §2/§3/§4): the in-stream text overlay's pure renderer.
// `docs/spec/ui.md` §7 says the OSD is "SeatHub's own rasteriser" behind a hook into the engine's
// overlay slot - nothing here needs a host, Sunshine, a stream, or even a window. Every function
// under test is a pure `(sizes, colours, strings) -> QImage` transform.
//
// Build (nothing is on PATH machine-wide - Qt and MSVC are both absolute):
//   call "<VS BuildTools>\VC\Auxiliary\Build\vcvars64.bat"
//   set PATH=C:\Qt\6.11.2\msvc2022_64\bin;%PATH%
//   cd tests && qmake tst_osd_render.pro && jom && tst_osd_render.exe -o out.txt,txt
//
// Or, from anywhere: tests\run-all-suites.cmd tst_osd_render

#include <QtTest>

#include <QFile>
#include <QFontDatabase>
#include <QFontInfo>
#include <QImage>
#include <QRect>
#include <QRgb>
#include <QStringList>

#include "seathub/osd_renderer.h"

namespace {

// Every helper below tolerates anti-aliasing: a glyph's edge pixels are a blend, never the exact
// fill colour, so "is this colour present" is a nearness test, not an equality test.

bool regionIsFullyTransparent(const QImage& image, const QRect& region)
{
    for (int y = region.top(); y <= region.bottom(); ++y) {
        for (int x = region.left(); x <= region.right(); ++x) {
            if (qAlpha(image.pixel(x, y)) != 0) {
                return false;
            }
        }
    }
    return true;
}

bool regionHasPixelNear(const QImage& image, const QRect& region, QRgb target, int tolerance = 24)
{
    for (int y = region.top(); y <= region.bottom(); ++y) {
        for (int x = region.left(); x <= region.right(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            if (qAlpha(pixel) == 0) {
                continue;
            }
            if (qAbs(qRed(pixel) - qRed(target)) <= tolerance
                && qAbs(qGreen(pixel) - qGreen(target)) <= tolerance
                && qAbs(qBlue(pixel) - qBlue(target)) <= tolerance) {
                return true;
            }
        }
    }
    return false;
}

// "Near-black": `kOsdOutline` is black at 0.85 alpha, blended over whatever sits behind it, so a
// low-brightness opaque-ish pixel is what the outline actually looks like on screen - not an
// exact colour match.
bool regionHasNearBlackPixel(const QImage& image, const QRect& region, int maxChannel = 60)
{
    for (int y = region.top(); y <= region.bottom(); ++y) {
        for (int x = region.left(); x <= region.right(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            if (qAlpha(pixel) == 0) {
                continue;
            }
            if (qRed(pixel) <= maxChannel && qGreen(pixel) <= maxChannel
                && qBlue(pixel) <= maxChannel) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

class TestOsdRender : public QObject
{
    Q_OBJECT

private slots:
    // Deliberately does not assert `registerOsdFonts()` here: a failed fixture aborts every test
    // function that follows it (QtTest runs only `cleanupTestCase()` after a failed
    // `initTestCase()`), which would hide the individually-named RED failures below behind one
    // generic fixture failure. Each test that needs the font asserts registration for itself.
    void initTestCase()
    {
        QVERIFY(QFile::exists(QStringLiteral(":/seathub/fonts/OpenSans-SemiBold.ttf")));
        QVERIFY(QFile::exists(QStringLiteral(":/seathub/fonts/OFL.txt")));
    }

    // RESEARCH-FORK.md Pitfall 7: a hard-coded "Open Sans" family name silently falls back to
    // Segoe UI on Windows. `osdFont()` must use the family Qt itself reports for the bundled file
    // - checked here by registering the same file a second time (harmless - Qt allows more than
    // one application-font id for the same bytes) purely to read that family name back.
    void openSansResolves()
    {
        QVERIFY(registerOsdFonts());

        const int id = QFontDatabase::addApplicationFont(
            QStringLiteral(":/seathub/fonts/OpenSans-SemiBold.ttf"));
        QVERIFY(id >= 0);
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        QVERIFY(!families.isEmpty());

        const QFontInfo info(osdFont(15));
        QCOMPARE(info.family(), families.first());
    }

    // D-08/D-17, `ui.md` §7: no background box, ever - only the shadow, the outline and the fill.
    void timeLeftIsOutlinedWithNoBox()
    {
        const OsdTimeLeft timeLeft{true, 9, false};
        const QImage image = renderOsdBottom(1920, 1080, QString(), 0, timeLeft);
        QVERIFY(!image.isNull());
        QCOMPARE(image.format(), QImage::Format_ARGB32);

        const QRect bottomRight(image.width() / 2, image.height() / 2,
                                image.width() - (image.width() / 2),
                                image.height() - (image.height() / 2));
        QVERIFY(regionHasPixelNear(image, bottomRight, qRgb(0xFF, 0xFF, 0xFF)));
        QVERIFY(regionHasNearBlackPixel(image, bottomRight));

        const QRect topLeftQuarter(0, 0, image.width() / 4, qMax(1, image.height() / 4));
        QVERIFY(regionIsFullyTransparent(image, topLeftQuarter));

        const QRect leftHalf(0, 0, image.width() / 2, image.height());
        QVERIFY(regionIsFullyTransparent(image, leftHalf));
    }

    // D-20: red from 5 minutes to the end, and never mixed with the white reminder colour.
    void timeLeftIsRedWhenCritical()
    {
        const OsdTimeLeft timeLeft{true, 9, true};
        const QImage image = renderOsdBottom(1920, 1080, QString(), 0, timeLeft);
        QVERIFY(!image.isNull());

        const QRect fullImage(0, 0, image.width(), image.height());
        QVERIFY(regionHasPixelNear(image, fullImage, qRgb(0xEF, 0x44, 0x44)));
        QVERIFY(!regionHasPixelNear(image, fullImage, qRgb(0xFF, 0xFF, 0xFF), /*tolerance=*/10));
    }

    // Nothing to show, nothing drawn (D-11: Time left never shows before a session starts; a
    // hidden Time left with no engine message to compose with is simply nothing at all).
    void nothingIsDrawnWhenTimeLeftIsHiddenAndNoEngineText()
    {
        const OsdTimeLeft timeLeft{false, 9, false};
        const QImage image = renderOsdBottom(1920, 1080, QString(), 0, timeLeft);
        QVERIFY(image.isNull());
    }
};

QTEST_MAIN(TestOsdRender)

#include "tst_osd_render.moc"
