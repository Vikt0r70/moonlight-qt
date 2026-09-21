import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// The country list behind the sign-in field's tag (`docs/spec/screens.md` §22, 05-UI-SPEC "SeatHubCountryPicker").
//
// The panel a popup wraps: a search box at the top and a scrolling list under it, each row the
// country's name with its dial code to the right, the current country marked, a sentence when
// nothing matches. It behaves the way the website's picker does (`country-picker.tsx`): a query
// matches a name anywhere in it, or the digits a dial code starts with, and `+962`, `962` and
// `00962` all mean the dial code.
//
// Its data is the `model` it is handed - the bundled list the facade exposes (`SeatHubClient.countries`).
// It fetches nothing and knows nothing about the network, so the picker is as available offline as
// it is online (the loading, error and partial rows of the UI contract do not exist for it).
//
// Flat, LTR, never mirrored (ADR-0043). Rows are 40px, the pointer target of `ui.md` §9. A long name
// elides, and the full name is the row's accessible name so a screen reader still says all of it.
//
// It is an Item rather than a Popup on purpose: the popup that hosts it lives in the field, and the
// panel stays loadable (and assertable) on its own.
Item {
    id: root

    /// `{iso, name, dial}` rows, in the order to show them.
    property var model: []
    /// The ISO code of the country in force, marked in the list.
    property string selectedIso: ""
    /// What the search box holds. Set it to search from code; typing sets it for the customer.
    property string query: ""

    /// The rows the query leaves, in the same order.
    readonly property var matches: root.filter(root.model, root.query)
    readonly property bool noMatches: root.matches.length === 0
    /// How many rows the query leaves, and which (their ISO codes, comma-joined, in list order).
    readonly property int matchCount: root.matches.length
    readonly property string matchedIsoList: root.matches.map(function (row) { return row.iso }).join(",")

    /// A row was chosen.
    signal picked(string iso)
    /// Escape was pressed: the host closes the popup and returns focus to the tag.
    signal dismissed()

    width: 288
    implicitWidth: 288
    implicitHeight: box.implicitHeight

    // The website's `countryMatches`: name anywhere, or the dial code's digits from the start.
    function rowMatches(row, query) {
        var needle = query.trim().toLowerCase()
        if (needle === "")
            return true
        if (row.name.toLowerCase().indexOf(needle) >= 0)
            return true
        var digits = needle.replace(/^\+|^00/, "")
        return /^\d+$/.test(digits) && row.dial.slice(1).indexOf(digits) === 0
    }

    function filter(rows, query) {
        var out = []
        for (var i = 0; i < rows.length; ++i) {
            if (root.rowMatches(rows[i], query))
                out.push(rows[i])
        }
        return out
    }

    /// Clears the search, puts the highlight on the country in force and focuses the search box.
    function reset() {
        search.text = ""
        root.query = ""
        var at = -1
        for (var i = 0; i < root.matches.length; ++i) {
            if (root.matches[i].iso === root.selectedIso) {
                at = i
                break
            }
        }
        list.currentIndex = at >= 0 ? at : 0
        list.positionViewAtIndex(list.currentIndex, ListView.Contain)
        search.forceActiveFocus()
    }

    function pickCurrent() {
        if (list.currentIndex >= 0 && list.currentIndex < root.matches.length)
            root.picked(root.matches[list.currentIndex].iso)
    }

    Rectangle {
        id: box
        anchors.fill: parent
        implicitHeight: content.implicitHeight + Metrics.s2 * 2
        radius: Metrics.radiusSm
        color: Tokens.surface1Default
        border.width: 1
        border.color: Tokens.borderDefault

        Column {
            id: content
            x: Metrics.s2
            y: Metrics.s2
            width: parent.width - Metrics.s2 * 2
            spacing: Metrics.s2

            TextField {
                id: search
                objectName: "countrySearch"
                width: parent.width
                height: Metrics.pointerTarget
                placeholderText: qsTr("Search countries")
                placeholderTextColor: Tokens.foregroundMutedDefault
                color: Tokens.foregroundDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontBody
                inputMethodHints: Qt.ImhNoPredictiveText
                Accessible.name: qsTr("Search countries")

                background: Rectangle {
                    radius: Metrics.radiusXs
                    color: Tokens.surface2Default
                    border.width: search.activeFocus ? 2 : 1
                    border.color: search.activeFocus ? Tokens.focusDefault : Tokens.borderDefault
                }

                onTextChanged: {
                    root.query = search.text
                    list.currentIndex = 0
                }

                Keys.onDownPressed: list.incrementCurrentIndex()
                Keys.onUpPressed: list.decrementCurrentIndex()
                Keys.onReturnPressed: root.pickCurrent()
                Keys.onEnterPressed: root.pickCurrent()
                Keys.onEscapePressed: root.dismissed()
            }

            ListView {
                id: list
                objectName: "countryList"
                width: parent.width
                // "A list of at most 256px with internal scroll" - it shrinks to fit a short result.
                height: Math.min(contentHeight, 256)
                visible: !root.noMatches
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: root.matches
                currentIndex: 0
                ScrollBar.vertical: ScrollBar {}

                delegate: Item {
                    id: row
                    required property var modelData
                    required property int index

                    objectName: "countryRow"
                    width: list.width
                    height: Metrics.pointerTarget

                    readonly property bool isSelected: row.modelData.iso === root.selectedIso

                    Accessible.role: Accessible.ListItem
                    // The full name and the code, even when the visible name is elided.
                    Accessible.name: row.modelData.name + ", " + row.modelData.dial
                    Accessible.selected: row.isSelected

                    Rectangle {
                        anchors.fill: parent
                        radius: Metrics.radiusXs
                        color: row.ListView.isCurrentItem ? Tokens.surface3Default : "transparent"
                    }

                    // The selected row's mark. A glyph beside the colour: the fork ships no icon set.
                    Text {
                        id: mark
                        objectName: "selectedMark"
                        x: Metrics.s2
                        anchors.verticalCenter: parent.verticalCenter
                        width: Metrics.s4
                        visible: row.isSelected
                        text: "\u2713"
                        color: Tokens.foregroundDefault
                        font.family: Tokens.fontSansDefault
                        font.pixelSize: Metrics.fontSm
                    }

                    Text {
                        id: dial
                        anchors.right: parent.right
                        anchors.rightMargin: Metrics.s2
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.modelData.dial
                        color: Tokens.foregroundMutedDefault
                        font.family: Tokens.fontMonoDefault
                        font.pixelSize: Metrics.fontSm
                    }

                    Text {
                        objectName: "countryName"
                        anchors.left: mark.right
                        anchors.leftMargin: Metrics.s1
                        anchors.right: dial.left
                        anchors.rightMargin: Metrics.s3
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.modelData.name
                        elide: Text.ElideRight
                        wrapMode: Text.NoWrap
                        color: Tokens.foregroundDefault
                        font.family: Tokens.fontSansDefault
                        font.pixelSize: Metrics.fontBody
                    }

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        onEntered: list.currentIndex = row.index
                        onClicked: root.picked(row.modelData.iso)
                    }
                }
            }

            Text {
                objectName: "noMatchLine"
                width: parent.width
                height: Metrics.pointerTarget
                visible: root.noMatches
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                // docs/spec/screens.md §22 and the website's own line.
                text: qsTr("No country matches that.")
                color: Tokens.foregroundMutedDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontSm
            }
        }
    }
}
