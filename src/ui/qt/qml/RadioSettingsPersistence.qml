// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// Track explicit default-control requests outside the sheet's lifetime. A queue
// acceptance is not a decoder acceptance: only a matching configured snapshot
// may update the saved system, never an effective scan-row override.
Item {
    id: persistence
    property string systemUid: ""
    property bool active: false
    property var pending: ({})
    property string error: ""
    signal saved()

    onSystemUidChanged: reset()
    onActiveChanged: reset()

    function reset() {
        pending = ({});
        error = "";
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
        Qt.callLater(confirm);
    }

    function squelchMatches(value) {
        if (value.mode === "auto" || value.mode === "noise")
            return (value.mode === "auto" ? metrics.configuredSquelchAuto : metrics.configuredSquelchNoise)
                && value.margin === metrics.configuredSquelchMarginDb;
        return !metrics.configuredSquelchAuto && !metrics.configuredSquelchNoise
            && value.off === metrics.configuredSquelchLevelOff
            && (value.off || Math.abs(value.db - metrics.configuredSquelchDb) < 0.11);
    }

    function squelchText(value) {
        if (value.mode === "auto" || value.mode === "noise")
            return value.mode + "+" + value.margin;
        // Zero dB can be the legacy linear full-scale level; CLI zero means off.
        return value.off ? "off" : String(value.db < 0 ? value.db : Math.pow(10, value.db / 10));
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
        if (next.squelch !== undefined && squelchMatches(next.squelch)) {
            changes.squelch = squelchText(next.squelch);
            delete next.squelch;
        }
        if (Object.keys(changes).length === 0)
            return;
        pending = next;
        if (savedSystems.updateRadioSettings(systemUid, changes)) {
            error = "";
            saved();
        } else {
            error = qsTr("Could not save Radio settings. Check available storage and try the change again.");
        }
    }

    Connections {
        target: metrics
        function onControlChanged() { persistence.confirm(); }
    }
}
