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
    // What closing DSD-neo would drop besides the pick: "add" or "edit" while
    // the system editor holds changes not yet saved, else "". Main says.
    property var unsavedDraft: function () {
        return "";
    }

    readonly property var request: FileBrowser.request
    readonly property bool shown: request !== null
    // Back and the navigation gate: the whole screen is the surface its controls live on.
    readonly property Item navigationSurface: screen
    readonly property bool isTopModal: Navigation.topModal === screen
    // The column everything below the header sits in: the screen's width on a
    // phone, a readable measure on a car's or tablet's wide screen.
    readonly property real contentWidth: Math.min(Theme.formWidth, width - 2 * Theme.screenPadding)
    // A short screen (a car's, a phone on its side) has no height to spare
    // for a title and a path above a folder's rows: in a folder the path
    // takes the title's place beside Back. The title shows at the places.
    readonly property bool compactHeader: d.view === "folder" && height < 480
    // The last problem read out and how many have been, for tests.
    property string announced: ""
    property int announcements: 0

    // What the screen shows, for tests and its own bindings.
    readonly property string view: d.view
    readonly property var root: d.root
    readonly property string path: d.path
    readonly property var entries: d.entries
    readonly property string notice: d.notice
    readonly property bool reading: d.readingShown && (d.view === "roots" ? d.rootsId !== 0 : d.listingId !== 0)
    // A file tapped and being checked, from the tap until its answer (a
    // retry included); further taps on files wait for it.
    readonly property bool opening: d.opening !== null
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

    readonly property bool inboxFolderEmpty: d.view === "folder" && d.root !== null && d.root.kind === "inbox" && d.path === d.root.path && d.folderStatus === "ok" && d.entries.length === 0 && (d.hiddenByFilter === 0 || d.showAll) && d.hiddenByName === 0
    // An empty inbox's help: its path, and the adb line that copies a file there.
    readonly property var inboxLines: d.root ? [d.root.path, "adb push FILE " + d.root.path + "/"] : []
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
        d.opening = null;
        d.handoffRetried = false;
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
        d.removedKey = "";
        d.removedSeenGone = false;
        d.showAll = false;
        d.accessAwaitingReturn = false;
        d.accessHelp = false;
        d.accessBlocked = false;
        d.accessDenied = false;
        d.accessReturnSeen = false;
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
        d.opening = null;
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
        d.removedKey = "";
        d.focusFirst = true;
        requestRoots(false);
    }
    // Back at the locations because drive @p key went away, which they say
    // until a list of them has that drive again. @p seenGone: the locations
    // just read already lack it (a folder read can fail before Android
    // reports the drive gone, and that drive is not back).
    function showDriveRemoved(key, seenGone) {
        showRoots();
        d.removedKey = key;
        d.removedSeenGone = seenGone;
        setNotice(qsTr("The drive was removed."));
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
        d.hiddenByName = 0;
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
        // A tap again (a slow drive) would only start the check over and
        // tie up another reader: the first one is still going. While DSD-neo
        // closes for a restart, a pick would start a copy the close cuts short.
        if (d.opening !== null || restartCloser.active)
            return;
        d.notice = "";
        d.opening = entry;
        d.handoffRetried = false;
        requestHandoff();
        startReading();
    }
    function requestHandoff() {
        d.handoffId = decoderHost.requestFileHandoff(d.session, d.root.key, d.generation, d.opening.reference);
    }
    function askAccess() {
        d.accessBlocked = false;
        d.accessDenied = false;
        d.accessReturnSeen = false;
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
        // An answer that lands after a folder was opened from the rows it was
        // replacing is only a check of that folder's location: it must not
        // stop the folder's reading line, report a problem with places the
        // user has left, or take the focus the folder's listing will give.
        var behind = d.checkingBehind || d.view === "folder";
        d.rootsId = 0;
        d.checkingBehind = false;
        if (!behind)
            readingDelay.stop();
        if (result.status === "cancelled")
            return;
        if (result.status !== "ok") {
            if (!behind) {
                d.rootsStatus = result.status;
                Navigation.announce(screen, locations.problemLine, problemText(result.status));
            } else if (handoffWaiting()) {
                endOpening(problemText(result.status));
            }
            return;
        }
        d.roots = result.roots || [];
        d.generation = result.generation || 0;
        d.rootsStatus = "";
        if (behind) {
            folderRootChecked();
            if (handoffWaiting())
                requestHandoff();
            return;
        }
        removedDriveBack();
        Qt.callLater(focusFirstRow);
    }
    // The removed drive is listed again (put back, or mounted again): its
    // notice no longer holds.
    function removedDriveBack() {
        if (d.removedKey.length === 0)
            return;
        var listed = d.roots.some(function (root) {
            return root.key === d.removedKey && root.state !== "removed";
        });
        if (!listed) {
            d.removedSeenGone = true;
        } else if (d.removedSeenGone) {
            d.removedKey = "";
            d.notice = "";
        }
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
        if (d.root.kind === "removable")
            showDriveRemoved(d.root.key, true);
        else
            showRoots();
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
            showDriveRemoved(d.root.key, false);
            return;
        }
        if (status === "ok") {
            d.entries = result.entries || [];
            d.snapshot = result.snapshot || 0;
            d.more = result.more === true;
            d.hiddenByFilter = result.hiddenByFilter || 0;
            d.hiddenByName = result.hiddenByName || 0;
        }
        d.folderStatus = status;
        if (screen.stateIsProblem)
            Navigation.announce(screen, folder.stateLine, screen.stateText);
        Qt.callLater(focusFirstRow);
    }
    function pageAnswered(result) {
        d.pageId = 0;
        // A file tapped meanwhile still gets its line.
        if (d.opening === null)
            readingDelay.stop();
        var status = result.status;
        if (status === "cancelled")
            return;
        if (status === "ok") {
            d.entries = d.entries.concat(result.entries || []);
            d.more = result.more === true;
            return;
        }
        // The folder was read too long ago to page on (two idle minutes): it
        // is read again from the top, on purpose, rather than paged on from
        // a listing that may no longer match the folder.
        if (status === "expired") {
            requestListing();
            return;
        }
        d.pageProblem = problemText(status);
        Navigation.announce(screen, folder.pageProblemLine, d.pageProblem);
    }
    function handoffAnswered(result) {
        d.handoffId = 0;
        if (result.ok === true) {
            d.opening = null;
            FileBrowser.finish(d.token, result.reference, result.name);
            return;
        }
        var reason = result.reason || "";
        if (reason === "cancelled") {
            d.opening = null;
            return;
        }
        // The device's drives changed between the listing and the tap (a
        // drive plugged in): the file is most likely fine. The locations are
        // read again behind the open folder and the check asked once more
        // (rootsAnswered); only a second stale answer says so.
        if (reason === "stale" && !d.handoffRetried) {
            d.handoffRetried = true;
            requestRoots(true);
            return;
        }
        if (reason === "stale")
            showRoots();
        endOpening(reason === "busy" || reason === "failed" || reason === "timedOut" ? problemText(reason) : qsTr("DSD-neo can't open that file. Pick another one."));
    }
    // The tapped file waits for the locations to be read again before its
    // second check.
    function handoffWaiting() {
        return d.opening !== null && d.handoffId === 0 && d.view === "folder";
    }
    function endOpening(problem) {
        d.opening = null;
        setNotice(problem);
    }
    function accessAnswered(outcome) {
        d.accessId = 0;
        var returned = d.accessReturnSeen;
        d.accessReturnSeen = false;
        if (outcome === "settingsOpened") {
            d.accessAwaitingReturn = true;
            // Settings may have closed again (an OEM screen that finishes at
            // once) before this answer was read: that return already happened.
            if (returned)
                accessReturned();
        } else if (outcome === "denied") {
            // Android 10, "Don't allow" once: the dialog can still show, so
            // Allow access is the way to ask again, not App info.
            if (!screen.settingsRoute)
                d.accessDenied = true;
        } else if (outcome === "noRoute" || outcome === "failed") {
            // Android 11 and later: the computer route. Android 10: App info,
            // the one place left to allow it once the dialog cannot show.
            if (screen.adbOffered)
                d.accessHelp = true;
            else if (!screen.settingsRoute)
                d.accessBlocked = true;
        } else if (outcome === "blocked") {
            d.accessBlocked = true;
        } else if (outcome === "granted" && d.view === "roots") {
            requestRoots(false);
        }
    }
    // Back from Settings without the switch turned on: say how else to allow it.
    // The app is in front again. While an access request is out, that is a
    // return from the screen it opened, whichever comes first: this or its
    // answer.
    function applicationActivated() {
        if (d.accessId !== 0)
            d.accessReturnSeen = true;
        accessReturned();
    }
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
    function hiddenByNameText(count) {
        return count === 1 ? qsTr("1 item here isn't listed because DSD-neo can't read its name. Rename it on a computer to see it.") : qsTr("%1 items here aren't listed because DSD-neo can't read their names. Rename them on a computer to see them.").arg(count);
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
            // The line about the names it cannot read says why it is empty.
            if (d.hiddenByName > 0)
                return "";
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
        var first = d.view === "roots" ? locations.firstRow() : folder.firstRow();
        (first || screen).forceActiveFocus();
    }
    function revealFocus() {
        var window = screen.Window.window;
        var focus = window ? window.activeFocusItem : null;
        if (!focus || !Navigation.contains(screen, focus))
            return;
        locations.revealFocus(focus);
        folder.revealFocus(focus);
        crumbList.reveal(focus);
    }
    Connections {
        target: screen.Window.window
        function onActiveFocusItemChanged() {
            if (screen.shown)
                Qt.callLater(screen.revealFocus);
        }
    }

    // The browser stays the top modal while it shows. A sheet drawn under it
    // (an owner's, one that opened on its own) must not take the top: every
    // control here would then fail Navigation.allows() and Back would go to
    // that hidden sheet, so the browser would look frozen. It waits under the
    // browser and is on top once the pick ends. A sheet drawn above the
    // browser (a Main-level ModalSheet, z 100 and up) is what the user sees,
    // so it keeps the top until it closes.
    function keepOnTop() {
        var top = Navigation.topModal;
        if (d.token === 0 || !top || top === screen || Navigation.contains(screen, top) || drawnAbove(top))
            return;
        Navigation.addModal(screen);
        // The sheet gave itself the focus as it opened.
        var window = screen.Window.window;
        if (window && !Navigation.contains(screen, window.activeFocusItem))
            screen.forceActiveFocus();
    }
    // Whether @p item is drawn above this screen: where their parent chains
    // meet, the branch with the higher z, or at the same z the later child.
    function drawnAbove(item) {
        var mine = [];
        for (var node = screen; node; node = node.parent)
            mine.push(node);
        var branch = item;
        for (var above = item.parent; above; above = above.parent) {
            var at = mine.indexOf(above);
            if (at > 0) {
                var ours = mine[at - 1];
                if (branch.z !== ours.z)
                    return branch.z > ours.z;
                return above.children.indexOf(branch) > above.children.indexOf(ours);
            }
            if (at === 0)
                return true;
            branch = above;
        }
        return false;
    }
    Connections {
        target: Navigation
        // Later, not inside the change: the stack cannot be written again
        // while its top is being told, and the sheet's opening finishes first.
        function onTopModalChanged() {
            Qt.callLater(screen.keepOnTop);
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
                screen.applicationActivated();
        }
    }
    Connections {
        target: restartCloser
        function onFinished(outcome) {
            if (!screen.shown || outcome === "closing")
                return;
            // A copy that began while listening stopped: the close gave way to it.
            if (outcome === "refused")
                d.closeProblem = qsTr("Wait for the copy to finish, then close DSD-neo.");
            else
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
        else if (request && request.kind === "backup")
            lines.push(qsTr("This restore is dropped."));
        var draft = unsavedDraft();
        if (draft === "add")
            lines.push(qsTr("The system you're adding is dropped."));
        else if (draft === "edit")
            lines.push(qsTr("Your changes to this system are dropped."));
        if (backup.restorePending === true)
            lines.push(qsTr("Your backup is restored when DSD-neo opens again."));
        lines.push(qsTr("Then open DSD-neo again."));
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

    // The browse's state, which the views (FileBrowserLocations,
    // FileBrowserFolder) read; only this screen writes it.
    readonly property QtObject internals: QtObject {
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
        // Items left out because their names are not text DSD-neo can read.
        property int hiddenByName: 0
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
        // The file being checked (null for none), and whether its check was
        // already asked again after a stale answer.
        property var opening: null
        property bool handoffRetried: false
        property bool checkingBehind: false
        property bool refreshingAccess: false
        property var seenAccess: ({})
        property bool readingShown: false
        property bool focusFirst: false
        property string notice: ""
        // The removed drive the notice is about, "" for none, and whether a
        // list of the locations has been read without it since.
        property string removedKey: ""
        property bool removedSeenGone: false
        property bool accessAwaitingReturn: false
        property bool accessHelp: false
        property bool accessBlocked: false
        property bool accessDenied: false
        // The app came back to the front while an access request was out.
        property bool accessReturnSeen: false
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
        onTriggered: d.readingShown = d.rootsId !== 0 || d.listingId !== 0 || d.pageId !== 0 || d.opening !== null
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
            height: Math.max(backButton.height, screen.compactHeader ? crumbList.height : title.height)

            IconButton {
                id: backButton
                objectName: "fileBrowserBack"
                icon: "back"
                x: 0
                y: title.visible && title.lineCount > 1 ? 0 : (header.height - height) / 2
                onClicked: screen.requestDismiss()
            }
            ReadableText {
                id: title
                objectName: "fileBrowserTitle"
                visible: !screen.compactHeader
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

        // Where the open folder is: each step up is a button to it. One line
        // that scrolls sideways, as the talkgroup filter chips do, so a deep
        // path of long names never takes the folder's rows' room; it shows
        // its end, the open folder, and a crumb that takes the focus scrolls
        // into view.
        Item {
            id: crumbSlot
            width: parent.width
            height: visible ? crumbList.height : 0
            visible: d.view === "folder" && !screen.compactHeader
        }
        Flickable {
            id: crumbList
            objectName: "fileBrowserCrumbs"
            parent: screen.compactHeader ? header : crumbSlot
            x: screen.compactHeader ? backButton.width + 14 : (crumbSlot.width - width) / 2
            y: screen.compactHeader ? (header.height - height) / 2 : 0
            width: screen.compactHeader ? header.width - backButton.width - 14 : screen.contentWidth
            height: Math.max(Theme.minimumTouchSize, Theme.fontSize(14) * 1.4 + 12)
            visible: d.view === "folder"
            clip: true
            contentWidth: crumbRow.width
            contentHeight: height
            flickableDirection: Flickable.HorizontalFlick
            boundsBehavior: Flickable.StopAtBounds
            readonly property int count: crumbRepeater.count
            // The end, the open folder, shows whenever the path or the room
            // changes.
            onContentWidthChanged: showEnd()
            onWidthChanged: showEnd()
            function showEnd() {
                contentX = Math.max(0, contentWidth - width);
            }
            function reveal(item) {
                if (!Navigation.contains(crumbRow, item))
                    return;
                var left = item.mapToItem(crumbRow, 0, 0).x;
                if (left < contentX)
                    contentX = left;
                else if (left + item.width > contentX + width)
                    contentX = Math.min(contentWidth - width, left + item.width - width);
            }

            Row {
                id: crumbRow
                height: crumbList.height

                Repeater {
                    id: crumbRepeater
                    model: screen.crumbs()

                    delegate: Row {
                        id: crumbStep
                        required property int index
                        required property var modelData
                        readonly property bool current: index === crumbRepeater.count - 1
                        height: crumbList.height

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
                            // No crumb is wider than the line: a longer name is cut
                            // in its middle on screen, and read whole.
                            readonly property real room: crumbList.width - (crumbStep.index > 0 ? 20 : 0)
                            // The first crumb lines up with the column's other
                            // lines; every crumb pads its sides alike, so the gaps
                            // around each separator match.
                            readonly property real leading: crumbStep.index > 0 ? 8 : 0
                            readonly property real trailing: 8
                            width: Math.min(room, crumbLabel.implicitWidth + leading + trailing)
                            height: crumbList.height
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
                            Keys.onPressed: function (event) {
                                if (Navigation.activationPress(event))
                                    activate();
                            }
                            // Inside the crumb: the line clips at its edges.
                            FocusFrame {
                                anchors.margins: 0
                            }
                            Rectangle {
                                anchors.fill: parent
                                radius: Theme.radiusButton
                                color: crumbTap.pressed && !crumbStep.current ? Theme.chipSelectedFill : "transparent"
                            }
                            Text {
                                id: crumbLabel
                                objectName: "fileBrowserCrumbLabel"
                                x: crumb.leading
                                anchors.verticalCenter: parent.verticalCenter
                                width: Math.min(implicitWidth, crumb.room - crumb.leading - crumb.trailing)
                                text: crumbStep.modelData.title
                                textFormat: Text.PlainText
                                elide: Text.ElideMiddle
                                font.family: Theme.sans
                                font.pixelSize: Theme.fontSize(14)
                                font.weight: crumbStep.current ? Font.DemiBold : Font.Normal
                                color: crumbStep.current ? Theme.textPrimary : Theme.cyan
                            }
                            GatedTapHandler {
                                id: crumbTap
                                enabled: !crumbStep.current
                                onAllowedTap: crumb.activate()
                            }
                        }
                    }
                }
            }
        }

        FileBrowserLine {
            id: noticeLine
            objectName: "fileBrowserNotice"
            x: (parent.width - width) / 2
            width: screen.contentWidth
            problem: true
            visible: d.notice.length > 0
            text: d.notice
        }
        FileBrowserLine {
            objectName: "fileBrowserReading"
            x: (parent.width - width) / 2
            width: screen.contentWidth
            visible: screen.reading
            text: qsTr("Reading folder…")
        }
        FileBrowserLine {
            objectName: "fileBrowserClosingNotice"
            x: (parent.width - width) / 2
            width: screen.contentWidth
            visible: d.view === "folder" && restartCloser.active
            text: qsTr("DSD-neo is closing, so files can't be picked now.")
        }
        FileBrowserLine {
            objectName: "fileBrowserOpening"
            x: (parent.width - width) / 2
            width: screen.contentWidth
            visible: d.readingShown && screen.opening
            text: d.opening ? qsTr("Opening %1…").arg(d.opening.name) : ""
        }
        FileBrowserLine {
            objectName: "fileBrowserHiddenByName"
            x: (parent.width - width) / 2
            width: screen.contentWidth
            visible: d.view === "folder" && d.folderStatus === "ok" && d.hiddenByName > 0
            text: visible ? hiddenByNameText(d.hiddenByName) : ""
        }
    }

    // ---- the views ----

    FileBrowserLocations {
        id: locations
        browser: screen
        anchors.top: top.bottom
        anchors.topMargin: Theme.gap - contentInset
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }
    FileBrowserFolder {
        id: folder
        browser: screen
        anchors.top: top.bottom
        anchors.topMargin: Theme.gap
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: showAllPanel.top
        anchors.bottomMargin: Theme.gap
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
