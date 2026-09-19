import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// The five-stage preparing stepper (screens.md §24, 03-UI-SPEC.md E2 row, audit F8).
//
// ui.md §6 rules out a spinner that resets for this screen, so the connect phase is shown as
// the deck's five stage lines with the active one carrying its sentence. The four states the
// copy deck names are `Waiting for a free rig`, `Preparing the rig`, `Preparing the stream`,
// `Ready`, plus the live `Streaming` stage the UI-SPEC asks to be shown.
//
// Motion (audit F7): the active stage's colour changes in `--dur-page` (420ms, ui.md §6's
// "420ms fill"), and the active dot alone breathes on ui.md §6's 2s ease-in-out loop - one
// pulsing element on the page, which is the spec's "one live signal" rule. Nothing animates a
// layout property (ui.md §12): colour and opacity only.
//
// The active stage is read from `SeatHubClient.stageText`, which is set from the same copy deck,
// so this file never invents a stage name and the two cannot drift apart.
Item {
    id: root

    /// The SeatHubClient facade. The only object this screen may talk to (D-35).
    property var client: null

    readonly property var stageLines: [
        qsTr("Waiting for a free rig"),
        qsTr("Preparing the rig"),
        qsTr("Preparing the stream"),
        qsTr("Ready"),
        qsTr("Streaming")
    ]

    readonly property var stageSentences: [
        qsTr("You're next. This usually takes under a minute."),
        qsTr("Waking the PC and switching it into rental mode. About 90 seconds."),
        qsTr("Starting Sunshine and pairing your client. Usually under a minute."),
        qsTr("Open SeatHub and press Play."),
        ""
    ]

    // 0 when nothing has been reported yet: the first stage is the honest default, because
    // that is where a Play request starts.
    readonly property int activeIndex: {
        var reported = root.client ? String(root.client.stageText) : ""
        for (var i = 0; i < root.stageLines.length; ++i) {
            if (root.stageLines[i] === reported)
                return i
        }
        return 0
    }

    width: parent ? parent.width : 0
    implicitHeight: column.implicitHeight

    Column {
        id: column
        width: parent.width
        spacing: Metrics.s4

        Repeater {
            model: root.stageLines.length

            Row {
                id: stageRow
                required property int index

                readonly property bool isActive: index === root.activeIndex
                readonly property bool isDone: index < root.activeIndex

                width: column.width
                spacing: Metrics.s3

                Rectangle {
                    width: Metrics.s5
                    height: Metrics.s5
                    radius: width / 2
                    anchors.verticalCenter: parent.verticalCenter
                    color: stageRow.isDone ? Tokens.primaryDefault
                                           : (stageRow.isActive ? Tokens.warnDefault
                                                                : Tokens.surface3Default)

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

                Column {
                    width: parent.width - Metrics.s5 - stageRow.spacing
                    spacing: Metrics.s1

                    Text {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: root.stageLines[stageRow.index]
                        color: stageRow.isActive || stageRow.isDone ? Tokens.foregroundDefault
                                                                    : Tokens.foregroundMutedDefault
                        font.family: Tokens.fontSansDefault
                        font.pixelSize: Metrics.fontBody
                    }

                    Text {
                        width: parent.width
                        wrapMode: Text.Wrap
                        visible: stageRow.isActive && text.length > 0
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
