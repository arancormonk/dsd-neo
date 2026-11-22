// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #901 B6/B10: the second-slot strip and the hero's subline are one line
// each, of the height they had, at any width and text size. What does not fit
// is left out in a fixed order and stays in the accessible text: the strip
// keeps its identity (at least min(natural, 96) px), then EMERGENCY, the slot
// label, PRIVATE and ENC; the hero keeps its ids, then EMERGENCY, PRIVATE, ENC
// and the algorithm text (never less than its "ALG nn" part), then the channel.
Item {
    id: root

    width: 320
    height: 900

    QtObject {
        id: fakePrefs
        property bool monitorDetailsExpanded: false
        property bool compactView: false
    }

    Loader {
        id: loader

        anchors.fill: parent
        source: uiDir + "/MonitorScreen.qml"
    }

    TestCase {
        id: tc

        name: "MonitorStatusLines"
        when: windowShown

        property var monitor: null

        function item(name) {
            var found = findChild(tc.monitor, name);
            verify(found !== null, name + " is missing");
            return found;
        }
        function m(key, value) {
            testContext.setMetric(key, value);
        }
        function settle() {
            waitForRendering(tc.monitor);
            wait(20);
        }
        // The width @p text takes in @p font, unelided.
        function naturalWidth(text, font) {
            var mirror = Qt.createQmlObject("import QtQuick; Text { visible: false; textFormat: Text.PlainText }", root);
            mirror.font = font;
            mirror.text = text;
            var width = mirror.implicitWidth;
            mirror.destroy();
            return width;
        }
        function rectIn(part, container) {
            var at = part.mapToItem(container, 0, 0);
            return {name: part.objectName, x: at.x, y: at.y, w: part.width, h: part.height};
        }
        function verifyInside(r, container, what) {
            verify(r.x >= -0.5 && r.y >= -0.5 && r.x + r.w <= container.width + 0.5 && r.y + r.h <= container.height + 0.5,
                   r.name + " inside " + what + ": x=" + r.x + " w=" + r.w + " y=" + r.y + " h=" + r.h + " in " + container.width + "x" + container.height);
        }
        function verifyClear(rects) {
            for (var i = 0; i < rects.length; ++i) {
                for (var j = i + 1; j < rects.length; ++j) {
                    var a = rects[i], b = rects[j];
                    var overlapX = Math.min(a.x + a.w, b.x + b.w) - Math.max(a.x, b.x);
                    var overlapY = Math.min(a.y + a.h, b.y + b.h) - Math.max(a.y, b.y);
                    verify(overlapX <= 0.5 || overlapY <= 0.5, a.name + " overlaps " + b.name + ": " + JSON.stringify(a) + " " + JSON.stringify(b));
                }
            }
        }
        // A lower item is never drawn while a higher one is left out, and the
        // first one left out could not have fitted in the room the lead item
        // had beyond its minimum.
        function verifyPriority(order, gap, spare, what) {
            var dropped = null;
            for (var part of order) {
                if (part.visible) {
                    verify(dropped === null, part.objectName + " drawn while " + (dropped ? dropped.objectName : "") + " is not (" + what + ")");
                } else if (dropped === null) {
                    dropped = part;
                    verify(gap + part.implicitWidth > spare + 0.5, part.objectName + " was left out with room for it (" + what + ", spare " + spare + ")");
                }
            }
        }
        function setCall(slot, call) {
            var p = "slot" + slot;
            m(p + "CallKind", call.kind);
            m(p + "CallName", call.name || "");
            m(p + "TgText", call.tg || "");
            m(p + "TgId", call.id || 0);
            m(p + "TgLabel", call.tgLabel || "");
            m(p + "SrcText", call.src || "");
            m(p + "ProtocolLabel", call.protocol || "DMR");
            m(p + "CallEnc", call.enc === true);
            m(p + "CallEmergency", call.emergency === true);
            m(p + "EncText", call.encText || "");
            m(p + "Channel", call.channel || "");
            m(p + "CallSeconds", 12);
            m(p + "CallState", 2);
        }
        function clearCall(slot) {
            var p = "slot" + slot;
            m(p + "CallState", 0);
            m(p + "CallKind", 0);
            for (var field of ["CallName", "TgText", "TgLabel", "SrcText", "ProtocolLabel", "EncText", "Channel"])
                m(p + field, "");
            m(p + "TgId", 0);
            m(p + "CallEnc", false);
            m(p + "CallEmergency", false);
        }

        function initTestCase() {
            tc.monitor = loader.item;
            verify(tc.monitor !== null, "MonitorScreen.qml failed to load");
            tc.monitor.prefsStore = fakePrefs;
        }
        function init() {
            failOnWarning(/Binding loop/);
            testContext.setHostRunning(true);
        }
        function cleanup() {
            clearCall(1);
            clearCall(2);
            m("leadSlot", 0);
            m("twoSlotCarrier", false);
            Ui.Theme.resetFontScale();
            root.width = 320;
            root.height = 900;
            testContext.setHostRunning(false);
        }

        function sizes(heights) {
            var rows = [];
            for (var w of [320, 360, 411]) {
                for (var s of [1, 1.6, 2]) {
                    for (var name of ["long", "short"]) {
                        for (var h of heights)
                            rows.push({tag: w + "@" + s + "-" + name + (heights.length > 1 ? "-h" + h : ""), width: w, height: h, scale: s, name: name});
                    }
                }
            }
            return rows;
        }

        // ---- B6: the second-slot strip ----

        function test_strip_one_line_data() {
            return sizes([900]);
        }
        function test_strip_one_line(data) {
            root.width = data.width;
            root.height = data.height;
            Ui.Theme.fontScale = data.scale;
            m("twoSlotCarrier", true);
            setCall(1, {kind: 2, name: "Fire Dispatch", tg: "4001", id: 4001, src: "7001"});
            m("leadSlot", 1);
            settle();
            var host = item("otherSlotHost");
            var idleHeight = host.height;
            var label = item("otherSlotLabel");
            verify(label.visible, "the idle strip shows its label");
            compare(label.text, "SLOT 2 · idle");
            verifyInside(rectIn(label, item("otherSlotPanel")), item("otherSlotPanel"), "the idle strip");

            var other = data.name === "long"
                    ? {kind: 3, name: "Dispatcher North Zone Seven", tgLabel: "Dispatcher North Zone Seven", tg: "2001234", id: 2001234, src: "7001", enc: true, emergency: true}
                    : {kind: 3, name: "2001234", tg: "2001234", id: 2001234, src: "7001", enc: true, emergency: true};
            setCall(2, other);
            settle();
            var panel = item("otherSlotPanel");
            var identity = item("otherSlotIdentity");
            var privateTag = item("otherPrivateTag");
            var emergency = item("otherEmergencyTag");
            var enc = item("otherEncTag");
            var skip = item("otherSlotSkip");
            var more = item("otherSlotMore");
            verify(identity.visible && skip.visible && more.visible);
            // One line of the height it always had, call or no call.
            compare(host.height, 56, "the strip's height");
            compare(host.height, idleHeight, "the strip moves nothing as a call starts");
            compare(panel.height, host.height);
            compare(tc.monitor.stripHeight, host.height, "the budget's strip is the drawn one");
            compare(item("monitorLiveStrip").height, tc.monitor.stripHeight);
            compare(more.width, 48);
            compare(more.height, 48);
            compare(skip.height, 48);
            verify(skip.width >= 48);

            var rects = [];
            for (var part of [label, identity, privateTag, emergency, enc, skip, more]) {
                if (!part.visible)
                    continue;
                var r = rectIn(part, panel);
                verifyInside(r, panel, "the strip");
                rects.push(r);
            }
            verifyClear(rects);
            var natural = naturalWidth(identity.text, identity.font);
            var lead = Math.min(natural, 96);
            var drawnBelow = label.visible || privateTag.visible || emergency.visible || enc.visible;
            if (drawnBelow)
                verify(identity.width >= lead - 0.5, "identity " + identity.width + " under its minimum " + lead);
            verify(identity.width >= 0);
            verifyPriority([emergency, label, privateTag, enc], 10, identity.width - lead, data.tag);

            // Every word, drawn or not.
            var spoken = item("otherSlotNode").Accessible.name;
            for (var word of [identity.text, "PRIVATE", "EMERGENCY", "ENC"])
                verify(spoken.indexOf(word) >= 0, "'" + word + "' missing from: " + spoken);
            if (!label.visible)
                verify(spoken.indexOf("SLOT 2") >= 0, "the slot missing from: " + spoken);

            // The same strip without tags.
            setCall(2, {kind: 2, name: "Fireground", tg: "5001", id: 5001, src: "7002"});
            settle();
            compare(host.height, idleHeight, "the strip without tags");
            var plain = Math.min(naturalWidth(identity.text, identity.font), 96);
            verifyPriority([label], 10, identity.width - plain, data.tag + " without tags");
            if (label.visible)
                verifyClear([rectIn(label, panel), rectIn(identity, panel)]);
        }

        // ---- B10: the hero's subline ----

        function test_hero_subline_data() {
            return sizes([1300, 600]);
        }
        function test_hero_subline(data) {
            root.width = data.width;
            root.height = data.height;
            Ui.Theme.fontScale = data.scale;
            m("leadSlot", 0);
            settle();
            var hero = item("monitorHero");
            var compactForm = tc.monitor.heroCompact;
            var waitingHeight = hero.height;
            compare(hero.height, compactForm ? tc.monitor.heroCompactHeight : tc.monitor.heroFullHeight);

            var call = data.name === "long"
                    ? {kind: 3, name: "Dispatcher 7", tgLabel: "Dispatcher 7", tg: "2001234", id: 2001234, src: "County Fire and Rescue North (1234567)",
                       enc: true, emergency: true, encText: "ALG 84 · KID 0001", channel: "North Yard"}
                    : {kind: 3, name: "Dispatcher 7", tgLabel: "Dispatcher 7", tg: "2001234", id: 2001234, src: "7",
                       enc: true, emergency: true, encText: "ALG 84 · KID 0001", channel: "North Yard"};
            setCall(1, call);
            m("leadSlot", 1);
            settle();
            compare(tc.monitor.heroCompact, compactForm, "the tier held");
            compare(hero.height, waitingHeight, "the hero's height with every tag");

            var ids = item("heroIds");
            var privateTag = item("heroPrivateTag");
            var emergency = item("heroEmergencyTag");
            var enc = item("heroEncTag");
            var algorithm = item("heroAlgorithm");
            var channel = item("heroChannel");
            var subline = ids.parent.parent;
            verify(ids.visible);
            var rects = [];
            for (var part of [ids, privateTag, emergency, enc, algorithm, channel]) {
                if (!part.visible)
                    continue;
                verify(part.width > 0, part.objectName + " drawn with no width");
                var r = rectIn(part, subline);
                verifyInside(r, subline, "the subline");
                verifyInside(rectIn(part, hero), hero, "the hero");
                rects.push(r);
            }
            // Clear of the name line and the duration, wherever the form puts it.
            var hrects = rects.map(function (r) {
                var at = subline.mapToItem(hero, r.x, r.y);
                return {name: r.name, x: at.x, y: at.y, w: r.w, h: r.h};
            });
            hrects.push(rectIn(item("heroNameLine"), hero));
            if (!compactForm)
                hrects.push(rectIn(item("heroDuration"), hero));
            verifyClear(hrects);
            verifyClear(rects);

            var natural = naturalWidth(ids.text, ids.font);
            var lead = Math.min(natural, 96);
            if (privateTag.visible || emergency.visible || enc.visible || algorithm.visible)
                verify(ids.width >= lead - 0.5, "ids " + ids.width + " under their minimum " + lead);
            // The algorithm text is whole, or cut to no less than "ALG 84".
            var algMin = naturalWidth("ALG 84", algorithm.font);
            if (algorithm.visible) {
                verify(algorithm.width >= algMin - 0.5, "algorithm " + algorithm.width + " under " + algMin);
                // What it draws keeps "ALG 84" whole: right-eliding fits the
                // ellipsis inside the width too.
                var drawn = Qt.createQmlObject("import QtQuick; TextMetrics { elide: Text.ElideRight }", root);
                drawn.font = algorithm.font;
                drawn.text = algorithm.text;
                drawn.elideWidth = algorithm.width;
                var shownText = drawn.elidedText;
                drawn.destroy();
                verify(shownText.indexOf("ALG 84") === 0, "the algorithm draws '" + shownText + "' at " + algorithm.width + " px");
            }
            var order = [emergency, privateTag, enc, algorithm];
            var dropped = null;
            for (var p of order) {
                if (p.visible)
                    verify(dropped === null, p.objectName + " drawn while " + (dropped ? dropped.objectName : "") + " is not");
                else if (dropped === null)
                    dropped = p;
            }
            if (dropped !== null) {
                verify(!channel.visible, "the channel drawn while " + dropped.objectName + " is not");
                var spare = ids.width - lead;
                // Cut, the algorithm needs its "ALG 84" and the ellipsis.
                var algCut = Math.min(naturalWidth(algorithm.text, algorithm.font), naturalWidth("ALG 84…", algorithm.font));
                var need = 8 + (dropped === algorithm ? algCut : dropped.implicitWidth);
                verify(need > spare + 0.5, dropped.objectName + " was left out with room for it (spare " + spare + ")");
            }

            // Every status word is heard.
            tryVerify(function () {
                var spoken = hero.Accessible.name;
                for (var word of [ids.text, "PRIVATE", "EMERGENCY", "ENC", "ALG 84 · KID 0001", "North Yard"]) {
                    if (spoken.indexOf(word) < 0)
                        return false;
                }
                return true;
            }, 6000, "the hero's accessible text keeps every word: " + hero.Accessible.name);

            // The same call without tags.
            setCall(1, {kind: 2, name: "Fire Dispatch", tg: "4001", id: 4001, src: "7001"});
            settle();
            compare(hero.height, waitingHeight, "the hero's height without tags");
        }

        // A long source name and a short channel, no tags, at 320 dp: the channel is drawn after the ids' minimum
        // and the ids take what is left, rather than the ids taking every spare pixel first and leaving the channel
        // out (issue #901 G5).
        function test_hero_channel_beside_long_ids_data() {
            return [{tag: "320x1300@1", width: 320, height: 1300, scale: 1.0}, {tag: "320x600@1", width: 320, height: 600, scale: 1.0}];
        }
        function test_hero_channel_beside_long_ids(data) {
            root.width = data.width;
            root.height = data.height;
            Ui.Theme.fontScale = data.scale;
            setCall(1, {kind: 3, name: "Dispatcher 7", tgLabel: "Dispatcher 7", tg: "2001234", id: 2001234,
                        src: "County Fire and Rescue North Battalion (1234567)", channel: "Tac 3"});
            m("leadSlot", 1);
            settle();
            var ids = item("heroIds");
            var channel = item("heroChannel");
            var subline = ids.parent.parent;
            verify(ids.visible);
            var lead = Math.min(naturalWidth(ids.text, ids.font), 96);
            verify(naturalWidth(ids.text, ids.font) > subline.width, "the ids are wider than the subline");
            verify(channel.visible, "the channel is left out beside long ids");
            compare(channel.width, Math.ceil(naturalWidth(channel.text, channel.font)), "the channel whole");
            verify(ids.width >= lead - 0.5, "ids " + ids.width + " under their minimum " + lead);
            var a = rectIn(ids, subline);
            var b = rectIn(channel, subline);
            verifyInside(b, subline, "the subline");
            verifyClear([a, b]);
            // The ids take the rest of the line.
            verify(Math.abs(b.x + b.w - subline.width) < 1.5, "the line is not filled: " + (b.x + b.w) + " of " + subline.width);
            clearCall(1);
            m("leadSlot", 0);
        }

        // Issue #901 r2 B1: the hero's subline, the strip's identity and the disclosure's answer each draw one line in
        // their fixed box. An imported channel name with line separators kept by dsd_channel_label_sanitize() drew
        // three lines (84 px) in a 29 px subline and painted into the strip. A break (U+2028, U+2029, a newline) is
        // drawn and measured as a space, and the words are all still said.
        function test_lines_draw_one_line_data() {
            var rows = [];
            for (var w of [320, 411]) {
                for (var sc of [1, 1.6]) {
                    rows.push({tag: w + "@" + sc + " U+2028", width: w, scale: sc, sep: "\u2028"});
                    rows.push({tag: w + "@" + sc + " U+2029", width: w, scale: sc, sep: "\u2029"});
                    rows.push({tag: w + "@" + sc + " newline", width: w, scale: sc, sep: "\n"});
                }
            }
            return rows;
        }
        function test_lines_draw_one_line(data) {
            var sep = data.sep;
            var breaks = /[\r\n\v\f\u0085\u2028\u2029]/;
            root.width = data.width;
            root.height = 1300;
            Ui.Theme.fontScale = data.scale;
            m("twoSlotCarrier", true);
            setCall(1, {kind: 2, name: "Fire" + sep + "Dispatch", tg: "4001", id: 4001, src: "E" + sep + "W",
                        channel: "E" + sep + "W" + sep + "S", enc: true, encText: "ALG 84" + sep + "KID 0001"});
            setCall(2, {kind: 2, name: "North" + sep + "Zone", tg: "4002", id: 4002, src: "7002"});
            m("leadSlot", 1);
            testContext.setMetric("uiMessage", "Output:" + sep + "Muted");
            settle();
            var subline = item("heroIds").parent;
            var lines = [item("heroIds"), item("heroChannel"), item("heroAlgorithm"), item("otherSlotIdentity")];
            var drawn = 0;
            for (var line of lines) {
                verify(!breaks.test(line.text), line.objectName + " draws a line break: " + JSON.stringify(line.text));
                if (!line.visible)
                    continue;
                ++drawn;
                compare(line.lineCount, 1, line.objectName + " draws one line");
            }
            verify(item("heroIds").visible && item("otherSlotIdentity").visible, "the ids and the strip's identity are drawn");
            // The algorithm can be left out beside ENC at 320 dp and large text (a decided limit); a clear call has
            // room for the channel at every size here, so each of the four is drawn in one of the two calls.
            var algorithmDrawn = item("heroAlgorithm").visible;
            m("slot1CallEnc", false);
            settle();
            verify(item("heroChannel").visible, "a clear call draws its channel");
            compare(item("heroChannel").lineCount, 1, "the channel draws one line");
            compare(item("heroChannel").text, "· E W S");
            verify(subline.height <= subline.parent.height + 0.5, "clear, the subline " + subline.height + " in its box " + subline.parent.height);
            m("slot1CallEnc", true);
            settle();
            if (data.width === 411)
                verify(algorithmDrawn, "at 411 dp the algorithm is drawn beside ENC");
            // The subline's row stays in its box, so nothing paints into the strip below.
            verify(subline.height <= subline.parent.height + 0.5, "the subline " + subline.height + " in its box " + subline.parent.height);
            var identity = item("otherSlotIdentity");
            verify(identity.contentHeight <= identity.height + 1, "the identity " + identity.contentHeight + " in its box " + identity.height);
            // The disclosure's answer in place of its summary: one line, said whole.
            var details = item("monitorDetailsRow");
            var subtitle = null;
            for (var t of [details].concat(details.children)) {
                (function walk(n) {
                    if (n.text !== undefined && n.text === "Output: Muted")
                        subtitle = n;
                    for (var c of n.children || [])
                        walk(c);
                })(t);
            }
            verify(subtitle !== null, "the answer is drawn on one line as 'Output: Muted'");
            compare(subtitle.lineCount, 1);
            // Every word is said: the hero's subline, the identity's name.
            var speech = tc.monitor.heroSublineSpeech();
            verify(speech.indexOf("SRC E W") >= 0, speech);
            verify(speech.indexOf("ALG 84 KID 0001") >= 0, speech);
            verify(speech.indexOf("· E W S") >= 0, speech);
            verify(item("otherSlotNode").Accessible.name.indexOf("North Zone") >= 0, item("otherSlotNode").Accessible.name);
            testContext.setMetric("uiMessage", "");
        }
    }
}
