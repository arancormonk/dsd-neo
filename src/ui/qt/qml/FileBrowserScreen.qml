// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Window
import "Util.js" as Util

// Issue #830: DSD-neo's own file browser, for a device with no usable file
// picker. It shows whatever FileBrowser request is live, above every owner
// screen and their sheets, and ends it with exactly one of FileBrowser.finish()
// (a file the host checked), reject() (Back at the locations) or otherApp().
//
// Each request gets its own host session: every listing, page, hand-off and
// access request names it, and every way the request ends (an answer, Back,
// a newer request, the owner closing or going away, this screen going away)
// closes it, so the host retires whatever is still out. Only an answer to the
// request this screen is waiting for is taken; any other is dropped.
//
// It is a modal (Navigation.addModal) while a request is live, so Back goes
// one level up through it and its controls are live over an owner's open
// sheet, which drops out of the accessibility tree under it. The owner stays
// visible below the opaque screen, so its import survives the pick.
Item {
    id: screen

    /** The user confirmed closing DSD-neo so a drive opens after the restart; Main runs the close. */
    signal closeConfirmed

    readonly property var request: FileBrowser.request
    readonly property bool shown: request !== null
    // Back and the navigation gate: the whole screen is the surface its controls live on.
    readonly property Item navigationSurface: screen
    readonly property bool isTopModal: Navigation.topModal === screen
    // The column everything below the header sits in: the screen's width on a
    // phone, a readable measure on a car's or tablet's wide screen.
    readonly property real contentWidth: Math.min(Theme.formWidth, width - 2 * Theme.screenPadding)
    // The last problem read out and how many have been, for tests.
    property string announced: ""
    property int announcements: 0

    // What the screen shows, for tests and its own bindings.
    readonly property string view: d.view
    readonly property var root: d.root
    readonly property string path: d.path
    readonly property var entries: d.entries
    readonly property string notice: d.notice
    readonly property bool reading: d.readingShown && (d.rootsId !== 0 || d.listingId !== 0)
    readonly property var access: decoderHost.fileAccess || ({})
    readonly property string crumbLine: crumbs().map(function (crumb) {
        return crumb.title;
    }).join(" › ")

    // The locations shown, in a fixed order. A removed drive has no row, and a
    // drive that needs access is reached through the access row instead.
    readonly property var rootRows: {
        var order = {inbox: 0, saved: 1, storage: 2, removable: 3};
        var listed = d.roots.filter(function (root) {
            return root.state !== "removed" && root.state !== "needsAccess";
        });
        return listed.map(function (root, index) {
            return {root: root, index: index};
        }).sort(function (a, b) {
            return (order[a.root.kind] - order[b.root.kind]) || a.index - b.index;
        }).map(function (item) {
            return item.root;
        });
    }
    // Android 11 and later: All files access through Settings, and only in a
    // build that declares it. Android 10: the runtime storage dialog.
    readonly property bool accessRowShown: access.granted !== true && ((access.route === "settings" && access.declared === true) || access.route === "permission")
    readonly property bool settingsRoute: access.route === "settings"
    readonly property bool adbOffered: settingsRoute && access.declared === true && access.apiLevel >= 30
    readonly property string adbAccessLine: "adb shell appops set --uid io.github.arancormonk.dsdneo MANAGE_EXTERNAL_STORAGE allow"

    readonly property bool inboxFolderEmpty: d.view === "folder" && d.root !== null && d.root.kind === "inbox" && d.path === d.root.path && d.folderStatus === "ok" && d.entries.length === 0 && (d.hiddenByFilter === 0 || d.showAll)
    readonly property string inboxCopyText: d.root ? d.root.path + "\n" + "adb push FILE " + d.root.path + "/" : ""
    readonly property string stateText: folderStateText()
    readonly property bool stateIsProblem: ["", "ok"].indexOf(d.folderStatus) < 0

    visible: shown
    Keys.onEscapePressed: Navigation.back(screen.Window.window)
    Keys.onBackPressed: Navigation.back(screen.Window.window)

    onRequestChanged: sync()
    Component.onCompleted: sync()
    Component.onDestruction: {
        if (d.session !== 0)
            decoderHost.closeFileBrowserSession(d.session);
        d.session = 0;
        Navigation.removeModal(screen);
        if (treeRefresh !== null)
            treeRefresh.destroy();
    }

    // ---- the request and its session ----

    function sync() {
        var token = request ? request.token : 0;
        if (token === d.token)
            return;
        if (token === 0) {
            end();
            return;
        }
        begin(token);
    }
    // A new request; one still showing is superseded: its session closes and
    // the screen starts again at the locations, keeping where focus returns to.
    function begin(token) {
        var window = screen.Window.window;
        if (d.token !== 0) {
            decoderHost.closeFileBrowserSession(d.session);
        } else {
            d.opener = window ? window.activeFocusItem : null;
            d.modalsUnder = Navigation.modals.slice();
            Navigation.clearInput(window);
            Navigation.addModal(screen);
        }
        d.token = token;
        reset();
        d.session = decoderHost.openFileBrowserSession();
        showRoots();
        screen.forceActiveFocus();
    }
    function end() {
        if (d.session !== 0)
            decoderHost.closeFileBrowserSession(d.session);
        d.session = 0;
        d.token = 0;
        reset();
        closeConfirm.visible = false;
        Navigation.removeModal(screen);
        var opener = d.opener;
        var under = d.modalsUnder;
        d.opener = null;
        d.modalsUnder = [];
        restoreFocus(opener, under);
        // Bindings may settle, or the pick may open another sheet, after the request ends.
        Qt.callLater(restoreFocus, opener, under);
    }
    function reset() {
        readingDelay.stop();
        d.readingShown = false;
        d.rootsId = 0;
        d.listingId = 0;
        d.pageId = 0;
        d.handoffId = 0;
        d.accessId = 0;
        d.view = "roots";
        d.roots = [];
        d.rootsStatus = "";
        d.root = null;
        d.path = "";
        d.entries = [];
        d.folderStatus = "";
        d.pageProblem = "";
        d.notice = "";
        d.showAll = false;
        d.accessAwaitingReturn = false;
        d.accessHelp = false;
        d.accessBlocked = false;
        d.closeKey = "";
        d.closeProblem = "";
        d.copied = "";
        d.seenAccess = screen.accessReading();
    }
    // Focus goes back where it was before the browser opened, when that is
    // still on screen and reachable, else to the surface on top. A sheet that
    // took the top since the browser opened (the backup's unlock sheet, after a
    // pick) keeps the focus it gave itself.
    function restoreFocus(opener, under) {
        // Read from the token, not `shown`: this runs inside the request's
        // change, before that binding has caught up. A new browse keeps focus.
        if (d.token !== 0)
            return;
        var top = Navigation.topModal;
        if (top && under.indexOf(top) < 0)
            return;
        var targets = [opener];
        if (top)
            targets.push(top.navigationSurface || top);
        else if (Navigation.layers.length)
            targets.push(Navigation.layers[Navigation.layers.length - 1].surface);
        if (Navigation.rootSurfaces.length)
            targets.push(Navigation.rootSurfaces[Navigation.rootSurfaces.length - 1]);
        for (var target of targets) {
            if (target && !Navigation.contains(screen, target) && Navigation.allows(target) && Navigation.presented(target) && target.enabled) {
                target.forceActiveFocus();
                return;
            }
        }
    }
    // One level up: a folder to its parent, a location to the list of
    // locations, which ends the request.
    function requestDismiss() {
        Navigation.commitComposing(false);
        if (!shown)
            return;
        if (d.view !== "folder") {
            FileBrowser.reject(d.token);
            return;
        }
        var cut = d.path.lastIndexOf("/");
        if (d.path.length > d.root.path.length && cut >= d.root.path.length)
            openFolder(d.root, d.path.substring(0, cut));
        else
            showRoots();
    }

    // ---- requests ----

    function accessReading() {
        var reading = decoderHost.fileAccess || {};
        return {
            granted: reading.granted === true,
            declared: reading.declared === true,
            route: reading.route || "",
            rootsGeneration: reading.rootsGeneration || 0
        };
    }
    function startReading() {
        d.readingShown = false;
        readingDelay.restart();
    }
    function cancelOutstanding() {
        for (var id of [d.listingId, d.pageId, d.handoffId]) {
            if (id !== 0)
                decoderHost.cancelFileRequest(id);
        }
        d.listingId = 0;
        d.pageId = 0;
        d.handoffId = 0;
    }
    // The locations, asked for again on every return to them: that is the
    // browser's refresh, so a drive plugged in or a grant given shows up.
    function showRoots() {
        cancelOutstanding();
        d.view = "roots";
        d.root = null;
        d.path = "";
        d.entries = [];
        d.folderStatus = "";
        d.pageProblem = "";
        d.notice = "";
        d.focusFirst = true;
        requestRoots(false);
    }
    // @p behind: a check of the locations under an open folder, after the
    // device's drives changed, which shows nothing while it runs.
    function requestRoots(behind) {
        if (d.rootsId !== 0)
            decoderHost.cancelFileRequest(d.rootsId);
        d.rootsId = 0;
        // A reading that changes now is the one the roots below are listed with.
        d.refreshingAccess = true;
        decoderHost.refreshFileAccess();
        d.refreshingAccess = false;
        d.seenAccess = accessReading();
        d.checkingBehind = behind;
        d.rootsId = decoderHost.requestFileRoots(d.session);
        if (!behind)
            startReading();
    }
    function openRoot(root) {
        openFolder(root, root.path);
    }
    function openFolder(root, path) {
        cancelOutstanding();
        d.view = "folder";
        d.root = root;
        d.path = path;
        d.notice = "";
        d.focusFirst = true;
        requestListing();
    }
    function requestListing() {
        cancelOutstanding();
        d.entries = [];
        d.folderStatus = "";
        d.pageProblem = "";
        d.more = false;
        d.snapshot = 0;
        d.hiddenByFilter = 0;
        d.listingId = decoderHost.requestFolderListing(d.session, d.root.key, d.generation, d.path, {
            kind: request.kind,
            showAll: d.showAll
        });
        startReading();
    }
    function showMore() {
        if (d.pageId !== 0 || !d.more)
            return;
        d.pageProblem = "";
        d.pageId = decoderHost.requestMorePage(d.session, d.snapshot, d.entries.length);
        startReading();
    }
    function choose(entry) {
        if (entry.dir) {
            openFolder(d.root, entry.path);
            return;
        }
        if (d.handoffId !== 0)
            decoderHost.cancelFileRequest(d.handoffId);
        d.notice = "";
        d.handoffId = decoderHost.requestFileHandoff(d.session, d.root.key, d.generation, entry.reference);
    }
    function askAccess() {
        d.accessBlocked = false;
        d.accessId = decoderHost.requestFileAccess(d.session);
    }
    function copy(text) {
        if (decoderHost.copyText(text)) {
            d.copied = text;
            copiedTimer.restart();
        }
    }
    function setNotice(text) {
        d.notice = text;
        Navigation.announce(screen, noticeLine, text);
    }

    // ---- answers ----

    function rootsAnswered(result) {
        var behind = d.checkingBehind;
        d.rootsId = 0;
        d.checkingBehind = false;
        if (!behind)
            readingDelay.stop();
        if (result.status === "cancelled")
            return;
        if (result.status !== "ok") {
            if (!behind) {
                d.rootsStatus = result.status;
                Navigation.announce(screen, rootsProblemLine, problemText(result.status));
            }
            return;
        }
        d.roots = result.roots || [];
        d.generation = result.generation || 0;
        d.rootsStatus = "";
        if (behind) {
            folderRootChecked();
            return;
        }
        Qt.callLater(focusFirstRow);
    }
    // The drives changed under an open folder: it stays open while its
    // location is still there, else the locations show and say why.
    function folderRootChecked() {
        if (d.view !== "folder")
            return;
        for (var root of d.roots) {
            if (root.key === d.root.key && root.state === "ready" && root.path === d.root.path) {
                d.root = root;
                return;
            }
        }
        var removable = d.root.kind === "removable";
        showRoots();
        if (removable)
            setNotice(qsTr("The drive was removed."));
    }
    function listingAnswered(result) {
        d.listingId = 0;
        readingDelay.stop();
        var status = result.status;
        if (status === "cancelled")
            return;
        if (status === "stale") {
            showRoots();
            return;
        }
        if (status === "gone" && d.root.kind === "removable") {
            showRoots();
            setNotice(qsTr("The drive was removed."));
            return;
        }
        if (status === "ok") {
            d.entries = result.entries || [];
            d.snapshot = result.snapshot || 0;
            d.more = result.more === true;
            d.hiddenByFilter = result.hiddenByFilter || 0;
        }
        d.folderStatus = status;
        if (screen.stateIsProblem)
            Navigation.announce(screen, stateLine, screen.stateText);
        Qt.callLater(focusFirstRow);
    }
    function pageAnswered(result) {
        d.pageId = 0;
        readingDelay.stop();
        var status = result.status;
        if (status === "cancelled")
            return;
        if (status === "ok") {
            d.entries = d.entries.concat(result.entries || []);
            d.more = result.more === true;
            return;
        }
        // The folder was read too long ago to page on: read it again.
        if (status === "expired") {
            requestListing();
            return;
        }
        d.pageProblem = problemText(status);
        Navigation.announce(screen, pageProblemLine, d.pageProblem);
    }
    function handoffAnswered(result) {
        d.handoffId = 0;
        if (result.ok === true) {
            FileBrowser.finish(d.token, result.reference, result.name);
            return;
        }
        var reason = result.reason || "";
        if (reason === "cancelled")
            return;
        if (reason === "stale")
            showRoots();
        setNotice(reason === "busy" || reason === "failed" || reason === "timedOut" ? problemText(reason) : qsTr("DSD-neo can't open that file. Pick another one."));
    }
    function accessAnswered(outcome) {
        d.accessId = 0;
        if (outcome === "settingsOpened") {
            d.accessAwaitingReturn = true;
        } else if (outcome === "noRoute" || outcome === "failed") {
            if (screen.adbOffered)
                d.accessHelp = true;
            else
                d.accessBlocked = outcome === "failed";
        } else if (outcome === "blocked") {
            d.accessBlocked = true;
        } else if (outcome === "granted" && d.view === "roots") {
            requestRoots(false);
        }
    }
    // Back from Settings without the switch turned on: say how else to allow it.
    function accessReturned() {
        if (!d.accessAwaitingReturn || screen.access.granted === true)
            return;
        d.accessAwaitingReturn = false;
        if (screen.adbOffered)
            d.accessHelp = true;
    }
    function fileAccessUpdated() {
        if (!shown)
            return;
        var now = accessReading();
        var before = d.seenAccess;
        d.seenAccess = now;
        accessReturned();
        if (d.refreshingAccess)
            return;
        if (now.granted === before.granted && now.declared === before.declared && now.route === before.route && now.rootsGeneration === before.rootsGeneration)
            return;
        if (d.view === "roots")
            requestRoots(false);
        else if (d.rootsId === 0)
            requestRoots(true);
    }

    // ---- words ----

    function rootTitle(root) {
        if (root.kind === "inbox")
            return qsTr("DSD-neo inbox");
        if (root.kind === "saved")
            return qsTr("Saved by DSD-neo");
        if (root.kind === "storage")
            return qsTr("Device storage");
        return root.title && root.title.length ? root.title : qsTr("Removable storage");
    }
    function rootSubtitle(root) {
        if (root.state === "needsRestart")
            return qsTr("DSD-neo can open this drive after it restarts.");
        if (root.state !== "ready")
            return qsTr("Not available right now.");
        if (root.kind === "saved")
            return qsTr("Files DSD-neo saved to Download/DSD-neo");
        return root.path;
    }
    function problemText(status) {
        if (status === "timedOut")
            return qsTr("DSD-neo could not read this in time. Try again.");
        return qsTr("The storage is not answering. Try again in a moment.");
    }
    function filteredText(count) {
        var kind = request ? request.kind : "";
        if (kind === "backup")
            return count === 1 ? qsTr("No backups here. Show all files lists the other item.") : qsTr("No backups here. Show all files lists the other %1 items.").arg(count);
        if (kind === "replay")
            return count === 1 ? qsTr("No replay files here. Show all files lists the other item.") : qsTr("No replay files here. Show all files lists the other %1 items.").arg(count);
        return count === 1 ? qsTr("No CSV files here. Show all files lists the other item.") : qsTr("No CSV files here. Show all files lists the other %1 items.").arg(count);
    }
    function folderStateText() {
        if (d.view !== "folder")
            return "";
        switch (d.folderStatus) {
        case "ok":
            if (d.entries.length > 0 || screen.inboxFolderEmpty)
                return "";
            if (d.hiddenByFilter > 0 && !d.showAll)
                return filteredText(d.hiddenByFilter);
            if (d.root.kind === "saved")
                return qsTr("Nothing saved here by this install of DSD-neo.");
            return qsTr("This folder is empty.");
        case "unreadable":
        case "outside":
            return qsTr("DSD-neo can't read this folder.");
        case "gone":
            return qsTr("That folder is gone.");
        case "tooMany":
            return qsTr("This folder has too many items to show.");
        case "":
            return "";
        default:
            return problemText(d.folderStatus);
        }
    }
    function dateText(ms) {
        if (!ms)
            return "";
        var stamp = new Date(ms);
        var now = new Date(savedSystems.realtimeNowMs());
        return Qt.formatDate(stamp, stamp.getFullYear() === now.getFullYear() ? "MMM d" : "MMM d, yyyy");
    }
    // "4.2 MB · Nov 9"; a file with no time (a FAT drive's) shows its size only.
    function entrySubtitle(entry) {
        if (entry.dir)
            return "";
        var parts = [Util.sizeText(entry.size)];
        var date = dateText(entry.modifiedMs);
        if (date.length)
            parts.push(date);
        return parts.join(" · ");
    }
    function entryName(entry) {
        if (entry.dir)
            return qsTr("%1, folder").arg(entry.name);
        var parts = [qsTr("%1, file").arg(entry.name), Util.sizeText(entry.size)];
        var date = dateText(entry.modifiedMs);
        if (date.length)
            parts.push(date);
        return parts.join(", ");
    }
    // The open folder's path from its location down: each step but the last
    // goes there.
    function crumbs() {
        if (d.view !== "folder" || !d.root)
            return [];
        var list = [{title: rootTitle(d.root), path: d.root.path}];
        var rest = d.path.substring(d.root.path.length).split("/").filter(function (part) {
            return part.length > 0;
        });
        var at = d.root.path;
        for (var part of rest) {
            at += "/" + part;
            list.push({title: part, path: at});
        }
        return list;
    }

    // ---- focus ----

    // The first row of what was just listed takes the focus, so a keyboard or
    // a screen reader starts at the new content.
    function focusFirstRow() {
        if (!d.focusFirst || !shown || !isTopModal)
            return;
        d.focusFirst = false;
        var first = null;
        if (d.view === "roots") {
            for (var i = 0; i < rootRepeater.count && !first; ++i) {
                var row = rootRepeater.itemAt(i);
                if (row && row.focusTarget)
                    first = row.focusTarget;
            }
            if (!first && accessButton.visible)
                first = accessButton;
            if (!first && otherAppRow.visible)
                first = otherAppRow;
        } else if (d.entries.length) {
            entryList.forceLayout();
            first = entryList.itemAtIndex(0);
        }
        (first || screen).forceActiveFocus();
    }
    function revealFocus() {
        var window = screen.Window.window;
        var focus = window ? window.activeFocusItem : null;
        if (!focus || !Navigation.contains(screen, focus))
            return;
        Theme.revealFocus(rootsScroll, rootsColumn, focus);
        Theme.revealFocus(stateScroll, stateColumn, focus);
        Theme.revealFocus(entryList, entryList.contentItem, focus);
    }
    Connections {
        target: screen.Window.window
        function onActiveFocusItemChanged() {
            if (screen.shown)
                Qt.callLater(screen.revealFocus);
        }
    }

    Connections {
        target: decoderHost
        function onFileRootsListed(id, result) {
            if (id !== 0 && id === d.rootsId)
                screen.rootsAnswered(result);
        }
        function onFolderListed(id, result) {
            if (id === 0)
                return;
            if (id === d.listingId)
                screen.listingAnswered(result);
            else if (id === d.pageId)
                screen.pageAnswered(result);
        }
        function onFileHandoffChecked(id, result) {
            if (id !== 0 && id === d.handoffId)
                screen.handoffAnswered(result);
        }
        function onFileAccessRequestDone(id, outcome) {
            if (id !== 0 && id === d.accessId)
                screen.accessAnswered(outcome);
        }
        function onFileAccessChanged() {
            screen.fileAccessUpdated();
        }
    }
    Connections {
        target: Qt.application
        function onStateChanged() {
            if (screen.shown && Qt.application.state === Qt.ApplicationActive)
                screen.accessReturned();
        }
    }
    Connections {
        target: restartCloser
        function onFinished(outcome) {
            if (!screen.shown || outcome === "closing")
                return;
            d.closeProblem = qsTr("DSD-neo could not close itself. Close it from Android's recent apps, then open it again.");
            Navigation.announce(screen, null, d.closeProblem);
        }
    }

    // ---- closing for a restart ----

    function closeMessage() {
        var lines = [];
        if (decoderHost.sessionActive === true)
            lines.push(qsTr("Listening stops."));
        if (request && request.kind === "csv")
            lines.push(qsTr("This import is dropped."));
        if (backup.restorePending === true)
            lines.push(qsTr("Your backup is restored when DSD-neo opens again."));
        return lines.join("\n");
    }
    function refuseClose() {
        if (!restartCloser.refused())
            return false;
        d.closeProblem = qsTr("Wait for the copy to finish, then close DSD-neo.");
        Navigation.announce(screen, null, d.closeProblem);
        return true;
    }
    function askClose(key) {
        d.closeKey = key;
        d.closeProblem = "";
        if (refuseClose())
            return;
        closeConfirm.message = closeMessage();
        closeConfirm.open();
    }
    function confirmClose() {
        if (refuseClose())
            return;
        screen.closeConfirmed();
    }

    QtObject {
        id: d
        property int token: 0
        property double session: 0
        property var opener: null
        property var modalsUnder: []
        // "roots" (the locations) or "folder".
        property string view: "roots"
        property var roots: []
        property double generation: 0
        property string rootsStatus: ""
        property var root: null
        property string path: ""
        property var entries: []
        property double snapshot: 0
        property bool more: false
        property int hiddenByFilter: 0
        // The open folder's listing status; "" while it is read.
        property string folderStatus: ""
        property string pageProblem: ""
        property bool showAll: false
        // The one request of each kind this screen waits for; 0 for none.
        property double rootsId: 0
        property double listingId: 0
        property double pageId: 0
        property double handoffId: 0
        property double accessId: 0
        property bool checkingBehind: false
        property bool refreshingAccess: false
        property var seenAccess: ({})
        property bool readingShown: false
        property bool focusFirst: false
        property string notice: ""
        property bool accessAwaitingReturn: false
        property bool accessHelp: false
        property bool accessBlocked: false
        // The drive whose Close DSD-neo was pressed, and why it did not close.
        property string closeKey: ""
        property string closeProblem: ""
        property string copied: ""
    }
    // "Reading folder…" only for a read that takes a moment, so a quick one
    // never flashes it.
    Timer {
        id: readingDelay
        interval: 250
        onTriggered: d.readingShown = d.rootsId !== 0 || d.listingId !== 0 || d.pageId !== 0
    }
    Timer {
        id: copiedTimer
        interval: 2000
        onTriggered: d.copied = ""
    }

    // Issue #782: as ModalSheet's, tells Android's bridge to read the window's
    // tree again each time this screen takes the top or leaves it.
    readonly property bool topTurn: shown && isTopModal
    property Item treeRefresh: null
    onTopTurnChanged: {
        if (topTurn && treeRefresh === null && Window.contentItem !== null && typeof screenReader !== "undefined" && screenReader.active)
            treeRefresh = treeRefreshComponent.createObject(Window.contentItem, {
                "objectName": "fileBrowserTreeRefresh"
            });
    }
    Component {
        id: treeRefreshComponent
        Item {
            width: 0
            height: 0
            opacity: 0
            visible: screen.topTurn
            Accessible.role: Accessible.Grouping
            Accessible.focusable: false
        }
    }

    // ---- view ----

    // A selectable command with its Copy button, for a step done from a computer.
    component CommandBlock: Column {
        id: block
        property string command: ""
        property string copyText: command
        property string copyName: "fileBrowserCopy"
        // True for a while after a copy the clipboard took.
        property bool copied: false
        signal copyRequested(string text)
        width: parent ? parent.width : 0
        spacing: 8
        Rectangle {
            width: parent.width
            height: commandText.implicitHeight + 20
            radius: Theme.radiusButton
            color: Theme.bg
            border.width: 1
            border.color: Theme.controlBorder
            TextEdit {
                id: commandText
                objectName: block.objectName + "Text"
                x: 10
                y: 10
                width: parent.width - 20
                text: block.command
                readOnly: true
                selectByMouse: true
                textFormat: TextEdit.PlainText
                // Breaks between words first, so a long command splits where
                // it is still readable.
                wrapMode: TextEdit.Wrap
                color: Theme.textPrimary
                selectionColor: Qt.alpha(Theme.cyan, 0.35)
                selectedTextColor: Theme.textPrimary
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(12)
                readonly property bool navigationAllowed: Navigation.allows(commandText)
                activeFocusOnTab: false
                Accessible.role: Accessible.StaticText
                Accessible.ignored: !navigationAllowed || !visible
                Accessible.name: Accessible.ignored ? "" : text
            }
        }
        OutlineButton {
            objectName: block.copyName
            text: block.copied ? qsTr("Copied") : qsTr("Copy")
            onClicked: block.copyRequested(block.copyText)
        }
    }
    // A line under a row: a hint, or what went wrong.
    component Line: ReadableText {
        property bool problem: false
        width: parent ? parent.width : 0
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(problem ? 13 : 12)
        color: problem ? Theme.alert : Theme.textSubdued
        wrapMode: Text.Wrap
        Accessible.role: problem ? Accessible.AlertMessage : Accessible.StaticText
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }
    // Nothing behind the browser takes a press, drag or wheel.
    PointerBarrier {}

    Column {
        id: top
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.screenPadding
        spacing: 6

        // Back, one level up, and the picker's title, which wraps at a large
        // font rather than running under the edge.
        Item {
            id: header
            width: parent.width
            height: Math.max(backButton.height, title.height)

            IconButton {
                id: backButton
                objectName: "fileBrowserBack"
                icon: "back"
                x: 0
                y: title.lineCount > 1 ? 0 : (header.height - height) / 2
                onClicked: screen.requestDismiss()
            }
            ReadableText {
                id: title
                objectName: "fileBrowserTitle"
                heading: true
                x: backButton.width + 14
                y: lineCount > 1 ? (backButton.height - Theme.fontSize(22) * 1.3) / 2 : (header.height - height) / 2
                width: parent.width - x
                text: screen.request ? screen.request.title : ""
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(22)
                font.weight: Font.Bold
                font.letterSpacing: -0.22
                color: Theme.textPrimary
                wrapMode: Text.Wrap
            }
        }

        // Where the open folder is: each step up is a button to it.
        Flow {
            id: crumbFlow
            objectName: "fileBrowserCrumbs"
            x: (parent.width - width) / 2
            width: screen.contentWidth
            visible: d.view === "folder"

            Repeater {
                model: screen.crumbs()
                Row {
                    id: crumbStep
                    required property int index
                    required property var modelData
                    readonly property bool current: index === screen.crumbs().length - 1
                    height: crumb.height

                    Text {
                        visible: crumbStep.index > 0
                        width: 20
                        height: crumb.height
                        text: "›"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(15)
                        color: Theme.textSubdued
                        Accessible.ignored: true
                    }
                    Item {
                        id: crumb
                        objectName: "fileBrowserCrumb" + crumbStep.index
                        readonly property real room: crumbFlow.width - (crumbStep.index > 0 ? 20 : 0)
                        width: Math.min(room, crumbLabel.implicitWidth + (crumbStep.current ? 0 : 16))
                        height: Math.max(Theme.minimumTouchSize, crumbLabel.height + 12)
                        readonly property bool navigationAllowed: Navigation.allows(crumb)
                        activeFocusOnTab: (navigationAllowed && !crumbStep.current) || activeFocus
                        Accessible.role: crumbStep.current ? Accessible.StaticText : Accessible.Button
                        Accessible.name: !navigationAllowed ? "" : crumbStep.current ? crumbStep.modelData.title : qsTr("Go to %1").arg(crumbStep.modelData.title)
                        Accessible.ignored: !navigationAllowed || !visible
                        Accessible.onPressAction: activate()
                        function activate() {
                            if (!crumbStep.current && Navigation.allows(crumb))
                                screen.openFolder(d.root, crumbStep.modelData.path);
                        }
                        Keys.onSpacePressed: activate()
                        Keys.onReturnPressed: activate()
                        Keys.onEnterPressed: activate()
                        FocusFrame {}
                        Rectangle {
                            anchors.fill: parent
                            radius: Theme.radiusButton
                            color: crumbTap.pressed && !crumbStep.current ? Theme.chipSelectedFill : "transparent"
                        }
                        Text {
                            id: crumbLabel
                            anchors.centerIn: parent
                            width: Math.min(implicitWidth, crumb.room - (crumbStep.current ? 0 : 16))
                            text: crumbStep.modelData.title
                            textFormat: Text.PlainText
                            wrapMode: Text.WrapAnywhere
                            horizontalAlignment: Text.AlignHCenter
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(14)
                            font.weight: crumbStep.current ? Font.DemiBold : Font.Normal
                            color: crumbStep.current ? Theme.textPrimary : Theme.cyan
                        }
                        TapHandler {
                            id: crumbTap
                            enabled: !crumbStep.current
                            onTapped: crumb.activate()
                        }
                    }
                }
            }
        }

        Line {
            id: noticeLine
            objectName: "fileBrowserNotice"
            x: (parent.width - width) / 2
            width: screen.contentWidth
            problem: true
            visible: d.notice.length > 0
            text: d.notice
        }
        Line {
            objectName: "fileBrowserReading"
            x: (parent.width - width) / 2
            width: screen.contentWidth
            visible: screen.reading
            text: qsTr("Reading folder…")
        }
    }

    // ---- the locations ----

    PlexFlickable {
        id: rootsScroll
        objectName: "fileBrowserRootsScroll"
        accessibleName: screen.request ? screen.request.title : ""
        anchors.top: top.bottom
        anchors.topMargin: Theme.gap - rootsColumn.y
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: d.view === "roots"
        clip: true
        contentWidth: width
        contentHeight: rootsColumn.y + rootsColumn.height + Theme.screenPadding
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: rootsColumn
            // Below the top edge by the focus frame's reach, so the first
            // row's frame is not cut off.
            x: (rootsScroll.width - width) / 2
            y: 4
            width: screen.contentWidth
            spacing: Theme.gap

            Line {
                id: rootsProblemLine
                objectName: "fileBrowserRootsProblem"
                problem: true
                visible: d.rootsStatus.length > 0
                text: visible ? screen.problemText(d.rootsStatus) : ""
            }
            OutlineButton {
                objectName: "fileBrowserRootsRetry"
                visible: d.rootsStatus.length > 0
                text: qsTr("Try again")
                onClicked: screen.showRoots()
            }

            UiPanel {
                objectName: "fileBrowserRootsPanel"
                width: parent.width
                height: rootsList.height
                visible: screen.rootRows.length > 0 || screen.accessRowShown

                Column {
                    id: rootsList
                    width: parent.width

                    Repeater {
                        id: rootRepeater
                        model: StableRows {
                            id: rootRowsModel
                            entries: screen.rootRows
                            keyOf: function (root) {
                                return root.key;
                            }
                            lineOf: function (root) {
                                return [root.state, screen.rootTitle(root), screen.rootSubtitle(root)].join("\n");
                            }
                        }
                        Item {
                            id: rootItem
                            required property int index
                            required property string key
                            required property string line
                            readonly property bool live: index >= 0 && index < rootRowsModel.live && index < screen.rootRows.length
                            readonly property var root: live ? screen.rootRows[index] : null
                            readonly property string rootState: root ? root.state : ""
                            readonly property bool ready: rootState === "ready"
                            readonly property bool last: index === screen.rootRows.length - 1 && !screen.accessRowShown
                            readonly property Item focusTarget: ready ? openRow : rootState === "needsRestart" ? closeButton : null
                            width: parent.width
                            height: ready ? openRow.height : rootStateColumn.height + 2 * Theme.cardPadding
                            visible: index < rootRowsModel.shown

                            DisclosureRow {
                                id: openRow
                                objectName: "fileBrowserRoot:" + rootItem.key
                                visible: rootItem.ready
                                title: rootItem.root ? screen.rootTitle(rootItem.root) : ""
                                subtitle: rootItem.root ? screen.rootSubtitle(rootItem.root) : ""
                                showDivider: !rootItem.last
                                onTapped: screen.openRoot(rootItem.root)
                            }
                            // A location that cannot open yet says why, and
                            // a drive that needs a restart offers it.
                            Column {
                                id: rootStateColumn
                                objectName: "fileBrowserRootState:" + rootItem.key
                                visible: !rootItem.ready && rootItem.live
                                x: Theme.cardPadding
                                y: Theme.cardPadding
                                width: parent.width - 2 * Theme.cardPadding
                                spacing: 4

                                ReadableText {
                                    width: parent.width
                                    text: rootItem.root ? screen.rootTitle(rootItem.root) : ""
                                    font.family: Theme.sans
                                    font.pixelSize: Theme.fontSize(15)
                                    font.weight: Font.DemiBold
                                    color: Theme.textPrimary
                                    wrapMode: Text.Wrap
                                }
                                Line {
                                    text: rootItem.root ? screen.rootSubtitle(rootItem.root) : ""
                                }
                                Item {
                                    width: 1
                                    height: 6
                                    visible: closeButton.visible
                                }
                                OutlineButton {
                                    id: closeButton
                                    objectName: "fileBrowserClose:" + rootItem.key
                                    visible: rootItem.rootState === "needsRestart"
                                    enabled: !restartCloser.active
                                    text: qsTr("Close DSD-neo")
                                    onClicked: screen.askClose(rootItem.key)
                                }
                                Line {
                                    visible: closeButton.visible
                                    text: qsTr("Then open DSD-neo again.")
                                }
                                Line {
                                    objectName: "fileBrowserCloseProblem:" + rootItem.key
                                    problem: true
                                    visible: closeButton.visible && d.closeKey === rootItem.key && d.closeProblem.length > 0
                                    text: visible ? d.closeProblem : ""
                                }
                            }
                            Rectangle {
                                anchors.bottom: parent.bottom
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.leftMargin: Theme.cardPadding
                                height: 1
                                visible: !rootItem.ready && !rootItem.last
                                color: Theme.divider
                            }
                        }
                    }

                    // Device storage and drives, before access is given.
                    Column {
                        id: accessBlock
                        objectName: "fileBrowserAccess"
                        visible: screen.accessRowShown
                        x: Theme.cardPadding
                        width: parent.width - 2 * Theme.cardPadding
                        topPadding: Theme.cardPadding
                        bottomPadding: Theme.cardPadding
                        spacing: 4

                        ReadableText {
                            objectName: "fileBrowserAccessTitle"
                            width: parent.width
                            text: screen.settingsRoute ? qsTr("Download and USB drives") : qsTr("Device storage")
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(15)
                            font.weight: Font.DemiBold
                            color: Theme.textPrimary
                            wrapMode: Text.Wrap
                        }
                        Line {
                            objectName: "fileBrowserAccessSubtitle"
                            text: screen.settingsRoute ? qsTr("Needs All files access") : qsTr("Needs access to device storage")
                        }
                        Item {
                            width: 1
                            height: 6
                        }
                        OutlineButton {
                            id: accessButton
                            objectName: "fileBrowserAllowAccess"
                            text: qsTr("Allow access")
                            onClicked: screen.askAccess()
                        }
                        Line {
                            objectName: "fileBrowserAccessRevoke"
                            visible: screen.settingsRoute
                            text: qsTr("Turning All files access off later closes DSD-neo (Android does that).")
                        }
                        Line {
                            objectName: "fileBrowserAccessBlocked"
                            problem: true
                            visible: d.accessBlocked
                            text: qsTr("Allow Files and media for DSD-neo in App info, then come back.")
                        }
                        Column {
                            objectName: "fileBrowserAccessHelp"
                            visible: d.accessHelp && screen.adbOffered
                            width: parent.width
                            topPadding: 8
                            spacing: 8

                            // Android 11 and 12 offer the switch under App info
                            // too, where Settings may have sent the user.
                            Line {
                                objectName: "fileBrowserAppInfoStep"
                                visible: screen.access.apiLevel >= 30 && screen.access.apiLevel <= 32
                                text: qsTr("In App info, open Permissions › Files and media, then allow management of all files.")
                            }
                            Line {
                                text: qsTr("If Settings had no switch for DSD-neo, allow it from a computer:")
                            }
                            CommandBlock {
                                objectName: "fileBrowserAdbCommand"
                                copyName: "fileBrowserCopyAdb"
                                copied: d.copied.length > 0 && d.copied === copyText
                                onCopyRequested: function (text) {
                                    screen.copy(text);
                                }
                                command: screen.adbAccessLine
                            }
                        }
                    }
                }
            }

            UiPanel {
                width: parent.width
                height: otherAppRow.height
                visible: screen.request !== null && screen.request.allowOtherApp === true

                DisclosureRow {
                    id: otherAppRow
                    objectName: "fileBrowserOtherApp"
                    visible: parent.visible
                    title: qsTr("Choose with another app")
                    subtitle: ""
                    onTapped: FileBrowser.otherApp(d.token)
                }
            }
        }
    }

    // ---- a folder ----

    // What the folder holds, or why it shows nothing.
    PlexFlickable {
        id: stateScroll
        objectName: "fileBrowserStateScroll"
        anchors.top: top.bottom
        anchors.topMargin: Theme.gap
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: showAllPanel.top
        anchors.bottomMargin: Theme.gap
        visible: d.view === "folder" && d.entries.length === 0
        clip: true
        contentWidth: width
        contentHeight: stateColumn.height
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: stateColumn
            x: (stateScroll.width - width) / 2
            width: screen.contentWidth
            spacing: Theme.gap

            ReadableText {
                id: stateLine
                objectName: "fileBrowserState"
                width: parent.width
                visible: text.length > 0
                text: screen.stateText
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(14)
                color: screen.stateIsProblem ? Theme.alert : Theme.textSubdued
                wrapMode: Text.Wrap
                Accessible.role: screen.stateIsProblem ? Accessible.AlertMessage : Accessible.StaticText
            }
            OutlineButton {
                objectName: "fileBrowserRetry"
                visible: ["timedOut", "busy", "failed", "unsupported"].indexOf(d.folderStatus) >= 0
                text: qsTr("Try again")
                onClicked: screen.requestListing()
            }
            // An empty inbox says how files get there.
            Column {
                objectName: "fileBrowserInboxHelp"
                width: parent.width
                visible: screen.inboxFolderEmpty
                spacing: Theme.gap

                ReadableText {
                    width: parent.width
                    text: qsTr("Files copied into this folder show up here. Depending on the device, you can copy them from a computer over USB or with adb:")
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(14)
                    color: Theme.textSubdued
                    wrapMode: Text.Wrap
                }
                CommandBlock {
                    objectName: "fileBrowserInboxCommand"
                    copyName: "fileBrowserCopyInbox"
                    copied: d.copied.length > 0 && d.copied === copyText
                    onCopyRequested: function (text) {
                        screen.copy(text);
                    }
                    command: screen.inboxCopyText
                }
                Line {
                    text: qsTr("Uninstalling DSD-neo or clearing its storage empties this folder.")
                }
            }
        }
    }

    ListView {
        id: entryList
        objectName: "fileBrowserList"
        anchors.top: top.bottom
        anchors.topMargin: Theme.gap
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: showAllPanel.top
        anchors.bottomMargin: Theme.gap
        // The column is the view's: a vertical ListView writes each row's x.
        width: screen.contentWidth
        visible: d.view === "folder" && d.entries.length > 0
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        keyNavigationEnabled: false
        model: StableRows {
            id: entryRows
            entries: d.view === "folder" ? d.entries : []
            keyOf: function (entry) {
                return entry.path.length ? entry.path : entry.reference;
            }
            lineOf: function (entry) {
                return screen.entryName(entry);
            }
        }

        delegate: Item {
            id: entryRow
            required property int index
            required property string key
            required property string line
            readonly property bool live: index >= 0 && index < entryRows.live && index < d.entries.length
            readonly property var entry: live ? d.entries[index] : null
            readonly property bool folder: entry !== null && entry.dir === true
            objectName: "fileBrowserEntry:" + key
            width: ListView.view.width
            height: Math.max(Theme.minimumTouchSize, labels.height + 24)
            opacity: index < entryRows.shown ? 1 : 0
            readonly property bool navigationAllowed: Navigation.allows(entryRow)
            activeFocusOnTab: (navigationAllowed && visible && live) || activeFocus
            Accessible.role: Accessible.Button
            Accessible.name: navigationAllowed && live ? line : ""
            Accessible.focusable: navigationAllowed && visible && live
            Accessible.ignored: !navigationAllowed || !visible || !live
            Accessible.onPressAction: activate()
            function activate() {
                if (live && Navigation.allows(entryRow))
                    screen.choose(entry);
            }
            Keys.onSpacePressed: activate()
            Keys.onReturnPressed: activate()
            Keys.onEnterPressed: activate()
            // Inside the row: the list clips at its edges.
            FocusFrame {
                anchors.margins: 0
            }

            Rectangle {
                anchors.fill: parent
                radius: Theme.radiusButton
                color: entryTap.pressed && entryRow.live ? Theme.chipSelectedFill : "transparent"
            }

            // Drawn marks, in the caret's stroke: a folder, or a page.
            Item {
                id: glyph
                x: 8
                anchors.verticalCenter: parent.verticalCenter
                width: 20
                height: 20
                Rectangle {
                    visible: entryRow.folder
                    x: 1
                    y: 2
                    width: 8
                    height: 5
                    radius: 1.5
                    color: "transparent"
                    border.width: 1.6
                    border.color: Theme.textSecondary
                }
                Rectangle {
                    visible: entryRow.folder
                    x: 1
                    y: 5
                    width: 18
                    height: 13
                    radius: 2
                    color: Theme.bg
                    border.width: 1.6
                    border.color: Theme.textSecondary
                }
                Rectangle {
                    visible: !entryRow.folder
                    x: 3
                    y: 1
                    width: 14
                    height: 18
                    radius: 2
                    color: "transparent"
                    border.width: 1.6
                    border.color: Theme.textSecondary
                }
                Repeater {
                    model: entryRow.folder ? 0 : 3
                    Rectangle {
                        required property int index
                        x: 6
                        y: 6 + index * 3.5
                        width: index === 2 ? 5 : 8
                        height: 1.5
                        radius: 0.75
                        color: Theme.textSecondary
                    }
                }
            }

            Column {
                id: labels
                anchors.left: glyph.right
                anchors.leftMargin: 14
                anchors.right: entryCaret.visible ? entryCaret.left : parent.right
                anchors.rightMargin: entryCaret.visible ? 12 : 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 3

                Text {
                    objectName: "fileBrowserEntryName"
                    width: parent.width
                    text: entryRow.entry ? entryRow.entry.name : ""
                    // A file's name as it is on the drive, never markup.
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(15)
                    font.weight: Font.DemiBold
                    color: Theme.textPrimary
                    Accessible.ignored: true
                }
                Text {
                    width: parent.width
                    visible: text.length > 0
                    text: entryRow.entry ? screen.entrySubtitle(entryRow.entry) : ""
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(12)
                    color: Theme.textSubdued
                    Accessible.ignored: true
                }
            }
            Caret {
                id: entryCaret
                visible: entryRow.folder
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                rotation: -90
                color: Theme.textSubdued
            }
            Rectangle {
                anchors.bottom: parent.bottom
                anchors.left: labels.left
                anchors.right: parent.right
                height: 1
                visible: entryRow.index < d.entries.length - 1 || d.more
                color: Theme.divider
            }
            TapHandler {
                id: entryTap
                gesturePolicy: TapHandler.ReleaseWithinBounds
                onTapped: entryRow.activate()
            }
        }

        footer: Column {
            width: ListView.view ? ListView.view.width : 0
            topPadding: d.more || d.pageProblem.length ? Theme.gap : 0
            spacing: 8

            OutlineButton {
                objectName: "fileBrowserShowMore"
                width: parent.width
                visible: d.more
                enabled: d.pageId === 0
                text: qsTr("Show more")
                onClicked: screen.showMore()
            }
            Line {
                objectName: "fileBrowserPageReading"
                visible: d.readingShown && d.pageId !== 0
                text: qsTr("Reading folder…")
            }
            Line {
                id: pageProblemLine
                objectName: "fileBrowserPageProblem"
                problem: true
                visible: d.pageProblem.length > 0
                text: d.pageProblem
            }
        }
    }

    // Hidden files and files of other kinds, on request.
    UiPanel {
        id: showAllPanel
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.screenPadding
        width: screen.contentWidth
        height: d.view === "folder" ? showAllRow.height : 0
        visible: d.view === "folder"

        ToggleRow {
            id: showAllRow
            objectName: "fileBrowserShowAll"
            visible: parent.visible
            title: qsTr("Show all files")
            checked: d.showAll
            onToggled: function (checked) {
                d.showAll = checked;
                screen.requestListing();
            }
        }
    }

    ConfirmDialog {
        id: closeConfirm
        objectName: "fileBrowserCloseConfirm"
        title: qsTr("Close DSD-neo?")
        confirmText: qsTr("Close DSD-neo")
        onConfirmed: screen.confirmClose()
    }
}
