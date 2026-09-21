import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// Every failure the customer ever sees, in one screen (D-51, `docs/spec/screens.md` error
// state): an inline reason, a retry action, and the ADR-0008 support reference.
//
// The reference is always mono - `docs/spec/copy.md` §5 microcopy rules put `SH-4F7KQ2` in mono and
// never translate it - and is shown whenever the control plane gave one. A failure that never reached
// it (offline) or that began on this machine has none, and the row is then not drawn at all: a label
// with nothing after it would be a blank, and a code the client made up would resolve to nothing
// (ADR-0008). The engine's own words never reach this screen: `failure.error` is SeatHub copy, and the
// raw engine text is kept out of the map the C++ facade hands over. Three kinds of sentence arrive
// here and each is shown as it is: the server's own with its reference, the deck's offline sentence in
// full, and the deck's generic sentence for anything that began on this machine.
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

        // The support reference (ADR-0008). Always mono, never translated, and one unbroken line.
        Row {
            objectName: "referenceRow"
            spacing: Metrics.s2
            visible: root.reference.length > 0

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
                wrapMode: Text.NoWrap
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
