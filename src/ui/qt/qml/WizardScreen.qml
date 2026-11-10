// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Window
import "Util.js" as Util

// The add-system wizard: source → tune → name. Three screens instead of one form,
// each asking one question, with everything advanced folded away.
Item {
    id: wizard

    signal closed
    property var importedSite: null
    property var importedSites: []
    property string retainedKeySourceUid: ""
    signal saved(int row)
    // Asks Main.qml to push the RadioReference screen over this one; the result
    // comes back through applyRadioReference().
    signal openRadioReference

    property string initialDraft: ""
    function revealFocus() {
        var window = wizard.Window.window;
        if (window)
            Theme.revealFocus(wizardScroll, stepColumn, window.activeFocusItem);
    }
    Connections {
        target: wizard.Window.window
        function onActiveFocusItemChanged() {
            if (wizard.visible)
                Qt.callLater(wizard.revealFocus);
        }
    }
    function draftFingerprint() {
        // The Airspy fields' typing counts before it commits (they commit on
        // editingFinished, and an emptied gain index never does).
        return JSON.stringify([sourceType, airspySettings, wizardAirspy.pendingText(), hostText, portText, fileText, freqText, decodeFlag, trunking, gainText, ppmText, hangtimeText, bwText, biasTee, extraText, nameText, chanCsvPath, groupCsvPath, keyCsvPath, keyCsvHex, encKeyType, encForceKey, p25BandplanCsvPath, srcCsvPath, decryptionProfileUid]);
    }
    // Whether leaving now would lose something typed or picked.
    function hasUnsavedDraft() {
        return initialDraft !== draftFingerprint() || encKeyValue.length > 0;
    }
    function requestBack() {
        Navigation.clearInput(wizard.Window.window);
        if (step > 0) {
            step--;
            return;
        }
        if (hasUnsavedDraft())
            discardDialog.ask(function () {
                wizard.closed();
            });
        else
            closed();
    }
    DiscardDialog {
        id: discardDialog
        objectName: "wizardDiscardDialog"
    }
    onStepChanged: Navigation.clearInput(wizard.Window.window)
    property int step: 0
    // -1 appends a new system; >= 0 edits in place.
    property int editRow: -1
    property string decryptionProfileUid: ""

    // Step 1 state
    property var airspySettings: ({})
    property string sourceType: "usb"
    // A replay copy belongs to the File source: leaving it drops the copy, so
    // nothing hidden keeps Continue waiting.
    onSourceTypeChanged: {
        Navigation.clearInput(wizard.Window.window);
        if (sourceType !== "file")
            dropReplay();
    }
    property alias hostText: hostField.text
    property alias portText: portField.text
    property string fileText: ""

    // Step 2 state
    property alias freqText: freqField.text
    property string decodeFlag: ""
    property bool trunking: false
    // Whether the trunking question has been answered — by the user's own
    // toggle, a RadioReference import, or a saved system being edited. Until
    // then the decode chip pick suggests it (see pickDecodeFlag), and an
    // explicit answer must survive every later chip change.
    property bool trunkingAnswered: false
    property bool advancedOpen: false
    property alias gainText: gainField.text
    property alias ppmText: ppmField.text
    property alias hangtimeText: hangtimeField.text
    property alias bwText: bwField.text
    // Tri-state: -1 follows the app-wide Settings pref, 0 forces off, 1 forces
    // on. An explicit Off must survive a global On — it means this dongle or
    // antenna must not be fed the tee's 4.5 V.
    property int biasTee: -1
    property alias extraText: extraField.text
    // Imported trunking-data files; stored library paths, empty = none.
    property string chanCsvPath: ""
    property string groupCsvPath: ""
    // Whether each of the two came from a RadioReference import (this one's,
    // or one the edited system was saved with) rather than the user's own
    // pick: the next import replaces the first kind, and keeps the second
    // only for the system being edited. See applyRadioReference().
    property bool chanCsvGenerated: false
    property bool groupCsvGenerated: false
    property string keyCsvPath: ""
    property bool keyCsvHex: false
    // WP-D2: the editor is the sole owner of the draft key text.
    property alias encKeyType: encryptionEditor.keyType
    property alias encKeyValue: encryptionEditor.keyValue
    property alias encForceKey: encryptionEditor.forceMode
    readonly property bool encryptionValid: encryptionEditor.valid
    property string p25BandplanCsvPath: ""
    property string srcCsvPath: ""
    // Which field the picker sheet serves: "chan"/"group"/"keys"/
    // "p25Bandplan"/"src" are the trunking-data fields ("source", the resting
    // value, opens no sheet; the step-1 Browse never sets it). An import
    // carries its own field in its context (issue #829), so changing this
    // never redirects a pick still out.
    property string pickerTarget: "source"
    // Dec/hex choice for a new key-file import from the picker sheet.
    property bool pickerKeyHex: false
    // Outcome of the last trunking-data import, shown under the picker rows.
    // Without it a file that could not be read, or that parsed to nothing,
    // leaves the row reading "None" with no explanation.
    property string csvNotice: ""
    property bool csvNoticeIsProblem: false
    // The last problem read out (an import's or the replay file's) and how
    // many have been, for tests.
    property string announced: ""
    property int announcements: 0
    // Issue #829: one generation per trunking-data field, bumped by every
    // choice for it (a library file, None, an import, RadioReference). A pick
    // that comes back after a newer choice for its field is dropped.
    property var csvGenerations: ({})
    // #832: which opening of the wizard an import belongs to. Every open and
    // every close takes the next, so the answer to an import started before
    // is dropped rather than written into another system.
    property int generation: 0
    // The replay pick waiting for its answer (0 when none), and why the last
    // Browse left the replay file as it was (issue #829 B8: the copy failed,
    // or no picker could open), shown under the Replay file row. Only a
    // provider's document is copied: a local file is answered at once, on the
    // next turn, so the row shows a copy only while the store says one runs,
    // and never flashes for a desktop pick.
    property int replayRequest: 0
    property string replayNotice: ""
    readonly property bool replayPending: replayRequest > 0
    readonly property bool replayCopying: replayPending && replayFiles.copying

    // Step 3 state
    property alias nameText: nameField.text

    readonly property bool radioSource: sourceType === "usb" || sourceType === "airspy" || sourceType === "rtltcp"
    // A file source replaying a symbol capture, which carries TETRA's symbols as a radio's I/Q does.
    readonly property bool symbolSource: sourceType === "file" && Util.isSymbolCapture(fileText)

    // Per-source conventions, not one shared guess: 1234 is rtl_tcp's port, but
    // GQRX/SDR++ audio streams (docs/network-audio.md) use 7355 and a TCP audio
    // source is usually a player on this device. Accepting the wrong prefill
    // leaves the session silent with no error.
    ModalSheet {
        id: replayDetails
        property string details: ""
        accessibleName: qsTr("Replay file details")
        ReadableText {
            width: parent.width
            text: replayDetails.details
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSize(15)
        }
        OutlineButton {
            width: parent.width
            text: qsTr("Close")
            onClicked: replayDetails.visible = false
        }
    }

    function defaultHostFor(type) {
        return type === "tcp" ? "127.0.0.1" : "192.168.1.10";
    }

    function defaultPortFor(type) {
        return (type === "udp" || type === "tcp") ? "7355" : "1234";
    }

    // Swap the prefill when the source type changes, but never text the user
    // already edited away from the previous type's default.
    function applySourceDefaults(prevType) {
        if (hostField.text === defaultHostFor(prevType))
            hostField.text = defaultHostFor(sourceType);
        if (portField.text === defaultPortFor(prevType))
            portField.text = defaultPortFor(sourceType);
    }

    function resetImport() {
        importedSite = null;
        importedSites = [];
        chanCsvGenerated = false;
        groupCsvGenerated = false;
    }

    // Whether a library file is one a RadioReference import generated.
    function generatedByRadioReference(path) {
        var row = path.length > 0 ? importedFiles.rowForPath(path) : -1;
        return row >= 0 && importedFiles.get(row).origin === "radioreference";
    }

    // Issues #829, #832: a pick or copy still out belongs to the session it
    // was asked in; closing the wizard by any route (Save clears wizardOpen in
    // Main directly) or reopening it for another system ends it. A copy in
    // flight is cancelled, and whatever it still answers belongs to an earlier
    // generation, so it is dropped.
    onVisibleChanged: {
        if (!visible)
            stopImports();
    }
    function stopImports() {
        generation++;
        csvImport.cancel();
        sourcePicker.cancel();
        dropReplay();
    }
    // Cancel the replay copy, if any, and forget its answer and its notice.
    function dropReplay() {
        if (replayPending)
            replayFiles.cancelReplayImport();
        replayRequest = 0;
        replayNotice = "";
    }

    function csvGeneration(target) {
        return csvGenerations[target] || 0;
    }
    function bumpCsvGeneration(target) {
        var next = Object.assign({}, csvGenerations);
        next[target] = csvGeneration(target) + 1;
        csvGenerations = next;
    }
    // Whether an import asked for with @p context may still fill its field:
    // this opening of the wizard, and no newer choice for the field since.
    function csvPickCurrent(context) {
        return context.generation === generation && !!context.target && context.fieldGeneration === csvGeneration(context.target);
    }
    // What a CSV import must remember from the moment it is asked for: the
    // field it fills, the key format it was picked as, this opening of the
    // wizard and the field's generation, so nothing changed afterwards
    // (another field's sheet, Browse, the key format) can redirect it.
    function csvContext() {
        var target = pickerTarget;
        return {
            target: target,
            keyFormat: target === "keys" ? (pickerKeyHex ? "hex" : "dec") : "",
            generation: generation,
            fieldGeneration: csvGeneration(target)
        };
    }
    // "Import new file" on the picker sheet.
    function importCsvFile() {
        var context = csvContext();
        var type = context.target === "keys" ? (context.keyFormat === "hex" ? "keysHex" : "keysDec") : context.target;
        csvImport.pick(type, context);
    }
    function showCsvProblem(text) {
        csvNotice = text;
        csvNoticeIsProblem = true;
        announceProblem(csvNoticeLine, text);
    }
    // Read out once per failure, a repeat included, while @p line is reachable.
    function announceProblem(line, text) {
        Navigation.announce(wizard, line, text);
    }

    // Browse for the replay file. A provider's document is copied into the
    // app's own storage (replayFiles), and so is a local file where the host
    // has DSD-neo's file browser (a drive can go away); on the desktop a
    // local file is used where it is. Until a copy lands the previous file
    // stays.
    function browseReplayFile() {
        replayNotice = "";
        sourcePicker.open();
    }
    // The host's name is decoded already and goes on as given; a pick
    // with none is named from its reference, decoded once (issue #830).
    function replayFilePicked(reference, displayName) {
        beginReplay(reference, displayName.length > 0 ? displayName : Util.referenceName(reference));
    }
    function showReplayProblem(text) {
        replayNotice = text;
        announceProblem(replayNoticeLine, text);
    }
    // Copy a picked replay where the decoder can read it; the Replay file row
    // shows the copy until replayFiles answers. The earlier file stays until
    // then, and stays if the copy fails.
    function beginReplay(reference, hint) {
        replayNotice = "";
        replayRequest = replayFiles.beginReplayImport(reference, hint);
    }
    // The store answers a cancel at once (on the next turn) and stops
    // copying, so the row leaves with no "Cancelling…" step.
    function cancelReplay() {
        if (replayPending)
            replayFiles.cancelReplayImport();
    }
    Connections {
        target: replayFiles
        function onReplayImportFinished(id, result) {
            if (wizard.replayRequest === 0 || id !== wizard.replayRequest)
                return;
            wizard.replayRequest = 0;
            if (result.ok) {
                wizard.fileText = result.path;
                wizard.replayNotice = "";
            } else {
                // A cancel the user asked for reads as nothing.
                wizard.showReplayProblem(Util.importMessage(result, "replay"));
            }
        }
        // The startup migration moved a cached replay to its own copy while
        // this draft was open. A draft still naming the cached file follows
        // it, and the draft's clean or changed state stays as it was.
        function onReplaysMigrated() {
            var uid = wizard.editRow >= 0 ? savedSystems.get(wizard.editRow).uid : "";
            var moved = replayFiles.migratedPath(uid, wizard.fileText);
            if (wizard.fileText.length === 0 || moved === wizard.fileText)
                return;
            wizard.initialDraft = wizard.initialDraft.split(JSON.stringify(wizard.fileText)).join(JSON.stringify(moved));
            wizard.fileText = moved;
        }
    }

    function openForAdd(preferNetwork) {
        stopImports();
        retainedKeySourceUid = "";
        resetImport();
        decryptionProfileUid = "";
        editRow = -1;
        step = 0;
        sourceType = preferNetwork ? "rtltcp" : "usb";
        hostField.text = defaultHostFor(sourceType);
        portField.text = defaultPortFor(sourceType);
        wizard.fileText = "";
        freqField.text = "851.375";
        decodeFlag = "";
        trunkingAnswered = false;
        // After the field and the chip, never before: the prefill is an 800 MHz
        // control channel, so this is what keeps the shipped defaults decoding.
        refreshTrunkingSuggestion();
        advancedOpen = false;
        gainField.text = "";
        ppmField.text = "";
        hangtimeField.text = "";
        bwField.text = "";
        airspySettings = ({});
        biasTee = -1;
        extraField.text = "";
        nameField.text = "";
        chanCsvPath = "";
        groupCsvPath = "";
        keyCsvPath = "";
        keyCsvHex = false;
        encryptionEditor.reset();
        p25BandplanCsvPath = "";
        srcCsvPath = "";
        csvNotice = "";
        csvNoticeIsProblem = false;
        wizardAirspy.resetText();
        initialDraft = draftFingerprint();
    }

    /**
     * Open on something found while exploring, with the source and frequency
     * already answered.
     *
     * Opens at step 2 rather than step 3: the source is settled, but what to
     * decode and whether to follow calls are real questions about the system just
     * found, and answering them for the user would produce a card that does not
     * work. @a sys supplies the source; it is the explore session's own map.
     * A Signal search @a detection (SignalSearchModel::systemForHit()) also
     * supplies the decoder, gain, bandwidth and receiver settings it captured.
     */
    function openForFound(sys, freqMhz, detection) {
        stopImports();
        resetImport();
        retainedKeySourceUid = "";
        decryptionProfileUid = "";
        editRow = -1;
        step = 1;
        sourceType = sys && sys.sourceType === "airspy" ? "airspy" : (sys && sys.sourceType === "rtltcp") ? "rtltcp" : "usb";
        hostField.text = sys && sys.host ? sys.host : defaultHostFor(sourceType);
        portField.text = sys && sys.port > 0 ? String(sys.port) : defaultPortFor(sourceType);
        wizard.fileText = "";
        freqField.text = freqMhz;
        decodeFlag = "";
        trunkingAnswered = false;
        // The frequency a user "found" while exploring 700/800 is most often a
        // constant-carrier control channel — that is what stands out on a
        // waterfall — so the same suggestion applies, and more strongly.
        refreshTrunkingSuggestion();
        advancedOpen = false;
        gainField.text = "";
        ppmField.text = "";
        hangtimeField.text = "";
        bwField.text = "";
        airspySettings = ({});
        biasTee = -1;
        extraField.text = "";
        nameField.text = "";
        chanCsvPath = "";
        groupCsvPath = "";
        keyCsvPath = "";
        keyCsvHex = false;
        encryptionEditor.reset();
        p25BandplanCsvPath = "";
        srcCsvPath = "";
        csvNotice = "";
        csvNoticeIsProblem = false;
        if (detection) {
            decodeFlag = detection.decodeFlag || "";
            trunking = false;
            trunkingAnswered = true;
            // A whole-dB override (0 is the tuner's AGC); -1 for an Airspy,
            // whose gains are in its own settings, leaves the field unset.
            gainField.text = detection.gainDb >= 0 ? String(detection.gainDb) : "";
            bwField.text = String(detection.bandwidthKhz > 0 ? detection.bandwidthKhz : 48);
            // The receiver the detection was made with, not the app defaults
            // reset above: an Airspy's own settings, or an RTL's PPM and its
            // explicit bias tee. A field the source does not use stays unset.
            airspySettings = detection.airspy || ({});
            ppmField.text = detection.ppm !== undefined ? String(detection.ppm) : "";
            biasTee = detection.biasTee !== undefined ? detection.biasTee : -1;
            extraField.text = detection.extraArgs || "";
            csvNotice = detection.searchUnresolved ? qsTr("Digital identification is unresolved. NFM monitoring does not confirm an analog transmission.") : "";
        }
        wizardAirspy.resetText();
        initialDraft = draftFingerprint();
    }

    function openForEdit(row) {
        stopImports();
        resetImport();
        var sys = savedSystems.get(row);
        retainedKeySourceUid = sys.uid || "";
        decryptionProfileUid = sys.decryptionProfileUid || "";
        editRow = row;
        step = 0;
        sourceType = sys.sourceType;
        hostField.text = sys.host;
        portField.text = sys.port > 0 ? String(sys.port) : "";
        wizard.fileText = sys.filePath;
        freqField.text = sys.freqMhz;
        decodeFlag = sys.decodeFlag;
        // The saved system already answered the question; a chip tap during
        // the edit must not silently flip what the card was doing yesterday.
        answerTrunking(sys.trunking);
        advancedOpen = false;
        airspySettings = sys.airspy || ({});
        gainField.text = sys.gainDb >= 0 ? String(sys.gainDb) : "";
        ppmField.text = sys.ppm;
        hangtimeField.text = sys.hangtime || "";
        bwField.text = sys.bandwidthKhz > 0 ? String(sys.bandwidthKhz) : "";
        biasTee = sys.biasTee;
        extraField.text = sys.extraArgs;
        nameField.text = sys.name;
        chanCsvPath = sys.chanCsvPath;
        groupCsvPath = sys.groupCsvPath;
        chanCsvGenerated = generatedByRadioReference(chanCsvPath);
        groupCsvGenerated = generatedByRadioReference(groupCsvPath);
        keyCsvPath = sys.keyCsvPath;
        keyCsvHex = sys.keyCsvHex;
        encryptionEditor.reset();
        encKeyType = sys.encKeyType || "";
        encryptionEditor.configuredKeyType = encKeyType;
        encryptionEditor.keyConfigured = sys.encKeyConfigured === true;
        encForceKey = sys.encForceKey || 0;
        p25BandplanCsvPath = sys.p25BandplanCsvPath;
        srcCsvPath = sys.srcCsvPath;
        csvNotice = "";
        csvNoticeIsProblem = false;
        wizardAirspy.resetText();
        initialDraft = draftFingerprint();
    }

    // Display line for a picker row: the file's name, or "None".
    function csvLabel(path) {
        if (path.length === 0)
            return qsTr("None");
        var row = importedFiles.rowForPath(path);
        return row >= 0 ? importedFiles.get(row).name : path.substring(path.lastIndexOf('/') + 1);
    }

    /**
     * Take a decode chip pick, suggesting the trunking answer with it.
     *
     * A new system starts with trunking off, and most users cannot answer
     * "is it trunked?" cold — but an explicit P25 pick almost certainly means
     * a trunked system, so the switch follows the chip until the user (or a
     * RadioReference import, or an edit's saved state) answers it for real.
     */
    function pickDecodeFlag(flag) {
        decodeFlag = flag;
        refreshTrunkingSuggestion();
    }

    /**
     * Whether the source can run a decode flag: AM needs a radio (issue #524),
     * TETRA a radio or a symbol capture file; network and other file audio
     * arrives already demodulated. Every radio runs every flag, an Airspy
     * TETRA too (issue #806); the bandwidth gates are tetraRateError() and
     * amBandwidthError().
     */
    function decodeFlagRunsHere(flag) {
        if (radioSource)
            return true;
        if (Util.decodeFlagNeedsRadio(flag))
            return false;
        return !Util.decodeFlagNeedsIqOrSymbols(flag) || symbolSource;
    }

    /**
     * A source change away from the radio drops a chip the new source cannot
     * run back to Auto, rather than saving a system the engine would refuse to
     * start. Its chip is greyed out for the new source.
     */
    function dropRadioOnlyDecodeFlag() {
        if (!decodeFlagRunsHere(decodeFlag))
            pickDecodeFlag("");
    }

    // The bandwidth the user asked for, or -1 for an empty field.
    function bandwidthRequestKhz() {
        return bwText.length > 0 ? intOr(bwText, -1) : -1;
    }

    /**
     * Why AM cannot run at the bandwidth this radio would start with (issue
     * #524), or empty: the engine refuses an AM channel the DSP bandwidth
     * cannot filter, so the wizard says so before saving the system. An empty
     * bandwidth field follows the app-wide default.
     */
    function amBandwidthError() {
        if (!radioSource || !Util.decodeFlagNeedsRadio(decodeFlag))
            return "";
        return sessionArgs.amBandwidthError(sourceType, bandwidthRequestKhz());
    }

    /**
     * Why TETRA cannot run on this radio (issue #803), or empty: a radio
     * decodes it only at 72 kHz, which an empty bandwidth field gives it. The
     * same check refuses the session at start, so the wizard says so before
     * saving the system.
     */
    function tetraRateError() {
        if (!radioSource)
            return "";
        return sessionArgs.tetraRateError(decodeFlag, sourceType, bandwidthRequestKhz());
    }

    /**
     * Re-derive the unanswered trunking switch from the chip and the frequency.
     *
     * Called on every chip pick and on every frequency edit, not just the pick:
     * Auto is the default chip and carries no system type, so a user who accepts
     * the prefills taps no chip at all and the suggestion would otherwise never
     * run. An explicit answer — the toggle, an import, an edit's saved state —
     * stops this for good, which is what trunkingAnswered is for.
     */
    function refreshTrunkingSuggestion() {
        if (trunkingAnswered)
            return;
        // parseFloat("") and a half-typed "8." are NaN, which suggestsTrunking()
        // reads as "no band, no suggestion" rather than as 0 Hz.
        trunking = Util.suggestsTrunking(decodeFlag, parseFloat(freqField.text) * 1.0e6);
    }

    /** The user's own toggle: an answer, which no later chip pick may undo. */
    function answerTrunking(state) {
        trunking = state;
        trunkingAnswered = true;
    }

    // parseInt alone lets a hardware-keyboard "abc" become NaN, which QVariant
    // then reads as 0 — and a gain of 0 is a meaningful override, not "unset".
    // NaN must collapse to the field's explicit "no override" value instead.
    function intOr(text, fallback) {
        var v = parseInt(text, 10);
        return isNaN(v) ? fallback : v;
    }

    function portValid() {
        var p = Number(portText);
        return /^[0-9]+$/.test(portText) && p >= 1 && p <= 65535;
    }

    function hangtimeValid() {
        return hangtimeText.length === 0 || (/^\d{1,2}(\.\d)?$/.test(hangtimeText) && Number(hangtimeText) <= 30);
    }

    function stepValid() {
        if (step === 0) {
            if (sourceType === "rtltcp" || sourceType === "tcp")
                return hostText.length > 0 && portValid();
            if (sourceType === "udp")
                return portValid();
            if (sourceType === "file")
                return fileText.length > 0;
            return true;
        }
        // A system edited or imported onto a non-radio source may still carry
        // AM, which only a radio can run, or TETRA, which a radio or a symbol
        // capture can: not until another chip is picked. Nor on a radio whose
        // bandwidth cannot filter the AM channel, or that cannot decode TETRA.
        if (step === 1)
            return hangtimeValid() && encryptionValid && (!radioSource || sessionArgs.freqValid(freqText))
                && decodeFlagRunsHere(decodeFlag) && amBandwidthError().length === 0 && tetraRateError().length === 0;
        return hangtimeValid() && encryptionValid && nameText.trim().length > 0;
    }

    function commit() {
        if (!hangtimeValid() || !encryptionValid || replayPending)
            return;
        var sys = {
            name: nameText.trim(),
            sourceType: sourceType,
            host: hostText,
            port: portText.length > 0 ? intOr(portText, 0) : 0,
            freqMhz: freqText,
            decodeFlag: decodeFlag,
            trunking: trunking,
            airspy: wizardAirspy.pendingSettings(),
            gainDb: gainText.length > 0 ? intOr(gainText, -1) : -1,
            ppm: ppmText,
            hangtime: hangtimeText,
            bandwidthKhz: bwText.length > 0 ? intOr(bwText, -1) : -1,
            biasTee: biasTee,
            extraArgs: extraText.trim(),
            filePath: fileText,
            decryptionProfileUid: decryptionProfileUid,
            chanCsvPath: chanCsvPath,
            groupCsvPath: groupCsvPath,
            keyCsvPath: keyCsvPath,
            keyCsvHex: keyCsvHex,
            encKeyType: encryptionEditor.clearingKey ? "" : encKeyType,
            encForceKey: encForceKey,
            p25BandplanCsvPath: p25BandplanCsvPath,
            srcCsvPath: srcCsvPath
        };
        if (!encryptionEditor.keepingKey)
            sys.encKeyValue = encryptionEditor.clearingKey ? "" : encKeyValue;
        if (importedSites.length > 0) {
            // Every site receives the source/tuner answers; its protocol and CSVs stay site-specific.
            for (var i = 0; i < importedSites.length; ++i) {
                var site = i === 0 ? Object.assign({}, importedSites[i], sys) : Object.assign({}, sys, importedSites[i]);
                if (site.freqMhz !== importedSites[i].freqMhz || site.decodeFlag !== importedSites[i].decodeFlag || site.chanCsvPath !== (importedSites[i].chanCsvPath || "") || site.groupCsvPath !== (importedSites[i].groupCsvPath || "")) {
                    site.rrSid = 0;
                    site.rrSiteId = 0;
                }
                if (encryptionEditor.keepingKey) {
                    if (!savedSystems.addWithKeyFrom(retainedKeySourceUid, site)) {
                        csvNotice = qsTr("The retained key is unavailable. Choose Replace or Clear before saving.");
                        csvNoticeIsProblem = true;
                        return;
                    }
                } else {
                    if (!savedSystems.add(site)) {
                        csvNotice = qsTr("Could not save this imported system.");
                        csvNoticeIsProblem = true;
                        return;
                    }
                }
            }
            wizard.saved(savedSystems.count - importedSites.length);
            return;
        }
        if (importedSite && importedSite.rrSiteId > 0 && sys.freqMhz === importedSite.freqMhz && sys.decodeFlag === importedSite.decodeFlag && sys.chanCsvPath === (importedSite.chanCsvPath || "") && sys.groupCsvPath === (importedSite.groupCsvPath || "")) {
            for (var key of ["rrSid", "rrSiteId", "siteName", "siteLat", "siteLon", "hasSitePos"])
                sys[key] = importedSite[key];
        }
        if (editRow >= 0) {
            if (!savedSystems.update(editRow, sys)) {
                csvNotice = qsTr("Could not save the system.");
                csvNoticeIsProblem = true;
                return;
            }
            wizard.saved(editRow);
        } else {
            if (!savedSystems.add(sys)) {
                csvNotice = qsTr("Could not save the system.");
                csvNoticeIsProblem = true;
                return;
            }
            wizard.saved(savedSystems.count - 1);
        }
    }

    // One trunking-data picker row: the disclosure shape with the picker sheet
    // behind it and the library label as its helper line. The helper reads as an
    // answer once a file is chosen, which is what the brighter colour says.
    component CsvPickerRow: DisclosureRow {
        id: pickerRow

        property string target: ""
        property string path: ""

        subtitle: wizard.csvLabel(pickerRow.path)
        subtitleColor: pickerRow.path.length > 0 ? Theme.textSecondary : Theme.textSubdued
        onTapped: {
            wizard.pickerTarget = pickerRow.target;
            wizard.pickerKeyHex = wizard.pickerTarget === "keys" ? wizard.keyCsvHex : false;
            csvSheet.openedKeyHex = wizard.pickerKeyHex;
            csvSheet.visible = true;
        }
    }

    // Assign an imported/picked library path to the field the picker serves.
    function assignCsvPath(target, path, hex) {
        csvNotice = "";
        csvNoticeIsProblem = false;
        bumpCsvGeneration(target);
        if (target === "chan") {
            chanCsvPath = path;
            chanCsvGenerated = false;
        } else if (target === "group") {
            groupCsvPath = path;
            groupCsvGenerated = false;
        } else if (target === "keys") {
            keyCsvPath = path;
            keyCsvHex = hex;
        } else if (target === "p25Bandplan") {
            p25BandplanCsvPath = path;
        } else if (target === "src") {
            srcCsvPath = path;
        }
    }

    /**
     * Fill the tune answers from a finished RadioReference import.
     *
     * The wizard stays the single writer of the saved system, so the user's
     * source, gain and ppm answers survive. @a result is performImport()'s map,
     * or a per-site batch's {rows}; a file the import did not generate is
     * absent from it, which is a valid outcome: a single conventional repeater
     * has no channel map, and a TETRA system listing no talkgroups has no file
     * at all.
     *
     * The import answers both files (issue #803). One it supplies replaces
     * whatever was there. One it does not clears a file an earlier import
     * generated, which belongs to that system: kept, it would tie this system
     * to another one's talkgroups, and drop the import's RadioReference
     * identity at save. A file the user picked is kept only while editing the
     * same saved system — clearing it would silently strip a hand-picked -C
     * from a system they were only editing. A per-site batch saves new
     * systems, so each site keeps only its own files.
     */
    function applyRadioReference(result) {
        importedSites = (result.rows || []).map(function (row) {
            return Object.assign({
                chanCsvPath: "",
                groupCsvPath: ""
            }, row);
        });
        if (importedSites.length > 0) {
            editRow = -1;
            result = importedSites[0];
        }
        importedSite = result;

        var keepPicked = editRow >= 0;
        var chan = result.chanCsvPath || "";
        if (chan.length > 0 || chanCsvGenerated || !keepPicked) {
            bumpCsvGeneration("chan");
            chanCsvPath = chan;
            chanCsvGenerated = chan.length > 0;
        }
        var group = result.groupCsvPath || "";
        if (group.length > 0 || groupCsvGenerated || !keepPicked) {
            bumpCsvGeneration("group");
            groupCsvPath = group;
            groupCsvGenerated = group.length > 0;
        }
        if (result.freqMhz && result.freqMhz.length > 0)
            freqField.text = result.freqMhz;
        wizard.decodeFlag = result.decodeFlag;
        // A bandwidth kept from an edited system that cannot decode TETRA:
        // an empty field runs it at the 72 kHz it needs.
        if (bwText.length > 0 && tetraRateError().length > 0)
            bwText = "";
        // The database's answer, and an answer: a chip tap after the import
        // must not second-guess what the record says the system is.
        wizard.answerTrunking(result.trunking);
        // Only when the wizard has no name yet: an edit already has one the user
        // chose, and RadioReference's is a database title, not their label.
        if (nameField.text.trim().length === 0 && result.name)
            nameField.text = result.name;
        wizard.csvNotice = qsTr("Imported from RadioReference");
        wizard.csvNoticeIsProblem = false;
        // Land on the tune step: the frequency, decode flag and files are all
        // answered now, and step 2 is where they are shown.
        if (wizard.step < 1)
            wizard.step = 1;
    }

    // The replay file's Browse (issue #829): the host's own picker where it
    // has one, or DSD-neo's file browser (issue #830). Backing out changes
    // nothing.
    DocumentPicker {
        id: sourcePicker
        objectName: "wizardReplayPicker"
        mode: "open"
        browseKind: "replay"
        title: qsTr("Choose a replay file")
        onAccepted: function (reference, displayName) {
            wizard.replayFilePicked(reference, displayName);
        }
        onUnavailable: function (reason, message) {
            wizard.showReplayProblem(message);
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    CsvImportFlow {
        id: csvImport
        objectName: "wizardCsvImport"
        overlayParent: wizard
        wanted: function (context) {
            return wizard.csvPickCurrent(context);
        }
        // The field and the key format come from the import's own context,
        // never pickerTarget, which the next sheet or Browse rewrites while a
        // pick is out or a copy runs.
        onFinished: function (result, context) {
            if (context.generation !== wizard.generation)
                return;
            if (result.error === "cancelled") {
                // The user backed out of this field's pick; a pick dropped
                // for a newer choice leaves that choice's notice alone.
                if (wizard.csvPickCurrent(context)) {
                    wizard.csvNotice = "";
                    wizard.csvNoticeIsProblem = false;
                }
                return;
            }
            if (!result.ok) {
                wizard.showCsvProblem(Util.importMessage(result, "csv"));
                return;
            }
            wizard.assignCsvPath(context.target, result.path, context.keyFormat === "hex");
            if (result.error === "empty")
                wizard.showCsvProblem(qsTr("%1 has no usable rows — check the file format.").arg(result.name));
        }
    }

    // Header: back chevron, title, segmented progress. The title stops short
    // of the progress and wraps at large text on a narrow phone; the header
    // grows to hold it, and the chevron and the progress stay level with its
    // first line (#830).
    Item {
        id: header

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.screenPadding
        height: Math.max(46, title.height)
        readonly property real firstLineCentre: title.lineCount > 1 ? title.height / title.lineCount / 2 : height / 2

        IconButton {
            id: back
            icon: "back"
            anchors.left: parent.left
            y: header.firstLineCentre - height / 2
            onClicked: {
                wizard.requestBack();
            }
        }

        ReadableText {
            id: title
            objectName: "wizardTitle"
            heading: true
            x: back.width + 14
            y: lineCount > 1 ? 0 : (header.height - height) / 2
            width: progress.x - 12 - x
            text: wizard.editRow >= 0 ? qsTr("Edit system") : qsTr("Add system")
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(22)
            font.weight: Font.Bold
            font.letterSpacing: -0.22
            color: Theme.textPrimary
            wrapMode: Text.Wrap
        }

        Row {
            id: progress
            objectName: "wizardProgress"
            anchors.right: parent.right
            y: header.firstLineCentre - height / 2
            spacing: 6

            Repeater {
                model: 3

                Rectangle {
                    required property int index

                    width: 20
                    height: 4
                    radius: 2
                    color: index <= wizard.step ? Theme.cyan : Theme.controlBorder
                }
            }
        }
    }

    MicroLabel {
        id: stepLabel
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.topMargin: 6
        anchors.leftMargin: Theme.screenPadding
        text: wizard.step === 0 ? qsTr("Step 1 of 3 · Source") : wizard.step === 1 ? qsTr("Step 2 of 3 · Tune") : qsTr("Step 3 of 3 · Name")
    }

    PlexFlickable {
        id: wizardScroll
        onHeightChanged: Qt.callLater(wizard.revealFocus)
        anchors.top: stepLabel.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: continueButton.top
        anchors.topMargin: 14
        anchors.bottomMargin: 14
        contentHeight: stepColumn.height
        clip: true

        Column {
            id: stepColumn

            x: (parent.width - width) / 2
            width: Math.min(Theme.formWidth, parent.width - 2 * Theme.screenPadding)
            spacing: Theme.gap

            // ---- Step 1: source ----
            Column {
                width: parent.width
                visible: wizard.step === 0
                spacing: Theme.gap

                ReadableText {
                    width: parent.width
                    text: qsTr("Where does the signal come from?")
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(15)
                    font.weight: Font.DemiBold
                    color: Theme.textPrimary
                    wrapMode: Text.Wrap
                }

                Flow {
                    width: parent.width
                    spacing: 10

                    Repeater {
                        model: [
                            {
                                label: qsTr("USB dongle"),
                                key: "usb"
                            },
                            { label: qsTr("Airspy R2 / Mini"), key: "airspy" },
                            {
                                label: qsTr("RTL-TCP"),
                                key: "rtltcp"
                            },
                            {
                                label: qsTr("UDP audio"),
                                key: "udp"
                            },
                            {
                                label: qsTr("TCP audio"),
                                key: "tcp"
                            },
                            {
                                label: qsTr("File"),
                                key: "file"
                            }
                        ]

                        DecodeChip {
                            required property var modelData

                            objectName: "wizardSource_" + modelData.key
                            text: modelData.label
                            selected: wizard.sourceType === modelData.key
                            onClicked: {
                                var prev = wizard.sourceType;
                                wizard.sourceType = modelData.key;
                                if (prev !== modelData.key) {
                                    wizard.applySourceDefaults(prev);
                                    wizard.dropRadioOnlyDecodeFlag();
                                }
                            }
                        }
                    }
                }

                ReadableText {
                    width: parent.width
                    visible: wizard.sourceType === "usb"
                    text: qsTr("An RTL-SDR dongle on a USB-OTG cable. Most public-safety listening starts here.")
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(13)
                    color: Theme.textSubdued
                    wrapMode: Text.Wrap
                }

                Column {
                    width: parent.width
                    visible: wizard.sourceType === "rtltcp" || wizard.sourceType === "tcp"
                    spacing: 10

                    PlexTextField {
                        id: hostField
                        label: qsTr("Host")
                        nextField: portField
                        error: text.trim().length && /^[A-Za-z0-9_.-]+$/.test(text.trim()) ? "" : qsTr("Enter a host name or IPv4 address.")
                        width: parent.width
                        mono: true
                        text: "192.168.1.10"
                        inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
                    }
                }

                Column {
                    width: parent.width
                    visible: wizard.sourceType === "rtltcp" || wizard.sourceType === "udp" || wizard.sourceType === "tcp"
                    spacing: 10

                    ReadableText {
                        text: wizard.sourceType === "udp" ? qsTr("Listen port") : qsTr("Port")
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(13)
                        color: Theme.textSecondary
                    }

                    PlexTextField {
                        id: portField
                        objectName: "wizardPortField"
                        label: qsTr("Port")
                        error: /^[0-9]+$/.test(text) && Number(text) >= 1 && Number(text) <= 65535 ? "" : qsTr("Enter a port from 1 to 65535.")
                        width: parent.width
                        mono: true
                        text: "1234"
                        // The hint only picks the soft keyboard; without the
                        // validator a hardware-keyboard "7,355" truncates to
                        // port 7 with no error.
                        inputMethodHints: Qt.ImhDigitsOnly
                        input.validator: IntValidator {
                            bottom: 1
                            top: 65535
                        }
                    }
                }

                Column {
                    width: parent.width
                    visible: wizard.sourceType === "file"
                    spacing: 10

                    ReadableText {
                        text: qsTr("Audio or capture file")
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(13)
                        color: Theme.textSecondary
                    }

                    Row {
                        width: parent.width
                        spacing: 10

                        PlexTextField {
                            id: fileField
                            text: wizard.fileText.substring(wizard.fileText.lastIndexOf("/") + 1)
                            input.readOnly: true
                            label: qsTr("Replay file")
                            width: parent.width - browse.width - 10
                            mono: true
                            placeholderText: qsTr("pick a .wav or .bin")
                        }

                        OutlineButton {
                            id: browse
                            objectName: "wizardReplayBrowse"
                            // At least as wide as its whole word: a fixed
                            // width split it over two lines at large text.
                            width: Math.max(96, implicitWidth)
                            text: qsTr("Browse")
                            enabled: !wizard.replayCopying
                            onClicked: wizard.browseReplayFile()
                        }
                    }

                    // #832: a document from a provider is copied before it can
                    // be replayed, and a capture can run to gigabytes. The copy
                    // shows as an export does on Call recordings: what is
                    // copied, how far it is, and a way out.
                    UiPanel {
                        objectName: "wizardReplayProgress"
                        width: parent.width
                        visible: wizard.replayCopying
                        height: replayProgressColumn.height + 2 * Theme.cardPadding

                        Column {
                            id: replayProgressColumn
                            x: Theme.cardPadding
                            y: Theme.cardPadding
                            width: parent.width - 2 * Theme.cardPadding
                            spacing: 10

                            ReadableText {
                                objectName: "wizardReplayProgressText"
                                width: parent.width
                                text: qsTr("Copying %1…").arg(replayFiles.copyName)
                                wrapMode: Text.Wrap
                                font.family: Theme.sans
                                font.pixelSize: Theme.fontSize(15)
                                font.weight: Font.DemiBold
                                color: Theme.textPrimary
                            }

                            // How far, on its own line: a long name wraps
                            // without splitting the figures.
                            LiveText {
                                id: replayAmount
                                objectName: "wizardReplayProgressAmount"
                                step: String(wizard.replayRequest)
                                width: parent.width
                                text: replayFiles.copyTotal > 0 ? qsTr("%1 of %2").arg(Util.sizeText(replayFiles.copyBytes)).arg(Util.sizeText(replayFiles.copyTotal)) : qsTr("%1 copied").arg(Util.sizeText(replayFiles.copyBytes))
                                wrapMode: Text.Wrap
                                font.family: Theme.sans
                                font.pixelSize: Theme.fontSize(13)
                                color: Theme.textSecondary
                            }

                            // Only for a known size: a bar with no end would be
                            // invented progress.
                            Rectangle {
                                objectName: "wizardReplayProgressBar"
                                width: parent.width
                                height: 4
                                radius: 2
                                visible: replayFiles.copyTotal > 0
                                color: Theme.toggleOffTrack
                                Accessible.role: Accessible.ProgressBar
                                Accessible.name: qsTr("Copy progress")
                                Accessible.description: replayAmount.text
                                // How far, as the bar shows it.
                                readonly property real fraction: replayFiles.copyTotal > 0 ? Math.min(1, replayFiles.copyBytes / replayFiles.copyTotal) : 0
                                // Read when asked (Android polls it); a plain Item's value raises no
                                // ValueChanged, so it stays live (issue #782). A whole percent, so a
                                // screen reader says "42", never a byte count.
                                readonly property int value: Math.floor(100 * fraction)
                                readonly property int minimumValue: 0
                                readonly property int maximumValue: 100

                                Rectangle {
                                    width: parent.width * parent.fraction
                                    height: parent.height
                                    radius: 2
                                    color: Theme.cyan
                                }
                            }

                            OutlineButton {
                                objectName: "wizardReplayCancel"
                                width: parent.width
                                text: qsTr("Cancel copy")
                                onClicked: wizard.cancelReplay()
                            }
                        }
                    }

                    // Why the last Browse kept the file above: the copy failed
                    // or no picker could open (B8). Only a problem ever shows
                    // here.
                    ReadableText {
                        id: replayNoticeLine
                        objectName: "wizardReplayNotice"
                        width: parent.width
                        visible: wizard.replayNotice.length > 0
                        text: wizard.replayNotice
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(13)
                        color: Theme.magenta
                        wrapMode: Text.Wrap
                        Accessible.role: Accessible.AlertMessage
                    }
                    DisclosureRow {
                        width: parent.width
                        visible: wizard.fileText.length > 0
                        title: qsTr("File details")
                        subtitle: qsTr("Inspect the saved replay copy")
                        onTapped: {
                            var info = decoderHost.documentInfo(wizard.fileText);
                            replayDetails.details = info.exists ? qsTr("%1\n%2 bytes\nModified: %3").arg(info.name).arg(info.sizeBytes).arg(info.modified) : qsTr("This file is missing. Use Browse to select it again.");
                            replayDetails.visible = true;
                        }
                    }
                }
            }

            // ---- Step 2: tune ----
            Column {
                width: parent.width
                visible: wizard.step === 1
                spacing: Theme.gap

                // The database can answer this whole step — frequency, decode
                // mode, trunking, files — and it covers conventional systems as
                // much as trunked ones. So the entry leads the step instead of
                // trailing the file pickers, where it read as a trunked-only
                // CSV utility discovered only after answering everything by
                // hand. A Column collapses an invisible child, so nothing
                // moves where the feature is absent.
                UiPanel {
                    width: parent.width
                    visible: radioReference.available
                    // From the row rather than a copy of its height: DisclosureRow
                    // is shared, and a card that restated its 58 would clip it the
                    // day that number moves.
                    height: rrEntryRow.height + 8

                    DisclosureRow {
                        id: rrEntryRow

                        // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                        objectName: "wizardRadioReferenceRow"

                        anchors.verticalCenter: parent.verticalCenter
                        title: qsTr("Import from RadioReference…")
                        subtitle: qsTr("Fills in the frequency, decode mode and talkgroups")
                        onTapped: wizard.openRadioReference()
                    }
                }

                UiPanel {
                    width: parent.width
                    visible: wizard.radioSource
                    height: freqColumn.height + 2 * Theme.cardPadding

                    Column {
                        id: freqColumn

                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: Theme.cardPadding
                        spacing: 8

                        // Read in the field below, which carries the same words (issue #782).
                        Text {
                            objectName: "wizardFrequencyCaption"
                            Accessible.ignored: true
                            text: qsTr("Frequency")
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(14)
                            color: Theme.textSecondary
                        }

                        FrequencyField {
                            id: freqField

                            width: parent.width
                            text: "851.375"
                            // Not editingFinished: every control on this screen
                            // is TapHandler-based and TapHandler takes no focus,
                            // so a user who types a frequency and taps Continue
                            // never commits the field. The hint text under this
                            // one already follows `trunking` live, so the switch
                            // moving as the band is entered is visible, not a
                            // surprise sprung at the end.
                            onTextChanged: wizard.refreshTrunkingSuggestion()
                        }

                        ReadableText {
                            width: parent.width
                            // "Control channel" only while the trunking answer
                            // below says there is one — a conventional system
                            // has no control channel, and the line follows the
                            // switch as it changes. Only a build without the
                            // importer sends the user to the website; with it,
                            // the entry above IS the way to look this up.
                            text: wizard.trunking ? (radioReference.available ? qsTr("Tune to the system's control channel.") : qsTr("Tune to the system's control channel — find it on RadioReference.")) : (radioReference.available ? qsTr("Tune to the frequency you want to hear.") : qsTr("Tune to the frequency you want to hear — find it on RadioReference."))
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(13)
                            color: Theme.textSubdued
                            wrapMode: Text.Wrap
                        }
                    }
                }

                ReadableText {
                    width: parent.width
                    text: qsTr("What should we decode?")
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(15)
                    font.weight: Font.DemiBold
                    color: Theme.textPrimary
                    wrapMode: Text.Wrap
                }

                Flow {
                    width: parent.width
                    spacing: 10

                    Repeater {
                        model: Util.decodeChips(wizard.decodeFlag)

                        DecodeChip {
                            required property var modelData

                            objectName: "wizardDecode_" + modelData.label
                            text: modelData.label
                            // AM needs the radio's I/Q (issue #524), TETRA the radio's I/Q or a symbol capture:
                            // network and other file audio arrives demodulated.
                            enabled: wizard.decodeFlagRunsHere(modelData.flag)
                            selected: wizard.decodeFlag === modelData.flag
                            onClicked: wizard.pickDecodeFlag(modelData.flag)
                        }
                    }
                }

                // Why the AM and TETRA chips are greyed out on a network or file
                // source, rather than leaving a dead control (issue #524).
                ReadableText {
                    objectName: "wizardDecodeIqNote"
                    visible: !wizard.radioSource
                    width: parent.width
                    text: qsTr("AM needs a radio source, and TETRA a radio source or a symbol capture (.bin): network and file audio arrives already demodulated.")
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(12)
                    color: Theme.textSecondary
                    wrapMode: Text.Wrap
                }

                ReadableText {
                    objectName: "wizardDecodeHint"
                    width: parent.width
                    text: Util.decodeHint(wizard.decodeFlag)
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(13)
                    color: Theme.textSubdued
                    wrapMode: Text.Wrap
                }

                // AM on a radio whose bandwidth (Advanced, or the app default)
                // cannot filter its channel: the step waits for a wider one.
                ReadableText {
                    objectName: "wizardDecodeAmBandwidthNote"
                    visible: text.length > 0
                    width: parent.width
                    text: wizard.amBandwidthError()
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(13)
                    color: Theme.magenta
                    wrapMode: Text.Wrap
                }

                // TETRA on a radio whose bandwidth is not 72 kHz (issue #803):
                // the step waits, as it does for AM.
                ReadableText {
                    objectName: "wizardDecodeTetraRateNote"
                    visible: text.length > 0
                    width: parent.width
                    text: wizard.tetraRateError()
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(13)
                    color: Theme.magenta
                    wrapMode: Text.Wrap
                }

                UiPanel {
                    width: parent.width
                    height: 66

                    Column {
                        anchors.left: parent.left
                        anchors.right: trunkSwitch.left
                        anchors.leftMargin: Theme.cardPadding
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3

                        // Both lines are read with the switch (issue #782): its
                        // name and its description.
                        Text {
                            id: trunkTitle
                            width: parent.width
                            Accessible.ignored: true
                            // The term, not a description: this audience knows
                            // trunking by name, and the subtitle carries the
                            // decision rule for anyone who does not.
                            text: qsTr("Trunking")
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(15)
                            font.weight: Font.DemiBold
                            color: Theme.textPrimary
                            elide: Text.ElideRight
                        }

                        Text {
                            id: trunkRule
                            width: parent.width
                            Accessible.ignored: true
                            // The decision rule, not the audience: a trunked
                            // utility system wants this on, a conventional fire
                            // channel wants it off. "Control channel" rather
                            // than "trunked" — the title and the card below
                            // already carry the term, and it is the thing the
                            // user actually tuned.
                            text: qsTr("On when the system uses a control channel")
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(13)
                            color: Theme.textSubdued
                            elide: Text.ElideRight
                        }
                    }

                    PlexSwitch {
                        id: trunkSwitch
                        objectName: "wizardTrunkSwitch"
                        accessibleName: trunkTitle.text
                        Accessible.description: trunkRule.text
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.cardPadding
                        anchors.verticalCenter: parent.verticalCenter
                        checked: wizard.trunking
                        onToggled: function (state) {
                            wizard.answerTrunking(state);
                        }
                    }
                }

                DecryptionProfileSelector {
                    objectName: "wizardDecryptionProfile"
                    width: parent.width
                    overlayParent: wizard
                    profileUid: wizard.decryptionProfileUid
                    protocol: Util.decryptionProtocol(wizard.decodeFlag)
                    onSelected: function (uid) {
                        wizard.decryptionProfileUid = uid;
                        encryptionEditor.reset();
                        // A profile replaces the key file: a choice for that
                        // field like any other (issue #829).
                        wizard.bumpCsvGeneration("keys");
                        wizard.keyCsvPath = "";
                    }
                }
                UiPanel {
                    width: parent.width
                    visible: wizard.decryptionProfileUid.length === 0
                    height: encryptionEditor.height + 2 * Theme.cardPadding
                    EncryptionEditor {
                        id: encryptionEditor
                        protocol: Util.decryptionProtocol(wizard.decodeFlag)
                        objectName: "wizardEncryptionEditor"
                        x: Theme.cardPadding
                        y: Theme.cardPadding
                        width: parent.width - 2 * Theme.cardPadding
                        csvPath: wizard.keyCsvPath
                    }
                }

                // Trunking data: the imported CSVs this system starts with. One
                // reusable row per file kind, each opening the shared picker sheet.
                UiPanel {
                    width: parent.width
                    height: csvColumn.height + Theme.cardPadding + 4

                    Column {
                        id: csvColumn

                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.topMargin: Theme.cardPadding
                        spacing: 0

                        MicroLabel {
                            text: qsTr("Trunking data")
                            leftPadding: Theme.cardPadding
                            bottomPadding: 6
                        }

                        CsvPickerRow {
                            title: qsTr("Channel map")
                            target: "chan"
                            path: wizard.chanCsvPath
                            showDivider: true
                        }

                        CsvPickerRow {
                            title: qsTr("Talkgroups")
                            target: "group"
                            path: wizard.groupCsvPath
                            showDivider: true
                        }

                        CsvPickerRow {
                            objectName: "wizardKeysCsvRow"
                            title: qsTr("Encryption keys")
                            target: "keys"
                            path: wizard.keyCsvPath
                            showDivider: true
                        }

                        // The operator-supplied IDEN table (--p25-bandplan) the
                        // P25 tuner falls back on when the control channel has
                        // not yet announced its own.
                        CsvPickerRow {
                            title: qsTr("P25 band plan")
                            target: "p25Bandplan"
                            path: wizard.p25BandplanCsvPath
                            showDivider: true
                        }

                        CsvPickerRow {
                            title: qsTr("Radio IDs")
                            target: "src"
                            path: wizard.srcCsvPath
                        }

                        ReadableText {
                            id: csvNoticeLine
                            objectName: "wizardCsvNotice"
                            width: parent.width
                            leftPadding: Theme.cardPadding
                            rightPadding: Theme.cardPadding
                            bottomPadding: 6
                            visible: wizard.csvNotice.length > 0
                            text: wizard.csvNotice
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(12)
                            color: wizard.csvNoticeIsProblem ? Theme.magenta : Theme.textSubdued
                            wrapMode: Text.Wrap
                            Accessible.role: wizard.csvNoticeIsProblem ? Accessible.AlertMessage : Accessible.StaticText
                        }

                        ReadableText {
                            width: parent.width
                            leftPadding: Theme.cardPadding
                            rightPadding: Theme.cardPadding
                            bottomPadding: 6
                            text: qsTr("Imported files are shared between systems. Manage them in Settings.")
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(12)
                            color: Theme.textSubdued
                            wrapMode: Text.Wrap
                        }
                    }
                }

                // Every source type gets the panel (the extra-flags field applies
                // to all of them); the tuner rows inside gate on radioSource.
                UiPanel {
                    width: parent.width
                    height: advHeader.height + (wizard.advancedOpen ? advBody.height + 6 : 0)
                    clip: true

                    Behavior on height {
                        NumberAnimation {
                            duration: 150
                            easing.type: Easing.OutCubic
                        }
                    }

                    Item {
                        id: advHeader

                        objectName: "wizardAdvancedHeader"
                        width: parent.width
                        height: 66
                        // A button a screen reader can press (issue #782), as the scan list's
                        // Advanced header is: its title is the name, its second line and
                        // whether it is open the description.
                        // Held while it has the focus (see OutlineButton).
                        activeFocusOnTab: (Navigation.allows(advHeader) && visible) || activeFocus
                        Accessible.role: Accessible.Button
                        Accessible.name: advTitle.text
                        Accessible.description: advSubtitle.text + ". " + (wizard.advancedOpen ? qsTr("Expanded") : qsTr("Collapsed"))
                        readonly property bool navigationAllowed: Navigation.allows(advHeader)
                        Accessible.ignored: !navigationAllowed || !visible
                        Accessible.onPressAction: toggle()
                        Keys.onSpacePressed: toggle()
                        Keys.onReturnPressed: toggle()
                        Keys.onEnterPressed: toggle()
                        function toggle() {
                            if (enabled && Navigation.allows(advHeader))
                                wizard.advancedOpen = !wizard.advancedOpen;
                        }
                        FocusFrame {}

                        Column {
                            anchors.left: parent.left
                            anchors.right: chevron.left
                            anchors.leftMargin: Theme.cardPadding
                            anchors.rightMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 3

                            Text {
                                id: advTitle
                                width: parent.width
                                text: qsTr("Advanced")
                                font.family: Theme.sans
                                font.pixelSize: Theme.fontSize(15)
                                font.weight: Font.DemiBold
                                color: Theme.textSecondary
                                elide: Text.ElideRight
                            }

                            Text {
                                id: advSubtitle
                                objectName: "wizardAdvancedSubtitle"
                                width: parent.width
                                // Only what the pane shows: an Airspy has its own
                                // gain controls and bias tee, and takes no PPM.
                                text: wizard.sourceType === "airspy" ? qsTr("Gain, bandwidth, bias tee — defaults work") : wizard.radioSource ? qsTr("Gain, PPM, bandwidth, bias tee — defaults work") : qsTr("Extra decoder flags — defaults work")
                                font.family: Theme.sans
                                font.pixelSize: Theme.fontSize(13)
                                color: Theme.textSubdued
                                elide: Text.ElideRight
                            }
                        }

                        Caret {
                            id: chevron
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.cardPadding
                            anchors.verticalCenter: parent.verticalCenter
                            rotation: wizard.advancedOpen ? 0 : -90
                            color: Theme.textSubdued

                            Behavior on rotation {
                                NumberAnimation {
                                    duration: 150
                                    easing.type: Easing.OutCubic
                                }
                            }
                        }

                        GatedTapHandler {
                            onAllowedTap: advHeader.toggle()
                        }
                    }

                    Column {
                        id: advBody

                        anchors.top: advHeader.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: Theme.cardPadding
                        anchors.rightMargin: Theme.cardPadding
                        spacing: 10
                        visible: wizard.advancedOpen

                        AirspyControls {
                            id: wizardAirspy
                            width: parent.width
                            visible: wizard.sourceType === "airspy"
                            settings: wizard.airspySettings
                            onEdited: function(key, value) {
                                var copy = Object.assign({}, wizard.airspySettings);
                                copy[key] = value;
                                wizard.airspySettings = copy;
                            }
                        }
                        // Gain and PPM are an RTL tuner's (an Airspy's gains are
                        // its own controls above, and it takes no PPM); the DSP
                        // bandwidth is every radio's (issue #806).
                        Row {
                            width: parent.width
                            visible: wizard.radioSource
                            spacing: 10

                            Column {
                                width: (parent.width - 20) / 3
                                visible: wizard.sourceType !== "airspy"
                                spacing: 6

                                // Read in the field below, which carries the same words (issue #782).
                                Text {
                                    objectName: "wizardGainCaption"
                                    Accessible.ignored: true
                                    text: qsTr("Gain dB")
                                    font.family: Theme.sans
                                    font.pixelSize: Theme.fontSize(12)
                                    color: Theme.textSecondary
                                }

                                PlexTextField {
                                    id: gainField
                                    objectName: "systemGainField"
                                    label: qsTr("Gain (dB)")
                                    width: parent.width
                                    mono: true
                                    placeholderText: String(prefs.gainDb)
                                    // Like the PPM field: the hint does not
                                    // constrain hardware keyboards, and a
                                    // non-numeric entry would otherwise read
                                    // back as an explicit 0 dB override.
                                    inputMethodHints: Qt.ImhDigitsOnly
                                    input.validator: IntValidator {
                                        bottom: 0
                                        top: 99
                                    }
                                }
                            }

                            Column {
                                width: (parent.width - 20) / 3
                                visible: wizard.sourceType !== "airspy"
                                spacing: 6

                                // Read in the field below, which carries the same words (issue #782).
                                Text {
                                    objectName: "wizardPpmCaption"
                                    Accessible.ignored: true
                                    text: qsTr("PPM")
                                    font.family: Theme.sans
                                    font.pixelSize: Theme.fontSize(12)
                                    color: Theme.textSecondary
                                }

                                PlexTextField {
                                    id: ppmField
                                    objectName: "systemPpmField"
                                    label: qsTr("PPM correction")
                                    width: parent.width
                                    mono: true
                                    placeholderText: String(prefs.ppm)
                                    // Signed integer only: this string is spliced
                                    // verbatim into the rtl input spec, and a hint
                                    // alone does not constrain hardware keyboards.
                                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                                    input.validator: IntValidator {
                                        bottom: -999
                                        top: 999
                                    }
                                }
                            }

                            // Alone on an Airspy, the field takes the row and
                            // its own label, without the caption the three
                            // narrow columns share.
                            Column {
                                width: wizard.sourceType === "airspy" ? parent.width : (parent.width - 20) / 3
                                spacing: 6

                                ReadableText {
                                    objectName: "wizardBandwidthCaption"
                                    visible: wizard.sourceType !== "airspy"
                                    text: qsTr("BW kHz")
                                    font.family: Theme.sans
                                    font.pixelSize: Theme.fontSize(12)
                                    color: Theme.textSecondary
                                }

                                PlexTextField {
                                    id: bwField
                                    objectName: "systemBandwidthField"
                                    label: qsTr("Bandwidth (kHz)")
                                    width: parent.width
                                    mono: true
                                    // What an empty field runs at: 72 kHz for
                                    // TETRA (issue #803), else the app default
                                    // as this radio takes it.
                                    placeholderText: {
                                        var appDefault = prefs.bandwidthKhz; // dependency: follow a change to the default
                                        var khz = sessionArgs.effectiveBandwidthKhz(wizard.decodeFlag, wizard.sourceType, -1);
                                        return khz > 0 ? String(khz) : String(appDefault);
                                    }
                                    inputMethodHints: Qt.ImhDigitsOnly
                                    input.validator: IntValidator {
                                        bottom: 1
                                        top: 9999
                                    }
                                }
                            }
                        }

                        PlexTextField {
                            id: hangtimeField
                            objectName: "systemHangtimeField"
                            width: parent.width
                            label: qsTr("Voice hang time")
                            unit: "s"
                            mono: true
                            placeholderText: prefs.hangtimeSec.toFixed(1)
                            hint: qsTr("Leave empty to follow the app default · next start. Scan lists always use the app default.")
                            inputMethodHints: Qt.ImhFormattedNumbersOnly
                            error: wizard.hangtimeValid() ? "" : qsTr("Enter hang time in seconds from 0 to 30.")
                        }

                        Column {
                            objectName: "systemBiasTee"
                            width: parent.width
                            spacing: 6
                            // rtl-tcp too: the engine applies the bias token on
                            // remote dongles, and an LNA on the far end needs it
                            // just as much as a local one.
                            visible: wizard.radioSource && wizard.sourceType !== "airspy"

                            ReadableText {
                                text: qsTr("Bias tee")
                                font.family: Theme.sans
                                font.pixelSize: Theme.fontSize(14)
                                color: Theme.textPrimary
                            }

                            ReadableText {
                                text: qsTr("Powers an external LNA. Off wins over the app-wide setting.")
                                font.family: Theme.sans
                                font.pixelSize: Theme.fontSize(12)
                                color: Theme.textSubdued
                            }

                            SegmentedControl {
                                width: parent.width
                                model: [qsTr("App default"), qsTr("On"), qsTr("Off")]
                                currentIndex: wizard.biasTee === 1 ? 1 : wizard.biasTee === 0 ? 2 : 0
                                onSelected: function (index) {
                                    wizard.biasTee = index === 1 ? 1 : index === 2 ? 0 : -1;
                                }
                            }
                        }

                        Column {
                            width: parent.width
                            spacing: 6

                            ReadableText {
                                text: qsTr("Extra CLI flags")
                                font.family: Theme.sans
                                font.pixelSize: Theme.fontSize(12)
                                color: Theme.textSecondary
                            }

                            PlexTextField {
                                id: extraField
                                label: qsTr("Extra decoder arguments")
                                width: parent.width
                                mono: true
                                // Appended after the app-wide extras from Settings;
                                // per-system channel/group imports live here.
                                placeholderText: qsTr("e.g. -C chan.csv -G group.csv")
                                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                            }
                        }

                        Item {
                            width: parent.width
                            height: 8
                        }
                    }
                }
            }

            // ---- Step 3: name ----
            Column {
                width: parent.width
                visible: wizard.step === 2
                spacing: Theme.gap

                ReadableText {
                    width: parent.width
                    text: qsTr("What should we call it?")
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(15)
                    font.weight: Font.DemiBold
                    color: Theme.textPrimary
                    wrapMode: Text.Wrap
                }

                PlexTextField {
                    id: nameField
                    label: qsTr("System name")
                    width: parent.width
                    placeholderText: qsTr("e.g. Hamilton Co P25")
                }

                ReadableText {
                    width: parent.width
                    text: qsTr("The name is yours — county, agency, whatever you'll recognize on the home screen.")
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(13)
                    color: Theme.textSubdued
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    GradientButton {
        id: continueButton
        objectName: "wizardContinue"

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.screenPadding
        anchors.bottomMargin: Math.max(22, wizard.height - Theme.keyboardTop(wizard) + 8)
        text: wizard.step < 2 ? qsTr("Continue") : qsTr("Save system")
        // Not while a replay copy runs: the system would save the earlier file.
        inputValid: wizard.stepValid() && !wizard.replayCopying
        onClicked: {
            if (wizard.step < 2) {
                wizard.step++;
            } else {
                wizard.commit();
            }
        }
    }

    // Picker for one trunking-data field: the library's files of that kind,
    // "None", and a fresh import.
    //
    // The keys picker's Decimal/Hex choice is pending until a file is chosen,
    // and reopening the picker starts it from the assigned file's format. So
    // the picker ignores an outside tap (issue #718), which would have reset a
    // choice the user just made, and leaves through Cancel, Back or Escape,
    // which share one discard check: a choice changed since the picker opened,
    // with no assigned file it already applies to, is asked about before it
    // goes. An unchanged choice closes at once.
    ModalSheet {
        id: csvSheet
        objectName: "wizardCsvSheet"
        closesOnOutsideTap: false
        dismissHandler: function () {
            csvSheet.requestCancel();
        }

        // The Decimal/Hex choice when the picker opened.
        property bool openedKeyHex: false

        function hasPendingChoice() {
            return wizard.pickerTarget === "keys" && wizard.keyCsvPath.length === 0 && wizard.pickerKeyHex !== csvSheet.openedKeyHex;
        }
        function requestCancel() {
            if (csvSheet.hasPendingChoice())
                csvDiscard.ask(function () {
                    csvSheet.visible = false;
                });
            else
                csvSheet.visible = false;
        }

        DiscardDialog {
            id: csvDiscard
            objectName: "wizardCsvDiscard"
            parent: csvSheet
        }

        readonly property string currentPath: wizard.pickerTarget === "chan" ? wizard.chanCsvPath : wizard.pickerTarget === "group" ? wizard.groupCsvPath : wizard.pickerTarget === "p25Bandplan" ? wizard.p25BandplanCsvPath : wizard.pickerTarget === "src" ? wizard.srcCsvPath : wizard.keyCsvPath
        readonly property var entries: {
            var n = importedFiles.revision; // dependency: recompute when the library changes, a replaced file's counts too
            if (!visible || wizard.pickerTarget === "source")
                return [];
            return wizard.pickerTarget === "keys" ? importedFiles.entriesForType("keysDec").concat(importedFiles.entriesForType("keysHex")) : importedFiles.entriesForType(wizard.pickerTarget);
        }

        function entrySummary(entry) {
            var noun = wizard.pickerTarget === "chan" ? (entry.accepted === 1 ? qsTr("channel") : qsTr("channels")) : wizard.pickerTarget === "group" ? (entry.accepted === 1 ? qsTr("talkgroup") : qsTr("talkgroups")) : wizard.pickerTarget === "p25Bandplan" ? (entry.accepted === 1 ? qsTr("identifier") : qsTr("identifiers")) : wizard.pickerTarget === "src" ? (entry.accepted === 1 ? qsTr("radio ID") : qsTr("radio IDs")) : (entry.accepted === 1 ? qsTr("key") : qsTr("keys"));
            var line = entry.accepted + " " + noun;
            if (wizard.pickerTarget === "keys")
                line += " · " + (entry.type === "keysHex" ? qsTr("hex") : qsTr("decimal"));
            return line;
        }

        MicroLabel {
            text: wizard.pickerTarget === "chan" ? qsTr("Channel map") : wizard.pickerTarget === "group" ? qsTr("Talkgroups") : wizard.pickerTarget === "p25Bandplan" ? qsTr("P25 band plan") : wizard.pickerTarget === "src" ? qsTr("Radio IDs") : qsTr("Encryption keys")
        }

        // The library is unbounded, and the sheet is centred with no scrolling of
        // its own: past a handful of files an unclipped Repeater would push the
        // "None"/import controls off the bottom and the title off the top, with
        // no way to reach either. Cap the list and let it scroll instead.
        PlexFlickable {
            width: parent.width
            height: Math.min(entryColumn.height, 48 * 5)
            visible: csvSheet.entries && csvSheet.entries.length > 0
            clip: true
            contentHeight: entryColumn.height
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: entryColumn

                width: parent.width

                Repeater {
                    model: csvSheet.entries

                    Item {
                        id: csvChoice
                        // Held while it has the focus (see OutlineButton).
                        activeFocusOnTab: Navigation.allows(csvChoice) || activeFocus
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: modelData.name
                        // The file's lines as shown (issue #782): whether it is the one selected, and what it holds.
                        Accessible.description: Util.shownText(csvChoiceLabels)
                        Accessible.checkable: true
                        Accessible.checked: modelData.path === csvSheet.currentPath
                        readonly property bool navigationAllowed: Navigation.allows(csvChoice)
                        Accessible.ignored: !navigationAllowed
                        Accessible.onPressAction: choose()
                        Keys.onReturnPressed: choose()
                        Keys.onEnterPressed: choose()
                        Keys.onSpacePressed: choose()
                        FocusFrame {}
                        function choose() {
                            if (!Navigation.allows(csvChoice))
                                return;
                            wizard.assignCsvPath(wizard.pickerTarget, modelData.path, modelData.type === "keysHex");
                            csvSheet.visible = false;
                        }
                        required property var modelData

                        width: entryColumn.width
                        height: Math.max(48, csvChoiceLabels.implicitHeight + 16)

                        Column {
                            id: csvChoiceLabels
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2

                            Text {
                                width: parent.width
                                text: modelData.path === csvSheet.currentPath ? qsTr("Selected: %1").arg(modelData.name) : modelData.name
                                // An imported file's name: drawn as written.
                                textFormat: Text.PlainText
                                font.family: Theme.sans
                                font.pixelSize: Theme.fontSize(15)
                                font.weight: Font.DemiBold
                                color: modelData.path === csvSheet.currentPath ? Theme.cyan : Theme.textPrimary
                                elide: Text.ElideRight
                            }

                            Text {
                                width: parent.width
                                text: csvSheet.entrySummary(modelData)
                                font.family: Theme.sans
                                font.pixelSize: Theme.fontSize(12)
                                color: Theme.textSubdued
                                elide: Text.ElideRight
                            }
                        }

                        GatedTapHandler {
                            onAllowedTap: csvChoice.choose()
                        }
                    }
                }
            }
        }

        OutlineButton {
            width: parent.width
            visible: csvSheet.currentPath.length > 0
            text: qsTr("None")
            onClicked: {
                wizard.assignCsvPath(wizard.pickerTarget, "", false);
                csvSheet.visible = false;
            }
        }

        SegmentedControl {
            objectName: "wizardKeyFormat"
            width: parent.width
            visible: wizard.pickerTarget === "keys"
            model: [qsTr("Decimal keys"), qsTr("Hex keys")]
            currentIndex: wizard.pickerKeyHex ? 1 : 0
            // Also re-reads the already-assigned file: the control opens showing
            // that file's dec/hex state, so flipping it has to change it —
            // otherwise it looks like a setting and silently does nothing.
            onSelected: function (index) {
                wizard.pickerKeyHex = index === 1;
                if (wizard.pickerTarget === "keys" && wizard.keyCsvPath.length > 0)
                    wizard.keyCsvHex = wizard.pickerKeyHex;
            }
        }

        GradientButton {
            objectName: "wizardCsvImportNew"
            width: parent.width
            text: qsTr("Import new file")
            onClicked: {
                csvSheet.visible = false;
                wizard.importCsvFile();
            }
        }

        OutlineButton {
            objectName: "wizardCsvCancel"
            width: parent.width
            text: qsTr("Cancel")
            onClicked: csvSheet.requestCancel()
        }
    }
}
