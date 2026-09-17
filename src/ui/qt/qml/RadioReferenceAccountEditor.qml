// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

Column {
    id: editor

    default property alias sessionFields: sessionContent.data
    property Item nextUsernameField: null
    property string usernameLabel: qsTr("Username")
    property string appKeyLabel: qsTr("Application key")
    property string appKeyHeading: ""
    property string passwordHint: qsTr("The password is asked for once per app session and is never saved. A RadioReference premium subscription is required.")
    signal appKeyEdited
    spacing: 10

    PlexTextField {
        id: usernameField
        objectName: "radioReferenceUsernameField"
        width: parent.width
        label: editor.usernameLabel
        nextField: editor.nextUsernameField
        text: prefs.rrUsername
        placeholderText: qsTr("radioreference.com username")
        inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
        onEditingFinished: prefs.rrUsername = text
    }

    Column {
        id: sessionContent
        visible: children.length > 0
        width: parent.width
        spacing: 10
    }

    Text {
        objectName: "radioReferencePasswordHint"
        width: parent.width
        text: editor.passwordHint
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(12)
        color: Theme.textSubdued
        wrapMode: Text.Wrap
    }

    Text {
        objectName: "radioReferenceAppKeyHeading"
        width: parent.width
        visible: !radioReference.buildHasAppKey && editor.appKeyHeading.length > 0
        text: editor.appKeyHeading
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: Theme.textSecondary
    }

    PlexTextField {
        objectName: "radioReferenceAppKeyField"
        width: parent.width
        visible: !radioReference.buildHasAppKey
        label: editor.appKeyLabel
        mono: true
        text: prefs.rrAppKey
        placeholderText: qsTr("application key")
        inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
        onEditingFinished: {
            prefs.rrAppKey = text;
            editor.appKeyEdited();
        }
    }

    Text {
        width: parent.width
        visible: !radioReference.buildHasAppKey
        text: qsTr("This build carries no application key. Request one at <a href=\"https://www.radioreference.com/account/api/apply\">radioreference.com/account/api/apply</a>.")
        textFormat: Text.StyledText
        linkColor: Theme.cyan
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(12)
        color: Theme.textSubdued
        wrapMode: Text.Wrap
        onLinkActivated: function (link) {
            Qt.openUrlExternally(link);
        }
    }
}
