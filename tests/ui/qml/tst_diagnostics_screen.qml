// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
Item {
    width: 420; height: 800
    Loader { id: screen; anchors.fill: parent; source: uiDir + "/DiagnosticsScreen.qml" }
    TestCase {
        name: "DiagnosticsScreen"
        when: windowShown
        function test_controls() {
            compare(screen.status, Loader.Ready);
            var pause = findChild(screen.item, "diagnosticsPause");
            verify(pause !== null);
            mouseClick(pause);
            compare(diagnosticsLog.paused, true);
            mouseClick(pause);
            compare(diagnosticsLog.paused, false);
            verify(!findChild(screen.item, "diagnosticsShare").visible);
            mouseClick(findChild(screen.item, "diagnosticsClear"));
            compare(diagnosticsLog.allText(), "");
        }
    }
}
