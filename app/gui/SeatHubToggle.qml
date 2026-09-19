import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// A labelled on/off setting row for the flat Settings page (D-48).
//
// The row reads and writes through `SettingsBridge`, which is the only object allowed to touch
// `StreamingPreferences`; this file never stores a value of its own beyond the copy it displays.
Item {
    id: root

    /// The SettingsBridge exposed by `SeatHubClient.settings`.
    property var bridge: null
    /// The `[streamsettings]` key this row is the control for.
    property string settingKey: ""
    property string title: ""
    property string description: ""

    /// The persisted value, mirrored here so the row has something to render while a write is
    /// refused (during a stream the whole page is read-only).
    property bool value: false
    /// D-14: the SeatHub sentence shown when the engine could not honour the saved value.
    property string warning: ""

    signal edited(bool value)

    width: parent ? parent.width : 0
    implicitHeight: column.implicitHeight
    opacity: root.bridge && root.bridge.writable ? 1.0 : 0.5

    function refresh() {
        if (!bridge || settingKey.length === 0)
            return
        value = bridge.getValue(settingKey) === true
        warning = bridge.negotiationWarning(settingKey)
    }

    Component.onCompleted: refresh()

    Connections {
        target: root.bridge

        function onValueChanged(key) {
            if (key === root.settingKey)
                root.refresh()
        }

        function onNegotiationChanged() {
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
                width: parent.width - toggle.width - Metrics.s6
                spacing: Metrics.s1

                Text {
                    text: root.title
                    width: parent.width
                    wrapMode: Text.Wrap
                    color: Tokens.foregroundDefault
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

            CheckBox {
                id: toggle
                width: Metrics.touchTarget
                height: Metrics.touchTarget
                enabled: root.bridge ? root.bridge.writable : false
                checked: root.value

                indicator: Rectangle {
                    x: (parent.width - width) / 2
                    y: (parent.height - height) / 2
                    width: Metrics.s6
                    height: Metrics.s6
                    radius: Metrics.radiusXs
                    color: toggle.checked ? Tokens.primaryDefault : "transparent"
                    border.color: toggle.checked ? Tokens.primaryDefault : Tokens.borderStrongDefault
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        visible: toggle.checked
                        text: "\u2713"
                        color: Tokens.primaryForegroundDefault
                        font.family: Tokens.fontSansDefault
                        font.pixelSize: Metrics.fontSm
                    }
                }

                onToggled: {
                    if (!root.bridge || !root.bridge.writable)
                        return
                    if (root.bridge.setValue(root.settingKey, checked))
                        root.edited(checked)
                    else
                        root.refresh()
                }
            }
        }

        Text {
            text: root.warning
            visible: text.length > 0
            width: parent.width
            wrapMode: Text.Wrap
            color: Tokens.warnDefault
            font.family: Tokens.fontSansDefault
            font.pixelSize: Metrics.fontSm
        }
    }
}
