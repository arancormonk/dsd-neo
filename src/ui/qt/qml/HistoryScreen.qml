// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// The call log, searchable like a phone's call history. Day groups, dimmed and
// tagged encrypted rows, filter pills for system and call type.
Item {
    id: screen

    property bool showTitle: true
    // Issue #901 (D6): Compact view; the log's rows follow it.
    readonly property bool compactView: typeof prefs !== "undefined" && !!prefs && prefs.compactView === true
    // The height every row of the log has at its fonts and density.
    readonly property real rowHeight: LineBoxes.callRowHeight(compactView)
    // Issue #800: the map (its kept tracks stay reachable after Stop), and a
    // row's "Show on map" with the sheet to return to.
    signal openMap
    signal showOnMap(var record, var sheet)
    // The keyboard stop of the call row logged under @p uid, brought into
    // view: where a sheet opened again by uid (Back from the map) returns the
    // focus. The row it was opened from can be gone by then: a covered list
    // holds no rows (issue #782) and builds new ones. Null when not listed.
    function rowFocusFor(uid) {
        var row = logPlace.rowOf(uid);
        if (row < 0 || !logList.model)
            return null;
        // The row placed here is the answer now: a return to the top that a
        // filter change still has queued would take it off screen again.
        logToStart.stop();
        settleListLayout();
        logList.positionViewAtIndex(row, ListView.Contain);
        var item = logList.itemAtIndex(row);
        return item ? item.node : null;
    }
    // The list's place and rows as the next frame shows them, for a row to be
    // brought into view against: the list hangs under the header, whose
    // Column and the positioners in it (the filters' Flow, each pill's Row)
    // lay out on their next polish. A header that changed since the last
    // frame (the log's first calls offer Export… and Clear on a line of their
    // own) would otherwise leave a row contained against the old height, and
    // cut off once the list moves.
    function settleListLayout() {
        Theme.settlePositioners(chrome);
        logList.forceLayout();
    }
    // Back from the map when the row it showed has left the log: the focus
    // goes to the list, where the row was.
    function focusList() {
        if (!logList.visible || !Navigation.allows(logList))
            return false;
        logList.forceActiveFocus();
        return true;
    }

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
            // Issue #909 (B5): a row's sheet ends with the screen, as the
            // Monitor's do: the session's History hidden under a Stop came
            // back with the sheet of a call from before. A "Show on map" has
            // closed its sheet already, and Back from the map opens it again
            // by its row's uid.
            detailSheet.visible = false;
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
    // Main's savedSystems, whose clock dates an export's suggested name.
    property var systems: typeof savedSystems !== "undefined" ? savedSystems : null
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
        var stamp = Qt.formatDateTime(new Date(screen.systems.realtimeNowMs()), "yyyy-MM-dd");
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
                return Util.fill(qsTr("Exported %1 of %2 entries to Download/DSD-neo as %3."), [shown, Util.countText(total), saved]);
            return Util.fill(qsTr("Exported %1 of %2 entries. The search and filters left out the rest."), [shown, Util.countText(total)]);
        }
        if (saved.length > 0)
            return count === 1 ? qsTr("Exported 1 entry to Download/DSD-neo as %1.").arg(saved)
                : Util.fill(qsTr("Exported %1 entries to Download/DSD-neo as %2."), [shown, saved]);
        return count === 1 ? qsTr("Exported 1 entry.") : qsTr("Exported %1 entries.").arg(shown);
    }
    function setExportMessage(text, error) {
        exportMessage = { text: text, error: error === true, heard: false };
        announceExportLater.restart();
        if (text.length > 0)
            revealExportLater.restart();
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

    // ---- Play all (issue #909): the recorded calls the list shows ----
    // Main's playlistStatus, which the bar at the bottom shows (null: no bar).
    property var playlistStatus: null
    readonly property bool canPlayAll: decoderHost.playbackSupported === true
    // Oldest first, from what the list shows at the press; the bar takes it
    // from there. A press refused says why on the header's message line,
    // which Export shares (the newer replaces), and what played plays on.
    function playAll() {
        if (!canPlayAll || historyView.count === 0)
            return;
        flushPendingSearch();
        setExportMessage("", false);
        var outcome = historyPlaylist.start(0) || {};
        var result = String(outcome.result || "");
        if (result === "accepted")
            return;
        if (result === "none")
            setExportMessage(qsTr("None of the shown calls has a recording that can be played now."), false);
        else if (result === "unsupported")
            setExportMessage(qsTr("Recordings can't be played on this device."), true);
        else
            setExportMessage(qsTr("Couldn't start playing. Try again."), true);
    }
    // Clear history ends a History playlist that plays, as the bar's ✕ does,
    // and lets go of an ended one's result: its rows are gone.
    function endPlaylist() {
        if (!playlistStatus)
            return;
        if (playlistStatus.live)
            playlistStatus.stop();
        else
            playlistStatus.dismiss();
    }
    // The bar's Show in list: the row logged under @p uid, looked up through
    // the list's filter now, scrolled into view and its sheet opened (which
    // follows what plays by itself). False when the list does not show it.
    function showPlaying(uid) {
        // Text typed less than the debounce ago is part of what the list shows.
        flushPendingSearch();
        var row = logPlace.rowOf(uid);
        if (row < 0 || !logList.model)
            return false;
        // The flush's filter change queued the list's return to its top,
        // which would run after the row is placed: the row supersedes it.
        logToStart.stop();
        settleListLayout();
        logList.positionViewAtIndex(row, ListView.Contain);
        detailSheet.open(callHistory.recordFor(uid));
        return true;
    }

    // Cycles: 0 everything, 1 clear calls, 2 encrypted calls, 3 messages
    // (SMS, GPS positions, data and control notices).
    readonly property var kindLabels: [qsTr("All activity"), qsTr("Unencrypted calls"), qsTr("Encrypted"), qsTr("Messages")]

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    // The list keeps at least this share of the screen. The header above it
    // takes its own height up to the rest, and scrolls inside that room when it
    // is taller: at a large text size with the export's result or the save
    // warning shown it is taller than a phone screen, and as one fixed column it
    // pushed the list off the bottom and its last controls out of reach. A
    // header that fits does not move.
    //
    // Issue #909: the now-playing bar docks at the bottom and the list ends
    // above it, so the bar never covers the last row. The bar's height comes
    // out of the list's share, down to one whole row of list (and its day's
    // heading) above it, before the header gives any room up. While the
    // keyboard is up (a search being typed) the bar hides and History is laid
    // out as it is without one; the notification keeps the playlist's
    // controls, and its events wait.
    //
    // That reservation never takes the whole header: in a short window at a
    // large text size the bar and a row of list leave it nothing, and Search,
    // the filters, Export and Clear could not be reached without ending the
    // playlist. While the bar takes room, the header keeps its top row (the
    // title's line, with Map and Play all), or at least a 48 px control row,
    // within what the bar leaves, and scrolls for the rest. Squeezed below its
    // top row, it scrolls that row's controls whole into view (alignTopRow()).
    // With no bar the list keeps its 40% share, as before the playlist.
    readonly property real listMinHeight: Math.ceil(height * 0.4)
    // By the keyboard's visibility, not its overlap: a window Android has
    // resized above the keyboard has none, and the bar would keep taking
    // room from the list it already shortened.
    readonly property bool keyboardUp: Theme.keyboardShown()
    readonly property real barHeight: nowPlaying.shown && !keyboardUp ? nowPlaying.height : 0
    // A day's heading over the rows (the list's section delegate).
    readonly property real sectionHeight: 34
    readonly property real listShare: Math.max(listMinHeight, barHeight > 0 ? barHeight + sectionHeight + rowHeight : 0)
    readonly property real topRowBottom: chrome.y + headerButtons.places.topRow
    readonly property real headerFloor: barHeight > 0 ? Math.min(headerScroll.contentHeight,
                                                                 Math.max(Theme.minimumTouchSize, topRowBottom),
                                                                 Math.max(0, height - barHeight - logList.anchors.topMargin))
                                                      : 0

    PlexFlickable {
        id: headerScroll
        objectName: "historyHeaderScroll"
        accessibleName: qsTr("History search and filters")

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: Math.min(contentHeight, Math.max(screen.headerFloor, screen.height - screen.listShare - logList.anchors.topMargin))
        contentWidth: width
        // A few pixels under the filters for their focus ring, which the clip
        // would otherwise cut; the list's top margin gives them back.
        contentHeight: chrome.y + chrome.height + 4
        clip: true
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        // A smaller room (the keyboard up, a larger text size) keeps the
        // focused control in view, or (nothing focused there) the top row's.
        onHeightChanged: {
            revealFocusLater.restart();
            alignTopRowLater.restart();
        }
        // TapHandlers never take exclusive grabs, so with the confirm sheet up a
        // tap on the dimmed search field would still pop the keyboard, a dimmed
        // pill would cycle its filter, and the dimmed Clear label would re-open
        // the sheet — same class of tap-through the list below guards against.
        // Nor does the header scroll under the sheet.
        enabled: !confirmClear.visible

        Column {
            id: chrome

            x: Theme.screenPadding
            y: Theme.screenPadding
            width: headerScroll.width - 2 * Theme.screenPadding
            spacing: Theme.gap

            // Title on the left, the buttons (Map, Play all, Export…, Clear) on the
            // right, on one line when they fit. When they do not (a narrow phone,
            // a large text size), the glyphs stay on the title's line while they
            // fit there and Export… and Clear take a line under it, still on the
            // right, rather than running over the title.
            Item {
                id: header

                readonly property real titleHeight: screen.showTitle ? Math.max(48, Theme.fontSize(24) + 20) : 0

                width: parent.width
                height: headerButtons.places.height
                visible: height > 0

                ReadableText {
                    id: historyTitle
                    objectName: "historyTitle"
                    heading: true
                    visible: screen.showTitle
                    anchors.left: parent.left
                    y: (headerButtons.places.titleLine - height) / 2
                    text: qsTr("History")
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(24)
                    font.weight: Font.Bold
                    font.letterSpacing: -0.24
                    color: Theme.textPrimary
                }

                // Issue #909: a flow set against the right edge, in two groups
                // kept whole while each fits a line: the glyphs (Map, Play all)
                // and the text buttons (Export…, Clear). Everything on the
                // title's line when it fits beside the title; else the glyphs
                // there and the text buttons on the lines under it; else the
                // title alone and the groups under it. The buttons keep their
                // order, and the keyboard's. Placed from the buttons' widths and
                // whether each is offered, never from their positions or the
                // screen's visibility, so a hidden screen keeps its layout.
                Item {
                    id: headerButtons

                    readonly property var controls: [historyMapButton, historyPlayAll, historyExportButton, clearHistory]
                    readonly property real spacing: 10
                    anchors.fill: parent

                    // {height, titleLine (the first line's height), topRow (the bottom of the first line with
                    // controls on it), at: [{x, y}, ...] per control, in its order}.
                    readonly property var places: {
                        var room = header.width;
                        var titleRoom = screen.showTitle ? room - historyTitle.implicitWidth - Theme.gap : room;
                        var controls = headerButtons.controls, spacing = headerButtons.spacing;
                        var groups = [[0, 1], [2, 3]].map(function (group) {
                            return group.filter(function (index) {
                                return controls[index].offered;
                            });
                        }).filter(function (group) {
                            return group.length > 0;
                        });
                        // The glyphs' 48 px targets sit side by side, as a top
                        // bar's icon actions do; framed buttons keep a gap.
                        var gapBefore = function (list, n) {
                            return n === 0 ? 0 : list[n] < 2 && list[n - 1] < 2 ? 0 : spacing;
                        };
                        var widthOf = function (list) {
                            var total = 0;
                            for (var n = 0; n < list.length; n++)
                                total += gapBefore(list, n) + controls[list[n]].implicitWidth;
                            return total;
                        };
                        var heightOf = function (list) {
                            var tallest = 0;
                            for (var index of list)
                                tallest = Math.max(tallest, controls[index].implicitHeight);
                            return tallest;
                        };
                        var all = [].concat.apply([], groups);
                        var first = [], rest = groups;
                        if (widthOf(all) <= titleRoom) {
                            first = all;
                            rest = [];
                        } else if (screen.showTitle && groups.length > 1 && widthOf(groups[0]) <= titleRoom) {
                            first = groups[0];
                            rest = groups.slice(1);
                        }
                        // The rest from the end, each line as wide as the screen:
                        // a group whole while it fits one, else button by button.
                        var units = [];
                        for (var group of rest) {
                            if (widthOf(group) <= room)
                                units.push(group);
                            else
                                for (var index of group)
                                    units.push([index]);
                        }
                        var lines = [], line = [];
                        for (var u = units.length - 1; u >= 0; u--) {
                            if (line.length > 0 && widthOf(units[u].concat(line)) > room) {
                                lines.unshift(line);
                                line = [];
                            }
                            line = units[u].concat(line);
                        }
                        if (line.length > 0)
                            lines.unshift(line);
                        var at = controls.map(function () {
                            return {x: 0, y: 0};
                        });
                        var place = function (list, top, height) {
                            var left = room - widthOf(list);
                            for (var n = 0; n < list.length; n++) {
                                var index = list[n];
                                left += gapBefore(list, n);
                                at[index] = {x: left, y: top + (height - controls[index].implicitHeight) / 2};
                                left += controls[index].implicitWidth;
                            }
                        };
                        var titleLine = Math.max(header.titleHeight, heightOf(first));
                        place(first, 0, titleLine);
                        var top = titleLine;
                        var topRow = first.length > 0 ? titleLine : 0;
                        for (var l = 0; l < lines.length; l++) {
                            if (top > 0)
                                top += spacing;
                            var lineHeight = heightOf(lines[l]);
                            place(lines[l], top, lineHeight);
                            top += lineHeight;
                            if (topRow === 0)
                                topRow = top;
                        }
                        return {height: top, titleLine: titleLine, topRow: topRow, at: at};
                    }

                    // A glyph, as the top bars' other icon actions.
                    MapControlButton {
                        id: historyMapButton
                        readonly property bool offered: true
                        objectName: "historyMapButton"
                        x: headerButtons.places.at[0].x
                        y: headerButtons.places.at[0].y
                        framed: false
                        shape: "map"
                        accessibleName: qsTr("Map")
                        accessibleDescription: qsTr("Positions radios sent, on a map")
                        onClicked: screen.openMap()
                    }

                    // Issue #909: beside Map and drawn as it is; on Export's rule,
                    // where the host has a player.
                    MapControlButton {
                        id: historyPlayAll
                        readonly property bool offered: callHistory.count > 0 && screen.canPlayAll
                        objectName: "historyPlayAll"
                        visible: offered
                        x: headerButtons.places.at[1].x
                        y: headerButtons.places.at[1].y
                        framed: false
                        shape: "play"
                        accessibleName: qsTr("Play all")
                        accessibleDescription: qsTr("Plays the recorded calls shown, oldest first")
                        // While the list shows rows (its empty state says why not).
                        enabled: historyView.count > 0
                        onClicked: screen.playAll()
                    }

                    OutlineButton {
                        id: historyExportButton
                        readonly property bool offered: callHistory.count > 0
                        objectName: "historyExportButton"
                        visible: offered
                        x: headerButtons.places.at[2].x
                        y: headerButtons.places.at[2].y
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
                        readonly property bool offered: callHistory.count > 0
                        objectName: "clearHistoryButton"
                        visible: offered
                        x: headerButtons.places.at[3].x
                        y: headerButtons.places.at[3].y
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
                    objectName: "historySystemFilter"
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
                    objectName: "historyActivityFilter"
                    text: screen.kindLabels[historyView.filterKind]
                    active: historyView.filterKind !== 0
                    onClicked: filterChoices.open(qsTr("Activity type"), screen.kindLabels, function (index) {
                        historyView.filterKind = index;
                    })
                }
            }
        }
    }

    // A header that scrolls ends on a hairline, so a line it cuts reads as
    // cut by the edge rather than run into the list.
    Rectangle {
        anchors.top: headerScroll.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.screenPadding
        anchors.rightMargin: Theme.screenPadding
        height: 1
        visible: headerScroll.interactive
        color: Theme.divider
    }

    // The control that takes the focus (Tab, a screen reader's step) scrolls
    // into the header's view, as it does on the other scrolling screens.
    function revealHeaderFocus() {
        var window = screen.Window.window;
        if (!window || !visible)
            return;
        Theme.revealFocus(headerScroll, chrome, window.activeFocusItem);
        // Clear filters reached by keyboard scrolls into the empty state's view.
        Theme.revealFocus(emptyArea, emptyState, window.activeFocusItem);
    }
    // A header squeezed below its top row (a short window, the bar docked)
    // with nothing of it focused shows that row's controls whole, its title's
    // margin above them scrolled away.
    function alignTopRow() {
        var window = screen.Window.window;
        for (var item = window ? window.activeFocusItem : null; item; item = item.parent) {
            if (item === chrome)
                return;
        }
        if (visible && headerScroll.height < topRowBottom)
            Theme.revealFocus(headerScroll, chrome, historyMapButton);
    }
    Timer {
        id: alignTopRowLater
        interval: 0
        onTriggered: screen.alignTopRow()
    }
    Connections {
        target: screen.Window.window
        function onActiveFocusItemChanged() {
            if (screen.visible)
                revealFocusLater.restart();
        }
    }
    Timer {
        id: revealFocusLater
        interval: 0
        onTriggered: screen.revealHeaderFocus()
    }
    // A new result scrolls into the header's view, once laid out.
    Timer {
        id: revealExportLater
        interval: 0
        onTriggered: {
            if (screen.visible && exportLine.visible)
                Theme.revealFocus(headerScroll, chrome, exportLine);
        }
    }

    ListView {
        id: logList

        // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
        objectName: "callLogList"

        anchors.top: headerScroll.bottom
        anchors.topMargin: 4
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        // Above the now-playing bar (issue #909).
        anchors.bottomMargin: screen.barHeight
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
            height: screen.sectionHeight
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
            compact: screen.compactView
            name: model.name
            metaText: {
                // A notice row's payload (the SMS body, the GPS string) is its
                // meta line; identity only where it exists. The access code and
                // the frequency sit where a call row puts them.
                if (model.kind === 1) {
                    var parts = [];
                    if (model.tg > 0)
                        parts.push(Util.targetMeta(model.tg, model.gi));
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
                    meta.push(Util.targetMeta(model.tg, model.gi));
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
            // Issue #909: the call a History playlist is on.
            playback: Util.playlistMark(decoderHost, model.uid)
            // The frequency a search found is not added: the meta line, read as
            // the row's description, already leads with it. A row with no start
            // time has no time to read.
            accessibleName: model.timeText.length > 0 ? model.name + ", " + model.timeText : model.name
            // What the meta line and the name above already say (r2 C2): a
            // call's meta line names EMERGENCY and "encrypted" (a notice's
            // does not), and the name the time.
            metaSaysEmergency: model.kind !== 1
            metaSaysEnc: model.kind !== 1
            nameSaysTime: true
            onActivated: detailSheet.open(callHistory.recordFor(model.uid))
        }
    }

    // Issue #909: the History playlist, docked under the list.
    NowPlayingBar {
        id: nowPlaying
        objectName: "nowPlayingBar"
        status: screen.playlistStatus
        showRow: screen.showPlaying
        visible: shown && !screen.keyboardUp
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        // As the list: a tap on the confirm sheet over it reaches nothing here.
        enabled: !confirmClear.visible
    }

    ChoiceSheet {
        id: filterChoices
        objectName: "historyFilterChoices"
    }
    HistoryDetailSheet {
        id: detailSheet
        objectName: "historyDetailSheet"
        showPlayFromHere: true
        onPlayFromHereRequested: screen.flushPendingSearch()
        onShowOnMapRequested: function (record) {
            screen.showOnMap(record, detailSheet);
        }
    }

    // Keeps the newest call in view for a reader parked at the top of the log.
    FollowLatest {
        id: follow
        objectName: "callLogFollow"
        list: logList
        rowHeight: screen.rowHeight
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
        // repositioning an already-topped list is a no-op either way. A timer
        // the screen owns, so a row placed after the change (Show in list
        // flushing a search, Back from the map) cancels it (issue #909).
        //
        // A hidden log has no rows to reposition, so the place it will come
        // back to is dropped as well (issue #782). That covers the filter the
        // other History screen shares, and a search flushed as this screen hides
        // (flushPendingSearch), which runs after the log noted its place.
        function onFilterChanged() {
            logPlace.forget();
            logToStart.restart();
        }
    }

    // Issue #901 (D11): a change of density puts the log back at the newest
    // call, as a new search does; hidden, it comes back there rather than at
    // the place it held. Deferred so the rows take their new height first, by
    // a timer the screen owns, so nothing runs after it closes; the rows are
    // laid out at their new height before the view is placed, or the start it
    // found was the old layout's and a log read deep stayed off its top. A
    // filter change uses the same timer, and a row placed after either stops it.
    onCompactViewChanged: {
        logPlace.forget();
        logToStart.restart();
    }
    Timer {
        id: logToStart
        interval: 0
        onTriggered: {
            logList.forceLayout();
            logList.positionViewAtBeginning();
        }
    }

    // A sibling of the view, not a child: ListView reparents declared children
    // into its contentItem, where `parent.count` is undefined and the empty state
    // would never show. Its own clipped area over the list's (issue #909): a
    // short window with the bar docked scrolls the text and Clear filters there,
    // never over the bar.
    Flickable {
        id: emptyArea
        objectName: "callLogEmptyArea"
        anchors.fill: logList
        // The filter's count, not the view's: the view holds no rows while hidden.
        visible: historyView.count === 0
        clip: true
        contentWidth: width
        contentHeight: Math.max(height, emptyState.height)
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: emptyState
            objectName: "callLogEmptyState"
            x: Theme.screenPadding
            // Centred while it fits; from the top, scrolled, when it does not.
            y: Math.max(0, (emptyArea.height - height) / 2)
            width: emptyArea.width - 2 * Theme.screenPadding
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
        onConfirmed: {
            // Issue #909: a History playlist ends with the log it came from.
            screen.endPlaylist();
            callHistory.clearAll();
        }
    }
}
