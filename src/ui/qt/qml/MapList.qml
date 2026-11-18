// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util
import "MapText.js" as MapText

// Issue #800: the map as a list, its complete equivalent for a screen reader
// and for anyone who wants names rather than marks. One row per radio the
// filter shows (positionView), newest first, in Activity's grammar: who on the
// first line with how long ago at its end, what sent it on the second, the
// fix's states in words on the third. Rows update in place and never move
// (the model's rule); a row's spoken name follows its age at a screen
// reader's pace. A tap opens the same sheet a marker opens, with the same
// Follow. One gap row says where positions may be missing.
Item {
    id: list

    // The selected and the followed track, drawn as the map draws them.
    property double selectedTrackId: 0
    property double followTrackId: 0
    // The device's position, for each row's distance from it.
    property bool deviceKnown: false
    property double deviceLatitude: 0
    property double deviceLongitude: 0
    // The widest the rows grow on a tablet; centred beyond it.
    property real maximumRowWidth: 840

    signal activated(double trackId)

    readonly property bool metric: typeof prefs !== "undefined" && prefs ? prefs.metricUnits !== false : true
    readonly property alias view: rows
    readonly property int count: rows.count

    ListView {
        id: rows
        objectName: "mapList"

        anchors.fill: parent
        anchors.topMargin: 4
        anchors.leftMargin: Math.max(0, (parent.width - list.maximumRowWidth) / 2)
        anchors.rightMargin: anchors.leftMargin
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        // Set by place below, only while the list is visible (issue #782).
        model: null

        delegate: Item {
            id: row

            required property var model

            readonly property double rowTrack: model.trackId
            readonly property bool gap: model.gap === true
            readonly property bool selected: !gap && list.selectedTrackId > 0 && model.trackId === list.selectedTrackId
            readonly property bool followed: !gap && list.followTrackId > 0 && model.trackId === list.followTrackId
            readonly property string name: MapText.trackName(model)
            readonly property double age: MapText.newestAge(model)
            readonly property string ageShown: gap ? "" : MapText.ageText(age).length > 0 ? MapText.ageText(age) : (model.timeText || "")
            readonly property double fixLat: model.hasPosition ? model.latitude : model.unknownLatitude
            readonly property double fixLon: model.hasPosition ? model.longitude : model.unknownLongitude
            readonly property string what: {
                if (gap)
                    return "";
                var parts = [];
                if (model.kindText && model.kindText.length > 0)
                    parts.push(model.kindText);
                if (model.targetId > 0)
                    parts.push(model.targetGroup ? qsTr("TG %1").arg(Util.idText(model.targetId)) : qsTr("to %1").arg(Util.idText(model.targetId)));
                if (!model.anonymous && model.sourceId > 0 && name.indexOf(Util.idText(model.sourceId)) < 0)
                    parts.push(qsTr("ID %1").arg(Util.idText(model.sourceId)));
                if (list.deviceKnown && (model.hasPosition || model.hasUnknownFix))
                    parts.push(qsTr("%1 away").arg(MapText.lengthText(MapText.distanceMetres(list.deviceLatitude, list.deviceLongitude, fixLat, fixLon), list.metric)));
                return parts.join(" · ");
            }
            // Emergency has its tag; the rest of the states are words here.
            readonly property var states: MapText.stateWords(model).filter(function (word) {
                return word !== qsTr("Emergency");
            })
            readonly property string gapTitle: model.timeText && model.timeText.length > 0 ? qsTr("Positions may be missing from %1").arg(model.timeText) : qsTr("Positions may be missing")
            readonly property string gapDetail: qsTr("Reports that came while the app could not read them were lost. Trails break here.")
            readonly property string spokenNow: gap ? gapTitle : (MapText.agoText(age, model.timeText).length > 0 ? name + ", " + MapText.agoText(age, model.timeText) : name)

            width: ListView.view ? ListView.view.width : 0
            implicitHeight: Math.max(Theme.rowHeight, (gap ? gapLabels.implicitHeight : labels.implicitHeight) + 24)
            height: implicitHeight

            // One node per row, read whole; its age at the pacer's step.
            activeFocusOnTab: (navigationAllowed && !gap && visible) || activeFocus
            Accessible.role: gap ? Accessible.StaticText : Accessible.Button
            Accessible.name: navigationAllowed && visible ? pacer.spoken : ""
            Accessible.description: navigationAllowed && visible ? (gap ? gapDetail : [model.emergency ? qsTr("Emergency") : "", what].concat(states).concat([selected ? qsTr("selected") : "", followed ? qsTr("followed on the map") : ""]).filter(function (part) {
                return part.length > 0;
            }).join(", ")) : ""
            readonly property bool navigationAllowed: Navigation.allows(row)
            Accessible.focusable: navigationAllowed && visible
            Accessible.ignored: !navigationAllowed || !visible
            Accessible.onPressAction: activate()
            function activate() {
                if (!gap && Navigation.allows(row))
                    list.activated(model.trackId);
            }
            Keys.onPressed: function (event) {
                if (Navigation.activationPress(event, Window.activeFocusItem))
                    activate();
            }
            Keys.onReleased: function (event) {
                Navigation.activationRelease(event);
            }
            FocusFrame {}
            GatedTapHandler {
                onAllowedTap: row.activate()
            }
            SpeechPacer {
                id: pacer
                shown: row.navigationAllowed && row.visible
                step: String(row.rowTrack) + ":" + (row.model.receivedTime || 0) + ":" + row.name
                read: function () {
                    return row.spokenNow;
                }
            }

            Rectangle {
                anchors.fill: parent
                visible: row.selected
                color: Theme.chipSelectedFill
            }

            Column {
                id: labels
                visible: !row.gap
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 18
                anchors.rightMargin: 18
                anchors.verticalCenter: parent.verticalCenter
                spacing: 3

                // The name, with how long ago at its end (and the emergency
                // tag before it). A long name is cut at its end; the age never.
                Item {
                    width: parent.width
                    height: Math.max(nameText.implicitHeight, stamp.height)

                    Text {
                        id: nameText
                        objectName: "mapRowName"
                        anchors.left: parent.left
                        anchors.right: stamp.left
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: Util.oneLine(row.name)
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(16)
                        font.weight: Font.DemiBold
                        // Emergency outranks selection (the map's ruling): the
                        // row's tint and description still say it is selected.
                        color: row.model.emergency ? Theme.alert : row.selected ? Theme.cyan : Theme.textPrimary
                    }

                    Row {
                        id: stamp
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 8

                        EmergencyTag {
                            objectName: "mapRowEmergency"
                            visible: row.model.emergency === true
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        MapGlyph {
                            objectName: "mapRowFollowed"
                            visible: row.followed
                            shape: "follow"
                            color: Theme.cyan
                            width: 16
                            height: 16
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            objectName: "mapRowAge"
                            anchors.verticalCenter: parent.verticalCenter
                            text: row.ageShown
                            font.family: Theme.mono
                            font.pixelSize: Theme.fontSize(12)
                            color: Theme.textSubdued
                        }
                    }
                }

                Text {
                    objectName: "mapRowWhat"
                    width: parent.width
                    visible: text.length > 0
                    text: row.what
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(14)
                    color: Theme.textSecondary
                }

                Text {
                    objectName: "mapRowStates"
                    width: parent.width
                    visible: text.length > 0
                    text: row.states.join(" · ")
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(13)
                    font.weight: Font.DemiBold
                    color: Theme.textPrimary
                }
            }

            Column {
                id: gapLabels
                visible: row.gap
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 18
                anchors.rightMargin: 18
                anchors.verticalCenter: parent.verticalCenter
                spacing: 3

                Text {
                    objectName: "mapGapTitle"
                    width: parent.width
                    text: row.gapTitle
                    wrapMode: Text.Wrap
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(14)
                    font.weight: Font.DemiBold
                    color: Theme.textSecondary
                }
                Text {
                    width: parent.width
                    text: row.gapDetail
                    wrapMode: Text.Wrap
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(13)
                    color: Theme.textSubdued
                }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 18
                height: 1
                color: Theme.divider
            }
        }
    }

    // Keeps the newest radio in view for a reader at the top of the list.
    FollowLatest {
        id: follow
        list: rows
    }

    // Hidden (the map shown, or the screen closed), the list holds no rows
    // (issue #782): fixes keep landing, and every row built under a hidden
    // parent is announced as hidden. Shown again, the reader is back on the
    // same radio at the same height.
    KeepPlace {
        id: place
        list: rows
        model: positionView
        atStart: follow.atLatest
        keyOf: function (row) {
            return row.rowTrack;
        }
        rowOf: function (trackId) {
            return positionView.rowForTrack(trackId);
        }
    }

    // A new age filter is a new question, answered from the top (as History's).
    Connections {
        target: positions
        function onAgeFilterSecondsChanged() {
            place.forget();
            toTopLater.restart();
        }
    }
    Timer {
        id: toTopLater
        interval: 0
        onTriggered: rows.positionViewAtBeginning()
    }
}
