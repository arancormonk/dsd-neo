// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

pragma Singleton
import QtQuick

// Fixed line boxes (issue #901, D10): the height a line of text gets in a
// layout that a budget counts on, read from the font alone, never from the
// text it shows. A line's own height follows the faces that draw it: a CJK
// name or an emoji comes from a fallback face whose ascent and descent differ
// from the primary's, so rows sized by their text were 59, 64 or 65 px high
// at the same text size, and a budget measured with "Ag" was wrong for two
// of them.
//
// The box is the height of a probe line that holds a Latin ascender and
// descender, a CJK ideograph and an emoji, so each of those faces is in it,
// ceiled to whole pixels. One probe per font family, pixel size and weight,
// made on first use and kept, shared by every row; a new text size or font
// mapping (Theme.fontRevision) is a new probe. Decided limit: a rarer script
// whose glyphs stand taller than the box (Tibetan stacks) is drawn centred
// in it and unclipped, into the row's padding.
//
// The call row's own measures are here as well, so the Monitor's budget and
// the row delegates use the same numbers (CallRow).
//
// An Item, never shown, so the probes it makes have an item for a parent.
Item {
    id: boxes

    visible: false
    width: 0
    height: 0

    // Latin, CJK and emoji, each drawn by its own face.
    readonly property string probeText: "Ag返🚒"

    // Probes made so far, keyed by font: never shown, only measured. A plain
    // cache: a binding that reads a box depends on the probe's own size.
    property var probes: ({})
    readonly property Component probeComponent: Component {
        Text {
            visible: false
            textFormat: Text.PlainText
        }
    }

    // A Text's font.pixelSize rounds a fractional size (17.6 draws at 18)
    // and Qt.font() truncates it (17), so sizes are rounded here first: a
    // box measured at the truncated size was a pixel short of the line it
    // holds at text 1.6.
    function pixelSizeOf(pixelSize) {
        return Math.round(pixelSize);
    }
    function probe(text, family, pixelSize, weight) {
        pixelSize = pixelSizeOf(pixelSize);
        var key = [Theme.fontRevision, family, pixelSize, weight, text].join("|");
        var made = probes[key];
        if (!made) {
            made = probeComponent.createObject(boxes, {
                text: text,
                font: Qt.font({family: family, pixelSize: pixelSize, weight: weight})
            });
            probes[key] = made;
        }
        return made;
    }

    // The height of one line in this font, whatever it says.
    function lineHeight(family, pixelSize, weight) {
        return Math.ceil(probe(probeText, family, pixelSize, weight).implicitHeight);
    }
    // The width of @p text in this font; of its widest line when it has
    // several.
    function textWidth(text, family, pixelSize, weight) {
        return Math.ceil(probe(text, family, pixelSize, weight).implicitWidth);
    }

    // ---- The call row (CallRow) ----
    readonly property font callRowNameFont: Qt.font({family: Theme.sans, pixelSize: pixelSizeOf(Theme.fontSize(16)), weight: Font.DemiBold})
    readonly property font callRowMetaFont: Qt.font({family: Theme.mono, pixelSize: pixelSizeOf(Theme.fontSize(12)), weight: Font.Normal})

    // The tags and the glyph a row can carry at its right, at their natural
    // size: what the worst-case right side is measured with.
    readonly property Item emergencyProbe: EmergencyTag {
        visible: false
    }
    readonly property Item encProbe: EncTag {
        visible: false
    }
    readonly property Item recordingProbe: PlayGlyph {
        visible: false
    }

    function callRowNameBox() {
        return lineHeight(Theme.sans, Theme.fontSize(16), Font.DemiBold);
    }
    function callRowMetaBox() {
        return lineHeight(Theme.mono, Theme.fontSize(12), Font.Normal);
    }
    // The narrow form's third line, the right side's: the meta line's box,
    // or a tag's height where a tag stands taller than that.
    function callRowClusterBox() {
        return Math.max(callRowMetaBox(), Math.ceil(emergencyProbe.implicitHeight), Math.ceil(encProbe.implicitHeight), Math.ceil(recordingProbe.implicitHeight));
    }
    // Above and below the lines, each.
    function callRowPadding(compact) {
        return compact ? 6 : 12;
    }
    // A call row's height: its lines and padding, at least the floor (a
    // default row is never under Theme.rowHeight, a compact one never under a
    // touch target). The narrow form has the right side as a third line.
    function callRowHeight(compact, narrow) {
        var lines = callRowNameBox() + 3 + callRowMetaBox() + (narrow ? 3 + callRowClusterBox() : 0);
        return Math.max(compact ? Theme.minimumTouchSize : Theme.rowHeight, lines + 2 * callRowPadding(compact));
    }
    // The widest right side a row can show: EMERGENCY, the recording glyph and
    // the time or ENC, with the gaps CallRow puts between them. @p timeProbe is
    // the widest time the view shows, one per line when there are several.
    function callRowClusterWidth(timeProbe) {
        var time = textWidth(timeProbe, Theme.mono, Theme.fontSize(12), Font.Normal);
        return Math.ceil(emergencyProbe.implicitWidth) + 8 + Math.ceil(recordingProbe.implicitWidth) + 8 + Math.max(Math.ceil(encProbe.implicitWidth), time);
    }
    // Whether a row @p width wide uses the narrow form: where the worst-case
    // right side would leave the name less than max(96, 35% of the row), it
    // moves to a line of its own. Width and fonts only, never what a row
    // shows, so every row of a view takes the same form and the same height.
    function callRowNarrow(width, timeProbe) {
        return width - 18 - 12 - callRowClusterWidth(timeProbe) - 18 < Math.max(96, 0.35 * width);
    }
}
