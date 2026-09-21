import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// The signed-in home: the one place a customer presses Play (`docs/spec/copy.md` §Play flow,
// "Ready" - `Open SeatHub and press Play.`).
//
// Every state `docs/spec/screens.md` §23 requires is here (audit F1):
//
//   loading  `SeatHubClient.homeStatus` is "checking" - Play has been pressed and the control
//            plane is being asked for a rig. The line names the thing loading (copy.md §5).
//   populated the ordinary state: Play available, the deck's "Ready" line and sentence.
//   empty    the control plane answered `AllocationRefused` with `NO_HOST_AVAILABLE`, so
//            the deck's own no-rig sentence is shown and Play stays as the one action.
//            (The `Notify me when one is free` action is Phase 5 per D-55.)
//   error    the control plane was not reachable at all - copy.md §Support & errors, offline.
//
// Interim visuals only (D-20) - the final visual language arrives with the design-system
// work; the structure and the copy are the frozen ones.
Item {
    id: root

    /// The SeatHubClient facade. The only object this screen may talk to (D-35).
    property var client

    readonly property string status: client && client.homeStatus !== undefined
                                     ? String(client.homeStatus) : "ready"
    readonly property bool checking: root.status === "checking"

    // The state line. Three of the four are copy.md verbatim; the loading line is the
    // `03-UI-SPEC.md` E2 "checking availability" line, written to copy.md §5's loading rule.
    readonly property string stateLine: {
        if (root.status === "checking")
            return qsTr("Checking availability\u2026")
        if (root.status === "busy")
            return qsTr("All rigs are busy right now.")
        if (root.status === "offline")
            return qsTr("Can't reach SevenHills right now.")
        return qsTr("Ready")
    }

    Column {
        anchors.centerIn: parent
        spacing: Metrics.s6
        width: Math.min(parent.width - Metrics.s16, 420)

        Text {
            id: stateText
            width: parent.width
            wrapMode: Text.Wrap
            text: root.stateLine
            color: Tokens.foregroundDefault
            font.family: Tokens.fontDisplayDefault
            font.pixelSize: root.status === "ready" ? Metrics.fontH1 : Metrics.fontH2
            font.weight: Font.DemiBold
        }

        Text {
            width: parent.width
            text: qsTr("Open SeatHub and press Play.")
            visible: root.status === "ready"
            color: Tokens.foregroundMutedDefault
            font.family: Tokens.fontSansDefault
            font.pixelSize: Metrics.fontBody
            wrapMode: Text.Wrap
        }

        // The last session's end reason, from copy.md §Session end reasons (audit E10). The
        // sentence wraps, so it reflows instead of clipping at 960x640, and it clears the
        // moment the next session starts.
        Text {
            id: endReasonText
            width: parent.width
            visible: text.length > 0
            text: root.client && root.client.endReasonText ? String(root.client.endReasonText) : ""
            textFormat: Text.StyledText
            wrapMode: Text.Wrap
            color: Tokens.foregroundMutedDefault
            font.family: Tokens.fontSansDefault
            font.pixelSize: Metrics.fontBody
        }

        SeatHubButton {
            id: playButton
            width: parent.width
            text: qsTr("Play")
            enabled: !root.checking

            onClicked: root.client.start()
        }

        // The signed-in identity. A phone number is not a credential, and no token or
        // header ever reaches this layer (D-35).
        Text {
            width: parent.width
            text: root.client.identity
            visible: text.length > 0
            horizontalAlignment: Text.AlignHCenter
            color: Tokens.foregroundMutedDefault
            font.family: Tokens.fontMonoDefault
            font.pixelSize: Metrics.fontSm
        }

        SeatHubButton {
            id: settingsButton
            width: parent.width
            variant: "ghost"
            text: qsTr("Settings")

            onClicked: root.client.openSettings()
        }

        SeatHubButton {
            id: signOutButton
            width: parent.width
            variant: "ghost"
            text: qsTr("Sign out")

            onClicked: root.client.signOut()
        }
    }
}
