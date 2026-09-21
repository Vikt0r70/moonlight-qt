import QtQuick
import QtQuick.Controls
import SeatHub.Tokens 1.0

// One of the profile's three histories, drawn inside a region of its own (`05-UI-SPEC` "Profile",
// `screens.md` §27; CUST-14, D-14, D-15).
//
// It draws a list model the facade owns (`SessionListModel` and its two siblings in
// `customer_lists.h`) and does nothing else: no request, no arithmetic, no formatting. Every row is
// already display text - a date in Jordan time, how it ended or what it was in plain words, the length
// or the signed amount - so this file only lays it out: date and time (mono) on the left, the words in
// the middle, the number (mono, right-aligned) on the right, in rows 44px tall with a hairline between
// them. Nothing here can name a rig, because nothing it is given does.
//
// The region is a fixed box: the list scrolls inside it (with a scrollbar that shows while there is
// more than fits) and the page around it does not. When the customer reaches the end and the server
// said there is more, it asks for the next page through `nextPageRequested()`; the facade decides
// whether that means anything (a request already out, or no cursor left, and it does not).
//
// Every state is its own, independent of the other two lists and of the rest of the profile:
//
//   loading      the first page is out. After 300 ms (a fast answer never flashes it): skeleton rows of
//                the final shape and the named line (`loadingText`).
//   empty        the list answered and is empty: one sentence and one action.
//   populated    the rows.
//   loading more a footer row with a spinner and the same named line; the rows stay.
//   end of feed  a hairline, a check mark and `You've reached the end.`
//   error        the server's sentence, its reference (mono) and `Try again`, in the region; a failure of
//                a later page shows the same in the footer row and keeps every loaded row.
//
// The two marks that carry meaning beside their words (a top-up's status dot) are never colour alone:
// the word is always next to the dot.
Item {
    id: root

    objectName: "seatHubListView"

    /// The list model from the facade (a `CustomerListModel`): `status`, `count`, `hasMore`,
    /// `loadingMore`, `moreFailed`, `errorText`, `errorReference`, and the four row roles.
    property var list: null

    /// The named loading line for this list (copy.md § Menu and profile).
    property string loadingText: ""

    /// The empty state's one sentence and its one action.
    property string emptyText: ""
    property string emptyActionText: ""

    /// How long a first page may take before its skeleton shows. A fast answer never flashes it.
    property int loadingDelay: 300

    /// The customer reached the end of the rows and the server has more.
    signal nextPageRequested()

    /// `Try again`: the first page failed and is asked for again, or a later page did and is.
    signal retryRequested()

    /// The empty state's action (`Back to home`, or `Top up`).
    signal emptyActionTriggered()

    // What the model says. Every read is guarded, so a list that is not attached draws nothing.
    readonly property string status: root.list && root.list.status !== undefined
                                     ? String(root.list.status) : "idle"
    readonly property int rowCount: root.list && root.list.count !== undefined
                                    ? Number(root.list.count) : 0
    readonly property bool hasMore: root.list ? root.list.hasMore === true : false
    readonly property bool loadingMore: root.list ? root.list.loadingMore === true : false
    readonly property bool moreFailed: root.list ? root.list.moreFailed === true : false
    readonly property string errorText: root.list && root.list.errorText
                                        ? String(root.list.errorText) : ""
    readonly property string errorReference: root.list && root.list.errorReference
                                             ? String(root.list.errorReference) : ""

    readonly property bool showLoading: root.status === "loading"
    readonly property bool showError: root.status === "error"
    readonly property bool showEmpty: root.status === "ready" && root.rowCount === 0
    readonly property bool showRows: root.status === "ready" && root.rowCount > 0

    /// True once a first page has been out for longer than `loadingDelay`.
    property bool pastLoadingDelay: false

    // The fixed widths of the two mono cells; the words in the middle take what is left.
    readonly property int whenWidth: 152
    readonly property int amountWidth: 116

    function restartDelay() {
        root.pastLoadingDelay = root.showLoading && root.loadingDelay <= 0
        if (root.showLoading && root.loadingDelay > 0)
            delayTimer.restart()
        else
            delayTimer.stop()
    }

    onShowLoadingChanged: root.restartDelay()
    Component.onCompleted: root.restartDelay()

    Timer {
        id: delayTimer
        interval: root.loadingDelay
        onTriggered: root.pastLoadingDelay = root.showLoading
    }

    // Asks for the next page when the rows end and there is one. Never while a request is out, and
    // never on its own after a failed page: `Try again` is the customer's to press.
    function maybeRequestMore() {
        if (root.showRows && root.hasMore && !root.loadingMore && !root.moreFailed
                && rows.atYEnd)
            root.nextPageRequested()
    }

    // The box the region is drawn in. The empty state swaps it for a dashed outline.
    Rectangle {
        id: frame
        anchors.fill: parent
        radius: Metrics.radiusMd
        color: Tokens.surface1Default
        border.width: 1
        border.color: Tokens.borderDefault
        visible: !root.showEmpty
    }

    // --- populated -------------------------------------------------------------------------------

    ListView {
        id: rows

        objectName: "rows"
        anchors.fill: parent
        anchors.margins: 1
        visible: root.showRows
        clip: true
        model: root.list
        boundsBehavior: Flickable.StopAtBounds

        // Shows while there is more than fits, and not otherwise.
        ScrollBar.vertical: ScrollBar {
            policy: rows.contentHeight > rows.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
        }

        // Checked after the view has settled: while rows are still being laid out `atYEnd` can be
        // true for a moment before it has caught up with the new content height, and that must not
        // read as the customer having reached the end.
        onAtYEndChanged: Qt.callLater(root.maybeRequestMore)
        onContentHeightChanged: Qt.callLater(root.maybeRequestMore)

        delegate: Rectangle {
            id: row

            required property string whenText
            required property string kindText
            required property string amountText
            required property string tone

            width: ListView.view ? ListView.view.width : 0
            height: Metrics.touchTarget
            color: hover.hovered ? Tokens.surface3Default : "transparent"

            Accessible.role: Accessible.ListItem
            Accessible.name: [row.whenText, row.kindText, row.amountText]
                                 .filter(function (part) { return part.length > 0 }).join(", ")

            HoverHandler {
                id: hover
            }

            Row {
                anchors.fill: parent
                anchors.leftMargin: Metrics.s4
                anchors.rightMargin: Metrics.s4
                spacing: Metrics.s4

                // Date and time, mono. The text is already `Thu 12 Sep, 21:40`.
                Text {
                    objectName: "whenCell"
                    width: root.whenWidth
                    height: parent.height
                    verticalAlignment: Text.AlignVCenter
                    text: row.whenText
                    // A number or a date is never cut short: it shrinks to fit instead.
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: Metrics.fontLabel
                    color: Tokens.foregroundDefault
                    font.family: Tokens.fontMonoDefault
                    font.pixelSize: Metrics.fontSm
                    font.features: { "tnum": 1 }
                }

                // How it ended, what it was, or waiting/credited. Wraps to a second line rather than
                // cutting a word off; a top-up's status carries its dot in front of the word.
                Item {
                    width: parent.width - root.whenWidth - root.amountWidth - parent.spacing * 2
                    height: parent.height

                    Row {
                        anchors.fill: parent
                        spacing: Metrics.s2

                        Rectangle {
                            visible: row.tone.length > 0
                            anchors.verticalCenter: parent.verticalCenter
                            width: Metrics.s2
                            height: Metrics.s2
                            radius: Metrics.s1
                            color: row.tone === "credited" ? Tokens.successDefault : Tokens.warnDefault
                        }

                        Text {
                            objectName: "kindCell"
                            width: parent.width - (row.tone.length > 0 ? Metrics.s2 + parent.spacing : 0)
                            height: parent.height
                            verticalAlignment: Text.AlignVCenter
                            text: row.kindText
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                            color: Tokens.foregroundDefault
                            font.family: Tokens.fontSansDefault
                            font.pixelSize: Metrics.fontSm
                        }
                    }
                }

                // The length, the signed amount or the minutes added: mono, right-aligned.
                Text {
                    objectName: "amountCell"
                    width: root.amountWidth
                    height: parent.height
                    verticalAlignment: Text.AlignVCenter
                    horizontalAlignment: Text.AlignRight
                    text: row.amountText
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: Metrics.fontLabel
                    color: Tokens.foregroundDefault
                    font.family: Tokens.fontMonoDefault
                    font.pixelSize: Metrics.fontSm
                    font.features: { "tnum": 1 }
                }
            }

            // The hairline between rows.
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Tokens.borderDefault
            }
        }

        footer: Item {
            id: footer

            objectName: "footer"
            width: rows.width
            height: root.loadingMore ? loadingMoreRow.height
                                     : (root.moreFailed ? moreFailedColumn.height
                                                        : (root.hasMore ? 0 : endRow.height))

            // Next page loading: a footer row with a spinner and the named line; the rows stay.
            Row {
                id: loadingMoreRow
                objectName: "footerLoading"
                visible: root.loadingMore
                height: Metrics.touchTarget
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Metrics.s2

                BusyIndicator {
                    anchors.verticalCenter: parent.verticalCenter
                    running: root.loadingMore
                    width: Metrics.s5
                    height: Metrics.s5
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.loadingText
                    color: Tokens.foregroundMutedDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontSm
                }
            }

            // A later page failed: the same reason and reference as a first-page failure, and the rows
            // already loaded are still above it.
            Column {
                id: moreFailedColumn
                objectName: "footerError"
                visible: root.moreFailed && !root.loadingMore
                width: parent.width
                padding: Metrics.s4
                spacing: Metrics.s1

                Text {
                    objectName: "footerErrorText"
                    width: parent.width - Metrics.s4 * 2
                    text: "◆ " + root.errorText
                    wrapMode: Text.Wrap
                    color: Tokens.destructiveDefault
                    font.family: Tokens.fontSansDefault
                    font.pixelSize: Metrics.fontSm
                }

                Text {
                    objectName: "footerErrorReference"
                    visible: root.errorReference.length > 0
                    text: root.errorReference
                    wrapMode: Text.NoWrap
                    color: Tokens.foregroundDefault
                    font.family: Tokens.fontMonoDefault
                    font.pixelSize: Metrics.fontSm
                }

                SeatHubButton {
                    objectName: "footerRetry"
                    variant: "ghost"
                    text: qsTr("Try again")
                    onClicked: root.retryRequested()
                }
            }

            // End of the feed: a hairline, a check node and one line. No refresh action: the list
            // reloads when the profile is opened again.
            Column {
                id: endRow
                objectName: "endOfFeed"
                visible: !root.hasMore && !root.loadingMore && !root.moreFailed
                width: parent.width
                height: visible ? Metrics.touchTarget : 0
                spacing: 0

                Rectangle {
                    width: parent.width
                    height: 1
                    color: Tokens.borderDefault
                }

                Row {
                    height: Metrics.touchTarget - 1
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: Metrics.s2

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "✓"
                        color: Tokens.foregroundMutedDefault
                        font.family: Tokens.fontSansDefault
                        font.pixelSize: Metrics.fontSm
                    }

                    Text {
                        objectName: "endOfFeedText"
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("You've reached the end.")
                        color: Tokens.foregroundMutedDefault
                        font.family: Tokens.fontSansDefault
                        font.pixelSize: Metrics.fontSm
                    }
                }
            }
        }
    }

    // --- loading: skeleton rows of the final shape, and the named line ----------------------------

    Column {
        id: loading

        objectName: "loadingState"
        anchors.fill: parent
        anchors.margins: 1
        visible: root.showLoading && root.pastLoadingDelay
        clip: true

        Text {
            objectName: "loadingLine"
            width: parent.width
            height: Metrics.touchTarget
            leftPadding: Metrics.s4
            verticalAlignment: Text.AlignVCenter
            text: root.loadingText
            color: Tokens.foregroundMutedDefault
            font.family: Tokens.fontSansDefault
            font.pixelSize: Metrics.fontSm
        }

        Repeater {
            objectName: "skeletonRows"
            model: 6

            delegate: Item {
                width: loading.width
                height: Metrics.touchTarget

                // The three cells of a real row, as quiet bars.
                Rectangle {
                    x: Metrics.s4
                    anchors.verticalCenter: parent.verticalCenter
                    width: root.whenWidth - Metrics.s4
                    height: Metrics.s3
                    radius: Metrics.radiusXs
                    color: Tokens.surface3Default
                }

                Rectangle {
                    x: Metrics.s4 + root.whenWidth + Metrics.s4
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(0, parent.width - root.whenWidth - root.amountWidth - Metrics.s4 * 4)
                    height: Metrics.s3
                    radius: Metrics.radiusXs
                    color: Tokens.surface3Default
                }

                Rectangle {
                    anchors.right: parent.right
                    anchors.rightMargin: Metrics.s4
                    anchors.verticalCenter: parent.verticalCenter
                    width: root.amountWidth - Metrics.s4
                    height: Metrics.s3
                    radius: Metrics.radiusXs
                    color: Tokens.surface3Default
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: Tokens.borderDefault
                }
            }
        }
    }

    // --- empty: one sentence and one action, in a dashed outline ----------------------------------

    Item {
        id: empty

        objectName: "emptyState"
        anchors.fill: parent
        visible: root.showEmpty

        // ui.md §7 Empty: a dashed border. `Canvas` is Qt Quick core, so no extra module is needed.
        Canvas {
            anchors.fill: parent
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onPaint: {
                var ctx = getContext("2d")
                ctx.reset()
                ctx.strokeStyle = Tokens.borderStrongDefault
                ctx.lineWidth = 1
                ctx.setLineDash([4, 4])
                ctx.beginPath()
                ctx.roundedRect(0.5, 0.5, width - 1, height - 1, Metrics.radiusMd, Metrics.radiusMd)
                ctx.stroke()
            }
        }

        Column {
            anchors.centerIn: parent
            width: Math.min(parent.width - Metrics.s8, 320)
            spacing: Metrics.s4

            Text {
                objectName: "emptyText"
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: root.emptyText
                wrapMode: Text.Wrap
                color: Tokens.foregroundDefault
                font.family: Tokens.fontSansDefault
                font.pixelSize: Metrics.fontBody
            }

            SeatHubButton {
                objectName: "emptyAction"
                anchors.horizontalCenter: parent.horizontalCenter
                variant: "ghost"
                text: root.emptyActionText
                onClicked: root.emptyActionTriggered()
            }
        }
    }

    // --- error: the server's sentence, its reference and a way to try again -----------------------

    Column {
        id: errorState

        objectName: "errorState"
        anchors.centerIn: parent
        width: Math.min(parent.width - Metrics.s8, 360)
        spacing: Metrics.s2
        visible: root.showError

        Text {
            objectName: "errorText"
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: "◆ " + root.errorText
            wrapMode: Text.Wrap
            color: Tokens.destructiveDefault
            font.family: Tokens.fontSansDefault
            font.pixelSize: Metrics.fontBody
        }

        // ADR-0008: a reference is mono, never translated, never broken across lines.
        Text {
            objectName: "errorReference"
            visible: root.errorReference.length > 0
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.errorReference
            wrapMode: Text.NoWrap
            color: Tokens.foregroundDefault
            font.family: Tokens.fontMonoDefault
            font.pixelSize: Metrics.fontSm
        }

        SeatHubButton {
            objectName: "retryButton"
            anchors.horizontalCenter: parent.horizontalCenter
            variant: "ghost"
            text: qsTr("Try again")
            onClicked: root.retryRequested()
        }
    }
}
