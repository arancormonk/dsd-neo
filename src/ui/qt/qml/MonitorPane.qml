// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// What a running session looks like. The settings that produced it collapse to the
// chip at the top: they cannot take effect until the next start, so leaving the form
// open would invite edits that silently do nothing.
ColumnLayout {
    id: monitor

    property string summaryText: ""
    signal summaryClicked()

    function scrollToNewest() {
        events.positionViewAtBeginning()
    }

    spacing: 10

    ItemDelegate {
        Layout.fillWidth: true
        text: monitor.summaryText
        onClicked: monitor.summaryClicked()

        contentItem: RowLayout {
            spacing: 8

            Label {
                Layout.fillWidth: true
                elide: Text.ElideMiddle
                font.pixelSize: 13
                opacity: 0.85
                text: monitor.summaryText
            }

            Label {
                opacity: 0.6
                text: "›"
            }
        }
    }

    StatusCard {
        Layout.fillWidth: true
    }

    GroupBox {
        title: qsTr("Events")
        Layout.fillWidth: true
        Layout.fillHeight: true

        EventLogView {
            id: events
            anchors.fill: parent
        }
    }
}
