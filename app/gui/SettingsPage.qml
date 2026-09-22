import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeatHub.Tokens 1.0

// The SeatHub Settings page - Moonlight 6.1.0's own page, section for section and row for row,
// drawn with SeatHub's controls on `ui.md` tokens (Phase 5 D-24, superseding Phase 3's own D-48
// grouping). "Identical" governs information architecture and content - section names and order,
// row labels, input types, options, ranges, numbers and defaults - never styling.
//
// The only differences from upstream's own page are the ones D-25 (as amended by D-25a) allow,
// plus the two deviations D-24 itself requires (the brand substitution, and upstream's hover
// tooltips becoming each row's visible description line, because `ui.md` has no tooltip
// component). A row D-25 removes is not rendered at all here - not disabled, not hidden behind a
// disclosure - and `settings-audit.md` carries every removal with its reason for the owner's
// sign-off (D-47).
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

    // The custom width/height fields are the "Custom" state of the Resolution control (`width` +
    // `height`), so they appear when the pair is neither of the four upstream presets OR when the
    // user has explicitly asked for Custom from the dropdown (see requestedCustomResolution).
    property bool customResolution: false
    // Same pattern for Frame rate: "Custom" means `fps` is neither of upstream's two fixed
    // presets (30, 60), or the user asked for Custom.
    property bool customFrameRate: false

    // "Custom" is a state of the stored pair, not a value the bridge can store (settings_bridge.cpp:
    // setResolutionPreset("Custom") is a deliberate no-op). So a user who picks "Custom" while the
    // stored values still match a preset would otherwise never see the fields — refreshDerived()
    // computed customResolution purely from the pair, which reads as a preset until the user can type
    // a custom value, which they can't because the fields are hidden. These flags carry the user's
    // REQUEST as transient UI state: set true when "Custom" is picked, false when a concrete preset
    // is picked. They deliberately do NOT persist across page reconstruction — if the user never
    // typed a distinct value, nothing custom is stored and showing the preset on reopen is correct.
    property bool requestedCustomResolution: false
    property bool requestedCustomFrameRate: false

    // ADR-0042 / D-49: one checkbox (unchecked = never) plus a dropdown reachable only when the
    // checkbox is checked. The stored value is a single key, `capturesyskeys`.
    property bool captureChecked: false
    property string captureWhen: "fullscreen"

    function refreshDerived() {
        if (!prefs)
            return
        // A genuinely-custom stored pair, or an explicit request, keeps the fields open. The
        // request term is what lets "Custom" work from a preset start and survive editing the pair
        // to preset-matching values (until the user picks a concrete preset, which clears it).
        customResolution = requestedCustomResolution || prefs.resolutionPreset() === "Custom"
        customFrameRate = requestedCustomFrameRate || prefs.frameRatePreset() === "Custom"
        var mode = String(prefs.captureSysKeysMode())
        captureChecked = mode !== "never"
        captureWhen = captureChecked ? mode : "fullscreen"
    }

    Component.onCompleted: refreshDerived()

    Connections {
        target: page.prefs

        function onValueChanged(key) {
            if (key === "width" || key === "height" || key === "fps" || key === "capturesyskeys")
                page.refreshDerived()
        }

        function onWritableChanged() {
            page.refreshDerived()
        }
    }

    // --- shared row spacing ------------------------------------------------------------------
    readonly property int rowSpacing: Metrics.s5
    readonly property int sectionSpacing: Metrics.s10
    readonly property int columnGap: Metrics.s10

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
        // Upstream's own two-column layout (`SettingsView.qml`, D-24): column 1 carries Basic,
        // Audio, Host and UI Settings; column 2 carries Input, Gamepad and Advanced Settings.
        Flickable {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: Metrics.s8
            Layout.rightMargin: Metrics.s8
            contentWidth: width
            contentHeight: Math.max(leftColumn.implicitHeight, rightColumn.implicitHeight) + Metrics.s8
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            ScrollBar.vertical: ScrollBar {}

            readonly property int colWidth: (width - page.columnGap) / 2

            Row {
                id: body
                width: flick.width
                spacing: page.columnGap

                // ================================================= column 1 (upstream order)
                Column {
                    id: leftColumn
                    width: flick.colWidth
                    spacing: page.sectionSpacing

                    // -------------------------------------------------------- Basic Settings
                    Column {
                        width: parent.width
                        spacing: page.rowSpacing

                        Text {
                            text: page.prefs ? page.prefs.groupTitle("basic") : qsTr("Basic Settings")
                            color: Tokens.foregroundDefault
                            font.family: Tokens.fontDisplayDefault
                            font.pixelSize: Metrics.fontH2
                            font.weight: Font.DemiBold
                        }

                        // Upstream's one row "Resolution and FPS" holds two combo boxes;
                        // SeatHub renders Resolution and Frame rate as two rows, each carrying
                        // its half of upstream's description (a SeatHub layout choice, not a
                        // different set of options - `settings-audit.md`).
                        SeatHubSelect {
                            bridge: page.prefs
                            title: qsTr("Resolution")
                            description: qsTr("Half of upstream's \"Resolution and FPS\" row (merges \"width\" and \"height\"). Presets 720p, 1080p, 1440p and 4K, or Custom.")
                            optionsOverride: page.prefs ? page.prefs.resolutionPresets() : []
                            // Display "Custom" whenever the fields are open (requested or derived),
                            // so the dropdown never snaps back to a preset name while a custom value
                            // is being entered.
                            valueProvider: function() { return page.customResolution ? "Custom" : (page.prefs ? page.prefs.resolutionPreset() : "") }
                            onEdited: function(value) {
                                // Record the request BEFORE the bridge write: a concrete preset's
                                // write emits valueChanged synchronously, and refreshDerived() must
                                // already see the cleared flag. The trailing refreshDerived() covers
                                // the "Custom" case, which stores nothing and emits no signal.
                                page.requestedCustomResolution = (value === "Custom")
                                page.prefs.setResolutionPreset(value)
                                page.refreshDerived()
                            }
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

                        SeatHubSelect {
                            title: qsTr("Frame rate")
                            bridge: page.prefs
                            description: qsTr("The other half of upstream's \"Resolution and FPS\" row (merges \"fps\"). 30 FPS, 60 FPS, or Custom.")
                            optionsOverride: page.prefs ? page.prefs.frameRatePresets() : []
                            valueProvider: function() { return page.customFrameRate ? "Custom" : (page.prefs ? page.prefs.frameRatePreset() : "") }
                            onEdited: function(value) {
                                page.requestedCustomFrameRate = (value === "Custom")
                                page.prefs.setFrameRatePreset(value)
                                page.refreshDerived()
                            }
                        }

                        SeatHubNumberField {
                            settingKey: "fps"
                            bridge: page.prefs
                            visible: page.customFrameRate
                            title: qsTr("Custom frame rate")
                            description: qsTr("10 to 480 FPS.")
                            suffix: qsTr("fps")
                            minimum: 10
                            maximum: 480
                        }

                        // --bitrate . Upstream's slider goes 500-150000 Kbps, or 500-500000 while
                        // "Unlock bitrate limit" is on (Advanced Settings) - no new bound.
                        SeatHubNumberField {
                            settingKey: "bitrate"
                            bridge: page.prefs
                            title: qsTr("Video bitrate")
                            description: qsTr("500 Kbps up to the unlock-bitrate ceiling below. Lower it on slower connections.")
                            suffix: qsTr("Kbps")
                            minimum: 500
                            maximum: page.prefs ? page.prefs.bitrateMaximum() : 150000
                            maximumProvider: function() { return page.prefs ? page.prefs.bitrateMaximum() : 150000 }
                            refreshTriggerKeys: ["unlockbitrate"]
                        }

                        // One control over `windowmode` and its legacy predecessor `fullscreen`;
                        // upstream resolves the legacy key into `windowmode` on load.
                        SeatHubSelect {
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
                    }

                    // -------------------------------------------------------- Audio Settings
                    Column {
                        width: parent.width
                        spacing: page.rowSpacing

                        Text {
                            text: page.prefs ? page.prefs.groupTitle("audio") : qsTr("Audio Settings")
                            color: Tokens.foregroundDefault
                            font.family: Tokens.fontDisplayDefault
                            font.pixelSize: Metrics.fontH2
                            font.weight: Font.DemiBold
                        }

                        SeatHubSelect {
                            settingKey: "audiocfg"
                            bridge: page.prefs
                            title: qsTr("Audio configuration")
                            description: qsTr("Surround sound needs a matching output device.")
                        }

                        // D-25a: NOT removed. Stays visible with upstream's own default,
                        // checked (host muted) - a renter's game audio does not play out of the
                        // owner's speakers unless they untick it.
                        SeatHubToggle {
                            settingKey: "hostaudio"
                            bridge: page.prefs
                            title: qsTr("Mute host PC speakers while streaming")
                            description: qsTr("Restart a running game for this to take effect.")
                        }

                        SeatHubToggle {
                            settingKey: "muteonfocusloss"
                            bridge: page.prefs
                            title: qsTr("Mute audio stream when SeatHub is not the active window")
                            description: qsTr("Mutes the stream's audio when you switch away.")
                        }
                    }

                    // --------------------------------------------------------- Host Settings
                    Column {
                        width: parent.width
                        spacing: page.rowSpacing

                        Text {
                            text: page.prefs ? page.prefs.groupTitle("host") : qsTr("Host Settings")
                            color: Tokens.foregroundDefault
                            font.family: Tokens.fontDisplayDefault
                            font.pixelSize: Metrics.fontH2
                            font.weight: Font.DemiBold
                        }

                        SeatHubToggle {
                            settingKey: "gameopts"
                            bridge: page.prefs
                            title: qsTr("Optimize game settings for streaming")
                            description: qsTr("Best on a wired local network.")
                        }

                        // "Quit app on host PC after ending stream" (D-25(c)) is not rendered:
                        // SeatHub owns the one-session teardown and forces this off on every load.
                    }

                    // ----------------------------------------------------------- UI Settings
                    Column {
                        width: parent.width
                        spacing: page.rowSpacing

                        Text {
                            text: page.prefs ? page.prefs.groupTitle("ui") : qsTr("UI Settings")
                            color: Tokens.foregroundDefault
                            font.family: Tokens.fontDisplayDefault
                            font.pixelSize: Metrics.fontH2
                            font.weight: Font.DemiBold
                        }

                        // "Language" (D-25(b)) is not rendered: forced to English on every load.
                        // ADR-0043 retires every language toggle on every surface.

                        // One control over `uidisplaymode` and its legacy predecessor
                        // `startwindowed`.
                        SeatHubSelect {
                            bridge: page.prefs
                            title: qsTr("GUI display mode")
                            description: qsTr("Merges the upstream keys \"startwindowed\" and \"uidisplaymode\" into one control.")
                            optionsOverride: page.prefs ? page.prefs.launchDisplayModes() : []
                            valueProvider: function() { return page.prefs ? page.prefs.launchDisplayMode() : "" }
                            onEdited: page.prefs.setLaunchDisplayMode(value)
                        }

                        // OD-05: stays visible with upstream's own default. SeatHub's overlay
                        // re-asserts itself after the engine writes status text, so a low-balance
                        // warning is never blanked (Plan 10).
                        SeatHubToggle {
                            settingKey: "connwarnings"
                            bridge: page.prefs
                            title: qsTr("Show connection quality warnings")
                            description: qsTr("Warnings appear in SeatHub's own words, never the engine's.")
                        }

                        // "Discord Rich Presence integration" (D-25(b)) is not rendered: forced
                        // off on every load. SeatHub has no external presence surface.

                        SeatHubToggle {
                            settingKey: "keepawake"
                            bridge: page.prefs
                            title: qsTr("Keep the display awake while streaming")
                            description: qsTr("Stops the screensaver and display sleep during a session.")
                        }
                    }
                }

                // ================================================= column 2 (upstream order)
                Column {
                    id: rightColumn
                    width: flick.colWidth
                    spacing: page.sectionSpacing

                    // -------------------------------------------------------- Input Settings
                    Column {
                        width: parent.width
                        spacing: page.rowSpacing

                        Text {
                            text: page.prefs ? page.prefs.groupTitle("input") : qsTr("Input Settings")
                            color: Tokens.foregroundDefault
                            font.family: Tokens.fontDisplayDefault
                            font.pixelSize: Metrics.fontH2
                            font.weight: Font.DemiBold
                        }

                        SeatHubToggle {
                            settingKey: "mouseacceleration"
                            bridge: page.prefs
                            title: qsTr("Optimize mouse for remote desktop instead of games")
                            description: qsTr("Seamless pointer control. Doesn't work in most games.")
                        }

                        // ------------------------------------------------- capture system keys
                        // ADR-0042 / D-49: a checkbox (unchecked = never) plus a dropdown
                        // offering only "In fullscreen" and "Always". SeatHub's own default
                        // (checked, "In fullscreen") differs from upstream's own default
                        // (never) - a third authorized deviation, recorded in the settings
                        // audit alongside the brand substitution and the tooltip move.
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
                                            text: "✓"
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

                                    popup.width: captureWhenBox.width
                                    // BUG (combobox-popup-collapsed): unqualified `contentItem`
                                    // resolved to this ComboBox's own one-line display Text, not
                                    // the popup's ListView - see SeatHubSelect.qml for the full
                                    // mechanism (this is a second, hand-copied instance of the
                                    // same override pattern).
                                    popup.height: Math.min(popup.contentItem.implicitHeight
                                                           + popup.topPadding + popup.bottomPadding, 320)

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

                        SeatHubToggle {
                            settingKey: "abstouchmode"
                            bridge: page.prefs
                            title: qsTr("Use touchscreen as a virtual trackpad")
                            description: qsTr("Unchecked, the touchscreen drives the pointer directly.")
                        }

                        SeatHubToggle {
                            settingKey: "swapmousebuttons"
                            bridge: page.prefs
                            title: qsTr("Swap left and right mouse buttons")
                        }

                        SeatHubToggle {
                            settingKey: "reversescroll"
                            bridge: page.prefs
                            title: qsTr("Reverse mouse scrolling direction")
                        }
                    }

                    // ------------------------------------------------------ Gamepad Settings
                    Column {
                        width: parent.width
                        spacing: page.rowSpacing

                        Text {
                            text: page.prefs ? page.prefs.groupTitle("gamepad") : qsTr("Gamepad Settings")
                            color: Tokens.foregroundDefault
                            font.family: Tokens.fontDisplayDefault
                            font.pixelSize: Metrics.fontH2
                            font.weight: Font.DemiBold
                        }

                        SeatHubToggle {
                            settingKey: "swapfacebuttons"
                            bridge: page.prefs
                            title: qsTr("Swap A/B and X/Y gamepad buttons")
                            description: qsTr("Nintendo-style button layout.")
                        }

                        SeatHubToggle {
                            settingKey: "multicontroller"
                            bridge: page.prefs
                            title: qsTr("Force gamepad #1 always connected")
                            description: qsTr("Keeps one gamepad connected to the host even with none attached here.")
                        }

                        SeatHubToggle {
                            settingKey: "gamepadmouse"
                            bridge: page.prefs
                            title: qsTr("Enable mouse control with gamepads by holding the 'Start' button")
                        }

                        SeatHubToggle {
                            settingKey: "backgroundgamepad"
                            bridge: page.prefs
                            title: qsTr("Process gamepad input when SeatHub is in the background")
                            description: qsTr("Captures gamepad input even when SeatHub isn't focused.")
                        }
                    }

                    // ----------------------------------------------------- Advanced Settings
                    Column {
                        width: parent.width
                        spacing: page.rowSpacing

                        Text {
                            text: page.prefs ? page.prefs.groupTitle("advanced") : qsTr("Advanced Settings")
                            color: Tokens.foregroundDefault
                            font.family: Tokens.fontDisplayDefault
                            font.pixelSize: Metrics.fontH2
                            font.weight: Font.DemiBold
                        }

                        SeatHubSelect {
                            settingKey: "videodec"
                            bridge: page.prefs
                            title: qsTr("Video decoder")
                            description: qsTr("Automatic picks the best decoder this PC supports.")
                        }

                        SeatHubSelect {
                            settingKey: "videocfg"
                            bridge: page.prefs
                            title: qsTr("Video codec")
                            description: qsTr("Your saved choice is never rewritten; a fallback is shown beside it.")
                        }

                        SeatHubToggle {
                            settingKey: "hdr"
                            bridge: page.prefs
                            title: qsTr("Enable HDR (Experimental)")
                            description: qsTr("Needs host and graphics support.")
                        }

                        SeatHubToggle {
                            settingKey: "yuv444"
                            bridge: page.prefs
                            title: qsTr("Enable YUV 4:4:4 (Experimental)")
                            description: qsTr("Sharper text and colour; needs a strong connection.")
                        }

                        SeatHubToggle {
                            settingKey: "unlockbitrate"
                            bridge: page.prefs
                            title: qsTr("Unlock bitrate limit (Experimental)")
                            description: qsTr("Allows very high bitrates. Best on a wired local network.")
                        }

                        // "Automatically find PCs on the local network (Recommended)" and
                        // "Automatically detect blocked connections (Recommended)" (D-25(b)/(c))
                        // are not rendered: both forced off on every load.

                        // D-23/CUST-17: "Show performance stats while streaming" becomes a
                        // group - the upstream label as the group's own title, and one toggle
                        // per stats value indented under it, each labelled with Moonlight's own
                        // line text (`copy.md` § Settings). All default off, and all off is a
                        // valid state (OD-03); the underlying option follows automatically.
                        Column {
                            width: parent.width
                            spacing: Metrics.s3

                            Text {
                                width: parent.width
                                wrapMode: Text.Wrap
                                text: qsTr("Show performance stats while streaming")
                                color: Tokens.foregroundDefault
                                font.family: Tokens.fontSansDefault
                                font.pixelSize: Metrics.fontBody
                            }

                            Column {
                                width: parent.width
                                leftPadding: Metrics.s6
                                spacing: Metrics.s3

                                Repeater {
                                    model: page.prefs ? page.prefs.statsToggleKeys() : []

                                    Row {
                                        id: statRow
                                        required property string modelData
                                        readonly property string statKey: modelData

                                        width: parent.width - Metrics.s6
                                        spacing: Metrics.s6

                                        Text {
                                            width: parent.width - statToggle.width - Metrics.s6
                                            wrapMode: Text.Wrap
                                            text: page.prefs ? page.prefs.statsToggleLabel(statRow.statKey) : ""
                                            color: Tokens.foregroundDefault
                                            font.family: Tokens.fontSansDefault
                                            font.pixelSize: Metrics.fontBody
                                        }

                                        CheckBox {
                                            id: statToggle
                                            width: Metrics.touchTarget
                                            height: Metrics.touchTarget
                                            enabled: page.prefs ? page.prefs.writable : false
                                            checked: page.prefs ? page.prefs.getStatsToggle(statRow.statKey) : false

                                            indicator: Rectangle {
                                                x: (parent.width - width) / 2
                                                y: (parent.height - height) / 2
                                                width: Metrics.s6
                                                height: Metrics.s6
                                                radius: Metrics.radiusXs
                                                color: statToggle.checked ? Tokens.primaryDefault : "transparent"
                                                border.color: statToggle.checked ? Tokens.primaryDefault : Tokens.borderStrongDefault
                                                border.width: 1

                                                Text {
                                                    anchors.centerIn: parent
                                                    visible: statToggle.checked
                                                    text: "✓"
                                                    color: Tokens.primaryForegroundDefault
                                                    font.family: Tokens.fontSansDefault
                                                    font.pixelSize: Metrics.fontSm
                                                }
                                            }

                                            onToggled: {
                                                if (!page.prefs || !page.prefs.writable)
                                                    return
                                                if (!page.prefs.setStatsToggle(statRow.statKey, checked))
                                                    checked = page.prefs.getStatsToggle(statRow.statKey)
                                            }
                                        }
                                    }
                                }
                            }
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
            // reader, and at least 40px tall. The way back Task 2 asks for.
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
