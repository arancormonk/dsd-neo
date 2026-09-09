// SPDX-License-Identifier: GPL-3.0-or-later
#include <QAbstractListModel>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
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
#include <cassert>
#include <cstdlib>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <initializer_list>
#include <memory>
#include "../../src/app_control/snapshot_internal.h"
#include "command_bridge.h"
#include "commands_internal.h"
#include "decoder_host.h"
#include "diagnostics_log.h"
#include "metrics_model.h"
#include "p25_network_model.h"
#include "snapshot_internal.h"
#include "talkgroup_list_model.h"
#include "ui_controller.h"

namespace {
void
check(bool passed) {
    if (!passed) {
        std::abort();
    }
}

class Host : public dsd_qt::DecoderHost {
  public:
    SessionState phase = Running;

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

// Run the actual sheet against the real bridge and decoder queue, including CSV media overrides.
static void
test_sheet_policy_edits() {
    dsd_qt::CommandBridge bridge;
    QQmlEngine engine;
    engine.rootContext()->setContextProperty("commands", &bridge);
    engine.rootContext()->setContextProperty("metrics", QVariantMap{{"uiMessage", QString()}});
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(DSD_QML_UI_DIR "/TalkgroupEditSheet.qml")));
    std::unique_ptr<QObject> sheet(component.create());
    assert(sheet);
    QObject* name = sheet->findChild<QObject*>("talkgroupName");
    QObject* listen = sheet->findChild<QObject*>("listeningToggle");
    assert(name && listen);
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
        assert(dsd_tg_policy_make_exact_entry(42, mode, "Dispatch", DSD_TG_POLICY_SOURCE_IMPORTED, &entry) == 0);
        entry.priority = 25;
        entry.audio = media.audio;
        entry.record = media.record;
        entry.stream = media.stream;
        assert(dsd_tg_policy_append_exact(&state, &entry) == 0);
        assert(dsd_tg_policy_entry_at(&state, 0, &entry));
        assert(entry.audio == media.audio && entry.record == media.record && entry.stream == media.stream);
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
            assert(!expression.hasError());
        };
        open();
        name->setProperty("text", "Renamed");
        assert(QMetaObject::invokeMethod(sheet.get(), "saveRow"));
        assert(dsd_app_drain_cmds(&opts, &state) == 1);
        assert(dsd_tg_policy_entry_at(&state, 0, &entry));
        assert(QString::fromUtf8(entry.name) == "Renamed");
        assert(QString::fromUtf8(entry.mode) == mode && entry.audio == media.audio && entry.record == media.record
               && entry.stream == media.stream);
        open();
        sheet->setProperty("priorityValue", 50);
        assert(QMetaObject::invokeMethod(sheet.get(), "saveRow"));
        assert(dsd_app_drain_cmds(&opts, &state) == 1);
        assert(dsd_tg_policy_entry_at(&state, 0, &entry));
        assert(entry.priority == 50 && QString::fromUtf8(entry.name) == "Renamed");
        assert(QString::fromUtf8(entry.mode) == mode && entry.audio == media.audio && entry.record == media.record
               && entry.stream == media.stream);
        open();
        name->setProperty("text", "Combined");
        sheet->setProperty("priorityValue", 100);
        assert(QMetaObject::invokeMethod(sheet.get(), "saveRow"));
        assert(dsd_app_drain_cmds(&opts, &state) == 1); // Name + priority must be one atomic edit.
        assert(dsd_tg_policy_entry_at(&state, 0, &entry));
        assert(entry.priority == 100 && QString::fromUtf8(entry.name) == "Combined");
        assert(QString::fromUtf8(entry.mode) == mode && entry.audio == media.audio && entry.record == media.record
               && entry.stream == media.stream);
        open();
        assert(QMetaObject::invokeMethod(sheet.get(), "saveRow"));
        assert(dsd_app_drain_cmds(&opts, &state) == 0); // Unchanged form submits nothing.
        listen->setProperty("checked", QString::fromUtf8(mode) != "A");
        assert(QMetaObject::invokeMethod(sheet.get(), "saveRow"));
        assert(dsd_app_drain_cmds(&opts, &state) == 1);
        assert(dsd_tg_policy_entry_at(&state, 0, &entry));
        assert(QString::fromUtf8(entry.mode) == (QString::fromUtf8(mode) == "A" ? "B" : "A"));
        dsd_app_frontend_runtime_stop();
        dsd_state_ext_free_all(&state);
    }
}

static void
test_zero_bounds() {
    dsd_qt::CommandBridge bridge;
    static dsd_opts opts;
    static dsd_state state;
    for (unsigned int end : {0U, 999U}) {
        dsd_app_frontend_runtime_start(nullptr, nullptr);
        dsd_tg_policy_entry entry = {};
        assert(dsd_tg_policy_make_exact_entry(0, "A", "Zero", DSD_TG_POLICY_SOURCE_IMPORTED, &entry) == 0);
        entry.id_end = end;
        entry.is_range = end != 0;
        assert((end ? dsd_tg_policy_add_range_entry(&state, &entry) : dsd_tg_policy_append_exact(&state, &entry)) == 0);
        uint64_t context = 0;
        unsigned int generation = 0;
        dsd_tg_policy_table_version(&state, &context, &generation);
        assert(bridge.renameTalkgroup(0, end, QString::number(context), generation, "Edited zero"));
        assert(dsd_app_drain_cmds(&opts, &state) == 1);
        assert(dsd_tg_policy_entry_at(&state, 0, &entry));
        assert(entry.id_start == 0 && entry.id_end == end && QString::fromUtf8(entry.name) == "Edited zero");
        dsd_tg_policy_table_version(&state, &context, &generation);
        assert(bridge.setTalkgroupPolicy(0, end, QString::number(context), generation, {{"priority", 50}}));
        assert(dsd_app_drain_cmds(&opts, &state) == 1);
        assert(dsd_tg_policy_entry_at(&state, 0, &entry));
        assert(entry.id_start == 0 && entry.id_end == end && entry.priority == 50);
        dsd_tg_policy_table_version(&state, &context, &generation);
        assert(bridge.removeTalkgroup(0, end, QString::number(context), generation));
        assert(dsd_app_drain_cmds(&opts, &state) == 1);
        assert(dsd_tg_policy_entry_count(&state) == 0);
        dsd_app_frontend_runtime_stop();
        dsd_state_ext_free_all(&state);
    }
}
} // namespace

int
main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    test_sheet_policy_edits();
    test_zero_bounds();
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
    assert(bridge.addTalkgroup(77, 77, QStringLiteral("0"), 0, "First heard", true, 0, false));
    assert(dsd_app_drain_cmds(&opts, &emptyPolicyState) == 1);
    assert(dsd_tg_policy_entry_count(&emptyPolicyState) == 1);
    dsd_state_ext_free_all(&emptyPolicyState);
    // The lifecycle case above owns a separate policy fixture.
    assert(dsd_tg_policy_clear(&state) == 0);
    entry = {};
    assert(dsd_tg_policy_make_exact_entry(42, "A", "Dispatch", DSD_TG_POLICY_SOURCE_IMPORTED, &entry) == 0);
    assert(dsd_tg_policy_append_exact(&state, &entry) == 0);
    uint64_t context = 0;
    unsigned int generation = 0;
    dsd_tg_policy_table_version(&state, &context, &generation);
    const QString version = QString::number(context);
    assert(!bridge.renameTalkgroup(42, 42, version, generation, QString(50, QLatin1Char('x'))));
    assert(!bridge.setTalkgroupPolicy(42, 42, version, generation, {{"priority", 101}}));
    assert(bridge.setTalkgroupPolicy(42, 42, version, generation,
                                     {{"name", "Fire"}, {"listening", true}, {"priority", 50}, {"preempt", true}}));
    assert(dsd_app_drain_cmds(&opts, &state) == 1);
    assert(dsd_tg_policy_entry_at(&state, 0, &entry));
    assert(entry.priority == 50 && entry.preempt == 1 && QString::fromUtf8(entry.name) == "Fire");
    // The original sheet's generation must be forwarded unchanged and refused.
    assert(bridge.renameTalkgroup(42, 42, version, generation, "Stale"));
    (void)dsd_app_drain_cmds(&opts, &state);
    assert(dsd_tg_policy_entry_at(&state, 0, &entry));
    assert(QString::fromUtf8(entry.name) == "Fire");
    assert(QString::fromUtf8(state.ui_msg).contains("stale", Qt::CaseInsensitive));
    dsd_tg_policy_table_version(&state, &context, &generation);
    assert(bridge.renameTalkgroup(42, 42, version, generation, "Renamed"));
    assert(dsd_app_drain_cmds(&opts, &state) == 1);
    assert(dsd_tg_policy_entry_at(&state, 0, &entry));
    assert(entry.priority == 50 && entry.preempt == 1 && QString::fromUtf8(entry.name) == "Renamed");
    dsd_tg_policy_table_version(&state, &context, &generation);
    assert(bridge.addTalkgroup(55, 55, version, generation, "Heard", false, 25, false));
    assert(dsd_app_drain_cmds(&opts, &state) == 1);
    assert(dsd_tg_policy_entry_count(&state) == 2);
    dsd_tg_policy_table_version(&state, &context, &generation);
    assert(bridge.removeTalkgroup(55, 55, version, generation));
    assert(dsd_app_drain_cmds(&opts, &state) == 1);
    assert(dsd_tg_policy_entry_count(&state) == 1);
    dsd_tg_policy_table_version(&state, &context, &generation);
    QTemporaryDir exportDir;
    assert(exportDir.isValid());
    const QString exportPath = exportDir.filePath("groups.csv");
    assert(bridge.saveTalkgroupList(version, generation, exportPath));
    assert(!QFile::exists(exportPath));
    assert(controller.talkgroupExportResult().isEmpty());
    assert(dsd_app_drain_cmds(&opts, &state) == 1);
    assert(QFile::exists(exportPath));
    (void)dsd_app_frontend_redraw_consume();
    QEventLoop exportLoop;
    QTimer::singleShot(65, &exportLoop, &QEventLoop::quit);
    controller.start();
    exportLoop.exec();
    controller.stop();
    const auto exported = controller.talkgroupExportResult();
    assert(exported.value("success").toBool());
    assert(exported.value("path").toString() == exportPath);
    assert(exported.value("policyContext").toString() == version);
    assert(exported.value("policyGeneration").toUInt() == generation);
    assert(exported.value("sequence").toString() != "0");
    assert(QString::fromUtf8(opts.group_in_file) == exportPath);
    dsd_app_frontend_runtime_stop();
    check(!bridge.renameTalkgroup(42, 42, version, generation, "After stop"));
    check(dsd_app_drain_cmds(&opts, &state) == 0);
    freeState(&state);
    return 0;
}
