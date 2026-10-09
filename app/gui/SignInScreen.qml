import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// Sign in (`docs/spec/screens.md` §22, C-SIGNIN, A-ID; Phase 5 D-01 to D-06).
//
// ONE screen: one identifier field ("Email or phone"), one `Password` field with its reveal control,
// and `Sign in`. A phone number and an email sign in through the same field and the same password
// (`POST /api/auth/login`); there is no code step and no separate email/password step. The country
// picker shows only while the field holds phone-shaped text (digits, spaces, `-`, `(`, `)`, `.` and one
// optional leading `+`, at least three digits - `SeatHubIdentifierField.classify`), its default is
// Jordan (`JO +962`) and any other country stays selectable.
//
// What goes out is the raw typed text and the picked ISO region: this screen never builds E.164 and
// never sends a doubled dial code. The server canonicalises a phone-shaped identifier with that
// region and treats anything else as an email.
//
// When the server answers `No account uses that email or phone.` the screen shows that sentence and
// also offers `Create an account on the website`, which opens the website's sign-up page. SeatHub
// never creates an account: signup and password reset are the WEBSITE's, reached by a link
// (D-03, D-04), and no link carries a credential. The client never builds an address itself; it asks
// the facade to open one (`openWebsite`), so the addresses live in `web_origin.h` and nowhere in QML.
//
// Every state `docs/spec/screens.md` requires is here: empty (both labels, the helper, `Sign in`
// enabled), loading (the pressed control keeps its width and is busy), error (the message in the
// destructive token above the field for a field-level mistake and above the control for a refusal,
// with the ADR-0008 reference in mono) and offline (the deck's offline sentence, which the facade
// supplies as the refusal). A field-level mistake is named before anything is sent.
//
// The typed password lives in the field for the length of one attempt and is cleared after it. It is
// never stored, never logged and never leaves this screen except as the argument of the sign-in call.
Item {
    id: root

    /// The SeatHubClient facade. The only object this screen may talk to (D-35).
    property var client

    /// Emitted when a sign-in succeeds and the view should move on.
    signal signedIn()

    // Local view state, not app state - it never leaves this screen.
    property bool signingIn: false
    readonly property bool working: root.signingIn

    /// A field-level mistake, named before anything is sent (shown above its own field).
    property string identifierError: ""
    property string passwordError: ""
    /// A refusal that names no field (shown above the control), with its ADR-0008 reference.
    property string errorText: ""
    property string errorReference: ""
    /// True after the server answered that no account uses that email or phone (C-SIGNIN): the
    /// website's sign-up page is then offered beside the sentence.
    property bool noAccount: false

    property bool showPassword: false

    function clearErrors() {
        identifierError = ""
        passwordError = ""
        errorText = ""
        errorReference = ""
        noAccount = false
    }

    // The one action. A mistake the screen can see is named on its field and nothing is sent.
    function signInWithPassword() {
        clearErrors()
        var typed = identifierField.typed
        // An empty field, or text that is neither phone-shaped (three digits or more) nor shaped like
        // an email address, is named before a round trip is spent on it.
        if (typed === "" || identifierField.mode === "partial"
                || (identifierField.mode === "email" && !identifierField.looksLikeEmail)) {
            identifierError = qsTr("Enter your email or phone number.")
        }
        if (passwordField.text === "") {
            passwordError = qsTr("Enter your password.")
        }
        if (identifierError.length > 0) {
            identifierField.focusInput()
            return
        }
        if (passwordError.length > 0) {
            passwordField.forceActiveFocus()
            return
        }
        signingIn = true
        // The raw typed text and the picked region (the country in force: a number written with `+`
        // or `00` names its own). An email carries no region.
        client.signInWithPassword(typed, passwordField.text,
                                  identifierField.mode === "email" ? "" : identifierField.countryIso)
    }

    Component.onCompleted: identifierField.focusInput()

    Connections {
        target: root.client

        function onPasswordSignInRejected(message, reference, noAccount) {
            root.signingIn = false
            // The message is SeatHub copy or the server's own sentence; the reference is what support
            // needs (ADR-0008).
            root.errorText = message
            root.errorReference = reference ? reference : ""
            root.noAccount = noAccount === true
            // The password is held for one attempt only.
            passwordField.text = ""
            if (root.noAccount)
                identifierField.focusInput()
            else
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
                objectName: "errorGlyph"
                text: "◆"
                color: Tokens.destructiveDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.s5
                // Decoration beside the colour (WINDOWS #26), at the 20px inline icon size of
                // `docs/spec/ui.md` §8: the sentence is what is read out.
                Accessible.ignored: true
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

            // --- The identifier: an email or a phone number ---
            Column {
                objectName: "identifierStep"
                width: parent.width
                spacing: Metrics.s2

                FieldLabel { text: qsTr("Email or phone") }

                ErrorLine {
                    objectName: "fieldErrorLine"
                    message: root.identifierError
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
                    errorText: root.identifierError
                    // Enter moves on to the password when it is still empty, and signs in when it is not.
                    onAccepted: {
                        if (passwordField.text === "")
                            passwordField.forceActiveFocus()
                        else
                            root.signInWithPassword()
                    }
                    // What was wrong is no longer wrong once the customer edits the field, and a
                    // different identifier is a different question for the server.
                    onTextChanged: {
                        if (root.identifierError.length > 0) root.identifierError = ""
                        if (root.noAccount) {
                            root.noAccount = false
                            root.errorText = ""
                            root.errorReference = ""
                        }
                    }
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

            // --- The password ---
            Column {
                objectName: "passwordStep"
                width: parent.width
                spacing: Metrics.s2

                FieldLabel {
                    id: passwordLabel
                    text: qsTr("Password")
                    // `labelledBy` below gives this label an attached object, whose name is then
                    // empty unless set, so the label's own text is named here.
                    Accessible.name: text
                }

                ErrorLine {
                    objectName: "passwordErrorLine"
                    message: root.passwordError
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
                    // Qt reports an empty `Accessible.name` for a password-echo field while
                    // accessibility is active (QQuickAccessibleAttached::name), so the label
                    // relation is what tells a screen reader this field is "Password".
                    Accessible.name: qsTr("Password")
                    Accessible.labelledBy: passwordLabel
                    Accessible.description: root.passwordError
                    background: Rectangle {
                        radius: Metrics.radiusSm
                        color: Tokens.surface2Default
                        border.width: passwordField.activeFocus ? 2 : 1
                        border.color: root.passwordError.length > 0
                                      ? Tokens.destructiveDefault
                                      : (passwordField.activeFocus ? Tokens.focusDefault
                                                                   : Tokens.borderDefault)
                    }
                    onAccepted: root.signInWithPassword()
                    onTextChanged: if (root.passwordError.length > 0) root.passwordError = ""
                }

                SeatHubButton {
                    objectName: "revealPassword"
                    variant: "ghost"
                    text: root.showPassword ? qsTr("Hide password") : qsTr("Show password")
                    onClicked: root.showPassword = !root.showPassword
                }
            }

            // --- Error ---
            // A refusal that names no field: above the control, with the reference in mono.
            ErrorLine {
                objectName: "refusalLine"
                message: root.errorText
                reference: root.errorReference
            }

            // C-SIGNIN: when nothing matched, the website's sign-up page is offered beside the
            // sentence. SeatHub never creates an account; the address is the facade's to build.
            SeatHubButton {
                objectName: "noAccountSignUp"
                visible: root.noAccount
                variant: "ghost"
                text: qsTr("Create an account on the website")
                glyph: "↗"
                onClicked: root.client.openWebsite("signup")
            }

            // --- Primary action ---
            // Loading state: the pressed control keeps its width and says it is busy.
            SeatHubButton {
                id: primaryButton
                objectName: "primaryAction"
                width: parent.width
                enabled: !root.working
                busy: root.working
                text: qsTr("Sign in")

                onClicked: root.signInWithPassword()
            }

            // --- The two links to the website ---
            Column {
                width: parent.width
                spacing: Metrics.s1

                // Signup and password reset are the website's (D-03, D-04). The address is the
                // facade's to build; nothing is passed along with it.
                SeatHubButton {
                    objectName: "createAccountLink"
                    variant: "ghost"
                    text: qsTr("New here? Create an account")
                    glyph: "↗"
                    onClicked: root.client.openWebsite("signup")
                }

                SeatHubButton {
                    objectName: "forgotPasswordLink"
                    variant: "ghost"
                    text: qsTr("Forgot password?")
                    glyph: "↗"
                    onClicked: root.client.openWebsite("reset")
                }
            }
        }
    }
}
