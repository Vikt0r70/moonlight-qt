import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// ADR-0070 item 9; copy.md Diagnostics notice; screens.md §48.
// Eligibility belongs to the facade. No timer, network request or upload gate lives here.
Item {
    id: root
    anchors.fill: parent
    z: 999
    property var client

    // Like SignedOutNotice, child visibility also settles in a bare QQmlComponent test.
    Rectangle {
        id: panel
        objectName: "diagnosticsPanel"
        anchors.fill: parent
        visible: root.client ? root.client.diagnosticsNoticePending === true : false
        // The existing forced-update scrim, not a new design-system colour.
        color: Qt.rgba(0, 0, 0, 0.72)

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            hoverEnabled: true
            onWheel: function(wheel) { wheel.accepted = true }
        }

        onVisibleChanged: {
            if (visible) {
                ok.forceActiveFocus()
                panel.Accessible.announce(title.text + ". " + body.text, Accessible.Polite)
            }
        }
        Accessible.role: Accessible.StaticText
        Accessible.name: title.text + ". " + body.text

        FocusScope {
            anchors.fill: parent
            focus: panel.visible
            Rectangle {
                anchors.centerIn: parent
                width: Math.min(parent.width - Metrics.s16, 480)
                height: content.implicitHeight + Metrics.s8 * 2
                radius: Metrics.radiusLg
                color: Tokens.surface1Default
                border.color: Tokens.borderDefault
                border.width: 1

                Column {
                    id: content
                    anchors.fill: parent
                    anchors.margins: Metrics.s8
                    spacing: Metrics.s3
                    Text {
                        id: title
                        width: parent.width
                        text: qsTr("What SeatHub sends us")
                        wrapMode: Text.Wrap
                        color: Tokens.foregroundDefault
                        font.family: Tokens.fontDisplayDefault
                        font.pixelSize: Metrics.fontH2
                        font.weight: Font.DemiBold
                    }
                    Text {
                        id: body
                        width: parent.width
                        text: qsTr("While you play, SeatHub sends SevenHills how the stream is running: frame rate, delay, lost frames and picture size. If an update fails, it also sends a short note about what went wrong. It never sends your PIN, password, email or phone number, or the address of the PC you rent.")
                        wrapMode: Text.Wrap
                        color: Tokens.foregroundMutedDefault
                        font.family: Tokens.fontSansDefault
                        font.pixelSize: Metrics.fontBody
                    }
                    SeatHubButton {
                        id: ok
                        objectName: "diagnosticsOK"
                        width: parent.width
                        text: qsTr("OK")
                        focus: true
                        // Keep the sole action focused; unrelated keys cannot reach the page.
                        Keys.onPressed: function(event) {
                            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                                root.client.acknowledgeDiagnosticsNotice()
                                event.accepted = true
                            } else {
                                event.accepted = event.key !== Qt.Key_Space
                            }
                        }
                        onClicked: root.client.acknowledgeDiagnosticsNotice()
                    }
                }
            }
        }
    }
}
