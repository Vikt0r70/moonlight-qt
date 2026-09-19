import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// A labelled dropdown row for the flat Settings page (D-48).
//
// The options come from the bridge's catalogue rather than being re-typed in QML, so a dropdown
// can never offer a value the setting does not accept. The control is 280px wide and the
// dropdown's own text elides, which is what keeps the longest codec and decoder names inside
// the page at the 960px minimum window width (UI-SPEC E9).
Item {
    id: root

    property var bridge: null
    property string settingKey: ""
    property string title: ""
    property string description: ""
    /// Overrides the bridge's option list when the control is a SeatHub aggregate rather than
    /// a single stored key (resolution presets are derived from `width` + `height`).
    property var optionsOverride: null
    /// A function returning the value to display. Used by the aggregate controls; when null the
    /// value is read straight from `settingKey`.
    property var valueProvider: null

    property string value: ""
    property string warning: ""

    signal edited(string value)

    readonly property int controlWidth: 280

    width: parent ? parent.width : 0
    implicitHeight: column.implicitHeight
    opacity: root.bridge && root.bridge.writable ? 1.0 : 0.5

    function currentOptions() {
        if (optionsOverride !== null)
            return optionsOverride
        return bridge && settingKey.length > 0 ? bridge.enumOptions(settingKey) : []
    }

    function refresh() {
        if (!bridge)
            return
        value = valueProvider !== null ? String(valueProvider())
                                       : (settingKey.length > 0 ? String(bridge.getValue(settingKey)) : "")
        warning = settingKey.length > 0 ? bridge.negotiationWarning(settingKey) : ""
        dropdown.currentIndex = Math.max(0, currentOptions().indexOf(value))
    }

    Component.onCompleted: refresh()

    Connections {
        target: root.bridge

        function onValueChanged(key) {
            if (key === root.settingKey || key === "width" || key === "height")
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
                    color: Tokens.foregroundMutedDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontSm
                    lineHeight: 1.4
                }
            }

            ComboBox {
                id: dropdown
                width: root.controlWidth
                height: Metrics.touchTarget
                enabled: root.bridge ? root.bridge.writable : false
                model: root.currentOptions()
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontBody

                background: Rectangle {
                    radius: Metrics.radiusSm
                    color: Tokens.surface2Default
                    border.color: dropdown.activeFocus ? Tokens.focusDefault : Tokens.borderDefault
                    border.width: dropdown.activeFocus ? 2 : 1
                }

                contentItem: Text {
                    leftPadding: Metrics.s3
                    rightPadding: dropdown.indicator.width + Metrics.s2
                    text: dropdown.displayText
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                    color: dropdown.enabled ? Tokens.foregroundDefault : Tokens.foregroundMutedDefault
                    font: dropdown.font
                }

                // E9: the closed control elides, and so must the popup - the longest codec and
                // decoder names must not widen the flat page at the 960px minimum window width.
                // The popup is capped to the control's own width and the row's text elides.
                popup.width: dropdown.width
                popup.height: Math.min(contentItem.implicitHeight, 320)

                delegate: ItemDelegate {
                    id: optionDelegate
                    width: dropdown.width
                    text: modelData
                    font: dropdown.font
                    highlighted: dropdown.highlightedIndex === index

                    contentItem: Text {
                        text: optionDelegate.text
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                        color: Tokens.foregroundDefault
                        font: dropdown.font
                    }

                    background: Rectangle {
                        color: optionDelegate.highlighted ? Tokens.surface3Default : Tokens.surface2Default
                    }
                }

                onActivated: {
                    if (!root.bridge || !root.bridge.writable)
                        return
                    if (root.settingKey.length > 0)
                        root.bridge.setValue(root.settingKey, currentText)
                    root.edited(currentText)
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
