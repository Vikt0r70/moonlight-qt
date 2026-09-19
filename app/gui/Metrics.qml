pragma Singleton
import QtQuick
import SeatHub.Tokens 1.0

// SeatHub's fork-side companion to the generated `Tokens.qml`.
//
// The generated file is shared with the web app, so it carries spacing, radius, type and motion
// values as CSS-ish strings ("16px", "2rem", "120ms") and keeps them as `var`. QML's `spacing`,
// `radius`, `font.pixelSize` and animation durations are numbers, so this file parses the tokens
// once, at load. Every value below is derived from `Tokens` - there is no second copy of any
// number here (audit F15) - and `Tokens.qml` stays byte-identical to `npm run tokens:build`
// output so it can be regenerated without a merge.
//
// Not derived, because no token can express it in QML: `scaleDisplaySize` is a CSS `clamp()`
// string, so the one display-size consumer would need a fixed value; nothing consumes it yet.
QtObject {
    // ui.md §4.1: the type scale is in rem against a 16px root.
    readonly property int rootFontPx: 16

    // "16px" -> 16, "16" -> 16.
    function tokenPx(value) {
        var parsed = parseFloat(String(value))
        return isNaN(parsed) ? 0 : Math.round(parsed)
    }

    // "2rem" -> 32 at the 16px root; a bare number passes through.
    function tokenRem(value) {
        var raw = String(value)
        var parsed = parseFloat(raw)
        if (isNaN(parsed))
            return 0
        return raw.indexOf("rem") !== -1 ? Math.round(parsed * rootFontPx)
                                         : Math.round(parsed)
    }

    // Spacing scale (ui.md §5), from the generated steps.
    readonly property int s1: tokenPx(Tokens.step1)
    readonly property int s2: tokenPx(Tokens.step2)
    readonly property int s3: tokenPx(Tokens.step3)
    readonly property int s4: tokenPx(Tokens.step4)
    readonly property int s5: tokenPx(Tokens.step5)
    readonly property int s6: tokenPx(Tokens.step6)
    readonly property int s8: tokenPx(Tokens.step8)
    readonly property int s10: tokenPx(Tokens.step10)
    readonly property int s12: tokenPx(Tokens.step12)
    readonly property int s16: tokenPx(Tokens.step16)
    readonly property int s20: tokenPx(Tokens.step20)
    readonly property int s24: tokenPx(Tokens.step24)

    // Radii (ui.md §5).
    readonly property int radiusXs: tokenPx(Tokens.xsDefault)
    readonly property int radiusSm: tokenPx(Tokens.smDefault)
    readonly property int radiusMd: tokenPx(Tokens.mdDefault)
    readonly property int radiusLg: tokenPx(Tokens.lgDefault)

    // Type scale (ui.md §4.1).
    readonly property int fontCaption: tokenRem(Tokens.scaleCaptionSize)
    readonly property int fontLabel: tokenRem(Tokens.scaleLabelSize)
    readonly property int fontSm: tokenRem(Tokens.scaleSmSize)
    readonly property int fontBody: tokenRem(Tokens.scaleBodySize)
    readonly property int fontH3: tokenRem(Tokens.scaleH3Size)
    readonly property int fontH2: tokenRem(Tokens.scaleH2Size)
    readonly property int fontH1: tokenRem(Tokens.scaleH1Size)

    // Motion (ui.md §6).
    readonly property int motionFast: tokenPx(Tokens.durationFastDefault)
    readonly property int motionBase: tokenPx(Tokens.durationBaseDefault)
    readonly property int motionSlow: tokenPx(Tokens.durationSlowDefault)
    readonly property int motionPage: tokenPx(Tokens.durationPageDefault)

    // Targets (ui.md §9): >=44px for touch, >=40px for pointer.
    readonly property int touchTarget: 44
    readonly property int pointerTarget: 40
}
