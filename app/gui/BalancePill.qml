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

    // `SeatHubClient.balanceMinutes` is signed (a negative balance is real, ADR-0075) and reads 0
    // until a read has succeeded, so it cannot say whether the number is known: `balanceText`
    // can, because it is empty until then and `durationText()` never returns empty for a number.
    readonly property real minutes: client && client.balanceMinutes !== undefined
                                    ? Number(client.balanceMinutes) : 0
    readonly property bool known: root.balanceKnownNow()
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

    // D-04, ADR-0067: the live-updates indicator (`ui.md` §7), shown only while live updates are
    // actually lost (`timing.md` L5, R7: no `stream.hello` for 10 s from the last close or from
    // start-up) - independent of `loading`/`level`, which are about the balance itself.
    readonly property bool paused: client ? client.liveUpdatesPaused === true : false

    // D-05, ADR-0067: one opacity pulse when a push changes the value already on screen - never on
    // the first known value, and never for a read that answers with the same figure.
    property bool everKnownMinutes: false
    property real previousKnownMinutes: 0

    // True when the facade holds a balance (negative included); read straight off the facade.
    function balanceKnownNow() {
        return client ? (client.balanceText !== undefined && String(client.balanceText) !== "") : false
    }

    function noteBalance() {
        // Read straight off the facade rather than `root.minutes` / `root.known`: a readonly
        // property one binding hop further away can still be evaluating last tick's value at the
        // instant a sibling property's own change handler runs.
        if (!root.balanceKnownNow()) {
            // Nothing to compare a later value against yet (still loading, or every read has
            // failed with none ever succeeding) - and no pulse for going stale either.
            return
        }
        var current = Number(client.balanceMinutes)
        if (root.everKnownMinutes && current !== root.previousKnownMinutes
                && client.animationEffects !== false) {
            pulseAnimation.restart()
        }
        root.everKnownMinutes = true
        root.previousKnownMinutes = current
    }

    // Both triggers: the first read can leave the number at 0 (unknown reads 0 too), so only the
    // known flip tells that a value has arrived. Calling twice for one change is harmless.
    onMinutesChanged: root.noteBalance()
    onKnownChanged: root.noteBalance()

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

            // Low and critical carry a glyph so colour is not the only signal. It is drawn at the
            // 20px inline icon size of `docs/spec/ui.md` §8 (WINDOWS #25), and the pill's own
            // `Accessible.name` already says the value, so the decoration is not read out a second
            // time.
            Text {
                objectName: "balanceGlyph"
                visible: root.level === "low" || root.level === "critical"
                anchors.verticalCenter: parent.verticalCenter
                text: root.level === "critical" ? "◆" : "▲"
                color: root.level === "critical" ? Tokens.destructiveDefault : Tokens.warnDefault
                font.pixelSize: Metrics.s5
                Accessible.ignored: true
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
