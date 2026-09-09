// SPDX-License-Identifier: GPL-3.0-or-later
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <cassert>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include "command_bridge.h"
#include "commands_internal.h"
#include "decoder_host.h"
#include "diagnostics_log.h"
#include "metrics_model.h"
#include "snapshot_internal.h"
#include "ui_controller.h"

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

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    Host host;
    dsd_qt::MetricsModel metrics;
    dsd_qt::UiController controller(&host, &metrics, nullptr, nullptr);
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
    state.synctype = DSD_SYNC_P25P1_POS;
    state.trunk_scan_active_ordinal = 1;
    tick();
    assert(!metrics.syncLabel().isEmpty());
    state.synctype = DSD_SYNC_NONE;
    state.lastsynctype = DSD_SYNC_NONE;
    state.trunk_scan_active_ordinal = 2;
    tick();
    assert(metrics.syncLabel().isEmpty()); // Old target's sync hold must not survive.
    diagnostics.submit("host", "info", "before next session");
    for (auto phase : {Host::Starting, Host::Idle, Host::Failed}) {
        host.setPhase(Host::Running);
        state.synctype = DSD_SYNC_P25P1_POS;
        state.p25_p1_fec_ok = 3;
        state.p25_p1_fec_err = 1;
        tick();
        assert(metrics.qualityValid() && metrics.ccFecOkPct() == 75.0);
        assert(!metrics.syncLabel().isEmpty());
        host.setPhase(phase);
        tick(); // The last engine snapshot is still published after stop.
        assert(metrics.syncLabel().isEmpty());
        assert(!metrics.qualityValid() && !metrics.ccFecValid());
        assert(diagnosticsModel.allText().contains("before next session"));
        assert(diagnosticsModel.allText().count("--- session starting ---") == 1);
        host.setPhase(phase); // Repeated state notification is not another boundary.
        assert(diagnosticsModel.allText().count("--- session starting ---") == 1);
    }
    // WP-F5: an idle decoder need not request redraw for process logs to refresh.
    (void)dsd_app_frontend_redraw_consume();
    diagnostics.submit("host", "info", "idle diagnostic");
    QEventLoop idleLoop;
    QTimer::singleShot(65, &idleLoop, &QEventLoop::quit);
    controller.start();
    idleLoop.exec();
    controller.stop();
    assert(diagnosticsModel.rowCount() == 3);
    host.setPhase(Host::Starting);
    diagnosticsModel.refresh();
    assert(diagnosticsModel.allText().count("--- session starting ---") == 2);
    assert(diagnosticsModel.allText().contains("idle diagnostic"));
    // WP-D1: real bridge -> queue -> policy, then retained completion with no redraw.
    dsd_qt::CommandBridge bridge;
    // A session without a policy table reports version 0/0; adding its first heard row is valid.
    static dsd_state emptyPolicyState;
    assert(bridge.addTalkgroup(77, 77, QStringLiteral("0"), 0, "First heard", true, 0, false));
    assert(dsd_app_drain_cmds(&opts, &emptyPolicyState) == 1);
    assert(dsd_tg_policy_entry_count(&emptyPolicyState) == 1);
    dsd_state_ext_free_all(&emptyPolicyState);
    dsd_tg_policy_entry entry = {};
    assert(dsd_tg_policy_make_exact_entry(42, "A", "Dispatch", DSD_TG_POLICY_SOURCE_IMPORTED, &entry) == 0);
    assert(dsd_tg_policy_append_exact(&state, &entry) == 0);
    uint64_t context = 0;
    unsigned int generation = 0;
    dsd_tg_policy_table_version(&state, &context, &generation);
    const QString version = QString::number(context);
    assert(!bridge.renameTalkgroup(42, 42, version, generation, QString(50, QLatin1Char('x'))));
    assert(!bridge.setTalkgroupPolicy(42, 42, version, generation, "Dispatch", true, 101, true));
    assert(bridge.setTalkgroupPolicy(42, 42, version, generation, "Fire", true, 50, true));
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
    freeState(&state);
    return 0;
}
