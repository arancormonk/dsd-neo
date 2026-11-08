// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #646 (B2): a segment's label never breaks a word in the middle. With
// large text a word wider than its segment was split across two lines
// ("Automatic", "simulcast", "System"), so every label set the app shows is
// measured here in the screen or sheet that holds it, at its real width: each
// word must fit on a line of its own at the size the label is drawn at, in the
// heavier weight a selected segment uses as well as the normal one, and the
// drawn label must stay inside its segment. A label with a word that cannot
// fit even at normal size (a long token in a narrow control) is one elided
// line instead, and the segment still reads the whole label.
Item {
    id: root

    width: 360
    height: 800

    Loader {
        id: host

        width: root.width
        height: root.height
        active: false
    }

    Ui.SegmentedControl {
        id: narrow

        y: 700
        width: 160
    }

    TextMetrics {
        id: wordMetrics
    }

    TestCase {
        id: tc

        name: "SegmentedWholeWords"
        when: windowShown

        readonly property var widths: [320, 360]
        readonly property var scales: [1.0, 1.6, 2.0]

        function cleanup() {
            host.active = false;
            host.source = "";
            Ui.Theme.resetFontScale();
            root.width = 360;
        }

        // Every SegmentedControl under an item, shown or not: a hidden one still
        // has its real width, and is shown as soon as its condition changes.
        function controlsIn(item) {
            var found = [];
            (function walk(node) {
                for (var child of node.children) {
                    if (child.measureLabels !== undefined && child.labelHeight !== undefined && child.model !== undefined)
                        found.push(child);
                    walk(child);
                }
            })(item);
            return found;
        }
        function segmentsOf(control) {
            var found = [];
            (function walk(node) {
                for (var child of node.children) {
                    if (child.modelData !== undefined && child.active !== undefined)
                        found.push(child);
                    else
                        walk(child);
                }
            })(control);
            return found;
        }
        function labelOf(segment) {
            for (var child of segment.children) {
                if (child.text !== undefined && child.lineCount !== undefined)
                    return child;
            }
            return null;
        }
        function widestWord(label, weight) {
            wordMetrics.font.family = label.font.family;
            wordMetrics.font.pixelSize = label.font.pixelSize;
            wordMetrics.font.weight = weight;
            var widest = {
                word: "",
                width: 0
            };
            for (var word of String(label.text).split(/\s+/)) {
                if (word.length === 0)
                    continue;
                wordMetrics.text = word;
                if (wordMetrics.advanceWidth > widest.width)
                    widest = {
                        word: word,
                        width: wordMetrics.advanceWidth
                    };
            }
            return widest;
        }
        // The oracle: every word fits on a line of its own at the drawn size
        // (counting lines would pass "QPSK / simulcast" split as "QPSK / simul" +
        // "cast"), and the drawn label sits inside its segment. `mayElide` allows
        // the one-line fallback, which must then be one truncated line.
        function checkSegment(where, control, segment, mayElide) {
            var label = labelOf(segment);
            verify(label !== null, where + ": no label");
            var tag = where + " \"" + label.text + "\" at " + label.font.pixelSize + " px in " + label.width.toFixed(1);
            compare(segment.Accessible.name, segment.modelData, tag + ": the segment does not read its whole label");
            var fits = true;
            for (var weight of [Font.Normal, Font.DemiBold]) {
                var widest = widestWord(label, weight);
                if (widest.width > label.width + 0.01) {
                    fits = false;
                    verify(mayElide, tag + ": \"" + widest.word + "\" is " + widest.width.toFixed(1) + " px wide at weight " + weight + ", so it breaks mid-word");
                }
            }
            if (!fits) {
                compare(label.lineCount, 1, tag + ": a label with a word that cannot fit is one line");
                verify(label.truncated, tag + ": the one line is elided");
            }
            verify(label.contentWidth <= label.width + 0.5, tag + ": the text is wider than its label (" + label.contentWidth + ")");
            var at = label.mapToItem(segment, 0, 0);
            verify(at.x >= -0.5 && at.x + label.width <= segment.width + 0.5, tag + ": the label leaves its segment sideways");
            verify(at.y >= -0.5 && at.y + label.height <= segment.height + 0.5, tag + ": the label leaves its segment vertically (" + at.y.toFixed(1) + " + " + label.height.toFixed(1) + " of " + segment.height.toFixed(1) + ")");
        }
        // Each segment selected in turn, so every label is checked in the
        // selected weight it is drawn in, not only the measured one.
        function checkControl(where, control, mayElide) {
            var segments = segmentsOf(control);
            compare(segments.length, control.model.length, where + ": one segment per choice");
            var kept = control.currentIndex;
            for (var selected = 0; selected < segments.length; ++selected) {
                control.currentIndex = selected;
                wait(0);
                var tallest = 0;
                for (var segment of segments) {
                    checkSegment(where + " (segment " + selected + " selected)", control, segment, mayElide);
                    tallest = Math.max(tallest, labelOf(segment).height);
                }
                // The segment's height follows the label as drawn.
                tryCompare(control, "implicitHeight", Math.max(54, tallest + 30), 1000, where + ": the control's height does not follow its labels");
            }
            control.currentIndex = kept;
        }

        // The 20 SegmentedControls the app has, by the file that holds them
        // (the 21st use is the QtTest fixture in tst_input_commit.qml, a "One /
        // Two" pair checked below).
        function test_every_label_set_keeps_whole_words_data() {
            return [
                {
                    tag: "RadioSheet",
                    file: "RadioSheet.qml",
                    count: 3
                },
                {
                    tag: "ToneFilterSheet",
                    file: "ToneFilterSheet.qml",
                    count: 2
                },
                {
                    tag: "SquelchSheet",
                    file: "SquelchSheet.qml",
                    count: 2
                },
                {
                    tag: "PassbandSheet",
                    file: "PassbandSheet.qml",
                    count: 1
                },
                {
                    tag: "ReceivePolicySheet",
                    file: "ReceivePolicySheet.qml",
                    count: 2
                },
                {
                    tag: "ScanListScreen",
                    file: "ScanListScreen.qml",
                    count: 5
                },
                {
                    tag: "SettingsScreen",
                    file: "SettingsScreen.qml",
                    count: 1
                },
                {
                    tag: "RadioReferenceScreen",
                    file: "RadioReferenceScreen.qml",
                    count: 1
                },
                {
                    tag: "WizardScreen",
                    file: "WizardScreen.qml",
                    count: 2
                },
                {
                    tag: "ImportsScreen",
                    file: "ImportsScreen.qml",
                    count: 1
                }
            ];
        }
        function test_every_label_set_keeps_whole_words(data) {
            testContext.setHostRunning(true);
            testContext.setMetric("radioInput", true);
            for (var width of tc.widths) {
                root.width = width;
                Ui.Theme.fontScale = 1.0;
                host.source = uiDir + "/" + data.file;
                host.active = true;
                tryCompare(host, "status", Loader.Ready);
                var screen = host.item;
                if (typeof screen.open === "function")
                    screen.open();
                else
                    screen.visible = true;
                var controls = controlsIn(screen);
                compare(controls.length, data.count, data.file + " holds " + data.count + " segmented controls");
                for (var scale of tc.scales) {
                    Ui.Theme.fontScale = scale;
                    wait(0);
                    for (var i = 0; i < controls.length; ++i) {
                        var where = data.file + " #" + i + " " + JSON.stringify(controls[i].model) + " at " + width + " x " + scale;
                        verify(controls[i].width > 0, where + ": no width");
                        checkControl(where, controls[i], false);
                    }
                }
                host.active = false;
                host.source = "";
            }
            testContext.setMetric("radioInput", false);
            testContext.setHostRunning(false);
        }

        // A word too long for its segment even at normal size, first or last:
        // the label is one elided line inside the segment rather than a word
        // split over two, and the segment still reads all of it.
        function test_a_word_that_cannot_fit_elides_data() {
            return [
                {
                    tag: "long token first",
                    model: ["Supercalifragilistic mode", "Off"]
                },
                {
                    tag: "long token last",
                    model: ["Mode supercalifragilistic", "On"]
                },
                {
                    tag: "fits",
                    model: ["One", "Two"]
                }
            ];
        }
        function test_a_word_that_cannot_fit_elides(data) {
            narrow.model = data.model;
            for (var scale of tc.scales) {
                Ui.Theme.fontScale = scale;
                wait(0);
                checkControl(data.tag + " at " + scale, narrow, true);
                var label = labelOf(segmentsOf(narrow)[0]);
                if (data.tag === "fits")
                    verify(!label.truncated, "a label whose words fit is not elided");
                else
                    verify(label.truncated, "the long token is elided at " + scale);
            }
        }
    }
}
