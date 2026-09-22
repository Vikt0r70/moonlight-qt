import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// A labelled numeric row for the flat Settings page (D-48).
//
// The bounds are the ones the value genuinely has - upstream's parser ranges are not re-typed
// here, and no new bound is invented, so a value this control refuses is a value the engine
// would refuse too (or one that makes no sense at all, like a negative packet size).
Item {
    id: root

    property var bridge: null
    property string settingKey: ""
    property string title: ""
    property string description: ""

    property int minimum: 0
    property int maximum: 2147483647
    property string suffix: ""
    /// See `SeatHubSelect.valueProvider`.
    property var valueProvider: null
    /// A function returning this field's own bound, re-read on `refresh()`. Used when the bound
    /// itself depends on another setting (the bitrate ceiling widens while "Unlock bitrate
    /// limit" is on, D-26) - `maximum` alone would not react to that other key's own writes.
    property var maximumProvider: null
    /// Extra keys, besides `settingKey`, whose `valueChanged` should also trigger a refresh -
    /// the key `maximumProvider` reads, when it is not this field's own.
    property var refreshTriggerKeys: []

    property int value: 0
    property string warning: ""

    signal edited(int value)

    readonly property int controlWidth: 280

    width: parent ? parent.width : 0
    implicitHeight: column.implicitHeight
    opacity: root.bridge && root.bridge.writable ? 1.0 : 0.5

    function refresh() {
        if (!bridge)
            return
        if (maximumProvider !== null)
            maximum = maximumProvider()
        value = valueProvider !== null ? parseInt(valueProvider(), 10)
                                       : (settingKey.length > 0 ? parseInt(bridge.getValue(settingKey), 10) : 0)
        if (isNaN(value))
            value = root.minimum
        field.text = String(value)
        warning = settingKey.length > 0 ? bridge.negotiationWarning(settingKey) : ""
    }

    Component.onCompleted: refresh()

    Connections {
        target: root.bridge

        function onValueChanged(key) {
            if (key === root.settingKey || root.refreshTriggerKeys.indexOf(key) >= 0)
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
                width: parent.width - root.controlWidth - Metrics.s6
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
                    color: Tokens.foregroundSubtleDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontSm
                    lineHeight: 1.4
                }
            }

            Row {
                width: root.controlWidth
                spacing: Metrics.s2

                TextField {
                    id: field
                    width: root.suffix.length > 0 ? parent.width - suffixLabel.width - Metrics.s2
                                                  : parent.width
                    height: Metrics.touchTarget
                    enabled: root.bridge ? root.bridge.writable : false
                    inputMethodHints: Qt.ImhDigitsOnly
                    selectByMouse: true
                    horizontalAlignment: TextInput.AlignRight
                    color: Tokens.foregroundDefault
                    font.family: Tokens.fontMonoDefault
                    font.pixelSize: Metrics.fontBody
                    font.kerning: false
                    font.features: { "tnum": 1 }
                    leftPadding: Metrics.s3
                    rightPadding: Metrics.s3
                    background: Rectangle {
                        radius: Metrics.radiusXs
                        color: Tokens.surface2Default
                        border.color: field.activeFocus ? Tokens.focusDefault : Tokens.borderDefault
                        border.width: field.activeFocus ? 2 : 1
                    }

                    validator: IntValidator {
                        bottom: root.minimum
                        top: root.maximum
                    }

                    onEditingFinished: {
                        if (!root.bridge || !root.bridge.writable)
                            return
                        var parsed = parseInt(text, 10)
                        if (isNaN(parsed) || parsed < root.minimum || parsed > root.maximum) {
                            root.refresh()
                            return
                        }
                        if (root.settingKey.length > 0 && root.bridge.setValue(root.settingKey, parsed))
                            root.edited(parsed)
                        root.refresh()
                    }
                }

                Text {
                    id: suffixLabel
                    visible: root.suffix.length > 0
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.suffix
                    color: Tokens.foregroundSubtleDefault
                    font.family: Tokens.fontMonoDefault
                    font.pixelSize: Metrics.fontSm
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
