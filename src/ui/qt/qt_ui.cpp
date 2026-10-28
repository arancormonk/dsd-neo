// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QMap>
#include <QVariantMap>
#include "qt_ui.h"

#include <QCoreApplication>
#include <QFontDatabase>
#include <QLatin1String>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <dsd-neo/app_control/history_tap.h>
#include <dsd-neo/runtime/git_ver.h>
#include <functional>
#include <qqml.h>

#include "app_prefs.h"
#include "auto_start_attachment.h"
#include "backup_apply.h"
#include "backup_model.h"
#include "call_history_filter.h"
#include "call_history_model.h"
#include "command_bridge.h"
#include "decoder_host.h"
#include "decryption_profiles_model.h"
#include "diagnostics_log.h"
#include "imported_files_model.h"
#include "input_commit.h"
#include "known_frequency_index.h"
#include "metrics_model.h"
#include "p25_network_model.h" // WP-F2
#include "radio_reference_model.h"
#include "recording_store.h"
#include "saved_systems_model.h"
#include "scan_list_starter.h"
#include "scan_lists_model.h"
#include "session_args.h"
#include "signal_search_model.h"
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
    QObject::connect(host, &DecoderHost::localDeviceAttached, controller, [=](const QString& attachment) {
        const QString attachedKind = attachedReceiverKind(attachment);
        const QString kind = prefs->lastStartedKind();
        const QString uid = prefs->lastStartedUid();
        const QVariantMap target = kind == QStringLiteral("saved")  ? systems->getByUid(uid)
                                   : kind == QStringLiteral("scan") ? scanLists->getByUid(uid)
                                                                    : QVariantMap();
        controller->requestAutoStart(prefs->autoStartOnAttach(), prefs->onboardingDone(), kind, uid, target,
                                     attachedKind);
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

static DecryptionProfilesModel*
wire_decryption_profiles(QQmlApplicationEngine& engine, DecoderHost* host, SavedSystemsModel* systems,
                         ScanListsModel* scanLists, SessionArgsBuilder* sessionArgs, ScanListStarter* scanListStarter,
                         CommandBridge* commands, MetricsModel* metrics) {
    auto* decryptionProfiles = new DecryptionProfilesModel(&engine);
    decryptionProfiles->setReferences(systems, scanLists);
    sessionArgs->setDecryptionProfiles(decryptionProfiles);
    scanListStarter->setDecryptionProfiles(decryptionProfiles);
    commands->setDecryptionProfiles(decryptionProfiles);
    (void)decryptionProfiles->migrateLegacySystems();
    QObject::connect(host, &DecoderHost::sessionStateChanged, decryptionProfiles, [host, decryptionProfiles]() {
        if (!host->sessionActive()) {
            decryptionProfiles->releaseSessionReferences();
        }
    });
    QObject::connect(metrics, &MetricsModel::controlChanged, decryptionProfiles, [host, metrics, decryptionProfiles]() {
        if (host->sessionActive()) {
            decryptionProfiles->retainForSession(
                decryptionProfiles->forRuntimeReference(metrics->keyProfileRef()).value("uid").toString());
        }
    });
    return decryptionProfiles;
}

static void
wire_target_files(ScanListStarter* starter, const ImportedFilesModel* files) {
    starter->setTargetFileLookup([files](const QString& path) {
        const int row = files->rowForPath(path);
        return row >= 0 && files->get(row).value("type") == "trunkTargets";
    });
}

// The screen preferences are storage; the policy is the host's (#574). It runs in
// the platform layer because Qt's event loop is stopped exactly when the policy
// must act: with the screen off, when a call has to wake it. Applied here on
// every process start because the platform keeps no copy of anyone's QSettings,
// and again on every change.
static void
wire_screen_policy(DecoderHost* host, const AppPrefs* prefs) {
    const auto applyScreenPolicy = [host, prefs]() {
        host->setScreenPolicy(prefs->screenMode(), prefs->screenDelaySec());
    };
    applyScreenPolicy();
    QObject::connect(prefs, &AppPrefs::screenPolicyChanged, host, applyScreenPolicy);
}

// Issue #708: the call recording store indexes the recordings, History asks it which rows have one, and the session
// starters ask it for each session's folder. It follows the host's session reports, holding every journal until the
// host knows its state (a decoder that kept running while the UI was recreated is only learnt at the first poll).
static RecordingStore*
wire_recordings(QQmlApplicationEngine& engine, DecoderHost* host, CallHistoryModel* history,
                SessionArgsBuilder* sessionArgs, ScanListStarter* scanListStarter) {
    auto* recordings = new RecordingStore(&engine);
    recordings->setRoots(host->recordingRoots());
    history->setRecordingProvider(recordings);
    QObject::connect(recordings, &RecordingStore::recordingFragmentsChanged, history,
                     &CallHistoryModel::recordingFragmentsChanged);
    sessionArgs->setRecordingSessions(recordings);
    scanListStarter->setRecordingSessions(recordings);
    recording_store_follow_host(recordings, host);
    recordings->start();
    return recordings;
}

// The C++ types QML instantiates.
static void
register_qml_types() {
    // WP-D4: cancellation observes pointer, keyboard and shortcut input.
    qmlRegisterType<dsd_qt::SiteInteractionGuard>("DsdNeo", 1, 0, "SiteInteractionGuard");
    qmlRegisterType<SpectrumTraceItem>("DsdNeo", 1, 0, "SpectrumTrace");
    qmlRegisterType<WaterfallItem>("DsdNeo", 1, 0, "Waterfall");
}

// A restore the user confirmed applies before any model reads a store it replaces (#580), so the restored data goes
// through the loaders, migrations and pruning below like any other start's.
static backup::ApplyResult
apply_confirmed_restore(const DecoderHost* host) {
    backup::ApplyEnv env;
    env.engineBusy = [host]() { return host != nullptr && host->engineBusy(); };
    return backup::apply_pending_restore(env);
}

static void
prune_missing_csv_paths(ImportedFilesModel* importedFiles, SavedSystemsModel* systems, ScanListsModel* scanLists) {
    // The library drops rows whose stored copy vanished behind the app's back;
    // saved systems that still point at one would build a `-G <missing>` argv and
    // fail to start with a parse error naming the input settings, not the file.
    // Reconciled here because the library cannot: it loads in its constructor,
    // and it has no business knowing what references it.
    for (const QString& gone : importedFiles->takePrunedPaths()) {
        systems->clearCsvPath(gone);
        scanLists->clearCsvPath(gone);
    }
}

bool
ui_load(QQmlApplicationEngine& engine, DecoderHost* host) {
    const backup::ApplyResult restoreOutcome = apply_confirmed_restore(host);
    load_fonts(engine.rootContext());

    auto* metrics = new MetricsModel(&engine);
    auto* network = new P25NetworkModel(&engine); // WP-F2
    auto* commands = new CommandBridge(&engine);
    DiagnosticsLog::installTap();
    // The log is process-lifetime and never destroyed, so its last records are made durable here (or, on Android, by
    // main()'s final flush after the teardown, which this only asks to start).
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                     [] { DiagnosticsLog::flushAtQuit(); });
    auto* diagnostics = new DiagnosticsLogModel(nullptr, &engine);
    auto* prefs = new AppPrefs(&engine);
    auto* backupModel = new BackupModel(host, restoreOutcome, &engine);
    auto* sessionArgs = new SessionArgsBuilder(prefs, &engine);
    auto* systems = new SavedSystemsModel(&engine);
    (void)systems->migrateCachedReplayFiles(host);
    sessionArgs->setSavedSystems(systems);

    // WP-S1: persisted lists and the pre-start validation facade.
    auto* scanLists = new ScanListsModel(&engine);
    auto* scanListStarter = new ScanListStarter(prefs, systems, &engine);
    auto* importedFiles = new ImportedFilesModel(host, &engine);
    wire_target_files(scanListStarter, importedFiles);
    auto* decryptionProfiles =
        wire_decryption_profiles(engine, host, systems, scanLists, sessionArgs, scanListStarter, commands, metrics);
    auto* history = new CallHistoryModel(&engine);
    // Issue #708: History keeps reading the published rings while Qt is paused in the background.
    history->setHistoryTapSource(&dsd_app_history_tap_copy);
    // Created after History so it outlives it: History holds it as its recording provider.
    auto* recordings = wire_recordings(engine, host, history, sessionArgs, scanListStarter);
    auto* talkgroups = new TalkgroupListModel(history, &engine);
    auto* talkgroupView = new TalkgroupFilterModel(&engine);
    talkgroupView->setSourceModel(talkgroups);
    auto* spectrum = new SpectrumModel(&engine);
    auto* knownFrequencies = new KnownFrequencyIndex(systems, scanLists, &engine);
    auto* signalSearch = new SignalSearchModel(host, knownFrequencies, &engine);
    // Each view that shows the call log owns its filter state: the history tab's
    // search and pills must not silently filter the monitor's recent-calls pane.
    auto* historyView = new CallHistoryFilterModel(&engine);
    historyView->setSourceModel(history);
    auto* monitorView = new CallHistoryFilterModel(&engine);
    monitorView->setSourceModel(history);
    auto* radioReference = new RadioReferenceModel(prefs, importedFiles, host, &engine);
    auto* controller = new UiController(host, metrics, history, talkgroups, &engine);
    controller->setCommandBridge(commands);
    controller->setP25Network(network); // WP-F2
    wire_attachment(host, controller, prefs, systems, scanLists);

    prune_missing_csv_paths(importedFiles, systems, scanLists);

    wire_screen_policy(host, prefs);

    /* Register the C++ types QML instantiates before loading their importers. */
    register_qml_types();

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
    context->setContextProperty(QStringLiteral("backup"), backupModel);
    context->setContextProperty(QStringLiteral("decryptionProfiles"), decryptionProfiles);
    context->setContextProperty(QStringLiteral("sessionArgs"), sessionArgs);
    context->setContextProperty(QStringLiteral("savedSystems"), systems);
    context->setContextProperty(QStringLiteral("scanLists"), scanLists);
    context->setContextProperty(QStringLiteral("scanListStarter"), scanListStarter);
    context->setContextProperty(QStringLiteral("importedFiles"), importedFiles);
    context->setContextProperty(QStringLiteral("radioReference"), radioReference);
    context->setContextProperty(QStringLiteral("callHistory"), history);
    context->setContextProperty(QStringLiteral("recordings"), recordings);
    context->setContextProperty(QStringLiteral("historyView"), historyView);
    context->setContextProperty(QStringLiteral("monitorView"), monitorView);
    context->setContextProperty(QStringLiteral("talkgroups"), talkgroups);
    context->setContextProperty(QStringLiteral("talkgroupView"), talkgroupView);
    context->setContextProperty(QStringLiteral("spectrum"), spectrum);
    context->setContextProperty(QStringLiteral("knownFrequencies"), knownFrequencies);
    context->setContextProperty(QStringLiteral("signalSearch"), signalSearch);
    context->setContextProperty(QStringLiteral("appVersionText"), QString::fromUtf8(GIT_TAG));
    // Issue #718: forms finish the keyboard's composing word before reading their fields.
    context->setContextProperty(QStringLiteral("inputCommit"), new InputCommit(&engine));

    engine.load(QUrl(QStringLiteral("qrc:/dsdneo/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return false;
    }

    controller->start();
    return true;
}

} // namespace dsd_qt
