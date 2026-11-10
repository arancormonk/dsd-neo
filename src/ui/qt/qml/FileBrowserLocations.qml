// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Issue #830: the file browser's list of places: the inbox, Saved by DSD-neo,
// device storage and drives, the access row that stands for storage before
// access is given, and Choose with another app. The browser (FileBrowserScreen)
// owns every request and the state; this view shows it and calls back.
PlexFlickable {
    id: rootsScroll
    objectName: "fileBrowserRootsScroll"

    // The FileBrowserScreen this view belongs to, and its state.
    required property Item browser
    readonly property QtObject d: browser.internals
    // How far the rows start below the view's top (see rootsColumn).
    readonly property real contentInset: rootsColumn.y
    readonly property Item problemLine: rootsProblemLine

    // The first control of the locations, for the focus a new answer gives.
    function firstRow() {
        for (var i = 0; i < rootRepeater.count; ++i) {
            var row = rootRepeater.itemAt(i);
            if (row && row.focusTarget)
                return row.focusTarget;
        }
        if (accessButton.visible)
            return accessButton;
        return otherAppRow.visible ? otherAppRow : null;
    }
    function revealFocus(focus) {
        Theme.revealFocus(rootsScroll, rootsColumn, focus);
    }

    accessibleName: browser.request ? browser.request.title : ""
    visible: d.view === "roots"
    clip: true
    contentWidth: width
    contentHeight: rootsColumn.y + rootsColumn.height + Theme.screenPadding
    boundsBehavior: Flickable.StopAtBounds

    Column {
        id: rootsColumn
        // Below the top edge by the focus frame's reach, so the first
        // row's frame is not cut off.
        x: (rootsScroll.width - width) / 2
        y: 4
        width: browser.contentWidth
        spacing: Theme.gap

        FileBrowserLine {
            id: rootsProblemLine
            objectName: "fileBrowserRootsProblem"
            problem: true
            visible: d.rootsStatus.length > 0
            text: visible ? browser.problemText(d.rootsStatus) : ""
        }
        OutlineButton {
            objectName: "fileBrowserRootsRetry"
            visible: d.rootsStatus.length > 0
            text: qsTr("Try again")
            onClicked: browser.showRoots()
        }

        UiPanel {
            objectName: "fileBrowserRootsPanel"
            width: parent.width
            height: rootsList.height
            visible: browser.rootRows.length > 0 || browser.accessRowShown

            Column {
                id: rootsList
                width: parent.width

                Repeater {
                    id: rootRepeater
                    model: StableRows {
                        id: rootRowsModel
                        entries: browser.rootRows
                        keyOf: function (root) {
                            return root.key;
                        }
                        lineOf: function (root) {
                            return [root.state, browser.rootTitle(root), browser.rootSubtitle(root)].join("\n");
                        }
                    }
                    Item {
                        id: rootItem
                        required property int index
                        required property string key
                        required property string line
                        readonly property bool live: index >= 0 && index < rootRowsModel.live && index < browser.rootRows.length
                        readonly property var root: live ? browser.rootRows[index] : null
                        readonly property string rootState: root ? root.state : ""
                        readonly property bool ready: rootState === "ready"
                        readonly property bool last: index === browser.rootRows.length - 1 && !browser.accessRowShown
                        readonly property Item focusTarget: ready ? openRow : rootState === "needsRestart" ? closeButton : null
                        width: parent.width
                        height: ready ? openRow.height : rootStateColumn.height + 2 * Theme.cardPadding
                        visible: index < rootRowsModel.shown

                        DisclosureRow {
                            id: openRow
                            objectName: "fileBrowserRoot:" + rootItem.key
                            visible: rootItem.ready
                            title: rootItem.root ? browser.rootTitle(rootItem.root) : ""
                            subtitle: rootItem.root ? browser.rootSubtitle(rootItem.root) : ""
                            showDivider: !rootItem.last
                            onTapped: browser.openRoot(rootItem.root)
                        }
                        // A location that cannot open yet says why, and
                        // a drive that needs a restart offers it.
                        Column {
                            id: rootStateColumn
                            objectName: "fileBrowserRootState:" + rootItem.key
                            visible: !rootItem.ready && rootItem.live
                            x: Theme.cardPadding
                            y: Theme.cardPadding
                            width: parent.width - 2 * Theme.cardPadding
                            spacing: 4

                            ReadableText {
                                width: parent.width
                                text: rootItem.root ? browser.rootTitle(rootItem.root) : ""
                                font.family: Theme.sans
                                font.pixelSize: Theme.fontSize(15)
                                font.weight: Font.DemiBold
                                color: Theme.textPrimary
                                wrapMode: Text.Wrap
                            }
                            FileBrowserLine {
                                text: rootItem.root ? browser.rootSubtitle(rootItem.root) : ""
                            }
                            Item {
                                width: 1
                                height: 6
                                visible: closeButton.visible
                            }
                            OutlineButton {
                                id: closeButton
                                objectName: "fileBrowserClose:" + rootItem.key
                                visible: rootItem.rootState === "needsRestart"
                                enabled: !restartCloser.active
                                text: qsTr("Close DSD-neo")
                                onClicked: browser.askClose(rootItem.key)
                            }
                            FileBrowserLine {
                                visible: closeButton.visible
                                text: qsTr("Then open DSD-neo again.")
                            }
                            FileBrowserLine {
                                objectName: "fileBrowserClosing:" + rootItem.key
                                visible: closeButton.visible && restartCloser.active && d.closeKey === rootItem.key
                                text: qsTr("Closing DSD-neo…")
                            }
                            FileBrowserLine {
                                objectName: "fileBrowserCloseProblem:" + rootItem.key
                                problem: true
                                visible: closeButton.visible && d.closeKey === rootItem.key && d.closeProblem.length > 0
                                text: visible ? d.closeProblem : ""
                            }
                        }
                        Rectangle {
                            anchors.bottom: parent.bottom
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin: Theme.cardPadding
                            height: 1
                            visible: !rootItem.ready && !rootItem.last
                            color: Theme.divider
                        }
                    }
                }

                // Device storage and drives, before access is given.
                Column {
                    id: accessBlock
                    objectName: "fileBrowserAccess"
                    visible: browser.accessRowShown
                    x: Theme.cardPadding
                    width: parent.width - 2 * Theme.cardPadding
                    topPadding: Theme.cardPadding
                    bottomPadding: Theme.cardPadding
                    spacing: 4

                    ReadableText {
                        objectName: "fileBrowserAccessTitle"
                        width: parent.width
                        text: browser.settingsRoute ? qsTr("Download and USB drives") : qsTr("Device storage")
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(15)
                        font.weight: Font.DemiBold
                        color: Theme.textPrimary
                        wrapMode: Text.Wrap
                    }
                    FileBrowserLine {
                        objectName: "fileBrowserAccessSubtitle"
                        text: browser.settingsRoute ? qsTr("Needs All files access") : qsTr("Needs access to device storage")
                    }
                    Item {
                        width: 1
                        height: 6
                    }
                    OutlineButton {
                        id: accessButton
                        objectName: "fileBrowserAllowAccess"
                        text: qsTr("Allow access")
                        onClicked: browser.askAccess()
                    }
                    FileBrowserLine {
                        objectName: "fileBrowserAccessRevoke"
                        visible: browser.settingsRoute
                        text: qsTr("Turning All files access off later closes DSD-neo (Android does that).")
                    }
                    FileBrowserLine {
                        objectName: "fileBrowserAccessBlocked"
                        problem: true
                        visible: d.accessBlocked
                        text: qsTr("Allow Files and media for DSD-neo in App info, then come back.")
                    }
                    Column {
                        objectName: "fileBrowserAccessHelp"
                        visible: d.accessHelp && browser.adbOffered
                        width: parent.width
                        topPadding: 8
                        spacing: 8

                        // Android 11 and 12 offer the switch under App info
                        // too, where Settings may have sent the user.
                        FileBrowserLine {
                            objectName: "fileBrowserAppInfoStep"
                            visible: browser.access.apiLevel >= 30 && browser.access.apiLevel <= 32
                            text: qsTr("In App info, open Permissions › Files and media, then allow management of all files.")
                        }
                        FileBrowserLine {
                            text: qsTr("If Settings had no switch for DSD-neo, allow it from a computer:")
                        }
                        FileBrowserCommand {
                            objectName: "fileBrowserAdbCommand"
                            copyName: "fileBrowserCopyAdb"
                            copied: d.copied.length > 0 && d.copied === copyText
                            onCopyRequested: function (text) {
                                browser.copy(text);
                            }
                            lines: [browser.adbAccessLine]
                        }
                    }
                }
            }
        }

        UiPanel {
            width: parent.width
            height: otherAppRow.height
            visible: browser.request !== null && browser.request.allowOtherApp === true

            DisclosureRow {
                id: otherAppRow
                objectName: "fileBrowserOtherApp"
                visible: parent.visible
                title: qsTr("Choose with another app")
                subtitle: ""
                onTapped: FileBrowser.otherApp(d.token)
            }
        }
    }
}
