// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtTest

// Issue #782: positive controls for the accessibility counter. Each case shows that the harness sees the event a
// churn case asserts is absent, so a churn case cannot pass because nothing was being counted: accessibility is
// really on, a hide and a show are each counted once, a move is counted, a Controls item attaches its accessible
// object, and tickMetrics() really re-evaluates the bindings that read `metrics`.
Item {
    id: root
    width: 420
    height: 900

    Rectangle {
        id: probe
        objectName: "harnessProbe"
        width: 40
        height: 40
        Accessible.role: Accessible.Button
        Accessible.name: "Probe"
    }

    Item {
        id: reader
        // A new array on every evaluation, so the property signals once per tick even with an unchanged reading.
        property var probe: [metrics.radioInput]
    }
    SignalSpy { id: readerSpy; target: reader; signalName: "probeChanged" }

    Component {
        id: buttonComponent
        Button { text: "Created" }
    }

    TestCase {
        name: "A11yHarness"
        when: windowShown

        function owned(counts, name, type) {
            var byType = counts.owners[name];
            return byType && byType[type] ? byType[type] : 0;
        }
        function typed(counts, type) {
            return counts.types[type] ? counts.types[type] : 0;
        }

        function cleanup() {
            testContext.stopAccessibilityCount();
            probe.visible = true;
            probe.x = 0;
        }

        function test_1_accessibility_is_active() {
            verify(testContext.accessibilityActive(),
                   "DSD_QML_TEST_ACCESSIBILITY=1 must leave accessibility active, or every churn case counts nothing");
        }

        function test_2_hide_and_show_count_once_each() {
            testContext.startAccessibilityCount();
            probe.visible = false;
            var hidden = testContext.accessibilityCount();
            compare(owned(hidden, "harnessProbe", "ObjectHide"), 1, JSON.stringify(hidden.chains));
            compare(owned(hidden, "harnessProbe", "ObjectShow"), 0, JSON.stringify(hidden.chains));
            probe.visible = true;
            var shown = testContext.accessibilityCount();
            compare(owned(shown, "harnessProbe", "ObjectHide"), 1, JSON.stringify(shown.chains));
            compare(owned(shown, "harnessProbe", "ObjectShow"), 1, JSON.stringify(shown.chains));
            compare(typed(shown, "ObjectHide"), 1, JSON.stringify(shown.chains));
            compare(typed(shown, "ObjectShow"), 1, JSON.stringify(shown.chains));
            // The scoped count, which the churn cases ask of whole sheets, sees the emitter and its ancestors.
            compare(testContext.accessibilityCountWithin(probe, "ObjectHide"), 1);
            compare(testContext.accessibilityCountWithin(root, "ObjectShow"), 1);
            compare(testContext.accessibilityCountWithin(reader, "ObjectShow"), 0);
        }

        // LocationChanged is raised only while accessibility is active, so this also proves the activation took.
        function test_3_a_move_counts_location_changed() {
            testContext.startAccessibilityCount();
            probe.x = 25;
            var counts = testContext.accessibilityCount();
            verify(owned(counts, "harnessProbe", "LocationChanged") >= 1, JSON.stringify(counts.chains));
        }

        // Controls attach their accessible object, with its role, only when accessibility is on at creation. The
        // show alone does not tell (a Controls item raises it either way), the attachment and the role do.
        function test_4_controls_button_created_after_activation_shows() {
            testContext.startAccessibilityCount();
            var button = buttonComponent.createObject(root, {objectName: "harnessButton"});
            verify(button !== null);
            var counts = testContext.accessibilityCount();
            var role = button.Accessible.role;
            button.destroy();
            compare(owned(counts, "harnessButton", "ObjectShow"), 1, JSON.stringify(counts.chains));
            compare(owned(counts, "harnessButton", "ObjectCreated"), 1, JSON.stringify(counts.chains));
            compare(role, Accessible.Button);
        }

        function test_5_tick_metrics_reevaluates_metrics_bindings() {
            readerSpy.clear();
            testContext.tickMetrics(10);
            compare(readerSpy.count, 10);
        }

        // The counter must hand back the handler it replaced: counting stops when asked.
        function test_6_stop_restores_the_previous_handler() {
            testContext.startAccessibilityCount();
            testContext.stopAccessibilityCount();
            probe.visible = false;
            probe.visible = true;
            compare(testContext.accessibilityCount().total, 0);
        }
    }
}
