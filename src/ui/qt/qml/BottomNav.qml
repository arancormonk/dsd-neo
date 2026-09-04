// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Three-tab bottom navigation. The glyphs are drawn geometry — bars for Listen
// (the logo mark's shape), lines for History, a slider pair for Settings — so no
// icon font travels with the app.
Rectangle {
    id: nav

    property int currentIndex: 0
    signal selected(int index)

    height: 62
    color: Theme.dark ? Theme.bg : Theme.panel

    Rectangle {
        anchors.top: parent.top
        width: parent.width
        height: 1
        color: Theme.divider
    }

    Row {
        anchors.fill: parent

        Repeater {
            model: [qsTr("Listen"), qsTr("History"), qsTr("Settings")]

            Item {
                required property int index
                required property var modelData

                width: nav.width / 3
                height: nav.height

                readonly property bool active: nav.currentIndex === index
                readonly property color tone: active ? Theme.cyan : Theme.textSubdued

                Column {
                    anchors.centerIn: parent
                    spacing: 6

                    Item {
                        width: 20
                        height: 14
                        anchors.horizontalCenter: parent.horizontalCenter

                        // Listen: three bars of the logo mark.
                        Row {
                            visible: index === 0
                            anchors.centerIn: parent
                            spacing: 3

                            Rectangle { width: 3; height: 8; radius: 1.5; color: tone; anchors.bottom: parent.bottom }
                            Rectangle { width: 3; height: 14; radius: 1.5; color: tone; anchors.bottom: parent.bottom }
                            Rectangle { width: 3; height: 11; radius: 1.5; color: tone; anchors.bottom: parent.bottom }
                        }

                        // History: three list lines.
                        Column {
                            visible: index === 1
                            anchors.centerIn: parent
                            spacing: 3

                            Rectangle { width: 16; height: 2; radius: 1; color: tone }
                            Rectangle { width: 16; height: 2; radius: 1; color: tone }
                            Rectangle { width: 16; height: 2; radius: 1; color: tone }
                        }

                        // Settings: two slider tracks with offset thumbs.
                        Column {
                            visible: index === 2
                            anchors.centerIn: parent
                            spacing: 5

                            Item {
                                width: 16
                                height: 4

                                Rectangle { width: 16; height: 2; radius: 1; color: tone; anchors.verticalCenter: parent.verticalCenter }
                                Rectangle { width: 4; height: 4; radius: 2; color: tone; x: 10 }
                            }

                            Item {
                                width: 16
                                height: 4

                                Rectangle { width: 16; height: 2; radius: 1; color: tone; anchors.verticalCenter: parent.verticalCenter }
                                Rectangle { width: 4; height: 4; radius: 2; color: tone; x: 2 }
                            }
                        }
                    }

                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: modelData
                        font.family: Theme.sans
                        font.pixelSize: 11
                        font.weight: active ? Font.DemiBold : Font.Normal
                        color: tone
                    }
                }

                TapHandler {
                    onTapped: nav.selected(index)
                }
            }
        }
    }
}
