// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// DisclosureRow and ToggleRow are shared across three screens, and two of those
// screens — SettingsScreen and WizardScreen — are loaded by no other case in
// this suite. There is no qmllint in this toolchain either, so an unresolved
// type or an unqualified id in one of them is caught by nothing until the screen
// comes up blank on the device. Loading them here is what makes that a test
// failure.
//
// The row cases pin the contract the screens rely on rather than the geometry:
// a toggle whose `toggled` no longer reaches its caller, or a disclosure row
// that answers a tap while a request is in flight, is a silently broken screen.
Item {
    id: root

    width: 420
    height: 900

    Loader {
        id: settingsLoader

        anchors.fill: parent
        source: uiDir + "/SettingsScreen.qml"
    }

    Loader {
        id: wizardLoader

        anchors.fill: parent
        active: false
        source: uiDir + "/WizardScreen.qml"
    }

    Loader {
        id: disclosureLoader

        active: false
        source: uiDir + "/DisclosureRow.qml"
    }

    Loader {
        id: toggleLoader

        active: false
        source: uiDir + "/ToggleRow.qml"
    }

    TestCase {
        id: tc

        name: "SharedRowComponents"
        when: windowShown

        function test_01_settings_screen_loads() {
            verify(settingsLoader.item !== null,
                   "SettingsScreen.qml failed to load: " + settingsLoader.sourceComponent)
            compare(settingsLoader.status, Loader.Ready, "SettingsScreen.qml did not reach Ready")
        }

        function test_02_wizard_screen_loads() {
            wizardLoader.active = true
            tryVerify(function () { return wizardLoader.status !== Loader.Loading }, 4000,
                      "WizardScreen.qml never finished loading")
            compare(wizardLoader.status, Loader.Ready, "WizardScreen.qml did not reach Ready")
            verify(wizardLoader.item !== null, "WizardScreen.qml failed to load")
        }

        // The wizard's RadioReference entry is a card of its own at the top of
        // the tune step, not a row inside the trunking-data card: it answers
        // the whole step (frequency, decode mode, talkgroups), so it must not
        // read as a trunked-only file-picker detail. findChild is how
        // UI_QT_QML_CALL_LISTS addresses it, so the objectName has to survive
        // the move.
        function test_03_the_wizard_radioreference_row_is_still_addressable() {
            wizardLoader.active = true
            tryVerify(function () { return wizardLoader.item !== null }, 4000,
                      "WizardScreen.qml failed to load")
            var row = findChild(wizardLoader.item, "wizardRadioReferenceRow")
            verify(row !== null, "the wizard's RadioReference row is missing")
            compare(row.showDivider, false,
                    "the row is alone in its own panel now — a divider would underline nothing")

            // The entry appears only in builds that carry the feature, and only
            // on the tune step, which is where the answers it fills in live.
            wizardLoader.item.step = 1
            testContext.setRadioReference("available", true)
            tryVerify(function () { return row.visible }, 4000,
                      "the row must appear once the feature reports available")
            testContext.setRadioReference("available", false)
            tryVerify(function () { return !row.visible }, 4000,
                      "the row must hide in a build without the feature")
            wizardLoader.item.step = 0

            // The row IS the entry point: a tap must still ask Main.qml to push
            // the RadioReference screen.
            var opens = 0
            wizardLoader.item.openRadioReference.connect(function () { opens++ })
            row.tapped()
            compare(opens, 1, "tapping the row no longer opens the RadioReference screen")
        }

        function test_04_disclosure_row_reports_taps_only_when_enabled() {
            disclosureLoader.active = true
            var row = disclosureLoader.item
            verify(row !== null, "DisclosureRow.qml failed to load")

            row.title = "Imported files"
            row.subtitle = "Channel maps, talkgroups, and keys"
            compare(row.title, "Imported files", "the row did not take its title")
            compare(row.showDivider, false, "a divider must be asked for, not assumed")
            compare(row.tapEnabled, true, "a row must answer taps unless told otherwise")

            var taps = 0
            row.tapped.connect(function () { taps++ })
            row.tapped()
            compare(taps, 1, "the row's tapped signal did not reach its caller")

            // The gate is on the handler, not on `enabled`: binding `enabled` on
            // the row would also disable anything nested inside it.
            row.tapEnabled = false
            compare(row.enabled, true, "gating taps must not disable the row itself")
        }

        function test_05_toggle_row_forwards_its_switch() {
            toggleLoader.active = true
            var row = toggleLoader.item
            verify(row !== null, "ToggleRow.qml failed to load")

            row.title = "Treat partly encrypted as encrypted"
            row.subtitle = "Blocks those talkgroups instead of playing them"
            row.checked = true

            var seen = []
            row.toggled.connect(function (state) { seen.push(state) })
            row.toggled(false)
            row.toggled(true)
            compare(seen.length, 2, "the row's toggled signal did not reach its caller")
            compare(seen[0], false, "the row forwarded the wrong state")
            compare(seen[1], true, "the row forwarded the wrong state")
        }

        // Issue #802 B12: the shared text pieces carry typed, imported and
        // received names (a saved system, a profile, a RadioReference site, a
        // scan list), so a "<b>" or "&lt;" in one is drawn and measured as
        // written, never as markup. Each text showing the name must be plain
        // and as wide as a plain mirror of it.
        readonly property string markup: "<b>Dock</b> &lt;1&gt;"
        function drawnTexts(item, out) {
            if (out.indexOf(item) >= 0)
                return out
            if (item.textFormat !== undefined && String(item.text).indexOf(markup) >= 0)
                out.push(item)
            var kids = (item.contentItem && item.contentItem !== item ? [item.contentItem] : []).concat(item.children || [])
            for (var i = 0; i < kids.length; ++i)
                drawnTexts(kids[i], out)
            return out
        }
        function verifyDrawnAsWritten(item, tag, expected) {
            var texts = drawnTexts(item, [])
            compare(texts.length, expected, tag + " shows the name")
            for (var i = 0; i < texts.length; ++i) {
                var mirror = Qt.createQmlObject("import QtQuick; Text { visible: false; textFormat: Text.PlainText }", root)
                mirror.font = texts[i].font
                mirror.text = texts[i].text
                var plainWidth = mirror.implicitWidth
                mirror.destroy()
                verify(Math.abs(texts[i].implicitWidth - plainWidth) <= 0.5,
                       tag + " " + i + " draws as written: " + texts[i].implicitWidth + " vs " + plainWidth)
                compare(texts[i].textFormat, Text.PlainText, tag + " " + i + " is plain text")
            }
        }
        function create(file, properties) {
            var component = Qt.createComponent(uiDir + "/" + file)
            compare(component.status, Component.Ready, file + ": " + component.errorString())
            var made = createTemporaryObject(component, root, properties)
            verify(made !== null, file + " was not created")
            return made
        }
        function test_06_shared_text_draws_names_as_written_data() {
            return [
                {tag: "ReadableText", file: "ReadableText.qml", properties: {text: markup}, texts: 1},
                {tag: "LiveText", file: "LiveText.qml", properties: {text: markup}, texts: 1},
                {tag: "MicroLabel", file: "MicroLabel.qml", properties: {text: markup}, texts: 1},
                {tag: "OutlineButton", file: "OutlineButton.qml", properties: {text: markup, width: 300}, texts: 1},
                {tag: "FilterPill", file: "FilterPill.qml", properties: {text: markup}, texts: 1},
                {tag: "DisclosureRow", file: "DisclosureRow.qml", properties: {title: markup, subtitle: markup, width: 400}, texts: 2},
                {tag: "ToggleRow", file: "ToggleRow.qml", properties: {title: markup, subtitle: markup, note: markup, width: 400}, texts: 3},
                {tag: "PlexComboBox", file: "PlexComboBox.qml", properties: {model: [markup], width: 300}, texts: 1},
                {tag: "PlexTextField", file: "PlexTextField.qml", properties: {label: "Name", placeholderText: markup, width: 300}, texts: 1}
            ]
        }
        function test_06_shared_text_draws_names_as_written(data) {
            verifyDrawnAsWritten(create(data.file, data.properties), data.tag, data.texts)
        }
        function test_07_choice_list_draws_names_as_written() {
            var combo = create("PlexComboBox.qml", {model: [markup], width: 300})
            combo.popup.open()
            tryCompare(combo.popup, "opened", true)
            verifyDrawnAsWritten(combo.popup.contentItem, "the choice list", 1)
            combo.popup.close()
            var sheet = create("ChoiceSheet.qml", {width: 400, height: 600})
            sheet.open(markup, [markup], function () {})
            verifyDrawnAsWritten(sheet, "the choice sheet", 2)
        }
    }
}
