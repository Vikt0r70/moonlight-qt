import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// The header's small menu (D-19, 05-UI-SPEC "Menu").
//
// A 40x40 trigger and a popup anchored under it, right-aligned. This plan ships two items - `Top up`
// and `Settings` - and the profile joins them when its screen exists, so nothing here points at a
// screen that is not there yet.
//
// `Top up` leaves the app: it asks the facade to open the website's top-up page, and it carries the
// external-link mark. No address is built in this file - the facade owns every website address, and
// a test asserts no SeatHub QML names one. `Settings` opens the settings view the app already has.
//
// Keyboard, from the design contract: Enter or Space on the trigger opens it, the arrow keys move
// through the items, Enter activates one, Escape closes and hands focus back to the trigger, and a
// click outside closes. `Menu` supplies the arrow-key, Enter and Escape behaviour; the outside click
// is its close policy, and the focus return is the one explicit line below.
//
// The fork ships no icon set (WINDOWS #25), so the trigger's mark and the external-link mark are
// geometric characters, as on the sign-in screen.
Item {
    id: root

    objectName: "seatHubMenu"

    /// The SeatHubClient facade. The only object this menu may talk to (D-35).
    property var client

    /// The popup and its trigger, exposed so a test can drive them.
    readonly property alias menu: menu
    readonly property alias trigger: trigger

    implicitWidth: Metrics.pointerTarget
    implicitHeight: Metrics.pointerTarget

    Button {
        id: trigger

        objectName: "menuButton"
        anchors.fill: parent
        activeFocusOnTab: true

        Accessible.role: Accessible.Button
        Accessible.name: qsTr("Menu")

        // Enter and Space both arrive here as `clicked`. A second press while it is open closes it.
        onClicked: menu.opened ? menu.close() : menu.open()

        contentItem: Text {
            text: "☰"
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: Tokens.foregroundMutedDefault
            font.family: Tokens.fontSansDefault
            font.pixelSize: Metrics.s5
        }

        background: Rectangle {
            radius: Metrics.radiusSm
            border.width: trigger.activeFocus ? 2 : 0
            border.color: Tokens.focusDefault
            color: trigger.hovered || trigger.activeFocus || menu.opened ? Tokens.primarySoftDefault
                                                                         : "transparent"
            Behavior on color {
                ColorAnimation { duration: Metrics.motionFast }
            }
        }
    }

    // One row: the label, and the external-link mark on an item that leaves the app. 40px tall,
    // `s4` of padding each side, body text.
    component MenuRow: MenuItem {
        id: row

        /// A trailing mark. Decoration only: the accessible name stays the label.
        property string glyph: ""

        implicitHeight: Metrics.pointerTarget
        implicitWidth: Math.max(contentItem.implicitWidth + Metrics.s4 * 2, Metrics.s24 * 2)
        leftPadding: Metrics.s4
        rightPadding: Metrics.s4

        Accessible.role: Accessible.MenuItem
        Accessible.name: row.text

        contentItem: Row {
            spacing: Metrics.s3

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: row.text
                color: Tokens.foregroundDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontBody
            }

            Text {
                visible: row.glyph.length > 0
                anchors.verticalCenter: parent.verticalCenter
                text: row.glyph
                color: Tokens.foregroundMutedDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontBody
            }
        }

        background: Rectangle {
            radius: Metrics.radiusXs
            border.width: row.activeFocus ? 2 : 0
            border.color: Tokens.focusDefault
            color: row.highlighted ? Tokens.primarySoftDefault : "transparent"
        }
    }

    Menu {
        id: menu

        objectName: "menuPopup"
        x: root.width - width
        y: root.height + Metrics.s1
        padding: Metrics.s1
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        // Escape (or a click outside) closes it and the trigger takes focus back, so the customer is
        // never left with focus on nothing.
        onClosed: trigger.forceActiveFocus()

        background: Rectangle {
            radius: Metrics.radiusSm
            color: Tokens.surface3Default
            border.width: 1
            border.color: Tokens.borderDefault
        }

        MenuRow {
            objectName: "menuItemTopUp"
            text: qsTr("Top up")
            glyph: "↗"

            onTriggered: root.client.openTopUp()
        }

        MenuRow {
            objectName: "menuItemSettings"
            text: qsTr("Settings")

            onTriggered: root.client.openSettings()
        }
    }
}
