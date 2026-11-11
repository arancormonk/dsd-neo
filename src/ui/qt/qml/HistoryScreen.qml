// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// The call log, searchable like a phone's call history. Day groups, dimmed and
// tagged encrypted rows, filter pills for system and call type.
Item {
    id: screen

    property bool showTitle: true

    // Hand off pending text before another History instance can accept input.
    // Otherwise this screen's older timer could overwrite the new editor.
    function flushPendingSearch() {
        if (!searchDebounce.running)
            return;
        searchDebounce.stop();
        historyView.filterText = search.text;
    }
    onVisibleChanged: {
        if (!visible) {
            flushPendingSearch();
            // A picker still out ends with the screen, as a cancel; a file
            // already being written finishes.
            exportPicker.cancel();
            return;
        }
        forgetHeardExportMessage();
        announceExportLater.restart();
    }
    onEnabledChanged: if (!enabled) flushPendingSearch()

    // ---- Export…: the rows the list shows, as a CSV file ----
    // Both History screens share historyView and the one `historyExport`, so
    // each keeps its own export: pending from the press until the picker backs
    // out or the file is written. The exporter holds the file it built at the
    // press under `exportToken`.
    property int exportToken: 0
    property int exportCount: 0
    property int exportTotal: 0
    property string exportReference: ""
    readonly property bool exportPending: exportToken > 0
    // What the last export said, under the buttons: {text, error, heard}.
    // `heard` once read out; a heard message is gone on the next visit, an
    // unheard one waits to be read.
    property var exportMessage: ({ text: "", error: false, heard: false })
    // Navigation.announce()'s record of what was read out.
    property string announced: ""
    property int announcements: 0
    function beginExport() {
        if (exportPending || historyExport.busy)
            return;
        // Text typed less than the debounce ago is part of what the list shows.
        flushPendingSearch();
        setExportMessage("", false);
        // Read before the exporter holds a file: a failure here leaves nothing pending.
        var stamp = Qt.formatDate(new Date(savedSystems.realtimeNowMs()), "yyyy-MM-dd");
        // The exporter builds the whole file now; a list that changes later does not change it.
        var prepared = historyExport.prepare(historyView) || {};
        var token = Number(prepared.token) || 0;
        if (token <= 0) {
            if ((Number(prepared.count) || 0) === 0)
                setExportMessage(qsTr("No entries to export."), false);
            else
                setExportMessage(qsTr("The export couldn't be written. Try another location."), true);
            return;
        }
        exportToken = token;
        exportCount = Number(prepared.count) || 0;
        exportTotal = Number(prepared.total) || 0;
        exportReference = "";
        // Always a lowercase .csv: Android's save picker adds the type's own
        // extension to a name whose extension differs in case.
        exportPicker.suggestedName = qsTr("Call history %1").arg(stamp) + ".csv";
        exportPicker.open();
    }
    function exportPicked(reference) {
        if (!exportPending) {
            // Not wanted any more: nothing will write a Download-folder row.
            if (exportPicker.savedToDownloads)
                decoderHost.discardPendingDownload(reference);
            return;
        }
        var token = exportToken;
        exportReference = reference;
        if (historyExport.save(token, reference))
            return;
        // Refused (a stale token, a write still running): the exporter has let
        // the Download-folder row go, and no result will follow.
        releaseExport(token);
        setExportMessage(qsTr("The export couldn't be written. Try another location."), true);
    }
    function exportPickCancelled() {
        if (!exportPending)
            return;
        var token = exportToken;
        historyExport.discard(token);
        releaseExport(token);
    }
    function exportPickUnavailable(message) {
        if (!exportPending)
            return;
        exportPickCancelled();
        setExportMessage(message, true);
    }
    // Let go of export @p token, only while it is still the one held.
    function releaseExport(token) {
        if (token !== exportToken)
            return;
        exportToken = 0;
        exportCount = 0;
        exportTotal = 0;
        exportReference = "";
    }
    // CallHistoryExporter::finished(): a result for an export this screen does
    // not hold (an older one, the other History screen's) changes nothing.
    function exportFinished(token, ok) {
        if (token <= 0 || token !== exportToken)
            return;
        var count = exportCount, total = exportTotal;
        // The Download folder's name for a save no picker took ("" otherwise).
        var saved = ok && exportPicker.savedToDownloads ? decoderHost.savedDocumentName(exportReference) : "";
        releaseExport(token);
        setExportMessage(ok ? exportedText(count, total, saved) : qsTr("The export couldn't be written. Try another location."), !ok);
    }
    // The name goes in last, so a % in it is never read as a place marker.
    function exportedText(count, total, saved) {
        var shown = Util.countText(count);
        if (count < total) {
            if (saved.length > 0)
                return qsTr("Exported %1 of %2 entries to Download/DSD-neo as %3.").arg(shown).arg(Util.countText(total)).arg(saved);
            return qsTr("Exported %1 of %2 entries. The search and filters left out the rest.").arg(shown).arg(Util.countText(total));
        }
        if (saved.length > 0)
            return count === 1 ? qsTr("Exported 1 entry to Download/DSD-neo as %1.").arg(saved)
                : qsTr("Exported %1 entries to Download/DSD-neo as %2.").arg(shown).arg(saved);
        return count === 1 ? qsTr("Exported 1 entry.") : qsTr("Exported %1 entries.").arg(shown);
    }
    function setExportMessage(text, error) {
        exportMessage = { text: text, error: error === true, heard: false };
        announceExportLater.restart();
    }
    function forgetHeardExportMessage() {
        if (exportMessage.heard)
            setExportMessage("", false);
    }
    // Read out once, as Signal search reads its export lines: only while the
    // line is on the visible screen with nothing over it (a detail sheet, the
    // Clear confirmation, the filter choices). One that arrives while the
    // screen is hidden or covered is read when the line can be reached.
    function announceExport() {
        var message = exportMessage;
        if (message.heard || !message.text.length || !visible || !Navigation.allows(exportLine) || !exportLine.visible)
            return;
        if (Navigation.announce(screen, exportLine, message.text))
            exportMessage = Object.assign({}, message, { heard: true });
    }
    readonly property bool exportLineReachable: Navigation.allows(screen)
    onExportLineReachableChanged: announceExportLater.restart()
    // Once every binding has the new message; a Timer, which dies with the
    // screen, rather than Qt.callLater.
    Timer {
        id: announceExportLater
        interval: 0
        onTriggered: screen.announceExport()
    }
    Connections {
        target: historyExport
        function onFinished(token, ok) {
            screen.exportFinished(token, ok);
        }
    }
    DocumentPicker {
        id: exportPicker
        objectName: "historyExportPicker"
        mode: "save"
        mimeType: "text/csv"
        nameFilters: [qsTr("CSV files") + " (*.csv)"]
        defaultSuffix: "csv"
        onAccepted: function (reference, displayName) {
            screen.exportPicked(reference);
        }
        onRejected: screen.exportPickCancelled()
        onUnavailable: function (reason, message) {
            screen.exportPickUnavailable(message);
        }
    }

    // The field's pending text goes too: the filter ignores a search it already
    // has, so clearing only the model would leave typed text that the debounce
    // then applies again.
    function clearFilters() {
        searchDebounce.stop();
        search.text = "";
        historyView.filterText = "";
        historyView.filterSystem = "";
        historyView.filterKind = 0;
    }

    // Cycles: 0 everything, 1 clear calls, 2 encrypted calls, 3 messages
    // (SMS, GPS positions, data and control notices).
    readonly property var kindLabels: [qsTr("All activity"), qsTr("Unencrypted calls"), qsTr("Encrypted"), qsTr("Messages")]

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    Column {
        id: chrome

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.screenPadding
        spacing: Theme.gap
        // TapHandlers never take exclusive grabs, so with the confirm sheet up a
        // tap on the dimmed search field would still pop the keyboard, a dimmed
        // pill would cycle its filter, and the dimmed Clear label would re-open
        // the sheet — same class of tap-through the list below guards against.
        enabled: !confirmClear.visible

        // Title on the left, Export… and Clear on the right, on one line when
        // they fit. When they do not (a narrow phone, a large text size), the
        // buttons take their own line under the title, still on the right,
        // rather than running over it.
        Item {
            id: header

            readonly property real titleHeight: screen.showTitle ? Math.max(48, Theme.fontSize(24) + 20) : 0
            readonly property bool buttonsShown: callHistory.count > 0
            readonly property bool stacked: screen.showTitle && buttonsShown
                                             && historyTitle.implicitWidth + Theme.gap + headerButtons.implicitWidth > width

            width: parent.width
            height: stacked ? titleHeight + headerButtons.implicitHeight
                            : Math.max(titleHeight, buttonsShown ? headerButtons.implicitHeight : 0)
            visible: height > 0

            ReadableText {
                id: historyTitle
                objectName: "historyTitle"
                heading: true
                visible: screen.showTitle
                anchors.left: parent.left
                y: ((header.stacked ? header.titleHeight : header.height) - height) / 2
                text: qsTr("History")
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(24)
                font.weight: Font.Bold
                font.letterSpacing: -0.24
                color: Theme.textPrimary
            }

            Row {
                id: headerButtons

                anchors.right: parent.right
                y: header.stacked ? header.titleHeight : (header.height - height) / 2
                spacing: 10
                visible: header.buttonsShown

                OutlineButton {
                    objectName: "historyExportButton"
                    text: qsTr("Export…")
                    accessibleName: qsTr("Export history as a CSV file")
                    // Held while this screen's picker is out, while either
                    // History screen's file is being written, and while the
                    // list shows no row (its empty state says why).
                    enabled: !screen.exportPending && !historyExport.busy && historyView.count > 0
                    onClicked: screen.beginExport()
                }

                OutlineButton {
                    id: clearHistory
                    objectName: "clearHistoryButton"
                    text: qsTr("Clear")
                    onClicked: confirmClear.open()
                }
            }
        }

        // The last export's result, under the buttons that started it. In the
        // header rather than the list, which holds no rows while hidden.
        ExportMessage {
            id: exportLine
            objectName: "historyExportMessage"
            message: screen.exportMessage
        }

        PlexTextField {
            id: search
            objectName: "historySearch"
            Binding {
                target: search
                property: "text"
                value: historyView.filterText
            }

            width: parent.width
            label: qsTr("Search talkgroup, unit or frequency")
            // The rule a bare number cannot meet: 851 is a talkgroup search, so a
            // frequency is told apart by its decimal point (frequency_query.h).
            // Helper text rather than a placeholder, which hides on focus and is
            // not announced.
            hint: qsTr("Frequencies need a decimal point: 851.0125")
            inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
            // Debounced: every keystroke otherwise re-evaluates the filter over
            // the full log, and on a phone that stutters the keyboard.
            onTextChanged: {
                if (text !== historyView.filterText)
                    searchDebounce.restart();
                else
                    searchDebounce.stop();
            }

            Timer {
                id: searchDebounce
                interval: 250
                onTriggered: historyView.filterText = search.text
            }
        }

        // Issue #708 (B6): a save that cannot reach the disk is retried on its own;
        // until one lands, the reader is told the log is not safe yet.
        ReadableText {
            objectName: "historySaveFailing"
            width: parent.width
            visible: callHistory.saveFailing === true
            text: qsTr("History isn't being saved right now. It keeps trying, but calls since the last save are lost if DSD-neo closes. Free up space if the phone is full.")
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(13)
            color: Theme.alert
            Accessible.role: Accessible.AlertMessage
        }

        Flow {
            width: parent.width
            spacing: 10

            // Keyed on the filter string itself, never an index: the label array
            // reorders as calls land, and a frozen index would let the pill show
            // one system while the model filters another.
            FilterPill {
                text: historyView.filterSystem.length === 0 ? qsTr("All systems") : historyView.filterSystem
                active: historyView.filterSystem.length > 0
                onClicked: {
                    var labels = [qsTr("All systems")].concat(callHistory.systemLabels);
                    filterChoices.open(qsTr("System"), labels, function (index) {
                        historyView.filterSystem = index === 0 ? "" : labels[index];
                    });
                }
            }

            FilterPill {
                text: screen.kindLabels[historyView.filterKind]
                active: historyView.filterKind !== 0
                onClicked: filterChoices.open(qsTr("Activity type"), screen.kindLabels, function (index) {
                    historyView.filterKind = index;
                })
            }
        }
    }

    ListView {
        id: logList

        // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
        objectName: "callLogList"

        anchors.top: chrome.bottom
        anchors.topMargin: 8
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        // TapHandlers never take exclusive grabs, so a tap on the confirm sheet
        // would also scroll the list under the overlay.
        enabled: !confirmClear.visible
        // Set by logPlace below, only while the log is visible (issue #782).
        model: null

        section.property: "dayLabel"
        section.delegate: MicroLabel {
            required property string section

            width: ListView.view ? ListView.view.width : 0
            height: 34
            leftPadding: Theme.screenPadding
            verticalAlignment: Text.AlignBottom
            bottomPadding: 8
            text: section
        }

        delegate: CallRow {
            id: callRow

            // Whether a frequency search found this row by its frequency. Not
            // every row it lists was: the same text can match a channel name
            // ("851.0125 backup" heard on 460 MHz). Reading frequencyQuery
            // re-runs this on each search change.
            readonly property bool freqMatched: historyView.frequencyQuery && model.freqHz > 0 && historyView.frequencyMatches(model.freqHz)
            // The row's identity, for logPlace.
            readonly property var rowUid: model.uid

            interactive: true
            width: ListView.view.width
            name: model.name
            metaText: {
                // A notice row's payload (the SMS body, the GPS string) is its
                // meta line; identity only where it exists. The access code and
                // the frequency sit where a call row puts them.
                if (model.kind === 1) {
                    var parts = [];
                    if (model.tg > 0)
                        parts.push("TG " + model.tg);
                    if (model.src > 0 || model.srcName)
                        parts.push("SRC " + Util.sourceText(model.src, model.srcName));
                    if (model.accessCode.length > 0)
                        parts.push(model.accessCode);
                    if (model.detail.length > 0)
                        parts.push(model.detail);
                    if (model.channel.length > 0 && model.channel !== model.name)
                        parts.push(model.channel);
                    if (callRow.freqMatched)
                        parts.unshift(Util.fmtMhz(model.freqHz));
                    else if (model.freqHz > 0)
                        parts.push(Util.fmtMhz(model.freqHz));
                    return parts.length > 0 ? parts.join(" · ") : qsTr("data message");
                }
                // "encrypted" states what the call was, nothing more: whether it was
                // skipped depended on the lockout toggle and loaded keys at the time,
                // which a logged row does not know. The duration is measured either way.
                // A zero talkgroup is "none decoded", not an identity — the same
                // rule the monitor's hero applies — so it is not printed under a
                // row that the scan channel already names.
                var meta = [];
                if (model.emergency)
                    meta.push(qsTr("EMERGENCY"));
                if (model.tg > 0)
                    meta.push("TG " + model.tg);
                if (model.src > 0 || model.srcName)
                    meta.push("SRC " + Util.sourceText(model.src, model.srcName));
                // The code the call's carrier was confirmed on ("CC 1", "NAC 293"),
                // which belongs with the ids that name the call. An analog
                // reception has none: its tone ("CTCSS 100.0 Hz") takes the place.
                var code = model.analog ? model.receivedTone : model.accessCode;
                if (code.length > 0)
                    meta.push(code);
                if (model.enc)
                    meta.push(qsTr("encrypted"));
                if (model.durationSecs >= 0)
                    meta.push(Util.fmtDuration(model.durationSecs));
                // Where the call was heard, when that is not already the name
                // above it — again the hero's rule, so both screens answer the
                // question the same way.
                if (model.channel.length > 0 && model.channel !== model.name)
                    meta.push(model.channel);
                // The frequency closes the line: the least of these on a phone,
                // so it is the first thing the elide takes. Except on a row a
                // frequency search found, which leads with it (ahead of EMERGENCY,
                // which the row's badge still shows): the elide would otherwise
                // take the very value the reader searched for.
                if (callRow.freqMatched)
                    meta.unshift(Util.fmtMhz(model.freqHz));
                else if (model.freqHz > 0)
                    meta.push(Util.fmtMhz(model.freqHz));
                return meta.join(" · ");
            }
            rightText: model.timeText
            enc: model.enc
            emergency: model.emergency
            hasRecording: model.hasRecording
            // The frequency a search found is not added: the meta line, read as
            // the row's description, already leads with it.
            accessibleName: model.name + ", " + model.timeText
            onActivated: detailSheet.open(callHistory.recordFor(model.uid))
        }
    }

    ChoiceSheet {
        id: filterChoices
        objectName: "historyFilterChoices"
    }
    HistoryDetailSheet {
        id: detailSheet
        objectName: "historyDetailSheet"
    }

    // Keeps the newest call in view for a reader parked at the top of the log.
    FollowLatest {
        id: follow
        list: logList
    }

    // Hidden, the log holds no rows (issue #782): a session runs on the monitor
    // with this screen under it, and every row built for a call landing there is
    // announced to accessibility clients as hidden. The search and filters live
    // in historyView and stay set. Shown again, a reader who was on the latest
    // call is on it again, and one reading further back is back on the same row
    // at the same height.
    KeepPlace {
        id: logPlace
        list: logList
        model: historyView
        atStart: follow.atLatest
        keyOf: function (row) {
            return row.rowUid;
        }
        rowOf: function (uid) {
            var source = callHistory.rowForUid(uid);
            return source < 0 ? -1 : historyView.mapFromSource(callHistory.index(source, 0)).row;
        }
    }

    // A new search or filter is a new question, answered from the top — which is
    // a different rule from the pin above, not the same one: the pin asks where
    // the reader left the list and holds their place when they are reading back,
    // while a filter change makes that place the answer to a question they are no
    // longer asking. The view keeps its scroll position across the change, so
    // without this a reader who had scrolled back lands somewhere inside the new
    // result, with the newest matching calls off-screen above and nothing to say
    // they are there. It only rights itself when the result is shorter than the
    // viewport, where the Flickable's own bounds drag it back.
    //
    // The pin cannot carry it: a filter change lands no call, so nothing would
    // fire the pin, and while no session is running no call lands at all.
    Connections {
        target: historyView

        // Deferred for the same reason the pin is: inside the signal the view is
        // still applying the filter's row changes, and a position asserted there
        // does not survive them. Setting several filters at once (the empty
        // state's Clear filters button sets three) coalesces into one call, and
        // repositioning an already-topped list is a no-op either way.
        //
        // A hidden log has no rows to reposition, so the place it will come
        // back to is dropped as well (issue #782). That covers the filter the
        // other History screen shares, and a search flushed as this screen hides
        // (flushPendingSearch), which runs after the log noted its place.
        function onFilterChanged() {
            logPlace.forget();
            Qt.callLater(logList.positionViewAtBeginning);
        }
    }

    // A sibling of the view, not a child: ListView reparents declared children
    // into its contentItem, where `parent.count` is undefined and the empty state
    // would never show.
    Column {
        objectName: "callLogEmptyState"
        anchors.centerIn: logList
        width: logList.width - 2 * Theme.screenPadding
        // The filter's count, not the view's: the view holds no rows while hidden.
        visible: historyView.count === 0
        spacing: 8

        // An empty filtered view and an empty log are different situations: the
        // first must say the calls are hidden, not gone, or the filter pill left
        // active yesterday reads as a decoder that stopped logging.
        readonly property bool filtered: callHistory.count > 0
        // Each count spelled as a whole sentence rather than one %n plural: the
        // app installs no QTranslator, and an untranslated %n form substitutes the
        // number but never picks a plural form — this read "1 logged call(s) are
        // hidden". Two strings also let the verb agree.
        readonly property string hiddenText: callHistory.count === 1 ? qsTr("1 logged call is hidden by the search or filters.") : qsTr("%1 logged calls are hidden by the search or filters.").arg(callHistory.count)

        ReadableText {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: parent.filtered ? qsTr("No matching activity") : qsTr("No calls yet")
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(16)
            font.weight: Font.DemiBold
            color: Theme.textSecondary
        }

        ReadableText {
            objectName: "logEmptyDetail"

            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: parent.filtered ? parent.hiddenText : qsTr("Start listening on a system and every call lands here.")
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(13)
            color: Theme.textSubdued
            wrapMode: Text.Wrap
        }

        Item {
            width: 1
            height: 6
            visible: parent.filtered
        }

        OutlineButton {
            objectName: "historyClearFilters"
            anchors.horizontalCenter: parent.horizontalCenter
            visible: parent.filtered
            width: Math.min(parent.width, 220)
            text: qsTr("Clear filters")
            onClicked: screen.clearFilters()
        }
    }

    // Clearing is destructive and irreversible (the persisted log goes too),
    // so it uses the shared destructive confirmation dialog.
    ConfirmDialog {
        id: confirmClear
        objectName: "clearHistoryConfirm"
        title: qsTr("Clear call history?")
        message: qsTr("Every logged call and message is removed. A call playing right now still gets logged.")
        confirmText: qsTr("Clear history")
        destructive: true
        onConfirmed: callHistory.clearAll()
    }
}
