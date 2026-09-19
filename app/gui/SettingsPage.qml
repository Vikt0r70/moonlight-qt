import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeatHub.Tokens 1.0

// The SeatHub Settings page (STREAM-02, CUST-05, D-11, D-48).
//
// One flat page, five groups, no tabs and no collapsed sections: every `[streamsettings]` key
// upstream serializes has a control here, and the key it is the control for is named in the row
// so the page and `settings-audit.md` can be checked against each other line by line.
//
// What this page deliberately is not:
//   * Not a second preference store. Every read and every write goes through
//     `SeatHubClient.settings` -> `SettingsBridge` -> upstream `StreamingPreferences` (D-12).
//   * Not editable during a session. `SettingsBridge.writable` is false while a stream runs, so
//     the rows disable themselves and show one banner instead of silently dropping a change
//     (Pitfall 6). Changes apply to the next stream (D-13).
//   * Not a place where a saved value is rewritten because the host disagreed with it. The
//     fallback the engine reports appears beside the saved value as text, and the saved value
//     stays exactly as saved (D-14).
Item {
    id: page

    /// The SeatHubClient facade. The only object this page may talk to (D-35).
    property var client

    readonly property var prefs: client ? client.settings : null

    // The custom width/height fields are the "Custom" state of the one merged Resolution
    // control (`width` + `height`), so they appear only when the pair is neither of the four
    // upstream presets.
    property bool customResolution: false

    // ADR-0042 / D-49: one checkbox (unchecked = never) plus a dropdown reachable only when the
    // checkbox is checked. The stored value is a single key, `capturesyskeys`.
    property bool captureChecked: false
    property string captureWhen: "fullscreen"

    function refreshDerived() {
        if (!prefs)
            return
        customResolution = prefs.resolutionPreset() === "Custom"
        var mode = String(prefs.captureSysKeysMode())
        captureChecked = mode !== "never"
        captureWhen = captureChecked ? mode : "fullscreen"
    }

    Component.onCompleted: refreshDerived()

    Connections {
        target: page.prefs

        function onValueChanged(key) {
            if (key === "width" || key === "height" || key === "capturesyskeys")
                page.refreshDerived()
        }

        function onWritableChanged() {
            page.refreshDerived()
        }
    }

    // --- shared row spacing ------------------------------------------------------------------
    readonly property int rowSpacing: Metrics.s5
    readonly property int sectionSpacing: Metrics.s10

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---------------------------------------------------------------------------- header
        Column {
            Layout.fillWidth: true
            Layout.margins: Metrics.s8
            spacing: Metrics.s2

            Text {
                text: qsTr("Settings")
                color: Tokens.foregroundDefault
                font.family: Tokens.fontDisplayDefault
                font.pixelSize: Metrics.fontH1
                font.weight: Font.DemiBold
            }

            Text {
                width: parent.width
                text: qsTr("Changes are saved immediately and apply to your next stream.")
                wrapMode: Text.Wrap
                color: Tokens.foregroundMutedDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontSm
            }

            // Pitfall 6: during a session nothing here can be changed, and the page says so.
            Text {
                width: parent.width
                visible: page.prefs ? !page.prefs.writable : false
                text: qsTr("You're streaming. These settings can't be changed until the session ends.")
                wrapMode: Text.Wrap
                color: Tokens.warnDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontSm
            }
        }

        // ------------------------------------------------------------------------------ body
        Flickable {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: Metrics.s8
            Layout.rightMargin: Metrics.s8
            contentWidth: width
            contentHeight: body.implicitHeight + Metrics.s8
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            ScrollBar.vertical: ScrollBar {}

            Column {
                id: body
                width: flick.width
                spacing: page.sectionSpacing

                // ============================================================ Video (D-48)
                Column {
                    width: parent.width
                    spacing: page.rowSpacing

                    Text {
                        text: qsTr("Video")
                        color: Tokens.foregroundDefault
                        font.family: Tokens.fontDisplayDefault
                        font.pixelSize: Metrics.fontH2
                        font.weight: Font.DemiBold
                    }

                    // One control over the stored pair `width` + `height`, with the four
                    // presets upstream's CLI shares (--720 --1080 --1440 --4K --resolution).
                    SeatHubSelect {
                        id: resolutionRow
                        bridge: page.prefs
                        title: qsTr("Resolution")
                        description: qsTr("Merges the upstream keys \"width\" and \"height\". Presets 720p, 1080p, 1440p and 4K, or Custom.")
                        optionsOverride: page.prefs ? page.prefs.resolutionPresets() : []
                        valueProvider: function() { return page.prefs ? page.prefs.resolutionPreset() : "" }
                        onEdited: page.prefs.setResolutionPreset(value)
                    }

                    SeatHubNumberField {
                        settingKey: "width"
                        bridge: page.prefs
                        visible: page.customResolution
                        title: qsTr("Custom width")
                        suffix: qsTr("px")
                        minimum: 1
                        maximum: 7680
                    }

                    SeatHubNumberField {
                        settingKey: "height"
                        bridge: page.prefs
                        visible: page.customResolution
                        title: qsTr("Custom height")
                        suffix: qsTr("px")
                        minimum: 1
                        maximum: 4320
                    }

                    // --fps . Upstream warns outside 10-480 FPS (`commandlineparser.cpp:413-417`).
                    SeatHubNumberField {
                        settingKey: "fps"
                        bridge: page.prefs
                        title: qsTr("Frame rate")
                        description: qsTr("10 to 480 FPS.")
                        suffix: qsTr("fps")
                        minimum: 10
                        maximum: 480
                    }

                    // --bitrate . Upstream warns outside 500-500000 Kbps
                    // (`commandlineparser.cpp:421-425`).
                    SeatHubNumberField {
                        settingKey: "bitrate"
                        bridge: page.prefs
                        title: qsTr("Video bitrate")
                        description: qsTr("500 to 500000 Kbps. Lower it on slower connections.")
                        suffix: qsTr("Kbps")
                        minimum: 500
                        maximum: 500000
                    }

                    // --display-mode . One control over `windowmode` and its legacy predecessor
                    // `fullscreen`; upstream resolves the legacy key into `windowmode` on load
                    // (`streamingpreferences.cpp:165-168`), so the two can never disagree here.
                    SeatHubSelect {
                        id: displayModeRow
                        bridge: page.prefs
                        title: qsTr("Display mode")
                        description: qsTr("Merges the upstream keys \"fullscreen\" and \"windowmode\" into one control.")
                        optionsOverride: page.prefs ? page.prefs.displayModes() : []
                        valueProvider: function() { return page.prefs ? page.prefs.displayMode() : "" }
                        onEdited: page.prefs.setDisplayMode(value)
                    }

                    SeatHubToggle {
                        settingKey: "vsync"
                        bridge: page.prefs
                        title: qsTr("V-Sync")
                        description: qsTr("Disabling allows sub-frame latency but can show tearing.")
                    }

                    SeatHubToggle {
                        settingKey: "framepacing"
                        bridge: page.prefs
                        title: qsTr("Frame pacing")
                        description: qsTr("Reduces micro-stutter by delaying early frames.")
                    }

                    SeatHubToggle {
                        settingKey: "hdr"
                        bridge: page.prefs
                        title: qsTr("HDR")
                        description: qsTr("Needs host and graphics support.")
                    }

                    SeatHubToggle {
                        settingKey: "yuv444"
                        bridge: page.prefs
                        title: qsTr("YUV 4:4:4")
                        description: qsTr("Sharper text and colour; needs a strong connection.")
                    }

                    // --video-codec
                    SeatHubSelect {
                        settingKey: "videocfg"
                        bridge: page.prefs
                        title: qsTr("Video codec")
                        description: qsTr("Your saved choice is never rewritten; a fallback is shown beside it.")
                    }

                    // --video-decoder
                    SeatHubSelect {
                        settingKey: "videodec"
                        bridge: page.prefs
                        title: qsTr("Video decoder")
                        description: qsTr("Automatic picks the best decoder this PC supports.")
                    }

                    // --performance-overlay
                    SeatHubToggle {
                        settingKey: "showperfoverlay"
                        bridge: page.prefs
                        title: qsTr("Show performance stats while streaming")
                        description: qsTr("Overlays stream performance information.")
                    }

                    SeatHubToggle {
                        settingKey: "unlockbitrate"
                        bridge: page.prefs
                        title: qsTr("Unlock bitrate limit")
                        description: qsTr("Allows very high bitrates. Best on a wired local network.")
                    }
                }

                // ============================================================ Audio (D-48)
                Column {
                    width: parent.width
                    spacing: page.rowSpacing

                    Text {
                        text: qsTr("Audio")
                        color: Tokens.foregroundDefault
                        font.family: Tokens.fontDisplayDefault
                        font.pixelSize: Metrics.fontH2
                        font.weight: Font.DemiBold
                    }

                    // --audio-config
                    SeatHubSelect {
                        settingKey: "audiocfg"
                        bridge: page.prefs
                        title: qsTr("Audio configuration")
                        description: qsTr("Surround sound needs a matching output device.")
                    }

                    // --audio-on-host
                    SeatHubToggle {
                        settingKey: "hostaudio"
                        bridge: page.prefs
                        title: qsTr("Mute host PC speakers while streaming")
                        description: qsTr("Restart a running game for this to take effect.")
                    }

                    // --mute-on-focus-loss
                    SeatHubToggle {
                        settingKey: "muteonfocusloss"
                        bridge: page.prefs
                        title: qsTr("Mute audio when SeatHub isn't the active window")
                        description: qsTr("Mutes the stream's audio when you switch away.")
                    }
                }

                // ============================================================ Input (D-48)
                Column {
                    width: parent.width
                    spacing: page.rowSpacing

                    Text {
                        text: qsTr("Input")
                        color: Tokens.foregroundDefault
                        font.family: Tokens.fontDisplayDefault
                        font.pixelSize: Metrics.fontH2
                        font.weight: Font.DemiBold
                    }

                    // --game-optimization
                    SeatHubToggle {
                        settingKey: "gameopts"
                        bridge: page.prefs
                        title: qsTr("Optimize game settings for streaming")
                        description: qsTr("Best on a wired local network.")
                    }

                    // --multi-controller
                    SeatHubToggle {
                        settingKey: "multicontroller"
                        bridge: page.prefs
                        title: qsTr("Force gamepad #1 always connected")
                        description: qsTr("Keeps one gamepad connected to the host even with none attached here.")
                    }

                    SeatHubToggle {
                        settingKey: "gamepadmouse"
                        bridge: page.prefs
                        title: qsTr("Mouse control with gamepads")
                        description: qsTr("Hold the Start button to move the pointer.")
                    }

                    // --background-gamepad
                    SeatHubToggle {
                        settingKey: "backgroundgamepad"
                        bridge: page.prefs
                        title: qsTr("Process gamepad input in the background")
                        description: qsTr("Captures gamepad input even when SeatHub isn't focused.")
                    }

                    // --swap-gamepad-buttons
                    SeatHubToggle {
                        settingKey: "swapfacebuttons"
                        bridge: page.prefs
                        title: qsTr("Swap A/B and X/Y gamepad buttons")
                        description: qsTr("Nintendo-style button layout.")
                    }

                    // --absolute-mouse
                    SeatHubToggle {
                        settingKey: "mouseacceleration"
                        bridge: page.prefs
                        title: qsTr("Optimize mouse for remote desktop instead of games")
                        description: qsTr("Seamless pointer control. Doesn't work in most games.")
                    }

                    // --touchscreen-trackpad
                    SeatHubToggle {
                        settingKey: "abstouchmode"
                        bridge: page.prefs
                        title: qsTr("Use touchscreen as a virtual trackpad")
                        description: qsTr("Unchecked, the touchscreen drives the pointer directly.")
                    }

                    // --mouse-buttons-swap
                    SeatHubToggle {
                        settingKey: "swapmousebuttons"
                        bridge: page.prefs
                        title: qsTr("Swap left and right mouse buttons")
                    }

                    // --reverse-scroll-direction
                    SeatHubToggle {
                        settingKey: "reversescroll"
                        bridge: page.prefs
                        title: qsTr("Reverse mouse scrolling direction")
                    }
                }

                // ============================================================ Network (D-48)
                Column {
                    width: parent.width
                    spacing: page.rowSpacing

                    Text {
                        text: qsTr("Network")
                        color: Tokens.foregroundDefault
                        font.family: Tokens.fontDisplayDefault
                        font.pixelSize: Metrics.fontH2
                        font.weight: Font.DemiBold
                    }

                    // --packet-size . 0 means "let the engine decide"; anything else follows
                    // upstream's own rule that it must be at least 1024 bytes.
                    SeatHubNumberField {
                        settingKey: "packetsize"
                        bridge: page.prefs
                        title: qsTr("Packet size")
                        description: qsTr("0 uses the engine's default. Otherwise 1024 bytes or more.")
                        suffix: qsTr("bytes")
                        minimum: 0
                        maximum: 65507
                    }

                    SeatHubToggle {
                        settingKey: "connwarnings"
                        bridge: page.prefs
                        title: qsTr("Show connection quality warnings")
                        description: qsTr("Warnings appear in SeatHub's own words, never the engine's.")
                    }

                    SeatHubToggle {
                        settingKey: "detectnetblocking"
                        bridge: page.prefs
                        title: qsTr("Automatically detect blocked connections")
                        description: qsTr("Checks whether the host's streaming ports are reachable.")
                    }
                }

                // ============================================================ Advanced (D-48)
                Column {
                    width: parent.width
                    spacing: page.rowSpacing

                    Text {
                        text: qsTr("Advanced")
                        color: Tokens.foregroundDefault
                        font.family: Tokens.fontDisplayDefault
                        font.pixelSize: Metrics.fontH2
                        font.weight: Font.DemiBold
                    }

                    // One control over `uidisplaymode` and its legacy predecessor
                    // `startwindowed`, which upstream resolves into it on load
                    // (`streamingpreferences.cpp:169-171`).
                    SeatHubSelect {
                        id: launchModeRow
                        bridge: page.prefs
                        title: qsTr("SeatHub window display mode")
                        description: qsTr("Merges the upstream keys \"startwindowed\" and \"uidisplaymode\" into one control.")
                        optionsOverride: page.prefs ? page.prefs.launchDisplayModes() : []
                        valueProvider: function() { return page.prefs ? page.prefs.launchDisplayMode() : "" }
                        onEdited: page.prefs.setLaunchDisplayMode(value)
                    }

                    // ------------------------------------------------- capture system keys
                    // ADR-0042 / D-49: a checkbox (unchecked = never) plus a dropdown offering
                    // only "In fullscreen" and "Always". Default: checked, "In fullscreen".
                    Column {
                        id: captureRow
                        width: parent.width
                        spacing: Metrics.s1

                        Row {
                            width: parent.width
                            spacing: Metrics.s6

                            Column {
                                width: parent.width - captureToggle.width - Metrics.s6
                                spacing: Metrics.s1

                                Text {
                                    width: parent.width
                                    wrapMode: Text.Wrap
                                    text: qsTr("Capture system keyboard shortcuts")
                                    color: Tokens.foregroundDefault
                                    font.family: Tokens.fontSansDefault
                                    font.pixelSize: Metrics.fontBody
                                }

                                Text {
                                    width: parent.width
                                    wrapMode: Text.Wrap
                                    text: qsTr("Lets combinations like Ctrl+Alt+Del reach the host instead of this PC. Checkbox and dropdown together are the [streamsettings] capturesyskeys key.")
                                    color: Tokens.foregroundMutedDefault
                                    font.family: Tokens.fontSansDefault
                                    font.pixelSize: Metrics.fontSm
                                    lineHeight: 1.4
                                }
                            }

                            CheckBox {
                                id: captureToggle
                                width: Metrics.touchTarget
                                height: Metrics.touchTarget
                                enabled: page.prefs ? page.prefs.writable : false
                                checked: page.captureChecked

                                indicator: Rectangle {
                                    x: (parent.width - width) / 2
                                    y: (parent.height - height) / 2
                                    width: Metrics.s6
                                    height: Metrics.s6
                                    radius: Metrics.radiusXs
                                    color: captureToggle.checked ? Tokens.primaryDefault : "transparent"
                                    border.color: captureToggle.checked ? Tokens.primaryDefault : Tokens.borderStrongDefault
                                    border.width: 1

                                    Text {
                                        anchors.centerIn: parent
                                        visible: captureToggle.checked
                                        text: "\u2713"
                                        color: Tokens.primaryForegroundDefault
                                        font.family: Tokens.fontSansDefault
                                        font.pixelSize: Metrics.fontSm
                                    }
                                }

                                onToggled: {
                                    if (!page.prefs || !page.prefs.writable)
                                        return
                                    if (checked)
                                        page.prefs.setCaptureSysKeysMode(page.captureWhen)
                                    else
                                        page.prefs.setCaptureSysKeysMode("never")
                                    page.refreshDerived()
                                }
                            }
                        }

                        Row {
                            width: parent.width
                            spacing: Metrics.s6
                            visible: page.captureChecked

                            Item {
                                width: parent.width - captureWhenBox.width - Metrics.s6
                                height: 1
                            }

                            ComboBox {
                                id: captureWhenBox
                                width: 280
                                height: Metrics.touchTarget
                                enabled: page.prefs ? page.prefs.writable : false
                                model: [qsTr("In fullscreen"), qsTr("Always")]
                                currentIndex: page.captureWhen === "always" ? 1 : 0
                                font.family: Tokens.fontSansDefault
                                font.pixelSize: Metrics.fontBody

                                background: Rectangle {
                                    radius: Metrics.radiusSm
                                    color: Tokens.surface2Default
                                    border.color: captureWhenBox.activeFocus ? Tokens.focusDefault : Tokens.borderDefault
                                    border.width: captureWhenBox.activeFocus ? 2 : 1
                                }

                                contentItem: Text {
                                    leftPadding: Metrics.s3
                                    rightPadding: captureWhenBox.indicator.width + Metrics.s2
                                    text: captureWhenBox.displayText
                                    elide: Text.ElideRight
                                    verticalAlignment: Text.AlignVCenter
                                    color: Tokens.foregroundDefault
                                    font: captureWhenBox.font
                                }

                                // E9: the popup elides too, so neither dropdown can widen the
                                // flat page at the 960px minimum window width.
                                popup.width: captureWhenBox.width
                                popup.height: Math.min(contentItem.implicitHeight, 320)

                                delegate: ItemDelegate {
                                    id: captureDelegate
                                    width: captureWhenBox.width
                                    text: modelData
                                    font: captureWhenBox.font
                                    highlighted: captureWhenBox.highlightedIndex === index

                                    contentItem: Text {
                                        text: captureDelegate.text
                                        elide: Text.ElideRight
                                        verticalAlignment: Text.AlignVCenter
                                        color: Tokens.foregroundDefault
                                        font: captureWhenBox.font
                                    }

                                    background: Rectangle {
                                        color: captureDelegate.highlighted ? Tokens.surface3Default : Tokens.surface2Default
                                    }
                                }

                                onActivated: {
                                    if (!page.prefs || !page.prefs.writable)
                                        return
                                    page.prefs.setCaptureSysKeysMode(currentIndex === 1 ? "always" : "fullscreen")
                                    page.refreshDerived()
                                }
                            }
                        }

                        // D-50: the consequence of "Always" is recorded, so it is also told.
                        Text {
                            width: parent.width
                            visible: page.captureChecked && page.captureWhen === "always"
                            wrapMode: Text.Wrap
                            text: qsTr("Sending Ctrl+Alt+Del to the host can bring up its secure desktop. The stream can't show that screen, so the session may end.")
                            color: Tokens.warnDefault
                            font.family: Tokens.fontSansDefault
                            font.pixelSize: Metrics.fontSm
                        }
                    }

                    // --keep-awake
                    SeatHubToggle {
                        settingKey: "keepawake"
                        bridge: page.prefs
                        title: qsTr("Keep the display awake while streaming")
                        description: qsTr("Stops the screensaver and display sleep during a session.")
                    }

                    // --------------------------------------------------------- language
                    // `language` is a real stored key and is shown with its current value, but
                    // ADR-0043 retires every language toggle on every surface: Arabic wording
                    // renders inside this same LTR layout and there is no switcher.
                    SeatHubReadOnlyRow {
                        settingKey: "language"
                        bridge: page.prefs
                        title: qsTr("Interface language")
                        description: qsTr("No language toggle in this build (ADR-0043). Arabic wording renders in the same layout.")
                    }

                    // ------------------------------------------------- settings SeatHub owns
                    // D-47: keys SeatHub manages itself are disclosed here with the reason
                    // rather than hidden, so the settings audit stays checkable on screen.
                    Text {
                        width: parent.width
                        text: qsTr("Managed by SeatHub")
                        color: Tokens.foregroundMutedDefault
                        font.family: Tokens.fontSansDefault
                        font.pixelSize: Metrics.fontSm
                        topPadding: Metrics.s4
                    }

                    Repeater {
                        model: page.prefs ? page.prefs.droppedKeys() : []

                        SeatHubReadOnlyRow {
                            required property string modelData

                            width: body.width
                            bridge: page.prefs
                            settingKey: modelData
                            title: page.prefs.labelOf(modelData)
                            description: page.prefs.dropReason(modelData)
                        }
                    }
                }
            }
        }

        // ---------------------------------------------------------------------------- footer
        Item {
            Layout.fillWidth: true
            Layout.margins: Metrics.s8
            implicitHeight: backButton.implicitHeight

            // A real control, not a Text + MouseArea (audit F3): focusable, named for a screen
            // reader, and at least 40px tall.
            SeatHubButton {
                id: backButton
                variant: "ghost"
                text: qsTr("Back")
                anchors.left: parent.left

                onClicked: {
                    if (page.client)
                        page.client.closeSettings()
                }
            }
        }
    }
}
