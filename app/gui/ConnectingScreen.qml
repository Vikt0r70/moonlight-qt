import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// The connecting view (`docs/spec/screens.md` §24, `05-UI-SPEC` "Connecting"; CUST-12, CUST-13). The
// header above it is `main.qml`'s; this is the centred column under it: the three-stage stepper and,
// beneath it, either the one way out of a wait (`Cancel`) or - when connecting has stopped - where it
// stopped and the two ways on.
//
// It is its own file, and not a component inside `main.qml`, so a test can load it with a stand-in
// facade and read what it shows; `main.qml` cannot be instantiated without the real facade type.
//
// A stall is named by the failure the facade reports, never by a timer of this screen's: the stage
// that was active turns destructive in the stepper, `Stopped at: {stage}` and the sentence for what
// the server decided (or the client's own pairing deadline) sit under it, and the two actions are
// `Try again` and `Back to home`. The customer is never offered another rig, a rig list or a retry
// against a named machine: the server chooses (CUST-03). Every word is the copy deck's or the
// server's own; no state name, end-reason key or engine stage string is drawn here (CUST-10).
//
// A reference, when the control plane gave one, is mono and never broken across lines (ADR-0008); a
// failure with none - a client-local one, or a session the server ended without saying why - draws no
// reference row at all rather than a label with nothing after it.
//
// Long sentences wrap inside the 420px column: the longest end-reason sentence and the stalled-step
// line stack rather than clip at the smallest supported window.
Item {
    id: root

    /// The SeatHubClient facade. The only object this screen may talk to (D-35).
    property var client: null

    readonly property bool failed: root.client && root.client.connectFailed === true
    readonly property string stalledStep: root.client && root.client.stalledStepText
                                          ? String(root.client.stalledStepText) : ""
    readonly property string stalledReason: root.client && root.client.stalledReasonText
                                            ? String(root.client.stalledReasonText) : ""
    readonly property string reference: root.client && root.client.reference
                                        ? String(root.client.reference) : ""

    // 06.1-19/J-07: the Reconnecting state (`screens.md` §24, `copy.md` §Reconnect). Reuses this
    // same column and stepper - no new screen, no new component, token or colour - because
    // `appState` deliberately stays "connecting" throughout a reconnect (D-32/main.qml is not
    // this plan's to edit); this property is the only thing that tells the two apart.
    readonly property bool reconnecting: root.client && root.client.reconnecting === true
    readonly property bool reconnectEnded: root.client && root.client.reconnectEnded === true
    readonly property int reconnectCount: root.client && root.client.reconnectCount !== undefined
                                          ? Number(root.client.reconnectCount) : 0
    readonly property int graceMinutesLeft: root.client && root.client.graceMinutesLeft !== undefined
                                            ? Number(root.client.graceMinutesLeft) : -1
    readonly property bool reconnectStillTrying: root.client && root.client.reconnectStillTrying === true

    // `Connection lost. Reconnecting… (n of 5)` until the first attempt has failed or timed out,
    // then `Still trying. {minutes} left to reconnect.` from the server's own grace deadline -
    // never a client-owned countdown (`{minutes}` uses `1 minute` in the singular, copy.md).
    readonly property string reconnectLineText: {
        if (!root.reconnectStillTrying) {
            return qsTr("Connection lost. Reconnecting… (%1 of 5)").arg(root.reconnectCount + 1)
        }
        var minutes = Math.max(root.graceMinutesLeft, 0)
        return minutes === 1
            ? qsTr("Still trying. 1 minute left to reconnect.")
            : qsTr("Still trying. %1 minutes left to reconnect.").arg(minutes)
    }

    Column {
        id: column
        anchors.centerIn: parent
        spacing: Metrics.s6
        width: Math.min(parent.width - Metrics.s16, 420)

        // The connect phase is the deck's three stages, each a real transition, not a spinner that
        // resets (ui.md §6, audit F8, CUST-12).
        SeatHubStepper {
            id: stepper
            objectName: "stepper"
            width: parent.width
            client: root.client
            failed: root.failed
        }

        // 06.1-19/J-07: shown while an automatic retry is in flight - the third stage's own
        // breathing warn dot (`SeatHubStepper.qml`'s existing "active" look, unchanged) is the
        // live signal; this line names the count or the time left, from the server.
        Text {
            objectName: "reconnectLine"
            width: parent.width
            wrapMode: Text.Wrap
            visible: root.reconnecting
            text: root.reconnectLineText
            color: Tokens.foregroundDefault
            font.family: Tokens.fontSansDefault
            font.pixelSize: Metrics.fontBody
            lineHeight: 1.4
        }

        // Where it stopped, and why, in the deck's words.
        Column {
            id: stalled
            objectName: "stalled"
            width: parent.width
            spacing: Metrics.s2
            visible: root.failed

            Row {
                width: parent.width
                spacing: Metrics.s2

                Text {
                    id: stalledMark
                    text: "◆"
                    color: Tokens.destructiveDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontBody
                }

                Text {
                    objectName: "stalledStep"
                    width: parent.width - stalledMark.width - parent.spacing
                    text: root.stalledStep
                    wrapMode: Text.Wrap
                    color: Tokens.destructiveDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontBody
                    font.weight: Font.DemiBold
                }
            }

            // Styled text: the end-reason sentences carry their minute count in the mono family. The
            // facade escapes any sentence that is not the deck's, so nothing here is read as markup.
            Text {
                objectName: "stalledReason"
                width: parent.width
                text: root.stalledReason
                textFormat: Text.StyledText
                wrapMode: Text.Wrap
                color: Tokens.foregroundDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontBody
                lineHeight: 1.4
            }

            Row {
                objectName: "stalledReferenceRow"
                spacing: Metrics.s2
                visible: root.reference.length > 0

                Text {
                    text: qsTr("Reference")
                    color: Tokens.foregroundMutedDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontSm
                }

                Text {
                    objectName: "stalledReference"
                    text: root.reference
                    wrapMode: Text.NoWrap
                    color: Tokens.foregroundDefault
                    font.family: Tokens.fontMonoDefault
                    font.pixelSize: Metrics.fontSm
                }
            }
        }

        // Stopped: try again (the primary action), or go home. There is no other choice to make.
        // 06.1-19/J-07: a reconnect the server gave up on is terminal - the session is over, so
        // `Try again` (which would end THIS session and start
        // a fresh one) is never offered for it; only `Back to home` is (screens.md §24).
        SeatHubButton {
            id: tryAgainButton
            objectName: "tryAgainButton"
            width: parent.width
            visible: root.failed && !root.reconnectEnded
            // D-05/C4: Try again always ends the old, never-streamed session first and waits for
            // it before a fresh Play goes out - the busy state is exactly that wait, not a local
            // spinner of this screen's own (`retryBusy`, `seathub_client.h`).
            busy: root.client && root.client.retryBusy === true
            text: qsTr("Try again")

            onClicked: root.client.retry()
        }

        SeatHubButton {
            id: backButton
            objectName: "backButton"
            width: parent.width
            variant: "ghost"
            visible: root.failed
            text: qsTr("Back to home")

            onClicked: root.client.dismissError()
        }

        // While it is still going: the one way out of a wait. A real control (audit F3). Not
        // offered during an automatic reconnect (screens.md §24) - `End session` below is.
        SeatHubButton {
            id: cancelButton
            objectName: "cancelButton"
            width: parent.width
            variant: "ghost"
            visible: !root.failed && !root.reconnecting
            text: qsTr("Cancel")

            onClicked: root.client.interrupt()
        }

        // 06.1-19/J-07: ends the held session at once, through the ordinary end - billing
        // continues while it is held, so the customer can always stop it (screens.md §24).
        SeatHubButton {
            id: endSessionButton
            objectName: "endSessionButton"
            width: parent.width
            variant: "ghost"
            visible: root.reconnecting
            text: qsTr("End session")

            onClicked: root.client.endHeldSession()
        }
    }
}
