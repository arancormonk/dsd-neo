// SPDX-License-Identifier: GPL-3.0-or-later
#include <QAbstractListModel>
#include <QChar>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
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
#include <QVariantMap>
#include <Qt>
#include <cstdio>
#include <cstdlib>
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
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/scan_row_edit.h>
#include <functional>
#include <initializer_list> // IWYU pragma: keep -- braced range-for lists require this header.
#include <memory>
#include <stdint.h>
#include <time.h>
#include <utility>
#include "../../android/local_device_state.h"
#include "../../src/app_control/commands_internal.h"
#include "../../src/app_control/snapshot_internal.h"
#include "../test_support/qt_test_paths.h"
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
    test_sheet_policy_edits();
    test_nfm_bandwidth_bridge();
    test_am_bandwidth_bridge();
    test_tone_filter_bridge();
    test_scan_row_edit_bridge();
    test_zero_bounds();
    test_bridge_row_tokens();
    test_auto_start_requests();
    test_auto_start_follows_the_attached_kind();
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
