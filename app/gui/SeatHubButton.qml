import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// The one action control in the SeatHub shell (`docs/spec/ui.md` §7 Actions, 03-UI-SPEC §Components).
//
// Two of the spec's action variants:
//   * `primary` - the single filled action on a view (Play, Send code, Verify and continue,
//     Try again, Update). 44px tall, the touch floor from ui.md §9.
//   * `ghost`   - the quiet text actions (Cancel, Settings, Sign out, Back, Back to home). Still a
//     real control: focusable, at least 40x40, with an accessible name. The audit's F3 was five
//     `Text` + `MouseArea` actions that no keyboard could reach and no screen reader could name.
//
// `Button` and not a bespoke Item: Qt Quick Controls supplies the tab chain, Space/Enter
// activation, the Accessible role and the pressed/hovered states, and `tst_update_feed`'s
// "exactly one button, labelled Update" assertion keeps holding because this is a QQuickButton.
//
// Disabled state (F5): ui.md §3.2 lets disabled text fall to 3:1 provided the disabled affordance
// survives. `foregroundMutedDefault` (#A3A3A3) on `surface3Default` (#262626) is 6.2:1, against
// the ~1.19:1 the audit measured for `primaryForegroundDefault` on the same fill.
//
// Focus ring (F14): ui.md §9 - a 2px `--focus` ring, always visible, never removed. The ring is a
// 2px border rather than an extra Rectangle so the button's box never changes size on focus.
Button {
    id: root

    /// "primary" | "ghost" | "destructive"
    property string variant: "primary"

    /// ui.md §7: a loading button keeps its width and swaps in a spinner. The label stays put so
    /// nothing on the page moves when work starts.
    property bool busy: false

    readonly property bool isGhost: root.variant === "ghost"
    readonly property bool isDestructive: root.variant === "destructive"

    implicitHeight: root.isGhost ? Metrics.pointerTarget : Metrics.touchTarget
    implicitWidth: Math.max(contentItem.implicitWidth + Metrics.s6,
                            root.isGhost ? Metrics.pointerTarget : 0)

    // ui.md §9: every action is reachable by keyboard, and the tab ring is visible.
    activeFocusOnTab: true

    Accessible.role: Accessible.Button
    Accessible.name: root.text

    contentItem: Row {
        spacing: Metrics.s3
        anchors.centerIn: parent

        BusyIndicator {
            visible: root.busy
            running: root.busy
            width: visible ? Metrics.s5 : 0
            height: Metrics.s5
            anchors.verticalCenter: parent.verticalCenter
        }

        Text {
            text: root.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.NoWrap
            elide: Text.ElideRight
            color: !root.enabled ? Tokens.foregroundMutedDefault
                                 : (root.isDestructive ? Tokens.destructiveForegroundDefault
                                                       : (root.isGhost ? Tokens.foregroundMutedDefault
                                                                       : Tokens.primaryForegroundDefault))
            font.family: Tokens.fontSansDefault
            font.pixelSize: root.isGhost ? Metrics.fontCaption : Metrics.fontBody
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    background: Rectangle {
        radius: Metrics.radiusSm
        border.width: root.activeFocus ? 2 : 0
        border.color: Tokens.focusDefault
        // ui.md §6: hover/press/focus change in `--dur-fast` (120ms), and only colour animates.
        color: {
            if (!root.enabled) {
                return root.isGhost ? "transparent" : Tokens.surface3Default
            }
            if (root.isGhost) {
                return root.hovered || root.activeFocus ? Tokens.primarySoftDefault : "transparent"
            }
            if (root.isDestructive) {
                return root.pressed ? Qt.darker(Tokens.destructiveDefault, 1.15)
                                    : Tokens.destructiveDefault
            }
            return root.pressed ? Tokens.surface3Default : Tokens.primaryDefault
        }
        Behavior on color {
            ColorAnimation { duration: Metrics.motionFast }
        }
    }
}
