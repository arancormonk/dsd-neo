// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Window
import QtTest
import "../../../src/ui/qt/qml" as Ui

Item {
    id: root
    width: 420
    height: 900
    property bool pageOpen: false
    Ui.NavigationLayer {
        active: root.pageOpen
        onLeave: root.pageOpen = false
    }
    Ui.OutlineButton {
        id: opener
        text: "Open"
        width: 100
    }
    Ui.ModalSheet {
        id: site
        Ui.PlexComboBox {
            id: choice
            width: parent.width
            model: ["One", "Two"]
        }
        Ui.OutlineButton {
            width: parent.width
            text: "Network"
            onClicked: network.visible = true
        }
    }
    Ui.ModalSheet {
        id: network
        Ui.OutlineButton {
            width: parent.width
            text: "Close"
            onClicked: network.visible = false
        }
    }
    TestCase {
        name: "NavigationLayers"
        when: windowShown
        function cleanup() {
            network.visible = false;
            site.visible = false;
            root.pageOpen = false;
        }
        function test_one_layer_per_back() {
            root.pageOpen = true;
            site.visible = true;
            network.visible = true;
            verify(Ui.Navigation.back(root.Window.window));
            verify(!network.visible && site.visible && root.pageOpen);
            verify(Ui.Navigation.back(root.Window.window));
            verify(!site.visible && root.pageOpen);
            verify(Ui.Navigation.back(root.Window.window));
            verify(!root.pageOpen);
            verify(!Ui.Navigation.back(root.Window.window));
        }
        function test_dropdown_back_keeps_dialog() {
            site.visible = true;
            choice.popup.open();
            tryVerify(function () {
                return choice.popup.visible;
            });
            verify(Ui.Navigation.back(root.Window.window));
            tryVerify(function () {
                return !choice.popup.visible;
            });
            verify(site.visible);
            compare(choice.currentIndex, 0);
        }
        function test_modal_blocks_background_action_and_restores_focus() {
            var presses = 0;
            function count() {
                presses++;
            }
            opener.clicked.connect(count);
            opener.forceActiveFocus();
            site.visible = true;
            opener.activate();
            compare(presses, 0);
            verify(opener.Accessible.ignored);
            site.requestDismiss();
            tryVerify(function () {
                return opener.activeFocus;
            });
            opener.activate();
            compare(presses, 1);
            opener.clicked.disconnect(count);
        }
    }
}
