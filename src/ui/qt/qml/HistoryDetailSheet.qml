// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

ModalSheet {
    id: sheet
    property var record: ({})
    accessibleName: qsTr("Activity details")
    function open(value) {
        record = value;
        visible = true;
    }
    Repeater {
        model: [sheet.record.name || "", sheet.record.when > 0 ? Qt.formatDateTime(new Date(sheet.record.when * 1000), "yyyy-MM-dd hh:mm:ss") : "", sheet.record.systemName || "", sheet.record.channel || "", sheet.record.tg > 0 ? qsTr("Talkgroup %1").arg(sheet.record.tg) : "", sheet.record.sourceName || "", sheet.record.src > 0 ? qsTr("Radio ID %1").arg(sheet.record.src) : "", sheet.record.emergency ? qsTr("Emergency") : "", sheet.record.enc ? qsTr("Encrypted") : qsTr("Unencrypted"), sheet.record.durationSecs >= 0 ? qsTr("Duration: %1 seconds").arg(sheet.record.durationSecs) : "", sheet.record.detail || ""]
        Text {
            required property string modelData
            width: parent.width
            visible: modelData.length > 0
            Accessible.role: Accessible.StaticText
            Accessible.name: modelData
            readonly property bool navigationAllowed: Navigation.allows(sheet)
            Accessible.ignored: !visible || !navigationAllowed
            text: modelData
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
            color: Theme.textPrimary
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(15)
        }
    }
    OutlineButton {
        width: parent.width
        text: qsTr("Close")
        onClicked: sheet.visible = false
    }
}
