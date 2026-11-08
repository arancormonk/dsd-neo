// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Issue #646: a short control label (a segment's, a tab's) that never breaks a
// word in the middle. It wraps between words only. When its widest word is
// wider than the label at the scaled text size, it is drawn at the largest
// whole pixel size at which every word fits, never smaller than its size at
// normal text, so a larger text preference still enlarges it as far as the
// space allows. A word that does not fit even then (a long token in a narrow
// control) makes the whole label one elided line: Qt elides only the last line
// it lays out, so an earlier wrapped line could not be cut short. Words are
// measured in `measureWeight`, the heaviest weight the label is shown in, so
// selecting its control never changes its size. Its control reads the whole
// text as its own name, so the label stays out of the accessibility tree.
// The fit is worked out when the text, width or text size changes, never per
// frame.
Text {
    id: label

    // The size at normal text, before the platform's text scale.
    property real basePixelSize: 13
    property int measureWeight: Font.DemiBold
    // True while a word does not fit even at the normal-text size, so the
    // label is one elided line.
    readonly property bool elided: !fit.wordsFit

    readonly property real scaledPixelSize: Theme.fontSize(basePixelSize)

    font.family: Theme.sans
    font.pixelSize: fit.pixelSize
    wrapMode: elided ? Text.NoWrap : Text.WordWrap
    elide: elided ? Text.ElideRight : Text.ElideNone
    // Read by its control (see above).
    Accessible.ignored: true

    QtObject {
        id: fit

        property real pixelSize: label.scaledPixelSize
        property bool wordsFit: true
        property string family: ""
    }

    FontMetrics {
        id: measure
    }

    function widestWord(words, pixelSize) {
        // At the whole size the label is drawn at: a Text rounds a fractional
        // pixel size (20.8 at 1.6 is drawn at 21), where Qt.font truncates it.
        measure.font = Qt.font({
            family: label.font.family,
            pixelSize: Math.round(pixelSize),
            weight: label.measureWeight
        });
        var widest = 0;
        for (var word of words)
            widest = Math.max(widest, measure.advanceWidth(word));
        return widest;
    }
    function refit() {
        fit.family = font.family;
        var full = scaledPixelSize;
        var floor = Math.min(basePixelSize, full);
        var words = String(text).split(/\s+/).filter(function (word) {
            return word.length > 0;
        });
        var size = full;
        var fits = true;
        // Unsized (not laid out yet) keeps the scaled size until it has a width.
        if (width > 0 && words.length > 0 && widestWord(words, full) > width) {
            fits = false;
            // The next size down is the whole pixel below the drawn one.
            for (var candidate = Math.round(full) - 1; candidate >= floor; --candidate) {
                if (widestWord(words, candidate) <= width) {
                    size = candidate;
                    fits = true;
                    break;
                }
            }
            if (!fits)
                size = floor;
        }
        if (Math.abs(fit.pixelSize - size) > 0.01)
            fit.pixelSize = size;
        fit.wordsFit = fits;
    }

    onTextChanged: refit()
    onWidthChanged: refit()
    onScaledPixelSizeChanged: refit()
    onBasePixelSizeChanged: refit()
    onMeasureWeightChanged: refit()
    // The size this sets changes the font too; only a new face needs a refit.
    onFontChanged: function () {
        if (label.font.family !== fit.family)
            label.refit();
    }
    Component.onCompleted: refit()
}
