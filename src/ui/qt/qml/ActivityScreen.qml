// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// The session's unit activity: logins, talkgroup joins, denials and radio
// commands a trunked system's control channel sent, newest first, with
// filter pills for each kind. History's shell and row grammar: who on the
// first line with the time at its end, what happened on the second, where and
// why on the third. Rows come from unitActivity (UnitActivityModel) through
// activityView, its filter; both live for the session only.
Item {
    id: screen

    property bool showTitle: true

    // The filter pills, in order: each one's words and the DSD_UA_FILTER_*
    // bit it shows (0: every row). One is chosen at a time, as History's.
    readonly property var filterChoices: [
        {text: qsTr("All"), bit: 0},
        {text: qsTr("Logins"), bit: 1},
        {text: qsTr("Talkgroups"), bit: 2},
        {text: qsTr("Refused & queued"), bit: 4},
        {text: qsTr("Radio commands"), bit: 8},
        {text: qsTr("Other"), bit: 16}
    ]

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    // Whether every row puts its time under the name rather than at its end:
    // one rule for the whole list, so rows keep one shape, decided by whether
    // an eight-digit ID and the widest time ("×99 00:00:00") fit on one line.
    TextMetrics {
        id: idMeasure
        font.family: Theme.sans
        font.pixelSize: Math.round(Theme.fontSize(16))
        font.weight: Font.DemiBold
        text: "00000000"
    }
    TextMetrics {
        id: stampMeasure
        font.family: Theme.mono
        font.pixelSize: Math.round(Theme.fontSize(12))
        text: "×99 00:00:00"
    }
    readonly property bool stackTime: activityList.width - 36 < idMeasure.advanceWidth + 12 + stampMeasure.advanceWidth

    // As History: the list keeps at least this share of the screen, and the
    // header scrolls inside the rest when a large text size makes it taller.
    readonly property real listMinHeight: Math.ceil(height * 0.4)

    PlexFlickable {
        id: headerScroll
        objectName: "activityHeaderScroll"
        accessibleName: qsTr("Activity filters")

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: Math.min(contentHeight, Math.max(0, screen.height - screen.listMinHeight - activityList.anchors.topMargin))
        contentWidth: width
        // Room under the pills for their focus ring, which the clip would cut.
        contentHeight: chrome.y + chrome.height + 4
        clip: true
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        onHeightChanged: revealFocusLater.restart()

        Column {
            id: chrome

            x: Theme.screenPadding
            y: Theme.screenPadding
            width: headerScroll.width - 2 * Theme.screenPadding
            spacing: Theme.gap

            ReadableText {
                objectName: "activityTitle"
                heading: true
                visible: screen.showTitle
                width: parent.width
                height: visible ? Math.max(48, implicitHeight) : 0
                verticalAlignment: Text.AlignVCenter
                text: qsTr("Activity")
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(24)
                font.weight: Font.Bold
                font.letterSpacing: -0.24
                color: Theme.textPrimary
            }

            Flow {
                objectName: "activityFilters"
                width: parent.width
                spacing: 10

                Repeater {
                    model: screen.filterChoices

                    FilterPill {
                        required property int index
                        required property var modelData

                        objectName: "activityFilter" + index
                        // A pill wider than the screen at a large text size
                        // wraps its words rather than running off the edge.
                        wrapLabel: true
                        width: Math.min(implicitWidth, parent.width)
                        caret: false
                        text: modelData.text
                        active: activityView.filter === modelData.bit
                        // One of six, as Listen's tabs: a radio button that
                        // says it is checked, not only drawn in the accent
                        // colour, and not "selected" as well.
                        Accessible.role: Accessible.RadioButton
                        Accessible.checkable: true
                        Accessible.checked: active
                        Accessible.selected: false
                        onClicked: activityView.filter = modelData.bit
                    }
                }
            }
        }
    }

    // A header that scrolls ends on a hairline, as History's does.
    Rectangle {
        anchors.top: headerScroll.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.screenPadding
        anchors.rightMargin: Theme.screenPadding
        height: 1
        visible: headerScroll.interactive
        color: Theme.divider
    }

    // The pill that takes the focus scrolls into the header's view, and Show
    // all into the empty state's.
    function revealHeaderFocus() {
        var window = screen.Window.window;
        if (!window || !visible)
            return;
        Theme.revealFocus(headerScroll, chrome, window.activeFocusItem);
        Theme.revealFocus(emptyScroll, emptyState, window.activeFocusItem);
    }
    Connections {
        target: screen.Window.window
        function onActiveFocusItemChanged() {
            if (screen.visible)
                revealFocusLater.restart();
        }
    }
    Timer {
        id: revealFocusLater
        interval: 0
        onTriggered: screen.revealHeaderFocus()
    }

    ListView {
        id: activityList
        objectName: "activityList"

        anchors.top: headerScroll.bottom
        anchors.topMargin: 4
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        // Set by activityPlace below, only while the screen is visible (issue #782).
        model: null

        delegate: Item {
            id: row

            required property var model

            // The row's identity for activityPlace: its seq (0 on the gap row).
            readonly property var rowSeq: model.seq
            readonly property bool gap: model.gap
            // Who: the radio's or group's name with its ID, or the ID alone.
            readonly property string who: model.name.length > 0 ? Util.oneLine(model.name) + " (" + model.subject + ")" : model.subject
            // What happened, and to what: the talkgroup, value, recipient.
            readonly property string what: [model.headline, model.target].filter(function (part) {
                return part.length > 0;
            }).join(" · ")
            // Where and why: a reason the headline does not already say (a
            // deny's is in it), then the protocol and the system it was heard on.
            readonly property bool reasonInHeadline: model.reason.length > 0 && model.headline.indexOf(model.reason) >= 0
            readonly property string where: [reasonInHeadline ? "" : model.reason, model.protocol, model.scope].filter(function (part) {
                return part.length > 0;
            }).join(" · ")
            readonly property string gapDetail: qsTr("The log keeps the newest %1 events.").arg(Util.countText(unitActivity.capacity))

            width: ListView.view ? ListView.view.width : 0
            implicitHeight: Math.max(Theme.rowHeight, (gap ? gapLabels.implicitHeight : labels.implicitHeight) + 24)
            height: implicitHeight

            // One node per row, read whole: its words are the row's name and
            // description, so no line inside it is a node of its own.
            Accessible.role: Accessible.StaticText
            Accessible.name: navigationAllowed && visible ? (gap ? gapTitle.text : (model.timeText.length > 0 ? who + ", " + model.timeText : who)) : ""
            Accessible.description: navigationAllowed && visible ? (gap ? gapDetail : [what, where, model.repeat > 1 ? qsTr("heard %1 times").arg(model.repeat) : ""].filter(function (part) {
                return part.length > 0;
            }).join(", ")) : ""
            readonly property bool navigationAllowed: Navigation.allows(row)
            Accessible.focusable: navigationAllowed && visible
            Accessible.ignored: !navigationAllowed || !visible

            Column {
                id: labels
                visible: !row.gap
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 18
                anchors.rightMargin: 18
                anchors.verticalCenter: parent.verticalCenter
                spacing: 3

                // Who, with the time of the first message (and, for one heard
                // again, how many) at its end: the lines under it keep the
                // row's whole width, which a large text size needs. Where an
                // ID and the time do not fit on one line (screen.stackTime)
                // every row puts its time under the name.
                Item {
                    id: whoLine
                    readonly property bool stacked: screen.stackTime
                    width: parent.width
                    height: stacked ? whoText.height + 3 + stamp.height : whoText.height

                    // Plain text, one line: a name is what the air or a list
                    // delivered, and must never be read as markup. A long one
                    // is cut in its middle, so the ID at its end stays whole.
                    Text {
                        id: whoText
                        objectName: "activityRowWho"
                        width: whoLine.stacked ? parent.width : parent.width - stamp.implicitWidth - 12
                        text: row.who
                        textFormat: Text.PlainText
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(16)
                        font.weight: Font.DemiBold
                        color: Theme.textPrimary
                        elide: Text.ElideMiddle
                    }

                    Row {
                        id: stamp
                        x: whoLine.stacked ? 0 : parent.width - width
                        y: whoLine.stacked ? whoText.height + 3 : whoText.baselineOffset - timeText.baselineOffset
                        spacing: 8

                        Text {
                            objectName: "activityRowRepeat"
                            visible: model.repeat > 1
                            text: "×" + Util.countText(model.repeat)
                            font.family: Theme.mono
                            font.pixelSize: Theme.fontSize(12)
                            color: Theme.textSubdued
                        }

                        Text {
                            id: timeText
                            objectName: "activityRowTime"
                            text: model.timeText
                            font.family: Theme.mono
                            font.pixelSize: Theme.fontSize(12)
                            color: Theme.textSubdued
                        }
                    }
                }

                // An inhibit or kill command is the one row drawn apart: it
                // says the command was sent, in the warning colour.
                Text {
                    objectName: "activityRowWhat"
                    width: parent.width
                    text: Util.oneLine(row.what)
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(14)
                    font.weight: model.warning ? Font.DemiBold : Font.Normal
                    color: model.warning ? Theme.alert : Theme.textSecondary
                }

                // Two lines at most, so a large text size still shows the system.
                Text {
                    objectName: "activityRowWhere"
                    width: parent.width
                    visible: text.length > 0
                    text: Util.oneLine(row.where)
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(12)
                    color: Theme.textSubdued
                    elide: Text.ElideRight
                }
            }

            // Where the ring dropped rows the app had not read: one row says so.
            Column {
                id: gapLabels
                visible: row.gap
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 18
                anchors.rightMargin: 18
                anchors.verticalCenter: parent.verticalCenter
                spacing: 3

                Text {
                    id: gapTitle
                    objectName: "activityGapTitle"
                    width: parent.width
                    text: qsTr("Earlier activity was not kept")
                    wrapMode: Text.Wrap
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(14)
                    font.weight: Font.DemiBold
                    color: Theme.textSecondary
                }

                Text {
                    width: parent.width
                    text: row.gapDetail
                    wrapMode: Text.Wrap
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(13)
                    color: Theme.textSubdued
                }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 18
                height: 1
                color: Theme.divider
            }
        }
    }

    // Keeps the newest row in view for a reader at the top of the list.
    FollowLatest {
        id: follow
        list: activityList
    }

    // Hidden, the list holds no rows (issue #782): the session runs on the
    // monitor with this screen under it, and every row built for a message
    // landing there is announced to accessibility clients as hidden. Shown
    // again, a reader on the newest row is on it again, and one reading
    // further down is back on the same row at the same height.
    KeepPlace {
        id: activityPlace
        list: activityList
        model: activityView
        atStart: follow.atLatest
        keyOf: function (row) {
            return row.rowSeq;
        }
        rowOf: function (seq) {
            return activityView.rowForSeq(seq);
        }
    }

    // A new filter is a new question, answered from the top (as History's).
    Connections {
        target: activityView

        function onFilterChanged() {
            activityPlace.forget();
            Qt.callLater(activityList.positionViewAtBeginning);
        }
    }

    // A sibling of the list, not a child: ListView reparents declared
    // children into its contentItem. It takes the list's own area and never
    // more: centred while it fits, and from the top, scrolling, when a large
    // text size makes it taller than the list (it ran over the filter pills).
    PlexFlickable {
        id: emptyScroll
        objectName: "activityEmptyScroll"
        accessibleName: qsTr("Activity explanation")

        anchors.fill: activityList
        // The filter's rows, not the view's: the view holds none while hidden.
        visible: activityView.count === 0
        contentWidth: width
        contentHeight: Math.max(height, emptyState.height + 2 * Theme.screenPadding)
        clip: true
        interactive: emptyState.height + 2 * Theme.screenPadding > height
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: emptyState
            objectName: "activityEmptyState"
            x: Theme.screenPadding
            y: Math.max(Theme.screenPadding, (emptyScroll.height - height) / 2)
            width: emptyScroll.width - 2 * Theme.screenPadding
            spacing: 8

            // An empty filter and an empty log are different situations: the
            // first must say the rows are hidden, not missing.
            readonly property bool filtered: unitActivity.count > 0
            readonly property string hiddenText: unitActivity.count === 1 ? qsTr("1 event is hidden by the filter.") : qsTr("%1 events are hidden by the filter.").arg(Util.countText(unitActivity.count))

            ReadableText {
                objectName: "activityEmptyTitle"
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: parent.filtered ? qsTr("No matching activity") : qsTr("No activity yet")
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(16)
                font.weight: Font.DemiBold
                color: Theme.textSecondary
            }

            // What shows here, and what a listener never hears.
            ReadableText {
                objectName: "activityEmptyDetail"
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: parent.filtered ? parent.hiddenText : qsTr("Logins, talkgroup joins, denials and radio commands from a P25, DMR Tier III, NXDN or TETRA control channel show here as they're heard.")
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(13)
                color: Theme.textSubdued
                wrapMode: Text.Wrap
            }

            ReadableText {
                objectName: "activityEmptyLimits"
                width: parent.width
                visible: !parent.filtered
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Some of it never reaches a listener: nothing shows while tuned to a voice channel, DMR and TETRA radios don't announce logging off, and whether a radio carried out a command isn't sent.")
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(13)
                color: Theme.textSubdued
                wrapMode: Text.Wrap
            }

            Item {
                width: 1
                height: 6
                visible: parent.filtered
            }

            OutlineButton {
                objectName: "activityShowAll"
                anchors.horizontalCenter: parent.horizontalCenter
                visible: parent.filtered
                width: Math.min(parent.width, 220)
                text: qsTr("Show all")
                onClicked: activityView.filter = 0
            }
        }
    }
}
