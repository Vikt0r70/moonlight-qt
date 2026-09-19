import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// Sign in, path B: phone + one-time code (`docs/spec/copy.md` §Sign in, D-55 Phase 3 shell).
//
// Every state `docs/spec/screens.md` requires is here: the empty state (field labels and
// helper text before anything is typed), the partial state (normal typing with
// auto-advance), the loading state (BusyIndicator on the button, button keeps its width)
// and the error state (message in the destructive token, with the ADR-0008 reference in
// mono, shown for both the phone step and the code step - audit F2).
//
// The resend control (audit F6) is the deck's own sequence: `Resend in 0:24` while
// ADR-0022's 24-second window runs, then the `Send code` action again.
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
    property string errorReference: ""
    property int resendRemaining: 0

    // copy.md §5: countdowns are mono. ADR-0022 fixes the window at 24 seconds.
    readonly property string countdownText: "0:" + (root.resendRemaining < 10 ? "0" : "")
                                            + root.resendRemaining

    function sendCode() {
        errorText = ""
        errorReference = ""
        client.requestOtp(phoneField.text)
    }

    function verify() {
        errorText = ""
        errorReference = ""
        verifying = true
        client.verifyOtp(phoneField.text, otpField.code)
    }

    Timer {
        id: resendTimer
        interval: 1000
        repeat: true
        running: root.codeSent && root.resendRemaining > 0
        onTriggered: root.resendRemaining = Math.max(0, root.resendRemaining - 1)
    }

    Connections {
        target: root.client

        function onOtpRequested(phoneE164) {
            root.codeSent = true
            root.verifying = false
            root.resendRemaining = 24
            otpField.clear()
        }

        function onOtpRejected(message, reference) {
            root.verifying = false
            // The message is SeatHub copy; the reference is what support needs (ADR-0008).
            root.errorText = message
            root.errorReference = reference ? reference : ""
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
                color: Tokens.foregroundDefault
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
                color: Tokens.foregroundMutedDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontCaption
                visible: !root.codeSent
            }
        }

        // --- Error, for both steps (audit F2) ---
        // Outside both columns on purpose: a rejected phone number has to be visible even
        // though the code column is still hidden.
        Column {
            width: parent.width
            spacing: Metrics.s2
            visible: root.errorText.length > 0

            Text {
                width: parent.width
                text: root.errorText
                wrapMode: Text.Wrap
                color: Tokens.destructiveDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontSm
            }

            Row {
                spacing: Metrics.s2
                visible: root.errorReference.length > 0

                Text {
                    text: qsTr("Reference")
                    color: Tokens.foregroundMutedDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontSm
                }

                Text {
                    // ADR-0008: the reference code is always monospace, never translated.
                    text: root.errorReference
                    color: Tokens.foregroundDefault
                    font.family: Tokens.fontMonoDefault
                    font.pixelSize: Metrics.fontSm
                }
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

            SeatHubOTPField {
                id: otpField
                entryEnabled: !root.verifying
                onCompleted: root.verify()
            }

            // Resend (audit F6): the countdown while the window runs, the action after it.
            Row {
                width: parent.width
                spacing: Metrics.s3

                Text {
                    visible: root.resendRemaining > 0
                    text: qsTr("Resend in %1").arg(root.countdownText)
                    color: Tokens.foregroundMutedDefault
                    font.family: Tokens.fontMonoDefault
                    font.pixelSize: Metrics.fontSm
                    anchors.verticalCenter: parent.verticalCenter
                }

                SeatHubButton {
                    id: resendButton
                    visible: root.resendRemaining <= 0
                    variant: "ghost"
                    text: qsTr("Send code")
                    enabled: !root.verifying

                    onClicked: root.sendCode()
                }
            }
        }

        // --- Primary action ---
        // Loading state: BusyIndicator beside the label, and the button keeps its width
        // so the layout does not jump.
        SeatHubButton {
            id: primaryButton
            width: parent.width
            enabled: !root.verifying
            busy: root.verifying
            text: root.codeSent ? qsTr("Verify and continue") : qsTr("Send code")

            onClicked: root.codeSent ? root.verify() : root.sendCode()
        }
    }
}
