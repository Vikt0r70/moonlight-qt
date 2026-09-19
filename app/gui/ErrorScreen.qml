import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// Every failure the customer ever sees, in one screen (D-51, `docs/spec/screens.md` error
// state): an inline reason, a retry action, and the ADR-0008 support reference.
//
// The reference is always present and always mono - `docs/spec/copy.md` §5 microcopy rules
// put `SH-4F7KQ2` in mono and never translate it. The engine's own words never reach this
// screen: `failure.error` is SeatHub copy, and the raw engine text is kept out of the map
// the C++ facade hands over.
Item {
    id: root

    /// The SeatHubClient facade. The only object this screen may talk to (D-35).
    property var client

    readonly property string errorText: (client && client.failure && client.failure.error)
                                        ? client.failure.error
                                        : qsTr("Something went wrong on our side.")
    readonly property string reference: (client && client.reference) ? client.reference : ""

    Column {
        anchors.centerIn: parent
        spacing: Metrics.s6
        width: Math.min(parent.width - Metrics.s16, 420)

        Text {
            text: qsTr("That didn't work")
            color: Tokens.foregroundDefault
            font.family: Tokens.fontDisplayDefault
            font.pixelSize: Metrics.fontH2
            font.weight: Font.DemiBold
        }

        // The inline reason. Reflows rather than clipping at 960x640 (UI-SPEC E10).
        Text {
            width: parent.width
            text: root.errorText
            wrapMode: Text.Wrap
            color: Tokens.foregroundMutedDefault
            font.family: Tokens.fontSansDefault
            font.pixelSize: Metrics.fontBody
        }

        // The support reference (ADR-0008). Always mono, never translated.
        Row {
            spacing: Metrics.s2

            Text {
                text: qsTr("Reference")
                color: Tokens.foregroundMutedDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontSm
                anchors.verticalCenter: parent.verticalCenter
            }

            Text {
                id: referenceText
                text: root.reference
                visible: text.length > 0
                color: Tokens.foregroundDefault
                font.family: Tokens.fontMonoDefault
                font.pixelSize: Metrics.fontSm
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        SeatHubButton {
            id: retryButton
            width: parent.width
            text: qsTr("Try again")

            onClicked: root.retry()
        }

        SeatHubButton {
            id: backButton
            width: parent.width
            variant: "ghost"
            text: qsTr("Back to home")

            onClicked: root.client.dismissError()
        }
    }

    function retry() {
        // Retry runs the last step again (audit F21): Play, or the sign-in screen when there
        // is no identity to play with yet. `SeatHubClient.retry` owns that decision.
        root.client.retry()
    }
}
