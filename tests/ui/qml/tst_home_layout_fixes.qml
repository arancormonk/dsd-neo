// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #646: Listen's own layout on its single-column body. B1: hidden, Home keeps its scroll range and place.
Item {
    id: root
    QtObject {
        id: mainRoot
        property bool transitioning: false
        property bool showFailure: false
        property string failureText: ""
        property string dismissedFailure: ""
    }
    width: 360
    height: 700
    Ui.HomeScreen {
        id: home
        anchors.fill: parent
    }
    // Changed once per frame wanted, so the window draws while Home is hidden.
    Rectangle {
        id: ticker
        width: 1
        height: 1
        z: 10
        color: "black"
    }
    TestCase {
        name: "HomeLayoutFixes"
        when: windowShown

        function init() {
            emptyModels();
        }
        function cleanup() {
            root.forceActiveFocus();
            home.visible = true;
            emptyModels();
            body().contentY = body().originY;
        }
        function emptyModels() {
            while (savedSystems.count)
                savedSystems.remove(0);
            while (scanLists.count)
                scanLists.remove(0);
        }
        function addSystem(name, fields) {
            verify(savedSystems.add(Object.assign({name: name, sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                freqMhz: "851.5"}, fields || {})), "could not add " + name);
        }
        function addList(name) {
            verify(scanLists.add({name: name, sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                entries: [{kind: "freq", name: "Simplex", protocol: "p25", freqMhz: "851.5", enabled: true}]}));
        }
        function body() {
            return findChild(home, "dongleScrollBody");
        }
        function cards() {
            var out = [];
            for (var child of body().contentItem.children[0].children) {
                if (child.hasOwnProperty("refreshHint") && child.hasOwnProperty("siblings"))
                    out.push(child);
            }
            return out;
        }
        // Draws @a count frames while Home stays as it is: every polish a hidden Home would see runs in them.
        function waitFrames(count) {
            for (var i = 0; i < count; ++i) {
                ticker.color = ticker.color === Qt.color("black") ? "white" : "black";
                verify(waitForRendering(ticker), "frame " + i);
            }
        }

        // B1: Main hides Home on another tab, the wizard, an editor, Signal search or a session. Hidden, the body's
        // cards keep their heights through every polish, so the scroll range and the place the user scrolled to
        // are there when Home shows again.
        function test_scroll_kept_while_hidden() {
            for (var i = 0; i < 15; ++i)
                addSystem("System " + i);
            for (i = 0; i < 5; ++i)
                addList("List " + i);
            verify(waitForPolish(root.Window.window));
            waitForRendering(home);
            var scrolled = body().contentHeight - body().height;
            verify(scrolled > 1000, "the body scrolls a long way: " + scrolled);
            body().contentY = scrolled;
            var range = body().contentHeight;
            var cardHeight = cards()[0].height;
            verify(cardHeight > 0);
            home.visible = false;
            waitFrames(3);
            compare(cards()[0].height, cardHeight, "a hidden card keeps its height");
            compare(body().contentHeight, range, "the hidden body keeps its scroll range");
            compare(body().contentY, scrolled, "the hidden body keeps its place");
            home.visible = true;
            verify(waitForPolish(home));
            waitFrames(3);
            compare(body().contentHeight, range, "the scroll range when shown again");
            compare(body().contentY, scrolled, "the place when shown again");
        }
    }
}
