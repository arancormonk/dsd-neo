// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #646: WholeWordLabel, the shared label of segments and tabs. It wraps
// between words only, draws at the largest whole size (up to the scaled one)
// at which every word fits, never below its normal-text size, and turns into
// one elided line when a word cannot fit even there. Its size is measured at
// `measureWeight`, so the weight it is drawn in never changes it.
Item {
    id: root

    width: 400
    height: 400

    Ui.WholeWordLabel {
        id: label

        width: 200
        text: "Automatic"
    }

    TextMetrics {
        id: wordMetrics
    }

    TestCase {
        name: "WholeWordLabel"
        when: windowShown

        function init() {
            label.width = 200;
            label.text = "Automatic";
            label.basePixelSize = 13;
            label.measureWeight = Font.DemiBold;
            label.font.weight = Font.Normal;
        }
        function cleanup() {
            Ui.Theme.resetFontScale();
        }
        function wordWidth(word, pixelSize, weight) {
            wordMetrics.font.family = label.font.family;
            wordMetrics.font.pixelSize = pixelSize;
            wordMetrics.font.weight = weight;
            wordMetrics.text = word;
            return wordMetrics.advanceWidth;
        }
        function widest(text, pixelSize, weight) {
            var widest = 0;
            for (var word of text.split(/\s+/))
                widest = Math.max(widest, wordWidth(word, pixelSize, weight));
            return widest;
        }

        function test_a_label_that_fits_is_drawn_as_before() {
            for (var scale of [1.0, 1.6, 2.0]) {
                Ui.Theme.fontScale = scale;
                compare(label.font.pixelSize, Math.round(Ui.Theme.fontSize(13)), "a label that fits is drawn at the scaled size at " + scale);
                verify(!label.elided);
                compare(label.wrapMode, Text.WordWrap);
                compare(label.elide, Text.ElideNone);
            }
            verify(label.Accessible.ignored, "the control the label belongs to reads it");
        }

        // The largest whole size at which the widest word fits at the measured
        // weight, and never the size above it.
        function test_a_word_too_wide_shrinks_to_the_largest_size_that_fits() {
            Ui.Theme.fontScale = 2.0;
            var full = Math.round(Ui.Theme.fontSize(13));
            label.width = Math.floor(wordWidth("Automatic", full, Font.DemiBold)) - 8;
            var size = label.font.pixelSize;
            verify(size < full && size >= 13, "drawn at " + size);
            verify(!label.elided);
            verify(wordWidth("Automatic", size, Font.DemiBold) <= label.width, "the word fits at " + size);
            verify(wordWidth("Automatic", size + 1, Font.DemiBold) > label.width, "a size larger also fits, " + (size + 1));
            verify(label.contentWidth <= label.width + 0.5);
            compare(label.lineCount, 1);
        }

        function test_words_wrap_between_and_never_inside() {
            Ui.Theme.fontScale = 1.6;
            label.text = "QPSK / simulcast";
            label.width = Math.ceil(wordWidth("simulcast", Math.round(Ui.Theme.fontSize(13)), Font.DemiBold)) + 2;
            compare(label.font.pixelSize, Math.round(Ui.Theme.fontSize(13)), "the widest word fits at the scaled size");
            verify(label.lineCount >= 2, "the label wraps between its words");
            verify(widest(label.text, label.font.pixelSize, Font.DemiBold) <= label.width);
            verify(label.contentWidth <= label.width + 0.5);
        }

        // Selecting a control draws its label heavier; the size is already
        // measured at that weight, so it does not move.
        function test_the_drawn_weight_does_not_change_the_size() {
            Ui.Theme.fontScale = 2.0;
            label.width = 70;
            var size = label.font.pixelSize;
            label.font.weight = Font.DemiBold;
            compare(label.font.pixelSize, size);
            label.font.weight = Font.Normal;
            compare(label.font.pixelSize, size);
            label.measureWeight = Font.Normal;
            verify(label.font.pixelSize >= size, "a lighter measured weight fits at least as large");
            label.measureWeight = Font.DemiBold;
            compare(label.font.pixelSize, size);
        }

        function test_a_word_that_cannot_fit_elides_at_the_normal_size_data() {
            return [
                {
                    tag: "first",
                    text: "Supercalifragilistic mode"
                },
                {
                    tag: "last",
                    text: "Mode supercalifragilistic"
                }
            ];
        }
        function test_a_word_that_cannot_fit_elides_at_the_normal_size(data) {
            label.text = data.text;
            label.width = 65;
            for (var scale of [1.0, 2.0]) {
                Ui.Theme.fontScale = scale;
                verify(label.elided, "elided at " + scale);
                compare(label.font.pixelSize, 13, "never below the normal-text size");
                compare(label.wrapMode, Text.NoWrap);
                compare(label.elide, Text.ElideRight);
                compare(label.lineCount, 1);
                verify(label.truncated);
                verify(label.contentWidth <= label.width + 0.5);
            }
            label.width = 400;
            verify(!label.elided, "room for the word ends the elision");
            compare(label.font.pixelSize, Math.round(Ui.Theme.fontSize(13)));
            compare(label.wrapMode, Text.WordWrap);
        }

        // The fit follows the text, the width and the text size as they change.
        function test_the_fit_follows_its_inputs() {
            Ui.Theme.fontScale = 2.0;
            label.width = 70;
            var shrunk = label.font.pixelSize;
            verify(shrunk < Math.round(Ui.Theme.fontSize(13)));
            label.text = "On";
            compare(label.font.pixelSize, Math.round(Ui.Theme.fontSize(13)), "a shorter text grows back");
            label.text = "Automatic";
            compare(label.font.pixelSize, shrunk);
            label.width = 300;
            compare(label.font.pixelSize, Math.round(Ui.Theme.fontSize(13)), "a wider label grows back");
            label.width = 70;
            Ui.Theme.fontScale = 1.0;
            compare(label.font.pixelSize, 13);
            label.basePixelSize = 11;
            compare(label.font.pixelSize, 11);
        }

        // Below normal text (a platform scale under 1) the label never grows
        // past its scaled size to reach the floor.
        function test_a_small_text_scale_is_kept() {
            Ui.Theme.fontScale = 0.85;
            label.width = 400;
            compare(label.font.pixelSize, Math.round(Ui.Theme.fontSize(13)));
            label.width = 20;
            verify(label.elided);
            compare(label.font.pixelSize, Math.round(Ui.Theme.fontSize(13)));
        }
    }
}
