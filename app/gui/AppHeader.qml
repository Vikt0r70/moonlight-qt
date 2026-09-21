import QtQuick
import SeatHub.Tokens 1.0

// The signed-in header (05-UI-SPEC "Screens and Interactions", D-19; CUST-06).
//
// A 64px row over a 1px hairline: the wordmark on the left and, on the right and in this order, the
// balance element and the menu button. It carries the balance on every signed-in screen, so the
// customer's credit never leaves the corner while they are signed in.
//
// It takes the facade as its only property and talks to nothing else. Which views get it is
// `main.qml`'s decision (every signed-in view; not sign-in, not the restore splash - there is no
// balance before sign-in), so this file has no idea what screen it sits over.
//
// The balance element is data, not a control: it is not focusable and never the screen's one bright
// accent, so Home keeps exactly one bright control.
Item {
    id: root

    objectName: "appHeader"

    /// The SeatHubClient facade. The only object this header may talk to (D-35).
    property var client

    implicitHeight: Metrics.s16
    height: Metrics.s16

    Rectangle {
        anchors.fill: parent
        color: Tokens.backgroundDefault
    }

    // The wordmark. The fork ships no wordmark asset; the product name in the display face is what the
    // sign-in card and the restore splash use too.
    Text {
        id: wordmark
        anchors.left: parent.left
        anchors.leftMargin: Metrics.s8
        anchors.verticalCenter: parent.verticalCenter
        text: qsTr("SeatHub")
        color: Tokens.foregroundDefault
        font.family: Tokens.fontDisplayDefault
        font.pixelSize: Metrics.fontH3
        font.weight: Font.DemiBold
    }

    Row {
        anchors.right: parent.right
        anchors.rightMargin: Metrics.s8
        anchors.verticalCenter: parent.verticalCenter
        spacing: Metrics.s3

        BalancePill {
            id: balancePill
            anchors.verticalCenter: parent.verticalCenter
            client: root.client
        }

        SeatHubMenu {
            id: menu
            anchors.verticalCenter: parent.verticalCenter
            client: root.client
        }
    }

    // The 1px hairline under the row.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Tokens.borderDefault
    }
}
