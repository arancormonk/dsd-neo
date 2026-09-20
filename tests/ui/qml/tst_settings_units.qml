// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    width: 420
    height: 900

    Loader {
        id: screenLoader
        anchors.fill: parent
    }

    TestCase {
        name: "SettingsUnits"
        when: windowShown

        function init() {
            testContext.useLifecycleHost(true)
            prefs.metricUnits = false
            screenLoader.source = uiDir + "/SettingsScreen.qml"
            verify(screenLoader.item !== null)
        }

        function cleanup() {
            screenLoader.source = ""
            prefs.metricUnits = false
            testContext.useLifecycleHost(false)
        }

        function test_toggle_units() {
            var toggle = findChild(screenLoader.item, "metricUnitsToggle")
            verify(toggle !== null)
            compare(toggle.checked, false)
            compare(toggle.subtitle, "Distances in miles (mi)")
            waitForRendering(screenLoader.item)
            mouseClick(toggle, toggle.width / 2, toggle.height / 2)
            compare(prefs.metricUnits, true)
            compare(toggle.checked, true)
            compare(toggle.subtitle, "Distances in kilometers (km)")

            // Reopening Settings must reflect the saved choice.
            screenLoader.source = ""
            screenLoader.source = uiDir + "/SettingsScreen.qml"
            verify(screenLoader.item !== null)
            toggle = findChild(screenLoader.item, "metricUnitsToggle")
            verify(toggle !== null)
            compare(toggle.checked, true)
            toggle.forceActiveFocus()
            keyClick(Qt.Key_Space)
            compare(prefs.metricUnits, false)
            compare(toggle.checked, false)
            compare(toggle.subtitle, "Distances in miles (mi)")
        }
    }
}
