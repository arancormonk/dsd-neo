// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
Item {
    id: screen
    signal closed()
    Rectangle { anchors.fill: parent; color: Theme.bg }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        RowLayout {
            Button { text: qsTr("Back"); onClicked: screen.closed() }
            Label { text: qsTr("Diagnostics"); color: Theme.textPrimary; Layout.fillWidth: true }
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Current-process decoder and host diagnostics plus a bounded redacted persistent tail from previous runs. Not a native-crash or ANR capture.")
            wrapMode: Text.WordWrap
            color: Theme.textPrimary
        }
        RowLayout {
            Button {
                objectName: "diagnosticsPause"
                text: diagnosticsLog.paused ? qsTr("Resume (%1)").arg(diagnosticsLog.pendingCount) : qsTr("Pause")
                onClicked: diagnosticsLog.paused = !diagnosticsLog.paused
            }
            Button { objectName: "diagnosticsCopy"; text: qsTr("Copy"); onClicked: diagnosticsLog.copyAll() }
            Button { objectName: "diagnosticsClear"; text: qsTr("Clear"); onClicked: diagnosticsLog.clear() }
        }
        Button {
            objectName: "diagnosticsShare"
            visible: decoderHost.shareSupported
            text: qsTr("Share diagnostics")
            onClicked: decoderHost.shareDiagnostics(diagnosticsLog.allText(), qsTr("DSD-neo diagnostics"))
        }
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: diagnosticsLog
            delegate: Text {
                required property string line
                width: list.width
                text: line
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                color: Theme.textPrimary
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(12)
            }
            ScrollBar.vertical: ScrollBar {}
        }
    }
    Keys.onEscapePressed: screen.closed()
    Keys.onBackPressed: screen.closed()
    onVisibleChanged: if (visible) forceActiveFocus()
}
