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
    }
}
