// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

UiPanel {
    id: row

    property var entry: ({
    })
    property string label: ""

    signal changed(string field, var value)
    signal move(int delta)
    signal remove()

    objectName: "scanEntryRow"
    height: fields.implicitHeight + 24

    Column {
        id: fields

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 12
        y: 12
        spacing: 6

        CheckBox {
            width: parent.width
            text: row.label
            checked: row.entry.enabled !== false
            onToggled: row.changed("enabled", checked)
        }

        Row {
            spacing: 6

            Button {
                text: qsTr("↑")
                onClicked: row.move(-1)
            }

            Button {
                text: qsTr("↓")
                onClicked: row.move(1)
            }

            Button {
                text: qsTr("Remove")
                onClicked: row.remove()
            }

        }

        TextField {
            objectName: "scanFrequencyName"
            width: parent.width
            visible: row.entry.kind === "freq"
            placeholderText: qsTr("Frequency name")
            text: row.entry.name || ""
            onTextEdited: row.changed("name", text)
        }

        Row {
            width: parent.width
            visible: row.entry.kind === "freq"
            spacing: 6

            ComboBox {
                width: (fields.width - 6) / 2
                model: ["p25", "dmr", "nxdn48", "nxdn"]
                currentIndex: model.indexOf(row.entry.protocol || "p25")
                onActivated: row.changed("protocol", currentText)
            }

            TextField {
                width: (fields.width - 6) / 2
                placeholderText: qsTr("MHz")
                text: row.entry.freqMhz || ""
                inputMethodHints: Qt.ImhFormattedNumbersOnly
                onTextEdited: row.changed("freqMhz", text)
            }

        }

        Text {
            text: qsTr("Dwell / hold ms (0 inherits)")
            color: Theme.textSecondary
            font.family: Theme.sans
        }

        Row {
            spacing: 6

            TextField {
                width: (fields.width - 6) / 2
                placeholderText: qsTr("Dwell")
                text: row.entry.dwellMs || "0"
                onTextEdited: row.changed("dwellMs", text.length ? Number(text) : 0)

                validator: IntValidator {
                    bottom: 0
                    top: 600000
                }

            }

            TextField {
                width: (fields.width - 6) / 2
                placeholderText: qsTr("Hold")
                text: row.entry.holdMs || "0"
                onTextEdited: row.changed("holdMs", text.length ? Number(text) : 0)

                validator: IntValidator {
                    bottom: 0
                    top: 600000
                }

            }

        }

        Row {
            spacing: 6

            ComboBox {
                width: (fields.width - 6) / 2
                model: [qsTr("Inherit modulation"), "c4fm", "cqpsk", "gfsk"]
                currentIndex: Math.max(0, ["", "c4fm", "cqpsk", "gfsk"].indexOf(row.entry.modulation || ""))
                onActivated: row.changed("modulation", ["", "c4fm", "cqpsk", "gfsk"][currentIndex])
            }

            TextField {
                width: (fields.width - 6) / 2
                placeholderText: qsTr("Gain (-1 inherits)")
                text: row.entry.gainDb === undefined ? "-1" : row.entry.gainDb
                onTextEdited: row.changed("gainDb", text.length ? Number(text) : -1)

                validator: IntValidator {
                    bottom: -1
                    top: 49
                }

            }

        }

    }

}
