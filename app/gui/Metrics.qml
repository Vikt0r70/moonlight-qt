pragma Singleton
import QtQuick

// SeatHub's fork-side companion to the generated `Tokens.qml`.
//
// The generated token file is shared with the web app, so it carries spacing and radius
// values as CSS-ish strings ("16px") and keeps them as `var`. QML's `spacing`, `radius`,
// `font.pixelSize` and friends are numbers, so this file parses them once. It is
// hand-written precisely so `Tokens.qml` stays byte-identical to `npm run tokens:build`
// output and can be regenerated without a merge.
QtObject {
    // Spacing scale, mirrored from Tokens.stepN.
    readonly property int s1: 4
    readonly property int s2: 8
    readonly property int s3: 12
    readonly property int s4: 16
    readonly property int s5: 20
    readonly property int s6: 24
    readonly property int s8: 32
    readonly property int s10: 40
    readonly property int s12: 48
    readonly property int s16: 64
    readonly property int s20: 80
    readonly property int s24: 96

    // Radii, mirrored from Tokens.xs/sm/md/lg.
    readonly property int radiusXs: 6
    readonly property int radiusSm: 8
    readonly property int radiusMd: 16
    readonly property int radiusLg: 20

    // Type scale, mirrored from Tokens.scale*Size (rem -> px at a 16px root).
    readonly property int fontCaption: 13
    readonly property int fontLabel: 12
    readonly property int fontSm: 14
    readonly property int fontBody: 16
    readonly property int fontH3: 20
    readonly property int fontH2: 24
    readonly property int fontH1: 32

    // Motion, mirrored from Tokens.duration*Default.
    readonly property int motionFast: 120
    readonly property int motionBase: 180
    readonly property int motionSlow: 260
    readonly property int motionPage: 420

    // The minimum interactive target from docs/spec/ui.md.
    readonly property int touchTarget: 44

    // Parses a CSS-ish token value ("16px", "1.6") into a number.
    function px(value) {
        var parsed = parseInt(value, 10)
        return isNaN(parsed) ? 0 : parsed
    }
}
