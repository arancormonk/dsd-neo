// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// The RadioReference sign-in form, shared by the import screen's credentials
// card and Settings → Account → RadioReference: username, password, the
// application key where this build needs one, "Check account" and the
// account check's verdict.
//
// The password is the session's, held by the model only (hasPassword). The
// field is a local draft: cleared and masked again whenever the screen shows
// or leaves, committed only when it holds something, so the two forms never
// disagree and tapping in and out never clears the session password. With the
// draft empty and a session password held, the placeholder says so.
//
// While `active` the form is the one a password manager fills: it takes a
// signInAutofill token, tracks its two inputs (never the application key) and
// applies a fill for that token as one pair, checked once.
Column {
    id: editor

    // The screen's own visibility. Only one of the two forms is on screen at a
    // time, and begin() from the one now shown retires the other's token.
    property bool active: false
    // The import screen reports a failed check in its notice line, so its form
    // shows only the checking and verified lines; Settings shows them all.
    property bool showsFailures: true
    property string usernameLabel: qsTr("Username")
    property string appKeyLabel: qsTr("Application key")
    property string appKeyHeading: ""
    property bool revealed: false
    // The live signInAutofill token while active; 0 otherwise.
    property int autofillToken: 0
    // The visibility last reported with the fields (see settleVisibility()).
    property bool shownToRelay: false
    // Set while a fill or the Check tap commits the fields itself, so the
    // fields' own handlers do not check the account a second time.
    property bool fillApplying: false
    property bool checkCommitting: false
    // "Check account" found a field empty; cleared by the next check or edit.
    property string missingNotice: ""
    spacing: 10

    // RadioReferenceModel::AccountState, which QML sees only as an int.
    readonly property int accountNone: 0
    readonly property int accountChecking: 1
    readonly property int accountVerified: 2
    readonly property int accountRejected: 3
    readonly property int accountExpired: 4
    readonly property int accountFailed: 5

    readonly property string resultText: {
        if (editor.missingNotice.length > 0)
            return editor.missingNotice;
        switch (radioReference.accountState) {
        case editor.accountChecking:
            return qsTr("Checking your RadioReference account…");
        case editor.accountVerified:
            return qsTr("Account verified.");
        case editor.accountRejected:
            if (!editor.showsFailures)
                return "";
            return radioReference.buildHasAppKey ? qsTr("RadioReference did not accept that username or password.") : qsTr("RadioReference did not accept that username, password or application key.");
        case editor.accountExpired:
            return editor.showsFailures ? qsTr("This account's RadioReference premium subscription has expired.") : "";
        case editor.accountFailed:
            if (!editor.showsFailures)
                return "";
            return radioReference.accountMessage.length > 0 ? radioReference.accountMessage : qsTr("The account could not be checked. Try again.");
        default:
            return "";
        }
    }
    readonly property bool resultIsProblem: editor.missingNotice.length > 0 || radioReference.accountState >= editor.accountRejected

    function resetDraft() {
        passwordField.text = "";
        editor.revealed = false;
    }

    function trackFields() {
        editor.shownToRelay = editor.visible;
        if (editor.autofillToken <= 0)
            return;
        signInAutofill.track(editor.autofillToken, 1, usernameField.input, editor.visible);
        signInAutofill.track(editor.autofillToken, 2, passwordField.input, editor.visible);
    }

    function startSession() {
        editor.resetDraft();
        editor.missingNotice = "";
        editor.autofillToken = signInAutofill.begin();
        editor.trackFields();
        if (editor.autofillToken > 0)
            signInAutofill.note(editor.autofillToken, 1, usernameField.text);
    }

    function endSession() {
        editor.resetDraft();
        editor.missingNotice = "";
        if (editor.autofillToken > 0)
            signInAutofill.end(editor.autofillToken);
        editor.autofillToken = 0;
    }

    onActiveChanged: {
        if (editor.active)
            editor.startSession();
        else
            editor.endSession();
    }
    Component.onCompleted: {
        if (editor.active)
            editor.startSession();
    }
    // The import screen's card hides while the screen stays up: the fields
    // leave the password manager's view, the session stays. Settled first:
    // within one commit or fill the card can hide and show again (the
    // credentials turn ready a moment before the check starts), and that
    // flicker must neither reach the password manager, which may end its
    // session when every field goes, nor mask a revealed password.
    onVisibleChanged: visibilitySettle.restart()
    // A child timer rather than Qt.callLater(): it goes with the editor, so a
    // form torn down meanwhile is never called back.
    Timer {
        id: visibilitySettle
        interval: 0
        onTriggered: editor.settleVisibility()
    }

    function settleVisibility() {
        if (editor.visible === editor.shownToRelay)
            return;
        // Hidden with no field in use: the credentials are committed (the
        // card hides only once they are, or after Done commits them), so the
        // draft goes and Change shows the session placeholder.
        if (!editor.visible) {
            editor.revealed = false;
            if (!usernameField.input.activeFocus && !passwordField.input.activeFocus)
                passwordField.text = "";
        }
        editor.trackFields();
    }

    /** Username editingFinished: store what the field shows if it changed. */
    function commitUsername() {
        if (usernameField.text !== prefs.rrUsername)
            prefs.rrUsername = usernameField.text;
        if (editor.autofillToken > 0)
            signInAutofill.note(editor.autofillToken, 1, usernameField.text);
    }

    /** Password editingFinished: an empty draft keeps the session password. */
    function commitPassword() {
        if (passwordField.text.length > 0)
            radioReference.setPassword(passwordField.text);
    }

    /**
     * A field's commit checks the account once every field is filled, unless
     * these credentials were already checked: every edit resets the verdict to
     * none, so a commit that changed nothing (focus leaving an untouched field,
     * the card hiding) starts no request. Check account always asks.
     */
    function fieldCommitted() {
        editor.missingNotice = "";
        if (editor.fillApplying || editor.checkCommitting)
            return;
        if (radioReference.credentialsReady && radioReference.accountState === editor.accountNone)
            radioReference.checkAccount();
    }

    /**
     * The "Check account" button. A TapHandler takes no focus, so the field the
     * user just typed in has not committed yet: move the focus off it first
     * (under checkCommitting, so its commit does not ask too), then check what
     * the form holds, with the session password when the draft is empty.
     */
    function checkTapped() {
        editor.checkCommitting = true;
        editor.forceActiveFocus();
        Qt.inputMethod.hide();
        editor.checkCommitting = false;
        if (!radioReference.credentialsReady) {
            editor.missingNotice = qsTr("Fill in every field above first.");
            return;
        }
        editor.missingNotice = "";
        radioReference.checkAccount();
    }

    /**
     * A password manager filled this form. The relay has already stored what it
     * filled (the username through prefs, which the field's binding shows; the
     * password in the model, never here). A field it left alone commits what it
     * shows, and the pair is then checked once.
     *
     * A filled username equal to the stored one changes nothing in prefs, so
     * the binding alone would leave a different draft in the field, committed
     * over the fill once the field loses focus. So the field is pointed at
     * prefs again first, before the check's busy sheet can take the focus,
     * with any word still composing finished so it cannot land afterwards.
     */
    function applyFill(hasUsername, hasPassword) {
        editor.fillApplying = true;
        if (hasUsername) {
            Navigation.commitComposing(false);
            usernameField.text = Qt.binding(function () {
                return prefs.rrUsername;
            });
        } else {
            editor.commitUsername();
        }
        if (hasPassword)
            editor.resetDraft();
        else
            editor.commitPassword();
        editor.fillApplying = false;
        editor.missingNotice = "";
        if (radioReference.credentialsReady)
            radioReference.checkAccount();
    }

    Connections {
        target: signInAutofill
        function onFilled(token, hasUsername, hasPassword) {
            if (token > 0 && token === editor.autofillToken)
                editor.applyFill(hasUsername, hasPassword);
        }
    }

    PlexTextField {
        id: usernameField
        objectName: "radioReferenceUsernameField"
        width: parent.width
        label: editor.usernameLabel
        nextField: passwordField
        text: prefs.rrUsername
        placeholderText: qsTr("radioreference.com username")
        inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
        onEditingFinished: {
            editor.commitUsername();
            editor.fieldCommitted();
        }
    }

    Row {
        width: parent.width
        spacing: 8

        PlexTextField {
            id: passwordField
            objectName: "radioReferencePasswordField"
            width: parent.width - reveal.width - parent.spacing
            label: qsTr("Password")
            input.echoMode: editor.revealed ? TextInput.Normal : TextInput.Password
            // No example: under the label it would only repeat the word. With
            // the draft empty and a session password held, it says so.
            placeholderText: passwordField.text.length === 0 && radioReference.hasPassword ? qsTr("Entered for this session") : ""
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText | Qt.ImhSensitiveData
            // Commits on Enter or focus loss, never per keystroke.
            onEditingFinished: {
                if (passwordField.text.length === 0)
                    return;
                editor.commitPassword();
                editor.fieldCommitted();
            }
        }

        // Keeps the field's editing open: a reveal is not a commit, so it
        // never checks the account.
        RevealButton {
            id: reveal
            objectName: "radioReferencePasswordReveal"
            // Level with the input, below the field's label.
            y: passwordField.labelHeight
            finishesEditing: false
            revealed: editor.revealed
            revealedName: qsTr("Hide password")
            hiddenName: qsTr("Show password")
            onClicked: editor.revealed = !editor.revealed
        }
    }

    ReadableText {
        objectName: "radioReferencePasswordHint"
        width: parent.width
        text: qsTr("DSD-neo keeps your password only until the app closes and never saves it. A RadioReference premium subscription is required.")
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(12)
        color: Theme.textSubdued
        wrapMode: Text.Wrap
    }

    ReadableText {
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
            editor.fieldCommitted();
        }
    }

    // A link a screen reader can open (issue #782): one node, the wrapper,
    // read as the words the line shows and pressed as the link in it. The
    // line itself stays a plain Text with no accessible interface, so Qt adds
    // no second, unpressable hyperlink node for its anchor.
    Item {
        id: appKeyRequest
        objectName: "radioReferenceAppKeyLink"
        width: parent.width
        height: appKeyRequestText.height
        visible: !radioReference.buildHasAppKey
        readonly property string linkUrl: (appKeyRequestText.text.match(/href="([^"]+)"/) || ["", ""])[1]
        Accessible.role: Accessible.Link
        readonly property bool navigationAllowed: Navigation.allows(appKeyRequest)
        Accessible.ignored: !navigationAllowed || !visible
        Accessible.name: Accessible.ignored ? "" : appKeyRequestText.text.replace(/<[^>]*>/g, "")
        Accessible.onPressAction: {
            if (Navigation.allows(appKeyRequest) && linkUrl.length > 0)
                Qt.openUrlExternally(linkUrl);
        }

        Text {
            id: appKeyRequestText
            width: parent.width
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

    OutlineButton {
        objectName: "radioReferenceCheckAccountButton"
        width: parent.width
        // Deliberately not gated on credentialsReady: the field the user is
        // still typing in has not committed when they reach for this button,
        // and gating on the committed state greys it out exactly when the last
        // answer is the one still sitting in it. checkTapped() moves the focus
        // off that field itself, so the button only finishes the word.
        enabled: !radioReference.busy
        finishesEditing: false
        text: qsTr("Check account")
        onClicked: editor.checkTapped()
    }

    ReadableText {
        objectName: "radioReferenceAccountResult"
        width: parent.width
        visible: text.length > 0
        text: editor.resultText
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: editor.resultIsProblem ? Theme.magenta : Theme.textSubdued
        wrapMode: Text.Wrap
    }
}
