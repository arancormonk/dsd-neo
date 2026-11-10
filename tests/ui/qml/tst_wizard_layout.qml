// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// #830: the wizard at the largest text on a narrow phone. The title stops short
// of the step progress and wraps, the header grows to hold it, and the back
// chevron and the progress stay level with its first line. Each step's question
// wraps inside the form instead of running off the screen.
Item {
    id: root

    width: 360
    height: 800

    Loader {
        id: wizardLoader

        anchors.fill: parent
        source: uiDir + "/WizardScreen.qml"
    }

    TestCase {
        id: tc

        name: "WizardLayout"
        when: windowShown

        readonly property var wizard: wizardLoader.item

        function init() {
            verify(tc.wizard !== null, "WizardScreen.qml failed to load");
            tc.wizard.visible = true;
            tc.wizard.openForAdd(false);
        }
        function cleanup() {
            Ui.Theme.fontScale = 1;
            root.width = 360;
            tc.wizard.openForAdd(false);
        }
        // The text item under @p item that shows @p text.
        function textItem(item, text) {
            if (item.text === text)
                return item;
            for (var i = 0; i < item.children.length; ++i) {
                var found = textItem(item.children[i], text);
                if (found !== null)
                    return found;
            }
            return null;
        }
        function leftEdge(item) {
            return item.mapToItem(tc.wizard, 0, 0).x;
        }
        function rightEdge(item) {
            return item.mapToItem(tc.wizard, item.width, 0).x;
        }
        function centreY(item) {
            return item.mapToItem(tc.wizard, 0, item.height / 2).y;
        }

        function test_header_at_large_font_data() {
            return [{tag: "320", width: 320}, {tag: "360", width: 360}];
        }
        function test_header_at_large_font(data) {
            root.width = data.width;
            Ui.Theme.fontScale = 1.6;
            var title = textItem(tc.wizard, "Add system");
            verify(title !== null, "the title is missing");
            var header = title.parent;
            var back = null;
            var progress = null;
            for (var i = 0; i < header.children.length; ++i) {
                var child = header.children[i];
                if (child.icon === "back")
                    back = child;
                else if (child !== title && child.children.length >= 3)
                    progress = child;
            }
            verify(back !== null && progress !== null, "the back chevron or the progress is missing");
            waitForRendering(header);
            // A clear gap, not only no overlap: at 360 the title ended 2 px
            // from the progress and read as one with it.
            var gap = leftEdge(progress) - (title.mapToItem(tc.wizard, 0, 0).x + title.contentWidth);
            verify(gap >= 12, "the title ends " + gap + " px from the progress");
            verify(rightEdge(title) <= leftEdge(progress), "the title runs " + (rightEdge(title) - leftEdge(progress)) + " px into the progress");
            verify(title.contentWidth <= title.width, "the title's text is wider than the title");
            verify(header.height >= title.height, "the header is " + header.height + " px for a " + title.height + " px title");
            var lineCentre = title.mapToItem(tc.wizard, 0, title.height / title.lineCount / 2).y;
            verify(Math.abs(centreY(back) - lineCentre) < 1, "the back chevron is not level with the title's first line");
            verify(Math.abs(centreY(progress) - lineCentre) < 1, "the progress is not level with the title's first line");
            var step = textItem(tc.wizard, "Step 1 of 3 · Source");
            verify(step !== null, "the step label is missing");
            verify(step.mapToItem(tc.wizard, 0, 0).y >= title.mapToItem(tc.wizard, 0, title.height).y,
                   "the step label overlaps the title");
        }

        function test_questions_wrap_at_large_font_data() {
            var rows = [];
            var questions = ["Where does the signal come from?", "What should we decode?", "What should we call it?"];
            for (var width of [320, 360]) {
                for (var step = 0; step < questions.length; ++step)
                    rows.push({tag: width + " step " + (step + 1), width: width, step: step, question: questions[step]});
            }
            return rows;
        }
        function test_questions_wrap_at_large_font(data) {
            root.width = data.width;
            Ui.Theme.fontScale = 1.6;
            tc.wizard.step = data.step;
            var question = textItem(tc.wizard, data.question);
            verify(question !== null, "the question is missing");
            waitForRendering(question);
            var form = question.parent;
            verify(leftEdge(question) >= leftEdge(form) && rightEdge(question) <= rightEdge(form),
                   "the question runs " + (rightEdge(question) - rightEdge(form)) + " px past the form");
            verify(question.contentWidth <= question.width, "the question's text is wider than its line");
            verify(rightEdge(form) <= tc.wizard.width, "the form runs off the screen");
        }
    }
}
