// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

Column {
    id: selector
    property string profileUid: ""
    property string protocol: "mixed"
    property var overlayParent: parent
    readonly property bool available: typeof decryptionProfiles !== "undefined" && decryptionProfiles !== null
    readonly property var profile: {
        if (!available)
            return ({});
        var count = decryptionProfiles.count;
        return decryptionProfiles.get(profileUid);
    }
    signal selected(string uid)
    spacing: 8
    visible: available
    DisclosureRow {
        title: qsTr("Decryption keys")
        subtitle: selector.profile.label || (selector.profileUid.length ? qsTr("Profile missing — choose another profile") : qsTr("No profile selected"))
        onTapped: {
            var profiles = decryptionProfiles.entries().filter(function (profile) {
                return profile.legacy || selector.protocol === "mixed" || profile.protocol === "mixed" || profile.protocol === selector.protocol;
            });
            choices.open(qsTr("Decryption profile"), [qsTr("No profile"), qsTr("Create profile")].concat(profiles.map(function (profile) {
                return profile.label;
            })), function (index) {
                if (index === 0)
                    selector.selected("");
                else if (index === 1)
                    editor.open("", selector.protocol);
                else
                    selector.selected(profiles[index - 2].uid);
            });
        }
    }
    OutlineButton {
        width: parent.width
        visible: selector.profileUid.length > 0
        text: qsTr("Manage profile")
        onClicked: editor.open(selector.profileUid, selector.protocol)
    }
    ChoiceSheet {
        id: choices
        parent: selector.overlayParent
    }
    DecryptionProfileEditor {
        id: editor
        parent: selector.overlayParent
        onSaved: function (uid) {
            selector.selected(uid);
        }
    }
}
