import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// The customer's own page: who they are, what they have played, what is left, and the three short
// histories behind those numbers (`docs/spec/screens.md` §27, `05-UI-SPEC` "Profile"; CUST-08,
// CUST-14, D-13 to D-17).
//
// Read-only. Two columns under the signed-in header (`AppHeader`, wired once in `main.qml`):
//
//   left, 320px   the title, three identity rows (username, email with its mark, phone number), the
//                 plain website link `Open the website` (owner answer OD-11: it promises nothing, so it
//                 is not worded as an instruction to change details there), the two totals, and Back and
//                 Sign out at the bottom.
//   right         three tabs over one list region that scrolls inside itself. The page does not scroll.
//
// It talks to the facade and to nothing else (D-35), and it draws what the facade gives it. Every
// number is the server's own answer, formatted in C++ by the one duration formatter: nothing here adds,
// counts or infers, and no row can name a rig because nothing it is given does (CUST-01). Each of the
// three lists, the two totals and the identity block loads, fails and is retried on its own: one
// failing never blanks another (D-14).
//
// The identity block has its own states: skeleton rows after 300 ms, the server's sentence with its
// reference and `Try again`, or the rows. A missing email or phone keeps its row and reads `Not added`.
// Long values wrap inside the column instead of being cut off. There is no unverified-email banner in
// the client (D-17): the mark says `Verified` or `Unverified`, in a word as well as a colour, and
// nothing blocks on it.
//
// Two deviations from the picture in the design contract, both forced by the numbers on the page:
//   * the totals sit side by side only where both fit. At 320px two 32px mono values do not, so they
//     stack, and stay one above the other until the column is wide enough.
//   * the left column scrolls when the window is at its smallest, so Sign out is always reachable;
//     at the default size nothing scrolls.
// `Back` is here because a customer who opened the profile needs a way out of it that is not the menu.
Item {
    id: root

    objectName: "profileScreen"

    /// The SeatHubClient facade. The only object this screen may talk to (D-35).
    property var client: null

    /// How long a load may take before its skeleton shows (a fast answer never flashes one).
    property int loadingDelay: 300

    /// The selected list.
    property int currentTab: 0

    // The three lists, in tab order: the facade's name for each, and the copy deck's words for it
    // (`copy.md` § Menu and profile).
    readonly property var lists: [
        { key: "sessions", title: qsTr("Sessions"),
          loading: qsTr("Loading your sessions…"), empty: qsTr("No sessions yet."),
          action: qsTr("Back to home") },
        { key: "credit", title: qsTr("Credit history"),
          loading: qsTr("Loading your credit history…"), empty: qsTr("No credit history yet."),
          action: qsTr("Top up") },
        { key: "topups", title: qsTr("Top-ups"),
          loading: qsTr("Loading your top-ups…"), empty: qsTr("No top-ups yet."),
          action: qsTr("Top up") }
    ]

    readonly property var account: root.client && root.client.account ? root.client.account : ({})
    readonly property string accountStatus: root.client && root.client.accountStatus
                                            ? String(root.client.accountStatus) : "loading"
    readonly property string totalsStatus: root.client && root.client.totalsStatus
                                           ? String(root.client.totalsStatus) : "loading"

    readonly property int leftWidth: 320

    function listFor(index) {
        if (!root.client)
            return null
        switch (index) {
        case 0: return root.client.sessionHistory
        case 1: return root.client.creditHistory
        default: return root.client.topupHistory
        }
    }

    // A list is asked for when its tab is first shown. The facade asks once and ignores the rest, so
    // switching back to a tab that is already loaded costs nothing.
    function loadTab(index) {
        if (root.client && index >= 0 && index < root.lists.length)
            root.client.loadFirstPage(root.lists[index].key)
    }

    onCurrentTabChanged: root.loadTab(root.currentTab)

    // The 300 ms before a skeleton shows, for the identity block and the totals.
    property bool identityPastDelay: false
    property bool totalsPastDelay: false

    readonly property bool identityLoading: root.accountStatus === "loading"
    readonly property bool totalsLoading: root.totalsStatus === "loading"
                                          || root.totalsStatus === "idle"

    function restartIdentityDelay() {
        root.identityPastDelay = root.identityLoading && root.loadingDelay <= 0
        if (root.identityLoading && root.loadingDelay > 0)
            identityTimer.restart()
        else
            identityTimer.stop()
    }
    function restartTotalsDelay() {
        root.totalsPastDelay = root.totalsLoading && root.loadingDelay <= 0
        if (root.totalsLoading && root.loadingDelay > 0)
            totalsTimer.restart()
        else
            totalsTimer.stop()
    }
    onIdentityLoadingChanged: root.restartIdentityDelay()
    onTotalsLoadingChanged: root.restartTotalsDelay()
    Component.onCompleted: {
        root.restartIdentityDelay()
        root.restartTotalsDelay()
        root.loadTab(root.currentTab)
    }

    Timer {
        id: identityTimer
        interval: root.loadingDelay
        onTriggered: root.identityPastDelay = root.identityLoading
    }
    Timer {
        id: totalsTimer
        interval: root.loadingDelay
        onTriggered: root.totalsPastDelay = root.totalsLoading
    }

    // A label above its value: the label is the small uppercase eyebrow, the value is body text that
    // wraps anywhere rather than being cut, and a missing value reads `Not added` in the muted colour.
    // A mark (`verified` or `unverified`) sits beside the value where it fits and under it where it
    // does not: a dot in the mark's colour and always its word.
    component IdentityRow: Column {
        id: identityRow

        /// The label, the value ("" is a missing one), whether the value is a number (mono), and the
        /// mark, if any.
        property string label: ""
        property string value: ""
        property bool mono: false
        property string mark: ""

        readonly property bool missing: identityRow.value.length === 0

        width: parent ? parent.width : 0
        spacing: Metrics.s1

        Text {
            objectName: "label"
            text: identityRow.label
            color: Tokens.foregroundMutedDefault
            font.family: Tokens.fontSansDefault
            font.pixelSize: Metrics.fontLabel
            font.weight: Font.DemiBold
            font.capitalization: Font.AllUppercase
            font.letterSpacing: Metrics.fontLabel * 0.08
        }

        Item {
            id: valueBlock

            width: parent.width
            height: beside ? valueText.height
                           : valueText.height + (markRow.visible ? Metrics.s1 + markRow.height : 0)

            readonly property bool beside: markRow.visible
                                           && metrics.advanceWidth + Metrics.s2 + markRow.width <= width

            TextMetrics {
                id: metrics
                font: valueText.font
                text: valueText.text
            }

            Text {
                id: valueText
                objectName: "value"
                width: valueBlock.beside ? metrics.advanceWidth : valueBlock.width
                text: identityRow.missing ? qsTr("Not added") : identityRow.value
                // A long value wraps at any character: an address with no spaces must not run off the
                // column or be cut short.
                wrapMode: Text.WrapAnywhere
                color: identityRow.missing ? Tokens.foregroundMutedDefault : Tokens.foregroundDefault
                font.family: identityRow.mono && !identityRow.missing ? Tokens.fontMonoDefault
                                                                       : Tokens.fontSansDefault
                font.pixelSize: Metrics.fontBody
            }

            Row {
                id: markRow
                objectName: "mark"
                visible: identityRow.mark.length > 0 && !identityRow.missing
                spacing: Metrics.s1
                x: valueBlock.beside ? metrics.advanceWidth + Metrics.s2 : 0
                y: valueBlock.beside ? (valueText.height - height) / 2 : valueText.height + Metrics.s1

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Metrics.s2
                    height: Metrics.s2
                    radius: Metrics.s1
                    color: identityRow.mark === "verified" ? Tokens.successDefault : Tokens.warnDefault
                }

                Text {
                    objectName: "markWord"
                    anchors.verticalCenter: parent.verticalCenter
                    text: identityRow.mark === "verified" ? qsTr("Verified") : qsTr("Unverified")
                    color: Tokens.foregroundMutedDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontSm
                }
            }
        }
    }

    // One total: a small uppercase label over the value in the mono face, 32px.
    component TotalTile: Rectangle {
        id: tile

        property string label: ""
        property string value: ""

        implicitHeight: tileColumn.implicitHeight + Metrics.s4 * 2
        radius: Metrics.radiusMd
        color: Tokens.surface1Default
        border.width: 1
        border.color: Tokens.borderDefault

        Column {
            id: tileColumn
            x: Metrics.s4
            y: Metrics.s4
            width: parent.width - Metrics.s4 * 2
            spacing: Metrics.s1

            Text {
                objectName: "tileLabel"
                text: tile.label
                color: Tokens.foregroundMutedDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontLabel
                font.weight: Font.DemiBold
                font.capitalization: Font.AllUppercase
                font.letterSpacing: Metrics.fontLabel * 0.08
            }

            Text {
                objectName: "tileValue"
                width: parent.width
                text: tile.value
                // The value is a number: it shrinks to fit rather than being cut short.
                fontSizeMode: Text.HorizontalFit
                minimumPixelSize: Metrics.fontBody
                wrapMode: Text.NoWrap
                color: Tokens.foregroundDefault
                font.family: Tokens.fontMonoDefault
                font.pixelSize: Metrics.fontH1
                font.weight: Font.DemiBold
                font.features: { "tnum": 1 }
            }
        }
    }

    // --- left column ------------------------------------------------------------------------------

    Flickable {
        id: leftScroll

        objectName: "leftColumn"
        x: Metrics.s8
        y: Metrics.s8
        width: root.leftWidth
        height: root.height - Metrics.s8 * 2
        contentWidth: width
        contentHeight: leftColumn.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        // Only at the smallest window does the column not fit; then it scrolls and nothing is lost.
        ScrollBar.vertical: ScrollBar {
            policy: leftScroll.contentHeight > leftScroll.height ? ScrollBar.AlwaysOn
                                                                 : ScrollBar.AlwaysOff
        }

        Column {
            id: leftColumn

            width: leftScroll.width
            spacing: Metrics.s6

            Text {
                objectName: "profileTitle"
                width: parent.width
                text: qsTr("Profile")
                color: Tokens.foregroundDefault
                font.family: Tokens.fontDisplayDefault
                font.pixelSize: Metrics.fontH1
                font.weight: Font.DemiBold
            }

            // The identity block: skeleton rows, the server's sentence, or the three rows.
            Item {
                id: identityBlock

                objectName: "identityBlock"
                width: parent.width
                height: root.accountStatus === "ready" ? identityRows.implicitHeight
                        : (root.accountStatus === "error" ? identityError.implicitHeight
                                                          : identitySkeleton.implicitHeight)

                Column {
                    id: identityRows

                    objectName: "identityRows"
                    width: parent.width
                    spacing: Metrics.s4
                    visible: root.accountStatus === "ready"

                    IdentityRow {
                        objectName: "rowUsername"
                        label: qsTr("Username")
                        value: root.account.username ? String(root.account.username) : ""
                    }

                    IdentityRow {
                        objectName: "rowEmail"
                        label: qsTr("Email")
                        value: root.account.email ? String(root.account.email) : ""
                        mark: !value.length ? ""
                              : (root.account.email_verified === true ? "verified" : "unverified")
                    }

                    IdentityRow {
                        objectName: "rowPhone"
                        label: qsTr("Phone number")
                        value: root.account.phone ? String(root.account.phone) : ""
                        mono: true
                    }
                }

                // Loading, past the delay: three skeletons of the final row shape.
                Column {
                    id: identitySkeleton

                    objectName: "identityLoading"
                    width: parent.width
                    spacing: Metrics.s4
                    visible: root.accountStatus === "loading" && root.identityPastDelay

                    Repeater {
                        model: 3

                        delegate: Column {
                            width: identitySkeleton.width
                            spacing: Metrics.s1

                            Rectangle {
                                width: Metrics.s16
                                height: Metrics.s3
                                radius: Metrics.radiusXs
                                color: Tokens.surface3Default
                            }

                            Rectangle {
                                width: parent.width * 0.7
                                height: Metrics.s5
                                radius: Metrics.radiusXs
                                color: Tokens.surface3Default
                            }
                        }
                    }
                }

                // Error: the server's sentence, its reference and a way to try again. The rest of the
                // page is unaffected.
                Column {
                    id: identityError

                    objectName: "identityError"
                    width: parent.width
                    spacing: Metrics.s2
                    visible: root.accountStatus === "error"

                    Text {
                        objectName: "identityErrorText"
                        width: parent.width
                        text: "◆ " + (root.client && root.client.accountError
                                       ? String(root.client.accountError) : "")
                        wrapMode: Text.Wrap
                        color: Tokens.destructiveDefault
                        font.family: Tokens.fontSansDefault
                        font.pixelSize: Metrics.fontBody
                    }

                    Text {
                        objectName: "identityErrorReference"
                        visible: text.length > 0
                        text: root.client && root.client.accountErrorReference
                              ? String(root.client.accountErrorReference) : ""
                        wrapMode: Text.NoWrap
                        color: Tokens.foregroundDefault
                        font.family: Tokens.fontMonoDefault
                        font.pixelSize: Metrics.fontSm
                    }

                    SeatHubButton {
                        objectName: "identityRetry"
                        variant: "ghost"
                        text: qsTr("Try again")
                        onClicked: root.client.reloadAccount()
                    }
                }
            }

            // A plain link that opens the website's home page. It promises nothing (OD-11).
            SeatHubButton {
                objectName: "openWebsiteButton"
                variant: "ghost"
                text: qsTr("Open the website")
                glyph: "↗"

                onClicked: root.client.openWebsite("home")
            }

            // The two totals: the server's numbers, formatted and nothing more. Side by side where
            // both fit, one above the other where they do not.
            Item {
                id: totalsBlock

                objectName: "totalsBlock"
                width: parent.width
                height: root.totalsStatus === "error" ? totalsError.implicitHeight
                        : (tiles.visible ? tiles.height : skeletonTiles.height)

                TextMetrics {
                    id: hoursMetrics
                    font.family: Tokens.fontMonoDefault
                    font.pixelSize: Metrics.fontH1
                    font.weight: Font.DemiBold
                    text: root.client && root.client.hoursPlayedText ? String(root.client.hoursPlayedText) : ""
                }

                TextMetrics {
                    id: creditMetrics
                    font: hoursMetrics.font
                    text: root.client && root.client.creditLeftText ? String(root.client.creditLeftText) : ""
                }

                readonly property bool sideBySide:
                    (Math.max(hoursMetrics.advanceWidth, creditMetrics.advanceWidth)
                     + Metrics.s4 * 2) * 2 + Metrics.s4 <= width
                readonly property int tileWidth: sideBySide ? (width - Metrics.s4) / 2 : width

                Flow {
                    id: tiles

                    objectName: "totalTiles"
                    width: parent.width
                    spacing: Metrics.s4
                    visible: root.totalsStatus === "ready"

                    TotalTile {
                        objectName: "tileHours"
                        width: totalsBlock.tileWidth
                        label: qsTr("Hours played")
                        value: root.client && root.client.hoursPlayedText
                               ? String(root.client.hoursPlayedText) : ""
                    }

                    TotalTile {
                        objectName: "tileCredit"
                        width: totalsBlock.tileWidth
                        label: qsTr("Credit left")
                        value: root.client && root.client.creditLeftText
                               ? String(root.client.creditLeftText) : ""
                    }
                }

                // Loading, past the delay: skeleton tiles.
                Flow {
                    id: skeletonTiles

                    objectName: "totalsLoading"
                    width: parent.width
                    spacing: Metrics.s4
                    visible: root.totalsLoading && root.totalsPastDelay

                    Repeater {
                        model: 2

                        delegate: Rectangle {
                            width: totalsBlock.tileWidth
                            height: Metrics.s16 + Metrics.s4
                            radius: Metrics.radiusMd
                            color: Tokens.surface3Default
                        }
                    }
                }

                // Error: one tile with the server's sentence and a way to try again.
                Rectangle {
                    id: totalsError

                    objectName: "totalsError"
                    width: parent.width
                    implicitHeight: totalsErrorColumn.implicitHeight + Metrics.s4 * 2
                    radius: Metrics.radiusMd
                    color: Tokens.surface1Default
                    border.width: 1
                    border.color: Tokens.borderDefault
                    visible: root.totalsStatus === "error"

                    Column {
                        id: totalsErrorColumn
                        x: Metrics.s4
                        y: Metrics.s4
                        width: parent.width - Metrics.s4 * 2
                        spacing: Metrics.s2

                        Text {
                            objectName: "totalsErrorText"
                            width: parent.width
                            text: "◆ " + (root.client && root.client.totalsError
                                           ? String(root.client.totalsError) : "")
                            wrapMode: Text.Wrap
                            color: Tokens.destructiveDefault
                            font.family: Tokens.fontSansDefault
                            font.pixelSize: Metrics.fontSm
                        }

                        Text {
                            objectName: "totalsErrorReference"
                            visible: text.length > 0
                            text: root.client && root.client.totalsErrorReference
                                  ? String(root.client.totalsErrorReference) : ""
                            wrapMode: Text.NoWrap
                            color: Tokens.foregroundDefault
                            font.family: Tokens.fontMonoDefault
                            font.pixelSize: Metrics.fontSm
                        }

                        SeatHubButton {
                            objectName: "totalsRetry"
                            variant: "ghost"
                            text: qsTr("Try again")
                            onClicked: root.client.reloadTotals()
                        }
                    }
                }
            }

            // Back, and Sign out at the bottom of the column: no confirmation (sign-in is one step away);
            // it revokes on the server, deletes the local credential and lands on sign-in even offline.
            Row {
                spacing: Metrics.s2

                SeatHubButton {
                    objectName: "backButton"
                    variant: "ghost"
                    text: qsTr("Back")

                    onClicked: root.client.closeProfile()
                }

                SeatHubButton {
                    objectName: "signOutButton"
                    variant: "ghost"
                    text: qsTr("Sign out")

                    onClicked: root.client.signOut()
                }
            }
        }
    }

    // --- right column -----------------------------------------------------------------------------

    Item {
        id: rightColumn

        objectName: "rightColumn"
        x: leftScroll.x + leftScroll.width + Metrics.s12
        y: Metrics.s8
        width: root.width - x - Metrics.s8
        height: root.height - Metrics.s8 * 2

        SeatHubTabs {
            id: tabsBar

            objectName: "tabsBar"
            titles: root.lists.map(function (list) { return list.title })

            onCurrentIndexChanged: root.currentTab = tabsBar.currentIndex
        }

        // One region under the tabs; the list of the selected tab fills it. The three lists are all
        // here and only the selected one shows, so each keeps its own scroll position and its own state.
        Item {
            id: listRegion

            x: 0
            y: tabsBar.height + Metrics.s4
            width: parent.width
            height: parent.height - y

            Repeater {
                model: root.lists

                delegate: SeatHubListView {
                    id: listView

                    required property int index
                    required property var modelData

                    objectName: "list_" + listView.modelData.key
                    anchors.fill: parent
                    visible: listView.index === root.currentTab
                    list: root.listFor(listView.index)
                    loadingText: listView.modelData.loading
                    emptyText: listView.modelData.empty
                    emptyActionText: listView.modelData.action
                    loadingDelay: root.loadingDelay

                    onNextPageRequested: root.client.loadNextPage(listView.modelData.key)

                    // A failed later page is asked for again; a failed first page starts the list over.
                    onRetryRequested: {
                        if (listView.moreFailed)
                            root.client.loadNextPage(listView.modelData.key)
                        else
                            root.client.reloadList(listView.modelData.key)
                    }

                    onEmptyActionTriggered: {
                        if (listView.modelData.key === "sessions")
                            root.client.closeProfile()
                        else
                            root.client.openTopUp()
                    }
                }
            }
        }
    }
}
