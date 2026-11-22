// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

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
//
// Issue #899 (B7): a default-scope change on a saved system's session is saved
// to it as the Radio sheet's is (settingRequested, through Main's Radio
// settings persistence); "This channel" edits stay with the session.
ModalSheet {
    id: sheet
    objectName: "squelchSheet"
    accessibleName: qsTr("Squelch")

    property var bridge: commands
    property bool savesToSystem: false
    property string saveError: ""
    // Issue #899 (B11): the saved system's own squelch ("" for none), and
    // whether Clear has taken it out for the next start.
    property string savedSquelch: ""
    property bool savedSquelchCleared: false
    // Why a level was not saved (one set outside the app), shown only while a
    // level is the setting; saveError is any other save failure.
    property string squelchSaveError: ""
    // Why the saved squelch could not be cleared or saved, shown whatever the
    // setting: the saved one still applies at the next start.
    property string squelchStoreError: ""
    // Navigation.announce()'s record of what was read out.
    property string announced: ""
    property int announcements: 0
    signal settingRequested(string key, var value)
    signal clearSavedSquelchRequested()
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
    // Issue #782: what makes a moving squelch reading worth hearing at once
    // (SpeechPacer): the gate opening or closing, or the setting changing.
    readonly property string squelchStep: [metrics.squelchGateOpen === true, squelchReading, squelchRowOverride].join("|")
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
     * Nudge the level, 5 dB a step, as the Radio sheet does (Util.squelchStep):
     * a whole dB, never against the step, down to the -120 dB floor and then
     * off (0); up from off is the floor, and -5 dB is the last threshold a step
     * up makes, because 0 is off.
     */
    function stepSquelch(delta) {
        if (!squelchSteppable || squelchHeld)
            return;
        var next = Util.squelchStep(squelchOff, squelchDb, delta, -120);
        if (isNaN(next))
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
        else if (sheet.bridge.setSquelchDb(db))
            settingRequested("squelch", {mode: "level", db: db, off: db >= 0});
    }
    /** Ask for the noise squelch with @p margin dB of quieting, on the row or the default. */
    function requestSquelchNoise(margin) {
        pendingSquelch = NaN;
        pendingSquelchMargin = margin;
        squelchTtl.restart();
        if (rowSquelch)
            rowEdit(sheet.bridge.scanRowEditSet, {"squelchMode": sheet.bridge.squelchModeNoise, "squelchMarginDb": margin});
        else if (sheet.bridge.setSquelchNoise(margin))
            settingRequested("squelch", {mode: "noise", margin: margin});
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
            // A row's own level is whole dB from -120 to -1 (0 is off there).
            requestSquelchDb(levelOff ? 0 : Math.min(-1, Math.max(-120, Math.round(db))));
            return;
        }
        // The default gets its level back exactly as stored. A refused request
        // leaves the engine's reading at once and saves nothing; what is saved
        // is the level asked for, once the decoder has answered with it, if
        // the app's controls could make it.
        pendingSquelchMargin = NaN;
        pendingSquelch = levelOff ? 0 : Math.min(db, -0.001);
        squelchTtl.restart();
        var asked = sheet.bridge.restoreSquelchLevel();
        if (asked)
            settingRequested("squelch", Object.assign({mode: "level", restore: true}, asked));
        else
            forgetRequests();
    }
    /**
     * Issue #899 (B11): ask for the saved squelch to go. Once it has, the
     * note that replaces the button is read out, and a keyboard focus the
     * button held goes to the squelch choice (or the buttons, with no choice).
     */
    function clearSavedSquelch(hadFocus) {
        clearSavedSquelchRequested();
        Navigation.announce(sheet, savedSquelchCleared ? squelchClearedNote : squelchSaveNote,
            savedSquelchCleared ? squelchClearedNote.text : squelchSaveNote.text);
        if (!hadFocus || !savedSquelchCleared)
            return;
        // A held setting selects no segment: the first enabled one takes it.
        if (!squelchModeControl.visible || !squelchModeControl.focusSegment())
            squelchDownButton.forceActiveFocus();
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
    ReadableText {
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
    ReadableText {
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
        id: squelchModeControl
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
            id: squelchValueText
            objectName: "squelchValue"
            width: parent.width - 116
            anchors.verticalCenter: parent.verticalCenter
            text: sheet.squelchReading
            // Read aloud as the terminal prints it, row note, default and status included.
            Accessible.role: Accessible.StaticText
            Accessible.name: squelchSpeech.spoken
            Accessible.ignored: !Navigation.allows(squelchValueText) || !visible
            // The auto and noise squelch readings move every tick (issue #782): heard at
            // once when the gate opens or closes or the setting changes, else at their pace.
            SpeechPacer {
                id: squelchSpeech
                shown: Navigation.allows(squelchValueText) && squelchValueText.visible
                step: sheet.squelchStep
                read: function () {
                    return sheet.squelchRowOverride ? metrics.squelchReadout
                        : (sheet.squelchStatus !== "" ? sheet.squelchReading + " (" + sheet.squelchStatus + ")"
                            : sheet.squelchReading);
                }
            }
            color: Theme.textPrimary
            font.family: Theme.mono
            font.pixelSize: Theme.fontSize(14)
        }
        // In Noise the buttons step the margin, 1 dB at a time.
        OutlineButton {
            id: squelchDownButton
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
    LiveText {
        objectName: "squelchStatus"
        step: sheet.squelchStep
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
    ReadableText {
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
    // Issue #899 (B7, B15): where a default-scope change goes, or why it did not.
    ReadableText {
        id: squelchSaveNote
        objectName: "squelchSaveNote"
        // A saved squelch that could not be cleared or saved whatever the
        // setting; the unsaved-level reason only while a level is the setting.
        readonly property string problem: sheet.squelchStoreError.length > 0 ? sheet.squelchStoreError
            : ((sheet.squelchSaveError.length > 0 && !sheet.squelchNoise && !sheet.squelchAuto)
                ? sheet.squelchSaveError : sheet.saveError)
        width: parent.width
        visible: !sheet.rowSquelch && (sheet.savesToSystem || problem.length > 0)
        text: problem.length > 0 ? problem : qsTr("Squelch changes here are saved to this system.")
        wrapMode: Text.Wrap
        color: problem.length > 0 ? Theme.alert : Theme.textSecondary
        font.pixelSize: Theme.fontSize(13)
    }
    // Issue #899 (B11): the saved system's own squelch out, from the next
    // start. An audio input takes no app default, so the button names what it
    // does; when it applies is under it, as on the Radio sheet.
    Column {
        objectName: "squelchClearRow"
        visible: sheet.savesToSystem && !sheet.rowSquelch && sheet.savedSquelch.length > 0
        width: parent.width
        spacing: 4
        OutlineButton {
            id: squelchClearButton
            objectName: "squelchClearSaved"
            text: qsTr("Remove saved squelch")
            accessibleName: qsTr("Remove Saved Squelch From This System")
            onClicked: sheet.clearSavedSquelch(activeFocus)
        }
        ReadableText {
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("Applies from the next start.")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSize(13)
        }
    }
    ReadableText {
        id: squelchClearedNote
        objectName: "squelchClearedNote"
        width: parent.width
        visible: sheet.savesToSystem && !sheet.rowSquelch && sheet.savedSquelchCleared
        text: qsTr("Saved squelch cleared for the next start.")
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
