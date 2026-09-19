import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// A setting SeatHub reports but does not offer a control for (D-47's "intentionally dropped"
// status, and `language`, which ADR-0043 keeps as a stored value with no toggle anywhere).
//
// Showing the key with its reason and its current stored value is what keeps the settings audit
// honest on screen: nothing is hidden, and nothing pretends to be changeable.
Item {
    id: root

    property var bridge: null
    property string settingKey: ""
    property string title: ""
    property string description: ""

    property string value: ""

    width: parent ? parent.width : 0
    implicitHeight: column.implicitHeight

    function refresh() {
        if (!bridge || settingKey.length === 0)
            return
        var v = bridge.getValue(settingKey)
        value = v === undefined || v === null ? "" : String(v)
    }

    Component.onCompleted: refresh()

    Connections {
        target: root.bridge

        function onValueChanged(key) {
            if (key === root.settingKey)
                root.refresh()
        }

        function onSessionOverridesChanged() {
            root.refresh()
        }
    }

    Column {
        id: column
        width: parent.width
        spacing: Metrics.s1

        Row {
            width: parent.width
            spacing: Metrics.s6

            Column {
                width: parent.width - valueLabel.width - Metrics.s6
                spacing: Metrics.s1

                Text {
                    text: root.title
                    width: parent.width
                    wrapMode: Text.Wrap
                    color: Tokens.foregroundMutedDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontBody
                }

                Text {
                    text: root.description
                    visible: text.length > 0
                    width: parent.width
                    wrapMode: Text.Wrap
                    color: Tokens.foregroundMutedDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontSm
                    lineHeight: 1.4
                }
            }

            Text {
                id: valueLabel
                width: Math.min(implicitWidth, 280)
                horizontalAlignment: Text.AlignRight
                text: root.value
                elide: Text.ElideRight
                color: Tokens.foregroundMutedDefault
                font.family: Tokens.fontMonoDefault
                font.pixelSize: Metrics.fontSm
            }
        }
    }
}
