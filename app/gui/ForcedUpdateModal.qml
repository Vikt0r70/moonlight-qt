import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// The forced-update modal (D-38, D-41, D-42, D-43).
//
// ADR-0070 / screens.md §49: required updates remain blocking. A newer release above its floor
// is optional and can be dismissed for this launch; a failed optional update renders nothing.
//
// It is never shown while a stream is running (Pitfall 8): `SeatHubClient` refuses to check,
// refuse to download, and refuse to install during a session, and this file renders nothing
// while `updates.blockedBySession` is true. An offer that arrives mid-session is offered again
// the moment the session ends.
//
// The installer is unsigned (D-43). Windows shows its own SmartScreen warning when it runs, and
// the per-machine install raises a UAC prompt (D-42); both are expected and neither is a bug.
Item {
    id: modal

    /// `SeatHubClient.updates`.
    property var updates: null

    readonly property var offer: updates !== null && updates !== undefined
                                 ? updates.availableUpdate : null
    readonly property string offeredVersion: {
        if (offer === null || offer === undefined || offer.version === undefined)
            return ""
        return String(offer.version)
    }
    readonly property bool hasOffer: offeredVersion.length > 0
    // D-41 + Pitfall 8: nothing while a stream is running.
    readonly property bool requiredUpdate: hasOffer && updates.mandatory === true
    readonly property bool exhausted: requiredUpdate && updates.exhausted === true
    readonly property bool shown: hasOffer && (requiredUpdate || updates.optionalOffer === true)
                                 && updates.blockedBySession !== true
    // `ready` is a verified download on its way to `installing`: the client starts the installer
    // by itself, because the one press that began the download is the only one there is (D-41).
    readonly property bool busy: updates !== null && updates !== undefined
                                 && (updates.state === "downloading"
                                     || updates.state === "verifying"
                                     || updates.state === "ready"
                                     || updates.state === "installing")

    anchors.fill: parent
    visible: shown
    z: 1000
    focus: shown

    // ui.md §3.1 has no scrim token, so this is the one untokenised colour in the shell
    // (audit F19). It is named once here rather than written inline, so it cannot drift into a
    // second value; a `--scrim` token in ui.md is the fix that would retire it.
    readonly property color scrimColor: Qt.rgba(0, 0, 0, 0.72)

    // Nothing behind this may be clicked, scrolled or dragged while it is up.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        onWheel: function(wheel) { wheel.accepted = true }
    }

    // Nor typed into, nor tabbed out of - with one exception, because swallowing every key
    // also swallowed Tab and left `Update` unreachable by keyboard (audit F4). Every key is
    // still refused; Tab is answered by moving the focus ring between this card's root and
    // its single action, so the ring cannot escape to the page behind. There is still no
    // dismissal path of any kind (D-41).
    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
            if (updateAction.enabled)
                updateAction.forceActiveFocus()
            event.accepted = true
            return
        }
        event.accepted = true
    }

    onShownChanged: {
        if (shown) {
            if (updateAction.enabled)
                updateAction.forceActiveFocus()
            else
                modal.forceActiveFocus()
        }
    }

    Rectangle {
        anchors.fill: parent
        color: modal.scrimColor
    }

    Rectangle {
        id: card

        anchors.centerIn: parent
        width: Math.min(parent.width - Metrics.s16, 480)
        implicitHeight: content.implicitHeight + Metrics.s8 * 2
        height: implicitHeight
        radius: Metrics.radiusLg
        color: Tokens.surface1Default
        border.color: Tokens.borderDefault
        border.width: 1

        Column {
            id: content
            anchors.fill: parent
            anchors.margins: Metrics.s8
            spacing: Metrics.s3

            Text {
                width: parent.width
                text: modal.requiredUpdate ? qsTr("Update SeatHub") : qsTr("SeatHub update")
                wrapMode: Text.Wrap
                color: Tokens.foregroundDefault
                font.family: Tokens.fontDisplayDefault
                font.pixelSize: Metrics.fontH2
                font.weight: Font.DemiBold
            }

            // The version, always in mono with tabular figures like every other number in the
            // product (`docs/spec/ui.md` §4).
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: {
                    // Exact frozen copy: copy.md §Update window, ADR-0070 items 8/9.
                    if (!modal.requiredUpdate)
                        return qsTr("Version %1 is ready. Update now, or later.").arg(modal.offeredVersion)
                    if (modal.exhausted)
                        return qsTr("SeatHub could not update itself. Download the new version and install it to keep playing.")
                    if (updates && updates.state === "failed")
                        return qsTr("SeatHub could not finish updating, so playing is paused until it does. It will try again %1.").arg(updates.nextAttemptText)
                    if (modal.hasOffer && modal.offer.rollback === true)
                        return qsTr("SevenHills has set version %1 for this app on your PC.").arg(modal.offeredVersion)
                    return qsTr("Version %1 is available. SeatHub has to restart to finish updating.").arg(modal.offeredVersion)
                }
                color: Tokens.foregroundMutedDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontBody
            }

            Text {
                width: parent.width
                visible: modal.hasOffer && modal.offer.notes !== undefined
                         && String(modal.offer.notes).length > 0
                wrapMode: Text.Wrap
                text: modal.hasOffer ? String(modal.offer.notes) : ""
                color: Tokens.foregroundMutedDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontSm
            }

            // Progress. Mono, tabular figures, and never the only signal - the percentage is
            // written out as well as drawn (ui.md §9).
            Column {
                width: parent.width
                spacing: Metrics.s1
                visible: modal.busy

                // "Starting the installer" only while the launch is really being attempted
                // (`installing`); a verified download that has not been launched yet still reads
                // as the finished check, so the copy never claims a launch that has not happened.
                Text {
                    text: updates && updates.state === "installing"
                          ? qsTr("Starting the installer\u2026")
                          : (updates && (updates.state === "verifying" || updates.state === "ready")
                             ? qsTr("Checking the download: %1%").arg(updates.progress)
                             : qsTr("Downloading: %1%").arg(updates ? updates.progress : 0))
                    color: Tokens.foregroundMutedDefault
                    font.family: Tokens.fontMonoDefault
                    font.pixelSize: Metrics.fontSm
                    font.kerning: false
                    font.features: { "tnum": 1 }
                }

                Rectangle {
                    width: parent.width
                    height: Metrics.s1
                    radius: Metrics.radiusXs
                    color: Tokens.surface3Default

                    Rectangle {
                        width: parent.width * Math.max(0, Math.min(100, updates ? updates.progress : 0)) / 100
                        height: parent.height
                        radius: Metrics.radiusXs
                        color: Tokens.primaryDefault
                    }
                }
            }

            // A failure here is shown with its support reference (`copy.md` §Support & errors).
            Column {
                width: parent.width
                spacing: Metrics.s1
                visible: updates !== null && updates !== undefined && updates.state === "failed"
                         && updates.failure !== undefined
                         && String(updates.failure.error ? updates.failure.error : "").length > 0

                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: updates && updates.failure && updates.failure.error
                          ? String(updates.failure.error) : ""
                    color: Tokens.destructiveDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontSm
                }

                Text {
                    visible: updates && updates.failure && String(updates.failure.reference).length > 0
                    text: updates && updates.failure ? String(updates.failure.reference) : ""
                    color: Tokens.foregroundMutedDefault
                    font.family: Tokens.fontMonoDefault
                    font.pixelSize: Metrics.fontSm
                    font.kerning: false
                    font.features: { "tnum": 1 }
                }
            }

            Row {
                width: parent.width
                spacing: Metrics.s3
                SeatHubButton {
                id: updateAction
                objectName: "updateAction"

                width: secondaryAction.active ? (parent.width - parent.spacing) / 2 : parent.width
                enabled: updates !== null && updates !== undefined && modal.busy === false
                text: updates && updates.state === "failed" ? qsTr("Try again") : qsTr("Update")

                // Tab must not leave the card (audit F4). Space and Enter still activate the
                // button through Qt's own handling; only the tab keys are answered here.
                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
                        if (secondaryAction.item && secondaryAction.item.enabled)
                            secondaryAction.item.forceActiveFocus()
                        else
                            modal.forceActiveFocus()
                        event.accepted = true
                    }
                    if (event.key === Qt.Key_Escape)
                        event.accepted = true
                }

                onClicked: {
                    if (updates === null || updates === undefined)
                        return
                    if (updates.state === "failed" || modal.exhausted) {
                        updates.tryAgain()
                    }
                    else if (updates.state === "available") {
                        // The digest the offer carries is the one baked in from the release pin
                        // at release prep. When it is empty the client refuses to install and
                        // says so, rather than running an unverified binary (D-43). A verified
                        // download goes on to launch the installer by itself.
                        updates.downloadUpdate(String(updates.availableUpdate.url),
                                               String(updates.availableUpdate.sha256))
                    }
                }
                }
                Loader {
                    id: secondaryAction
                    width: (parent.width - parent.spacing) / 2
                    active: modal.hasOffer && (modal.exhausted || updates.optionalOffer === true)
                    sourceComponent: Component {
                        SeatHubButton {
                            width: secondaryAction.width
                            variant: "ghost"
                            objectName: modal.exhausted ? "downloadAction" : "notNowAction"
                            enabled: !modal.busy
                            text: modal.exhausted ? qsTr("Download") : qsTr("Not now")
                            Keys.onPressed: function(event) {
                                if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
                                    updateAction.forceActiveFocus()
                                    event.accepted = true
                                }
                                if (event.key === Qt.Key_Escape)
                                    event.accepted = true
                            }
                            onClicked: {
                                if (modal.exhausted)
                                    updates.openManualDownload()
                                else
                                    updates.dismissOptional()
                            }
                        }
                    }
                }
            }
        }
    }
}
