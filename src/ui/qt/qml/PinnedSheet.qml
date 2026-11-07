// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Every shortcut pinned to Listen (issue #801), opened from the Pinned strip's
// More: the ones the strip has no room for, and the ones whose system, site or
// scan list is not found right now, which the strip leaves out. Each row starts
// its shortcut or unpins it; a shortcut not found can only be unpinned, and this
// is the one place that can always do that. A start closes the sheet first.
ModalSheet {
    id: sheet
    objectName: "pinnedSheet"
    panelObjectName: "pinnedSheetPanel"
    accessibleName: qsTr("Pinned")

    // What a row starts: Home passes it on to Main's startPin().
    signal startRequested(string kind, string uid)

    // The words a shortcut not found is shown with: its last label this run,
    // else what kind of thing it was.
    function missingText(entry) {
        if (entry.label.length > 0)
            return qsTr("%1 (not found)").arg(entry.label);
        if (entry.kind === "scan")
            return qsTr("Scan list not found");
        if (entry.kind === "site")
            return qsTr("Site not found");
        return qsTr("Saved system not found");
    }
    function shownText(entry) {
        return entry.found ? entry.label : missingText(entry);
    }
    function unpinName(entry) {
        if (entry.label.length > 0)
            return qsTr("Unpin %1").arg(entry.label);
        if (entry.kind === "scan")
            return qsTr("Unpin the missing scan list");
        if (entry.kind === "site")
            return qsTr("Unpin the missing site");
        return qsTr("Unpin the missing saved system");
    }
    function start(key) {
        var entry = entryFor(key);
        if (entry === null || !entry.found)
            return;
        visible = false;
        startRequested(entry.kind, entry.uid);
    }
    function entryFor(key) {
        for (var entry of listenPins.entries) {
            if (entry.key === key)
                return entry;
        }
        return null;
    }

    ReadableText {
        objectName: "pinnedSheetTitle"
        heading: true
        width: parent.width
        text: qsTr("Pinned")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textPrimary
        font.pixelSize: Theme.fontSize(20)
    }
    ReadableText {
        objectName: "pinnedSheetHint"
        width: parent.width
        text: qsTr("Up to 6. Pin more with Pin to Listen, in the More menu of a system, scan list, Signal search or Explore, or in a site list.")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(13)
    }
    ReadableText {
        objectName: "pinnedSheetEmpty"
        width: parent.width
        visible: listenPins.count === 0
        text: qsTr("Nothing is pinned.")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(14)
    }

    // A row per pin, kept on its pin while the sheet is open (StableRows), so
    // an unpin or a rename moves or renames rows rather than rebuilding them.
    // Closed, the sheet holds no rows (issue #782).
    Column {
        objectName: "pinnedSheetRows"
        width: parent.width
        visible: listenPins.count > 0
        spacing: 12
        Repeater {
            model: StableRows {
                id: rows
                entries: sheet.visible ? listenPins.entries : []
                keyOf: function (entry) {
                    return entry.key;
                }
                // The row's words: what it shows, its start's name, its
                // Unpin's name, and whether it can start.
                lineOf: function (entry) {
                    return JSON.stringify([sheet.shownText(entry), entry.found ? entry.accessibleName : sheet.shownText(entry),
                                           sheet.unpinName(entry), entry.found]);
                }
            }
            Item {
                id: pinRow
                required property int index
                required property string key
                required property string line
                readonly property var parts: JSON.parse(line)
                readonly property bool found: parts[3] === true
                // A row past the end of a shrinking list goes at the end of
                // the turn; until then it is not drawn and reads as nothing.
                opacity: index >= 0 && index < rows.shown ? 1 : 0
                width: parent.width
                height: Math.max(startButton.height, unpinButton.height)

                OutlineButton {
                    id: startButton
                    objectName: "pinnedSheetStart"
                    readonly property string pinKey: pinRow.key
                    width: pinRow.width - unpinButton.width - 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: pinRow.parts[0]
                    accessibleName: pinRow.parts[1]
                    wrapMode: Text.WordWrap
                    enabled: pinRow.found && !decoderHost.transitioning
                    onClicked: sheet.start(pinRow.key)
                }
                OutlineButton {
                    id: unpinButton
                    objectName: "pinnedSheetUnpin"
                    readonly property string pinKey: pinRow.key
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    // Whole pixels: at a fractional x the sheet's clip shaves
                    // off the right border.
                    width: Math.ceil(implicitWidth)
                    text: qsTr("Unpin")
                    accessibleName: pinRow.parts[2]
                    onClicked: listenPins.unpinKey(pinRow.key)
                }
            }
        }
    }

    OutlineButton {
        objectName: "pinnedSheetDone"
        width: parent.width
        text: qsTr("Done")
        onClicked: sheet.visible = false
    }
}
