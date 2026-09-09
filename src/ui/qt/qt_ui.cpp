// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QMap>
#include <QVariantMap>
#include "qt_ui.h"

#include <QFontDatabase>
#include <QLatin1String>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <dsd-neo/runtime/git_ver.h>
#include <qqml.h>

#include "app_prefs.h"
#include "call_history_filter.h"
#include "call_history_model.h"
#include "command_bridge.h"
#include "decoder_host.h"
#include "diagnostics_log.h"
#include "imported_files_model.h"
#include "metrics_model.h"
#include "p25_network_model.h" // WP-F2
#include "radio_reference_model.h"
#include "saved_systems_model.h"
#include "scan_list_starter.h"
#include "scan_lists_model.h"
#include "session_args.h"
#include "site_groups.h"
#include "spectrum_model.h"
#include "spectrum_view_item.h"
#include "talkgroup_filter_model.h"
#include "talkgroup_list_model.h"
#include "ui_controller.h"

namespace dsd_qt {

namespace {

/**
 * @brief Load one bundled face and hand its family name back.
 *
 * The whole UI sets its faces explicitly (IBM Plex Sans for text, IBM Plex Mono
 * for data); per-OS system-font fallback would break both the design's metrics
 * and the tabular-numeral alignment the monitor relies on.
 */
QString
load_font(const char* resource) {
    const int id = QFontDatabase::addApplicationFont(QLatin1String(resource));
    if (id < 0) {
        return QString();
    }
    const QStringList families = QFontDatabase::applicationFontFamilies(id);
    return families.isEmpty() ? QString() : families.first();
}

void
load_fonts(QQmlContext* context) {
    // The weight variants register into the same family, so one name per family is
    // enough; Qt resolves font.weight against whichever variants are loaded.
    const QString sans_family = load_font(":/dsdneo/fonts/IBMPlexSans-Regular.ttf");
    (void)load_font(":/dsdneo/fonts/IBMPlexSans-SemiBold.ttf");
    (void)load_font(":/dsdneo/fonts/IBMPlexSans-Bold.ttf");
    const QString mono_family = load_font(":/dsdneo/fonts/IBMPlexMono-Regular.ttf");
    (void)load_font(":/dsdneo/fonts/IBMPlexMono-Medium.ttf");

    context->setContextProperty(QStringLiteral("sansFontFamily"),
                                sans_family.isEmpty() ? QStringLiteral("sans-serif") : sans_family);
    context->setContextProperty(QStringLiteral("monoFontFamily"),
                                mono_family.isEmpty() ? QStringLiteral("monospace") : mono_family);
}

void
wire_attachment(DecoderHost* host, UiController* controller, const AppPrefs* prefs, const SavedSystemsModel* systems,
                const ScanListsModel* scanLists) {
    // WP-S2: resolve stable identities at the attachment edge; no recency writes here.
    QObject::connect(host, &DecoderHost::localDeviceAttached, controller, [=](const QString&) {
        const QString kind = prefs->lastStartedKind();
        const QString uid = prefs->lastStartedUid();
        const QVariantMap target = kind == QStringLiteral("saved")  ? systems->getByUid(uid)
                                   : kind == QStringLiteral("scan") ? scanLists->getByUid(uid)
                                                                    : QVariantMap();
        controller->requestAutoStart(prefs->autoStartOnAttach(), prefs->onboardingDone(), kind, uid, target);
    });
}

} // namespace

void
ui_apply_style(void) {
    // Basic, not Material: every control the design needs is custom-drawn from the
    // token set, and the Material style would fight those with its own metrics,
    // ripples and theming.
    QQuickStyle::setStyle(QStringLiteral("Basic"));
}

bool
ui_load(QQmlApplicationEngine& engine, DecoderHost* host) {
    load_fonts(engine.rootContext());

    auto* metrics = new MetricsModel(&engine);
    auto* network = new P25NetworkModel(&engine); // WP-F2
    auto* commands = new CommandBridge(&engine);
    DiagnosticsLog::installTap();
    auto* diagnostics = new DiagnosticsLogModel(nullptr, &engine);
    auto* prefs = new AppPrefs(&engine);
    auto* sessionArgs = new SessionArgsBuilder(prefs, &engine);
    auto* systems = new SavedSystemsModel(&engine);
    sessionArgs->setSavedSystems(systems);

    // WP-S1: persisted lists and the pre-start validation facade.
    auto* scanLists = new ScanListsModel(&engine);
    auto* scanListStarter = new ScanListStarter(prefs, systems, &engine);
    auto* importedFiles = new ImportedFilesModel(host, &engine);
    auto* history = new CallHistoryModel(&engine);
    auto* talkgroups = new TalkgroupListModel(history, &engine);
    auto* talkgroupView = new TalkgroupFilterModel(&engine);
    talkgroupView->setSourceModel(talkgroups);
    auto* spectrum = new SpectrumModel(&engine);
    // Each view that shows the call log owns its filter state: the history tab's
    // search and pills must not silently filter the monitor's recent-calls pane.
    auto* historyView = new CallHistoryFilterModel(&engine);
    historyView->setSourceModel(history);
    auto* monitorView = new CallHistoryFilterModel(&engine);
    monitorView->setSourceModel(history);
    auto* radioReference = new RadioReferenceModel(prefs, importedFiles, host, &engine);
    auto* controller = new UiController(host, metrics, history, talkgroups, &engine);
    controller->setP25Network(network); // WP-F2
    wire_attachment(host, controller, prefs, systems, scanLists);

    // The library drops rows whose stored copy vanished behind the app's back;
    // saved systems that still point at one would build a `-G <missing>` argv and
    // fail to start with a parse error naming the input settings, not the file.
    // Reconciled here because the library cannot: it loads in its constructor,
    // and it has no business knowing what references it.
    for (const QString& gone : importedFiles->takePrunedPaths()) {
        systems->clearCsvPath(gone);
    }

    // The keep-awake preference is storage; the effect is the host's (an Android
    // window flag). Re-asserted here on every process start because the platform
    // recreates the window without consulting anyone's QSettings.
    host->setKeepScreenAwake(prefs->keepScreenAwake());
    QObject::connect(prefs, &AppPrefs::keepScreenAwakeChanged, host,
                     [host, prefs]() { host->setKeepScreenAwake(prefs->keepScreenAwake()); });

    /* Register the C++ types QML instantiates before loading their importers. */
    // WP-D4: cancellation observes pointer, keyboard and shortcut input.
    qmlRegisterType<dsd_qt::SiteInteractionGuard>("DsdNeo", 1, 0, "SiteInteractionGuard");
    qmlRegisterType<SpectrumTraceItem>("DsdNeo", 1, 0, "SpectrumTrace");
    qmlRegisterType<WaterfallItem>("DsdNeo", 1, 0, "Waterfall");

    // Integration slots: register later packages' owned context objects here,
    // before QML loads. Keeping these in one place prevents parallel packages
    // from creating duplicate owners or reading the decoder snapshot separately.
    // Location context registration (location package).
    // Trunk-scan context registration (scan package).
    // Diagnostics context registration (diagnostics package; exclude keys/location).
    // Recording/playback context registration (media packages).
    QQmlContext* context = engine.rootContext();
    controller->setDiagnosticsLog(diagnostics);
    context->setContextProperty(QStringLiteral("diagnosticsLog"), diagnostics);
    context->setContextProperty(QStringLiteral("decoderHost"), host);
    context->setContextProperty(QStringLiteral("metrics"), metrics);
    context->setContextProperty(QStringLiteral("p25Network"), network); // WP-F2
    context->setContextProperty(QStringLiteral("commands"), commands);
    context->setContextProperty(QStringLiteral("uiController"), controller);
    context->setContextProperty(QStringLiteral("prefs"), prefs);
    context->setContextProperty(QStringLiteral("sessionArgs"), sessionArgs);
    context->setContextProperty(QStringLiteral("savedSystems"), systems);
    context->setContextProperty(QStringLiteral("scanLists"), scanLists);
    context->setContextProperty(QStringLiteral("scanListStarter"), scanListStarter);
    context->setContextProperty(QStringLiteral("importedFiles"), importedFiles);
    context->setContextProperty(QStringLiteral("radioReference"), radioReference);
    context->setContextProperty(QStringLiteral("callHistory"), history);
    context->setContextProperty(QStringLiteral("historyView"), historyView);
    context->setContextProperty(QStringLiteral("monitorView"), monitorView);
    context->setContextProperty(QStringLiteral("talkgroups"), talkgroups);
    context->setContextProperty(QStringLiteral("talkgroupView"), talkgroupView);
    context->setContextProperty(QStringLiteral("spectrum"), spectrum);
    context->setContextProperty(QStringLiteral("appVersionText"), QString::fromUtf8(GIT_TAG));

    engine.load(QUrl(QStringLiteral("qrc:/dsdneo/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return false;
    }

    controller->start();
    return true;
}

} // namespace dsd_qt
