import QtQuick
import SeatHub.Tokens 1.0

// The view for the "restoring" app state: what the customer sees while the stored credential is
// being read and confirmed with the control plane (CUST-08, `SeatHubClient.restoreSession()`).
//
// It exists so the sign-in form is never on screen while a valid credential is on disk - the form
// flashing up and then being replaced by Home is the failure this view prevents. It has no header
// and no balance element: there is no balance before the customer is confirmed (05-UI-SPEC "Screens
// and Interactions"). The line is `docs/spec/copy.md` C2, the owner-approved
// "Signing you in..." string (Sign in, restore splash on launch).
Item {
    id: root

    Column {
        anchors.centerIn: parent
        spacing: Metrics.s4
        width: Math.min(parent.width - Metrics.s16, 420)

        // The wordmark. The fork ships no wordmark asset; the product name in the display face is
        // what the sign-in card uses too.
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("SeatHub")
            color: Tokens.foregroundDefault
            font.family: Tokens.fontDisplayDefault
            font.pixelSize: Metrics.fontH1
            font.weight: Font.DemiBold
        }

        Text {
            objectName: "restoreLine"
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("Signing you in…")
            color: Tokens.foregroundMutedDefault
            font.family: Tokens.fontSansDefault
            font.pixelSize: Metrics.fontBody
        }
    }
}
