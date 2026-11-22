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
    // cache: a binding that reads a box depends on the probe's own size. It
    // holds the current font revision's probes only: one per family, size and
    // weight a box is read in, with the line probe text or one of the views'
    // fixed time probes (callRowTimeWidth()), never a call's text. A new
    // revision (the app bumps it on every font refresh, also back to a
    // setting it had) destroys the older ones; every binding that read them
    // reads the revision too, so it runs again and takes a new probe. Both
    // objects are changed in place, never reassigned: a property written in
    // the binding that reads it would be a binding loop.
    readonly property var probes: ({})
    readonly property var probesMade: ({revision: -1})
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
    // Made inside the binding that first reads a box, deliberately: the
    // binding then depends on the probe it gets, and a later read of the same
    // key finds it in the cache.
    function probe(text, family, pixelSize, weight) {
        pixelSize = pixelSizeOf(pixelSize);
        var revision = Theme.fontRevision;
        if (revision !== probesMade.revision) {
            for (var old of Object.keys(probes)) {
                probes[old].destroy();
                delete probes[old];
            }
            probesMade.revision = revision;
        }
        var key = [revision, family, pixelSize, weight, text].join("|");
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
    // Read a box through a property or a Binding, never inline in a
    // ReadableText's own height binding: read there, the first probe of a
    // size warned of a binding loop (MonitorScreen's otherIdentityBox).
    function lineHeight(family, pixelSize, weight) {
        return Math.ceil(probe(probeText, family, pixelSize, weight).implicitHeight);
    }

    // ---- The call row (CallRow) ----
    readonly property font callRowNameFont: Qt.font({family: Theme.sans, pixelSize: pixelSizeOf(Theme.fontSize(16)), weight: Font.DemiBold})
    readonly property font callRowMetaFont: Qt.font({family: Theme.mono, pixelSize: pixelSizeOf(Theme.fontSize(12)), weight: Font.Normal})

    function callRowNameBox() {
        return lineHeight(Theme.sans, Theme.fontSize(16), Font.DemiBold);
    }
    function callRowMetaBox() {
        return lineHeight(Theme.mono, Theme.fontSize(12), Font.Normal);
    }
    // The time slot at a row's right: the widest of @p timeProbe's lines (the
    // widest times the view shows, one per line) in the meta font. Fixed per
    // view, so a ticking age never changes what a row draws.
    function callRowTimeWidth(timeProbe) {
        return Math.ceil(probe(timeProbe, Theme.mono, Theme.fontSize(12), Font.Normal).implicitWidth);
    }
    // Above and below the lines, each.
    function callRowPadding(compact) {
        return compact ? 6 : 12;
    }
    // A call row's height: its two lines and padding, at least the floor (a
    // default row is never under Theme.rowHeight, a compact one never under a
    // touch target). Fonts and density only: never the row's width or call.
    function callRowHeight(compact) {
        var lines = callRowNameBox() + 3 + callRowMetaBox();
        return Math.max(compact ? Theme.minimumTouchSize : Theme.rowHeight, lines + 2 * callRowPadding(compact));
    }
}
