import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// Six-cell one-time-code entry: 3 + 3 with a gap, auto-advance as digits arrive, and
// backspace stepping back a cell (03-PATTERNS.md §SeatHubOTPField).
//
// Flat, LTR, never mirrored (ADR-0043). Each cell is a 44px target per docs/spec/ui.md.
Item {
    id: root

    /// The digits entered so far, 0 to 6 characters.
    property string code: ""
    property bool entryEnabled: true

    /// Emitted once the sixth digit lands.
    signal completed()

    implicitWidth: cells.implicitWidth
    implicitHeight: cells.implicitHeight

    function clear() {
        code = ""
        for (var i = 0; i < otpCells.count; i++) {
            otpCells.itemAt(i).cellInput.text = ""
        }
        focusCell(0)
    }

    function focusCell(index) {
        if (index < 0 || index >= otpCells.count) {
            return
        }
        otpCells.itemAt(index).cellInput.forceActiveFocus()
    }

    function cellText() {
        var digits = ""
        for (var i = 0; i < otpCells.count; i++) {
            digits += otpCells.itemAt(i).cellInput.text
        }
        return digits
    }

    Row {
        id: cells
        spacing: Metrics.s2

        Repeater {
            id: otpCells
            model: 6

            delegate: Item {
                id: cellSlot
                required property int index

                // The field inside this cell. Exposed as a property because QML ids are
                // component-private: without it, focusCell() could only focus this Item, and
                // the key events would land on the Item (which has no handlers) instead of on
                // the TextInput, so typing would silently go nowhere.
                property alias cellInput: input

                // The "3 + 3" grouping: the fourth cell reserves the group gap on its left,
                // and its box is right-aligned inside that wider slot, so the six cells stay
                // identical and only the gap is added.
                width: Metrics.touchTarget + (index === 3 ? Metrics.s3 : 0)
                height: Metrics.touchTarget

                Rectangle {
                    anchors.fill: parent
                    anchors.leftMargin: cellSlot.index === 3 ? Metrics.s3 : 0
                    radius: Metrics.radiusSm
                    color: Tokens.surface2Default
                    border.width: 1
                    border.color: input.activeFocus ? Tokens.focusDefault
                                                    : (input.text.length > 0 ? Tokens.borderStrongDefault
                                                                             : Tokens.borderDefault)

                    TextInput {
                        id: input
                        anchors.fill: parent
                        anchors.margins: 1
                        horizontalAlignment: TextInput.AlignHCenter
                        verticalAlignment: TextInput.AlignVCenter
                        enabled: root.entryEnabled
                        inputMethodHints: Qt.ImhDigitsOnly
                        maximumLength: 1
                        selectByMouse: true
                        color: Tokens.foregroundDefault
                        font.family: Tokens.fontMonoDefault
                        font.pixelSize: Metrics.fontH3

                        // Focus ring must be visible (docs/spec/ui.md a11y).
                        Keys.onLeftPressed: root.focusCell(cellSlot.index - 1)
                        Keys.onRightPressed: root.focusCell(cellSlot.index + 1)

                        onTextChanged: {
                            if (text.length === 1) {
                                if (cellSlot.index < 5) {
                                    root.focusCell(cellSlot.index + 1)
                                } else {
                                    input.focus = false
                                }
                                root.code = root.cellText()
                                if (root.code.length === 6) {
                                    root.completed()
                                }
                            } else if (text.length === 0) {
                                root.code = root.cellText()
                            }
                        }

                        Keys.onPressed: function (event) {
                            if (event.key === Qt.Key_Backspace && input.text.length === 0) {
                                root.focusCell(cellSlot.index - 1)
                                var previous = otpCells.itemAt(cellSlot.index - 1)
                                if (previous) {
                                    previous.cellInput.text = ""
                                }
                                event.accepted = true
                            }
                        }
                    }
                }
            }
        }
    }
}
