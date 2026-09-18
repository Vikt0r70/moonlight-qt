import QtQuick
import QtQuick.Controls
import QtQuick.Window
import SeatHub 1.0
import SeatHub.Tokens 1.0

// SeatHub's QML entry point. Replaces upstream moonlight-qt's launcher window entirely.
//
// What is deliberately absent, and must stay absent:
//   * The stock PC/app browser (`PcView.qml`, `AppView.qml`) and its "Moonlight" toolbar.
//   * `ErrorMessageDialog.qml`. No error is ever shown in the engine's own words - every
//     failure goes through SeatHubClient's mapped copy plus an ADR-0008 reference (D-51).
//   * The `initialView` CLI route. `app/main.cpp` still constructs it for the CLI
//     commands, but nothing here consumes it, so no Moonlight-worded segue window can
//     appear during connect, stream or teardown.
//
// The window is the thing the D-01 visibility sequence hides and restores; `SessionSegue`
// owns that, using this same window.
ApplicationWindow {
    id: window

    width: 960
    height: 640
    minimumWidth: 960
    minimumHeight: 640
    visible: true
    title: qsTr("SeatHub")
    color: Tokens.backgroundDefault

    // The single object the QML layer talks to (D-35).
    SeatHubClient {
        id: seatHub
    }

    Component.onCompleted: {
        seatHub.setHostWindow(window)
    }

    function componentForState(state) {
        switch (state) {
        case "signed_out": return signInComponent
        // Settings is a view inside the home state, not a state of its own: a session can end
        // while the page is open, and the page must still be the right view when it does.
        case "home":       return seatHub.inSettings ? settingsComponent : homeComponent
        case "connecting": return connectingComponent
        case "streaming":  return streamingComponent
        case "error":      return errorComponent
        }
        return signInComponent
    }

    Loader {
        id: viewLoader
        anchors.fill: parent
        sourceComponent: window.componentForState(seatHub.appState)

        onLoaded: if (item && item.forceActiveFocus) item.forceActiveFocus()
    }

    Connections {
        target: seatHub

        function onAppStateChanged() {
            viewLoader.sourceComponent = window.componentForState(seatHub.appState)
        }

        function onInSettingsChanged() {
            viewLoader.sourceComponent = window.componentForState(seatHub.appState)
        }
    }

    // The D-01 visibility sequence and the lifecycle handlers. Instantiated outside the
    // Loader so it survives every view change - it must outlive the view it hides.
    SessionSegue {
        id: segue
        window: window
        lifecycle: seatHub.session
    }

    // D-02: end the active stream immediately. While the real stream window has focus the
    // engine's own SdlInputHandler matches this combo; this shortcut covers the case where
    // the Qt window still has it (the connecting phase, and the tracer's stubbed run).
    Shortcut {
        sequence: "Ctrl+Alt+Shift+Q"
        context: Qt.ApplicationShortcut
        onActivated: seatHub.interrupt()
    }

    Component {
        id: signInComponent

        SignInScreen {
            client: seatHub
        }
    }

    Component {
        id: homeComponent

        HomeScreen {
            client: seatHub
        }
    }

    Component {
        id: settingsComponent

        SettingsPage {
            client: seatHub
        }
    }

    // Connecting: visible only until the engine reports connectionStarted, at which point
    // SessionSegue hides this whole window (D-01).
    Component {
        id: connectingComponent

        Item {
            Column {
                anchors.centerIn: parent
                spacing: Metrics.s5
                width: Math.min(parent.width - Metrics.s16, 420)

                BusyIndicator {
                    anchors.horizontalCenter: parent.horizontalCenter
                    running: true
                    width: Metrics.s10
                    height: Metrics.s10
                }

                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: seatHub.stageText
                    wrapMode: Text.Wrap
                    color: Tokens.foregroundDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontH3
                }

                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("Cancel")
                    color: Tokens.foregroundSubtleDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontCaption

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: seatHub.interrupt()
                    }
                }
            }
        }
    }

    // Streaming. Normally unreachable - the window is hidden before the stream window
    // exists (D-01). It exists so the state machine has a defined view rather than a blank
    // window if the hide ever fails.
    Component {
        id: streamingComponent

        Item {
            Column {
                anchors.centerIn: parent
                spacing: Metrics.s4

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("Streaming")
                    color: Tokens.successDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontH2
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("End session")
                    color: Tokens.foregroundMutedDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontBody

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: seatHub.interrupt()
                    }
                }
            }
        }
    }

    Component {
        id: errorComponent

        ErrorScreen {
            client: seatHub
        }
    }
}
