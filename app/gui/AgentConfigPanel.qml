import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import SeatHub.Tokens 1.0

// The host agent's config file, chosen by the customer (audit follow-up, 03-UI-REVIEW-FIXES.md).
//
// What it is for: the Node Agent on the rental PC writes its credential to
// `%ProgramData%\SeatHub\node-agent.json` (`seathub-host-agents/crates/node-agent/src/main.rs` -
// the same `{ host_id, token, base_url }` shape the Host Switcher writes). Support asks the
// customer to point at that file when an installation will not enrol.
//
// What crosses the boundary: the absolute path and the token in masked form, nothing else. The
// token itself never reaches QML - `AgentConfig::describe()` does not return it (D-30 applied one
// layer up), it is not logged, and it is not stored. A file that is missing, unreadable or carries
// no token is an ordinary answer (`ok: false` plus a reason), never a session failure: the customer
// picked the file and nothing about the session depends on it.
//
// COPY: every string in this panel is new and is NOT yet in `docs/spec/copy.md`. They are named
// here so a copy review can find them all at once; the deck owns them before this ships.
Item {
    id: root

    /// The SeatHubClient facade (D-35).
    property var client: null

    /// `AgentConfig::describe()`'s map: `{ path, ok, token_masked, error }`.
    property var described: null

    readonly property string describedPath: root.described ? String(root.described.path) : ""
    readonly property bool tokenFound: root.described ? root.described.ok === true : false
    readonly property string maskedToken: root.tokenFound ? String(root.described.token_masked) : ""
    readonly property string reason: root.described === null || root.tokenFound
                                     ? "" : String(root.described.error)

    readonly property int controlWidth: 280

    width: parent ? parent.width : 0
    implicitHeight: column.implicitHeight

    Column {
        id: column
        width: parent.width
        spacing: Metrics.s3

        Text {
            width: parent.width
            text: qsTr("Agent config file")
            color: Tokens.foregroundDefault
            font.family: Tokens.fontSansDefault
            font.pixelSize: Metrics.fontBody
        }

        Text {
            width: parent.width
            text: qsTr("Point at the host agent's config file if support asks for it.")
            wrapMode: Text.Wrap
            color: Tokens.foregroundMutedDefault
            font.family: Tokens.fontSansDefault
            font.pixelSize: Metrics.fontSm
            lineHeight: 1.4
        }

        Row {
            width: parent.width
            spacing: Metrics.s6

            SeatHubButton {
                id: chooseButton
                variant: "ghost"
                text: qsTr("Choose file")
                onClicked: picker.open()
            }

            Text {
                id: pathText
                width: parent.width - chooseButton.width - Metrics.s6
                anchors.verticalCenter: chooseButton.verticalCenter
                text: root.describedPath
                visible: text.length > 0
                elide: Text.ElideMiddle
                color: Tokens.foregroundDefault
                font.family: Tokens.fontMonoDefault
                font.pixelSize: Metrics.fontSm
                font.kerning: false
                font.features: {"tnum": 1}
            }
        }

        // copy.md §5: the masked credential is a value, so it is mono, like every other data
        // value. Fixed-length mask - it never discloses the token's length.
        Text {
            id: tokenText
            width: parent.width
            text: qsTr("Agent token %1").arg(root.maskedToken)
            visible: root.tokenFound
            color: Tokens.foregroundMutedDefault
            font.family: Tokens.fontMonoDefault
            font.pixelSize: Metrics.fontSm
            font.kerning: false
            font.features: {"tnum": 1}
        }

        Text {
            id: reasonText
            width: parent.width
            text: root.reason
            visible: text.length > 0
            wrapMode: Text.Wrap
            color: Tokens.destructiveDefault
            font.family: Tokens.fontSansDefault
            font.pixelSize: Metrics.fontSm
            lineHeight: 1.4
        }
    }

    FileDialog {
        id: picker
        title: qsTr("Agent config file")
        nameFilters: [qsTr("Agent config (*.json)"), qsTr("All files (*)")]
        onAccepted: {
            root.described = root.client
                             ? root.client.readAgentConfigFile(picker.selectedFile)
                             : null
        }
    }
}
