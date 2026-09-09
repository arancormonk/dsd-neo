// SPDX-License-Identifier: GPL-3.0-or-later
#include <QAbstractListModel>
#include <QCoreApplication>
#include <QEventLoop>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdlib>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <initializer_list>
#include "../../src/app_control/snapshot_internal.h"
#include "decoder_host.h"
#include "diagnostics_log.h"
#include "metrics_model.h"
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

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    Host host;
    dsd_qt::MetricsModel metrics;
    dsd_qt::TalkgroupListModel talkgroups(nullptr);
    dsd_qt::UiController controller(&host, &metrics, nullptr, &talkgroups);
    int resets = 0;
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
    state.trunk_scan_active_ordinal = 1;
    tick();
    check(!metrics.syncLabel().isEmpty());
    check(talkgroups.count() == 1);
    QObject::connect(&talkgroups, &QAbstractItemModel::modelReset, [&]() { ++resets; });
    state.synctype = DSD_SYNC_NONE;
    state.lastsynctype = DSD_SYNC_NONE;
    state.trunk_scan_active_ordinal = 2;
    tick();
    check(resets == 0); // A scan hop must preserve the list view.
    check(talkgroups.count() == 1);
    check(metrics.syncLabel().isEmpty()); // Old target's sync hold must not survive.
    diagnostics.submit("host", "info", "before next session");
    for (auto phase : {Host::Starting, Host::Idle, Host::Failed}) {
        host.setPhase(Host::Running);
        state.synctype = DSD_SYNC_P25P1_POS;
        state.p25_p1_fec_ok = 3;
        state.p25_p1_fec_err = 1;
        tick();
        check(metrics.qualityValid() && metrics.ccFecOkPct() == 75.0);
        check(!metrics.syncLabel().isEmpty());
        host.setPhase(phase);
        tick(); // The last engine snapshot is still published after stop.
        check(metrics.syncLabel().isEmpty());
        check(!metrics.qualityValid() && !metrics.ccFecValid());
        check(diagnosticsModel.allText().contains("before next session"));
        check(diagnosticsModel.allText().count("--- session starting ---") == 1);
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
    freeState(&state);
    return 0;
}
