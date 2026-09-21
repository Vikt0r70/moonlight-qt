import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// Sign in (`docs/spec/screens.md` §22, Phase 5 D-01 to D-06).
//
// ONE field, "Email or phone", and then the step its content calls for:
//
//   identifier  the field, its helper, `Continue`, and two links that open the website
//   code        (a phone number) the six-cell one-time code, resend, `Verify and continue`, `Back`
//   password    (an email) the password with a reveal control, `Sign in`, `Forgot password?`, `Back`
//
// Signup and password reset are the WEBSITE's, reached by a link (D-03, D-04): this client has no
// screen for either, and no link carries a credential. The client never builds an address itself; it
// asks the facade to open one (`openWebsite`), so the addresses live in `web_origin.h` and nowhere
// in QML.
//
// Every state `docs/spec/screens.md` requires is here: empty (label, helper, `Continue` enabled),
// loading (the pressed control keeps its width and is busy), error (the message in the destructive
// token above the field for a field-level mistake and above the control for a refusal, with the
// ADR-0008 reference in mono) and offline (the deck's offline sentence, which the facade supplies as
// the refusal). A field-level mistake is named before anything is sent.
//
// The typed password lives in the field for the length of one attempt and is cleared after it. It is
// never stored, never logged and never leaves this screen except as the argument of the sign-in call.
//
// The resend control (audit F6) is the deck's own sequence: `Resend in 0:24` while ADR-0022's
// 24-second window runs, then the `Send code` action again.
Item {
    id: root

    /// The SeatHubClient facade. The only object this screen may talk to (D-35).
    property var client

    /// Emitted when a sign-in succeeds and the view should move on.
    signal signedIn()

    // Local view state, not app state - it never leaves this screen.
    /// "identifier" | "code" | "password"
    property string step: "identifier"
    readonly property bool codeSent: root.step === "code"

    property bool requesting: false
    property bool verifying: false
    property bool signingIn: false
    readonly property bool working: root.requesting || root.verifying || root.signingIn

    /// A field-level mistake, named before anything is sent (shown above the field).
    property string fieldError: ""
    /// A refusal that names no field (shown above the control), with its ADR-0008 reference.
    property string errorText: ""
    property string errorReference: ""

    property bool showPassword: false
    property int resendRemaining: 0
    /// The E.164 number the code was asked for, so verify and resend use exactly it.
    property string phoneE164: ""

    // copy.md §5: countdowns are mono. ADR-0022 fixes the window at 24 seconds.
    readonly property string countdownText: "0:" + (root.resendRemaining < 10 ? "0" : "")
                                            + root.resendRemaining

    function clearErrors() {
        fieldError = ""
        errorText = ""
        errorReference = ""
    }

    // Step one: work out what was typed and go where it says.
    function continueFromIdentifier() {
        clearErrors()
        var typed = identifierField.typed
        if (typed === "") {
            fieldError = qsTr("Enter your email or phone number.")
            return
        }
        if (identifierField.mode === "email") {
            // An email goes to the password step without spending a request; the shape is checked
            // first, the way the website checks it.
            if (!identifierField.looksLikeEmail) {
                fieldError = qsTr("Enter your email or phone number.")
                return
            }
            root.step = "password"
            return
        }
        // A phone number: build E.164 from the chosen country and what was typed, and refuse a
        // mistake here instead of spending a round trip on it.
        var e164 = client.toE164(typed, identifierField.selectedDial)
        if (e164 === "") {
            fieldError = qsTr("That doesn't look like a phone number.")
            return
        }
        root.phoneE164 = e164
        sendCode()
    }

    function sendCode() {
        errorText = ""
        errorReference = ""
        requesting = true
        client.requestOtp(root.phoneE164)
    }

    function verify() {
        errorText = ""
        errorReference = ""
        verifying = true
        client.verifyOtp(root.phoneE164, otpField.code)
    }

    function signInWithPassword() {
        clearErrors()
        if (passwordField.text === "") {
            fieldError = qsTr("Enter your password.")
            return
        }
        signingIn = true
        client.signInWithPassword(identifierField.typed, passwordField.text)
    }

    // Back keeps what was typed in the field and drops everything else.
    function back() {
        clearErrors()
        passwordField.text = ""
        showPassword = false
        resendRemaining = 0
        requesting = false
        verifying = false
        signingIn = false
        root.step = "identifier"
    }

    function primaryAction() {
        if (root.step === "code")
            root.verify()
        else if (root.step === "password")
            root.signInWithPassword()
        else
            root.continueFromIdentifier()
    }

    Component.onCompleted: identifierField.focusInput()

    onStepChanged: {
        if (root.step === "identifier")
            identifierField.focusInput()
        else if (root.step === "password")
            passwordField.forceActiveFocus()
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
            root.requesting = false
            root.verifying = false
            root.phoneE164 = phoneE164
            root.step = "code"
            root.resendRemaining = 24
            otpField.clear()
        }

        function onOtpRejected(message, reference) {
            root.requesting = false
            root.verifying = false
            // The message is SeatHub copy; the reference is what support needs (ADR-0008).
            root.errorText = message
            root.errorReference = reference ? reference : ""
            if (root.step === "code")
                otpField.clear()
        }

        function onOtpAccepted() {
            root.verifying = false
            root.signedIn()
        }

        function onPasswordSignInRejected(message, reference) {
            root.signingIn = false
            root.errorText = message
            root.errorReference = reference ? reference : ""
            // The password is held for one attempt only.
            passwordField.text = ""
            if (root.step === "password")
                passwordField.forceActiveFocus()
        }

        function onPasswordSignInAccepted() {
            root.signingIn = false
            passwordField.text = ""
            root.signedIn()
        }
    }

    // One message: a glyph beside the colour (the fork ships no icon set), the sentence, and the
    // reference in mono when the server gave one.
    component ErrorLine: Column {
        id: line
        property string message: ""
        property string reference: ""
        objectName: "errorLine"
        width: parent ? parent.width : 0
        spacing: Metrics.s1
        visible: line.message.length > 0

        Row {
            width: parent.width
            spacing: Metrics.s2

            Text {
                id: glyph
                text: "\u25c6"
                color: Tokens.destructiveDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontSm
            }

            Text {
                width: parent.width - glyph.width - parent.spacing
                text: line.message
                wrapMode: Text.Wrap
                color: Tokens.destructiveDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontSm
            }
        }

        Row {
            spacing: Metrics.s2
            visible: line.reference.length > 0

            Text {
                text: qsTr("Reference")
                color: Tokens.foregroundMutedDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontSm
            }

            Text {
                // ADR-0008: the reference code is always monospace, never translated.
                text: line.reference
                color: Tokens.foregroundDefault
                font.family: Tokens.fontMonoDefault
                font.pixelSize: Metrics.fontSm
            }
        }
    }

    // A field's label, in the deck's small-caps-ish label style.
    component FieldLabel: Text {
        color: Tokens.foregroundMutedDefault
        font.family: Tokens.fontSansDefault
        font.pixelSize: Metrics.fontLabel
        font.letterSpacing: 0.08 * Metrics.fontLabel
    }

    // The centered card (screens.md §22, 05-UI-SPEC): 448px at most, `s8` padding, `radiusLg`.
    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.min(parent.width - Metrics.s16, 448)
        height: content.implicitHeight + Metrics.s8 * 2
        radius: Metrics.radiusLg
        color: Tokens.surface1Default
        border.width: 1
        border.color: Tokens.borderDefault

        Column {
            id: content
            x: Metrics.s8
            y: Metrics.s8
            width: parent.width - Metrics.s8 * 2
            spacing: Metrics.s6

            Text {
                text: qsTr("Sign in")
                color: Tokens.foregroundDefault
                font.family: Tokens.fontDisplayDefault
                font.pixelSize: Metrics.fontH1
                font.weight: Font.DemiBold
            }

            // --- Step 1: the one field ---
            Column {
                objectName: "identifierStep"
                width: parent.width
                spacing: Metrics.s2
                visible: root.step === "identifier"

                FieldLabel { text: qsTr("Email or phone") }

                ErrorLine {
                    objectName: "fieldErrorLine"
                    message: root.step === "identifier" ? root.fieldError : ""
                }

                SeatHubIdentifierField {
                    id: identifierField
                    objectName: "identifierField"
                    width: parent.width
                    enabled: !root.working
                    countries: root.client && root.client.countries ? root.client.countries : []
                    defaultCountryCode: root.client && root.client.defaultCountryCode
                                        ? root.client.defaultCountryCode : "JO"
                    animate: root.client && root.client.animationEffects !== undefined
                             ? root.client.animationEffects : true
                    errorText: root.step === "identifier" ? root.fieldError : ""
                    onAccepted: root.continueFromIdentifier()
                    // What was wrong is no longer wrong once the customer edits the field.
                    onTextChanged: if (root.fieldError.length > 0) root.fieldError = ""
                }

                Text {
                    // Empty-state helper, verbatim from docs/spec/copy.md §Sign in.
                    width: parent.width
                    text: qsTr("Enter an email address, or pick your country and enter your phone number.")
                    wrapMode: Text.Wrap
                    color: Tokens.foregroundMutedDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontCaption
                }
            }

            // --- Step 2a: the one-time code (a phone number) ---
            Column {
                objectName: "codeStep"
                width: parent.width
                spacing: Metrics.s2
                visible: root.step === "code"

                FieldLabel { text: qsTr("Enter the 6-digit code") }

                SeatHubOTPField {
                    id: otpField
                    entryEnabled: !root.working
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
                        enabled: !root.working

                        onClicked: root.sendCode()
                    }
                }
            }

            // --- Step 2b: the password (an email) ---
            Column {
                objectName: "passwordStep"
                width: parent.width
                spacing: Metrics.s2
                visible: root.step === "password"

                FieldLabel { text: qsTr("Password") }

                ErrorLine {
                    objectName: "passwordErrorLine"
                    message: root.step === "password" ? root.fieldError : ""
                }

                TextField {
                    id: passwordField
                    objectName: "passwordField"
                    width: parent.width
                    height: Metrics.touchTarget
                    enabled: !root.working
                    echoMode: root.showPassword ? TextInput.Normal : TextInput.Password
                    // No prediction, no history: this is a secret.
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                    color: Tokens.foregroundDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontBody
                    Accessible.name: qsTr("Password")
                    Accessible.description: root.fieldError
                    background: Rectangle {
                        radius: Metrics.radiusSm
                        color: Tokens.surface2Default
                        border.width: passwordField.activeFocus ? 2 : 1
                        border.color: root.fieldError.length > 0 && root.step === "password"
                                      ? Tokens.destructiveDefault
                                      : (passwordField.activeFocus ? Tokens.focusDefault
                                                                   : Tokens.borderDefault)
                    }
                    onAccepted: root.signInWithPassword()
                    onTextChanged: if (root.fieldError.length > 0) root.fieldError = ""
                }

                SeatHubButton {
                    objectName: "revealPassword"
                    variant: "ghost"
                    text: root.showPassword ? qsTr("Hide password") : qsTr("Show password")
                    onClicked: root.showPassword = !root.showPassword
                }
            }

            // --- Error, for the steps (audit F2) ---
            // A refusal that names no field: outside the step columns on purpose, so a rejected
            // phone number is visible even though the code column is still hidden.
            ErrorLine {
                objectName: "refusalLine"
                message: root.errorText
                reference: root.errorReference
            }

            // --- Primary action ---
            // Loading state: the pressed control keeps its width and says it is busy.
            SeatHubButton {
                id: primaryButton
                objectName: "primaryAction"
                width: parent.width
                enabled: !root.working
                busy: root.working
                text: root.step === "code" ? qsTr("Verify and continue")
                                           : (root.step === "password" ? qsTr("Sign in")
                                                                       : qsTr("Continue"))

                onClicked: root.primaryAction()
            }

            // --- The links and Back ---
            Column {
                width: parent.width
                spacing: Metrics.s1

                // Signup and password reset are the website's (D-03, D-04). The address is the
                // facade's to build; nothing is passed along with it.
                SeatHubButton {
                    objectName: "createAccountLink"
                    visible: root.step === "identifier"
                    variant: "ghost"
                    text: qsTr("New here? Create an account")
                    glyph: "\u2197"
                    onClicked: root.client.openWebsite("signup")
                }

                SeatHubButton {
                    objectName: "forgotPasswordLink"
                    visible: root.step === "identifier" || root.step === "password"
                    variant: "ghost"
                    text: qsTr("Forgot password?")
                    glyph: "\u2197"
                    onClicked: root.client.openWebsite("reset")
                }

                SeatHubButton {
                    objectName: "backButton"
                    visible: root.step !== "identifier"
                    variant: "ghost"
                    text: qsTr("Back")
                    enabled: !root.working
                    onClicked: root.back()
                }
            }
        }
    }
}
