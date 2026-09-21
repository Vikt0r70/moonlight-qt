import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// The three-stage connecting stepper (`docs/spec/screens.md` §24, `05-UI-SPEC` "Connecting"; CUST-12).
//
// Each stage is a real transition and none is drawn on a timer or a guess. The facade owns which
// stage has been reached (`SeatHubClient.connectStage`): the first two come from the session's own
// state as the control plane reports it, and from the engine's own stages inside the second; the
// third is the stream actually starting. This file only draws that number, so the stepper cannot
// invent a stage the facade did not reach, and it never moves back because the facade never does.
//
//   1  Preparing the rig      waking the PC and switching it into rental mode
//   2  Preparing the stream   starting the streaming service and pairing this client
//   3  Streaming              live; the window then hides
//
// Before the first answer about the session has come back nothing is marked done: the first stage is
// where a session that exists begins, so it is the one drawn as active.
//
// A finished stage shows a check, the active stage a breathing warn dot (the screen's one live signal,
// ui.md §6) and a pending stage a hollow ring. When connecting stops, `failed` turns the stage that
// was active destructive with a mark of its own, so colour is never the only signal. The fork ships no
// icon set, so the marks are geometric characters until the spec's icons land.
//
// Motion (audit F7): the colour changes in `--dur-page` (420ms), and the active dot alone breathes on
// ui.md §6's 2s ease-in-out loop - one pulsing element on the page. Nothing animates a layout
// property (ui.md §12): colour and opacity only.
Item {
    id: root

    /// The SeatHubClient facade. The only object this screen may talk to (D-35).
    property var client: null

    /// True when connecting stopped: the stage that was active is drawn failed. The reason and the
    /// two ways out are the connecting screen's, not the stepper's.
    property bool failed: false

    readonly property var stageLines: [
        qsTr("Preparing the rig"),
        qsTr("Preparing the stream"),
        qsTr("Streaming")
    ]

    readonly property var stageSentences: [
        qsTr("Waking the PC and switching it into rental mode. About 90 seconds."),
        qsTr("Starting Sunshine and pairing your client. Usually under a minute."),
        ""
    ]

    // The highest stage the facade has reached, 0 while nothing has been read yet.
    readonly property int reached: root.client && root.client.connectStage !== undefined
                                   ? Number(root.client.connectStage) : 0

    // The stage that is active - or, when connecting stopped, the one that was. Never below the first.
    readonly property int activeIndex: Math.min(Math.max(root.reached, 1), root.stageLines.length) - 1

    width: parent ? parent.width : 0
    implicitHeight: column.implicitHeight

    // One polite announcement of where the customer is, in the deck's own words.
    Accessible.role: Accessible.StaticText
    Accessible.name: root.stageLines[root.activeIndex]

    Column {
        id: column
        width: parent.width
        spacing: Metrics.s4

        Repeater {
            model: root.stageLines.length

            Row {
                id: stageRow
                objectName: "stage" + index
                required property int index

                readonly property bool isFailed: root.failed && index === root.activeIndex
                readonly property bool isActive: !root.failed && index === root.activeIndex
                readonly property bool isDone: index < root.activeIndex
                // What the stage looks like, for a test and for the accessible name to read.
                readonly property string look: isFailed ? "failed"
                                               : (isActive ? "active" : (isDone ? "done" : "pending"))

                width: column.width
                spacing: Metrics.s3

                // The indicator column is as wide as the widest mark so the names line up.
                Item {
                    width: Metrics.s5
                    height: Metrics.s5
                    anchors.verticalCenter: parent.verticalCenter

                    // Finished: a check.
                    Text {
                        objectName: "doneMark"
                        anchors.centerIn: parent
                        visible: stageRow.isDone
                        text: "✓"
                        color: Tokens.foregroundDefault
                        font.family: Tokens.fontSansDefault
                        font.pixelSize: Metrics.fontBody
                    }

                    // Stopped here: a mark that is not a colour alone.
                    Text {
                        objectName: "failedMark"
                        anchors.centerIn: parent
                        visible: stageRow.isFailed
                        text: "◆"
                        color: Tokens.destructiveDefault
                        font.family: Tokens.fontSansDefault
                        font.pixelSize: Metrics.fontBody
                    }

                    // Active: the warn dot, breathing. Pending: a hollow ring.
                    Rectangle {
                        objectName: "dot"
                        anchors.centerIn: parent
                        visible: stageRow.isActive || (!stageRow.isDone && !stageRow.isFailed)
                        width: 10
                        height: 10
                        radius: 5
                        color: stageRow.isActive ? Tokens.warnDefault : "transparent"
                        border.width: stageRow.isActive ? 0 : 1
                        border.color: Tokens.borderStrongDefault

                        Behavior on color {
                            ColorAnimation { duration: Metrics.motionPage }
                        }

                        // ui.md §6: a 2s ease-in-out loop, on the live element only.
                        SequentialAnimation on opacity {
                            running: stageRow.isActive
                            loops: Animation.Infinite
                            NumberAnimation {
                                to: 0.35
                                duration: 1000
                                easing.type: Easing.InOutSine
                            }
                            NumberAnimation {
                                to: 1.0
                                duration: 1000
                                easing.type: Easing.InOutSine
                            }
                        }
                    }
                }

                Column {
                    width: parent.width - Metrics.s5 - stageRow.spacing
                    spacing: Metrics.s1

                    Text {
                        objectName: "stageName"
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: root.stageLines[stageRow.index]
                        color: stageRow.isFailed ? Tokens.destructiveDefault
                               : (stageRow.isActive || stageRow.isDone ? Tokens.foregroundDefault
                                                                       : Tokens.foregroundMutedDefault)
                        font.family: Tokens.fontSansDefault
                        font.pixelSize: Metrics.fontBody
                        font.weight: Font.DemiBold

                        Behavior on color {
                            ColorAnimation { duration: Metrics.motionPage }
                        }
                    }

                    Text {
                        objectName: "stageSentence"
                        width: parent.width
                        wrapMode: Text.Wrap
                        visible: (stageRow.isActive || stageRow.isFailed) && text.length > 0
                        text: root.stageSentences[stageRow.index]
                        color: Tokens.foregroundMutedDefault
                        font.family: Tokens.fontSansDefault
                        font.pixelSize: Metrics.fontSm
                        lineHeight: 1.4
                    }
                }
            }
        }
    }
}
