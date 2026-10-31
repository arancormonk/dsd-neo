// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

ModalSheet {
    id: sheet
    objectName: "networkSheet"
    panelObjectName: "networkSheetPanel"
    property var network: p25Network
    onVisibleChanged: network.active = visible
    Component.onDestruction: network.active = false
    function open() {
        visible = true;
    }

    function hex(value, width) {
        return Number(value).toString(16).toUpperCase().padStart(width, "0");
    }
    function rowText(section, row) {
        if (section === 0) {
            return (row.freqHz / 1000000).toFixed(6) + " MHz" + (row.isCurrentCc ? " [CC]" : "") + (row.isCandidate ? " [C]" : "") + " · SYS:" + hex(row.sysid, 3) + " RFSS:" + row.rfss + " Site:" + row.site + (row.wacnValid ? " WACN:" + hex(row.wacn, 5) : "") + (row.lraValid ? " LRA:" + hex(row.lra, 2) : "") + " CFVA:" + row.cfvaText;
        }
        if (section === 1)
            return "SG:" + row.sgid + (row.isPatch ? " · Patch" : " · Simulselect") + (row.groups.length ? " · TG:" + row.groups.join(", ") : "") + (row.radios.length ? " · RID:" + row.radios.join(", ") : "");
        return "RID:" + row.rid + (section === 2 ? " · TG:" + row.tg : "");
    }
    // Issue #782: with a screen reader on, each section in a fixed order of
    // its entries: neighbours by frequency and site identity, patches by
    // SGID, affiliations by RID and talkgroup, radios by RID. Most recent
    // first moves a row with each entry heard again, and Qt's bridge tells
    // Android nothing of a move, so TalkBack swiped the rows in an order
    // they had left: it skipped, repeated and looped. Without a screen
    // reader the most recent comes first.
    function readingOrder(section, rows) {
        var ordered = rows.slice();
        ordered.sort(function (a, b) {
            if (section === 0)
                return a.freqHz - b.freqHz || a.sysid - b.sysid || a.rfss - b.rfss || a.site - b.site
                    || (a.wacnValid ? a.wacn : -1) - (b.wacnValid ? b.wacn : -1);
            if (section === 1)
                return a.sgid - b.sgid;
            return a.rid - b.rid || (section === 2 ? a.tg - b.tg : 0);
        });
        return ordered;
    }
    // What tells one entry from another, so its row follows it (StableRows).
    // Neighbours are told apart as the decoder keeps them: two sites may
    // announce one frequency.
    function entryKey(section, row) {
        if (section === 0)
            return [row.freqHz, row.sysid, row.rfss, row.site, row.wacnValid ? row.wacn : "-"].join(":");
        if (section === 1)
            return String(row.sgid);
        return row.rid + (section === 2 ? ":" + row.tg : "");
    }

    MicroLabel {
        text: qsTr("P25 Network")
    }
    Repeater {
        model: [qsTr("Neighbours"), qsTr("Patches"), qsTr("Affiliations"), qsTr("Radios")]
        Column {
            id: section
            required property int index
            required property string modelData
            readonly property var listed: [sheet.network.neighbours, sheet.network.patches, sheet.network.affiliations, sheet.network.radios][index]
            readonly property var rows: screenReader.active ? sheet.readingOrder(index, listed) : listed
            width: parent.width
            spacing: 8
            MicroLabel {
                text: section.modelData
            }
            // In the tree while hidden (issue #782), so that it shows when the
            // section empties: that show is what makes Android's bridge refresh
            // its copy of the sheet (the held rows below leave the tree without
            // an event it sends on), and a swipe then reaches this line.
            ReadableText {
                visible: sectionRows.live === 0
                text: qsTr("(none announced)")
                Accessible.ignored: !navigationAllowed
                Accessible.name: navigationAllowed && visible ? text : ""
                font.family: Theme.sans
                color: Theme.textSubdued
            }
            // Issue #782: a row per entry, kept across refreshes (StableRows): a
            // refresh hands a new list, and a list model would rebuild every row
            // a screen reader is told about. Without a screen reader each
            // section lists its entries most recent first, so an entry heard
            // again moves to the top: its row moves with it, and no row is
            // renamed for it. A row is renamed only when its own entry's words
            // change, or a new entry takes the row of one that left.
            //
            // A target with fewer of these entries, or none (a hop between P25
            // sites of other sizes, or to a DMR or analog target), keeps the
            // rows past the end of its list, out of the tree and out of sight,
            // rather than removing them: removed or hidden, every row would
            // raise a hide, and again a show on the way back, each a
            // main-thread task with blocking calls into the Qt thread on
            // Android. Leaving the tree raises only ObjectDestroyed, which
            // Android's bridge does not send on. A held row is transparent, so
            // Qt reports it invisible, and unnamed, so a copy Android still
            // holds, or keeps focus on, reads nothing. Unnaming a row is a call
            // into Java that returns at once unless TalkBack is on that row.
            Column {
                id: sectionList
                // The bottom of the last row drawn. Held rows come last, below
                // it, and keep their size, so holding one moves nothing.
                property real shownBottom: 0
                function place() {
                    var last = sectionRows.shown > 0 ? sectionRepeater.itemAt(sectionRows.shown - 1) : null;
                    shownBottom = last ? last.y + last.height : 0;
                }
                width: parent.width
                spacing: 8
                height: shownBottom
                Connections {
                    target: sectionRows
                    function onShownChanged() {
                        sectionList.place();
                    }
                }
                Repeater {
                    id: sectionRepeater
                    model: StableRows {
                        id: sectionRows
                        entries: section.rows
                        holdRows: true
                        keyOf: function (row) {
                            return sheet.entryKey(section.index, row);
                        }
                        lineOf: function (row) {
                            return sheet.rowText(section.index, row);
                        }
                    }
                    ReadableText {
                        required property int index
                        required property string line
                        width: parent.width
                        text: line
                        // A row being removed has index -1 for an instant: it stays hidden and unnamed.
                        opacity: index >= 0 && index < sectionRows.shown ? 1 : 0
                        onYChanged: sectionList.place()
                        onHeightChanged: sectionList.place()
                        Accessible.ignored: !navigationAllowed || !visible || index < 0 || index >= sectionRows.live
                        Accessible.name: navigationAllowed && visible && index >= 0 && index < sectionRows.live ? text : ""
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize(12)
                        color: Theme.textPrimary
                    }
                }
            }
        }
    }
    OutlineButton {
        objectName: "networkDone"
        width: parent.width
        text: qsTr("Done")
        onClicked: sheet.visible = false
    }
}
