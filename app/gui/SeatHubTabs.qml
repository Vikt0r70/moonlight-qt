import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// The profile's tabs: one tab per list, one list region under them (`05-UI-SPEC` "Profile", D-14).
//
// A small toggle group in the `ui.md` §7 sense: exactly one tab is selected, the selected one is
// marked by a fill AND by its text (semibold, in the primary colour) so the mark never rests on
// colour alone, and the others are quiet. It is a real control: each tab is a `Button` (focusable,
// named, at least 40px tall), only the selected one is in the tab chain, and the arrow keys move
// between the tabs, as they do in a toolbar - one Tab stop for the group, arrows inside it.
//
// It only says which tab is selected (`currentIndex`) and when one is activated
// (`tabActivated(index)`); what each tab shows is the screen's business. The titles come from the
// screen, which takes them from the copy deck.
FocusScope {
    id: root

    objectName: "seatHubTabs"

    /// The tab labels, in order.
    property var titles: []

    /// The selected tab.
    property int currentIndex: 0

    /// Emitted when the customer selects a tab (a click or an arrow key), including the one that is
    /// already selected.
    signal tabActivated(int index)

    implicitHeight: Metrics.pointerTarget
    implicitWidth: row.implicitWidth

    /// Selects tab `index` (ignored when there is no such tab), and moves focus to it so the arrow
    /// keys keep working from where the customer is.
    function select(index) {
        if (index < 0 || index >= root.titles.length)
            return
        root.currentIndex = index
        const tab = tabs.itemAt(index)
        if (tab)
            tab.forceActiveFocus()
        root.tabActivated(index)
    }

    Row {
        id: row
        spacing: Metrics.s2

        Repeater {
            id: tabs
            model: root.titles

            delegate: Button {
                id: tab

                required property int index
                required property var modelData

                readonly property bool selected: tab.index === root.currentIndex

                objectName: "tab" + tab.index
                text: String(tab.modelData)
                height: Metrics.pointerTarget
                // One tab stop for the whole group: the selected tab. The arrows do the rest.
                activeFocusOnTab: tab.selected
                focusPolicy: Qt.StrongFocus
                leftPadding: Metrics.s4
                rightPadding: Metrics.s4

                Accessible.role: Accessible.PageTab
                Accessible.name: tab.text
                Accessible.selected: tab.selected

                onClicked: root.select(tab.index)

                Keys.onLeftPressed: root.select(tab.index - 1)
                Keys.onRightPressed: root.select(tab.index + 1)

                contentItem: Text {
                    text: tab.text
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    wrapMode: Text.NoWrap
                    // Selected: the primary colour and the semibold weight, over the fill below.
                    color: tab.selected ? Tokens.primaryDefault : Tokens.foregroundMutedDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontBody
                    font.weight: tab.selected ? Font.DemiBold : Font.Normal
                }

                background: Rectangle {
                    radius: Metrics.radiusSm
                    border.width: tab.activeFocus ? 2 : 0
                    border.color: Tokens.focusDefault
                    // The selected tab is `--primary-soft`; a tab that is only hovered is one step
                    // up from the ground so the two can never be mistaken for each other.
                    color: tab.selected ? Tokens.primarySoftDefault
                                        : (tab.hovered ? Tokens.surface2Default : "transparent")
                    Behavior on color {
                        ColorAnimation { duration: Metrics.motionFast }
                    }
                }
            }
        }
    }
}
