// SPDX-License-Identifier: GPL-3.0-or-later
#include <QCoreApplication>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>
#include <cassert>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
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
    freeState(&state);
    return 0;
}
