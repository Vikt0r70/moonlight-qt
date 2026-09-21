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

    // screens.md §26 asks for 1100x720 with 960x640 as the supported minimum (audit F17).
    width: 1100
    height: 720
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

        // D-41: the feed is checked on launch. `SeatHubClient` refuses the check while a stream
        // is running (Pitfall 8), and re-checks when one ends.
        seatHub.updates.checkForUpdates()

        // CUST-08: the stored sign-in is read and confirmed once, here. Until it resolves the
        // client is in the "restoring" state and shows the splash - never the sign-in form.
        seatHub.restoreSession()
    }

    function componentForState(state) {
        switch (state) {
        // Routed explicitly: an unrecognised state falls through to the sign-in form below, which is
        // exactly the flash the restore splash exists to prevent.
        case "restoring":  return restoreComponent
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

    // The forced-update modal lives outside the Loader so a view change can never take it away
    // mid-update (D-41). It renders nothing while a stream is running (Pitfall 8) and swallows
    // every event aimed at the page behind it while it is up.
    ForcedUpdateModal {
        id: forcedUpdate
        updates: seatHub.updates
    }

    Connections {
        target: seatHub.updates

        // D-43/D-42: the installer is unsigned, so Windows raises its own SmartScreen warning
        // and the per-machine install raises a UAC prompt. SeatHub's part is to stop, so the
        // installer can replace the binary it is running from.
        function onInstallRequested() {
            Qt.quit()
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
        id: restoreComponent

        RestoreSplash {
        }
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
                spacing: Metrics.s6
                width: Math.min(parent.width - Metrics.s16, 420)

                // The connect phase is the deck's five stages, not a spinner that resets
                // (ui.md §6, audit F8).
                SeatHubStepper {
                    id: stepper
                    width: parent.width
                    client: seatHub
                }

                // A real control, not a Text + MouseArea (audit F3).
                SeatHubButton {
                    id: cancelButton
                    width: parent.width
                    variant: "ghost"
                    text: qsTr("Cancel")

                    onClicked: seatHub.interrupt()
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
                width: Math.min(parent.width - Metrics.s16, 420)

                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("Streaming")
                    color: Tokens.successDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontH2
                }

                SeatHubButton {
                    id: endSessionButton
                    width: parent.width
                    variant: "destructive"
                    text: qsTr("End session")

                    onClicked: endSessionConfirm.open()
                }
            }

            // copy.md §In session "End": a destructive confirm that names the object and the
            // consequence (copy.md §5, ui.md §7 Confirm/alert dialog). The deck's leading
            // minute count is omitted because no client surface knows the session's remaining
            // minutes yet - recorded in 03-UI-REVIEW-FIXES.md rather than filled with an
            // invented number.
            Popup {
                id: endSessionConfirm
                anchors.centerIn: parent
                width: Math.min(parent.width - Metrics.s16, 420)
                modal: true
                focus: true
                padding: Metrics.s8
                closePolicy: Popup.CloseOnEscape

                background: Rectangle {
                    radius: Metrics.radiusLg
                    color: Tokens.surface1Default
                    border.color: Tokens.borderDefault
                    border.width: 1
                }

                contentItem: Column {
                    spacing: Metrics.s4

                    Text {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: qsTr("End session?")
                        color: Tokens.foregroundDefault
                        font.family: Tokens.fontDisplayDefault
                        font.pixelSize: Metrics.fontH3
                        font.weight: Font.DemiBold
                    }

                    Text {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: qsTr("Unused minutes stay in your account.")
                        color: Tokens.foregroundMutedDefault
                        font.family: Tokens.fontSansDefault
                        font.pixelSize: Metrics.fontSm
                    }

                    SeatHubButton {
                        id: confirmEndButton
                        width: parent.width
                        variant: "destructive"
                        text: qsTr("End session")

                        onClicked: {
                            endSessionConfirm.close()
                            seatHub.interrupt()
                        }
                    }

                    SeatHubButton {
                        id: keepPlayingButton
                        width: parent.width
                        variant: "ghost"
                        text: qsTr("Keep playing")

                        onClicked: endSessionConfirm.close()
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
