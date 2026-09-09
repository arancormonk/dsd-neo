// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
Item {
    width: 420; height: 800
    Loader { id: screen; anchors.fill: parent; source: uiDir + "/DiagnosticsScreen.qml" }
    TestCase {
        name: "DiagnosticsScreen"
        when: windowShown
        function containsText(item, text) {
            if (item.text === text)
                return true;
            for (var i = 0; i < item.children.length; ++i) {
                if (containsText(item.children[i], text))
                    return true;
            }
            return false;
        }
        function test_controls() {
            compare(screen.status, Loader.Ready);
            diagnosticsLog.paused = false;
            diagnosticsLog.clear();
            testContext.pushDiagnostic("diagnostics clear fixture");
            diagnosticsLog.refresh();
            var line = "[fixture] [info] diagnostics clear fixture";
            verify(diagnosticsLog.allText().indexOf(line) >= 0);
            tryVerify(function () { return containsText(screen.item, line); });
            var pause = findChild(screen.item, "diagnosticsPause");
            verify(pause !== null);
            mouseClick(pause);
            compare(diagnosticsLog.paused, true);
            mouseClick(pause);
            compare(diagnosticsLog.paused, false);
            verify(!findChild(screen.item, "diagnosticsShare").visible);
            mouseClick(findChild(screen.item, "diagnosticsClear"));
            compare(diagnosticsLog.allText(), "");
            tryVerify(function () { return !containsText(screen.item, line); });
        }
    }
}
