// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// The passband a rigctl peer is asked for on audio input (#621), opened from
// the monitor's Passband row. The peer demodulates what this input hears, so
// the analog width is the passband the peer runs rather than a channel filter:
// the NFM and AM widths step over the common channel plans with no DSP rate to
// bound them, and -B (or the peer's own passband without it) stands in for an
// unset NFM width. On a radio input the Radio sheet edits the width.
//
// Every control reads the engine, with the Radio sheet's one bounded exception:
// a request sent and not yet answered stands in for the reading until the
// engine's arrives or requestTtlMs passes, so taps inside one 250 ms poll each
// step from the last request rather than from the same stale reading.
ModalSheet {
    id: sheet
    objectName: "passbandSheet"
    accessibleName: qsTr("Passband")

    property var bridge: commands
    readonly property bool sessionRunning: decoderHost.running
    onSessionRunningChanged: {
        if (!sessionRunning)
            closeEditor();
    }
    // The peer gone (a disconnect, a switch to another input) leaves no
    // passband to set here.
    readonly property bool peerPassband: metrics.peerPassband === true
    onPeerPassbandChanged: {
        if (!peerPassband)
            closeEditor();
    }

    readonly property int requestTtlMs: 1500
    // NaN means "no outstanding request; the engine's reading is the truth".
    // One per kind: each section sends its own kind's width command.
    property real pendingNfm: NaN
    property real pendingAm: NaN

    // Neither kind offered nor asked for: the monitor's row is gone as well.
    // Nothing is outstanding then, so there is no request to forget.
    readonly property bool kindsLeft: metrics.nfmBandwidthOffered === true || metrics.amBandwidthOffered === true
        || !isNaN(pendingNfm) || !isNaN(pendingAm)
    onKindsLeftChanged: {
        if (!kindsLeft)
            visible = false;
    }

    // Issue #518: while a scan row on air takes a "this channel" width edit,
    // the sheet can set that row's own passband for the rest of the session
    // instead of the default. The row is captured when the scope is chosen,
    // and the sheet goes back to the default when another row comes on air:
    // the readings would no longer be the row's.
    readonly property int rowFields: metrics.scanRowActive === true ? metrics.scanRowEditable : 0
    readonly property bool rowTakesWidth: (rowFields & sheet.bridge.scanRowFieldWidth) !== 0
    property bool rowScopeChosen: false
    property var rowContext: ({})
    readonly property bool rowWidth: rowScopeChosen && rowTakesWidth
    readonly property bool rowSynced: metrics.scanRowSynced === true
    readonly property string onAirRow: metrics.scanRowActive === true ? metrics.scanRowKey : ""
    onOnAirRowChanged: {
        if (rowScopeChosen)
            chooseScope(false);
    }
    // The kind the readings describe: the configured preset's, or while an
    // analog scan row is on air the kind that row runs, which "This channel"
    // edits alone.
    readonly property bool rowAm: metrics.analogBandwidthAm === true
    // A row on air with its own width keeps it while on air; "All channels"
    // then edits the default beneath it.
    readonly property bool rowOverride: metrics.analogBandwidthRowOverride === true

    readonly property bool nfmShown: rowWidth ? !rowAm : (metrics.nfmBandwidthOffered === true || !isNaN(pendingNfm))
    readonly property bool amShown: rowWidth ? rowAm : (metrics.amBandwidthOffered === true || !isNaN(pendingAm))

    // What the monitor's Passband row shows, as the terminal's Passband field
    // spells it: the passband the peer is asked for now ("NFM 12.5 kHz (-B)"),
    // which the shared view reads whenever a width is offered, off the monitor
    // too. Never a setting the peer is not asked for.
    readonly property string summary: {
        var reading = metrics.analogBandwidthReading;
        if (reading === undefined || reading === "")
            return "";
        return (metrics.analogBandwidthAm === true ? qsTr("AM %1") : qsTr("NFM %1")).arg(reading);
    }

    function openEditor() {
        forgetRequests();
        chooseScope(false);
        open();
    }
    function closeEditor() {
        visible = false;
        forgetRequests();
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
    /** "Use default" or "List value": the reading changes to a value the sheet did not compute. */
    function rowAction(action) {
        forgetRequests();
        sheet.bridge.editScanRow(rowContext, sheet.bridge.scanRowFieldWidth, action, {});
    }
    function forgetRequests() {
        pendingNfm = NaN;
        pendingAm = NaN;
        nfmTtl.stop();
        amTtl.stop();
    }

    /** The configured width of AM (@p am) or NFM, 0 when unset. */
    function configuredOf(am) {
        return (am ? metrics.amBandwidthConfiguredHz : metrics.nfmBandwidthConfiguredHz) || 0;
    }
    /** The setting "All channels" edits, a request standing in for it: a width, or 0 for unset. */
    function settingOf(am) {
        var pending = am ? pendingAm : pendingNfm;
        return isNaN(pending) ? configuredOf(am) : pending;
    }
    /**
     * A setting as the session reads it: a width, or what an unset one stands
     * for ("-B 12.5 kHz", "peer's own", "default").
     */
    function settingText(am, hz) {
        if (hz > 0)
            return Util.widthKhzText(hz);
        return am ? metrics.amBandwidthUnsetText : metrics.nfmBandwidthUnsetText;
    }
    /**
     * The width an unset setting stands for: -B for NFM, the 6 kHz default for
     * AM, and the kind's default where neither gives one (the peer's own
     * passband has no width to step from).
     */
    function unsetWidthOf(am) {
        var hz = (am ? metrics.amBandwidthUnsetHz : metrics.nfmBandwidthUnsetHz) || 0;
        if (hz > 0)
            return hz;
        return am ? Util.AM_DEFAULT_WIDTH_HZ : Util.NFM_DEFAULT_WIDTH_HZ;
    }

    /**
     * What a kind's section reads. "This channel": the step asked for, else
     * the row's passband. "All channels": a request, then the passband asked
     * for this kind's setting while its monitor runs and no row sets its own,
     * then the setting. While a typed digital row on air keeps the peer at -B,
     * the reading is not the setting's, and the setting is what a step moves.
     */
    function valueText(am) {
        var pending = am ? pendingAm : pendingNfm;
        if (rowWidth)
            return isNaN(pending) ? metrics.analogBandwidthReading : Util.widthKhzText(pending);
        if (!isNaN(pending))
            return settingText(am, pending);
        if (metrics.passbandInForce === true && rowAm === am && !rowOverride)
            return metrics.analogBandwidthReading;
        return settingText(am, configuredOf(am));
    }

    /**
     * Where a step starts. "This channel": the width setting the row runs,
     * from the same snapshots as its identity, or what an unset one stands for.
     * "All channels": the setting, or what an unset one stands for (-B), never
     * a row's own width.
     */
    function stepFrom(am) {
        var pending = am ? pendingAm : pendingNfm;
        if (!isNaN(pending) && pending > 0)
            return pending;
        if (rowWidth)
            return metrics.analogBandwidthSettingHz > 0 ? metrics.analogBandwidthSettingHz : unsetWidthOf(am);
        var configured = isNaN(pending) ? configuredOf(am) : 0;
        return configured > 0 ? configured : unsetWidthOf(am);
    }
    /** The next width in @p direction, or -1 at the end of the list; no DSP rate bounds a peer's passband. */
    function nextWidth(am, direction) {
        return Util.nextWidthIn(am ? Util.AM_WIDTHS_HZ : Util.NFM_WIDTHS_HZ, stepFrom(am), direction, 0);
    }
    /** In "This channel" the steps wait while the readings are not the row's. */
    function canStep(am, direction) {
        return (!rowWidth || rowSynced) && nextWidth(am, direction) > 0;
    }

    /** Ask for @p hz (0 = unset) of AM (@p am) or NFM, on the row or the default. */
    function requestWidth(am, hz) {
        if (am) {
            pendingAm = hz;
            amTtl.restart();
        } else {
            pendingNfm = hz;
            nfmTtl.restart();
        }
        if (rowWidth)
            sheet.bridge.editScanRow(rowContext, sheet.bridge.scanRowFieldWidth, sheet.bridge.scanRowEditSet,
                {"widthHz": hz});
        else if (am)
            sheet.bridge.setAmBandwidthHz(hz);
        else
            sheet.bridge.setNfmBandwidthHz(hz);
    }
    function stepWidth(am, direction) {
        if (!canStep(am, direction))
            return;
        requestWidth(am, nextWidth(am, direction));
    }
    /**
     * Return the default to unset (0): on NFM -B, or the peer's own passband,
     * stands in again. A row edit has no unset; "Use default" does that.
     */
    function resetWidth(am) {
        if (rowWidth || settingOf(am) <= 0)
            return;
        requestWidth(am, 0);
    }

    Timer {
        id: nfmTtl

        interval: sheet.requestTtlMs
        onTriggered: sheet.pendingNfm = NaN
    }
    Timer {
        id: amTtl

        interval: sheet.requestTtlMs
        onTriggered: sheet.pendingAm = NaN
    }

    Text {
        width: parent.width
        text: qsTr("Passband")
        wrapMode: Text.Wrap
        color: Theme.textPrimary
        font.pixelSize: Theme.fontSize(20)
    }
    Text {
        width: parent.width
        text: qsTr("The rigctl peer demodulates this input, so the width is the passband it is asked for. An unset NFM width asks for -B, or leaves the peer's own passband without it.")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(13)
    }
    // What the controls edit while a scan row on air takes "this channel" edits.
    SegmentedControl {
        objectName: "passbandScope"
        visible: sheet.rowTakesWidth || sheet.rowWidth
        width: parent.width
        model: [qsTr("All channels"), qsTr("This channel")]
        currentIndex: sheet.rowWidth ? 1 : 0
        onSelected: function (index) {
            sheet.chooseScope(index === 1);
        }
    }
    Text {
        objectName: "passbandScopeNote"
        width: parent.width
        visible: sheet.rowWidth
        text: qsTr("Applies to %1 for this session, not to the list or the default.").arg(sheet.rowContext.label || "")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(13)
    }

    // ---- NFM ----
    Column {
        objectName: "passbandNfm"
        visible: sheet.nfmShown
        width: parent.width
        spacing: 8
        Text {
            objectName: "passbandNfmTitle"
            text: qsTr("NFM passband")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSize(14)
        }
        Row {
            width: parent.width
            spacing: 10
            Text {
                objectName: "passbandNfmValue"
                width: parent.width - 116
                anchors.verticalCenter: parent.verticalCenter
                text: sheet.valueText(false)
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.textPrimary
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(14)
            }
            OutlineButton {
                objectName: "passbandNfmDown"
                width: 48
                text: "−"
                accessibleName: qsTr("Narrower NFM Passband")
                enabled: sheet.canStep(false, -1)
                onClicked: sheet.stepWidth(false, -1)
            }
            OutlineButton {
                objectName: "passbandNfmUp"
                width: 48
                text: "+"
                accessibleName: qsTr("Wider NFM Passband")
                enabled: sheet.canStep(false, 1)
                onClicked: sheet.stepWidth(false, 1)
            }
        }
        // Back to the unset default, offered while an explicit width is set.
        OutlineButton {
            objectName: "passbandNfmDefault"
            visible: !sheet.rowWidth && sheet.settingOf(false) > 0
            width: parent.width
            text: qsTr("Use the default width")
            accessibleName: qsTr("Default NFM Passband")
            onClicked: sheet.resetWidth(false)
        }
    }

    // ---- AM ----
    Column {
        objectName: "passbandAm"
        visible: sheet.amShown
        width: parent.width
        spacing: 8
        Text {
            objectName: "passbandAmTitle"
            text: qsTr("AM passband")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSize(14)
        }
        Row {
            width: parent.width
            spacing: 10
            Text {
                objectName: "passbandAmValue"
                width: parent.width - 116
                anchors.verticalCenter: parent.verticalCenter
                text: sheet.valueText(true)
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.textPrimary
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(14)
            }
            OutlineButton {
                objectName: "passbandAmDown"
                width: 48
                text: "−"
                accessibleName: qsTr("Narrower AM Passband")
                enabled: sheet.canStep(true, -1)
                onClicked: sheet.stepWidth(true, -1)
            }
            OutlineButton {
                objectName: "passbandAmUp"
                width: 48
                text: "+"
                accessibleName: qsTr("Wider AM Passband")
                enabled: sheet.canStep(true, 1)
                onClicked: sheet.stepWidth(true, 1)
            }
        }
        OutlineButton {
            objectName: "passbandAmDefault"
            visible: !sheet.rowWidth && sheet.settingOf(true) > 0
            width: parent.width
            text: qsTr("Use the default width")
            accessibleName: qsTr("Default AM Passband")
            onClicked: sheet.resetWidth(true)
        }
    }

    // "This channel": follow the default, offered when the list sets the
    // width, or go back to the list's value, offered while the row runs an edit.
    Row {
        objectName: "passbandRowActions"
        visible: sheet.rowWidth && ((metrics.scanRowListed | metrics.scanRowEdited)
            & sheet.bridge.scanRowFieldWidth) !== 0
        spacing: 8
        OutlineButton {
            objectName: "passbandRowInherit"
            visible: (metrics.scanRowListed & sheet.bridge.scanRowFieldWidth) !== 0
            text: qsTr("Use default")
            accessibleName: qsTr("This Channel Uses the Default Passband")
            onClicked: sheet.rowAction(sheet.bridge.scanRowEditInherit)
        }
        OutlineButton {
            objectName: "passbandRowReset"
            visible: (metrics.scanRowEdited & sheet.bridge.scanRowFieldWidth) !== 0
            text: qsTr("List value")
            accessibleName: qsTr("This Channel Back to Its List Passband")
            onClicked: sheet.rowAction(sheet.bridge.scanRowEditReset)
        }
    }
    // A row with its own width keeps it while on air: the controls edit the default.
    Text {
        objectName: "passbandRowNote"
        width: parent.width
        visible: sheet.rowOverride && !sheet.rowWidth
        text: qsTr("This channel sets its own passband; changes here apply to the default (%1).").arg(
            sheet.settingText(sheet.rowAm, sheet.settingOf(sheet.rowAm)))
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(13)
    }
    OutlineButton {
        objectName: "passbandClose"
        width: parent.width
        height: Math.max(48, implicitHeight)
        text: qsTr("Close")
        onClicked: sheet.visible = false
    }
}
