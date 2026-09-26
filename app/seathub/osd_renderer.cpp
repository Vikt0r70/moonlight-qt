#include "osd_renderer.h"

// RED-phase stub (Task 1, 06.6-06): every function returns the "nothing built yet" value so the
// suite links and runs, and `tst_osd_render`'s behavior slots fail on their assertions rather than
// on a build, link or resource error - the #3770 distinction between a real RED and an INVALID_RED.
// Replaced by the real renderer in the GREEN commit that follows.

bool registerOsdFonts()
{
    return false;
}

QFont osdFont(int pixelSize)
{
    QFont font;
    font.setPixelSize(pixelSize);
    return font;
}

OsdSizes osdSizesFor(int windowHeight)
{
    Q_UNUSED(windowHeight);
    return OsdSizes{};
}

QImage renderOsdStats(const QList<OsdStatsRow>& rows, int windowHeight)
{
    Q_UNUSED(rows);
    Q_UNUSED(windowHeight);
    return QImage();
}

QImage renderOsdBottom(int windowWidth, int windowHeight, const QString& engineText,
                       QRgb engineColor, const OsdTimeLeft& timeLeft)
{
    Q_UNUSED(windowWidth);
    Q_UNUSED(windowHeight);
    Q_UNUSED(engineText);
    Q_UNUSED(engineColor);
    Q_UNUSED(timeLeft);
    return QImage();
}
