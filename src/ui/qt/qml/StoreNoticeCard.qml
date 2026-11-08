// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// Issue #828: the files the app couldn't read this run, and what became of
// each, at the top of Home until dismissed. `storeNotices` is process-lifetime
// (src/ui/qt/store_notices.h), so a dismissal holds across a rebuilt screen;
// a notice that changes later (its file kept aside after all) shows again.
// The card is one alert named by everything it says; its lines are plain
// Text, which a screen reader reaches only through it.
UiPanel {
    id: card
    objectName: "storeNoticeCard"

    readonly property var notices: typeof storeNotices !== "undefined" && storeNotices ? storeNotices.notices : []
    readonly property string speech: notices.map(spoken).join(" ")
    // The notices already read out, by file, state and kept names, so each is
    // heard once: a dismissal or a second file does not repeat the first.
    property var heard: ({})
    property int announcements: 0

    visible: notices.length > 0
    height: body.implicitHeight + 2 * Theme.cardPadding
    border.color: Theme.alertBorder
    Accessible.role: Accessible.AlertMessage
    Accessible.name: speech
    readonly property bool navigationAllowed: Navigation.allows(card)
    Accessible.ignored: !navigationAllowed || !visible

    function title(notice) {
        if (notice.state === "leftInPlace")
            return qsTr("Signal search history from an older version couldn't be read");
        return qsTr("%1 couldn't be read completely").arg(Util.storeLabel(notice.store));
    }
    // What became of the file. "May be missing": a file can also be empty or
    // unreadable from its first byte, and nothing here knows it held anything.
    function detail(notice) {
        var kept = String(notice.kept || "");
        if (notice.state === "leftInPlace")
            return qsTr("It was left in place.");
        if (notice.state === "kept")
            return qsTr("Some entries may be missing. The file was kept as %1.").arg(kept);
        if (notice.state === "unsaved")
            return kept.length ? qsTr("Some entries may be missing. The file was kept as %1, but changes made now won't be saved.").arg(kept)
                : qsTr("Some entries may be missing. The file couldn't be kept aside, so changes made now won't be saved.");
        var earlier = String(notice.earlierKept || "");
        return earlier.length ? qsTr("Some entries may be missing. The file won't be overwritten. An earlier copy was kept as %1.").arg(earlier)
            : qsTr("Some entries may be missing. The file won't be overwritten.");
    }
    function spoken(notice) {
        return title(notice) + ". " + detail(notice);
    }
    function key(notice) {
        return [notice.file, notice.state, notice.kept, notice.earlierKept].join("\n");
    }
    function announceNew() {
        if (!visible)
            return;
        var fresh = notices.filter(function (notice) {
            return !heard[key(notice)];
        });
        if (!fresh.length)
            return;
        var next = Object.assign({}, heard);
        for (var notice of fresh)
            next[key(notice)] = true;
        heard = next;
        announcements++;
        if (typeof Accessible.announce === "function")
            Accessible.announce(fresh.map(spoken).join(" "));
    }
    // The names first: each dismissal changes the list being read.
    function dismiss() {
        var files = notices.map(function (notice) {
            return notice.file;
        });
        for (var file of files)
            storeNotices.dismiss(file);
    }
    // Once every binding has the new list; a Timer, which dies with the card,
    // rather than Qt.callLater, whose call would outlive it.
    Timer {
        id: announceLater
        interval: 0
        onTriggered: card.announceNew()
    }
    onSpeechChanged: announceLater.restart()
    onVisibleChanged: announceLater.restart()
    Component.onCompleted: announceLater.restart()

    Column {
        id: body
        x: Theme.cardPadding
        y: Theme.cardPadding
        width: parent.width - 2 * Theme.cardPadding
        spacing: 16

        Repeater {
            model: card.notices
            Column {
                required property var modelData
                width: body.width
                spacing: 8
                Text {
                    objectName: "storeNoticeTitle"
                    width: parent.width
                    text: card.title(parent.modelData)
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(16)
                    font.weight: Font.DemiBold
                    color: Theme.textPrimary
                }
                Text {
                    objectName: "storeNoticeDetail"
                    width: parent.width
                    text: card.detail(parent.modelData)
                    // Names a file the app keeps; plain, as every file name here.
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(13)
                    color: Theme.textSecondary
                }
            }
        }
        OutlineButton {
            objectName: "storeNoticeDismiss"
            width: parent.width
            text: qsTr("Dismiss")
            onClicked: card.dismiss()
        }
    }
}
