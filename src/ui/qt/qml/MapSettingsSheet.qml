// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Issue #800: where the map's tiles come from. Tiles stay off until turned on
// here or from the map, and turning them on names the server the viewed area
// is sent to. The address is a URL template (https only, with {z}, {x} and
// {y}), checked by the tile source itself; the attribution is what the map
// shows at all times. A form sheet: Save applies, Cancel, Back or Escape ask
// before an edit goes.
ModalSheet {
    id: sheet

    // The tile source the map uses, for its template checks and availability.
    property var tileSource: null
    // What is saved now; the form starts from these and Save hands them back.
    property bool savedEnabled: false
    property string savedUrl: ""
    property string savedAttribution: ""
    property string defaultUrl: ""
    property string defaultAttribution: ""

    // The form.
    property bool draftEnabled: false

    signal saved(bool enabled, string url, string attribution)

    objectName: "mapSettingsSheet"
    accessibleName: qsTr("Map settings")
    closesOnOutsideTap: false

    readonly property bool available: !!tileSource && tileSource.available
    readonly property string trimmedUrl: urlField.text.trim()
    readonly property bool urlValid: !!tileSource && tileSource.isValidTemplate(trimmedUrl)
    readonly property string host: urlValid ? tileSource.templateHost(trimmedUrl) : ""
    readonly property string trimmedAttribution: attributionField.text.trim()
    readonly property bool changed: draftEnabled !== savedEnabled || trimmedUrl !== savedUrl || trimmedAttribution !== savedAttribution

    function openEditor() {
        draftEnabled = savedEnabled;
        urlField.text = savedUrl;
        attributionField.text = savedAttribution;
        open();
    }
    function closeEditor() {
        visible = false;
        Qt.inputMethod.hide();
    }
    function requestClose() {
        if (visible && changed)
            discard.ask(closeEditor);
        else
            closeEditor();
    }
    function save() {
        if (!urlValid || trimmedAttribution.length === 0)
            return;
        saved(draftEnabled && available, trimmedUrl, trimmedAttribution);
        closeEditor();
    }
    dismissHandler: requestClose
    DiscardDialog {
        id: discard
        parent: sheet
    }

    ReadableText {
        objectName: "mapSettingsTitle"
        heading: true
        width: parent.width
        text: qsTr("Map settings")
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(20)
        font.weight: Font.DemiBold
        color: Theme.textPrimary
    }

    ToggleRow {
        objectName: "mapSettingsTiles"
        width: parent.width
        enabled: sheet.available
        title: qsTr("Load map tiles")
        subtitle: !sheet.available ? qsTr("Online map tiles are not available in this build.")
            : sheet.host.length > 0 ? qsTr("Sends the area you view to %1. Off, the map is a plain grid.").arg(sheet.host)
            : qsTr("Off, the map is a plain grid.")
        checked: sheet.draftEnabled && sheet.available
        onToggled: function (on) {
            sheet.draftEnabled = on;
        }
    }

    PlexTextField {
        id: urlField
        objectName: "mapSettingsUrl"
        width: parent.width
        mono: true
        label: qsTr("Tile address")
        hint: qsTr("An https address for PNG tiles, with {z}, {x} and {y} where the tile's zoom and position go.")
        error: sheet.trimmedUrl.length > 0 && !sheet.urlValid ? qsTr("Use an https address that has {z}, {x} and {y} once each.") : sheet.trimmedUrl.length === 0 ? qsTr("Enter a tile address.") : ""
        inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
        nextField: attributionField
    }

    PlexTextField {
        id: attributionField
        objectName: "mapSettingsAttribution"
        width: parent.width
        label: qsTr("Attribution")
        hint: qsTr("Shown on the map at all times, as the tile server's terms ask.")
        error: sheet.trimmedAttribution.length === 0 ? qsTr("Enter the attribution the tile server asks for.") : ""
    }

    OutlineButton {
        objectName: "mapSettingsDefaults"
        width: parent.width
        visible: sheet.trimmedUrl !== sheet.defaultUrl || sheet.trimmedAttribution !== sheet.defaultAttribution
        text: qsTr("Use OpenStreetMap")
        onClicked: {
            urlField.text = sheet.defaultUrl;
            attributionField.text = sheet.defaultAttribution;
        }
    }

    GradientButton {
        objectName: "mapSettingsSave"
        width: parent.width
        text: qsTr("Save")
        inputValid: sheet.urlValid && sheet.trimmedAttribution.length > 0
        onClicked: sheet.save()
    }
    OutlineButton {
        objectName: "mapSettingsCancel"
        width: parent.width
        text: qsTr("Cancel")
        onClicked: sheet.requestClose()
    }
}
