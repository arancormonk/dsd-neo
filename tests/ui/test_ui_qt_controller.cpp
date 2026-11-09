// SPDX-License-Identifier: GPL-3.0-or-later
#include <QAbstractListModel>
#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QMap>
#include <QMetaMethod>
#include <QMetaType>
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
#include <QVariantList>
#include <QVariantMap>
#include <Qt>
#include <QtGlobal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dsd-neo/app_control/call_view.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/history_tap.h>
#include <dsd-neo/app_control/rx_tone_view.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
/* The scan-list store helpers (issue #672) have no C++ linkage of their own. */
extern "C" {
#include <dsd-neo/core/state.h>
}
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/scan_row_edit.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <functional>
#include <initializer_list> // IWYU pragma: keep -- braced range-for lists require this header.
#include <memory>
#include <stdint.h>
#include <time.h>
#include <utility>
#include <vector>
#include "../../android/local_device_state.h"
#include "../../src/app_control/commands_internal.h"
#include "../../src/app_control/snapshot_internal.h"
#include "../test_support/qt_test_paths.h"
#include "app_prefs.h"
#include "auto_start_attachment.h"
#include "call_history_model.h"
#include "command_bridge.h"
#include "decoder_host.h"
#include "decryption_profiles_model.h"
#include "diagnostics_log.h"
#include "json_store.h"
#include "metrics_model.h"
#include "p25_network_model.h"
#include "qml_gc_policy.h"
#include "saved_systems_model.h"
#include "scan_avoid_list_model.h"
#include "scan_list_starter.h"
#include "scan_lists_model.h"
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

// #647 WP3: the record's outstanding access request is kept with the rest of it; a record without one has none.
static void
test_usb_request_record() {
    Host host;
    dsd_android::LocalDeviceState usb;
    int properties = 0;
    QObject::connect(&host, &dsd_qt::DecoderHost::localDeviceChanged, [&]() { ++properties; });
    usb.apply(host, QJsonObject{{"ready", false}, {"text", "Plug in an RTL-SDR"}});
    check(properties == 1 && !usb.pending && usb.requestedSource.isEmpty() && usb.requestedSerial.isEmpty());
    const QJsonObject pending{{"ready", false},
                              {"text", "Plug in an RTL-SDR"},
                              {"pending", true},
                              {"requestedSource", "airspy"},
                              {"requestedSerial", "26a464dc"}};
    usb.apply(host, pending);
    check(properties == 2 && usb.pending && usb.requestedSource == "airspy" && usb.requestedSerial == "26a464dc");
    usb.apply(host, pending);
    check(properties == 2);
    QJsonObject serial = pending;
    serial.insert("requestedSerial", "");
    usb.apply(host, serial);
    check(properties == 3 && usb.requestedSerial.isEmpty() && usb.requestedSource == "airspy");
    serial.insert("pending", false);
    usb.apply(host, serial);
    check(properties == 4 && !usb.pending);
    // Desktop hosts broker nothing: no request, and every session's receiver reads healthy.
    check(!host.property("localDeviceRequestPending").toBool());
    check(host.property("localDeviceRequestedSource").toString().isEmpty());
    check(host.property("receiverHealth").toMap().value("state").toString() == "ok");
}

// #647 B11: refusing to switch receivers mid-session is feedback on the request; the connection still works.
static void
test_usb_refusal_record() {
    Host host;
    dsd_android::LocalDeviceState usb;
    const QJsonObject ready{{"ready", true}, {"text", "Ready: RTL-SDR"}, {"kind", ""}, {"source", "usb"}};
    usb.apply(host, ready);
    QJsonObject refused = ready;
    refused.insert("text", "Stop the current receiver before switching USB devices");
    refused.insert("kind", "refused");
    int properties = 0;
    QObject::connect(&host, &dsd_qt::DecoderHost::localDeviceChanged, [&]() { ++properties; });
    usb.apply(host, refused);
    check(properties == 1 && usb.ready && usb.text == "Stop the current receiver before switching USB devices");
    check(usb.failureKind == Host::NoDeviceFailure);
    QJsonObject failed = refused;
    failed.insert("kind", "open_failed");
    usb.apply(host, failed);
    check(properties == 2 && usb.failureKind == Host::DeviceOpenFailed);
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
            // The decoder stamps a call heard under a running scan (issue #671); the history reads the stamp, and
            // grants the saved-system UID to the no-scan rows only when the controller hands it the options.
            item.scan_member = mode != 2 ? 1U : 0U;
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

// Issue #708, B1: the first tick after the app returns takes History over from its background tap, folding in what
// the tap found, even on a tick with no redraw (a session that ended while the app was away publishes none).
static void
test_history_handoff_after_resume_without_redraw() {
    auto opts = std::make_unique<dsd_opts>();
    auto* state = static_cast<dsd_state*>(calloc(1, sizeof(dsd_state)));
    initOpts(opts.get());
    initState(state);
    for (int slot = 0; slot < 2; ++slot) {
        // Past every revision the earlier tests published, so the publisher copies this ring.
        state->event_history_s[slot].revision = 50000000U;
    }
    dsd_app_telemetry_publish_opts_snapshot(opts.get());
    dsd_app_telemetry_publish_snapshot(state);
    {
        dsd_qt::CallHistoryModel history;
        history.setHistoryTapSource(&dsd_app_history_tap_copy);
        history.setBackgroundCaptureTimingForTest(2, 5);
        dsd_qt::UiController controller(nullptr, nullptr, &history, nullptr);
        controller.setPollIntervalMs(50);
        controller.flushHistory();
        const int before = history.count();
        Q_EMIT qGuiApp->applicationStateChanged(Qt::ApplicationSuspended);
        auto& ring = state->event_history_s[0];
        auto& item = ring.Event_History_Items[1];
        item.category = DSD_EVENT_CATEGORY_VOICE;
        item.target_id = 9100;
        item.source_id = 8100;
        item.event_start_time = 1754900000;
        item.event_time = item.event_start_time + 4;
        ++ring.push_seq;
        ++ring.commit_rev;
        ++ring.revision;
        dsd_app_telemetry_publish_snapshot(state);
        history.waitForBackgroundPollForTest();
        (void)dsd_app_frontend_redraw_consume();
        Q_EMIT qGuiApp->applicationStateChanged(Qt::ApplicationActive);
        check(history.backgroundCaptureHandoffOwed() && history.count() == before);
        QEventLoop loop;
        QTimer::singleShot(65, &loop, &QEventLoop::quit);
        controller.start();
        loop.exec();
        controller.stop();
        check(!history.backgroundCaptureHandoffOwed() && !history.backgroundCaptureRunningForTest());
        check(history.count() == before + 1
              && history.data(history.index(0), dsd_qt::CallHistoryModel::TgRole).toULongLong() == 9100U);
    }
    freeState(state);
    free(state);
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
    state.scan_timing.started_m = dsd_decode_now_mono_s();
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

/* Every NOTIFY signal MetricsModel has, so a test can watch each publication. The list is checked against the meta
   object, so a signal added later cannot go unwatched. */
static void
connectEveryMetricsSignal(dsd_qt::MetricsModel& metrics, const std::function<void()>& onEmit) {
    using Signal = void (dsd_qt::MetricsModel::*)();
    static const Signal kSignals[] = {
        &dsd_qt::MetricsModel::siteChanged,           &dsd_qt::MetricsModel::qualityChanged,
        &dsd_qt::MetricsModel::tunerChanged,          &dsd_qt::MetricsModel::slot1Changed,
        &dsd_qt::MetricsModel::slot2Changed,          &dsd_qt::MetricsModel::leadSlotChanged,
        &dsd_qt::MetricsModel::twoSlotCarrierChanged, &dsd_qt::MetricsModel::controlChanged,
        &dsd_qt::MetricsModel::scanTimingChanged,     &dsd_qt::MetricsModel::scanAvoidsChanged,
        &dsd_qt::MetricsModel::rxToneChanged,         &dsd_qt::MetricsModel::rxToneConfiguredTextChanged,
        &dsd_qt::MetricsModel::toneFilterChanged,     &dsd_qt::MetricsModel::toneFilterSettingChanged,
        &dsd_qt::MetricsModel::tetraChanged,          &dsd_qt::MetricsModel::uiMessageChanged,
        &dsd_qt::MetricsModel::decodeNowMsChanged,
    };
    std::vector<QMetaMethod> watched;
    for (const Signal signal : kSignals) {
        watched.push_back(QMetaMethod::fromSignal(signal));
        QObject::connect(&metrics, signal, [onEmit]() { onEmit(); });
    }
    const QMetaObject* meta = metrics.metaObject();
    for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
        const QMetaProperty property = meta->property(i);
        if (!property.hasNotifySignal()) {
            continue;
        }
        bool found = false;
        for (const QMetaMethod& method : watched) {
            found = found || method == property.notifySignal();
        }
        check(found);
    }
}

/* What the Monitor screen's gates read, at one publication. */
struct MetricsGates {
    bool radio_input;
    bool options_known;
    bool scan_rotation_active;
    bool squelch_noise_offered;
    bool two_slot_carrier;
};

static MetricsGates
readGates(const dsd_qt::MetricsModel& metrics) {
    return MetricsGates{metrics.radioInput(), metrics.optionsKnown(), metrics.scanRotationActive(),
                        metrics.squelchNoiseOffered(), metrics.twoSlotCarrier()};
}

static void
seedScanTarget(dsd_opts* opts, dsd_state* state, unsigned int ordinal) {
    initOpts(opts);
    initState(state);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_scan_enabled = 1;
    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->tdma_carrier = 1U;
    state->trunk_scan_active_ordinal = ordinal;
}

static void
pollController(dsd_qt::UiController& controller, const dsd_opts* opts, const dsd_state* state) {
    dsd_app_telemetry_publish_opts_snapshot(opts);
    dsd_app_telemetry_publish_snapshot(state);
    dsd_app_request_redraw();
    QEventLoop loop;
    QTimer::singleShot(65, &loop, &QEventLoop::quit);
    controller.start();
    loop.exec();
    controller.stop();
}

// Issue #782: a scan-target change publishes the new target once; no reading passes through its default on the way.
static void
test_target_change_publishes_once() {
    static dsd_opts opts;
    static dsd_state state;
    seedScanTarget(&opts, &state, 1U);
    Host host;
    dsd_qt::MetricsModel metrics;
    dsd_qt::UiController controller(&host, &metrics, nullptr, nullptr);
    controller.setPollIntervalMs(50);
    pollController(controller, &opts, &state);
    const MetricsGates before = readGates(metrics);
    check(before.radio_input && before.options_known && before.scan_rotation_active && before.squelch_noise_offered
          && before.two_slot_carrier);
    check(!metrics.syncLabel().isEmpty());

    std::vector<MetricsGates> seen;
    int controlChanges = 0;
    connectEveryMetricsSignal(metrics, [&]() { seen.push_back(readGates(metrics)); });
    QObject::connect(&metrics, &dsd_qt::MetricsModel::controlChanged, [&]() { ++controlChanges; });
    state.trunk_scan_active_ordinal = 2U;
    pollController(controller, &opts, &state);
    check(!seen.empty()); // The new ordinal is itself a published change.
    for (const MetricsGates& gates : seen) {
        check(gates.radio_input);
        check(gates.options_known);
        check(gates.scan_rotation_active);
        check(gates.squelch_noise_offered);
        check(gates.two_slot_carrier);
    }
    check(controlChanges <= 1);
    check(metrics.scanTargetOrdinal() == 2);
    freeState(&state);
}

// Issue #782: the old target's sync hold does not caption the new one, not even for one publication.
static void
test_target_change_drops_sync_hold() {
    static dsd_opts opts;
    static dsd_state state;
    seedScanTarget(&opts, &state, 1U);
    Host host;
    dsd_qt::MetricsModel metrics;
    dsd_qt::UiController controller(&host, &metrics, nullptr, nullptr);
    controller.setPollIntervalMs(50);
    pollController(controller, &opts, &state);
    check(metrics.syncedHere() && !metrics.syncLabel().isEmpty());

    bool labelled = false;
    connectEveryMetricsSignal(metrics, [&]() { labelled = labelled || !metrics.syncLabel().isEmpty(); });
    state.synctype = DSD_SYNC_NONE;
    state.lastsynctype = DSD_SYNC_NONE;
    state.trunk_scan_active_ordinal = 2U;
    pollController(controller, &opts, &state);
    check(metrics.syncLabel().isEmpty() && !metrics.syncedHere());
    check(!labelled);
    check(metrics.radioInput() && metrics.optionsKnown()); // Only the sync hold went, not the readings.
    freeState(&state);
}

// Issue #782: with the session stopped, a target change still takes the readings down, in one publication.
static void
test_target_change_while_stopped_clears() {
    static dsd_opts opts;
    static dsd_state state;
    seedScanTarget(&opts, &state, 1U);
    Host host;
    dsd_qt::MetricsModel metrics;
    dsd_qt::UiController controller(&host, &metrics, nullptr, nullptr);
    controller.setPollIntervalMs(50);
    pollController(controller, &opts, &state);
    check(metrics.radioInput() && metrics.optionsKnown());
    host.setPhase(Host::Idle);
    pollController(controller, &opts, &state); // The stopped session's target is the one on record now.
    metrics.refresh(&opts, &state);            // Readings left over from the stopped session, seeded directly.
    check(metrics.radioInput() && metrics.optionsKnown() && !metrics.syncLabel().isEmpty());

    int controlChanges = 0;
    QObject::connect(&metrics, &dsd_qt::MetricsModel::controlChanged, [&]() { ++controlChanges; });
    state.trunk_scan_active_ordinal = 2U;
    pollController(controller, &opts, &state);
    check(controlChanges == 1);
    check(!metrics.radioInput() && !metrics.optionsKnown() && !metrics.scanRotationActive());
    check(!metrics.twoSlotCarrier() && metrics.syncLabel().isEmpty());
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
    controller.requestAutoStart(true, true, "saved", "target", usb, "usb");
    check(requests == 0); // No QML binding installed yet.
    controller.setProperty("autoStartBlocked", false);
    controller.requestAutoStart(true, true, "saved", "target", usb, "usb");
    check(requests == 1 && requestedKind == "saved");
    controller.requestAutoStart(true, true, "scan", "target", usb, "usb");
    check(requests == 2 && requestedKind == "scan");
    controller.requestAutoStart(true, true, "explore", "target", usb, "usb");
    controller.requestAutoStart(true, true, "saved", "deleted", usb, "usb");
    controller.requestAutoStart(true, true, "saved", "target", {}, "usb");
    controller.requestAutoStart(true, true, "saved", "", {}, "usb");
    for (const char* source : {"rtltcp", "udp", "tcp", "file", ""}) {
        // Not even an attachment that names the same kind makes a network or file target a receiver's.
        controller.requestAutoStart(true, true, "saved", "target", {{"uid", "target"}, {"sourceType", source}}, source);
    }
    controller.requestAutoStart(false, true, "saved", "target", usb, "usb");
    controller.requestAutoStart(true, false, "saved", "target", usb, "usb");
    for (auto phase : {Host::Starting, Host::Running, Host::Stopping, Host::Failed}) {
        host.phase = phase;
        controller.requestAutoStart(true, true, "saved", "target", usb, "usb");
    }
    host.phase = Host::Idle;
    controller.setProperty("autoStartBlocked", true);
    controller.requestAutoStart(true, true, "saved", "target", usb, "usb");
    controller.setProperty("autoStartBlocked", false);
    check(requests == 2); // Suppressed requests do not retry when the gate opens.
}

// Issue #697: a scan session's other lists ride with an auto-start, in their order; any other kind carries none.
static void
test_auto_start_carries_the_scan_bundle() {
    Host host;
    host.phase = Host::Idle;
    dsd_qt::UiController controller(&host, nullptr, nullptr, nullptr);
    controller.setProperty("autoStartBlocked", false);
    QStringList requested;
    QObject::connect(&controller, &dsd_qt::UiController::autoStartRequested,
                     [&](const QString& kind, const QString& uid, const QStringList& companions) {
                         requested << kind + ":" + uid + ":" + companions.join(',');
                     });
    const QVariantMap target{{"uid", "security"}, {"sourceType", "usb"}};
    controller.requestAutoStart(true, true, "scan", "security", target, "usb", {"hospital", "parks"});
    controller.requestAutoStart(true, true, "scan", "security", target, "usb");
    controller.requestAutoStart(true, true, "saved", "security", target, "usb", {"hospital"});
    check(requested == (QStringList{"scan:security:hospital,parks", "scan:security:", "saved:security:"}));
    // A start a gate refuses carries nothing.
    requested.clear();
    controller.requestAutoStart(false, true, "scan", "security", target, "usb", {"hospital"});
    check(requested.isEmpty());
}

// Issue #697: the bundle's preferences are device-local, and a load that re-mints a list UID (a duplicate or missing
// one, as a hand-edited or restored store can hold) is what drops them: the UIDs they name may be other lists now.
static void
test_scan_bundle_prefs_and_uid_repair() {
    {
        dsd_qt::AppPrefs prefs;
        prefs.setAcceptedStartCompanions({"b", "c"});
        prefs.setLastStartedCompanions({"b"});
        check(prefs.acceptedStartCompanions() == QStringList({"b", "c"})
              && prefs.lastStartedCompanions() == QStringList{"b"});
        prefs.setLastStartedCompanions({});
        check(prefs.lastStartedCompanions().isEmpty());
        prefs.setLastStartedCompanions({"b"});
    }
    for (const auto& spec : dsd_qt::AppPrefs::keyTable()) {
        if (QString::fromLatin1(spec.name).endsWith(QStringLiteral("Companions"))) {
            check(!spec.portable);
        }
    }
    check(!dsd_qt::AppPrefs::portableKeys().contains(QStringLiteral("listen/acceptedStartCompanions")));
    // A clean store loads as it was.
    QJsonArray rows{QJsonObject{{"uid", "a"}, {"name", "A"}}, QJsonObject{{"uid", "b"}, {"name", "B"}}};
    check(dsd_qt::json_store_save_array(QStringLiteral("scan_lists.json"), rows));
    {
        const dsd_qt::ScanListsModel lists;
        check(lists.count() == 2 && !lists.uidsRepaired());
    }
    {
        const dsd_qt::AppPrefs prefs;
        check(prefs.acceptedStartCompanions() == QStringList({"b", "c"})
              && prefs.lastStartedCompanions() == QStringList{"b"});
    }
    // A UID collision (a restored or hand-edited store): the second "a" is given a new UID, and the load forgets the
    // remembered companions, which may now name another list.
    rows.append(QJsonObject{{"uid", "a"}, {"name", "A copy"}});
    check(dsd_qt::json_store_save_array(QStringLiteral("scan_lists.json"), rows));
    {
        const dsd_qt::ScanListsModel lists;
        check(lists.count() == 3 && lists.uidsRepaired() && lists.get(2).value("uid").toString() != "a");
    }
    {
        const dsd_qt::AppPrefs prefs;
        check(prefs.acceptedStartCompanions().isEmpty() && prefs.lastStartedCompanions().isEmpty());
    }
    // And one with none.
    {
        dsd_qt::AppPrefs prefs;
        prefs.setAcceptedStartCompanions({"b"});
    }
    check(dsd_qt::json_store_save_array(QStringLiteral("scan_lists.json"), QJsonArray{QJsonObject{{"name", "N"}}}));
    {
        const dsd_qt::ScanListsModel lists;
        check(lists.uidsRepaired());
    }
    {
        const dsd_qt::AppPrefs prefs;
        check(prefs.acceptedStartCompanions().isEmpty());
    }
    check(dsd_qt::json_store_save_array(QStringLiteral("scan_lists.json"), QJsonArray{}));
    // What Home and the Scan with other lists sheet count: the entries, or the rows a CSV list's import accepted.
    const dsd_qt::ScanListsModel lists;
    check(lists.entryCount({{"targetSource", "entries"}, {"entries", QVariantList{QVariantMap{}, QVariantMap{}}}})
          == 2);
    const QVariantMap csv{{"targetSource", "csv"}, {"targetsCsvPath", "/imports/targets.csv"}};
    check(lists.entryCount(csv) == 0);
    dsd_qt::ScanListsModel looked;
    looked.setAcceptedRowsLookup([](const QString& path) { return path == "/imports/targets.csv" ? 12 : -1; });
    check(looked.entryCount(csv) == 12);
    check(looked.entryCount({{"targetSource", "csv"}, {"targetsCsvPath", "/gone.csv"}}) == 0);
}

/* Scan-list repair warnings so far. The log tap is installed once and never replaced, so what it counts into must live
 * as long as the process. */
static int g_scan_repair_warnings = 0;

static void
count_scan_repair_warnings() {
    dsd_neo_log_set_tap(
        [](dsd_neo_log_level_t level, const char* text, void*) {
            if (level == LOG_LEVEL_WARN && QString::fromUtf8(text).contains("Scan-list")) {
                ++g_scan_repair_warnings;
            }
        },
        nullptr);
}

// Issue #801: a UID the load mints is saved, so the next load gives the list the same one (a pinned shortcut or a
// remembered scan session names it). A store the save cannot replace keeps the minted UIDs for this run and says so.
static void
test_scan_list_uid_repair_persists() {
    count_scan_repair_warnings();
    const QString store = QStringLiteral("scan_lists.json");
    check(dsd_qt::json_store_save_array(store, QJsonArray{QJsonObject{{"name", "No uid"}},
                                                          QJsonObject{{"uid", "a"}, {"name", "A"}},
                                                          QJsonObject{{"uid", "a"}, {"name", "A copy"}},
                                                          QJsonObject{{"uid", 7}, {"name", "Numbered"}}}));
    QString minted, copy;
    {
        const dsd_qt::ScanListsModel lists;
        check(lists.count() == 4 && lists.uidsRepaired() && lists.get(3).value("uid") == "7");
        minted = lists.get(0).value("uid").toString();
        copy = lists.get(2).value("uid").toString();
        check(!minted.isEmpty() && copy != "a" && lists.get(1).value("uid") == "a");
    }
    const QJsonArray stored = dsd_qt::json_store_load_array(store);
    check(stored.size() == 4 && stored.at(0).toObject().value("uid").toString() == minted
          && stored.at(2).toObject().value("uid").toString() == copy && stored.at(3).toObject().value("uid") == "7");
    {
        const dsd_qt::ScanListsModel lists;
        check(!lists.uidsRepaired() && lists.get(0).value("uid") == minted && lists.get(2).value("uid") == copy
              && lists.get(3).value("uid") == "7");
    }
    // A store the app can read but not replace: the minted UID holds for this run, and the failure is logged once.
    check(dsd_qt::json_store_save_array(store, QJsonArray{QJsonObject{{"name", "Unsaved"}}}));
    const QString path = dsd_qt::json_store_path(store);
    const auto permissions = QFile::permissions(path);
    check(QFile::setPermissions(path, QFileDevice::ReadOwner));
    const int before = g_scan_repair_warnings;
    {
        const dsd_qt::ScanListsModel lists;
        check(lists.count() == 1 && lists.uidsRepaired() && !lists.get(0).value("uid").toString().isEmpty());
        check(g_scan_repair_warnings - before == 1);
    }
    check(QFile::setPermissions(path, permissions));
    check(!dsd_qt::json_store_load_array(store).at(0).toObject().contains("uid"));
    check(dsd_qt::json_store_save_array(store, QJsonArray{}));
}

namespace {
/* The uid of entry @p entry of list @p row. */
QString
entry_uid(const dsd_qt::ScanListsModel& lists, int row, int entry) {
    return lists.get(row).value("entries").toList().value(entry).toMap().value("uid").toString();
}

/* The uid stored for entry @p entry of list @p row. */
QString
stored_entry_uid(const QJsonArray& rows, int row, int entry) {
    return rows.at(row).toObject().value("entries").toArray().at(entry).toObject().value("uid").toString();
}

/* A frequency entry named @p uid, none when empty. */
QJsonObject
uid_entry(const QString& uid, const QString& freq) {
    QJsonObject entry{{"kind", "freq"}, {"protocol", "p25"}, {"freqMhz", freq}};
    if (!uid.isEmpty()) {
        entry.insert("uid", uid);
    }
    return entry;
}
} // namespace

// Issue #697: a target id names one target in a session, so no two of the app's lists share an entry uid. One an
// earlier list holds (a hand-edited or older store) is given another at load, the first list keeping it, and saved, so
// the next load keeps it; list uids, and so the remembered companions and pins that name lists, are left alone. A store
// the save cannot replace keeps the new uid for this run.
static void
test_scan_list_entry_uid_repair_at_load() {
    count_scan_repair_warnings();
    const QString store = QStringLiteral("scan_lists.json");
    {
        dsd_qt::AppPrefs prefs;
        prefs.setAcceptedStartCompanions({"b"});
    }
    const QJsonArray rows{
        QJsonObject{
            {"uid", "a"}, {"name", "A"}, {"entries", QJsonArray{uid_entry("e1", "851.0"), uid_entry("e2", "852.0")}}},
        QJsonObject{
            {"uid", "b"}, {"name", "B"}, {"entries", QJsonArray{uid_entry("e1", "853.0"), uid_entry("e3", "854.0")}}},
        QJsonObject{
            {"uid", "c"}, {"name", "C"}, {"entries", QJsonArray{uid_entry("e2", "855.0"), uid_entry("", "856.0")}}}};
    check(dsd_qt::json_store_save_array(store, rows));
    QStringList minted;
    {
        const dsd_qt::ScanListsModel lists;
        check(lists.count() == 3 && !lists.uidsRepaired() && lists.get(0).value("uid") == "a"
              && lists.get(1).value("uid") == "b" && lists.get(2).value("uid") == "c");
        check(entry_uid(lists, 0, 0) == "e1" && entry_uid(lists, 0, 1) == "e2" && entry_uid(lists, 1, 1) == "e3");
        minted = {entry_uid(lists, 1, 0), entry_uid(lists, 2, 0), entry_uid(lists, 2, 1)};
        for (const QString& uid : minted) {
            check(!uid.isEmpty() && uid != "e1" && uid != "e2" && uid != "e3");
        }
        check(minted.at(0) != minted.at(1) && minted.at(1) != minted.at(2) && minted.at(0) != minted.at(2));
    }
    {
        dsd_qt::AppPrefs prefs;
        check(prefs.acceptedStartCompanions() == QStringList{"b"});
        prefs.setAcceptedStartCompanions({});
    }
    QJsonArray stored = dsd_qt::json_store_load_array(store);
    check(stored.size() == 3 && stored_entry_uid(stored, 0, 0) == "e1" && stored_entry_uid(stored, 1, 0) == minted.at(0)
          && stored_entry_uid(stored, 2, 0) == minted.at(1) && stored_entry_uid(stored, 2, 1) == minted.at(2));
    // The next load mints nothing and saves nothing.
    {
        const dsd_qt::ScanListsModel lists;
        check(entry_uid(lists, 1, 0) == minted.at(0) && entry_uid(lists, 2, 0) == minted.at(1)
              && entry_uid(lists, 2, 1) == minted.at(2));
    }
    check(dsd_qt::json_store_load_array(store) == stored);

    // A store the app can read but not replace: the new uid holds for this run, and the failure is logged once.
    check(dsd_qt::json_store_save_array(store, rows));
    const QString path = dsd_qt::json_store_path(store);
    const auto permissions = QFile::permissions(path);
    check(QFile::setPermissions(path, QFileDevice::ReadOwner));
    const int before = g_scan_repair_warnings;
    {
        const dsd_qt::ScanListsModel lists;
        check(entry_uid(lists, 0, 0) == "e1" && !entry_uid(lists, 1, 0).isEmpty() && entry_uid(lists, 1, 0) != "e1");
        check(g_scan_repair_warnings - before == 1);
    }
    check(QFile::setPermissions(path, permissions));
    check(stored_entry_uid(dsd_qt::json_store_load_array(store), 1, 0) == "e1");
    check(dsd_qt::json_store_save_array(store, QJsonArray{}));
}

// Issue #697: add() and update() give an entry whose uid another list holds a new one, as the load does; the list being
// updated is no conflict with itself, so its own entries keep theirs.
static void
test_scan_list_entry_uid_repair_on_save() {
    const QString store = QStringLiteral("scan_lists.json");
    check(dsd_qt::json_store_save_array(
        store,
        QJsonArray{QJsonObject{{"uid", "a"}, {"name", "A"}, {"entries", QJsonArray{uid_entry("e1", "851.0")}}}}));
    dsd_qt::ScanListsModel lists;
    const QVariantMap added{
        {"name", "B"},
        {"entries", QVariantList{uid_entry("e1", "853.0").toVariantMap(), uid_entry("e4", "854.0").toVariantMap()}}};
    check(lists.add(added) && lists.count() == 2);
    const QString addedUid = entry_uid(lists, 1, 0);
    check(entry_uid(lists, 0, 0) == "e1" && !addedUid.isEmpty() && addedUid != "e1" && entry_uid(lists, 1, 1) == "e4");
    // A's own e1 is no conflict; B's e4 is.
    check(lists.update(0, {{"entries", QVariantList{uid_entry("e1", "851.0").toVariantMap(),
                                                    uid_entry("e4", "852.0").toVariantMap()}}}));
    const QString updatedUid = entry_uid(lists, 0, 1);
    check(entry_uid(lists, 0, 0) == "e1" && !updatedUid.isEmpty() && updatedUid != "e4" && updatedUid != addedUid
          && entry_uid(lists, 1, 0) == addedUid && entry_uid(lists, 1, 1) == "e4");
    // An update that leaves the entries alone keeps every uid.
    check(lists.update(1, {{"name", "B renamed"}}) && entry_uid(lists, 1, 0) == addedUid
          && entry_uid(lists, 1, 1) == "e4");
    const QJsonArray stored = dsd_qt::json_store_load_array(store);
    check(stored.size() == 2 && stored_entry_uid(stored, 0, 0) == "e1" && stored_entry_uid(stored, 0, 1) == updatedUid
          && stored_entry_uid(stored, 1, 0) == addedUid && stored_entry_uid(stored, 1, 1) == "e4");
    check(dsd_qt::json_store_save_array(store, QJsonArray{}));
}

namespace {
// A host that keeps the argv of each start it is asked for.
class StartHost : public Host {
  public:
    QList<QStringList> starts;

    bool
    start(const QStringList& args) override {
        starts << args;
        return true;
    }
};

QVariantMap
manual_list(dsd_qt::ScanListsModel& lists, const QString& name, const QString& entryUid, const QString& freq,
            const QVariantMap& overrides = {}) {
    QVariantMap list{{"name", name},
                     {"sourceType", "rtltcp"},
                     {"host", "127.0.0.1"},
                     {"port", 1234},
                     {"entries", QVariantList{QVariantMap{{"uid", entryUid},
                                                          {"kind", "freq"},
                                                          {"name", name + " channel"},
                                                          {"protocol", "p25"},
                                                          {"freqMhz", freq},
                                                          {"enabled", true}}}}};
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it) {
        list.insert(it.key(), it.value());
    }
    check(lists.add(list));
    return lists.get(lists.count() - 1);
}
} // namespace

// Issue #697: several lists start as one session, the first list's receiver with one --trunk-scan each, in order;
// a list that cannot join starts nothing at all.
static void
test_scan_lists_start_together() {
    dsd_qt::AppPrefs prefs;
    dsd_qt::SavedSystemsModel systems;
    dsd_qt::ScanListsModel lists;
    while (lists.count() > 0) {
        lists.remove(0);
    }
    dsd_qt::ScanListStarter starter(&prefs, &systems);
    StartHost host;
    host.phase = Host::Idle;
    const QVariantMap security = manual_list(lists, "Security", "gate-a", "851.0125");
    const QVariantMap hospital = manual_list(lists, "Hospital", "er-desk", "852.0125");
    const QVariantMap parks = manual_list(lists, "Parks", "ranger", "853.0125");
    const QVariantMap result = starter.startLists({security, hospital, parks}, &host);
    check(result.value("ok").toBool() && result.value("started").toBool());
    check(result.value("targetCount").toInt() == 3 && !result.contains("args"));
    check(host.starts.size() == 1);
    const QStringList args = host.starts.value(0);
    const auto csv = [](const QVariantMap& list) {
        return dsd_qt::scan_list_csv_path(list.value("uid").toString(), list.value("name").toString(), 1);
    };
    const qsizetype first = args.indexOf(QStringLiteral("--trunk-scan"));
    check(first > 0 && args.count(QStringLiteral("--trunk-scan")) == 3
          && args.mid(first, 6)
                 == QStringList(
                     {"--trunk-scan", csv(security), "--trunk-scan", csv(hospital), "--trunk-scan", csv(parks)}));
    check(args.count(QStringLiteral("-i")) == 1
          && args.contains(QStringLiteral("rtltcp:127.0.0.1:1234:851.0125M:30:0:48:0:2")));
    // One list is start(): a single --trunk-scan.
    check(starter.startLists({hospital}, &host).value("started").toBool() && host.starts.size() == 2
          && host.starts.value(1).count(QStringLiteral("--trunk-scan")) == 1);

    // A list on another receiver setting cannot join; nothing starts.
    const QVariantMap louder = manual_list(lists, "Louder", "siren", "854.0125", {{"gainDb", 40}});
    check(starter.joinConflict(security, louder) == QStringLiteral("Different gain"));
    check(starter.joinConflict(security, hospital).isEmpty());
    QVariantMap refused = starter.startLists({security, louder}, &host);
    check(!refused.value("ok").toBool() && !refused.value("started").toBool() && host.starts.size() == 2);
    check(refused.value("error").toString() == QStringLiteral("“Louder” can't scan with “Security”: different gain."));

    // A list given an entry uid another list holds: the save gives it a new one, so the two are independent targets
    // and start together.
    const QVariantMap twin = manual_list(lists, "Twin", "gate-a", "855.0125");
    const QString twinUid = twin.value("entries").toList().value(0).toMap().value("uid").toString();
    check(!twinUid.isEmpty() && twinUid != QStringLiteral("gate-a")
          && security.value("entries").toList().value(0).toMap().value("uid") == QStringLiteral("gate-a"));
    const QVariantMap together = starter.startLists({security, twin}, &host);
    check(together.value("ok").toBool() && together.value("started").toBool()
          && together.value("targetCount").toInt() == 2 && host.starts.size() == 3);
    // A draft among them: refused by name.
    QVariantMap draft = twin;
    draft.insert("isDraft", true);
    draft.insert("name", "Draft");
    refused = starter.startLists({security, draft}, &host);
    check(!refused.value("started").toBool() && host.starts.size() == 3
          && refused.value("error").toString().startsWith(QStringLiteral("“Draft”: ")));
    // Nine lists are more than a session takes.
    QVariantList nine;
    for (int i = 0; i < 9; ++i) {
        nine << security;
    }
    refused = starter.startLists(nine, &host);
    check(!refused.value("started").toBool() && host.starts.size() == 3);
    while (lists.count() > 0) {
        lists.remove(0);
    }
}

namespace {
/* @p body written to @p path, its folder made. */
void
write_csv(const QString& path, const QByteArray& body) {
    check(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write(body) == body.size());
}

QVariantMap
csv_list(dsd_qt::ScanListsModel& lists, const QString& name, const QString& path) {
    check(lists.add({{"name", name},
                     {"sourceType", "rtltcp"},
                     {"host", "127.0.0.1"},
                     {"port", 1234},
                     {"targetSource", "csv"},
                     {"targetsCsvPath", path}}));
    return lists.get(lists.count() - 1);
}
} // namespace

// Issue #697: two imported lists whose files both hold ROK start as one session that runs ROK once, when the rows are
// the same: each list runs from its own copy, so a talkgroup file both rows name is reached by two spellings (one per
// copy's folder), and is still the one file. A ROK row that differs refuses the start, naming the column, and nothing
// starts.
static void
test_scan_lists_share_a_csv_target() {
    const QTemporaryDir dir;
    check(dir.isValid());
    const QString imports = dir.filePath("imports");
    write_csv(imports + "/groups.csv", "id,mode,name\n123,B,Rock TG\n");
    const QByteArray header = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options\n";
    const QByteArray rok = "ROK,dmr-conventional,851012500,,,,,-G groups.csv\n";
    const QString conv = imports + "/conv.csv";
    const QString low = imports + "/low.csv";
    const QString other = imports + "/other.csv";
    write_csv(conv, header + rok + "A,dmr-conventional,851512500,,,,,\n");
    write_csv(low, header + "B,p25-conventional,852012500,,,,,\n" + rok);
    write_csv(other, header + "ROK,dmr-conventional,851025000,,,,,-G groups.csv\n");
    const dsd_qt::AppPrefs prefs;
    const dsd_qt::SavedSystemsModel systems;
    dsd_qt::ScanListsModel lists;
    while (lists.count() > 0) {
        lists.remove(0);
    }
    dsd_qt::ScanListStarter starter(&prefs, &systems);
    starter.setTargetFileLookup([&](const QString& path) { return path == conv || path == low || path == other; });
    StartHost host;
    host.phase = Host::Idle;
    const QVariantMap convList = csv_list(lists, "Conv", conv);
    const QVariantMap lowList = csv_list(lists, "Low", low);
    const QVariantMap otherList = csv_list(lists, "Other", other);

    const QVariantMap started = starter.startLists({convList, lowList}, &host);
    if (!started.value("started").toBool()) {
        std::fprintf(stderr, "a shared CSV target: %s\n", qPrintable(started.value("error").toString()));
    }
    check(started.value("ok").toBool() && started.value("started").toBool());
    check(started.value("targetCount").toInt() == 3 && host.starts.size() == 1);
    const QStringList args = host.starts.value(0);
    const QString convCopy = dsd_qt::scan_list_csv_path(convList.value("uid").toString(), "Conv", 1);
    const QString lowCopy = dsd_qt::scan_list_csv_path(lowList.value("uid").toString(), "Low", 2);
    const qsizetype first = args.indexOf(QStringLiteral("--trunk-scan"));
    check(first > 0 && args.mid(first, 4) == QStringList({"--trunk-scan", convCopy, "--trunk-scan", lowCopy}));
    // Each copy names the talkgroup file from its own folder: two spellings of one file.
    QFile copy(lowCopy);
    check(copy.open(QIODevice::ReadOnly)
          && copy.readAll().contains(
              QDir(QFileInfo(lowCopy).absolutePath()).relativeFilePath(imports + "/groups.csv").toUtf8()));
    check(QFileInfo(convCopy).absolutePath() != QFileInfo(lowCopy).absolutePath());

    const QVariantMap refused = starter.startLists({convList, otherList}, &host);
    check(!refused.value("ok").toBool() && !refused.value("started").toBool() && host.starts.size() == 1);
    const QString error = refused.value("error").toString();
    if (!error.startsWith(QStringLiteral("Scan lists rejected: ")) || !error.contains(QStringLiteral("'ROK'"))
        || !error.contains(QStringLiteral("frequency_hz"))) {
        std::fprintf(stderr, "a ROK row that differs: %s\n", qPrintable(error));
        check(false);
    }
    while (lists.count() > 0) {
        lists.remove(0);
    }
}

// #647 D3a: an Airspy target resumes like an RTL-SDR one, but only when the receiver that just attached is of the
// target's kind. What else is on the bus does not count, so the second receiver plugged in, in either order, resumes
// only a target on its own kind.
static void
test_auto_start_follows_the_attached_kind() {
    Host host;
    host.phase = Host::Idle;
    dsd_qt::UiController controller(&host, nullptr, nullptr, nullptr);
    controller.setProperty("autoStartBlocked", false);
    QStringList requested;
    QObject::connect(&controller, &dsd_qt::UiController::autoStartRequested,
                     [&](const QString& kind, const QString& uid) { requested << kind + ":" + uid; });
    const QVariantMap airspy{{"uid", "air"}, {"sourceType", "airspy"}};
    const QVariantMap rtl{{"uid", "rtl"}, {"sourceType", "usb"}};
    controller.requestAutoStart(true, true, "saved", "air", airspy, "airspy");
    check(requested == QStringList{"saved:air"});
    controller.requestAutoStart(true, true, "scan", "air", airspy, "airspy");
    check(requested == (QStringList{"saved:air", "scan:air"}));
    requested.clear();
    controller.requestAutoStart(true, true, "saved", "air", airspy, "usb");
    controller.requestAutoStart(true, true, "saved", "rtl", rtl, "airspy");
    controller.requestAutoStart(true, true, "saved", "air", airspy, "");
    controller.requestAutoStart(true, true, "saved", "rtl", rtl, "fixture-usb");
    check(requested.isEmpty());
    for (const QStringList& order : {QStringList{"usb", "airspy"}, QStringList{"airspy", "usb"}}) {
        for (const QVariantMap& target : {airspy, rtl}) {
            requested.clear();
            const QString uid = target.value("uid").toString();
            for (const QString& attached : order) {
                controller.requestAutoStart(true, true, "saved", uid, target, attached);
            }
            check(requested == QStringList{"saved:" + uid});
        }
    }
    // A suppressed attachment of the right kind is consumed, not kept for when the gate opens.
    requested.clear();
    controller.setProperty("autoStartBlocked", true);
    controller.requestAutoStart(true, true, "saved", "air", airspy, "airspy");
    controller.setProperty("autoStartBlocked", false);
    check(requested.isEmpty());
}

// #647 D3a: the attachment the host delivers is "<kind>:<device name>"; only its kind decides, and a payload without
// one names no receiver.
static void
test_attachment_kind() {
    check(dsd_qt::attachedReceiverKind("usb:/dev/bus/usb/001/002") == "usb");
    check(dsd_qt::attachedReceiverKind("airspy:/dev/bus/usb/001/003") == "airspy");
    for (const char* payload : {"", "fixture-usb", ":x", "usb"}) {
        const QString kind = dsd_qt::attachedReceiverKind(payload);
        for (const char* source : {"usb", "airspy"}) {
            check(dsd_qt::autoStartKindMatches(source, kind)
                  == (QString(payload) == "usb" && QString(source) == "usb"));
        }
    }
    check(dsd_qt::autoStartKindMatches("usb", dsd_qt::attachedReceiverKind("usb:/dev/bus/usb/001/002")));
    check(dsd_qt::autoStartKindMatches("airspy", dsd_qt::attachedReceiverKind("airspy:/dev/bus/usb/001/003")));
    check(!dsd_qt::autoStartKindMatches("airspy", dsd_qt::attachedReceiverKind("usb:/dev/bus/usb/001/002")));
    for (const char* source : {"rtltcp", "udp", "tcp", "file", ""}) {
        check(!dsd_qt::autoStartKindMatches(source, source));
    }
}

// Issue #525: the Radio sheet's NFM width control goes through the real bridge and decoder queue as
// DSD_APP_CMD_NFM_BANDWIDTH_SET with the width as typed; the decoder applies what it accepts (0 is the default)
// and refuses the rest with the width unchanged. PCM input keeps the front end out of it.
static void
test_nfm_bandwidth_bridge() {
    dsd_qt::CommandBridge bridge;
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    opts.audio_in_type = AUDIO_IN_PULSE;
    opts.audio_out_type = 9;
    opts.analog_only = 1;
    dsd_app_frontend_runtime_start(nullptr, nullptr);
    check(bridge.setNfmBandwidthHz(12500));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(opts.analog_nfm_bandwidth_hz == 12500);
    check(bridge.setNfmBandwidthHz(30000));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(opts.analog_nfm_bandwidth_hz == 12500);
    check(bridge.setNfmBandwidthHz(0));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(opts.analog_nfm_bandwidth_hz == 0);
    freeState(&state);
}

// Issue #524: the Radio sheet's AM width control goes through the real bridge and decoder queue as
// DSD_APP_CMD_AM_BANDWIDTH_SET with the width as stepped; the decoder stores what it accepts (0 is the default) and
// refuses the rest with the width unchanged. With no radio running it is configuration only.
static void
test_am_bandwidth_bridge() {
    dsd_qt::CommandBridge bridge;
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    opts.audio_in_type = AUDIO_IN_PULSE;
    opts.audio_out_type = 9;
    dsd_app_frontend_runtime_start(nullptr, nullptr);
    check(bridge.setAmBandwidthHz(8000));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(opts.analog_am_bandwidth_hz == 8000);
    check(bridge.setAmBandwidthHz(25000));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(opts.analog_am_bandwidth_hz == 8000);
    check(bridge.setAmBandwidthHz(0));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(opts.analog_am_bandwidth_hz == 0);
    dsd_app_frontend_runtime_stop();
    freeState(&state);
}

// The reloads Qt posts after replacing a file go through the real bridge and decoder queue: reloadGroupList() with no
// path re-reads the configured file (none here, so the decoder says so), with a path only when the session uses it
// (nothing happens otherwise); reloadChannelMap() needs its path. The bridge reports queued, never reloaded.
static void
test_reload_bridge() {
    dsd_qt::CommandBridge bridge;
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    opts.audio_in_type = AUDIO_IN_PULSE;
    opts.audio_out_type = 9;
    dsd_app_frontend_runtime_start(nullptr, nullptr);
    check(bridge.reloadGroupList(QString()));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(std::strcmp(state.ui_msg, "Failed: Group list reload -> no group list file; import one first") == 0);
    state.ui_msg[0] = '\0';
    check(bridge.reloadGroupList(QStringLiteral("/tmp/dsd-neo-not-in-use.csv")));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(state.ui_msg[0] == '\0');
    check(!bridge.reloadChannelMap(QString()));
    check(bridge.reloadChannelMap(QStringLiteral("/tmp/dsd-neo-not-in-use.csv")));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(state.ui_msg[0] == '\0');
    dsd_app_frontend_runtime_stop();
    freeState(&state);
}

// The live tone-filter editor goes through the real bridge and decoder queue as DSD_APP_CMD_TONE_FILTER_SET with the
// mode and the list as typed; the decoder applies what the list parser accepts and refuses the rest with the policy
// unchanged. The bridge's inline check is that same parser's answer, empty when the edit would apply, and a list the
// payload cannot hold is refused before anything is queued.
static void
test_tone_filter_bridge() {
    dsd_qt::CommandBridge bridge;
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    opts.audio_in_type = AUDIO_IN_PULSE;
    opts.audio_out_type = 9;
    opts.analog_only = 1;
    dsd_app_frontend_runtime_start(nullptr, nullptr);
    check(bridge.toneFilterError(DSD_TONE_FILTER_ALLOW, QStringLiteral("100/d023")).isEmpty());
    check(bridge.toneFilterError(DSD_TONE_FILTER_OFF, QString()).isEmpty());
    check(bridge.toneFilterError(DSD_TONE_FILTER_ALLOW, QStringLiteral("100,67"))
          == QStringLiteral("use / between entries, not commas"));
    check(bridge.toneFilterError(DSD_TONE_FILTER_BLOCK, QString())
          == QStringLiteral("block needs a list of CTCSS tones or DCS codes, e.g. 67.0/100.0/D023N"));
    check(bridge.toneFilterError(DSD_TONE_FILTER_ALLOW, QStringLiteral("100/xyzzy"))
          == QStringLiteral("entry 2 is not a standard CTCSS tone or DCS code"));
    check(bridge.setToneFilter(DSD_TONE_FILTER_ALLOW, QStringLiteral("100/d023")));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    dsd_tone_set want = {};
    check(dsd_tone_set_parse("100.0/D023N", &want, nullptr, 0) == 0);
    check(opts.analog_tone_filter == DSD_TONE_FILTER_ALLOW && dsd_tone_set_equal(&opts.analog_tone_set, &want));
    check(bridge.setToneFilter(DSD_TONE_FILTER_BLOCK, QStringLiteral("100,67")));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(opts.analog_tone_filter == DSD_TONE_FILTER_ALLOW && dsd_tone_set_equal(&opts.analog_tone_set, &want));
    const QString overlong(DSD_APP_TONE_FILTER_LIST_SIZE, QLatin1Char('/'));
    check(!bridge.setToneFilter(DSD_TONE_FILTER_ALLOW, overlong));
    check(!bridge.toneFilterError(DSD_TONE_FILTER_ALLOW, overlong).isEmpty());
    check(dsd_app_drain_cmds(&opts, &state) == 0);
    check(bridge.setToneFilter(DSD_TONE_FILTER_OFF, QString()));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(opts.analog_tone_filter == DSD_TONE_FILTER_OFF && dsd_tone_set_count(&opts.analog_tone_set) == 0);
    dsd_app_frontend_runtime_stop();
    freeState(&state);
}

/* Issue #518: the bridge's "this channel" edits. The constants are the engine's; an editor opened with no row on air
 * sends nothing; an edit naming a row is queued as a DSD_APP_CMD_SCAN_ROW_EDIT carrying the row and the value, which the
 * decoder refuses with no scan running; text that would not fit is refused before anything is queued. */
static void
test_scan_row_edit_bridge() {
    dsd_qt::CommandBridge bridge;
    static dsd_opts opts;
    static dsd_state state;
    check(dsd_qt::CommandBridge::scanRowFieldSquelch() == DSD_SCAN_ROW_FIELD_SQUELCH);
    check(dsd_qt::CommandBridge::scanRowFieldWidth() == DSD_SCAN_ROW_FIELD_WIDTH);
    check(dsd_qt::CommandBridge::scanRowFieldTone() == DSD_SCAN_ROW_FIELD_TONE);
    check(dsd_qt::CommandBridge::scanRowFieldGain() == DSD_SCAN_ROW_FIELD_GAIN);
    check(dsd_qt::CommandBridge::scanRowEditSet() == DSD_SCAN_ROW_EDIT_SET);
    check(dsd_qt::CommandBridge::scanRowEditInherit() == DSD_SCAN_ROW_EDIT_INHERIT);
    check(dsd_qt::CommandBridge::scanRowEditReset() == DSD_SCAN_ROW_EDIT_RESET);
    dsd_app_frontend_runtime_start(nullptr, nullptr);
    check(!bridge.scanRowContext().value("active").toBool());
    check(!bridge.editScanRow({{"active", false}}, DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_ROW_EDIT_SET,
                              {{"squelchDb", -50}}));

    /* A target on air in the published pair: the context carries the row and the tone policy the options run, but
       only while both snapshots are of that row's scope; between the decoder's two publishes it is not offered. */
    static dsd_opts published_opts;
    static dsd_state published_state;
    initOpts(&published_opts);
    initState(&published_state);
    published_opts.trunk_scan_enabled = 1;
    published_opts.analog_tone_filter = DSD_TONE_FILTER_ALLOW;
    check(dsd_tone_set_parse("100.0", &published_opts.analog_tone_set, nullptr, 0U) == 0);
    published_state.scan_row_scanner = (uint8_t)DSD_SCAN_ROW_SCANNER_TRUNK_SCAN;
    published_state.scan_row_session = 4U;
    published_state.scan_row_index = 1;
    published_state.scan_row_editable = (uint8_t)(DSD_SCAN_ROW_FIELD_SQUELCH | DSD_SCAN_ROW_FIELD_TONE);
    DSD_SNPRINTF(published_state.trunk_scan_active_id, sizeof published_state.trunk_scan_active_id, "%s", "fire");
    published_opts.scan_row_scope_seq = 11U;
    published_state.scan_row_scope_seq = 11U;
    dsd_app_telemetry_publish_opts_snapshot(&published_opts);
    dsd_app_telemetry_publish_snapshot(&published_state);
    const QVariantMap context = bridge.scanRowContext();
    check(context.value("active").toBool() && context.value("key").toString() == QStringLiteral("1:4:1")
          && context.value("toneMode").toInt() == DSD_TONE_FILTER_ALLOW
          && context.value("toneList").toString() == QStringLiteral("100.0"));
    published_state.scan_row_scope_seq = 12U;
    dsd_app_telemetry_publish_snapshot(&published_state);
    check(!bridge.scanRowContext().value("active").toBool());
    published_state.scan_row_scanner = (uint8_t)DSD_SCAN_ROW_SCANNER_NONE;
    published_state.scan_row_session = 0U;
    published_state.scan_row_index = -1;
    published_opts.trunk_scan_enabled = 0;
    dsd_app_telemetry_publish_opts_snapshot(&published_opts);
    dsd_app_telemetry_publish_snapshot(&published_state);
    freeState(&published_state);
    const QVariantMap row{{"active", true}, {"scanner", 1},           {"session", 3},         {"row", 0},
                          {"mode", 9},      {"target", "county-p25"}, {"label", "county-p25"}};
    check(bridge.editScanRow(row, DSD_SCAN_ROW_FIELD_TONE, DSD_SCAN_ROW_EDIT_SET,
                             {{"toneMode", 1}, {"toneList", "100.0/D023N"}}));
    state.ui_msg[0] = '\0';
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(QString::fromUtf8(state.ui_msg) == QStringLiteral("Refused: no scan is running"));
    check(!bridge.editScanRow(row, DSD_SCAN_ROW_FIELD_TONE, DSD_SCAN_ROW_EDIT_SET,
                              {{"toneMode", 1}, {"toneList", QString(2000, QLatin1Char('1'))}}));
    QVariantMap long_target = row;
    long_target["target"] = QString(100, QLatin1Char('x'));
    check(!bridge.editScanRow(long_target, DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_ROW_EDIT_INHERIT, {}));
    check(dsd_app_drain_cmds(&opts, &state) == 0);
    dsd_app_frontend_runtime_stop();
    freeState(&state);
}

static int g_scan_control_calls = 0;
static int g_scan_control_last_op = 0;

static int
record_scan_control(dsd_opts* opts, dsd_state* state, int op) {
    (void)opts;
    (void)state;
    g_scan_control_calls++;
    g_scan_control_last_op = op;
    return 0;
}

/* Issue #666: the Monitor's Prev and Next go through the real bridge and decoder queue as their own commands. Under a
 * running trunk scan the two reach the coordinator as the step each names, so a bridge that sent Next's id for Prev (or
 * nothing at all) shows here. */
static void
test_scan_step_bridge() {
    dsd_qt::CommandBridge bridge;
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    opts.trunk_scan_enabled = 1;
    DSD_SNPRINTF(state.trunk_scan_active_id, sizeof state.trunk_scan_active_id, "%s", "county");
    dsd_trunk_scan_hooks hooks = {};
    hooks.control = record_scan_control;
    dsd_trunk_scan_hooks_set(&hooks);
    dsd_app_frontend_runtime_start(nullptr, nullptr);
    check(bridge.previousChannel());
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(g_scan_control_calls == 1 && g_scan_control_last_op == DSD_TRUNK_SCAN_CONTROL_RETREAT);
    check(bridge.nextChannel());
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(g_scan_control_calls == 2 && g_scan_control_last_op == DSD_TRUNK_SCAN_CONTROL_ADVANCE);
    dsd_app_frontend_runtime_stop();
    dsd_trunk_scan_hooks_set(nullptr);
    freeState(&state);
}

static int g_unavoid_calls = 0;
static uint32_t g_unavoid_session = 0;
static QString g_unavoid_target;
static QByteArray g_unavoid_bytes;

static int
record_unavoid(const dsd_opts* opts, dsd_state* state, uint32_t session, const char* target_id,
               dsd_trunk_scan_target_name* name) {
    (void)opts;
    (void)state;
    g_unavoid_calls++;
    g_unavoid_session = session;
    g_unavoid_target = QString::fromUtf8(target_id);
    g_unavoid_bytes = QByteArray(target_id);
    if (name) {
        name->ordinal = 2U;
        DSD_SNPRINTF(name->label, sizeof name->label, "%s", "County Fire");
    }
    return DSD_TRUNK_SCAN_CONTROL_OK;
}

/* Issue #672: the put-back payload is parsed from the avoid list's entry, 64-bit fields from decimal strings, and
   anything that does not parse or would reach the decoder cut short is refused with nothing filled in. */
static void
test_scan_avoid_identity_parse() {
    dsd_app_scan_avoid_remove_payload p;
    const QVariantMap lcn{{"kind", DSD_APP_SCAN_AVOID_KIND_LCN},
                          {"instance", "18446744073709551615"},
                          {"listRev", "9007199254740993"},
                          {"frequencyHz", "851012500"},
                          {"row", 3}};
    check(dsd_qt::CommandBridge::scanAvoidIdentity(lcn, &p));
    check(p.kind == DSD_APP_SCAN_AVOID_KIND_LCN && p.instance == 18446744073709551615ULL
          && p.list_rev == 9007199254740993ULL && p.frequency_hz == 851012500ULL && p.row == 3 && p.session == 0U
          && p.target_id[0] == '\0');
    const QVariantMap trunk{
        {"kind", DSD_APP_SCAN_AVOID_KIND_TRUNK_SCAN}, {"session", 4294967295ULL}, {"targetId", "fire"}};
    check(dsd_qt::CommandBridge::scanAvoidIdentity(trunk, &p));
    check(p.kind == DSD_APP_SCAN_AVOID_KIND_TRUNK_SCAN && p.session == 4294967295U && p.row == -1
          && QString::fromUtf8(p.target_id) == QStringLiteral("fire") && p.instance == 0U);

    const auto refused = [](QVariantMap entry, const char* key, const QVariant& value) {
        if (value.isValid()) {
            entry[QLatin1String(key)] = value;
        } else {
            entry.remove(QLatin1String(key));
        }
        dsd_app_scan_avoid_remove_payload out;
        DSD_MEMSET(&out, 0xA5, sizeof out);
        const bool parsed = dsd_qt::CommandBridge::scanAvoidIdentity(entry, &out);
        dsd_app_scan_avoid_remove_payload zero;
        DSD_MEMSET(&zero, 0, sizeof zero);
        return !parsed && std::memcmp(&out, &zero, sizeof out) == 0;
    };
    check(refused(lcn, "kind", QVariant()));
    check(refused(lcn, "kind", DSD_APP_SCAN_AVOID_KIND_NONE));
    check(refused(lcn, "kind", 3));
    check(refused(lcn, "instance", QVariant()));
    check(refused(lcn, "instance", "12x"));
    check(refused(lcn, "instance", "18446744073709551616"));
    check(refused(lcn, "listRev", ""));
    check(refused(lcn, "frequencyHz", "-1"));
    check(refused(lcn, "row", -1));
    check(refused(lcn, "row", "x"));
    check(refused(trunk, "session", 0));
    check(refused(trunk, "session", 4294967296ULL));
    check(refused(trunk, "session", "x"));
    check(refused(trunk, "targetId", ""));
    check(refused(trunk, "targetId", QStringLiteral("fi") + QChar(u'\0') + QStringLiteral("re")));
    check(refused(trunk, "targetId", QString(64, QLatin1Char('x'))));
    check(dsd_qt::CommandBridge::scanAvoidIdentity(
        [&] {
            QVariantMap longest = trunk;
            longest["targetId"] = QString(63, QLatin1Char('x'));
            return longest;
        }(),
        &p));
    check(!dsd_qt::CommandBridge::scanAvoidIdentity(trunk, nullptr));
}

/* Issue #672: Put back goes through the real bridge and decoder queue by the key the Monitor's list shows, which names
   the identity the list holds: one -Y row comes back with no rotation running (B1), a trunk-scan target reaches the
   coordinator's unavoid with its session and id, and a key the list no longer holds sends nothing. */
static void
test_scan_avoid_restore_bridge() {
    dsd_qt::CommandBridge bridge;
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    check(!bridge.restoreScanAvoid(QStringLiteral("lcn:1:1:0:851012500")));
    check(dsd_state_trunk_lcn_reserve(&state, 3U) == 0);
    for (int i = 0; i < 3; ++i) {
        *dsd_state_trunk_lcn_slot(&state, i) = 851012500L + (12500L * i);
    }
    state.lcn_freq_count = 3;
    check(dsd_state_trunk_lcn_avoid_set(&state, 1U, 1) == 0 && dsd_state_trunk_lcn_avoid_set(&state, 2U, 1) == 0);
    dsd_qt::MetricsModel metrics;
    metrics.refresh(&opts, &state);
    dsd_qt::ScanAvoidListModel* list = metrics.scanAvoidModel();
    check(list->rowCount() == 2);
    bridge.setScanAvoids(list);
    const QString key = list->data(list->index(0, 0), dsd_qt::ScanAvoidListModel::KeyRole).toString();
    dsd_app_frontend_runtime_start(nullptr, nullptr);
    check(!bridge.restoreScanAvoid(QStringLiteral("lcn:1:1:0:851012500")));
    check(dsd_app_drain_cmds(&opts, &state) == 0);
    check(bridge.restoreScanAvoid(key));
    state.ui_msg[0] = '\0';
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(!dsd_state_trunk_lcn_avoid_get(&state, 1U) && dsd_state_trunk_lcn_avoid_get(&state, 2U));
    check(QString::fromUtf8(state.ui_msg) == QStringLiteral("Put back row 2 (851.025000 MHz, 1 avoided)"));
    /* Sent twice before the list moves: the second names a row already back, and the decoder says so. */
    check(bridge.restoreScanAvoid(key));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(QString::fromUtf8(state.ui_msg) == QStringLiteral("Already put back: row 2 (851.025000 MHz)"));
    metrics.refresh(&opts, &state);
    check(!bridge.restoreScanAvoid(key));
    check(dsd_app_drain_cmds(&opts, &state) == 0);

    /* A trunk scan's target, paused or not, reaches the coordinator by its session and id. */
    dsd_trunk_scan_hooks hooks = {};
    hooks.unavoid = record_unavoid;
    dsd_trunk_scan_hooks_set(&hooks);
    opts.trunk_scan_enabled = 1;
    dsd_trunk_scan_avoid_entry target{};
    target.ordinal = 2U;
    target.frequency_hz = 851012500ULL;
    DSD_SNPRINTF(target.id, sizeof target.id, "%s", "county-p25");
    check(dsd_state_trunk_scan_avoids_set(&state, &target, 1U) == 0);
    state.trunk_scan_session = 9U;
    state.trunk_scan_avoid_rev = dsd_state_rev_next();
    metrics.refresh(&opts, &state);
    check(list->rowCount() == 1);
    check(bridge.restoreScanAvoid(QStringLiteral("trunk:9:county-p25")));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(g_unavoid_calls == 1 && g_unavoid_session == 9U && g_unavoid_target == QStringLiteral("county-p25"));
    /* The Monitor shows the decoder's answer: the target as the sheet lists it, never its entry UID. */
    check(QString::fromUtf8(state.ui_msg) == QStringLiteral("Put back target #2 County Fire (0 avoided)"));

    /* Target ids are UTF-8 (the target list loader refuses any other), so the list's key and the payload carry each
       one byte for byte: look-alike ids -- 2- and 4-byte characters, quotes, a backslash, a control -- each put back
       their own target. */
    static const char* const k_ids[] = {
        "caf\xC3\xA9 \xF0\x9F\x93\xA1",
        "caf\xC3\xA9 \xF0\x9F\x93\xA1 \"q\"\\\x01",
        "cafe \xF0\x9F\x93\xA1",
    };
    dsd_trunk_scan_avoid_entry lookalikes[3]{};
    for (int i = 0; i < 3; ++i) {
        lookalikes[i].ordinal = static_cast<uint32_t>(i + 1);
        lookalikes[i].frequency_hz = 851012500ULL + (12500ULL * static_cast<uint64_t>(i));
        DSD_SNPRINTF(lookalikes[i].id, sizeof lookalikes[i].id, "%s", k_ids[i]);
    }
    check(dsd_state_trunk_scan_avoids_set(&state, lookalikes, 3U) == 0);
    state.trunk_scan_avoid_rev = dsd_state_rev_next();
    metrics.refresh(&opts, &state);
    check(list->rowCount() == 3);
    for (int i = 0; i < 3; ++i) {
        const QString row = list->data(list->index(i, 0), dsd_qt::ScanAvoidListModel::KeyRole).toString();
        g_unavoid_bytes.clear();
        check(bridge.restoreScanAvoid(row));
        check(dsd_app_drain_cmds(&opts, &state) == 1);
        check(g_unavoid_session == 9U && g_unavoid_bytes == QByteArray(k_ids[i]));
    }
    dsd_app_frontend_runtime_stop();
    dsd_trunk_scan_hooks_set(nullptr);
    bridge.setScanAvoids(nullptr);
    freeState(&state);
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
        entry.kind = end != 0 ? DSD_TG_POLICY_ROW_RANGE : DSD_TG_POLICY_ROW_EXACT;
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

/* B14: the bridge names a listed row by its token, so a duplicate, a zero-start range and a pattern row are edited,
 * toggled, selected and removed as listed, and a stale Listening toggle changes nothing. */
static void
test_bridge_row_tokens() {
    dsd_qt::CommandBridge bridge;
    static dsd_opts opts;
    static dsd_state state;
    dsd_app_frontend_runtime_start(nullptr, nullptr);
    dsd_tg_policy_entry entry = {};
    for (const char* name : {"First", "Second"}) {
        check(dsd_tg_policy_make_exact_entry(0, "A", name, DSD_TG_POLICY_SOURCE_IMPORTED, &entry) == 0);
        entry.id_end = 99;
        entry.kind = DSD_TG_POLICY_ROW_RANGE;
        check(dsd_tg_policy_add_range_entry(&state, &entry) == 0);
    }
    check(dsd_tg_policy_make_exact_entry(0, "A", "Twelves", DSD_TG_POLICY_SOURCE_IMPORTED, &entry) == 0);
    entry.kind = DSD_TG_POLICY_ROW_PATTERN;
    DSD_SNPRINTF(entry.pattern, sizeof entry.pattern, "%s", "12*");
    check(dsd_tg_policy_add_pattern_entry(&state, &entry) == 0);
    uint64_t context = 0;
    unsigned int generation = 0;
    QString version;
    // Re-capture before each edit (arguments are evaluated in no fixed order, so never inside the call).
    const auto capture = [&]() {
        dsd_tg_policy_table_version(&state, &context, &generation);
        version = QString::number(context);
    };
    const auto name = [&](size_t index) {
        dsd_tg_policy_entry row = {};
        return dsd_tg_policy_entry_at(&state, index, &row) ? QString::fromUtf8(row.name) : QString();
    };
    const auto mode = [&](size_t index) {
        dsd_tg_policy_entry row = {};
        return dsd_tg_policy_entry_at(&state, index, &row) ? QString::fromUtf8(row.mode) : QString();
    };

    capture();
    check(bridge.renameTalkgroup(0, 99, version, generation, "Edited", 1));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(name(0) == "First" && name(1) == "Edited");
    capture();
    check(bridge.setTalkgroupPolicy(0, 0, version, generation, {{"name", "Twelve rule"}}, 2, "12*"));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(name(2) == "Twelve rule");
    capture();
    check(!bridge.setTalkgroupPolicy(0, 0, version, generation, {{"name", "No index"}}, -1, "12*"));
    capture();
    check(!bridge.renameTalkgroup(12, 12, version, generation, "Bounds", 2, "12*"));

    capture();
    check(bridge.setTalkgroupListening(0, 99, false, version, generation, 1));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(mode(0) == "A" && mode(1) == "B");
    // The same captured version again: the first toggle moved the generation, so this one is stale.
    check(bridge.setTalkgroupListening(0, 0, false, version, generation, 2, "12*"));
    (void)dsd_app_drain_cmds(&opts, &state);
    check(mode(2) == "A");
    check(QString::fromUtf8(state.ui_msg).contains("stale", Qt::CaseInsensitive));
    capture();
    check(bridge.setTalkgroupListening(0, 0, false, version, generation, 2, "12*"));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(mode(2) == "B");
    check(!bridge.setTalkgroupListening(0, 99, true, QString(), 0, 1));

    const QVariantList rows = {QVariantMap{{"index", 0}, {"first", 0U}, {"last", 99U}, {"pattern", QString()}},
                               QVariantMap{{"index", 2}, {"first", 0U}, {"last", 0U}, {"pattern", "12*"}}};
    capture();
    check(bridge.setTalkgroupSelection(false, version, generation, rows));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(mode(0) == "B" && mode(1) == "B" && mode(2) == "B");
    capture();
    check(bridge.setTalkgroupSelection(true, version, generation, rows));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(mode(0) == "A" && mode(1) == "B" && mode(2) == "A");
    capture();
    check(!bridge.setTalkgroupSelection(true, version, generation,
                                        {QVariantMap{{"index", -1}, {"first", 0U}, {"last", 0U}}}));

    capture();
    check(bridge.removeTalkgroup(0, 0, version, generation, 2, "12*"));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(dsd_tg_policy_entry_count(&state) == 2 && dsd_tg_policy_pattern_count(&state) == 0);
    capture();
    check(bridge.removeTalkgroup(0, 99, version, generation, 1));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(dsd_tg_policy_entry_count(&state) == 1 && name(0) == "First");
    bridge.clearSessionInputs();
    dsd_app_frontend_runtime_stop();
    dsd_state_ext_free_all(&state);
}

/* A saved direct profile whose key was typed byte-spaced applies live through the real DECRYPTION_APPLY queue: its 95
   characters do not fit the 72-byte command field, the 64 digits configuration() hands the bridge do. */
static void
test_spaced_direct_profile_applies_live() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_app_frontend_runtime_start(&opts, &state);
    dsd_qt::DecryptionProfilesModel profiles;
    QString spaced;
    for (int i = 0; i < 32; ++i) {
        spaced += i ? QStringLiteral(" 1a") : QStringLiteral("1a");
    }
    check(spaced.size() == 95);
    check(profiles
              .saveProfile({{"uid", "live-direct"},
                            {"label", "Live direct"},
                            {"protocol", "p25"},
                            {"mode", "direct"},
                            {"directType", "hex"},
                            {"directValue", spaced}})
              .value("ok")
              .toBool());
    dsd_qt::CommandBridge bridge;
    bridge.setDecryptionProfiles(&profiles);
    const QVariantMap context = bridge.decryptionContext(QString(), QStringLiteral("0"));
    check(!bridge.applyDecryptionProfile("live-direct", DSD_APP_KEY_SCOPE_DEFAULTS, context).isEmpty());
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    dsd_app_decryption_result result = {};
    check(dsd_app_decryption_result_get(&result) && result.status == DSD_APP_KEY_APPLIED);
    check(state.K1 == 0x1A1A1A1A1A1A1A1AULL && state.K2 == state.K1 && state.K3 == state.K1 && state.K4 == state.K1);
    check(state.aes_key_segments[0] == 4 && state.aes_key_loaded[0]);
    dsd_app_frontend_runtime_stop();
    freeState(&state);
}

// A full log's save can wait a minute, and a stop often comes just before the app goes away. The
// tick that sees the session end saves the history, after it ingests the session's last rows: the
// host publishes Idle or Failed inside its refresh, before this tick reads the snapshot. With or
// without a redraw, the stored log holds the final row as soon as the save worker is done.
static void
test_session_end_saves_after_final_ingest() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::CallHistoryModel history;
    Host host;
    dsd_qt::UiController controller(&host, nullptr, &history, nullptr);
    controller.setPollIntervalMs(50);
    const auto stored = [](unsigned long long tg) {
        for (const auto& value : dsd_qt::json_store_load_array("call_history.json")) {
            if (value.toObject().value("tg").toVariant().toULongLong() == tg) {
                return true;
            }
        }
        return false;
    };
    unsigned int tg = 9201;
    for (const bool redraw : {true, false}) {
        for (const auto phase : {Host::Idle, Host::Failed}) {
            host.setPhase(Host::Running);
            const int before = history.count();
            const unsigned int finalTg = tg++;
            host.onRefresh = [&]() {
                if (host.phase != Host::Running) {
                    return;
                }
                auto& ring = state.event_history_s[0];
                auto& item = ring.Event_History_Items[1];
                item.category = DSD_EVENT_CATEGORY_VOICE;
                item.target_id = finalTg;
                item.source_id = 8101;
                item.event_start_time = 1754600000 + static_cast<time_t>(finalTg) * 10;
                item.event_time = item.event_start_time + 4;
                ++ring.push_seq;
                ++ring.commit_rev;
                ++ring.revision;
                dsd_app_telemetry_publish_opts_snapshot(&opts);
                dsd_app_telemetry_publish_snapshot(&state);
                if (redraw) {
                    dsd_app_request_redraw();
                }
                host.setPhase(phase);
            };
            (void)dsd_app_frontend_redraw_consume();
            QEventLoop loop;
            QTimer::singleShot(65, &loop, &QEventLoop::quit);
            controller.start();
            loop.exec();
            controller.stop();
            host.onRefresh = {};
            history.waitForSaveForTest();
            check(history.count() == before + 1);
            check(stored(finalTg));
        }
    }
    freeState(&state);
}
} // namespace

int
main(int argc, char** argv) {
    dsd_qt::applyQmlGcPolicy();
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName("dsd-neo-test");
    QCoreApplication::setApplicationName("dsd-neo-controller");
    dsd_test_qt_isolate_paths();
    test_history_receives_effective_scan_options();
    test_history_handoff_after_resume_without_redraw();
    test_session_end_saves_after_final_ingest();
    test_startup_options_wait_for_redraw();
    test_metrics_age_without_decoder_redraw();
    test_target_change_publishes_once();
    test_target_change_drops_sync_hold();
    test_target_change_while_stopped_clears();
    test_sheet_policy_edits();
    test_nfm_bandwidth_bridge();
    test_am_bandwidth_bridge();
    test_reload_bridge();
    test_tone_filter_bridge();
    test_scan_row_edit_bridge();
    test_scan_step_bridge();
    test_scan_avoid_identity_parse();
    test_scan_avoid_restore_bridge();
    test_zero_bounds();
    test_bridge_row_tokens();
    test_auto_start_requests();
    test_auto_start_follows_the_attached_kind();
    test_auto_start_carries_the_scan_bundle();
    test_scan_bundle_prefs_and_uid_repair();
    test_scan_list_uid_repair_persists();
    test_scan_list_entry_uid_repair_at_load();
    test_scan_list_entry_uid_repair_on_save();
    test_scan_lists_start_together();
    test_scan_lists_share_a_csv_target();
    test_attachment_kind();
    test_initial_usb_record();
    test_usb_request_record();
    test_usb_refusal_record();
    test_spaced_direct_profile_applies_live();
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
    // A running trunk scan owns the tuner across the hop below (issue #671).
    opts.trunk_scan_enabled = 1;
    state.trunk_scan_target_count = 2;
    tick();
    check(!metrics.syncLabel().isEmpty());
    check(metrics.scanRotationActive() && metrics.tunerControlled());
    int ownershipDrops = 0;
    QObject::connect(&metrics, &dsd_qt::MetricsModel::controlChanged, [&]() {
        if (!metrics.scanRotationActive() || !metrics.tunerControlled()) {
            ++ownershipDrops;
        }
    });
    check(talkgroups.count() == 1);
    QObject::connect(&talkgroups, &QAbstractItemModel::modelReset, [&]() { ++resets; });
    check(network.radios().size() == 1);
    network.setActive(false); // Clearing must also reach closed sheets.
    state.synctype = DSD_SYNC_NONE;
    state.lastsynctype = DSD_SYNC_NONE;
    state.trunk_scan_active_ordinal = 2;
    tick();
    check(resets == 0);         // A scan hop must preserve the list view.
    check(ownershipDrops == 0); // A hop never reads as the scan letting go of the tuner (issue #671).
    opts.trunk_scan_enabled = 0;
    state.trunk_scan_target_count = 0;
    check(talkgroups.count() == 1);
    check(metrics.syncLabel().isEmpty()); // Old target's sync hold must not survive.
    check(network.radios().isEmpty());
    network.setActive(true);
    {
        // Issue #782: with the Network sheet open, a hop publishes the new target's rows in one replacement, never
        // passing through an empty list: the sheet then keeps the rows both targets list rather than rebuilding
        // every one. The old target's rows still do not survive into the new one.
        QList<int> published;
        QMetaObject::Connection spy = QObject::connect(&network, &dsd_qt::P25NetworkModel::radiosChanged,
                                                       [&]() { published.append(network.radios().size()); });
        state.p25_aff_rid[1] = 456;
        tick();
        check(published == QList<int>{2});
        published.clear();
        state.trunk_scan_active_ordinal = 3;
        tick();
        check(published.isEmpty()); // The same rows on the next target: nothing to publish.
        check(network.radios().size() == 2);
        state.p25_aff_rid[0] = 0;
        state.trunk_scan_active_ordinal = 4;
        tick();
        check(published == QList<int>{1});
        check(network.radios().size() == 1 && network.radios().at(0).toMap().value("rid").toUInt() == 456U);
        published.clear();
        state.p25_aff_rid[1] = 0;
        state.trunk_scan_active_ordinal = 5;
        tick();
        check(published == QList<int>{0}); // A target with no rows clears them, once.
        QObject::disconnect(spy);
        state.p25_aff_rid[0] = 123;
    }
    diagnostics.submit("host", "info", "before next session");
    for (auto phase : {Host::Starting, Host::Idle, Host::Failed}) {
        host.setPhase(Host::Running);
        state.synctype = DSD_SYNC_P25P1_POS;
        dsd_rxid_note_access(&state, DSD_RXID_FAMILY_P25, static_cast<uint8_t>(DSD_ACCESS_CODE_NAC), 0x293U);
        state.p25_p1_fec_ok = 3;
        state.p25_p1_fec_err = 1;
        tick();
        check(metrics.p25NacValid() && !metrics.siteLine().isEmpty());
        check(!metrics.carrierIdentityTokens().isEmpty() && metrics.carrierIdentityLive());
        check(metrics.qualityValid() && metrics.ccFecOkPct() == 75.0);
        check(!metrics.syncLabel().isEmpty());
        check(network.radios().size() == 1);
        host.setPhase(phase);
        check(network.radios().isEmpty());
        tick(); // The last engine snapshot is still published after stop.
        check(metrics.syncLabel().isEmpty());
        check(metrics.siteLine().isEmpty() && !metrics.p25NacValid() && !metrics.siteConfirmed());
        check(metrics.carrierIdentityTokens().isEmpty() && metrics.carrierIdentityProtocol().isEmpty()
              && metrics.carrierIdentityText().isEmpty() && !metrics.carrierIdentityLive());
        check(!metrics.qualityValid() && !metrics.ccFecValid());
        check(diagnosticsModel.allText().contains("before next session"));
        check(diagnostics.snapshot().join('\n').count("--- session starting ---") == 1
              && diagnostics.hostSnapshot().join('\n').count("--- session starting ---") == 1);
        check(network.radios().isEmpty());
        host.setPhase(phase); // Repeated state notification is not another boundary.
        check(diagnostics.snapshot().join('\n').count("--- session starting ---") == 1
              && diagnostics.hostSnapshot().join('\n').count("--- session starting ---") == 1);
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
    check(diagnostics.snapshot().join('\n').count("--- session starting ---") == 2
          && diagnostics.hostSnapshot().join('\n').count("--- session starting ---") == 2);
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
    check(bridge.setPersistTgLockouts(false));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(opts.persist_tg_lockouts == 0);
    // Settings' Relaxed CRC switch sets the value through the queue, as QML calls it: a repeat leaves it as set.
    for (const bool on : {true, true, false, false}) {
        bool queued = false;
        check(QMetaObject::invokeMethod(&bridge, "setRelaxedCrc", Q_RETURN_ARG(bool, queued), Q_ARG(bool, on)));
        check(queued);
        check(dsd_app_drain_cmds(&opts, &state) == 1);
        check(dsd_opts_relaxed_crc(&opts) == (on ? 1 : 0));
    }
    dsd_call_observation call = {};
    call.protocol = DSD_SYNC_P25P1_POS;
    call.slot = 0U;
    call.kind = DSD_CALL_KIND_GROUP_VOICE;
    call.ota_target_id = 42;
    call.policy_target_id = 42;
    call.ota_source_id = 1;
    call.observed_m = dsd_decode_now_mono_s();
    check(dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    check(!dsd_tg_policy_call_skip_active(&state, 42, dsd_decode_now_mono_s()));
    check(bridge.skipSlot(0));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(dsd_tg_policy_call_skip_active(&state, 42, dsd_decode_now_mono_s()));
    check(dsd_tg_policy_session_avoid_add(&state, 42) == 0);
    check(!bridge.clearTemporaryTgAvoids("invalid context"));
    check(bridge.clearTemporaryTgAvoids(QString::number(context + 1)));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(dsd_tg_policy_session_avoid_contains(&state, 42));
    check(bridge.clearTemporaryTgAvoids(QString::number(context)));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(!dsd_tg_policy_session_avoid_contains(&state, 42));
    check(!dsd_tg_policy_call_skip_active(&state, 42, dsd_decode_now_mono_s()));
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
    // Issue #708: Record goes with Listening in one edit, applied after it; it takes a bool only.
    refreshVersion();
    check(!bridge.setTalkgroupPolicy(42, 42, version, generation, {{"record", 0}}));
    check(bridge.setTalkgroupPolicy(42, 42, version, generation, {{"listening", true}, {"record", false}}));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(dsd_tg_policy_entry_at(&state, 0, &entry));
    check(QString::fromUtf8(entry.mode) == "A" && entry.audio == 1 && entry.record == 0);
    refreshVersion();
    check(bridge.setTalkgroupPolicy(42, 42, version, generation, {{"record", true}}));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(dsd_tg_policy_entry_at(&state, 0, &entry));
    check(entry.record == 1 && entry.priority == 50 && QString::fromUtf8(entry.name) == "Renamed");
    refreshVersion();
    check(bridge.addTalkgroup(55, 55, version, generation, "Heard", false, 25, false));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(dsd_tg_policy_entry_count(&state) == 2);
    // Issue #708: a row added listening takes the editor's Record with it; a default add records.
    refreshVersion();
    check(bridge.addTalkgroup(56, 56, version, generation, "Quiet", true, 0, false, false));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(dsd_tg_policy_entry_count(&state) == 3 && dsd_tg_policy_entry_at(&state, 2, &entry));
    check(entry.id_start == 56 && QString::fromUtf8(entry.mode) == "A" && entry.record == 0);
    refreshVersion();
    check(bridge.removeTalkgroup(56, 56, version, generation));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    refreshVersion();
    check(bridge.addTalkgroup(57, 57, version, generation, "Kept", true, 0, false));
    check(dsd_app_drain_cmds(&opts, &state) == 1);
    check(dsd_tg_policy_entry_count(&state) == 3 && dsd_tg_policy_entry_at(&state, 2, &entry));
    check(entry.id_start == 57 && entry.record == 1);
    refreshVersion();
    check(bridge.removeTalkgroup(57, 57, version, generation));
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
        Q_EMIT host.localDeviceAttached(QStringLiteral("usb:RTL-SDR"));
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
