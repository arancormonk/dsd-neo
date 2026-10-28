// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #708: the call recording a session the app starts asks for, through the real CLI parser. Each start path (a
 * saved system, Explore, a scan list) goes through its starter with a recording folder from a stand-in store, and the
 * argv the host receives is parsed as the decoder parses it: the folder, the journal beside it, per-call recording and
 * unknown-only land in the options, and nothing deletes a recording. Then Settings' live switches go through the real
 * command bridge and queue into those options.
 */

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QList>
#include <QMap>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <cstdio>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/recording_view.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/cli.h>
#include <memory>
#include <utility>
#include "../../src/app_control/commands_internal.h"
#include "../test_support/qt_test_paths.h"
#include "app_prefs.h"
#include "command_bridge.h"
#include "decoder_host.h"
#include "recording_sessions.h"
#include "scan_list_starter.h"
#include "scan_lists_model.h"
#include "session_args.h"

namespace {

int g_failures = 0;

void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

/* The recording store, as far as a starter sees it: one folder for every session. */
class Sessions : public dsd_qt::RecordingSessions {
  public:
    QString folder;
    QStringList kinds;

    QString
    beginSession(const QString& kind, const QString& uid, const QString& name) override {
        (void)uid;
        (void)name;
        kinds.append(kind);
        return folder;
    }

    void
    endSession() override {}
};

class Host : public dsd_qt::DecoderHost {
  public:
    QStringList argv;

    bool
    isRunning() const override {
        return false;
    }

    QString
    statusText() const override {
        return {};
    }

    bool
    start(const QStringList& args) override {
        argv = args;
        return true;
    }

    void
    stop() override {}
};

struct Decoder {
    std::unique_ptr<dsd_opts> opts = std::make_unique<dsd_opts>();
    std::unique_ptr<dsd_state> state = std::make_unique<dsd_state>();

    Decoder() {
        initOpts(opts.get());
        initState(state.get());
    }

    ~Decoder() { freeState(state.get()); }

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
};

/* Parse @p args as the decoder does, into @p decoder. */
bool
parse(const QStringList& args, Decoder& decoder) {
    QList<QByteArray> storage{QByteArray("dsd-neo")};
    for (const QString& arg : args) {
        storage.append(arg.toUtf8());
    }
    QList<char*> argv;
    for (QByteArray& arg : storage) {
        argv.append(arg.data());
    }
    argv.append(nullptr);
    int effective = 0;
    int exitCode = 0;
    return dsd_parse_args(static_cast<int>(storage.size()), argv.data(), decoder.opts.get(), decoder.state.get(),
                          &effective, &exitCode)
               == DSD_PARSE_CONTINUE
           && exitCode == 0;
}

/* What the parser made of a session's call recording. */
void
expectRecording(const char* what, const Decoder& decoder, const QString& folder, bool perCall, bool unknownOnly) {
    const dsd_opts& opts = *decoder.opts;
    const bool ok = QString::fromUtf8(opts.wav_out_dir) == folder
                    && QString::fromUtf8(opts.wav_journal_file) == folder + QStringLiteral("/recordings.jsonl")
                    && (opts.dmr_stereo_wav != 0) == perCall && opts.static_wav_file == 0
                    && (opts.record_unknown_only != 0) == unknownOnly && opts.rdio_api_delete_after_upload == 0;
    if (!ok) {
        DSD_FPRINTF(stderr, "  dir=%s journal=%s per_call=%d unknown_only=%d\n", opts.wav_out_dir,
                    opts.wav_journal_file, opts.dmr_stereo_wav, opts.record_unknown_only);
    }
    expect(what, ok);
}

int
slotFilesIn(const QString& folder) {
    return static_cast<int>(QDir(folder).entryList({QStringLiteral("TEMP_*")}, QDir::Files).size());
}

void
test_saved_system(dsd_qt::AppPrefs& prefs, Sessions& sessions) {
    prefs.setRecordCalls(true);
    prefs.setRecordUnknownOnly(true);
    dsd_qt::SessionArgsBuilder builder(&prefs);
    builder.setRecordingSessions(&sessions);
    Host host;
    const QVariantMap system{
        {QStringLiteral("uid"), QStringLiteral("sys-1")},      {QStringLiteral("name"), QStringLiteral("County")},
        {QStringLiteral("sourceType"), QStringLiteral("usb")}, {QStringLiteral("freqMhz"), QStringLiteral("851.375")},
        {QStringLiteral("decodeFlag"), QStringLiteral("-ft")}, {QStringLiteral("trunking"), true}};
    expect("a saved system starts", builder.start(system, &host, QStringLiteral("saved")).value("started").toBool());
    expect("into its own folder", sessions.kinds.value(0) == QStringLiteral("saved"));
    Decoder decoder;
    expect("the saved system's argv parses", parse(host.argv, decoder));
    expectRecording("a saved system records each call, unknown talkgroups only, into its folder", decoder,
                    sessions.folder, true, true);
    prefs.setRecordCalls(false);
    prefs.setRecordUnknownOnly(false);
}

/* Explore with Record calls off: the folder and journal are named anyway, so a -P among the extra options records
 * into a folder the app manages. */
void
test_explore(dsd_qt::AppPrefs& prefs, Sessions& sessions) {
    prefs.setExtraArgs(QStringLiteral("-P"));
    dsd_qt::SessionArgsBuilder builder(&prefs);
    builder.setRecordingSessions(&sessions);
    Host host;
    const QVariantMap explore{{QStringLiteral("sourceType"), QStringLiteral("rtltcp")},
                              {QStringLiteral("host"), QStringLiteral("127.0.0.1")},
                              {QStringLiteral("port"), 1234},
                              {QStringLiteral("freqMhz"), QStringLiteral("851.0125")}};
    sessions.kinds.clear();
    expect("Explore starts", builder.start(explore, &host, QStringLiteral("explore")).value("started").toBool());
    expect("into the Explore folder", sessions.kinds.value(0) == QStringLiteral("explore"));
    Decoder decoder;
    expect("Explore's argv parses", parse(host.argv, decoder));
    expectRecording("an extra -P records into the folder the app named", decoder, sessions.folder, true, false);
    prefs.setExtraArgs(QString());
}

void
test_scan_list(dsd_qt::AppPrefs& prefs, Sessions& sessions) {
    prefs.setRecordCalls(true);
    dsd_qt::ScanListsModel lists;
    lists.add(
        {{"name", "Local"},
         {"sourceType", "usb"},
         {"entries",
          QVariantList{QVariantMap{
              {"kind", "freq"}, {"name", "Simplex"}, {"protocol", "p25"}, {"freqMhz", "851.5"}, {"enabled", true}}}}});
    dsd_qt::ScanListStarter starter(&prefs, nullptr);
    starter.setRecordingSessions(&sessions);
    Host host;
    sessions.kinds.clear();
    expect("a scan list starts", starter.start(lists.get(0), &host).value("started").toBool());
    expect("into its own folder", sessions.kinds.value(0) == QStringLiteral("scan"));
    expect("its argv names the folder once", host.argv.count(QStringLiteral("-7")) == 1);
    Decoder decoder;
    expect("the scan list's argv parses", parse(host.argv, decoder));
    expect("as a trunk scan", decoder.opts->trunk_scan_enabled != 0);
    expectRecording("a scan list records each call into its folder", decoder, sessions.folder, true, false);
    prefs.setRecordCalls(false);
}

/* Settings applies Record calls and unknown-only to a running session (D8): through the real bridge and queue, the
 * slot files open in the folder the session was started with, and close again. */
void
test_live_commands(dsd_qt::AppPrefs& prefs, Sessions& sessions) {
    dsd_qt::SessionArgsBuilder builder(&prefs);
    builder.setRecordingSessions(&sessions);
    Host host;
    const QVariantMap system{{QStringLiteral("sourceType"), QStringLiteral("usb")},
                             {QStringLiteral("freqMhz"), QStringLiteral("851.375")}};
    expect("a session without Record calls starts",
           builder.start(system, &host, QStringLiteral("explore")).value("started").toBool());
    Decoder decoder;
    expect("its argv parses", parse(host.argv, decoder));
    expectRecording("it records nothing yet", decoder, sessions.folder, false, false);
    expect("and its folder holds no recording", slotFilesIn(sessions.folder) == 0);

    dsd_opts* opts = decoder.opts.get();
    dsd_state* state = decoder.state.get();
    dsd_app_frontend_runtime_start(opts, state);
    const dsd_qt::CommandBridge bridge;
    dsd_app_recording_view view{};

    expect("Record calls on is queued", bridge.setRecordCalls(true));
    expect("and applied", dsd_app_drain_cmds(opts, state) == 1);
    (void)dsd_app_recording_view_get(&view);
    expect("the session now records each call", opts->dmr_stereo_wav == 1 && opts->wav_out_f != nullptr
                                                    && view.per_call == 1
                                                    && view.command_state == DSD_APP_RECORDING_COMMAND_APPLIED);
    expect("into the session's folder", slotFilesIn(sessions.folder) > 0);

    expect("unknown-only on is queued", bridge.setRecordUnknownOnly(true));
    expect("and applied", dsd_app_drain_cmds(opts, state) == 1);
    (void)dsd_app_recording_view_get(&view);
    expect("only unknown talkgroups record now", opts->record_unknown_only == 1 && view.unknown_only == 1);

    expect("unknown-only off is queued", bridge.setRecordUnknownOnly(false));
    expect("Record calls off is queued", bridge.setRecordCalls(false));
    expect("both apply", dsd_app_drain_cmds(opts, state) == 2);
    (void)dsd_app_recording_view_get(&view);
    expect("the session stops recording", opts->record_unknown_only == 0 && opts->dmr_stereo_wav == 0
                                              && opts->wav_out_f == nullptr && opts->wav_out_fR == nullptr
                                              && view.per_call == 0);
    dsd_app_frontend_runtime_stop();
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("dsd-neo-test"));
    app.setApplicationName(QStringLiteral("session-args-parse-%1").arg(QCoreApplication::applicationPid()));
    dsd_test_qt_isolate_paths();
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    QTemporaryDir dir;
    expect("a scratch folder", dir.isValid());
    Sessions sessions;
    sessions.folder = dir.filePath(QStringLiteral("Call recordings/County (sys-1abc)"));
    expect("the session's folder", QDir().mkpath(sessions.folder));
    {
        dsd_qt::AppPrefs prefs;
        test_saved_system(prefs, sessions);
        test_explore(prefs, sessions);
        test_scan_list(prefs, sessions);
        test_live_commands(prefs, sessions);
    }
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("OK");
    return 0;
}
