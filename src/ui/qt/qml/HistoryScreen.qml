// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// The call log, searchable like a phone's call history. Day groups, dimmed and
// tagged encrypted rows, filter pills for system and call type.
Item {
    id: screen

    // Cycles: 0 all calls, 1 clear only, 2 encrypted only.
    readonly property var kindLabels: [qsTr("All calls"), qsTr("Clear calls"), qsTr("Encrypted")]
    // -1 = all systems, otherwise index into callHistory.systemLabels.
    property int systemIndex: -1

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    Column {
        id: chrome

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.screenPadding
        spacing: Theme.gap

        Text {
            text: qsTr("History")
            font.family: Theme.sans
            font.pixelSize: 24
            font.weight: Font.Bold
            font.letterSpacing: -0.24
            color: Theme.textPrimary
        }

        PlexTextField {
            id: search

            width: parent.width
            placeholderText: qsTr("Search talkgroup or unit")
            inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
            onTextChanged: callHistory.filterText = text
        }

        Row {
            spacing: 10

            FilterPill {
                text: screen.systemIndex < 0
                      ? qsTr("All systems")
                      : callHistory.systemLabels[screen.systemIndex]
                active: screen.systemIndex >= 0
                onClicked: {
                    var labels = callHistory.systemLabels
                    if (labels.length === 0)
                        return
                    screen.systemIndex = screen.systemIndex + 1 >= labels.length ? -1 : screen.systemIndex + 1
                    callHistory.filterSystem = screen.systemIndex < 0 ? "" : labels[screen.systemIndex]
                }
            }

            FilterPill {
                text: screen.kindLabels[callHistory.filterKind]
                active: callHistory.filterKind !== 0
                onClicked: callHistory.filterKind = (callHistory.filterKind + 1) % 3
            }
        }
    }

    ListView {
        anchors.top: chrome.bottom
        anchors.topMargin: 8
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        model: callHistory

        section.property: "dayLabel"
        section.delegate: MicroLabel {
            required property string section

            width: ListView.view ? ListView.view.width : 0
            height: 34
            leftPadding: Theme.screenPadding
            verticalAlignment: Text.AlignBottom
            bottomPadding: 8
            text: section
        }

        delegate: CallRow {
            width: ListView.view.width
            name: model.name
            metaText: {
                var meta = "TG " + model.tg
                if (model.src > 0)
                    meta += " · SRC " + model.src
                if (model.enc)
                    meta += " · " + qsTr("encrypted") + " · " + qsTr("skipped")
                else if (model.durationSecs >= 0)
                    meta += " · " + Util.fmtDuration(model.durationSecs)
                return meta
            }
            rightText: model.timeText
            enc: model.enc
        }

        Column {
            anchors.centerIn: parent
            width: parent.width - 2 * Theme.screenPadding
            visible: parent.count === 0
            spacing: 8

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("No calls yet")
                font.family: Theme.sans
                font.pixelSize: 16
                font.weight: Font.DemiBold
                color: Theme.textSecondary
            }

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Start listening on a system and every call lands here.")
                font.family: Theme.sans
                font.pixelSize: 13
                color: Theme.textSubdued
                wrapMode: Text.Wrap
            }
        }
    }
}
