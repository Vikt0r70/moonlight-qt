import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// Sign in, path B: phone + one-time code (`docs/spec/copy.md` §Sign in, D-55 Phase 3 shell).
//
// Every state `docs/spec/screens.md` requires is here: the empty state (field labels and
// helper text before anything is typed), the partial state (normal typing with
// auto-advance), the loading state (BusyIndicator on the button, button keeps its width)
// and the error state (message above the field, in the destructive token).
//
// Plan 03-02 stubs the control plane: `client.requestOtp` / `client.verifyOtp` accept the
// input locally. Plan 03-03 replaces those bodies with the real calls; nothing in this
// file changes when it does.
Item {
    id: root

    /// The SeatHubClient facade. The only object this screen may talk to (D-35).
    property var client

    /// Emitted when the code is accepted and the view should move on.
    signal signedIn()

    // Local view state, not app state - it never leaves this screen.
    property bool codeSent: false
    property bool verifying: false
    property string errorText: ""

    function sendCode() {
        errorText = ""
        client.requestOtp(phoneField.text)
    }

    function verify() {
        errorText = ""
        verifying = true
        client.verifyOtp(phoneField.text, otpField.code)
    }

    Connections {
        target: root.client

        function onOtpRequested(phoneE164) {
            root.codeSent = true
            root.verifying = false
            otpField.clear()
        }

        function onOtpRejected(message, reference) {
            root.verifying = false
            // The message is SeatHub copy; the reference is what support needs (ADR-0008).
            root.errorText = reference ? message + " Reference " + reference + "." : message
            otpField.clear()
        }

        function onOtpAccepted() {
            root.verifying = false
            root.signedIn()
        }
    }

    Column {
        anchors.centerIn: parent
        spacing: Metrics.s6
        width: Math.min(parent.width - Metrics.s16, 420)

        Text {
            text: qsTr("SeatHub")
            color: Tokens.foregroundDefault
            font.family: Tokens.fontDisplayDefault
            font.pixelSize: Metrics.fontH1
            font.weight: Font.DemiBold
        }

        // --- Phone number ---
        Column {
            width: parent.width
            spacing: Metrics.s2

            Text {
                text: qsTr("Enter your phone number")
                color: Tokens.foregroundMutedDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontLabel
                font.letterSpacing: 0.08 * Metrics.fontLabel
            }

            TextField {
                id: phoneField
                width: parent.width
                height: Metrics.touchTarget
                enabled: !root.verifying
                placeholderText: qsTr("+962 7 0000 0000")
                color: Tokens.foregroundDefault
                placeholderTextColor: Tokens.foregroundSubtleDefault
                font.family: Tokens.fontMonoDefault
                font.pixelSize: Metrics.fontBody
                background: Rectangle {
                    radius: Metrics.radiusSm
                    color: Tokens.surface1Default
                    border.width: 1
                    border.color: phoneField.activeFocus ? Tokens.focusDefault : Tokens.borderDefault
                }
                onAccepted: if (!root.codeSent) root.sendCode()
            }

            Text {
                // Empty-state helper, verbatim from docs/spec/copy.md §Sign in.
                text: qsTr("We'll send a code on WhatsApp")
                color: Tokens.foregroundSubtleDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontCaption
                visible: !root.codeSent
            }
        }

        // --- One-time code (only after the phone is submitted) ---
        Column {
            width: parent.width
            spacing: Metrics.s2
            visible: root.codeSent

            Text {
                text: qsTr("Enter the 6-digit code")
                color: Tokens.foregroundMutedDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontLabel
                font.letterSpacing: 0.08 * Metrics.fontLabel
            }

            // Error state: above the field, in the destructive token.
            Text {
                width: parent.width
                text: root.errorText
                visible: root.errorText.length > 0
                wrapMode: Text.Wrap
                color: Tokens.destructiveDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontSm
            }

            SeatHubOTPField {
                id: otpField
                entryEnabled: !root.verifying
                onCompleted: root.verify()
            }
        }

        // --- Primary action ---
        // Loading state: BusyIndicator beside the label, and the button keeps its width
        // so the layout does not jump.
        Button {
            id: primaryButton
            width: parent.width
            height: Metrics.touchTarget
            enabled: !root.verifying
            text: root.codeSent ? qsTr("Verify and continue") : qsTr("Send code")

            contentItem: Row {
                spacing: Metrics.s3
                anchors.centerIn: parent

                BusyIndicator {
                    visible: root.verifying
                    running: root.verifying
                    width: visible ? Metrics.s5 : 0
                    height: Metrics.s5
                    anchors.verticalCenter: parent.verticalCenter
                }

                Text {
                    text: primaryButton.text
                    color: Tokens.primaryForegroundDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontBody
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            background: Rectangle {
                radius: Metrics.radiusSm
                color: primaryButton.enabled ? Tokens.primaryDefault : Tokens.surface3Default
            }

            onClicked: root.codeSent ? root.verify() : root.sendCode()
        }
    }
}
