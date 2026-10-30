// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #782: every word a screen shows reaches a screen reader.
//
// A plain Text has no accessible interface, so Qt's Android bridge makes no node for it and TalkBack never reads it.
// Its words reach TalkBack only through an accessible node on its own ancestor chain: itself named (ReadableText), or
// a button, row or group whose name, description or value carries them. testContext.unreachedTexts() walks the visible
// items and reports each Text whose words no such node carries; this file fails on any it reports that the allowlist
// below does not explain.
//
// Each screen and sheet is checked twice: as it opens, and with every branch shown at once (every item's `visible`
// forced on), so a line that only some state shows is checked too. With every branch shown, each sheet nested inside
// is checked on its own as the top layer, the way a user reaches it.
Item {
    id: root
    width: 420
    height: 900

    Item {
        id: host
        anchors.fill: parent
    }

    // The receive policy sheet's decoder side, as tst_receive_policy_sheet.qml fakes it.
    QtObject {
        id: receiveBridge
        property int scanRowEditSet: 1
        property int scanRowEditInherit: 2
        property int scanRowEditReset: 3
        function receiveContext() {
            return {filters: [{enabled: true, allow: "1", block: "2"}], names: [{name: "Dispatch", value: "1"}]};
        }
        function scanRowContext() {
            return {active: true, editable: 0x130, label: "Hospital", row: 4, session: 7,
                receive: {filters: [{enabled: false, allow: "", block: ""}], names: []}};
        }
        function receiveError(kind, enabled, allow, block, names) {
            return "";
        }
        function setReceive(kind, enabled, allow, block, names, destination, action) {
            return true;
        }
    }

    // The positive controls' fixture: each way a text is or is not reached.
    Item {
        id: controls
        visible: false
        anchors.fill: parent
        Column {
            Text {
                objectName: "plainText"
                text: "Plain words nobody reads"
            }
            Text {
                objectName: "namedText"
                text: "Words read as static text"
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
            Text {
                objectName: "ignoredText"
                text: "Words a screen reader skips"
                Accessible.ignored: true
            }
            Text {
                objectName: "glyphText"
                text: "›  ·  —"
            }
            Text {
                objectName: "hiddenText"
                visible: false
                text: "Words never on screen"
            }
            Rectangle {
                width: 100
                height: 40
                Accessible.role: Accessible.Button
                Accessible.name: "Apply the change"
                Text {
                    objectName: "buttonLabel"
                    text: "Apply"
                }
            }
            Rectangle {
                width: 100
                height: 40
                Accessible.role: Accessible.Button
                Accessible.name: "Close"
                Text {
                    objectName: "mislabelled"
                    text: "Apply"
                }
            }
        }
    }

    TestCase {
        name: "A11yTextReach"
        when: windowShown

        // Every screen and sheet this harness can build on its own, with how it opens. The pieces they are built from
        // (buttons, tags, rows) are checked inside them, where the node that reads them is.
        readonly property var components: [
            {file: "ActionMenu", open: function (menu) {
                menu.title = "Talkgroup 100";
                menu.actions = [{text: "Hold", description: "Only this talkgroup is heard"}, {text: "Avoid"}];
                menu.visible = true;
            }},
            {file: "AirspyControls"},
            {file: "BackupFlow"},
            {file: "BackupResultCard", open: function (card) {
                card.outcome = {kind: "restored", detail: "Settings and saved systems were restored."};
            }},
            {file: "BandRail"},
            {file: "BottomNav"},
            {file: "CallRecordingPlayer"},
            {file: "CallRecordingsScreen"},
            {file: "ChoiceSheet", open: function (sheet) {
                sheet.open("Pick one", ["First", "Second"], function () {});
            }},
            {file: "ConfirmDialog", open: function (sheet) {
                sheet.title = "Remove the list?";
                sheet.message = "This deletes the list and its settings.";
                sheet.visible = true;
            }},
            {file: "CsvImportFlow"},
            {file: "DecryptionApplyFlow"},
            {file: "DecryptionProfileEditor", open: function (sheet) {
                sheet.open("", "dmr");
            }},
            {file: "DecryptionProfileSelector"},
            {file: "DiagnosticsScreen"},
            {file: "DiscardDialog"},
            {file: "EncryptionEditor"},
            {file: "ExploreSetupScreen"},
            {file: "FailureCard", open: function (card) {
                card.message = "The dongle stopped sending samples.";
                card.source = "RTL-SDR";
            }},
            {file: "FrequencyField"},
            {file: "HistoryDetailSheet", open: function (sheet) {
                sheet.open({uid: "call-1", name: "Fire dispatch", when: 1767225600, systemName: "County P25", systemUid: "",
                    channel: "", tg: "100", src: "2001", sourceName: "", enc: false, emergency: false, detail: "",
                    durationSecs: 12, freqHz: 851012500, accessCode: -1, accessCodeText: ""});
            }},
            {file: "HistoryScreen"},
            {file: "HomeScreen"},
            {file: "ImportsScreen"},
            {file: "KeySheet"},
            {file: "LicensesScreen"},
            {file: "MonitorScreen"},
            {file: "NetworkSheet"},
            {file: "OnboardingScreen"},
            {file: "PassbandSheet"},
            {file: "RadioReferenceAccountEditor"},
            {file: "RadioReferenceAccountScreen"},
            {file: "RadioReferenceScreen"},
            {file: "RadioSheet"},
            {file: "ReceivePolicySheet", properties: {bridge: receiveBridge}},
            {file: "ScanEntryRow", properties: {parseInteger: function (text) { return parseInt(text, 10); },
                frequencyValid: function () { return true; }}},
            {file: "ScanListCard", properties: {listName: "County scan", entryCount: 3}},
            {file: "ScanListScreen"},
            {file: "SettingsScreen"},
            {file: "SignalSearchRangeEditor", open: function (sheet) {
                sheet.visible = true;
                sheet.errorText = "Start must be below end.";
            }},
            {file: "SignalSearchScreen", open: function (screen) {
                screen.localFailure = "The radio is in use by a session.";
            }},
            {file: "SiteChooserSheet"},
            {file: "SiteSheet"},
            {file: "SpectrumScreen"},
            {file: "SquelchSheet"},
            {file: "TalkgroupCard", properties: {idText: "100", name: "Fire dispatch", tags: "Fire", listening: true,
                listed: true, priority: 2, preempt: true, temporaryAvoidCount: 1}},
            {file: "TalkgroupEditSheet"},
            {file: "TalkgroupSaveFlow"},
            {file: "TalkgroupsScreen"},
            {file: "ToneFilterSheet"},
            {file: "WizardScreen"}
        ]

        // Visible text that no node on its own chain reads, each with why. An entry with readBy names the item that
        // reads its words instead (a field reads its label as its name), and the walk checks that that item really
        // does; an entry without one is text with nothing to read. Keyed by the declaring file and the item's id or
        // objectName, and, where only some of an item's texts are meant, by those texts.
        readonly property var allowlist: [
            {file: "PlexTextField.qml", name: /^(fieldLabel|helper|placeholder)$/,
                readBy: function (text) { return text.parent.input; },
                reason: "The field reads its label as its name and its unit, example and helper line as its description; on its own each would read twice."},
            {file: "AirspyControls.qml", name: /^(textLabel|gainLabel)$/, readBy: function (text) { return text.parent.input; },
                reason: "The field below reads its label as its name."},
            {file: "FrequencyField.qml", name: "frequencyUnit", readBy: function (text) { return text.parent.children[0]; },
                reason: "The field's name, \"Frequency in MHz\", reads the unit."},
            {file: "ActionMenu.qml", name: /^actionMenuDescription\d+$/, readBy: function (text) { return text.parent.children[0]; },
                reason: "The action above it reads its description in its own name."},
            {file: "HomeScreen.qml", name: "dongleStatusText", readBy: function (text) { return findNear(text, "dongleStatusPill"); },
                reason: "The receiver status pill reads the same detail as its description."},
            {file: "WizardScreen.qml", name: /^(trunkTitle|trunkRule)$/, readBy: function (text) { return findNear(text, "wizardTrunkSwitch"); },
                reason: "The trunking switch reads its title as its name and the rule under it as its description."},
            {file: "EncTag.qml", name: "tag", readBy: function (text) { return findNear(text, "otherSlotIdentity"); },
                reason: "Beside the other slot's call, whose identity line reads its tags."},
            {file: "EmergencyTag.qml", name: "tag", readBy: function (text) { return findNear(text, "otherSlotIdentity"); },
                reason: "Beside the other slot's call, whose identity line reads its tags."},
            {file: "OutlineButton.qml", name: "label", texts: ["Hold TG", "Use default", "List value", "Use the default width"],
                reason: "A short label for a button whose name spells the action out (\"Hold this talkgroup\", \"This Channel Uses the Default Squelch\")."},
            {file: "BandRail.qml", name: /^rail(Low|High)$/,
                reason: "The band's ends drawn under the coarse tuning rail, a scale; the rail reads its own frequency and steps by its actions."},
            {file: "SignalSearchScreen.qml", name: "savedMore",
                reason: "\"+N\", how many saved names the line leaves out; the saved line's name reads every one of them."},
            {file: "MonitorScreen.qml", name: /^(liveLabelProbe|rowNameProbe|rowMetaProbe)$/,
                reason: "Sizing probes, never shown (visible: false); only the every-branch pass shows them."}
        ]
        property var used: ({})

        function isSheet(item) {
            return item !== null && item.closesOnOutsideTap !== undefined && typeof item.requestDismiss === "function";
        }
        function sheetsWithin(item, out) {
            var kids = item.children || [];
            for (var i = 0; i < kids.length; ++i) {
                if (isSheet(kids[i]))
                    out.push(kids[i]);
                sheetsWithin(kids[i], out);
            }
            return out;
        }
        // The item named @p name nearest to @p item: in its parent, else its parent's parent, and so on up.
        function findNear(item, name) {
            for (var at = item.parent; at; at = at.parent) {
                var found = findChild(at, name);
                if (found !== null)
                    return found;
            }
            return null;
        }
        function matches(pattern, value) {
            return pattern instanceof RegExp ? pattern.test(value) : pattern === value;
        }
        function describe(entry) {
            return entry.file + "#" + (entry.id || entry.objectName || "?") + " " + JSON.stringify(entry.text);
        }
        // Whether the allowlist explains @p entry. An entry whose reader does not read it is explained as a problem.
        function explained(entry, problems) {
            for (var i = 0; i < allowlist.length; ++i) {
                var allowed = allowlist[i];
                if (allowed.file !== entry.file || !(matches(allowed.name, entry.id) || matches(allowed.name, entry.objectName)))
                    continue;
                if (allowed.texts !== undefined && allowed.texts.indexOf(entry.text) < 0)
                    continue;
                if (allowed.readBy !== undefined) {
                    var reader = allowed.readBy(entry.item);
                    if (reader === null || reader === undefined || !testContext.nodeCarries(reader, entry.text)) {
                        problems.push(describe(entry) + " is allowlisted as read by another item, which does not read it");
                        return true;
                    }
                }
                used[i] = true;
                return true;
            }
            return false;
        }
        // The unexplained unreached texts under @p scope, the subtrees of @p skipped left out, as failure lines.
        function unexplained(scope, skipped, where) {
            var problems = [];
            var found = testContext.unreachedTexts(scope, skipped);
            for (var i = 0; i < found.length; ++i) {
                if (!explained(found[i], problems))
                    problems.push(where + ": " + describe(found[i]));
            }
            return problems;
        }
        function build(spec) {
            var component = Qt.createComponent(uiDir + "/" + spec.file + ".qml");
            compare(component.status, Component.Ready, spec.file + ": " + component.errorString());
            var properties = Object.assign({width: root.width, height: root.height}, spec.properties || {});
            var item = component.createObject(host, properties);
            verify(item !== null, spec.file + " must build");
            return item;
        }
        function open(item, spec) {
            if (spec.open !== undefined)
                spec.open(item);
            else if (typeof item.openEditor === "function")
                item.openEditor();
            else if (typeof item.open === "function")
                item.open();
            item.visible = true;
        }
        function reveal(item) {
            var kids = item.children || [];
            for (var i = 0; i < kids.length; ++i) {
                kids[i].visible = true;
                reveal(kids[i]);
            }
        }
        function dispose(item) {
            item.visible = false;
            Ui.Navigation.modals = [];
            Ui.Navigation.layers = [];
            item.destroy();
            wait(0);
        }
        // A failure message that lists the first problems and counts the rest.
        function report(problems) {
            var shown = problems.slice(0, 25);
            if (problems.length > shown.length)
                shown.push("... and " + (problems.length - shown.length) + " more");
            return "text no screen reader reaches:\n" + shown.join("\n");
        }
        function components_data() {
            return components.map(function (spec) {
                return {tag: spec.file, spec: spec};
            });
        }

        function initTestCase() {
            verify(testContext.accessibilityActive(), "run with DSD_QML_TEST_ACCESSIBILITY=1, or there is no tree");
        }
        function cleanup() {
            Ui.Navigation.modals = [];
            Ui.Navigation.layers = [];
        }

        // The walk sees what TalkBack sees: a plain or ignored Text is unreached, a named one or a button's own label
        // is reached, and a hidden or wordless one is not asked about. A label whose words its button does not say
        // is unreached too.
        function test_1_walk_finds_plain_text() {
            controls.visible = true;
            waitForRendering(controls);
            var found = testContext.unreachedTexts(controls).map(function (entry) {
                return entry.objectName;
            }).sort();
            compare(JSON.stringify(found), JSON.stringify(["ignoredText", "mislabelled", "plainText"]));
            var named = findChild(controls, "namedText");
            verify(testContext.nodeCarries(named, "read as static text"));
            verify(!testContext.nodeCarries(named, "words nobody reads"));
            verify(!testContext.nodeCarries(findChild(controls, "plainText"), "Plain words nobody reads"));
            controls.visible = false;
        }

        // Each screen and sheet as it opens.
        function test_2_screens_as_opened_data() {
            return components_data();
        }
        function test_2_screens_as_opened(data) {
            var item = build(data.spec);
            open(item, data.spec);
            waitForRendering(host);
            var problems = unexplained(item, [], data.spec.file);
            dispose(item);
            compare(problems.length, 0, report(problems));
        }

        // Each screen and sheet with every branch shown at once; each sheet inside it on its own, as the top layer.
        function test_3_every_branch_shown_data() {
            return components_data();
        }
        function test_3_every_branch_shown(data) {
            var item = build(data.spec);
            open(item, data.spec);
            reveal(item);
            var nested = sheetsWithin(item, []);
            Ui.Navigation.layers = [];
            Ui.Navigation.modals = isSheet(item) ? [item] : [];
            waitForRendering(host);
            var problems = unexplained(item, nested, data.spec.file);
            for (var i = 0; i < nested.length; ++i) {
                Ui.Navigation.modals = [nested[i]];
                problems = problems.concat(unexplained(nested[i], sheetsWithin(nested[i], []),
                                                       data.spec.file + " > " + (nested[i].objectName || nested[i].accessibleName)));
            }
            dispose(item);
            compare(problems.length, 0, report(problems));
        }

        // The monitor in a two-slot call with its details open: the hero, the other slot and every reading group.
        function test_4_monitor_in_a_call() {
            var monitor = build({file: "MonitorScreen"});
            testContext.setHostRunning(true);
            var calls = {leadSlot: 1, twoSlotCarrier: true, slot1CallState: 2, slot2CallState: 2, slot1CallName: "Dispatch",
                slot2CallName: "Fireground", slot1TgId: 456, slot2TgId: 789, slot1TgText: "456", slot2TgText: "789",
                slot1SrcText: "2001", slot1CallEnc: true, slot1EncText: "AES-256 · key 1", slot2CallEnc: true,
                slot2CallEmergency: true, slot1Channel: "Main", radioInput: true, carrierLock: true, snrValid: true,
                snrDb: 12.5, centerFreqHz: 851012500, qualityValid: true, ccFecValid: true, ccFecOkPct: 98,
                encLockoutCount: 1, tetraVisible: true, tetraModeText: "Trunked", tetraSecurityText: "Class 1",
                tetraCodecText: "OK"};
            var restore = {};
            for (var key in calls) {
                restore[key] = metrics[key];
                testContext.setMetric(key, calls[key]);
            }
            monitor.setDetailsExpanded(true);
            waitForRendering(host);
            var problems = unexplained(monitor, [], "MonitorScreen in a call");
            monitor.setDetailsExpanded(false);
            for (var name in restore)
                testContext.setMetric(name, restore[name]);
            testContext.setHostRunning(false);
            dispose(monitor);
            compare(problems.length, 0, report(problems));
        }

        // Every allowlist entry explains a text some case met: an entry nothing needs any more must go.
        function test_9_allowlist_entries_are_all_needed() {
            var unused = [];
            for (var i = 0; i < allowlist.length; ++i) {
                if (!used[i])
                    unused.push(allowlist[i].file + " " + allowlist[i].name);
            }
            compare(unused.length, 0, "allowlist entries no case needed: " + JSON.stringify(unused));
        }
    }
}
