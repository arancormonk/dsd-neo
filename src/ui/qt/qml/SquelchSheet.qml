// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// The squelch on audio input (#628), opened from the monitor's Squelch row: a
// level in dB on the input's audio, or the noise squelch, which learns the
// noise above voice from the input itself and opens when it quiets by its
// margin, as a radio's squelch does. The auto squelch needs a radio input, whose
// channel power it learns a floor from, so it is not offered here; on a radio
// input the Radio sheet edits the squelch.
//
// Every control reads the engine, with the Radio sheet's one bounded exception:
// a request sent and not yet answered stands in for the reading until the
// engine's arrives or requestTtlMs passes, so taps inside one 250 ms poll each
// step from the last request rather than from the same stale reading.
ModalSheet {
    id: sheet
    objectName: "squelchSheet"
    accessibleName: qsTr("Squelch")

    property var bridge: commands
    readonly property bool sessionRunning: decoderHost.running
    onSessionRunningChanged: {
        if (!sessionRunning) {
            visible = false;
            forgetRequests();
        }
    }

    readonly property int requestTtlMs: 1500
    // NaN means "no outstanding request; the engine's reading is the truth".
    property real pendingSquelch: NaN
    // A noise squelch requested, by its margin.
    property real pendingSquelchMargin: NaN

    // Issue #518: while a scan row on air takes a "this channel" squelch edit,
    // the sheet can set that row's own squelch for the rest of the session
    // instead of the default. The row is captured when the scope is chosen,
    // and the sheet goes back to the default when another row comes on air:
    // the readings would no longer be the row's.
    readonly property int rowFields: metrics.scanRowActive === true ? metrics.scanRowEditable : 0
    readonly property bool rowTakesSquelch: (rowFields & sheet.bridge.scanRowFieldSquelch) !== 0
    property bool rowScopeChosen: false
    property var rowContext: ({})
    readonly property bool rowSquelch: rowScopeChosen && rowTakesSquelch
    // Only an nfm row takes a tone policy, and only it is an FM channel for the
    // noise squelch to quiet.
    readonly property bool rowNfm: rowSquelch && (rowFields & sheet.bridge.scanRowFieldTone) !== 0
    readonly property bool rowSynced: metrics.scanRowSynced === true
    readonly property bool squelchSteppable: !rowSquelch || rowSynced
    readonly property string onAirRow: metrics.scanRowActive === true ? metrics.scanRowKey : ""
    onOnAirRowChanged: {
        if (rowScopeChosen)
            chooseScope(false);
    }

    // A scan row with its own squelch keeps it in force while on air; the
    // controls then edit the default beneath it.
    readonly property bool squelchRowOverride: metrics.squelchRowOverride === true
    readonly property real baseSquelchDb: rowSquelch ? metrics.effectiveSquelchDb
        : (squelchRowOverride ? metrics.configuredSquelchDb : metrics.squelchDb)
    readonly property bool baseSquelchOff: rowSquelch ? metrics.effectiveSquelchOff
        : (squelchRowOverride ? metrics.configuredSquelchOff : metrics.squelchOff)
    readonly property real squelchDb: isNaN(pendingSquelch) ? baseSquelchDb : pendingSquelch
    readonly property bool squelchOff: isNaN(pendingSquelch) ? baseSquelchOff : pendingSquelch >= 0
    // The noise squelch needs an FM channel: an nfm row, or a default the AM
    // monitor on its own does not refuse.
    readonly property bool squelchNoiseOffered: rowSquelch ? rowNfm : metrics.squelchNoiseOffered === true
    readonly property bool baseSquelchNoise: rowSquelch ? metrics.effectiveSquelchNoise === true
        : metrics.configuredSquelchNoise === true
    // An auto setting from the command line or a config: it does not run on
    // audio input, so neither segment is chosen and the reading says why.
    readonly property bool baseSquelchAuto: rowSquelch ? metrics.effectiveSquelchAuto === true
        : metrics.configuredSquelchAuto === true
    readonly property int baseSquelchMargin: rowSquelch ? metrics.effectiveSquelchMarginDb
        : metrics.configuredSquelchMarginDb
    readonly property bool squelchNoise: !isNaN(pendingSquelchMargin) || (isNaN(pendingSquelch) && baseSquelchNoise)
    readonly property bool squelchAuto: isNaN(pendingSquelchMargin) && isNaN(pendingSquelch) && baseSquelchAuto
        && !baseSquelchNoise
    readonly property int squelchMargin: !isNaN(pendingSquelchMargin) ? pendingSquelchMargin
        : (baseSquelchMargin > 0 ? baseSquelchMargin : sheet.bridge.squelchMarginDefaultDb)
    // A setting that does not run here -- auto, or noise on an AM channel -- is
    // not stepped: neither segment is chosen, and dB (or Noise where an FM
    // channel takes it) replaces it.
    readonly property bool squelchHeld: squelchAuto || (squelchNoise && !squelchNoiseOffered)
    // The setting in force comes first; with a row override that is the row's,
    // and the default being edited is named below it.
    readonly property string squelchReading: (squelchRowOverride && !rowSquelch)
        ? squelchSettingText(metrics.effectiveSquelchAuto === true, metrics.effectiveSquelchNoise === true,
            metrics.effectiveSquelchMarginDb, metrics.effectiveSquelchOff, metrics.effectiveSquelchDb)
        : squelchSettingText(squelchAuto, squelchNoise, squelchMargin, squelchOff, squelchDb)
    // What the squelch in force shows: "learning" until the noise squelch has a
    // reference, its quieting, or why it is off here ("off: no band above
    // voice"). Held while a request is outstanding; it describes the old setting.
    readonly property string squelchStatus: (isNaN(pendingSquelch) && isNaN(pendingSquelchMargin)
        && metrics.squelchAutoStatus !== undefined) ? metrics.squelchAutoStatus : ""

    function openEditor() {
        forgetRequests();
        chooseScope(false);
        open();
    }
    /** Edit the default (false) or "this channel" (true), capturing the row on air for the latter. */
    function chooseScope(thisChannel) {
        forgetRequests();
        rowContext = thisChannel ? sheet.bridge.scanRowContext() : ({});
        // The controls show the metrics' row: a context from a newer snapshot
        // names a row whose values they do not show yet.
        rowScopeChosen = thisChannel && rowContext.active === true && rowContext.key === onAirRow && rowSynced;
        if (!rowScopeChosen)
            rowContext = ({});
    }
    function rowEdit(action, value) {
        sheet.bridge.editScanRow(rowContext, sheet.bridge.scanRowFieldSquelch, action, value);
    }
    /** "Use default" or "List value": the reading changes to a value the sheet did not compute. */
    function rowAction(action) {
        forgetRequests();
        rowEdit(action, {});
    }
    function forgetRequests() {
        pendingSquelch = NaN;
        pendingSquelchMargin = NaN;
        squelchTtl.stop();
    }

    /** A squelch setting as the sheet prints it: the noise squelch by its margin, a level in dB, or off. */
    function squelchSettingText(auto, noise, margin, off, db) {
        if (noise)
            return qsTr("noise +%1 dB").arg(margin);
        if (auto)
            return qsTr("auto +%1 dB").arg(margin);
        return off ? qsTr("off") : Math.round(db) + " dB";
    }

    /**
     * Nudge the level, 5 dB a step. Off sits one step below the floor, as on
     * the Radio sheet: stepping down from the floor switches the squelch off
     * (0), and stepping up from off lands back on the floor. -5 dB is the last
     * real threshold, because 0 is off.
     */
    function stepSquelch(delta) {
        if (!squelchSteppable || squelchHeld)
            return;
        // A row's own squelch is whole dB down to -100, the options cell's range.
        var floor = rowSquelch ? -100 : -120;
        var cur = squelchOff ? floor - 5 : squelchDb;
        var next = cur + delta;
        if (next < floor) {
            if (delta > 0) {
                next = floor;
            } else {
                if (squelchOff)
                    return;
                next = 0;
            }
        } else if (next > -5) {
            next = -5;
        }
        if (!squelchOff && Math.abs(next - squelchDb) < 0.001)
            return;
        requestSquelchDb(next);
    }
    /** Ask for a level of @p db (0 = off), on the row or the default. */
    function requestSquelchDb(db) {
        var margin = squelchMargin;
        pendingSquelchMargin = NaN;
        pendingSquelch = db;
        squelchTtl.restart();
        // A row keeps its margin through a level, for a switch back to Noise.
        if (rowSquelch)
            rowEdit(sheet.bridge.scanRowEditSet, {"squelchDb": Math.round(db), "squelchMarginDb": margin});
        else
            sheet.bridge.setSquelchDb(db);
    }
    /** Ask for the noise squelch with @p margin dB of quieting, on the row or the default. */
    function requestSquelchNoise(margin) {
        pendingSquelch = NaN;
        pendingSquelchMargin = margin;
        squelchTtl.restart();
        if (rowSquelch)
            rowEdit(sheet.bridge.scanRowEditSet, {"squelchMode": sheet.bridge.squelchModeNoise, "squelchMarginDb": margin});
        else
            sheet.bridge.setSquelchNoise(margin);
    }
    /** Nudge the noise squelch's margin, 1 dB a step within the range the engine takes. */
    function stepSquelchMargin(delta) {
        if (!squelchSteppable || !squelchNoise || squelchHeld)
            return;
        var next = Math.max(sheet.bridge.squelchMarginMinDb, Math.min(sheet.bridge.squelchMarginMaxDb, squelchMargin + delta));
        if (next === squelchMargin)
            return;
        requestSquelchNoise(next);
    }
    /**
     * The dB | Noise choice (@p index 0, 1). Noise starts from the margin the
     * setting last ran, or the default; dB goes back to the level the setting
     * kept beneath it.
     */
    function chooseSquelchMode(index) {
        var current = squelchHeld ? -1 : (squelchNoise ? 1 : 0);
        if (!squelchSteppable || index === current)
            return;
        if (index === 1) {
            if (squelchNoiseOffered)
                requestSquelchNoise(squelchMargin);
            return;
        }
        var levelOff = metrics.configuredSquelchLevelOff === true;
        var db = rowSquelch ? metrics.effectiveSquelchDb : metrics.configuredSquelchDb;
        if (rowSquelch) {
            // A row's own level is whole dB from -100 to -1 (0 is off there).
            requestSquelchDb(levelOff ? 0 : Math.min(-1, Math.max(-100, Math.round(db))));
            return;
        }
        // The default gets its level back exactly as stored.
        pendingSquelchMargin = NaN;
        pendingSquelch = levelOff ? 0 : Math.min(db, -0.001);
        squelchTtl.restart();
        sheet.bridge.restoreSquelchLevel();
    }

    Timer {
        id: squelchTtl

        interval: sheet.requestTtlMs
        onTriggered: {
            sheet.pendingSquelch = NaN;
            sheet.pendingSquelchMargin = NaN;
        }
    }

    Text {
        width: parent.width
        text: qsTr("Squelch")
        wrapMode: Text.Wrap
        color: Theme.textPrimary
        font.pixelSize: Theme.fontSize(20)
    }
    Text {
        width: parent.width
        text: qsTr("A level mutes audio below it. Noise opens when the FM noise above voice quiets by its margin, as a radio's squelch does: feed it the discriminator or unfiltered FM audio.")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(13)
    }
    // What the controls edit while a scan row on air takes "this channel" edits.
    SegmentedControl {
        objectName: "squelchScope"
        visible: sheet.rowTakesSquelch || sheet.rowSquelch
        width: parent.width
        model: [qsTr("All channels"), qsTr("This channel")]
        currentIndex: sheet.rowSquelch ? 1 : 0
        onSelected: function (index) {
            sheet.chooseScope(index === 1);
        }
    }
    Text {
        objectName: "squelchScopeNote"
        width: parent.width
        visible: sheet.rowSquelch
        text: qsTr("Applies to %1 for this session, not to the list or the default.").arg(sheet.rowContext.label || "")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(13)
    }
    SegmentedControl {
        objectName: "squelchMode"
        visible: sheet.squelchNoiseOffered || sheet.squelchHeld
        width: parent.width
        enabled: sheet.squelchSteppable
        model: sheet.squelchNoiseOffered ? [qsTr("dB"), qsTr("Noise")] : [qsTr("dB")]
        currentIndex: sheet.squelchHeld ? -1 : (sheet.squelchNoise ? 1 : 0)
        onSelected: function (index) {
            sheet.chooseSquelchMode(index);
        }
    }
    Row {
        width: parent.width
        spacing: 10
        Text {
            objectName: "squelchValue"
            width: parent.width - 116
            anchors.verticalCenter: parent.verticalCenter
            text: sheet.squelchReading
            // Read aloud as the terminal prints it, row note, default and status included.
            Accessible.role: Accessible.StaticText
            Accessible.name: sheet.squelchRowOverride ? metrics.squelchReadout
                : (sheet.squelchStatus !== "" ? sheet.squelchReading + " (" + sheet.squelchStatus + ")"
                    : sheet.squelchReading)
            color: Theme.textPrimary
            font.family: Theme.mono
            font.pixelSize: Theme.fontSize(14)
        }
        // In Noise the buttons step the margin, 1 dB at a time.
        OutlineButton {
            objectName: "squelchDown"
            width: 48
            text: "−"
            accessibleName: sheet.squelchNoise ? qsTr("Decrease Squelch Margin") : qsTr("Decrease Squelch")
            enabled: sheet.squelchSteppable && !sheet.squelchHeld
            onClicked: sheet.squelchNoise ? sheet.stepSquelchMargin(-1) : sheet.stepSquelch(-5)
        }
        OutlineButton {
            objectName: "squelchUp"
            width: 48
            text: "+"
            accessibleName: sheet.squelchNoise ? qsTr("Increase Squelch Margin") : qsTr("Increase Squelch")
            enabled: sheet.squelchSteppable && !sheet.squelchHeld
            onClicked: sheet.squelchNoise ? sheet.stepSquelchMargin(1) : sheet.stepSquelch(5)
        }
    }
    // The squelch in force: "learning" until the noise squelch has a reference,
    // its quieting, or why it is off here.
    Text {
        objectName: "squelchStatus"
        visible: sheet.squelchStatus !== ""
        width: parent.width
        text: sheet.squelchStatus
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.family: Theme.mono
        font.pixelSize: Theme.fontSize(12)
    }
    // "This channel": follow the default, offered when the list sets the
    // squelch, or go back to the list's value, offered while the row runs an edit.
    Row {
        objectName: "squelchRowActions"
        visible: sheet.rowSquelch && ((metrics.scanRowListed | metrics.scanRowEdited)
            & sheet.bridge.scanRowFieldSquelch) !== 0
        spacing: 8
        OutlineButton {
            objectName: "squelchRowInherit"
            visible: (metrics.scanRowListed & sheet.bridge.scanRowFieldSquelch) !== 0
            text: qsTr("Use default")
            accessibleName: qsTr("This Channel Uses the Default Squelch")
            onClicked: sheet.rowAction(sheet.bridge.scanRowEditInherit)
        }
        OutlineButton {
            objectName: "squelchRowReset"
            visible: (metrics.scanRowEdited & sheet.bridge.scanRowFieldSquelch) !== 0
            text: qsTr("List value")
            accessibleName: qsTr("This Channel Back to Its List Squelch")
            onClicked: sheet.rowAction(sheet.bridge.scanRowEditReset)
        }
    }
    // A row with its own squelch keeps it while on air: the controls edit the default.
    Text {
        objectName: "squelchRowNote"
        width: parent.width
        visible: sheet.squelchRowOverride && !sheet.rowSquelch
        text: qsTr("This channel sets its own squelch. Changes here apply to the default (%1), from the next channel without one.").arg(
            sheet.squelchSettingText(sheet.squelchAuto, sheet.squelchNoise, sheet.squelchMargin, sheet.squelchOff,
                sheet.squelchDb))
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(13)
    }
    OutlineButton {
        objectName: "squelchClose"
        width: parent.width
        height: Math.max(48, implicitHeight)
        text: qsTr("Close")
        onClicked: sheet.visible = false
    }
}
