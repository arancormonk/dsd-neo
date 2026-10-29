// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #773 (D10): the hero and the second-slot strip word a call by what it is. A talkgroup call is unchanged; a
// private call headlines its other party (label or text, else "ID <n>", else "Private call") with a PRIVATE tag and
// the talker kept visible; a call no party named headlines its protocol ("TETRA call"), and TETRA, which normally
// names its parties, says the identity did not decode.
Item {
    id: root
    width: 420
    height: 900

    Ui.MonitorScreen {
        id: monitor
        anchors.fill: parent
    }

    TestCase {
        name: "MonitorCallIdentity"
        when: windowShown

        readonly property int kindVoice: 1
        readonly property int kindGroup: 2
        readonly property int kindPrivate: 3

        function item(name) {
            var found = findChild(monitor, name);
            verify(found !== null, name + " is missing");
            return found;
        }
        // One call on slot @p slot, with every field the hero and the strip read.
        function setCall(slot, call) {
            var p = "slot" + slot;
            testContext.setMetric(p + "CallState", 2);
            testContext.setMetric(p + "CallKind", call.kind);
            testContext.setMetric(p + "CallName", call.name || "");
            testContext.setMetric(p + "TgText", call.tg || "");
            testContext.setMetric(p + "TgId", call.id || 0);
            testContext.setMetric(p + "TgLabel", call.tgLabel || "");
            testContext.setMetric(p + "SrcText", call.src || "");
            testContext.setMetric(p + "ProtocolLabel", call.protocol || "");
            testContext.setMetric(p + "CallEnc", call.enc === true);
        }
        function cleanup() {
            for (var slot = 1; slot <= 2; ++slot) {
                var p = "slot" + slot;
                testContext.setMetric(p + "CallState", 0);
                testContext.setMetric(p + "CallKind", 0);
                for (var field of ["CallName", "TgText", "TgLabel", "SrcText", "ProtocolLabel", "Channel"])
                    testContext.setMetric(p + field, "");
                testContext.setMetric(p + "TgId", 0);
                testContext.setMetric(p + "CallEnc", false);
            }
            testContext.setMetric("leadSlot", 0);
            testContext.setHostRunning(false);
            testContext.resetCommands();
        }

        function test_hero_words_the_call_data() {
            return [
                // A talkgroup call is unchanged.
                {tag: "group", call: {kind: kindGroup, name: "Fire Dispatch", tg: "100601", id: 100601, tgLabel: "Fire Dispatch", src: "Engine 21 (100702)", protocol: "TETRA"},
                    headline: "Fire Dispatch", ids: "TG 100601 · SRC Engine 21 (100702)", privateTag: false, note: ""},
                {tag: "group-unlisted", call: {kind: kindGroup, name: "100601", tg: "100601", id: 100601, src: "100702", protocol: "TETRA"},
                    headline: "TG 100601", ids: "SRC 100702", privateTag: false, note: ""},
                // A private call: the other party, never "TG", and the talker stays visible.
                {tag: "private-labelled", call: {kind: kindPrivate, name: "Dispatcher 7", tg: "2001234", id: 2001234, tgLabel: "Dispatcher 7", src: "Engine 21 (100702)", protocol: "TETRA"},
                    headline: "Dispatcher 7", ids: "SRC Engine 21 (100702)", privateTag: true, note: ""},
                {tag: "private-numeric", call: {kind: kindPrivate, name: "2001234", tg: "2001234", id: 2001234, src: "100702", protocol: "DMR"},
                    headline: "ID 2001234", ids: "SRC 100702", privateTag: true, note: ""},
                {tag: "private-callsign", call: {kind: kindPrivate, name: "KC1ABC", tg: "KC1ABC", id: 0, src: "N0CALL", protocol: "M17"},
                    headline: "KC1ABC", ids: "SRC N0CALL", privateTag: true, note: ""},
                // A service or channel name never headlines a private call over its other party.
                {tag: "private-channel-named", call: {kind: kindPrivate, name: "Site 1", tg: "2001234", id: 2001234, src: "100702", protocol: "NXDN"},
                    headline: "ID 2001234", ids: "SRC 100702", privateTag: true, note: ""},
                {tag: "private-unknown-destination", call: {kind: kindPrivate, name: "", tg: "", id: 0, src: "4242", protocol: "NXDN"},
                    headline: "Private call", ids: "SRC 4242", privateTag: true, note: ""},
                {tag: "private-policy-target", call: {kind: kindPrivate, name: "", tg: "", id: 654321, src: "4242", protocol: "P25p1"},
                    headline: "ID 654321", ids: "SRC 4242", privateTag: true, note: ""},
                // A call no party named headlines its protocol; only TETRA, which names its parties, says so.
                {tag: "tetra-anonymous", call: {kind: kindVoice, protocol: "TETRA"},
                    headline: "TETRA call", ids: "", privateTag: false, note: "identity not decoded"},
                {tag: "tetra-anonymous-encrypted", call: {kind: kindVoice, protocol: "TETRA", enc: true},
                    headline: "TETRA call", ids: "", privateTag: false, note: "identity not decoded"},
                {tag: "provoice-anonymous", call: {kind: kindVoice, protocol: "ProVoice"},
                    headline: "ProVoice call", ids: "", privateTag: false, note: ""},
                {tag: "x2tdma-anonymous", call: {kind: kindVoice, protocol: "X2-TDMA"},
                    headline: "X2-TDMA call", ids: "", privateTag: false, note: ""},
                // A scan channel names the call's place, not its parties: still a TETRA call, heard on that channel.
                {tag: "tetra-anonymous-on-channel", call: {kind: kindVoice, name: "Site 1", protocol: "TETRA"}, channel: "Site 1",
                    headline: "TETRA call", ids: "", privateTag: false, note: "identity not decoded"},
                // Analog has no protocol label: its channel names its call.
                {tag: "analog", call: {kind: kindVoice, name: "NFM"},
                    headline: "NFM", ids: "", privateTag: false, note: ""}
            ];
        }
        function test_hero_words_the_call(data) {
            setCall(1, data.call);
            testContext.setMetric("slot1Channel", data.channel || "");
            testContext.setMetric("leadSlot", 1);
            compare(monitor.heroHeadline, data.headline);
            var ids = item("heroIds");
            compare(ids.text, data.ids);
            compare(ids.visible, data.ids.length > 0);
            var privateTag = item("heroPrivateTag");
            compare(privateTag.visible, data.privateTag);
            var note = item("heroIdentityNote");
            compare(note.visible, data.note.length > 0);
            if (data.note.length > 0)
                compare(note.text, data.note);
            compare(item("heroEncTag").visible, data.call.enc === true);
            if (data.channel) {
                var channel = item("heroChannel");
                verify(channel.visible, "the channel an unnamed call was heard on is shown");
                compare(channel.text, "· " + data.channel);
            }
            if (data.privateTag) {
                // PRIVATE first, then the talker.
                waitForRendering(monitor);
                verify(privateTag.x < ids.x, "the PRIVATE tag leads the subline");
            }
        }

        // The painted headline names the call to a screen reader.
        function test_headline_has_an_accessible_name() {
            setCall(1, {kind: kindPrivate, name: "Dispatcher 7", tg: "2001234", id: 2001234, tgLabel: "Dispatcher 7", src: "100702", protocol: "TETRA"});
            testContext.setMetric("leadSlot", 1);
            var headline = item("heroHeadlineText");
            compare(headline.Accessible.name, "Dispatcher 7");
            compare(headline.Accessible.role, Accessible.StaticText);
        }

        // A private call's target is a radio: Hold holds calls with that radio (the decoder's hold matches either party),
        // and Avoid / Lock out, which write a talkgroup row, are off. A talkgroup call keeps both.
        function test_private_call_actions() {
            testContext.setHostRunning(true);
            setCall(1, {kind: kindPrivate, tg: "2001234", id: 2001234, src: "100702", protocol: "DMR"});
            testContext.setMetric("leadSlot", 1);
            var hold = item("holdTalkgroupButton");
            var avoid = item("avoidButton");
            compare(hold.text, "Hold");
            compare(hold.accessibleName, "Hold calls with radio 2001234");
            verify(hold.enabled);
            verify(!avoid.enabled, "a private call has no talkgroup to lock out");
            compare(avoid.accessibleName, "A private call has no talkgroup to lock out");
            mouseClick(hold);
            compare(testContext.holdCalls(), 1);
            compare(testContext.lastHoldTg(), 2001234);
            mouseClick(avoid);
            compare(testContext.lockoutSlotCalls(), 0);
            setCall(1, {kind: kindGroup, name: "Dispatch", tg: "456", id: 456, src: "100702", protocol: "DMR"});
            compare(hold.text, "Hold TG");
            compare(hold.accessibleName, "Hold this talkgroup");
            verify(avoid.enabled);
        }

        // The tag follows the incumbent ENC tag's metrics: same height, so the subline keeps its reserved height.
        function test_private_tag_matches_enc_tag_metrics() {
            setCall(1, {kind: kindPrivate, tg: "2001234", id: 2001234, src: "100702", protocol: "DMR", enc: true});
            testContext.setMetric("leadSlot", 1);
            waitForRendering(monitor);
            var privateTag = item("heroPrivateTag");
            var enc = item("heroEncTag");
            verify(privateTag.visible && enc.visible);
            compare(privateTag.height, enc.height);
            compare(privateTag.radius, enc.radius);
            verify(privateTag.y === enc.y);
        }

        // A long talker alias yields room to the tags rather than pushing them off the panel.
        function test_private_subline_fits_a_narrow_screen() {
            setCall(1, {kind: kindPrivate, tg: "2001234", id: 2001234, src: "County Fire and Rescue Dispatch North Zone (1234567)", protocol: "DMR", enc: true});
            testContext.setMetric("leadSlot", 1);
            root.width = 320;
            try {
                var ids = item("heroIds");
                var enc = item("heroEncTag");
                tryVerify(function () {
                    return ids.visible && ids.truncated && enc.visible && enc.x + enc.width <= ids.parent.width + 0.5
                            && ids.x >= item("heroPrivateTag").x + item("heroPrivateTag").width + ids.parent.spacing - 0.5;
                }, 5000, "the tags do not fit beside a long alias");
            } finally {
                root.width = 420;
            }
        }

        // On a phone-width strip the call's name outranks its PRIVATE tag: the tag gives way rather than cut the name, and
        // ENC keeps its place.
        function test_strip_name_outranks_the_private_tag() {
            setCall(1, {kind: kindGroup, name: "Dispatch", tg: "456", id: 456, src: "7001", protocol: "DMR"});
            setCall(2, {kind: kindPrivate, tg: "2001234", id: 2001234, src: "7001", protocol: "DMR", enc: true});
            testContext.setMetric("leadSlot", 1);
            root.width = 411;
            try {
                var identity = item("otherSlotIdentity");
                tryVerify(function () { return !item("otherPrivateTag").visible; }, 5000, "the tag crowds the name");
                waitForRendering(monitor);
                compare(identity.text, "ID 2001234");
                verify(!identity.truncated, "the name is cut");
                verify(findChild(monitor, "otherSlotPanel") !== null);
                root.width = 420;
                testContext.setMetric("slot2CallEnc", false);
                tryVerify(function () { return item("otherPrivateTag").visible; }, 5000, "the tag does not return with room");
            } finally {
                root.width = 420;
            }
        }

        function test_strip_words_the_call_data() {
            return [
                {tag: "group", call: {kind: kindGroup, name: "Fireground", tg: "1234567", id: 1234567, src: "7001", protocol: "DMR"},
                    identity: "Fireground · TG 1234567", privateTag: false},
                {tag: "private-labelled", call: {kind: kindPrivate, name: "Dispatcher 7", tg: "2001234", id: 2001234, tgLabel: "Dispatcher 7", src: "7001", protocol: "DMR"},
                    identity: "Dispatcher 7", privateTag: true},
                {tag: "private-numeric", call: {kind: kindPrivate, name: "2001234", tg: "2001234", id: 2001234, src: "7001", protocol: "DMR"},
                    identity: "ID 2001234", privateTag: true},
                {tag: "private-unknown-destination", call: {kind: kindPrivate, src: "7001", protocol: "DMR"},
                    identity: "Private call", privateTag: true},
                {tag: "x2tdma-anonymous", call: {kind: kindVoice, protocol: "X2-TDMA"},
                    identity: "X2-TDMA call", privateTag: false}
            ];
        }
        function test_strip_words_the_call(data) {
            setCall(1, {kind: kindGroup, name: "Dispatch", tg: "456", id: 456, src: "7001", protocol: "DMR"});
            setCall(2, data.call);
            testContext.setMetric("leadSlot", 1);
            var identity = item("otherSlotIdentity");
            verify(identity.visible);
            compare(identity.text, data.identity);
            var privateTag = item("otherPrivateTag");
            compare(privateTag.visible, data.privateTag);
            if (data.privateTag) {
                waitForRendering(monitor);
                verify(identity.x + identity.width <= privateTag.x + 0.5, "the strip's identity runs under its PRIVATE tag");
                verify(privateTag.x + privateTag.width <= item("otherSlotSkip").x + 0.5, "the PRIVATE tag runs under Skip");
            }
        }
    }
}
