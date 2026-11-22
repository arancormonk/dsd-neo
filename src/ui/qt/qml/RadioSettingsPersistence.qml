// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// Track explicit default-control requests outside the sheet's lifetime. A queue
// acceptance is not a decoder acceptance: only a matching configured snapshot
// may update the saved system, never an effective scan-row override.
//
// Issue #899 (B15): a squelch level is saved only in a form the app's own
// controls make, Off or a whole dB from -120 to -1, read from the configured
// level itself. A level set elsewhere (Advanced arguments, the command line)
// is not saved, and the sheet says so. A dynamic choice saves only itself.
// The level kept beneath a dynamic one, put back, is saved only as the level
// asked for and once the decoder has answered that request, never from an
// older snapshot. Clear (B11) removes the saved squelch for the next start and
// drops any squelch request still waiting, so an older one cannot put it back.
Item {
    id: persistence
    property string systemUid: ""
    property bool active: false
    property var pending: ({})
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
        error = "";
        if (key === "squelch") {
            squelchCleared = false;
            squelchLevelError = "";
        }
        Qt.callLater(confirm);
    }

    // A level request is {mode: "level", db, off} from a step, or {mode:
    // "level", restore: true, request, off, whole, db} for the level kept
    // beneath a dynamic squelch, as CommandBridge.restoreSquelchLevel()
    // submitted it. A level matches only the same Off or whole dB, so two
    // requests can never confirm on one snapshot; a restore also only on a
    // snapshot that shows the decoder answered it, and one of a level the
    // app's controls cannot make on any such level, which is then not saved.
    function squelchMatches(value) {
        if (value.mode === "auto" || value.mode === "noise")
            return (value.mode === "auto" ? metrics.configuredSquelchAuto : metrics.configuredSquelchNoise)
                && value.margin === metrics.configuredSquelchMarginDb;
        if (metrics.configuredSquelchAuto || metrics.configuredSquelchNoise)
            return false;
        if (value.restore === true && !restoreAnswered(value))
            return false;
        if (value.off || metrics.configuredSquelchLevelOff === true)
            return value.off === metrics.configuredSquelchLevelOff;
        if (value.restore === true && value.whole !== true)
            return metrics.configuredSquelchLevelWhole !== true;
        return metrics.configuredSquelchLevelWhole === true && value.db === metrics.configuredSquelchLevelWholeDb;
    }

    // Whether the decoder has handled a restore request: a snapshot taken after it.
    function restoreAnswered(value) {
        return value.restore === true && value.request !== undefined
            && metrics.configuredSquelchRequestAnswered === value.request;
    }

    // What the saved system keeps for a confirmed request, from the
    // configured setting; "" for a level the app's controls cannot make.
    function squelchText(value) {
        if (value.mode === "auto" || value.mode === "noise")
            return value.mode + "+" + value.margin;
        if (metrics.configuredSquelchLevelOff === true)
            return "off";
        return metrics.configuredSquelchLevelWhole === true && metrics.configuredSquelchLevelWholeDb < 0
            ? String(metrics.configuredSquelchLevelWholeDb) : "";
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
        if (next.squelch !== undefined && squelchMatches(next.squelch)) {
            var text = squelchText(next.squelch);
            if (text.length > 0)
                changes.squelch = text;
            else
                refusedLevel = true;
            delete next.squelch;
        } else if (next.squelch !== undefined && restoreAnswered(next.squelch)) {
            // Answered, and not on the level asked for: refused, or set since
            // by something else. Nothing of it is saved.
            delete next.squelch;
            settled = true;
        }
        if (refusedLevel || settled)
            pending = next;
        if (refusedLevel)
            squelchLevelError = qsTr("This level was set outside the app, so it is not saved.");
        if (Object.keys(changes).length === 0)
            return;
        pending = next;
        if (savedSystems.updateRadioSettings(systemUid, changes)) {
            error = "";
            if (changes.squelch !== undefined)
                squelchStoreError = "";
            saved();
        } else {
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
