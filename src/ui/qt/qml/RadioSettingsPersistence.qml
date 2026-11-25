// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// Track explicit default-control requests outside the sheet's lifetime. A queue
// acceptance is not a decoder acceptance: only a matching configured snapshot
// may update the saved system, never an effective scan-row override.
//
// Issue #899 (B15): a squelch level is saved only in a form the app's own
// controls make, Off or a whole dB from -120 to -1: a step asks for one, and
// a restore reads it from the configured level. A level set elsewhere (Advanced arguments, the command line)
// is not saved, and the sheet says so. A dynamic choice saves only itself.
// Every squelch request carries the tag the bridge gave it, and is settled
// only on the decoder's answer to that request, never on an older snapshot:
// applied, the setting asked for is saved; refused, or superseded before the
// decoder reached it, nothing is. Clear (B11) removes the saved squelch for the
// next start and drops any squelch request still waiting, so an older one
// cannot put it back.
Item {
    id: persistence
    property string systemUid: ""
    property bool active: false
    property var pending: ({})
    // Why a Radio setting other than the squelch could not be saved.
    property string error: ""
    // Issue #899 (B11): Clear took the saved squelch out, until the next request.
    property bool squelchCleared: false
    // Issue #899: why a level was not saved (one set outside the app), shown
    // by the squelch controls while a level is the setting.
    property string squelchLevelError: ""
    // Why the saved squelch could not be cleared or saved, shown whatever the
    // setting: the saved one still applies at the next start. It stands until
    // a clear or a squelch save succeeds.
    property string squelchStoreError: ""
    signal saved()

    onSystemUidChanged: reset()
    onActiveChanged: reset()

    function reset() {
        pending = ({});
        error = "";
        squelchCleared = false;
        squelchLevelError = "";
        squelchStoreError = "";
    }

    function request(key, value) {
        if (!active || systemUid.length === 0 || !metrics.optionsKnown)
            return;
        // The decoder treats a selected named preset as a no-op. Auto still
        // reapplies its preset; a pending mode change also makes this a new choice.
        if (key === "decodeFlag" && pending.decodeFlag === undefined
            && commands.decodeModeForFlag(value) !== commands.decodeModeForFlag("")
            && commands.decodeModeForFlag(value) === metrics.decodeMode)
            return;
        var next = Object.assign({}, pending);
        // A decoder preset resets modulation; a later modulation request wins.
        if (key === "decodeFlag")
            delete next.modulation;
        next[key] = value;
        pending = next;
        if (key === "squelch") {
            squelchCleared = false;
            squelchLevelError = "";
        } else {
            error = "";
        }
        Qt.callLater(confirm);
    }

    // A request is {mode: "auto" or "noise", margin, request}, {mode:
    // "level", db, off, request} from a step, or {mode: "level", restore:
    // true, request, off, whole, db} for the level kept beneath a dynamic
    // squelch, as CommandBridge.restoreSquelchLevel() submitted it; request is
    // the bridge's tag. The decoder answers it in the state snapshot with
    // whether it applied it, the same publication as a scan scope's configured
    // squelch, so the answer holds for whichever scope the request edited,
    // beneath a scan row's own squelch too.
    function squelchAnswered(value) {
        return value.request !== undefined && value.request > 0
            && metrics.configuredSquelchRequestAnswered === value.request;
    }

    // What the saved system keeps for an applied request: the setting it asked
    // for; "" for a level the app's controls cannot make (a restore of such a
    // level), which is then not saved.
    function squelchText(value) {
        if (value.mode === "auto" || value.mode === "noise")
            return value.mode + "+" + value.margin;
        if (value.off === true)
            return "off";
        if (value.restore === true && value.whole !== true)
            return "";
        var db = Number(value.db);
        return Number.isInteger(db) && db >= -120 && db <= -1 ? String(db) : "";
    }

    /**
     * Issue #899 (B11): remove the saved system's own squelch. It applies from
     * the next start; the session keeps what it runs. A squelch request still
     * waiting is dropped first, so it cannot put the setting back.
     */
    function clearSquelch() {
        if (systemUid.length === 0)
            return false;
        var next = Object.assign({}, pending);
        delete next.squelch;
        pending = next;
        if (savedSystems.clearRadioSetting(systemUid, "squelch")) {
            squelchStoreError = "";
            squelchCleared = true;
            saved();
            return true;
        }
        squelchStoreError = qsTr("Could not clear the saved squelch. Check available storage and try again.");
        return false;
    }

    function confirm() {
        if (!active || systemUid.length === 0 || !metrics.optionsKnown)
            return;
        var next = Object.assign({}, pending);
        var changes = {};
        if (next.decodeFlag !== undefined && commands.decodeModeForFlag(next.decodeFlag) === metrics.decodeMode) {
            changes.decodeFlag = next.decodeFlag;
            delete next.decodeFlag;
        }
        // A stale snapshot may already match a modulation picked after a preset.
        // Confirm that preset first so saving it cannot erase the later choice.
        if (next.decodeFlag === undefined && next.modulation !== undefined && next.modulation === metrics.modulation) {
            changes.modulation = next.modulation;
            delete next.modulation;
        }
        var refusedLevel = false;
        var settled = false;
        if (next.squelch !== undefined && squelchAnswered(next.squelch)) {
            if (metrics.configuredSquelchRequestApplied === true) {
                var text = squelchText(next.squelch);
                if (text.length > 0)
                    changes.squelch = text;
                else
                    refusedLevel = true;
            } else {
                // Refused: nothing of it is saved, even when it asked for the
                // setting already in force.
                settled = true;
            }
            delete next.squelch;
        }
        if (refusedLevel || settled)
            pending = next;
        if (refusedLevel)
            squelchLevelError = qsTr("This level was set outside the app, so it is not saved.");
        if (Object.keys(changes).length === 0)
            return;
        pending = next;
        // A squelch save's outcome is the squelch's own (squelchStoreError),
        // so a later successful clear can take it back without hiding a
        // failure of the other settings, and the reverse.
        var others = Object.keys(changes).some(function (key) { return key !== "squelch"; });
        if (savedSystems.updateRadioSettings(systemUid, changes)) {
            if (others)
                error = "";
            if (changes.squelch !== undefined)
                squelchStoreError = "";
            saved();
        } else {
            if (others)
                error = qsTr("Could not save Radio settings. Check available storage and try the change again.");
            if (changes.squelch !== undefined)
                squelchStoreError = qsTr("Could not save the squelch. Check available storage and try again.");
        }
    }

    Connections {
        target: metrics
        function onControlChanged() { persistence.confirm(); }
    }
}
