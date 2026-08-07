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
            onTextChanged: historyView.filterText = text
        }

        Row {
            spacing: 10

            // Keyed on the filter string itself, never an index: the label array
            // reorders as calls land, and a frozen index would let the pill show
            // one system while the model filters another.
            FilterPill {
                text: historyView.filterSystem.length === 0 ? qsTr("All systems") : historyView.filterSystem
                active: historyView.filterSystem.length > 0
                onClicked: {
                    var labels = callHistory.systemLabels
                    if (labels.length === 0) {
                        historyView.filterSystem = ""
                        return
                    }
                    var idx = labels.indexOf(historyView.filterSystem)
                    historyView.filterSystem = idx + 1 >= labels.length ? "" : labels[idx + 1]
                }
            }

            FilterPill {
                text: screen.kindLabels[historyView.filterKind]
                active: historyView.filterKind !== 0
                onClicked: historyView.filterKind = (historyView.filterKind + 1) % 3
            }
        }
    }

    ListView {
        id: logList

        anchors.top: chrome.bottom
        anchors.topMargin: 8
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        model: historyView

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
                // "encrypted" states what the call was, nothing more: whether it was
                // skipped depended on the lockout toggle and loaded keys at the time,
                // which a logged row does not know. The duration is measured either way.
                var meta = "TG " + model.tg
                if (model.src > 0)
                    meta += " · SRC " + model.src
                if (model.enc)
                    meta += " · " + qsTr("encrypted")
                if (model.durationSecs >= 0)
                    meta += " · " + Util.fmtDuration(model.durationSecs)
                return meta
            }
            rightText: model.timeText
            enc: model.enc
        }
    }

    // A sibling of the view, not a child: ListView reparents declared children
    // into its contentItem, where `parent.count` is undefined and the empty state
    // would never show.
    Column {
        anchors.centerIn: logList
        width: logList.width - 2 * Theme.screenPadding
        visible: logList.count === 0
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
