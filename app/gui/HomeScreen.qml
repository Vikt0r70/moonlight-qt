import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// The signed-in home: one large Play control and, at most, one honest line (`docs/spec/screens.md`
// §23, `05-UI-SPEC` "Home"; CUST-03, CUST-04, D-19, D-20). The header above it (`AppHeader`, wired
// in `main.qml`) carries the balance and the menu, so this screen holds neither.
//
// Every state §23 requires is here (audit F1):
//
//   ready     nothing but Play. While a session is live the control reads `Resume session` and
//             resumes that session instead of asking for another one (`client.liveSession`). When the
//             previous session ended, its end reason sits above Play as one quiet sentence.
//   checking  `client.homeStatus` is "checking": Play has been pressed and the control plane is being
//             asked for a rig. The line names the thing loading (copy.md §5) and Play is busy.
//   busy      the control plane said nothing is free. The deck's one sentence and nothing more: no
//             count, no position in a line, no rig detail and no offer of a notification (CUST-04,
//             CUST-02, D-20). Play stays the one action: pressing it asks again.
//   refused   the control plane answered and refused in its own words (the balance floor, or a
//             session the customer already has). That sentence is shown as the server wrote it, its
//             reference in mono after it, and a quiet top-up control sits under Play. The client
//             holds no balance threshold of its own: whether a customer may start is only ever the
//             server's answer (CUST-10, D-19).
//   offline   the control plane could not be reached. The deck's full offline sentence sits above
//             Play, the balance element in the header reads `last known`, and Play stays pressable.
//   error     any other server error is the error view, not a state of this screen.
//
// Exactly one control is bright: Play. The top-up control is a quiet (ghost) one. The signed-in identity
// and Sign out live on the profile (`ProfileScreen.qml`), reached from the header's menu (CUST-08).
//
// Long sentences wrap inside the column, and a reference code is never broken across lines.
Item {
    id: root

    /// The SeatHubClient facade. The only object this screen may talk to (D-35).
    property var client

    readonly property string status: client && client.homeStatus !== undefined
                                     ? String(client.homeStatus) : "ready"
    readonly property bool checking: root.status === "checking"
    readonly property bool refused: root.status === "refused"

    /// A session is live: Play becomes Resume session.
    readonly property bool live: client && client.liveSession === true ? true : false

    // The one line above Play for the states that have one. `copy.md` §Play flow gives the first two
    // and §Support & errors the third, each verbatim.
    readonly property string stateLine: {
        if (root.status === "checking")
            return qsTr("Checking availability…")
        if (root.status === "busy")
            return qsTr("None available right now.")
        if (root.status === "offline")
            return qsTr("Can't reach SevenHills right now. Showing the last known balance.")
        return ""
    }

    // The server's own words for a refusal, and its ADR-0008 reference.
    readonly property string refusalText: (client && client.failure && client.failure.error)
                                          ? String(client.failure.error) : ""
    readonly property string refusalReference: (client && client.reference)
                                               ? String(client.reference) : ""

    Column {
        anchors.centerIn: parent
        spacing: Metrics.s6
        width: Math.min(parent.width - Metrics.s16, 420)

        // The last session's end reason, from copy.md §Session end reasons (audit E10). One quiet
        // sentence that wraps, so it reflows instead of clipping at 960x640, and it clears the moment
        // the next session starts.
        Text {
            id: endReasonText
            objectName: "endReasonLine"
            width: parent.width
            visible: text.length > 0
            text: root.client && root.client.endReasonText ? String(root.client.endReasonText) : ""
            textFormat: Text.StyledText
            wrapMode: Text.Wrap
            color: Tokens.foregroundMutedDefault
            font.family: Tokens.fontSansDefault
            font.pixelSize: Metrics.fontBody
        }

        // The state line: named loading, the no-rig sentence, or the offline sentence.
        Text {
            id: stateText
            objectName: "stateLine"
            width: parent.width
            visible: text.length > 0
            text: root.stateLine
            wrapMode: Text.Wrap
            color: Tokens.foregroundDefault
            font.family: Tokens.fontSansDefault
            font.pixelSize: Metrics.fontBody
        }

        // A refusal: a glyph beside the colour (the fork ships no icon set), the server's sentence,
        // and its reference in mono. The sentence wraps; the reference does not.
        Column {
            id: refusal
            objectName: "refusal"
            width: parent.width
            spacing: Metrics.s1
            visible: root.refused && root.refusalText.length > 0

            Row {
                width: parent.width
                spacing: Metrics.s2

                Text {
                    id: refusalGlyph
                    text: "◆"
                    color: Tokens.destructiveDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontBody
                }

                Text {
                    objectName: "refusalText"
                    width: parent.width - refusalGlyph.width - parent.spacing
                    text: root.refusalText
                    wrapMode: Text.Wrap
                    color: Tokens.destructiveDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontBody
                }
            }

            Row {
                spacing: Metrics.s2
                visible: root.refusalReference.length > 0

                Text {
                    text: qsTr("Reference")
                    color: Tokens.foregroundMutedDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontSm
                }

                Text {
                    objectName: "refusalReference"
                    // ADR-0008: a reference is always mono, never translated, and never broken
                    // across lines: one that wrapped could not be read out or searched for.
                    text: root.refusalReference
                    wrapMode: Text.NoWrap
                    color: Tokens.foregroundDefault
                    font.family: Tokens.fontMonoDefault
                    font.pixelSize: Metrics.fontSm
                }
            }
        }

        // The one bright control, and the largest on screen: full column width, 64px tall.
        SeatHubButton {
            id: playButton
            objectName: "playButton"
            width: parent.width
            implicitHeight: Metrics.s16
            text: root.live ? qsTr("Resume session") : qsTr("Play")
            enabled: !root.checking
            busy: root.checking

            onClicked: root.client.start()
        }

        // Under a refusal, a quiet way to top up: it opens the website, through the facade.
        SeatHubButton {
            id: topUpButton
            objectName: "topUpButton"
            width: parent.width
            variant: "ghost"
            visible: root.refused
            text: qsTr("Top up")
            glyph: "↗"

            onClicked: root.client.openTopUp()
        }
    }
}
