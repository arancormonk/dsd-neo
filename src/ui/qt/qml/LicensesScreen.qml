// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Window

// Where Qt's source code is, then the list of every license and notice file
// the package carries (the host's notice index). A file's text is read and
// laid out only while it is open, one file at a time: all of them together
// are near a megabyte of text.
Item {
    id: screen
    signal closed
    // The file shown, a name from the list; empty shows the list.
    property string openFile: ""
    // The row the open file came from, given the keyboard back when it closes.
    property Item openedFrom: null

    function openNotice(name, row) {
        screen.openedFrom = row || null;
        screen.openFile = name;
        fileScroll.contentY = 0;
        screen.forceActiveFocus();
    }
    // Back from a file goes to the list; from the list it leaves the screen.
    function requestClose() {
        if (screen.openFile.length === 0) {
            screen.closed();
            return;
        }
        screen.openFile = "";
        var row = screen.openedFrom;
        screen.openedFrom = null;
        Qt.callLater(function () {
            if (row && screen.visible && Navigation.allows(row))
                row.forceActiveFocus();
            else if (screen.visible)
                screen.forceActiveFocus();
        });
    }

    Keys.onEscapePressed: Navigation.back(screen.Window.window)
    Keys.onBackPressed: Navigation.back(screen.Window.window)
    // The layer clears input during activation; take focus after its bindings settle.
    onVisibleChanged: {
        if (!visible) {
            screen.openFile = "";
            screen.openedFrom = null;
        }
        if (visible) Qt.callLater(function () {
            if (screen.visible && Navigation.allows(screen))
                screen.forceActiveFocus();
        });
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    Item {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.screenPadding
        height: 46

        IconButton {
            id: back
            objectName: "licensesBack"
            icon: "back"
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            onClicked: screen.requestClose()
        }

        ReadableText {
            objectName: "licensesTitle"
            heading: true
            anchors.left: back.right
            anchors.leftMargin: 14
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: screen.openFile.length > 0 ? screen.openFile : qsTr("Open source licenses")
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(22)
            font.weight: Font.Bold
            font.letterSpacing: -0.22
            color: Theme.textPrimary
            elide: Text.ElideRight
        }
    }

    PlexFlickable {
        id: scroll
        objectName: "licensesScroll"
        accessibleName: qsTr("Open source licenses")
        visible: screen.openFile.length === 0
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.screenPadding
        contentHeight: listColumn.height
        clip: true

        Column {
            id: listColumn
            width: scroll.width
            spacing: Theme.gap

            TextEdit {
                id: qtSource
                // Issue #902: its reach for the keyboard (PressGate).
                readonly property bool inReach: qtSource.navigationAllowed && qtSource.visible
                objectName: "licensesQtSource"
                width: parent.width
                text: screen.visible ? decoderHost.qtSourceNotice() : ""
                readOnly: true
                selectByMouse: true
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                color: Theme.textPrimary
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(14)
                // A text field is a node of its own: its own gate, so a sheet
                // that opens over the screen (Open with) takes it out of the tree.
                readonly property bool navigationAllowed: Navigation.allows(qtSource)
                Accessible.ignored: !navigationAllowed || !visible
                // Issue #902: Qt reads a text field's words, and makes it a
                // stop, whatever its flags; covered or hidden, a plain pane
                // (see PlexTextField).
                Accessible.role: navigationAllowed && visible ? Accessible.EditableText : Accessible.Pane
                Accessible.focusable: navigationAllowed && visible
            }

            ReadableText {
                objectName: "licensesMissing"
                width: parent.width
                visible: files.count === 0
                text: qsTr("The license and notice files are missing from this package.")
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(14)
                color: Theme.textSubdued
            }

            UiPanel {
                width: parent.width
                height: filesColumn.height
                visible: files.count > 0

                Column {
                    id: filesColumn
                    anchors.left: parent.left
                    anchors.right: parent.right

                    Repeater {
                        id: files
                        objectName: "licenseFileRows"
                        model: screen.visible ? decoderHost.licenseFiles() : []

                        DisclosureRow {
                            id: fileRow
                            required property string modelData
                            required property int index
                            title: modelData
                            showDivider: index < files.count - 1
                            onTapped: screen.openNotice(modelData, fileRow)
                        }
                    }
                }
            }
        }
    }

    PlexFlickable {
        id: fileScroll
        objectName: "licenseFileScroll"
        accessibleName: screen.openFile
        visible: screen.openFile.length > 0
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.screenPadding
        contentHeight: fileText.height
        clip: true

        TextEdit {
            id: fileText
            // Issue #902: its reach for the keyboard (PressGate).
            readonly property bool inReach: fileText.navigationAllowed && fileText.visible
            objectName: "licenseFileText"
            width: fileScroll.width
            text: screen.visible && screen.openFile.length > 0 ? decoderHost.licenseFileText(screen.openFile) : ""
            readOnly: true
            selectByMouse: true
            textFormat: TextEdit.PlainText
            wrapMode: TextEdit.Wrap
            color: Theme.textPrimary
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(14)
            // A text field is a node of its own: its own gate (see the Qt source text above).
            readonly property bool navigationAllowed: Navigation.allows(fileText)
            Accessible.ignored: !navigationAllowed || !visible
            Accessible.role: navigationAllowed && visible ? Accessible.EditableText : Accessible.Pane
            Accessible.focusable: navigationAllowed && visible
        }
    }
}
