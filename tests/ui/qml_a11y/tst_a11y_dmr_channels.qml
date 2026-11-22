// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Discussion #900 F3, issue #782: the DMR Tier III Channels sheet open with a screen reader on, on the real
// DmrChannelsModel (testContext.setDmrChannelsView() publishes a new record body each call, as a snapshot copied anew
// is). Its lists keep a fixed reading order, each row is read as what it shows, and a record that hands the same
// channels again, or the same channels heard again in another order, raises nothing: no row shows, hides, moves or is
// renamed.
Item {
    id: root
    width: 412
    height: 915
    Loader {
        id: sheetLoader
        anchors.fill: parent
        source: uiDir + "/ChannelsSheet.qml"
    }
    Ui.MonitorScreen {
        id: monitor
        anchors.fill: parent
        detailsExpanded: true
        visible: false
    }
    TestCase {
        name: "A11yDmrChannels"
        when: windowShown
        readonly property var sheet: sheetLoader.item

        // Channels 7, 9, 12 and 30 granted with no frequency, every one heard within the last minute, newest first in
        // @p order; 40 past the ledger's details; and channels 18 and 20 with a frequency.
        function view(order, agos) {
            var details = {
                7: {lpcn: 7, slot: 1, group: false, caller: 5002, recipient: 5003},
                9: {lpcn: 9, slot: 0, group: true, data: true, caller: 5004, recipient: 77},
                12: {lpcn: 12, slot: 0, group: true, caller: 5001, recipient: 1234},
                30: {lpcn: 30, slot: 1, group: true, caller: 5005, recipient: 4321}
            };
            return {
                generation: 2,
                revision: 4,
                missing: order.map(function (lpcn, i) {
                    return Object.assign({agoMs: agos ? agos[i] : 1000 + i * 1000}, details[lpcn]);
                }),
                extra: [40],
                channels: [
                    {lpcn: 18, origin: 1, hz: 451012500},
                    {lpcn: 20, origin: 2, hz: 451137500, baseHz: 451100000}
                ]
            };
        }
        function rows(name) {
            var out = [];
            (function collect(item) {
                for (var k = 0; k < item.children.length; ++k) {
                    var child = item.children[k];
                    if (child.objectName === name && child.live)
                        out.push(child);
                    collect(child);
                }
            })(sheet);
            return out;
        }
        function lpcns(list) {
            return list.map(function (row) {
                return row.channel;
            });
        }
        function churn(scope) {
            var counts = testContext.accessibilityCount();
            var raised = {};
            for (var type of ["ObjectShow", "ObjectHide", "NameChanged", "LocationChanged"])
                raised[type] = testContext.accessibilityCountWithin(scope, type);
            raised.chains = counts.chains;
            return raised;
        }
        function openSheet(spec) {
            testContext.setDmrChannelsView(spec);
            sheet.open();
            Ui.Navigation.modals = [sheet];
            testContext.setDmrChannelsView(spec);
            waitForRendering(sheet);
            wait(20);
        }
        function cleanup() {
            testContext.stopAccessibilityCount();
            monitor.visible = false;
            testContext.setMetric("noFreqCount", 0);
            sheet.visible = false;
            Ui.Navigation.modals = [];
            testContext.clearDmrChannels();
        }

        function test_reading_order_is_fixed_and_rows_read_as_shown() {
            verify(screenReader.active, "this run has a screen reader on");
            openSheet(view([30, 12, 9, 7]));
            var missing = rows("channelsMissingRow");
            compare(lpcns(missing), [7, 9, 12, 30, 40], "by channel, not most recent first");
            compare(lpcns(rows("channelsChannelRow")), [18, 20]);
            for (var r of missing.concat(rows("channelsChannelRow"))) {
                compare(testContext.accessibleNode(r).name, r.words, "each row is read as itself");
                verify(r.words.indexOf("Channel " + r.channel) === 0, r.words);
            }
            compare(testContext.accessibleNode(rows("channelsMissingRow")[0]).name,
                    "Channel 7 · slot 2 · 5002 to 5003 · last heard just now");
        }

        // A record published again with nothing changed, and the same channels heard again in another order (the
        // model's newest first moves with every grant): no row of either list is touched.
        function test_repeated_grants_raise_nothing() {
            openSheet(view([30, 12, 9, 7]));
            var missing = rows("channelsMissingRow");
            var channels = rows("channelsChannelRow");
            var names = missing.concat(channels).map(function (r) {
                return r.words;
            });
            testContext.startAccessibilityCount();
            for (var i = 0; i < 10; ++i) {
                testContext.setDmrChannelsView(view([30, 12, 9, 7]));
                wait(10);
            }
            var orders = [[7, 30, 12, 9], [9, 7, 30, 12], [12, 9, 7, 30], [30, 12, 9, 7]];
            for (var order of orders) {
                testContext.setDmrChannelsView(view(order, [500, 1500, 2500, 3500]));
                wait(10);
            }
            var raised = churn(sheet);
            testContext.stopAccessibilityCount();
            for (var type of ["ObjectShow", "ObjectHide", "NameChanged", "LocationChanged"])
                compare(raised[type], 0, type + ": " + JSON.stringify(raised.chains));
            compare(rows("channelsMissingRow"), missing, "each row stays with its channel");
            compare(rows("channelsChannelRow"), channels);
            compare(missing.concat(channels).map(function (r) {
                return r.words;
            }), names);
        }

        // A channel given a frequency leaves "No frequency" and joins "Channels"; a hop to a target with none holds
        // the rows rather than hiding them, and the held rows read as nothing. (The lines around the lists show and
        // hide with them: "No unresolved grants recorded." showing is what has Android read the sheet again.)
        function test_resolved_and_emptied_lists_hide_nothing() {
            openSheet(view([30, 12, 9, 7]));
            var resolved = view([30, 9, 7]);
            resolved.revision = 5;
            resolved.channels = [{lpcn: 12, origin: 2, hz: 451500000}].concat(resolved.channels);
            testContext.startAccessibilityCount();
            testContext.setDmrChannelsView(resolved);
            wait(20);
            var lists = [findChild(sheet, "channelsMissingList"), findChild(sheet, "channelsChannelList")];
            var raised = churn(lists[0]);
            raised.ObjectHide += testContext.accessibilityCountWithin(lists[1], "ObjectHide");
            testContext.stopAccessibilityCount();
            compare(raised.ObjectHide, 0, JSON.stringify(raised.chains));
            compare(lpcns(rows("channelsMissingRow")), [7, 9, 30, 40]);
            compare(lpcns(rows("channelsChannelRow")), [12, 18, 20]);
            var held = rows("channelsChannelRow");
            testContext.startAccessibilityCount();
            testContext.setDmrChannelsView({missing: [], extra: [], channels: []});
            wait(20);
            raised = churn(lists[0]);
            raised.ObjectHide += testContext.accessibilityCountWithin(lists[1], "ObjectHide");
            testContext.stopAccessibilityCount();
            compare(raised.ObjectHide, 0, JSON.stringify(raised.chains));
            compare(rows("channelsMissingRow").length, 0);
            for (var r of held) {
                compare(r.live, false);
                compare(testContext.accessibleNode(r).name, "", "a held row reads nothing");
            }
            verify(findChild(sheet, "channelsMissingEmpty").visible);
        }

        // The Monitor's NO FREQ reading moves with every grant on a channel with no frequency: it is renamed at the
        // readings' pace, never once per grant, and catches up with the count on screen.
        function test_no_freq_row_is_paced() {
            monitor.visible = true;
            testContext.setMetric("noFreqCount", 1);
            var row = findChild(monitor, "noFreqRow");
            tryCompare(row, "visible", true);
            tryVerify(function () { return testContext.accessibleNode(row).name === "1 channel without a frequency"; }, 7000);
            testContext.startAccessibilityCount();
            for (var n = 2; n <= 21; ++n) {
                testContext.setMetric("noFreqCount", n);
                compare(findChild(monitor, "noFreqValue").text, String(n), "the text on screen follows every grant");
                wait(10);
            }
            var renames = testContext.accessibilityCountWithin(row, "NameChanged");
            var shows = testContext.accessibilityCountWithin(row, "ObjectShow") + testContext.accessibilityCountWithin(row, "ObjectHide");
            testContext.stopAccessibilityCount();
            verify(renames <= 1, "renamed " + renames + " times for 20 grants");
            compare(shows, 0);
            tryVerify(function () { return testContext.accessibleNode(row).name === "21 channels without a frequency"; }, 7000,
                      testContext.accessibleNode(row).name);
        }
    }
}
