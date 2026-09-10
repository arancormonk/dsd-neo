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
        name: "SettingsHangtime"
        when: windowShown

        function init() {
            testContext.useLifecycleHost(true)
            prefs.hangtimeSec = 2.0
            screenLoader.source = uiDir + "/SettingsScreen.qml"
            verify(screenLoader.item !== null)
        }

        function cleanup() {
            screenLoader.source = ""
            prefs.hangtimeSec = 2.0
            testContext.useLifecycleHost(false)
        }

        function test_edit_data() {
            return [
                {tag: "valid", text: "3.5", value: 3.5, valid: true},
                {tag: "text", text: "abc", value: 2.0, valid: false},
                {tag: "range", text: "45", value: 2.0, valid: false}
            ]
        }

        function test_edit(data) {
            var field = findChild(screenLoader.item, "hangtimePreferenceField")
            verify(field !== null)
            compare(field.text, "2.0")
            field.input.forceActiveFocus()
            field.input.selectAll()
            for (var i = 0; i < data.text.length; ++i)
                keyClick(data.text.charAt(i))
            keyClick(Qt.Key_Return)
            compare(prefs.hangtimeSec, data.value)
            compare(field.error, data.valid ? "" : "Enter seconds from 0 to 30, e.g. 2.0.")
        }
    }
}
