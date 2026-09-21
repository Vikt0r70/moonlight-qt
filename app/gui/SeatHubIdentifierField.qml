import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// The one sign-in field: an email or a phone number (`docs/spec/screens.md` §22, Phase 5 D-01, D-02).
//
// It works out which one it is as the customer types. Digits (with spaces, dashes, dots, parentheses
// and an optional leading plus) make it a phone number, and once there are three of them a COUNTRY
// TAG - the ISO code, the dial code and a chevron - appears inside the field frame, to the left of the
// text, as a sibling of the input and not part of it. An `@` or any letter makes it an email: the tag
// goes away and the field is a plain field again. The three-digit threshold is a spec value
// (`screens.md` §22), not a feeling.
//
// The tag is a button that opens the country picker and reads its own country aloud. It starts on
// the machine's region (`defaultCountryCode`, from the facade), and changing it never touches the
// digits already typed. A number typed with a leading `+` or `00` names its own country, and the tag
// then shows the country the number matched: what the customer typed wins over what the tag says.
//
// MOTION (`ui.md` §6, §12): the tag reveals with opacity and a translate of the tag and the input
// text from -8px to 0 over `motionBase`, ease-out, and hides in reverse. Nothing here animates a
// width, an `x` or a padding: the input's position changes at once, and only `reveal` - which drives
// opacity and a transform - is animated. The caret stays where it was because the input is never
// recreated or refocused. With Windows' "Animation effects" off (`animate: false`) the tag appears
// and hides without a transition.
//
// A long email scrolls inside the input (it is clipped to the frame) rather than stretching or
// wrapping the field: the frame's width is the field's, never the text's.
//
// Flat, LTR, never mirrored (ADR-0043). The frame is 44px, the touch floor of `ui.md` §9.
Item {
    id: root

    /// `{iso, name, dial}` rows: the bundled list, `SeatHubClient.countries`.
    property var countries: []
    /// The ISO code the tag starts on (`SeatHubClient.defaultCountryCode`).
    property string defaultCountryCode: "JO"
    /// The country the customer chose with the picker (or the default until they do).
    property string countryCode: defaultCountryCode
    /// What the input holds.
    property alias text: input.text
    /// False turns the reveal into an instant change (Windows animation effects off).
    property bool animate: true
    /// The sentence naming what is wrong with the field, if any: it is the field's accessible
    /// description and draws the frame in the destructive colour.
    property string errorText: ""

    /// The number of digits at which the tag appears (`screens.md` §22, Phase 5 D-02).
    readonly property int tagThreshold: 3

    readonly property string typed: input.text.trim()

    /// "empty" | "email" | "phone" (a number with the tag showing) | "partial" (digits, but too few
    /// for the tag yet).
    readonly property string mode: root.classify(input.text)
    readonly property bool tagShown: root.mode === "phone"
    /// True when `typed` has the shape of an email address (what the website checks before it sends).
    readonly property bool looksLikeEmail: /^[^@\s]+@[^@\s]+\.[^@\s]+$/.test(root.typed)
                                           && root.typed.length <= 254

    /// The country the customer picked, for the dial code a national number is completed with.
    readonly property var selectedCountry: root.countryFor(root.countryCode)
    readonly property string selectedDial: root.selectedCountry.dial
    /// The country in force: the one a leading `+`/`00` names, else the one picked.
    readonly property var country: root.matchByNumber(input.text) || root.selectedCountry
    /// The country in force, as plain strings (what the tag shows and reads aloud).
    readonly property string countryIso: root.country.iso
    readonly property string countryName: root.country.name
    readonly property string countryDial: root.country.dial

    /// 0 with the tag hidden, 1 with it shown. Drives opacity and a translate, nothing else.
    property real reveal: root.tagShown ? 1.0 : 0.0
    Behavior on reveal {
        enabled: root.animate
        NumberAnimation { duration: Metrics.motionBase; easing.type: Easing.OutCubic }
    }

    /// Enter was pressed in the input.
    signal accepted()

    implicitWidth: 360
    implicitHeight: Metrics.touchTarget

    function focusInput() {
        input.forceActiveFocus()
    }

    function setCountry(iso) {
        root.countryCode = iso
    }

    function openPicker() {
        pickerPopup.open()
    }

    // Digits typed as Arabic-Indic or Persian numerals are the Latin digits they are, the same
    // reading the website and `ControlPlaneClient::normalisePhoneE164` apply.
    function latinDigits(value) {
        return value.replace(/[\u0660-\u0669]/g, function (d) { return String(d.charCodeAt(0) - 0x0660) })
                    .replace(/[\u06f0-\u06f9]/g, function (d) { return String(d.charCodeAt(0) - 0x06f0) })
    }

    function classify(raw) {
        var t = root.latinDigits(raw).trim()
        if (t === "")
            return "empty"
        // Phone only when it is nothing but digits, spaces, dashes, dots, parentheses and an
        // optional leading plus; an `@`, a letter or anything else is an email being typed.
        if (!/^\+?[0-9\s\-().]*$/.test(t))
            return "email"
        var digits = t.replace(/[^0-9]/g, "").length
        return digits >= root.tagThreshold ? "phone" : "partial"
    }

    function countryFor(iso) {
        for (var i = 0; i < root.countries.length; ++i) {
            if (root.countries[i].iso === iso)
                return root.countries[i]
        }
        return root.countries.length > 0 ? root.countries[0] : { iso: "", name: "", dial: "" }
    }

    // The country a number written with `+` or `00` belongs to: the longest dial code that starts
    // it (`+1876` before `+1`); of countries sharing a dial code, the first in list order.
    function matchByNumber(raw) {
        var t = root.latinDigits(raw).replace(/[\s\-().]/g, "")
        var digits
        if (t.charAt(0) === "+")
            digits = t.slice(1)
        else if (t.slice(0, 2) === "00")
            digits = t.slice(2)
        else
            return null
        var best = null
        var bestLength = 0
        for (var i = 0; i < root.countries.length; ++i) {
            var dialDigits = root.countries[i].dial.slice(1)
            if (dialDigits.length > bestLength && digits.indexOf(dialDigits) === 0) {
                best = root.countries[i]
                bestLength = dialDigits.length
            }
        }
        return best
    }

    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Email or phone")

    Rectangle {
        id: frame
        objectName: "identifierFrame"
        anchors.fill: parent
        radius: Metrics.radiusSm
        color: Tokens.surface2Default
        border.width: input.activeFocus || tagButton.activeFocus ? 2 : 1
        border.color: root.errorText.length > 0 ? Tokens.destructiveDefault
                      : (input.activeFocus || tagButton.activeFocus ? Tokens.focusDefault
                                                                      : Tokens.borderDefault)

        // The country tag, a sibling of the input. Its slot is laid out at its final width whenever
        // it is shown; it fades and slides in through opacity and a transform.
        Button {
            id: tagButton
            objectName: "countryTag"
            x: Metrics.s2
            anchors.verticalCenter: parent.verticalCenter
            height: 28
            visible: root.reveal > 0
            opacity: root.reveal
            enabled: root.tagShown && root.enabled
            activeFocusOnTab: root.tagShown
            transform: Translate { x: (1 - root.reveal) * -8 }

            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Country code: %1").arg(root.country.name)

            contentItem: Row {
                spacing: Metrics.s1

                Text {
                    objectName: "tagIso"
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.country.iso
                    color: Tokens.foregroundDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontSm
                    font.weight: Font.DemiBold
                }

                Text {
                    objectName: "tagDial"
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.country.dial
                    color: Tokens.foregroundDefault
                    font.family: Tokens.fontMonoDefault
                    font.pixelSize: Metrics.fontSm
                }

                // The chevron: a glyph, the fork ships no icon set.
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "\u25be"
                    color: Tokens.foregroundMutedDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontSm
                }
            }

            leftPadding: Metrics.s2
            rightPadding: Metrics.s2

            background: Rectangle {
                radius: Metrics.radiusXs
                color: Tokens.surface3Default
                border.width: tagButton.activeFocus ? 2 : 0
                border.color: Tokens.focusDefault
            }

            onClicked: root.openPicker()
        }

        TextInput {
            id: input
            objectName: "identifierInput"
            // Discrete, never animated: the text simply sits after the tag while it is shown.
            x: root.tagShown ? tagButton.x + tagButton.width + Metrics.s2 : Metrics.s3
            width: frame.width - input.x - Metrics.s3
            anchors.verticalCenter: parent.verticalCenter
            clip: true
            selectByMouse: true
            autoScroll: true
            enabled: root.enabled
            color: Tokens.foregroundDefault
            selectionColor: Tokens.primaryDefault
            selectedTextColor: Tokens.primaryForegroundDefault
            font.family: Tokens.fontSansDefault
            font.pixelSize: Metrics.fontBody
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            transform: Translate { x: (1 - root.reveal) * -8 }

            Accessible.role: Accessible.EditableText
            Accessible.name: qsTr("Email or phone")
            Accessible.description: root.errorText

            onAccepted: root.accepted()
        }
    }

    Popup {
        id: pickerPopup
        parent: frame
        x: 0
        y: frame.height + Metrics.s1
        width: 288
        padding: 0
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        // The panel draws its own box.
        background: Item {}

        // Whether the popup is closing because a row was chosen (focus goes back to the text, so the
        // customer can carry on typing) or because it was dismissed (focus goes back to the tag).
        property bool chosen: false

        contentItem: SeatHubCountryPicker {
            id: picker
            model: root.countries
            selectedIso: root.country.iso
            onPicked: function (iso) {
                pickerPopup.chosen = true
                root.setCountry(iso)
                pickerPopup.close()
            }
            onDismissed: pickerPopup.close()
        }

        onOpened: {
            pickerPopup.chosen = false
            picker.reset()
        }
        onClosed: {
            if (pickerPopup.chosen)
                input.forceActiveFocus()
            else if (root.tagShown)
                tagButton.forceActiveFocus()
            else
                input.forceActiveFocus()
        }
    }
}
