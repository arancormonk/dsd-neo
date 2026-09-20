// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
Item {
    width: 420; height: 800
    Loader { id: screen; anchors.fill: parent; source: uiDir + "/DiagnosticsScreen.qml" }
    SignalSpy { id: closedSpy; target: screen.item; signalName: "closed" }
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
        function test_header_back() {
            var back = findChild(screen.item, "diagnosticsBack");
            verify(back !== null);
            compare(back.icon, "back");
            closedSpy.clear();
            mouseClick(back);
            compare(closedSpy.count, 1);
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
            var confirm = findChild(screen.item, "diagnosticsClearConfirm");
            verify(confirm.visible);
            compare(confirm.title, "Clear diagnostics?");
            compare(confirm.message, "The current log and the saved previous-run tail are removed.");
            compare(confirm.confirmText, "Clear log");
            verify(confirm.destructive);
            verify(diagnosticsLog.allText().indexOf(line) >= 0);
            mouseClick(findChild(confirm, "confirmCancelButton"));
            verify(!confirm.visible);
            verify(diagnosticsLog.allText().indexOf(line) >= 0);
            mouseClick(findChild(screen.item, "diagnosticsClear"));
            mouseClick(findChild(confirm, "confirmActionButton"));
            verify(!confirm.visible);
            compare(diagnosticsLog.allText(), "");
            tryVerify(function () { return !containsText(screen.item, line); });
            // A second activation of a dismissed confirmation must not clear new arrivals.
            testContext.pushDiagnostic("after clear");
            diagnosticsLog.refresh();
            confirm.confirm();
            verify(diagnosticsLog.allText().indexOf("after clear") >= 0);
            diagnosticsLog.clear();
        }
    }
}
