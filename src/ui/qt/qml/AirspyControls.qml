// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

Column {
    id: panel
    property bool gainsOnly: false

    property var settings: ({
    })
    readonly property string gainMode: String(setting("gain_mode", "sensitivity"))

    signal edited(string key, string value)

    function setting(key, fallback) {
        return settings && settings[key] !== undefined ? settings[key] : fallback;
    }

    // The text fields commit on editingFinished. A form that is saved or
    // dismissed while one still has focus reads what is typed through these.
    /** Every text field's text as it stands now, by setting key. */
    function pendingText() {
        var text = {};
        for (var repeater of [textFields, gainFields]) {
            for (var i = 0; i < repeater.count; ++i) {
                var row = repeater.itemAt(i);
                if (row)
                    text[row.key] = row.input.text;
            }
        }
        return text;
    }

    /** Put every text field back to its committed setting, dropping typing a dismissed form left behind. */
    function resetText() {
        for (var repeater of [textFields, gainFields]) {
            for (var i = 0; i < repeater.count; ++i) {
                var row = repeater.itemAt(i);
                if (row)
                    row.input.text = Qt.binding(function (input) {
                        return function () {
                            return input.committedText;
                        };
                    }(row.input));
            }
        }
    }

    /** settings with every field's typing that would commit applied. */
    function pendingSettings() {
        var next = Object.assign({}, settings);
        for (var i = 0; i < textFields.count; ++i) {
            var row = textFields.itemAt(i);
            if (row && row.input.text.trim() !== row.input.committedText)
                next[row.key] = row.input.text.trim();
        }
        for (var j = 0; j < gainFields.count; ++j) {
            var gain = gainFields.itemAt(j);
            if (gain && gain.input.acceptableInput && gain.input.text !== gain.input.committedText)
                next[gain.key] = gain.input.text;
        }
        return next;
    }

    spacing: 12

    ReadableText {
        width: parent.width
        text: qsTr("Airspy R2 / Mini")
        color: Theme.textPrimary
        font.pixelSize: Theme.fontSize(16)
    }

    ReadableText {
        width: parent.width
        visible: panel.setting("actual_rate", 0) > 0
        text: panel.setting("actual_serial", "") + " · " + (panel.setting("actual_rate", 0) / 1e+06) + " MS/s"
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(13)
    }

    Repeater {
        id: textFields

        model: [{
            "key": "serial",
            "label": qsTr("Serial (empty selects first device)"),
            "value": ""
        }, {
            "key": "sample_rate",
            "label": qsTr("Sample rate (samples/s or auto)"),
            "value": "auto"
        }]

        delegate: Column {
            required property var modelData
            readonly property string key: modelData.key
            property alias input: textField

            width: panel.width
            spacing: 4
            visible: !panel.gainsOnly

            // Read as the field's name (issue #782), not on its own.
            Text {
                id: textLabel
                text: modelData.label
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSize(13)
                Accessible.ignored: true
            }

            PlexInput {
                id: textField
                Accessible.name: textLabel.text

                readonly property string committedText: modelData.key === "sample_rate" && Number(panel.setting(modelData.key, 0)) === 0 ? "auto" : String(panel.setting(modelData.key, modelData.value))

                objectName: "airspy_" + modelData.key
                width: parent.width
                text: committedText
                onEditingFinished: panel.edited(modelData.key, text.trim())
            }

        }

    }

    ReadableText {
        width: parent.width
        visible: panel.settings.rates !== undefined
        text: qsTr("Available rates: ") + (panel.settings.rates || []).join(", ")
        color: Theme.textSecondary
        wrapMode: Text.Wrap
        font.pixelSize: Theme.fontSize(12)
    }

    PlexComboBox {
        objectName: "airspy_gain_mode"
        width: parent.width
        model: [qsTr("Sensitivity"), qsTr("Linearity"), qsTr("Manual gain")]
        currentIndex: Math.max(0, ["sensitivity", "linearity", "manual"].indexOf(panel.gainMode))
        onActivated: panel.edited("gain_mode", ["sensitivity", "linearity", "manual"][currentIndex])
    }

    Repeater {
        id: gainFields

        model: [{
            "key": "sensitivity_gain",
            "label": qsTr("Sensitivity index"),
            "mode": "sensitivity",
            "max": 21,
            "value": 10
        }, {
            "key": "linearity_gain",
            "label": qsTr("Linearity index"),
            "mode": "linearity",
            "max": 21,
            "value": 10
        }, {
            "key": "lna_gain",
            "label": qsTr("LNA gain index"),
            "mode": "manual",
            "max": 15,
            "value": 1
        }, {
            "key": "mixer_gain",
            "label": qsTr("Mixer gain index"),
            "mode": "manual",
            "max": 15,
            "value": 5
        }, {
            "key": "vga_gain",
            "label": qsTr("VGA gain index"),
            "mode": "manual",
            "max": 15,
            "value": 5
        }]

        delegate: Column {
            required property var modelData
            readonly property string key: modelData.key
            property alias input: gainField

            width: panel.width
            spacing: 4
            visible: panel.gainMode === modelData.mode

            // Read as the field's name (issue #782), not on its own.
            Text {
                id: gainLabel
                text: modelData.label + " (0–" + modelData.max + ")"
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSize(13)
                Accessible.ignored: true
            }

            PlexInput {
                id: gainField
                Accessible.name: gainLabel.text

                readonly property string committedText: String(panel.setting(modelData.key, modelData.value))

                objectName: "airspy_" + modelData.key
                width: parent.width
                text: committedText
                onEditingFinished: {
                    if (acceptableInput)
                        panel.edited(modelData.key, text);

                }

                validator: IntValidator {
                    bottom: 0
                    top: modelData.max
                }

            }

        }

    }

    Repeater {
        model: [{
            "key": "lna_agc",
            "label": qsTr("LNA AGC")
        }, {
            "key": "mixer_agc",
            "label": qsTr("Mixer AGC")
        }, {
            "key": "bias_tee",
            "label": qsTr("Bias tee")
        }]

        delegate: ToggleRow {
            objectName: "airspy_" + modelData.key
            required property var modelData

            width: panel.width
            visible: modelData.key === "bias_tee" ? !panel.gainsOnly : panel.gainMode === "manual"
            title: modelData.label
            checked: panel.setting(modelData.key, 0) === true || String(panel.setting(modelData.key, 0)) === "1" || String(panel.setting(modelData.key, 0)) === "true"
            onToggled: function(on) {
                panel.edited(modelData.key, on ? "1" : "0");
            }
        }

    }

}
