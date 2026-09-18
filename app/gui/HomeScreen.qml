import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// The signed-in home: the one place a customer presses Play (`docs/spec/copy.md` §Play
// flow, "Ready" - `Open SeatHub and press Play.`).
//
// Interim visuals only (D-20) - the final visual language arrives with the design-system
// work; the structure and the copy are already the frozen ones.
Item {
    id: root

    /// The SeatHubClient facade. The only object this screen may talk to (D-35).
    property var client

    Column {
        anchors.centerIn: parent
        spacing: Metrics.s6
        width: Math.min(parent.width - Metrics.s16, 420)

        Text {
            text: qsTr("Ready")
            color: Tokens.foregroundDefault
            font.family: Tokens.fontDisplayDefault
            font.pixelSize: Metrics.fontH1
            font.weight: Font.DemiBold
        }

        Text {
            text: qsTr("Open SeatHub and press Play.")
            color: Tokens.foregroundMutedDefault
            font.family: Tokens.fontSansDefault
            font.pixelSize: Metrics.fontBody
            wrapMode: Text.Wrap
        }

        Button {
            id: playButton
            width: parent.width
            height: Metrics.touchTarget
            text: qsTr("Play")

            contentItem: Text {
                text: playButton.text
                color: Tokens.primaryForegroundDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontBody
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }

            background: Rectangle {
                radius: Metrics.radiusSm
                color: playButton.pressed ? Tokens.surface3Default : Tokens.primaryDefault
            }

            onClicked: root.client.start()
        }

        // The signed-in identity. A phone number is not a credential, and no token or
        // header ever reaches this layer (D-35).
        Text {
            width: parent.width
            text: root.client.identity
            visible: text.length > 0
            horizontalAlignment: Text.AlignHCenter
            color: Tokens.foregroundSubtleDefault
            font.family: Tokens.fontMonoDefault
            font.pixelSize: Metrics.fontSm
        }

        Text {
            text: qsTr("Settings")
            color: Tokens.foregroundMutedDefault
            font.family: Tokens.fontSansDefault
            font.pixelSize: Metrics.fontCaption
            horizontalAlignment: Text.AlignHCenter
            width: parent.width

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: root.client.openSettings()
            }
        }

        Text {
            text: qsTr("Sign out")
            color: Tokens.foregroundSubtleDefault
            font.family: Tokens.fontSansDefault
            font.pixelSize: Metrics.fontCaption
            horizontalAlignment: Text.AlignHCenter
            width: parent.width

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: root.client.signOut()
            }
        }
    }
}
