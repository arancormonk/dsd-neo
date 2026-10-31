// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Controls
import "Util.js" as Util

ModalSheet {
    id: sheet
    objectName: "signalSearchRangeEditor"
    property var searchModel: null
    property string sourceType: "usb"
    property int row: -1
    property var draft: ({})
    property string errorText: ""
    // The form as openFor() filled it: a dismissal asks only once it differs.
    property string openedForm: ""
    accessibleName: row < 0 ? qsTr("Add search range") : qsTr("Edit search range")
    // A form sheet (issue #718): an outside tap keeps it; Cancel, Back and Escape ask first.
    closesOnOutsideTap: false

    function matchingStep(numerator, denominator) {
        var n = Number(numerator), d = Number(denominator);
        if (!(n > 0) || !(d > 0))
            return 0;
        var numerators = [0, 5000, 6250, 25000, 12500, 25000];
        for (var i = 1; i < numerators.length; ++i) {
            if (n * (i === 3 ? 3 : 1) === numerators[i] * d)
                return i;
        }
        return 0;
    }

    function openFor(index, value) {
        row = index;
        draft = Object.assign({}, value);
        nameField.text = draft.name;
        startField.text = draft.startMHz;
        endField.text = draft.endMHz;
        stepNumerator.text = String(draft.stepNum);
        stepDenominator.text = String(draft.stepDen);
        bandwidthField.text = String(draft.bandwidth);
        gainField.text = String(draft.gain);
        sensitivityField.text = String(draft.sensitivity);
        minimumField.text = String(draft.minimum);
        marginField.text = String(draft.squelchMargin);
        levelField.text = String(draft.squelchLevel);
        probeField.text = String(draft.probe);
        visitField.text = String(draft.visit);
        modulation.currentIndex = draft.am ? 1 : 0;
        digital.currentIndex = draft.digital;
        squelch.currentIndex = draft.squelchMode;
        gainMode.currentIndex = !draft.gainSet ? 0 : draft.gainAuto ? 1 : 2;
        minimumSwitch.checked = draft.minimumSet;
        airspyOverride.checked = !!draft.airspyGainSet;
        airspyGain.settings = Object.assign({}, draft.airspy || {});
        airspyGain.resetText();
        errorText = "";
        openedForm = formFingerprint();
        open();
    }

    /** What the form holds now, read from the live field text. */
    function formValue() {
        return {
            name: nameField.text, startMHz: startField.text, endMHz: endField.text,
            stepNum: stepNumerator.text, stepDen: stepDenominator.text, bandwidth: bandwidthField.text,
            am: modulation.currentIndex === 1, digital: digital.currentIndex,
            gainSet: gainMode.currentIndex !== 0, gainAuto: gainMode.currentIndex === 1, gain: gainField.text,
            sensitivity: sensitivityField.text, minimumSet: minimumSwitch.checked, minimum: minimumField.text,
            squelchMode: squelch.currentIndex, squelchMargin: marginField.text, squelchLevel: levelField.text,
            probe: probeField.text, visit: visitField.text,
            airspyGainSet: airspyOverride.checked, airspy: airspyGain.pendingSettings()
        };
    }
    // The form plus the Airspy fields' typing, committed or not: a gain typed
    // but not yet committed (or not yet a valid index) still counts.
    function formFingerprint() {
        return JSON.stringify({form: formValue(), airspyText: airspyGain.pendingText()});
    }
    function requestClose() {
        if (visible && formFingerprint() !== openedForm)
            discard.ask(function () {
                sheet.visible = false;
            });
        else
            visible = false;
    }
    dismissHandler: requestClose
    DiscardDialog {
        id: discard
        parent: sheet
    }

    Column {
        width: parent.width
        spacing: Theme.gap

        Text { text: sheet.accessibleName; color: Theme.textPrimary; font.family: Theme.sans; font.pixelSize: Theme.fontSize(20); font.weight: Font.Bold }
        PlexTextField { id: nameField; objectName: "searchRangeName"; width: parent.width; label: qsTr("Range name") }
        Row {
            width: parent.width
            spacing: 12
            PlexTextField { id: startField; width: (parent.width - 12) / 2; label: qsTr("Start · MHz"); mono: true; inputMethodHints: Qt.ImhFormattedNumbersOnly }
            PlexTextField { id: endField; width: (parent.width - 12) / 2; label: qsTr("End · MHz"); mono: true; inputMethodHints: Qt.ImhFormattedNumbersOnly }
        }
        ReadableText { text: qsTr("Channel step"); color: Theme.textPrimary; font.family: Theme.sans; font.pixelSize: Theme.fontSize(15) }
        PlexComboBox {
            id: stepPreset
            objectName: "searchStepPreset"
            width: parent.width
            Accessible.name: qsTr("Channel step preset")
            model: ["Custom", "5 kHz", "6.25 kHz", "8⅓ kHz (25 kHz / 3)", "12.5 kHz", "25 kHz"]
            currentIndex: sheet.matchingStep(stepNumerator.text, stepDenominator.text)
            onActivated: function (index) {
                var steps = [0, 5000, 6250, 25000, 12500, 25000];
                if (index > 0) {
                    stepNumerator.text = String(steps[index]);
                    stepDenominator.text = index === 3 ? "3" : "1";
                }
            }
        }
        Row {
            width: parent.width
            spacing: 12
            PlexTextField { id: stepNumerator; objectName: "searchStepNumerator"; width: (parent.width - 12) / 2; label: qsTr("Step numerator · Hz"); inputMethodHints: Qt.ImhDigitsOnly }
            PlexTextField { id: stepDenominator; objectName: "searchStepDenominator"; width: (parent.width - 12) / 2; label: qsTr("Step denominator"); inputMethodHints: Qt.ImhDigitsOnly }
        }
        PlexComboBox {
            id: modulation
            width: parent.width
            Accessible.name: qsTr("Analog modulation")
            model: ["NFM", "AM"]
            onActivated: {
                bandwidthField.text = currentIndex === 1 ? "6000" : "16000";
                if (currentIndex === 1)
                    digital.currentIndex = 0;
                squelch.currentIndex = currentIndex === 1 ? 1 : 2;
            }
        }
        PlexTextField { id: bandwidthField; width: parent.width; label: qsTr("Channel bandwidth · Hz"); inputMethodHints: Qt.ImhDigitsOnly }
        ReadableText { text: qsTr("Digital identification"); color: Theme.textPrimary; font.family: Theme.sans; font.pixelSize: Theme.fontSize(15) }
        PlexComboBox {
            id: digital
            width: parent.width
            enabled: modulation.currentIndex === 0
            Accessible.name: qsTr("Digital identification profile")
            model: [qsTr("Analog only"), "Auto", "P25", "DMR", "NXDN96", "NXDN48", "dPMR", "D-STAR", "YSF", "M17"]
        }
        ReadableText {
            width: parent.width
            text: qsTr("Auto needs about six seconds to try all symbol profiles. A two-second probe may leave digital identification unresolved.")
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(13)
        }
        Row {
            width: parent.width
            spacing: 12
            PlexTextField { id: probeField; width: (parent.width - 12) / 2; label: qsTr("Probe · seconds (2–10)"); inputMethodHints: Qt.ImhDigitsOnly }
            PlexTextField { id: visitField; width: (parent.width - 12) / 2; label: qsTr("Max visit · seconds"); placeholderText: qsTr("0 disables"); inputMethodHints: Qt.ImhDigitsOnly }
        }
        ReadableText { heading: true; text: qsTr("RF detection"); color: Theme.textPrimary; font.family: Theme.sans; font.pixelSize: Theme.fontSize(17); font.weight: Font.DemiBold }
        PlexTextField { id: sensitivityField; width: parent.width; label: qsTr("Above local noise · dB (3–30)"); inputMethodHints: Qt.ImhDigitsOnly }
        PlexCheckBox { id: minimumSwitch; text: qsTr("Also require a minimum RF level") }
        PlexTextField { id: minimumField; width: parent.width; visible: minimumSwitch.checked; label: qsTr("Minimum RF level · dBFS"); inputMethodHints: Qt.ImhFormattedNumbersOnly }
        ReadableText { heading: true; text: qsTr("Listening squelch"); color: Theme.textPrimary; font.family: Theme.sans; font.pixelSize: Theme.fontSize(17); font.weight: Font.DemiBold }
        PlexComboBox { id: squelch; objectName: "searchRangeSquelch"; width: parent.width; Accessible.name: qsTr("Listening squelch mode"); model: [qsTr("Level"), qsTr("Auto"), qsTr("Noise (NFM)")] }
        PlexTextField { id: marginField; width: parent.width; visible: squelch.currentIndex !== 0; label: qsTr("Squelch margin / quieting · dB"); inputMethodHints: Qt.ImhDigitsOnly }
        PlexTextField { id: levelField; width: parent.width; visible: squelch.currentIndex === 0; label: qsTr("Listening level · dBFS"); inputMethodHints: Qt.ImhFormattedNumbersOnly }
        ReadableText { heading: true; text: qsTr("Receiver gain"); color: Theme.textPrimary; font.family: Theme.sans; font.pixelSize: Theme.fontSize(17); font.weight: Font.DemiBold }
        PlexComboBox { id: gainMode; width: parent.width; visible: sheet.sourceType !== "airspy"; Accessible.name: qsTr("Receiver gain mode"); model: [qsTr("Session setting"), qsTr("Hardware auto"), qsTr("Manual")] }
        PlexTextField { id: gainField; width: parent.width; visible: sheet.sourceType !== "airspy" && gainMode.currentIndex === 2; label: qsTr("Gain · dB"); inputMethodHints: Qt.ImhFormattedNumbersOnly }
        PlexCheckBox { id: airspyOverride; visible: sheet.sourceType === "airspy"; text: qsTr("Use Airspy gain settings for this range") }
        AirspyControls {
            id: airspyGain
            width: parent.width
            visible: sheet.sourceType === "airspy" && airspyOverride.checked
            gainsOnly: true
            onEdited: function (key, value) {
                var next = Object.assign({}, settings);
                next[key] = value;
                settings = next;
            }
        }
        Text {
            id: rangeErrorText
            width: parent.width
            visible: sheet.errorText.length > 0
            text: sheet.errorText
            wrapMode: Text.Wrap
            color: Theme.textPrimary
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(14)
            Accessible.role: Accessible.AlertMessage
            // A role alone reads nothing (issue #782): the error is its name.
            Accessible.name: text
            Accessible.ignored: !Navigation.allows(rangeErrorText) || !visible
        }
        GradientButton {
            objectName: "searchRangeSave"
            width: parent.width
            text: qsTr("Save range")
            onClicked: {
                var r = Object.assign({}, sheet.draft, sheet.formValue());
                sheet.errorText = searchModel.saveRange(sheet.row, r);
                if (!sheet.errorText.length)
                    sheet.visible = false;
            }
        }
        OutlineButton {
            objectName: "searchRangeCancel"
            width: parent.width
            text: qsTr("Cancel")
            onClicked: sheet.requestClose()
        }
    }
}
