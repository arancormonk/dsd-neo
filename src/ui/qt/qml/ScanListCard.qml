// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import "Util.js" as Util

// Issue #901 (B4): laid out as a saved system's card is (HomeScreen), by the
// same rules (Util.listenCardActions(), Util.listenCardRows()): the insets,
// the bold title, the labels 10 px above the middle of the actions' box (122
// px at normal text) and Play under More; in Compact view the name over its
// count with Play and More on the title's row. As tall as what it holds.
UiPanel {
    id: card

    property string listName: ""
    property int entryCount: 0
    property bool isDraft: false
    // Issue #901 (D7): Compact view.
    property bool compact: false

    signal play
    signal edit

    objectName: "scanListCard"
    readonly property var actionPlan: Util.listenCardActions({width: width, padding: Theme.cardPadding, compact: compact,
        hasSites: false, siteWidth: 0, playShown: true, playWidth: playButton.implicitWidth,
        moreWidth: editButton.implicitWidth})
    readonly property var rowPlan: Util.listenCardRows(actionPlan, {compact: compact, padding: Theme.cardPadding,
        headLines: nameText.height + labels.spacing + countText.height, labelsHeight: labels.height, siteHeight: 0,
        playHeight: playButton.implicitHeight, moreHeight: editButton.implicitHeight})
    height: rowPlan.height

    Column {
        id: labels
        x: Theme.cardPadding
        y: card.rowPlan.labelsY
        width: card.actionPlan.labelsWidth
        spacing: 4

        ReadableText {
            id: nameText
            width: parent.width
            objectName: "scanListTitle"
            // Compact: one line, cut, as a system card's (G6).
            text: card.compact ? Util.oneLine(card.listName) : card.listName
            elide: Text.ElideRight
            color: Theme.textPrimary
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(17)
            font.weight: Font.Bold
        }

        ReadableText {
            id: countText
            width: parent.width
            objectName: "scanListCount"
            // Compact: one line, cut; its name still says it whole.
            wrapMode: card.compact ? Text.NoWrap : Text.Wrap
            elide: card.compact ? Text.ElideRight : Text.ElideNone
            // Each count a whole string, as HistoryScreen's: no QTranslator is
            // installed, so a %n plural would read "1 entries".
            text: card.isDraft ? qsTr("Not ready · draft") : card.entryCount === 1 ? qsTr("1 entry") : qsTr("%1 entries").arg(card.entryCount)
            color: Theme.textSubdued
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(12)
        }
    }

    PlayCircle {
        id: playButton
        // B11: unnamed while covered or hidden.
        accessibleName: navigationAllowed && visible ? qsTr("Listen to %1").arg(card.listName) : ""
        objectName: "scanListPlay"

        x: card.actionPlan.playX
        y: card.rowPlan.playY
        enabled: !decoderHost.transitioning && !card.isDraft
        onClicked: card.play()
    }

    IconButton {
        id: editButton
        objectName: "scanListManageButton"
        icon: "more"
        accessibleName: navigationAllowed && visible ? qsTr("More options for %1").arg(card.listName) : ""
        x: card.actionPlan.moreX
        y: card.rowPlan.moreY
        onClicked: card.edit()
    }

    GatedTapHandler {
        onAllowedLongPress: card.edit()
    }
}
