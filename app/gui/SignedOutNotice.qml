import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// The D-07 window-level signed-out overlay (ADR-0067, the owner's design A-87; `ui.md` §7
// "Signed-out notice"; `screens.md` Common shell).
//
// Parented to the window's own content item the way `NavigableDialog.qml` already is, filling
// it, so it shows over whichever screen is current. Instantiated once in `main.qml`, outside the
// view Loader, beside `ForcedUpdateModal`, so a view change can never take it away mid-notice.
//
// No `Timer` of its own: it shows and hides purely from `client.signedOutNotice`, which the one
// C++ clock (`SeatHubClient::m_signedOutNoticeTimer`, set by a revoked stream or the fallback
// read's own 401) drives - one clock, not two. No icon, no dismiss control and no `MouseArea`,
// so it never takes input from the screen under it (`ui.md` §7: "never blocking").
//
// `visible` lives on `panel` below, not on this root item: a root object with no window of its
// own (as a shell test's bare `QQmlComponent::create()` instantiates it) never settles a binding
// on its OWN `visible` property, the same reason every other screen in this client toggles
// visibility on a child rather than the root it is loaded as (`BalancePill.qml`'s own `content`
// Row is the precedent this follows).
Item {
    id: root

    parent: ApplicationWindow.contentItem
    anchors.fill: parent

    /// The SeatHubClient facade (D-35): `signedOutNotice`.
    property var client

    // `ui.md` §7's Toast placement: top right.
    Rectangle {
        id: panel
        objectName: "signedOutPanel"

        visible: root.client ? root.client.signedOutNotice === true : false

        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: Metrics.s4

        implicitWidth: label.implicitWidth + Metrics.s4 * 2
        implicitHeight: label.implicitHeight + Metrics.s3 * 2
        radius: Metrics.radiusLg
        color: Tokens.surface1Default
        border.width: 1
        border.color: Tokens.borderDefault

        Text {
            id: label
            anchors.centerIn: parent
            text: qsTr("You were signed out.")
            color: Tokens.foregroundDefault
            font.family: Tokens.fontSansDefault
            font.pixelSize: Metrics.fontBody
        }
    }
}
