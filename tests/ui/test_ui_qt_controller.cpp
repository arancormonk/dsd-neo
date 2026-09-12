// SPDX-License-Identifier: GPL-3.0-or-later
#include <QAbstractListModel>
#include <QChar>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <Qt>
#include <cstdio>
#include <cstdlib>
#include <dsd-neo/app_control/call_view.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <functional>
#include <initializer_list>
#include <memory>
#include <stdint.h>
#include <utility>
#include "../../android/local_device_state.h"
#include "../../src/app_control/commands_internal.h"
#include "../../src/app_control/snapshot_internal.h"
#include "../test_support/qt_test_paths.h"
#include "call_history_model.h"
#include "command_bridge.h"
#include "decoder_host.h"
#include "diagnostics_log.h"
#include "metrics_model.h"
#include "p25_network_model.h"
#include "session_args.h"
#include "talkgroup_list_model.h"
#include "ui_controller.h"

namespace {

void
expect(bool condition, int line) {
    if (!condition) {
        std::fprintf(stderr, "controller assertion failed at line %d\n", line);
        std::abort();
    }
}

#define check(condition) expect(!!(condition), __LINE__)

class Host : public dsd_qt::DecoderHost {
  public:
    SessionState phase = Running;
    std::function<void()> onRefresh;

    void
    refresh() override {
        if (onRefresh) {
            onRefresh();
        }
    }

    bool
    isRunning() const override {
        return phase == Running;
    }

    QString
    statusText() const override {
        return {};
    }

    SessionState
    sessionState() const override {
        return phase;
    }

    bool
    start(const QStringList&) override {
        return true;
    }

    void
    stop() override {}

    void
    setPhase(SessionState value) {
        phase = value;
        Q_EMIT sessionStateChanged();
    }
};

static void
test_initial_usb_record() {
    for (const bool ready : {true, false}) {
        Host host;
        host.phase = Host::Idle;
        dsd_android::LocalDeviceState usb;
        int properties = 0;
        int attachments = 0;
        QObject::connect(&host, &dsd_qt::DecoderHost::localDeviceChanged, [&]() {
            ++properties;
            check(usb.ready == ready);
            check(usb.text == (ready ? "Ready: RTL-SDR" : "Permission denied"));
        });
        QObject::connect(&host, &dsd_qt::DecoderHost::localDeviceAttached, [&]() { ++attachments; });
        const QJsonObject record{{"ready", ready},
                                 {"text", ready ? "Ready: RTL-SDR" : "Permission denied"},
                                 {"kind", ready ? "" : "permission"},
                                 {"name", "RTL-SDR"}};
        usb.apply(host, record);
        check(properties == 1 && attachments == 0);
        usb.apply(host, record);
        check(properties == 1 && attachments == 0);
        check(usb.failureKind == (ready ? Host::NoDeviceFailure : Host::DevicePermission));
    }
}

static void
test_history_receives_effective_scan_options() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::CallHistoryModel history;
    history.setSessionLabel("Saved source");
    history.setSessionUid("saved-source");
    dsd_qt::UiController controller(nullptr, nullptr, &history, nullptr);
    controller.setPollIntervalMs(50);
    for (int mode = 0; mode < 3; ++mode) {
        opts.scanner_mode = mode == 0;
        opts.trunk_scan_enabled = mode == 1;
        for (bool flush : {false, true}) {
            auto& ring = state.event_history_s[0];
            auto& item = ring.Event_History_Items[1];
            item.category = DSD_EVENT_CATEGORY_VOICE;
            item.target_id = 9001 + mode * 2 + flush;
            item.source_id = 8001;
            item.event_start_time = 1754500000 + mode * 20 + (flush ? 10 : 0);
            item.event_time = item.event_start_time + 4;
            ++ring.push_seq;
            ++ring.commit_rev;
            ++ring.revision;
            dsd_app_telemetry_publish_opts_snapshot(&opts);
            dsd_app_telemetry_publish_snapshot(&state);
            if (flush) {
                controller.flushHistory();
            } else {
                dsd_app_request_redraw();
                QEventLoop loop;
                QTimer::singleShot(65, &loop, &QEventLoop::quit);
                controller.start();
                loop.exec();
                controller.stop();
            }
            check(history.count() == mode * 2 + (flush ? 2 : 1));
            check(history.data(history.index(0), dsd_qt::CallHistoryModel::SystemUidRole).toString()
                  == (mode == 2 ? "saved-source" : ""));
        }
    }
    freeState(&state);
}

// Reproduce the options-publication/redraw gap during Android input startup.
static void
test_startup_options_wait_for_redraw() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    Host host;
    dsd_qt::MetricsModel metrics;
    dsd_qt::UiController controller(&host, &metrics, nullptr, nullptr);
    controller.setPollIntervalMs(50);
    const auto poll = [&]() {
        QEventLoop loop;
        QTimer::singleShot(65, &loop, &QEventLoop::quit);
        controller.start();
        loop.exec();
        controller.stop();
    };
    for (int mode = 0; mode < 3; ++mode) {
        // A completed single-system session left a valid, non-scanning view.
        opts.scanner_mode = 0;
        opts.trunk_scan_enabled = 0;
        metrics.refresh(&opts, &state);
        check(metrics.optionsKnown() && !metrics.scanRotationActive());
        host.setPhase(Host::Starting);
        check(!metrics.optionsKnown());
        (void)dsd_app_frontend_redraw_consume();
        opts.scanner_mode = mode == 0;
        opts.trunk_scan_enabled = mode == 1;
        // Runtime admission and snapshots precede the slow input open. Android
        // already reports Running, but no decoder redraw has arrived yet.
        dsd_app_frontend_runtime_start(&opts, &state);
        host.setPhase(Host::Running);
        poll();
        check(!metrics.optionsKnown() && !metrics.scanRotationActive());
        dsd_app_request_redraw();
        poll();
        check(metrics.optionsKnown() && metrics.scanRotationActive() == (mode != 2));
        host.setPhase(Host::Idle);
        check(!metrics.optionsKnown());
        dsd_app_frontend_runtime_stop();
    }
    freeState(&state);
}

// A held snapshot still has deadlines to age when input stops producing redraws.
static void
test_metrics_age_without_decoder_redraw() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    opts.scanner_mode = 1;
    Host host;
    dsd_qt::MetricsModel metrics;
    dsd_qt::UiController controller(&host, &metrics, nullptr, nullptr);
    controller.setPollIntervalMs(50);
    const auto poll = [&](int duration_ms) {
        QEventLoop loop;
        QTimer::singleShot(duration_ms, &loop, &QEventLoop::quit);
        controller.start();
        loop.exec();
        controller.stop();
    };

    // Seed the existing short sync hold, then publish loss and a scan deadline.
    state.synctype = DSD_SYNC_P25P1_POS;
    metrics.refresh(&opts, &state);
    state.synctype = DSD_SYNC_NONE;
    state.scan_timing.reason = DSD_SCAN_STAY_HANGTIME;
    state.scan_timing.conventional = 1U;
    state.scan_timing.started_m = dsd_time_now_monotonic_s();
    state.scan_timing.deadline_m = state.scan_timing.started_m + 1.0;
    state.scan_timing.span_ms = 1000U;
    dsd_app_telemetry_publish_opts_snapshot(&opts);
    dsd_app_telemetry_publish_snapshot(&state);
    dsd_app_request_redraw();
    poll(65);
    check(metrics.scanTimerLive() && metrics.scanTimerRemainingDs() > 0);
    check(metrics.syncedHere());
    const int remaining = metrics.scanTimerRemainingDs();
    check(dsd_app_frontend_redraw_consume() == 0);
    poll(250);
    check(metrics.scanTimerRemainingDs() < remaining);
    poll(static_cast<int>(DSD_APP_SYNC_HOLD_S * 1000.0) + 100);
    check(metrics.scanTimerRemainingDs() == 0);
    check(!metrics.syncedHere());

    // Lifecycle still gates stale snapshots after the engine stops.
    host.setPhase(Host::Idle);
    poll(65);
    check(!metrics.scanTimingVisible() && !metrics.optionsKnown());
    freeState(&state);
}

// Run the actual sheet against the real bridge and decoder queue, including CSV media overrides.
static void
test_sheet_policy_edits() {
    dsd_qt::CommandBridge bridge;
    QQmlEngine engine;
    engine.rootContext()->setContextProperty("commands", &bridge);
    engine.rootContext()->setContextProperty("metrics", QVariantMap{{"uiMessage", QString()}});
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(DSD_QML_UI_DIR "/TalkgroupEditSheet.qml")));
    std::unique_ptr<QObject> sheet(component.create());
    check(sheet);
    QObject* name = sheet->findChild<QObject*>("talkgroupName");
    QObject* listen = sheet->findChild<QObject*>("listeningToggle");
    check(name && listen);
    static dsd_opts opts;
    static dsd_state state;

    struct MediaCase {
        const char* mode;
        uint8_t audio;
        uint8_t record;
        uint8_t stream;
    };

    for (const auto& media :
         {MediaCase{"A", 0, 0, 0}, MediaCase{"A", 1, 0, 1}, MediaCase{"A", 1, 1, 0}, MediaCase{"DE", 0, 0, 0}}) {
        dsd_app_frontend_runtime_start(nullptr, nullptr);
        const char* mode = media.mode;
        dsd_tg_policy_entry entry = {};
        check(dsd_tg_policy_make_exact_entry(42, mode, "Dispatch", DSD_TG_POLICY_SOURCE_IMPORTED, &entry) == 0);
        entry.priority = 25;
        entry.audio = media.audio;
        entry.record = media.record;
        entry.stream = media.stream;
        check(dsd_tg_policy_append_exact(&state, &entry) == 0);
        check(dsd_tg_policy_entry_at(&state, 0, &entry));
        check(entry.audio == media.audio && entry.record == media.record && entry.stream == media.stream);
        const auto open = [&]() {
            uint64_t context = 0;
            unsigned int generation = 0;
            dsd_tg_policy_table_version(&state, &context, &generation);
            QQmlExpression expression(engine.rootContext(), sheet.get(),
                                      QStringLiteral("openRow(42, 42, '%1', %2, true, %3, false, '%4', %5)")
                                          .arg(QString::fromUtf8(entry.name))
                                          .arg(QString::fromUtf8(mode) == "A" ? "true" : "false")
                                          .arg(entry.priority)
                                          .arg(context)
                                          .arg(generation));
            expression.evaluate();
            check(!expression.hasError());
        };
        open();
        name->setProperty("text", "Renamed");
        check(QMetaObject::invokeMethod(sheet.get(), "saveRow"));
        check(dsd_app_drain_cmds(&opts, &state) == 1);
        check(dsd_tg_policy_entry_at(&state, 0, &entry));
        check(QString::fromUtf8(entry.name) == "Renamed");
        check(QString::fromUtf8(entry.mode) == mode && entry.audio == media.audio && entry.record == media.record
              && entry.stream == media.stream);
        check(sheet->property("submitted").toBool());
        // Consecutive operations on the submitted sheet must not queue a stale edit.
        check(QMetaObject::invokeMethod(sheet.get(), "saveRow"));
        check(dsd_app_drain_cmds(&opts, &state) == 0);
        open();
        check(!sheet->property("submitted").toBool());
        sheet->setProperty("priorityValue", 50);
        check(QMetaObject::invokeMethod(sheet.get(), "saveRow"));
        check(dsd_app_drain_cmds(&opts, &state) == 1);
        check(dsd_tg_policy_entry_at(&state, 0, &entry));
        check(entry.priority == 50 && QString::fromUtf8(entry.name) == "Renamed");
        check(QString::fromUtf8(entry.mode) == mode && entry.audio == media.audio && entry.record == media.record
              && entry.stream == media.stream);
        open();
        name->setProperty("text", "Combined");
        sheet->setProperty("priorityValue", 100);
        check(QMetaObject::invokeMethod(sheet.get(), "saveRow"));
        check(dsd_app_drain_cmds(&opts, &state) == 1); // Name + priority must be one atomic edit.
        check(dsd_tg_policy_entry_at(&state, 0, &entry));
        check(entry.priority == 100 && QString::fromUtf8(entry.name) == "Combined");
        check(QString::fromUtf8(entry.mode) == mode && entry.audio == media.audio && entry.record == media.record
              && entry.stream == media.stream);
        open();
        check(QMetaObject::invokeMethod(sheet.get(), "saveRow"));
        check(dsd_app_drain_cmds(&opts, &state) == 0); // Unchanged form submits nothing.
        listen->setProperty("checked", QString::fromUtf8(mode) != "A");
        check(QMetaObject::invokeMethod(sheet.get(), "saveRow"));
        check(dsd_app_drain_cmds(&opts, &state) == 1);
        check(dsd_tg_policy_entry_at(&state, 0, &entry));
        check(QString::fromUtf8(entry.mode) == (QString::fromUtf8(mode) == "A" ? "B" : "A"));
        dsd_app_frontend_runtime_stop();
        dsd_state_ext_free_all(&state);
    }
}

// WP-S2: production controller emits only a usable stable target while idle.
static void
test_auto_start_requests() {
    Host host;
    host.phase = Host::Idle;
    dsd_qt::UiController controller(&host, nullptr, nullptr, nullptr);
    int requests = 0;
    QString requestedKind;
    QObject::connect(&controller, &dsd_qt::UiController::autoStartRequested,
                     [&](const QString& kind, const QString& uid) {
                         ++requests;
                         requestedKind = kind;
                         check(uid == "target");
                     });
    const QVariantMap usb{{"uid", "target"}, {"sourceType", "usb"}};
    controller.requestAutoStart(true, true, "saved", "target", usb);
    check(requests == 0); // No QML binding installed yet.
    controller.setProperty("autoStartBlocked", false);
    controller.requestAutoStart(true, true, "saved", "target", usb);
    check(requests == 1 && requestedKind == "saved");
    controller.requestAutoStart(true, true, "scan", "target", usb);
    check(requests == 2 && requestedKind == "scan");
    controller.requestAutoStart(true, true, "explore", "target", usb);
    controller.requestAutoStart(true, true, "saved", "deleted", usb);
    controller.requestAutoStart(true, true, "saved", "target", {});
    controller.requestAutoStart(true, true, "saved", "", {});
    for (const char* source : {"rtltcp", "udp", "tcp", "file", ""}) {
        controller.requestAutoStart(true, true, "saved", "target", {{"uid", "target"}, {"sourceType", source}});
    }
    controller.requestAutoStart(false, true, "saved", "target", usb);
    controller.requestAutoStart(true, false, "saved", "target", usb);
    for (auto phase : {Host::Starting, Host::Running, Host::Stopping, Host::Failed}) {
        host.phase = phase;
        controller.requestAutoStart(true, true, "saved", "target", usb);
    }
    host.phase = Host::Idle;
    controller.setProperty("autoStartBlocked", true);
    controller.requestAutoStart(true, true, "saved", "target", usb);
    controller.setProperty("autoStartBlocked", false);
    check(requests == 2); // Suppressed requests do not retry when the gate opens.
}

static void
test_zero_bounds() {
    dsd_qt::CommandBridge bridge;
    static dsd_opts opts;
    static dsd_state state;
    for (unsigned int end : {0U, 999U}) {
        dsd_app_frontend_runtime_start(nullptr, nullptr);
        dsd_tg_policy_entry entry = {};
        check(dsd_tg_policy_make_exact_entry(0, "A", "Zero", DSD_TG_POLICY_SOURCE_IMPORTED, &entry) == 0);
        entry.id_end = end;
        entry.is_range = end != 0;
        check((end ? dsd_tg_policy_add_range_entry(&state, &entry) : dsd_tg_policy_append_exact(&state, &entry)) == 0);
        uint64_t context = 0;
        unsigned int generation = 0;
        dsd_tg_policy_table_version(&state, &context, &generation);
        check(bridge.renameTalkgroup(0, end, QString::number(context), generation, "Edited zero"));
        check(dsd_app_drain_cmds(&opts, &state) == 1);
        check(dsd_tg_policy_entry_at(&state, 0, &entry));
        check(entry.id_start == 0 && entry.id_end == end && QString::fromUtf8(entry.name) == "Edited zero");
        dsd_tg_policy_table_version(&state, &context, &generation);
        check(bridge.setTalkgroupPolicy(0, end, QString::number(context), generation, {{"priority", 50}}));
        check(dsd_app_drain_cmds(&opts, &state) == 1);
        check(dsd_tg_policy_entry_at(&state, 0, &entry));
        check(entry.id_start == 0 && entry.id_end == end && entry.priority == 50);
        dsd_tg_policy_table_version(&state, &context, &generation);
        check(bridge.removeTalkgroup(0, end, QString::number(context), generation));
        check(dsd_app_drain_cmds(&opts, &state) == 1);
        check(dsd_tg_policy_entry_count(&state) == 0);
        dsd_app_frontend_runtime_stop();
        dsd_state_ext_free_all(&state);
    }
}
} // namespace

int
main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName("dsd-neo-test");
    QCoreApplication::setApplicationName("dsd-neo-controller");
    dsd_test_qt_isolate_paths();
    test_history_receives_effective_scan_options();
    test_startup_options_wait_for_redraw();
    test_metrics_age_without_decoder_redraw();
    test_sheet_policy_edits();
    test_zero_bounds();
    test_auto_start_requests();
    test_initial_usb_record();
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    Host host;
    dsd_qt::MetricsModel metrics;
    dsd_qt::TalkgroupListModel talkgroups(nullptr);
    dsd_qt::UiController controller(&host, &metrics, nullptr, &talkgroups);
    int resets = 0;
    dsd_qt::P25NetworkModel network;
    network.setActive(true);
    controller.setP25Network(&network);
    controller.setPollIntervalMs(50);
    QTemporaryDir diagnosticsDir;
    dsd_qt::DiagnosticsLog diagnostics(diagnosticsDir.path());
    dsd_qt::DiagnosticsLogModel diagnosticsModel(&diagnostics);
    controller.setDiagnosticsLog(&diagnosticsModel);
    auto tick = [&]() {
        dsd_app_telemetry_publish_opts_snapshot(&opts);
        dsd_app_telemetry_publish_snapshot(&state);
        dsd_app_request_redraw();
        QEventLoop loop;
        QTimer::singleShot(65, &loop, &QEventLoop::quit);
        controller.start();
        loop.exec();
        controller.stop();
    };
    dsd_tg_policy_entry entry;
    check(dsd_tg_policy_make_exact_entry(1001, "A", "Dispatch", DSD_TG_POLICY_SOURCE_IMPORTED, &entry) == 0);
    check(dsd_tg_policy_append_exact(&state, &entry) == 0);
    state.synctype = DSD_SYNC_P25P1_POS;
    state.p25_aff_rid[0] = 123;
    state.trunk_scan_active_ordinal = 1;
    tick();
    check(!metrics.syncLabel().isEmpty());
    check(talkgroups.count() == 1);
    QObject::connect(&talkgroups, &QAbstractItemModel::modelReset, [&]() { ++resets; });
    check(network.radios().size() == 1);
    network.setActive(false); // Clearing must also reach closed sheets.
    state.synctype = DSD_SYNC_NONE;
    state.lastsynctype = DSD_SYNC_NONE;
    state.trunk_scan_active_ordinal = 2;
    tick();
    check(resets == 0); // A scan hop must preserve the list view.
    check(talkgroups.count() == 1);
    check(metrics.syncLabel().isEmpty()); // Old target's sync hold must not survive.
    check(network.radios().isEmpty());
    network.setActive(true);
    diagnostics.submit("host", "info", "before next session");
    for (auto phase : {Host::Starting, Host::Idle, Host::Failed}) {
        host.setPhase(Host::Running);
        state.synctype = DSD_SYNC_P25P1_POS;
        state.p2_cc = 0x293;
        state.p25_p1_fec_ok = 3;
        state.p25_p1_fec_err = 1;
        tick();
        check(metrics.p25NacValid() && !metrics.siteLine().isEmpty());
        check(metrics.qualityValid() && metrics.ccFecOkPct() == 75.0);
        check(!metrics.syncLabel().isEmpty());
        check(network.radios().size() == 1);
        host.setPhase(phase);
        check(network.radios().isEmpty());
        tick(); // The last engine snapshot is still published after stop.
        check(metrics.syncLabel().isEmpty());
        check(metrics.siteLine().isEmpty() && !metrics.p25NacValid() && !metrics.siteConfirmed());
        check(!metrics.qualityValid() && !metrics.ccFecValid());
        check(diagnosticsModel.allText().contains("before next session"));
        check(diagnosticsModel.allText().count("--- session starting ---") == 1);
        check(network.radios().isEmpty());
        host.setPhase(phase); // Repeated state notification is not another boundary.
        check(diagnosticsModel.allText().count("--- session starting ---") == 1);
    }
    // WP-F5: an idle decoder need not request redraw for process logs to refresh.
    (void)dsd_app_frontend_redraw_consume();
    diagnostics.submit("host", "info", "idle diagnostic");
    QEventLoop idleLoop;
    QTimer::singleShot(65, &idleLoop, &QEventLoop::quit);
    controller.start();
    idleLoop.exec();
    controller.stop();
    check(diagnosticsModel.rowCount() == 3);
    host.setPhase(Host::Starting);
    diagnosticsModel.refresh();
    check(diagnosticsModel.allText().count("--- session starting ---") == 2);
    check(diagnosticsModel.allText().contains("idle diagnostic"));
    // WP-D1: real bridge -> queue -> policy, then retained completion with no redraw.
    dsd_qt::CommandBridge bridge;
    check(!bridge.addTalkgroup(77, 77, QStringLiteral("0"), 0, "While idle", true, 0, false));
    check(dsd_app_drain_cmds(&opts, &state) == 0);
    dsd_app_frontend_runtime_start(nullptr, nullptr);
    // A session without a policy table reports version 0/0; adding its first heard row is valid.
    static dsd_state emptyPolicyState;
    check(bridge.addTalkgroup(77, 77, QStringLiteral("0"), 0, "First heard", true, 0, false));
    check(dsd_app_drain_cmds(&opts, &emptyPolicyState) == 1);
    check(dsd_tg_policy_entry_count(&emptyPolicyState) == 1);
    dsd_state_ext_free_all(&emptyPolicyState);
    // The lifecycle case above owns a separate policy fixture.
    check(dsd_tg_policy_clear(&state) == 0);
    entry = {};
    check(dsd_tg_policy_make_exact_entry(42, "A", "Dispatch", DSD_TG_POLICY_SOURCE_IMPORTED, &entry) == 0);
    check(dsd_tg_policy_append_exact(&state, &entry) == 0);
    uint64_t context = 0;
    unsigned int generation = 0;
    // Use the same publish -> consumer -> model path as the Android editor.
    const auto refreshVersion = [&]() {
        dsd_app_telemetry_publish_snapshot(&state);
        talkgroups.refresh(&opts, dsd_app_get_latest_snapshot());
        context = talkgroups.policyContext().toULongLong();
        generation = talkgroups.policyGeneration();
    };
    refreshVersion();
    const QString version = QString::number(context);
    check(!bridge.renameTalkgroup(42, 42, version, generation, QString(50, QLatin1Char('x'))));
    check(!bridge.setTalkgroupPolicy(42, 42, version, generation, {{"priority", 101}}));
    check(bridge.setTalkgroupPolicy(42, 42, version, generation,
                                    {{"name", "Fire"}, {"listening", true}, {"priority", 50}, {"preempt", true}}));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(dsd_tg_policy_entry_at(&state, 0, &entry));
    check(entry.priority == 50 && entry.preempt == 1 && QString::fromUtf8(entry.name) == "Fire");
    // The original sheet's generation must be forwarded unchanged and refused.
    check(bridge.renameTalkgroup(42, 42, version, generation, "Stale"));
    (void)dsd_app_drain_cmds(&opts, &state);
    check(dsd_tg_policy_entry_at(&state, 0, &entry));
    check(QString::fromUtf8(entry.name) == "Fire");
    check(QString::fromUtf8(state.ui_msg).contains("stale", Qt::CaseInsensitive));
    refreshVersion();
    check(bridge.renameTalkgroup(42, 42, version, generation, "Renamed"));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(dsd_tg_policy_entry_at(&state, 0, &entry));
    check(entry.priority == 50 && entry.preempt == 1 && QString::fromUtf8(entry.name) == "Renamed");
    refreshVersion();
    check(bridge.addTalkgroup(55, 55, version, generation, "Heard", false, 25, false));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(dsd_tg_policy_entry_count(&state) == 2);
    refreshVersion();
    check(bridge.removeTalkgroup(55, 55, version, generation));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(dsd_tg_policy_entry_count(&state) == 1);
    refreshVersion();
    QTemporaryDir exportDir;
    check(exportDir.isValid());
    const QString exportPath = exportDir.filePath("groups.csv");
    check(bridge.saveTalkgroupList(version, generation, exportPath));
    check(!QFile::exists(exportPath));
    check(controller.talkgroupExportResult().isEmpty());
    bool restarted = false;
    bool restartHasGroupFile = false;
    QObject::connect(&host, &dsd_qt::DecoderHost::localDeviceAttached, &controller, [&]() {
        // The saved map is associated by the retained completion before argv is built.
        const auto completion = controller.talkgroupExportResult();
        QVariantMap system{{"sourceType", "rtltcp"},
                           {"host", "127.0.0.1"},
                           {"port", 1234},
                           {"freqMhz", "851.5"},
                           {"groupCsvPath", completion.value("path")}};
        dsd_qt::SessionArgsError error;
        const QStringList args = dsd_qt::session_args_build(system, {}, &error);
        const int group = args.indexOf("-G");
        restartHasGroupFile =
            error == dsd_qt::SessionArgsError::None && group >= 0 && args.value(group + 1) == exportPath;
        restarted = true;
    });
    host.onRefresh = [&]() {
        if (host.phase == Host::Idle) {
            return;
        }
        check(dsd_app_drain_cmds(&opts, &state) == 1);
        check(QFile::exists(exportPath));
        dsd_app_frontend_runtime_stop();
        host.setPhase(Host::Idle);
        Q_EMIT host.localDeviceAttached(QStringLiteral("RTL-SDR"));
    };
    (void)dsd_app_frontend_redraw_consume();
    QEventLoop exportLoop;
    QTimer::singleShot(65, &exportLoop, &QEventLoop::quit);
    controller.start();
    exportLoop.exec();
    controller.stop();
    check(restarted && restartHasGroupFile);
    host.onRefresh = {};
    const auto exported = controller.talkgroupExportResult();
    check(exported.value("success").toBool());
    check(exported.value("path").toString() == exportPath);
    check(exported.value("policyContext").toString() == version);
    check(exported.value("policyGeneration").toUInt() == generation);
    check(exported.value("sequence").toString() != "0");
    check(QString::fromUtf8(opts.group_in_file) == exportPath);
    check(!bridge.renameTalkgroup(42, 42, version, generation, "After stop"));
    check(dsd_app_drain_cmds(&opts, &state) == 0);
    freeState(&state);
    return 0;
}
