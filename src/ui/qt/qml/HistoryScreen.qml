// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// The call log, searchable like a phone's call history. Day groups, dimmed and
// tagged encrypted rows, filter pills for system and call type.
Item {
    id: screen

    // Cycles: 0 everything, 1 clear calls, 2 encrypted calls, 3 messages
    // (SMS, GPS positions, data and control notices).
    readonly property var kindLabels: [qsTr("All activity"), qsTr("Clear calls"), qsTr("Encrypted"), qsTr("Messages")]

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
        // TapHandlers never take exclusive grabs, so with the confirm sheet up a
        // tap on the dimmed search field would still pop the keyboard, a dimmed
        // pill would cycle its filter, and the dimmed Clear label would re-open
        // the sheet — same class of tap-through the list below guards against.
        enabled: !confirmClear.visible

        Item {
            width: parent.width
            height: 32

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("History")
                font.family: Theme.sans
                font.pixelSize: 24
                font.weight: Font.Bold
                font.letterSpacing: -0.24
                color: Theme.textPrimary
            }

            Text {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                visible: callHistory.count > 0
                text: qsTr("Clear")
                font.family: Theme.sans
                font.pixelSize: 14
                color: Theme.textSecondary

                TapHandler {
                    onTapped: confirmClear.visible = true
                }
            }
        }

        PlexTextField {
            id: search

            width: parent.width
            placeholderText: qsTr("Search talkgroup or unit")
            inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
            // Debounced: every keystroke otherwise re-evaluates the filter over
            // the full log, and on a phone that stutters the keyboard.
            onTextChanged: searchDebounce.restart()

            Timer {
                id: searchDebounce
                interval: 250
                onTriggered: historyView.filterText = search.text
            }
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
                onClicked: historyView.filterKind = (historyView.filterKind + 1) % screen.kindLabels.length
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
        // TapHandlers never take exclusive grabs, so a tap on the confirm sheet
        // would also scroll the list under the overlay.
        enabled: !confirmClear.visible
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
                // A notice row's payload (the SMS body, the GPS string) is its
                // meta line; identity only where it exists.
                if (model.kind === 1) {
                    var parts = []
                    if (model.tg > 0)
                        parts.push("TG " + model.tg)
                    if (model.src > 0)
                        parts.push("SRC " + model.src)
                    if (model.detail.length > 0)
                        parts.push(model.detail)
                    return parts.length > 0 ? parts.join(" · ") : qsTr("data message")
                }
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

        // An empty filtered view and an empty log are different situations: the
        // first must say the calls are hidden, not gone, or the filter pill left
        // active yesterday reads as a decoder that stopped logging.
        readonly property bool filtered: callHistory.count > 0

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: parent.filtered ? qsTr("No matching activity") : qsTr("No calls yet")
            font.family: Theme.sans
            font.pixelSize: 16
            font.weight: Font.DemiBold
            color: Theme.textSecondary
        }

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: parent.filtered ? qsTr("%n logged call(s) are hidden by the search or filters.", "",
                                         callHistory.count)
                                  : qsTr("Start listening on a system and every call lands here.")
            font.family: Theme.sans
            font.pixelSize: 13
            color: Theme.textSubdued
            wrapMode: Text.Wrap
        }

        Item { width: 1; height: 6; visible: parent.filtered }

        OutlineButton {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: parent.filtered
            width: Math.min(parent.width, 220)
            text: qsTr("Clear filters")
            onClicked: {
                search.text = ""
                historyView.filterText = ""
                historyView.filterSystem = ""
                historyView.filterKind = 0
            }
        }
    }

    // Clearing is destructive and irreversible (the persisted log goes too),
    // so it hides behind a confirm — same shape as the home screen's manage
    // sheet.
    Rectangle {
        id: confirmClear

        anchors.fill: parent
        visible: false
        color: Qt.alpha("#000000", 0.5)

        TapHandler {
            onTapped: confirmClear.visible = false
        }

        UiPanel {
            anchors.centerIn: parent
            width: parent.width - 2 * Theme.screenPadding
            height: confirmColumn.height + 2 * Theme.cardPadding

            Column {
                id: confirmColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: Theme.cardPadding
                spacing: 12

                Text {
                    width: parent.width
                    text: qsTr("Clear call history?")
                    font.family: Theme.sans
                    font.pixelSize: 17
                    font.weight: Font.Bold
                    color: Theme.textPrimary
                }

                Text {
                    width: parent.width
                    text: qsTr("Every logged call and message is removed. A call playing right now still gets logged.")
                    font.family: Theme.sans
                    font.pixelSize: 13
                    color: Theme.textSubdued
                    wrapMode: Text.Wrap
                }

                OutlineButton {
                    width: parent.width
                    text: qsTr("Clear history")
                    onClicked: {
                        callHistory.clearAll()
                        confirmClear.visible = false
                    }
                }

                OutlineButton {
                    width: parent.width
                    text: qsTr("Cancel")
                    onClicked: confirmClear.visible = false
                }
            }
        }
    }
}
