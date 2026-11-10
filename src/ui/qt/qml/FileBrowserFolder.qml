// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Issue #830: an open folder in the file browser: its rows (folders first, as
// listed), Show more, and, while it shows no rows, why (empty, a problem) with
// Try again and the inbox's help. The browser (FileBrowserScreen) owns every
// request and the state; this view shows it and calls back.
Item {
    id: folderView

    // The FileBrowserScreen this view belongs to, and its state.
    required property Item browser
    readonly property QtObject d: browser.internals
    readonly property Item stateLine: stateText
    // In the list's footer, which the list makes; null until it has.
    readonly property Item pageProblemLine: entryList.footerItem ? entryList.footerItem.problemLine : null

    // The first row of the folder, for the focus a new listing gives.
    function firstRow() {
        if (!d.entries.length)
            return null;
        entryList.forceLayout();
        return entryList.itemAtIndex(0);
    }
    function revealFocus(focus) {
        Theme.revealFocus(stateScroll, stateColumn, focus);
        Theme.revealFocus(entryList, entryList.contentItem, focus);
    }

    visible: d.view === "folder"

    // What the folder holds, or why it shows nothing.
    PlexFlickable {
        id: stateScroll
        objectName: "fileBrowserStateScroll"
        anchors.fill: parent
        visible: d.entries.length === 0
        clip: true
        contentWidth: width
        contentHeight: stateColumn.height
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: stateColumn
            x: (stateScroll.width - width) / 2
            width: browser.contentWidth
            spacing: Theme.gap

            ReadableText {
                id: stateText
                objectName: "fileBrowserState"
                width: parent.width
                visible: text.length > 0
                text: browser.stateText
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(14)
                color: browser.stateIsProblem ? Theme.alert : Theme.textSubdued
                wrapMode: Text.Wrap
                Accessible.role: browser.stateIsProblem ? Accessible.AlertMessage : Accessible.StaticText
            }
            OutlineButton {
                objectName: "fileBrowserRetry"
                visible: ["timedOut", "busy", "failed", "unsupported"].indexOf(d.folderStatus) >= 0
                text: qsTr("Try again")
                onClicked: browser.requestListing()
            }
            // An empty inbox says how files get there.
            Column {
                objectName: "fileBrowserInboxHelp"
                width: parent.width
                visible: browser.inboxFolderEmpty
                spacing: Theme.gap

                ReadableText {
                    width: parent.width
                    text: qsTr("Files copied into this folder show up here. Depending on the device, you can copy them from a computer over USB or with adb:")
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(14)
                    color: Theme.textSubdued
                    wrapMode: Text.Wrap
                }
                FileBrowserCommand {
                    objectName: "fileBrowserInboxCommand"
                    copyName: "fileBrowserCopyInbox"
                    copied: d.copied.length > 0 && d.copied === copyText
                    onCopyRequested: function (text) {
                        browser.copy(text);
                    }
                    lines: browser.inboxLines
                }
                FileBrowserLine {
                    text: qsTr("Uninstalling DSD-neo or clearing its storage empties this folder.")
                }
            }
        }
    }

    ListView {
        id: entryList
        objectName: "fileBrowserList"
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        // The column is the view's: a vertical ListView writes each row's x.
        width: browser.contentWidth
        visible: d.entries.length > 0
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        keyNavigationEnabled: false
        model: StableRows {
            id: entryRows
            entries: d.view === "folder" ? d.entries : []
            keyOf: function (entry) {
                return entry.path.length ? entry.path : entry.reference;
            }
            lineOf: function (entry) {
                return browser.entryName(entry);
            }
        }

        delegate: Item {
            id: entryRow
            required property int index
            required property string key
            required property string line
            readonly property bool live: index >= 0 && index < entryRows.live && index < d.entries.length
            readonly property var entry: live ? d.entries[index] : null
            readonly property bool folder: entry !== null && entry.dir === true
            objectName: "fileBrowserEntry:" + key
            width: ListView.view.width
            height: Math.max(Theme.minimumTouchSize, labels.height + 24)
            // A file cannot be picked while DSD-neo closes for a restart (the
            // screen's closing line says so): it shows dimmed.
            opacity: index < entryRows.shown ? (inert ? 0.5 : 1) : 0
            readonly property bool inert: !folder && restartCloser.active
            readonly property bool navigationAllowed: Navigation.allows(entryRow)
            activeFocusOnTab: (navigationAllowed && visible && live) || activeFocus
            Accessible.role: Accessible.Button
            Accessible.name: navigationAllowed && live ? line : ""
            Accessible.focusable: navigationAllowed && visible && live
            Accessible.ignored: !navigationAllowed || !visible || !live
            Accessible.onPressAction: activate()
            function activate() {
                if (live && Navigation.allows(entryRow))
                    browser.choose(entry);
            }
            Keys.onSpacePressed: activate()
            Keys.onReturnPressed: activate()
            Keys.onEnterPressed: activate()
            // Inside the row: the list clips at its edges.
            FocusFrame {
                anchors.margins: 0
            }

            Rectangle {
                anchors.fill: parent
                radius: Theme.radiusButton
                color: entryTap.pressed && entryRow.live ? Theme.chipSelectedFill : "transparent"
            }

            // Drawn marks, in the caret's stroke: a folder, or a page. Drawn
            // on a 20 px grid and scaled with the text, so they keep their
            // size beside the names at a large font.
            Item {
                id: glyph
                x: 8
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.fontSize(20)
                height: width
                Item {
                    width: 20
                    height: 20
                    scale: glyph.width / 20
                    transformOrigin: Item.TopLeft
                    Rectangle {
                        visible: entryRow.folder
                        x: 1
                        y: 2
                        width: 8
                        height: 5
                        radius: 1.5
                        color: "transparent"
                        border.width: 1.6
                        border.color: Theme.textSecondary
                    }
                    Rectangle {
                        visible: entryRow.folder
                        x: 1
                        y: 5
                        width: 18
                        height: 13
                        radius: 2
                        color: Theme.bg
                        border.width: 1.6
                        border.color: Theme.textSecondary
                    }
                    Rectangle {
                        visible: !entryRow.folder
                        x: 3
                        y: 1
                        width: 14
                        height: 18
                        radius: 2
                        color: "transparent"
                        border.width: 1.6
                        border.color: Theme.textSecondary
                    }
                    Repeater {
                        model: entryRow.folder ? 0 : 3
                        Rectangle {
                            required property int index
                            x: 6
                            y: 6 + index * 3.5
                            width: index === 2 ? 5 : 8
                            height: 1.5
                            radius: 0.75
                            color: Theme.textSecondary
                        }
                    }
                }
            }

            Column {
                id: labels
                anchors.left: glyph.right
                anchors.leftMargin: 14
                anchors.right: entryCaret.visible ? entryCaret.left : parent.right
                anchors.rightMargin: entryCaret.visible ? 12 : 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 3

                Text {
                    objectName: "fileBrowserEntryName"
                    width: parent.width
                    text: entryRow.entry ? entryRow.entry.name : ""
                    // A file's name as it is on the drive, never markup.
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(15)
                    font.weight: Font.DemiBold
                    color: Theme.textPrimary
                    Accessible.ignored: true
                }
                Text {
                    width: parent.width
                    visible: text.length > 0
                    text: entryRow.entry ? browser.entrySubtitle(entryRow.entry) : ""
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(12)
                    color: Theme.textSubdued
                    Accessible.ignored: true
                }
            }
            Caret {
                id: entryCaret
                visible: entryRow.folder
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                rotation: -90
                color: Theme.textSubdued
            }
            Rectangle {
                anchors.bottom: parent.bottom
                anchors.left: labels.left
                anchors.right: parent.right
                height: 1
                visible: entryRow.index < d.entries.length - 1 || d.more
                color: Theme.divider
            }
            GatedTapHandler {
                id: entryTap
                gesturePolicy: TapHandler.ReleaseWithinBounds
                onAllowedTap: entryRow.activate()
            }
        }

        footer: Column {
            readonly property Item problemLine: pageProblem
            width: ListView.view ? ListView.view.width : 0
            topPadding: d.more || d.pageProblem.length ? Theme.gap : 0
            spacing: 8

            OutlineButton {
                objectName: "fileBrowserShowMore"
                width: parent.width
                visible: d.more
                enabled: d.pageId === 0
                text: qsTr("Show more")
                onClicked: browser.showMore()
            }
            FileBrowserLine {
                objectName: "fileBrowserPageReading"
                visible: d.readingShown && d.pageId !== 0
                text: qsTr("Reading folder…")
            }
            FileBrowserLine {
                id: pageProblem
                objectName: "fileBrowserPageProblem"
                problem: true
                visible: d.pageProblem.length > 0
                text: d.pageProblem
            }
        }
    }
}
