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

#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QImage>
#include <QList>
#include <QPainter>
#include <QPoint>
#include <QRandomGenerator>
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

// D-17's picture sheet: a plain white frame, a plain black frame, and a "busy" mid-grey noise
// frame from a FIXED seed (so a re-run overwrites the same pixels, not a different-looking one).
QImage makeBackground(int width, int height, const QString& kind)
{
    QImage image(width, height, QImage::Format_ARGB32);
    if (kind == QLatin1String("white")) {
        image.fill(Qt::white);
    }
    else if (kind == QLatin1String("black")) {
        image.fill(Qt::black);
    }
    else {
        QRandomGenerator rng(0xD17); // fixed seed - reproducible, not "true" randomness
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const int grey = 96 + rng.bounded(64); // mid-grey band, busy enough to be a real test
                image.setPixel(x, y, qRgb(grey, grey, grey));
            }
        }
    }
    return image;
}

// Composites `overlay` onto `background` at `origin`, then saves a 1:1 (never scaled) crop of the
// overlay's own bounding box plus 64px of background on every side that has room - the picture
// sheet's whole point is a close-up glance at the overlay, not a full frame nobody can read at
// thumbnail size.
void writeCrop(const QString& dir, int height, const QString& background, const QString& kind,
              const QImage& backgroundImage, const QImage& overlay, const QPoint& origin)
{
    QImage composed = backgroundImage.copy();
    QPainter painter(&composed);
    painter.drawImage(origin, overlay);
    painter.end();

    const QRect overlayRect(origin, overlay.size());
    const QRect cropRect = overlayRect.adjusted(-64, -64, 64, 64)
        .intersected(QRect(0, 0, composed.width(), composed.height()));

    const QString path = QStringLiteral("%1/%2p-%3-%4.png")
        .arg(dir, QString::number(height), background, kind);
    QVERIFY(composed.copy(cropRect).save(path, "PNG"));
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

        // The image itself IS the bottom strip (research §6: `height = block height + inset`,
        // no extra headroom) - Time left's own bottom-right placement within it is `inset` from
        // the image's own right and bottom edges, so "bottom-right" is checked as the right half
        // of this already-short crop, not a further vertical quadrant of it.
        const QRect rightHalf(image.width() / 2, 0, image.width() - (image.width() / 2),
                              image.height());
        QVERIFY(regionHasPixelNear(image, rightHalf, qRgb(0xFF, 0xFF, 0xFF)));
        QVERIFY(regionHasNearBlackPixel(image, rightHalf));

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

    // D-14: `13, 15, 20 and 30 px at 720, 1080, 1440 and 2160`; the outline and status-line floors
    // (`06.6-RESEARCH-FORK.md` §2).
    void sizesScaleWithWindowHeight()
    {
        QCOMPARE(osdSizesFor(720).valuePx, 13);
        QCOMPARE(osdSizesFor(1080).valuePx, 15);
        QCOMPARE(osdSizesFor(1440).valuePx, 20);
        QCOMPARE(osdSizesFor(2160).valuePx, 30);

        QCOMPARE(osdSizesFor(1080).outlinePx, 1.5);
        QVERIFY(osdSizesFor(1).outlinePx >= 1.0);
        QVERIFY(osdSizesFor(2160).outlinePx > 1.5);

        QCOMPARE(osdSizesFor(1080).statusPx, 36);
        QVERIFY(osdSizesFor(1).statusPx >= 13);
    }

    void unitIsSmallerThanValue()
    {
        for (const int height : {720, 1080, 1440, 2160}) {
            const OsdSizes sizes = osdSizesFor(height);
            QVERIFY(sizes.unitPx < sizes.valuePx);
        }
    }

    // `ui.md` §7: stats labels in `--osd-label`, the resolution row's label in
    // `--osd-label-media`, values and units in `--osd-value`.
    void statsLabelsAreOrangeValuesWhite()
    {
        const QList<OsdStatsRow> rows{
            OsdStatsRow{QStringLiteral("FPS"), false,
                       {OsdValuePart{QString(), QStringLiteral("59.9"), QStringLiteral("fps")}}},
            OsdStatsRow{QStringLiteral("RES"), true,
                       {OsdValuePart{QString(), QStringLiteral("1920x1080"), QString()}}},
        };
        const QImage image = renderOsdStats(rows, 1080);
        QVERIFY(!image.isNull());

        const OsdSizes sizes = osdSizesFor(1080);
        const QRect row1(0, 0, sizes.statsMargin + sizes.labelColumn, sizes.statsMargin + sizes.rowHeight);
        const QRect row1Value(sizes.statsMargin + sizes.labelColumn, 0,
                              image.width() - (sizes.statsMargin + sizes.labelColumn),
                              sizes.statsMargin + sizes.rowHeight);
        const QRect row2(0, sizes.statsMargin + sizes.rowHeight, sizes.statsMargin + sizes.labelColumn,
                         sizes.rowHeight + sizes.statsMargin);

        QVERIFY(regionHasPixelNear(image, row1, qRgb(0xFF, 0x9A, 0x2E)));
        QVERIFY(regionHasPixelNear(image, row1Value, qRgb(0xFF, 0xFF, 0xFF)));
        QVERIFY(regionHasPixelNear(image, row2, qRgb(0x2D, 0xD4, 0xBF)));
    }

    // A grouped row (LATENCY's `NET`/`TOTAL`) draws every part, so it is wider than a row with
    // just one.
    void latencyRowCarriesTwoParts()
    {
        const OsdStatsRow singlePart{
            QStringLiteral("LATENCY"), false,
            {OsdValuePart{QString(), QStringLiteral("31"), QStringLiteral("ms")}}};
        const OsdStatsRow twoParts{
            QStringLiteral("LATENCY"), false,
            {OsdValuePart{QStringLiteral("NET"), QStringLiteral("12"), QStringLiteral("ms")},
             OsdValuePart{QStringLiteral("TOTAL"), QStringLiteral("31"), QStringLiteral("ms")}}};

        const QImage singleImage = renderOsdStats({singlePart}, 1080);
        const QImage twoImage = renderOsdStats({twoParts}, 1080);
        QVERIFY(!singleImage.isNull());
        QVERIFY(!twoImage.isNull());
        QVERIFY(twoImage.width() > singleImage.width());
    }

    // OD-04: with every toggle off, the caller passes an empty row list and nothing is drawn, even
    // if the stats hotkey shows the slot.
    void nothingDrawnWithEveryRowOff()
    {
        const QImage image = renderOsdStats({}, 1080);
        QVERIFY(image.isNull());
    }

    // D-13: Moonlight's own status lines keep the engine's wording and colour, with no outline.
    void engineLineInSlotColour()
    {
        const OsdTimeLeft none{false, 0, false};
        const QImage image = renderOsdBottom(1920, 1080, QStringLiteral("Poor connection to PC"),
                                             0xFFCC0000, none);
        QVERIFY(!image.isNull());

        const QRect leftHalf(0, 0, image.width() / 2, image.height());
        QVERIFY(regionHasPixelNear(image, leftHalf, qRgb(0xCC, 0x00, 0x00)));
        QVERIFY(!regionHasNearBlackPixel(image, leftHalf));
    }

    // D-13/§6: when both are present they compose into one bitmap - the engine's line stays
    // bottom-left, Time left stays bottom-right.
    void engineLineAndTimeLeftCompose()
    {
        const OsdTimeLeft timeLeft{true, 4, true};
        const QImage image = renderOsdBottom(1920, 1080, QStringLiteral("Poor connection to PC"),
                                             0xFFCC0000, timeLeft);
        QVERIFY(!image.isNull());

        const QRect leftHalf(0, 0, image.width() / 2, image.height());
        const QRect rightHalf(image.width() / 2, 0, image.width() - (image.width() / 2), image.height());
        QVERIFY(regionHasPixelNear(image, leftHalf, qRgb(0xCC, 0x00, 0x00)));
        QVERIFY(regionHasPixelNear(image, rightHalf, qRgb(0xEF, 0x44, 0x44)));
    }

    // `ui.md` §4: digits never jitter - every one of them has the same horizontal advance.
    void digitsAreTabular()
    {
        const QFontMetricsF metrics(osdFont(15));
        const qreal zeroAdvance = metrics.horizontalAdvance(QStringLiteral("0"));
        const QString digits = QStringLiteral("123456789");
        for (const QChar digit : digits) {
            QCOMPARE(metrics.horizontalAdvance(QString(digit)), zeroAdvance);
        }
    }

    // D-17: a picture sheet of both overlays at four resolutions on three backgrounds, for the
    // owner to glance at - not a blocking question, so it is produced on request, never on every
    // run. `SEATHUB_PICTURE_SHEET_DIR` is set by `06.6-06-PLAN.md`'s Task 3 verify command to the
    // phase's own `06.6-picture-sheet` folder.
    void pictureSheet()
    {
        const QString dir = qEnvironmentVariable("SEATHUB_PICTURE_SHEET_DIR");
        if (dir.isEmpty()) {
            QSKIP("SEATHUB_PICTURE_SHEET_DIR not set - the picture sheet is produced on request");
        }

        QVERIFY(QDir().mkpath(dir));
        QVERIFY(registerOsdFonts());

        struct Resolution
        {
            int width;
            int height;
        };
        const QList<Resolution> resolutions{{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
        const QStringList backgrounds{QStringLiteral("white"), QStringLiteral("black"),
                                      QStringLiteral("noise")};

        // The default stats block (`RES`, `FPS`, `LATENCY`) every picture sheet crop shows.
        const QList<OsdStatsRow> statsRows{
            OsdStatsRow{QStringLiteral("RES"), true,
                       {OsdValuePart{QString(), QStringLiteral("1920x1080"), QString()}}},
            OsdStatsRow{QStringLiteral("FPS"), false,
                       {OsdValuePart{QString(), QStringLiteral("59.9"), QStringLiteral("fps")}}},
            OsdStatsRow{QStringLiteral("LATENCY"), false,
                       {OsdValuePart{QStringLiteral("NET"), QStringLiteral("12"), QStringLiteral("ms")},
                        OsdValuePart{QStringLiteral("TOTAL"), QStringLiteral("31"), QStringLiteral("ms")}}},
        };

        for (const Resolution& resolution : resolutions) {
            const QImage stats = renderOsdStats(statsRows, resolution.height);
            const QImage bottomWhite = renderOsdBottom(resolution.width, resolution.height,
                                                       QString(), 0,
                                                       OsdTimeLeft{true, 9, false});
            const QImage bottomRed = renderOsdBottom(
                resolution.width, resolution.height, QStringLiteral("Poor connection to PC"),
                0xFFCC0000, OsdTimeLeft{true, 4, true});
            QVERIFY(!stats.isNull());
            QVERIFY(!bottomWhite.isNull());
            QVERIFY(!bottomRed.isNull());

            for (const QString& background : backgrounds) {
                const QImage backgroundImage =
                    makeBackground(resolution.width, resolution.height, background);

                writeCrop(dir, resolution.height, background, QStringLiteral("stats"),
                         backgroundImage, stats, QPoint(0, 0));
                writeCrop(dir, resolution.height, background, QStringLiteral("bottom-white"),
                         backgroundImage, bottomWhite,
                         QPoint(0, resolution.height - bottomWhite.height()));
                writeCrop(dir, resolution.height, background, QStringLiteral("bottom-red"),
                         backgroundImage, bottomRed,
                         QPoint(0, resolution.height - bottomRed.height()));
            }
        }
    }
};

QTEST_MAIN(TestOsdRender)

#include "tst_osd_render.moc"
