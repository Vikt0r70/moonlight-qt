import QtQuick
import SeatHub.Tokens 1.0

// The persistent balance element (CUST-06, D-18, D-19; 05-UI-SPEC "Balance element").
//
// A rounded `--surface-2` capsule, 32px tall, showing the customer's credit in mono, tabular
// figures. It is data, not a control: not focusable, not clickable, and never the screen's one
// bright accent. The value is the wallet's `balance_minutes` as `SeatHubClient.balanceText` - the
// hours-and-minutes text (`copy.md` section 5) is made in C++ by `durationText()`, the one
// formatter every client surface uses, and this file draws it without doing any arithmetic on it.
//
// States:
//   loading    the first wallet read is still out (skeleton, after 300 ms so a fast answer never
//              flashes it)
//   populated  the value, `--foreground`
//   low        10 minutes or fewer: value, glyph and border in `--warn`
//   critical   2 minutes or fewer: the same in `--destructive`
//   last known the most recent read failed but an earlier one succeeded: the value in
//              `--foreground-muted`, followed by `last known` (copy.md C7)
//   unavailable the read failed and no read has ever succeeded: `Unavailable` (copy.md C7)
//
// The pill never disappears from a signed-in screen. Colour is never the only signal: the low and
// critical states add a glyph. (The fork ships no icon set, so the two glyphs are geometric
// characters; the spec's icons replace them when the icon assets land.)
Rectangle {
    id: root

    objectName: "balancePill"

    /// The SeatHubClient facade (D-35): `balanceMinutes`, `balanceText`, `balanceStale`.
    property var client

    // `SeatHubClient.balanceMinutes` is -1 until a read has succeeded.
    readonly property real minutes: client && client.balanceMinutes !== undefined
                                    ? Number(client.balanceMinutes) : -1
    readonly property bool known: root.minutes >= 0
    readonly property bool stale: client ? client.balanceStale === true : false

    // `screens.md` section 25: warn at 10 minutes and under, danger at 2 minutes and under. Only
    // meaningful for a value that is current.
    readonly property string level: {
        if (!root.known || root.stale)
            return "muted"
        if (root.minutes <= 2)
            return "critical"
        if (root.minutes <= 10)
            return "low"
        return "normal"
    }

    // The 300 ms before the skeleton shows (UI-SPEC "loading (first read, past 300 ms)").
    property bool pastLoadingDelay: false

    readonly property bool loading: !root.known && !root.stale

    readonly property string valueText: {
        if (root.known)
            return client && client.balanceText ? String(client.balanceText) : ""
        return root.stale ? qsTr("Unavailable") : ""
    }

    readonly property color accent: root.level === "critical" ? Tokens.destructiveDefault
                                    : root.level === "low" ? Tokens.warnDefault
                                    : Tokens.borderDefault

    // D-04, ADR-0067: the live-updates indicator (`ui.md` §7), shown only while the account push
    // channel is paused (`timing.md` L5) - independent of `loading`/`level`, which are about the
    // balance itself.
    readonly property bool paused: client ? client.liveUpdatesPaused === true : false

    // D-05, ADR-0067: one opacity pulse when a push changes the value already on screen - never on
    // the first known value, and never for a read that answers with the same figure.
    property bool everKnownMinutes: false
    property real previousKnownMinutes: 0
    onMinutesChanged: {
        // Read straight off `root.minutes` (this handler's own trigger, always fresh) rather than
        // `root.known`: a readonly property one binding hop further away can still be evaluating
        // last tick's value at the instant a sibling property's own change handler runs, and
        // `root.minutes < 0` says exactly what `!root.known` would once it catches up.
        if (root.minutes < 0) {
            // Nothing to compare a later value against yet (still loading, or every read has
            // failed with none ever succeeding) - and no pulse for going stale either.
            return
        }
        if (root.everKnownMinutes && root.minutes !== root.previousKnownMinutes
                && client && client.animationEffects !== false) {
            pulseAnimation.restart()
        }
        root.everKnownMinutes = true
        root.previousKnownMinutes = root.minutes
    }

    height: Metrics.s8
    implicitWidth: Math.max(Metrics.s24, layout.implicitWidth + Metrics.s4 * 2)
    radius: height / 2
    color: root.loading && root.pastLoadingDelay ? Tokens.surface3Default : Tokens.surface2Default
    border.width: 1
    border.color: root.accent

    // `Your credit` is copy.md's own name for the balance; the value follows it.
    Accessible.role: Accessible.StaticText
    Accessible.name: root.loading ? qsTr("Your credit: loading")
                                  : qsTr("Your credit: ") + root.valueText
                                    + (root.known && root.stale ? " " + qsTr("last known") : "")

    Timer {
        interval: 300
        running: root.loading
        onTriggered: root.pastLoadingDelay = true
    }

    // D-05: 180ms (`Tokens.durationBaseDefault`, read as a number through `Metrics.motionBase`
    // exactly as every other fork QML file reads a duration token), 0.6 to 1, `--ease-out`
    // (`Easing.OutCubic`, the same mapping `SeatHubIdentifierField.qml`'s own reveal already
    // uses). A standalone `NumberAnimation`, not an `Animator`: an `Animator` runs on the scene
    // graph's render thread and needs a real, showing window to progress at all, which a shell
    // test that only ever calls `QQmlComponent::create()` never has.
    NumberAnimation {
        id: pulseAnimation
        objectName: "balancePulse"
        target: valueLabel
        property: "opacity"
        from: 0.6
        to: 1
        duration: Metrics.motionBase
        easing.type: Easing.OutCubic
    }

    Row {
        id: layout
        anchors.centerIn: parent
        spacing: Metrics.s2

        Row {
            id: content
            anchors.verticalCenter: parent.verticalCenter
            spacing: Metrics.s2
            visible: !root.loading

            // Low and critical carry a glyph so colour is not the only signal.
            Text {
                visible: root.level === "low" || root.level === "critical"
                anchors.verticalCenter: parent.verticalCenter
                text: root.level === "critical" ? "◆" : "▲"
                color: root.level === "critical" ? Tokens.destructiveDefault : Tokens.warnDefault
                font.pixelSize: Metrics.fontSm
            }

            Text {
                id: valueLabel
                anchors.verticalCenter: parent.verticalCenter
                text: root.valueText
                color: root.level === "critical" ? Tokens.destructiveDefault
                       : root.level === "low" ? Tokens.warnDefault
                       : root.level === "muted" ? Tokens.foregroundMutedDefault
                       : Tokens.foregroundDefault
                font.family: Tokens.fontMonoDefault
                font.pixelSize: Metrics.fontSm
                font.weight: Font.DemiBold
                font.features: { "tnum": 1 }
            }

            // A cached value the last read could not refresh (copy.md C7, the state matrix's word).
            Text {
                visible: root.known && root.stale
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("last known")
                color: Tokens.foregroundMutedDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontCaption
            }
        }

        // D-04: a warn dot plus copy.md's own indicator sentence - never colour alone, never
        // blocking, gone the moment the stream is live again. Independent of `content`'s own
        // `loading`/`level` states.
        Row {
            id: pausedIndicator
            anchors.verticalCenter: parent.verticalCenter
            spacing: Metrics.s1
            visible: root.paused

            Rectangle {
                objectName: "liveUpdatesDot"
                width: Metrics.s2
                height: Metrics.s2
                radius: width / 2
                color: Tokens.warnDefault
                anchors.verticalCenter: parent.verticalCenter
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Reconnecting to live updates…")
                color: Tokens.foregroundMutedDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontCaption
            }
        }
    }
}
