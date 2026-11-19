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
// Follow. One gap row says where positions may be missing. A History row's
// pin ("Show on map") leads the list as its own row, as it leads the map, and
// the device's own dot follows it: where it is and how sure, in words.
Item {
    id: list

    // The selected and the followed track, drawn as the map draws them.
    property double selectedTrackId: 0
    property double followTrackId: 0
    // The device's position, for each row's distance from it and its own
    // row: live (following on the map) or the saved one-shot fix, saved at
    // deviceSavedAtMs (Unix ms).
    property bool deviceKnown: false
    property bool deviceLive: false
    property double deviceLatitude: 0
    property double deviceLongitude: 0
    property double deviceAccuracyM: 0
    property double deviceSavedAtMs: 0
    // The widest the rows grow on a tablet; centred beyond it.
    property real maximumRowWidth: 840

    // A History row's pin ({name, when, position: {...}}), or null.
    property var pin: null

    signal activated(double trackId)
    signal pinActivated
    signal deviceActivated

    readonly property bool metric: typeof prefs !== "undefined" && prefs ? prefs.metricUnits !== false : true
    readonly property alias view: rows
    readonly property int count: rows.count
    // The height its rows take from the top: the pin's and the device's rows
    // (the list's header), then the radios'.
    readonly property real usedHeight: rows.contentHeight

    ListView {
        id: rows
        objectName: "mapList"

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.topMargin: 4
        anchors.leftMargin: Math.max(0, (parent.width - list.maximumRowWidth) / 2)
        anchors.rightMargin: anchors.leftMargin
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        // Set by place below, only while the list is visible (issue #782).
        model: null

        // The History pin's row and the device's own row lead the list and
        // scroll with it: one scroll surface for every row, so a short
        // landscape List at a large text size still reaches every radio
        // (review r2). Their heights follow whether there is a pin or a dot,
        // not whether the List shows: a hidden List keeps its header's size,
        // so it opens at its start with both rows in view (a header that grew
        // only once shown left the view below them).
        header: Item {
            width: rows.width
            height: pinRow.height + deviceRow.height

            // The pin's row: what it is, whose, and its states, in the rows' grammar.
            Item {
                id: pinRow
                objectName: "mapPinRow"

                readonly property var position: list.pin && list.pin.position ? list.pin.position : ({})
                readonly property string name: list.pin ? (list.pin.name || list.pin.sourceName || qsTr("Saved position")) : ""
                // With its date, as the pin's sheet says it: a History row can be
                // from any day, unlike the live rows below.
                readonly property double whenS: !list.pin ? 0 : pinRow.position.receivedTime > 0 ? pinRow.position.receivedTime : (list.pin.when || 0)
                readonly property string when: whenS > 0 ? Qt.formatDateTime(new Date(whenS * 1000), "yyyy-MM-dd hh:mm") : ""
                readonly property string whenFull: whenS > 0 ? Qt.formatDateTime(new Date(whenS * 1000), "yyyy-MM-dd hh:mm:ss") : ""
                readonly property string states: MapText.stateWords(pinRow.position).join(" · ")

                visible: list.pin !== null
                width: parent.width
                height: list.pin !== null ? Math.max(Theme.rowHeight, pinLabels.implicitHeight + 24) : 0

                activeFocusOnTab: (navigationAllowed && visible) || activeFocus
                Accessible.role: Accessible.Button
                Accessible.name: navigationAllowed && visible ? qsTr("Saved position from History: %1").arg(name) : ""
                Accessible.description: navigationAllowed && visible ? [whenFull.length > 0 ? qsTr("received %1").arg(whenFull) : "", pinRow.position.kindText || "", states].filter(function (part) {
                    return part.length > 0;
                }).join(", ") : ""
                readonly property bool navigationAllowed: Navigation.allows(pinRow)
                Accessible.focusable: navigationAllowed && visible
                Accessible.ignored: !navigationAllowed || !visible
                Accessible.onPressAction: activate()
                function activate() {
                    if (visible && Navigation.allows(pinRow))
                        list.pinActivated();
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
                    onAllowedTap: pinRow.activate()
                }

                Column {
                    id: pinLabels
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: 18
                    anchors.rightMargin: 18
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3

                    Item {
                        width: parent.width
                        height: Math.max(pinName.implicitHeight, pinWhen.implicitHeight)
                        Text {
                            id: pinName
                            anchors.left: parent.left
                            anchors.right: pinWhen.left
                            anchors.rightMargin: 12
                            text: Util.oneLine(pinRow.name)
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(16)
                            font.weight: Font.DemiBold
                            color: pinRow.position.emergency ? Theme.alert : Theme.textPrimary
                        }
                        Text {
                            id: pinWhen
                            anchors.right: parent.right
                            anchors.baseline: pinName.baseline
                            text: pinRow.when
                            font.family: Theme.mono
                            font.pixelSize: Theme.fontSize(12)
                            color: Theme.textSubdued
                        }
                    }
                    Text {
                        width: parent.width
                        text: [qsTr("Saved position from History"), pinRow.position.kindText || ""].filter(function (part) {
                            return part.length > 0;
                        }).join(" · ")
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(14)
                        color: Theme.textSecondary
                    }
                    Text {
                        width: parent.width
                        visible: text.length > 0
                        text: pinRow.states
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(13)
                        font.weight: Font.DemiBold
                        color: Theme.textPrimary
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

            // The device's dot: the canvas draws it with its accuracy ring and keeps
            // it out of the accessibility tree, so here it is in words (review r1).
            Item {
                id: deviceRow
                objectName: "mapDeviceRow"

                readonly property string name: list.deviceLive ? qsTr("Your position") : qsTr("Your saved location")
                readonly property string saved: !list.deviceLive && list.deviceSavedAtMs > 0 ? Qt.formatDateTime(new Date(list.deviceSavedAtMs), "hh:mm") : ""
                readonly property string position: qsTr("Position %1").arg(MapText.coordinateText(list.deviceLatitude, list.deviceLongitude))
                readonly property string accuracy: MapText.accuracyText(list.deviceAccuracyM, false, list.metric)

                visible: list.deviceKnown
                y: pinRow.height
                width: parent.width
                height: list.deviceKnown ? Math.max(Theme.rowHeight, deviceLabels.implicitHeight + 24) : 0

                activeFocusOnTab: (navigationAllowed && visible) || activeFocus
                Accessible.role: Accessible.Button
                Accessible.name: navigationAllowed && visible ? name : ""
                Accessible.description: navigationAllowed && visible ? [saved.length > 0 ? qsTr("saved at %1").arg(saved) : "", position, accuracy].filter(function (part) {
                    return part.length > 0;
                }).join(", ") : ""
                readonly property bool navigationAllowed: Navigation.allows(deviceRow)
                Accessible.focusable: navigationAllowed && visible
                Accessible.ignored: !navigationAllowed || !visible
                Accessible.onPressAction: activate()
                function activate() {
                    if (visible && Navigation.allows(deviceRow))
                        list.deviceActivated();
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
                    onAllowedTap: deviceRow.activate()
                }

                Column {
                    id: deviceLabels
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: 18
                    anchors.rightMargin: 18
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3

                    Item {
                        width: parent.width
                        height: Math.max(deviceName.implicitHeight, deviceSaved.implicitHeight)
                        Text {
                            id: deviceName
                            anchors.left: parent.left
                            anchors.right: deviceSaved.left
                            anchors.rightMargin: 12
                            text: deviceRow.name
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(16)
                            font.weight: Font.DemiBold
                            color: Theme.textPrimary
                        }
                        Text {
                            id: deviceSaved
                            anchors.right: parent.right
                            anchors.baseline: deviceName.baseline
                            text: deviceRow.saved
                            font.family: Theme.mono
                            font.pixelSize: Theme.fontSize(12)
                            color: Theme.textSubdued
                        }
                    }
                    Text {
                        width: parent.width
                        text: deviceRow.position
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(14)
                        color: Theme.textSecondary
                    }
                    Text {
                        width: parent.width
                        visible: text.length > 0
                        text: deviceRow.accuracy
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(14)
                        color: Theme.textSecondary
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

        delegate: Item {
            id: row

            required property var model

            readonly property double rowTrack: model.trackId
            readonly property bool gap: model.gap === true
            readonly property bool selected: !gap && list.selectedTrackId > 0 && model.trackId === list.selectedTrackId
            readonly property bool followed: !gap && list.followTrackId > 0 && model.trackId === list.followTrackId
            readonly property string name: MapText.trackName(model)
            // Final review M1 and its review: the row describes the marks the
            // map draws for it under the active filter: the filled marker's
            // report (currentFix) while it is drawn, else the open ring's,
            // each with its own age, format and states; a drawn ring beside a
            // drawn marker gets its own words.
            readonly property bool currentDrawn: !gap && model.currentShown === true
            readonly property bool ringDrawn: !gap && model.unknownShown === true
            readonly property bool currentFirst: currentDrawn || (!ringDrawn && model.hasPosition === true)
            readonly property var shownFix: gap ? ({}) : currentFirst ? (model.currentFix || ({})) : (model.unknownFix || ({}))
            readonly property double age: gap ? -1 : currentFirst ? model.ageSeconds : model.unknownAgeSeconds
            readonly property string ageShown: gap ? "" : MapText.ageText(age).length > 0 ? MapText.ageText(age) : (shownFix.timeText || "")
            readonly property double fixLat: Number(shownFix.latitude) || 0
            readonly property double fixLon: Number(shownFix.longitude) || 0
            readonly property string what: {
                if (gap)
                    return "";
                var parts = [];
                if (shownFix.kindText && shownFix.kindText.length > 0)
                    parts.push(shownFix.kindText);
                // The shown report's recipient: each report names its own.
                if (shownFix.targetId > 0)
                    parts.push(shownFix.targetGroup ? qsTr("TG %1").arg(Util.idText(shownFix.targetId)) : qsTr("to %1").arg(Util.idText(shownFix.targetId)));
                if (!model.anonymous && model.sourceId > 0 && name.indexOf(Util.idText(model.sourceId)) < 0)
                    parts.push(qsTr("ID %1").arg(Util.idText(model.sourceId)));
                if (list.deviceKnown && (model.hasPosition || model.hasUnknownFix))
                    parts.push(qsTr("%1 away").arg(MapText.lengthText(MapText.distanceMetres(list.deviceLatitude, list.deviceLongitude, fixLat, fixLon), list.metric)));
                return parts.join(" · ");
            }
            // Emergency has its tag; the rest of the states are words here: the
            // shown fix's, the track's source, then the ring's beside it.
            // A LIP report up to 5 or 30 minutes old when sent says so: the
            // row's age is only when it arrived.
            readonly property var states: gap ? [] : MapText.fixWords(shownFix).concat(shownFix.ageAtSendMaxSeconds >= 300 ? [MapText.sentAgeText(shownFix.ageAtSendMaxSeconds)] : [])
                .concat(model.anonymous ? [qsTr("Radio not confirmed")] : [])
                .concat(currentDrawn && ringDrawn ? [MapText.ringWords(model.unknownFix)] : [])
            readonly property string gapTitle: model.timeText && model.timeText.length > 0 ? qsTr("Positions may be missing from %1").arg(model.timeText) : qsTr("Positions may be missing")
            readonly property string gapDetail: qsTr("Reports that came while the app could not read them were lost. Trails break here.")
            readonly property string spokenNow: gap ? gapTitle : (MapText.agoText(age, shownFix.timeText).length > 0 ? name + ", " + MapText.agoText(age, shownFix.timeText) : name)

            width: ListView.view ? ListView.view.width : 0
            implicitHeight: Math.max(Theme.rowHeight, (gap ? gapLabels.implicitHeight : labels.implicitHeight) + 24)
            height: implicitHeight

            // One node per row, read whole; its age at the pacer's step. The
            // node is a child that fills the row, never the row: the view
            // writes Accessible.ignored on each row's root as it lays it out
            // (issue #800, F1), and a row that was its own node came back
            // into the tree under the position sheet as fixes landed.
            // The keyboard lands on the node (Tab, a sheet's focus return): a
            // focus given to the row itself is passed on to it.
            onActiveFocusChanged: {
                if (activeFocus)
                    rowNode.forceActiveFocus();
            }
            readonly property bool navigationAllowed: Navigation.allows(row)
            readonly property Item node: rowNode
            // The root gates itself too: a cover with no re-layout leaves
            // nothing of it; re-laid, it is a bare node offering nothing.
            Accessible.ignored: !navigationAllowed || !visible
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

            Item {
                id: rowNode
                objectName: "mapRowNode"
                // Sized by binding, not anchors: a row the view builds is measured at once.
                width: row.width
                height: row.height
                readonly property bool reachable: row.navigationAllowed && row.visible
                Accessible.role: row.gap ? Accessible.StaticText : Accessible.Button
                Accessible.name: reachable ? pacer.spoken : ""
                Accessible.description: reachable ? (row.gap ? row.gapDetail : [row.model.emergency ? qsTr("Emergency") : "", row.what].concat(row.states).concat([row.selected ? qsTr("selected") : "", row.followed ? qsTr("followed on the map") : ""]).filter(function (part) {
                    return part.length > 0;
                }).join(", ")) : ""
                Accessible.focusable: reachable
                Accessible.ignored: !reachable
                Accessible.onPressAction: row.activate()
                // The row's Tab stop (see OutlineButton); keys reach the row from here.
                activeFocusOnTab: (row.navigationAllowed && !row.gap && row.visible) || activeFocus
                FocusFrame {}

                // The row's text inside its node: the node's words are its lines'.
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
