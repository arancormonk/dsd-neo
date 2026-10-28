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
#include <QObject>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <cstdio>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/recording_view.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/wav_recording.h>
#include <dsd-neo/runtime/cli.h>
#include <memory>
#include <utility>
#include "../../src/app_control/commands_internal.h"
#include "../test_support/qt_test_paths.h"
#include "app_prefs.h"
#include "command_bridge.h"
#include "decoder_host.h"
#include "recording_sessions.h"
#include "recording_status.h"
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

/* A session the app starts, parsed, with the frontend runtime up on it as a decoder run has it. */
struct LiveSession {
    Decoder decoder;

    LiveSession(dsd_qt::AppPrefs& prefs, Sessions& sessions, bool* ok) {
        dsd_qt::SessionArgsBuilder builder(&prefs);
        builder.setRecordingSessions(&sessions);
        Host host;
        const QVariantMap system{{QStringLiteral("sourceType"), QStringLiteral("usb")},
                                 {QStringLiteral("freqMhz"), QStringLiteral("851.375")}};
        *ok = builder.start(system, &host, QStringLiteral("explore")).value("started").toBool()
              && parse(host.argv, decoder);
        dsd_app_frontend_runtime_start(decoder.opts.get(), decoder.state.get());
    }

    ~LiveSession() {
        dsd_app_frontend_runtime_stop();
        decoder.opts->wav_out_f = close_wav_file(decoder.opts->wav_out_f);
        decoder.opts->wav_out_fR = close_wav_file(decoder.opts->wav_out_fR);
    }

    LiveSession(const LiveSession&) = delete;
    LiveSession& operator=(const LiveSession&) = delete;

    int
    drain() {
        return dsd_app_drain_cmds(decoder.opts.get(), decoder.state.get());
    }
};

/* Settings applies Record calls and unknown-only to a running session (D8): through the real bridge and queue, the
 * slot files open in the folder the session was started with, and close again; RecordingStatus shows each switch
 * applying, then applied. Unknown-only travels with Record calls. */
void
test_live_commands(dsd_qt::AppPrefs& prefs, Sessions& sessions) {
    bool started = false;
    LiveSession live(prefs, sessions, &started);
    expect("a session without Record calls starts and parses", started);
    expectRecording("it records nothing yet", live.decoder, sessions.folder, false, false);
    expect("and its folder holds no recording", slotFilesIn(sessions.folder) == 0);
    dsd_opts* opts = live.decoder.opts.get();

    dsd_qt::CommandBridge bridge;
    dsd_qt::RecordingStatus status;
    bridge.setRecordingStatus(&status);
    status.setSessionActive(true);
    status.setRunUp(true);
    status.refresh();
    expect("the status has the session's view", status.published() && !status.perCall() && status.storageNamed()
                                                    && !status.staticWav() && status.commandState() == "none");

    expect("unknown-only while Record calls is off sends nothing", bridge.setRecordUnknownOnly(true));
    expect("nothing queued", live.drain() == 0 && opts->record_unknown_only == 0);

    expect("Record calls on, unknown-only on, is queued", bridge.setRecordCalls(true, true));
    status.refresh();
    expect("and shows applying", status.commandState() == "pending" && status.command() == "start");
    expect("unknown-only, then the start, apply", live.drain() == 2);
    status.refresh();
    expect("the session now records each call, unknown talkgroups only",
           opts->dmr_stereo_wav == 1 && opts->wav_out_f != nullptr && opts->record_unknown_only == 1 && status.perCall()
               && status.unknownOnly() && status.commandState() == "applied" && status.refusal().isEmpty());
    expect("into the session's folder", slotFilesIn(sessions.folder) > 0);

    expect("unknown-only off while recording is queued", bridge.setRecordUnknownOnly(false));
    expect("and applied", live.drain() == 1 && opts->record_unknown_only == 0);

    expect("Record calls off is queued", bridge.setRecordCalls(false, true));
    expect("unknown-only off and the stop apply", live.drain() == 2);
    status.refresh();
    expect("the session stops recording", opts->record_unknown_only == 0 && opts->dmr_stereo_wav == 0
                                              && opts->wav_out_f == nullptr && opts->wav_out_fR == nullptr
                                              && !status.perCall() && !status.unknownOnly());
}

/* A session started with no storage for call recordings names no folder: Record calls on is refused there, nothing is
 * sent, and the status says why. */
void
test_live_without_storage(dsd_qt::AppPrefs& prefs) {
    Sessions none; // no folder: no storage available
    bool started = false;
    LiveSession live(prefs, none, &started);
    expect("a session with no storage starts", started && live.decoder.opts->wav_journal_file[0] == '\0');
    dsd_qt::CommandBridge bridge;
    dsd_qt::RecordingStatus status;
    bridge.setRecordingStatus(&status);
    status.setSessionActive(true);
    status.setRunUp(true);
    expect("Record calls on is refused", !bridge.setRecordCalls(true, false));
    status.refresh();
    expect("nothing is sent", live.drain() == 0 && live.decoder.opts->dmr_stereo_wav == 0);
    expect("and the status says storage is unavailable", status.refusal() == QStringLiteral("storage-unavailable")
                                                             && !status.storageNamed()
                                                             && status.commandState() != "pending");
}

/* A session whose extra options record to one static WAV (Record calls off at its start): turning Record calls on is
 * refused, so the user's file is not closed under them, and it applies at the next start. */
void
test_live_beside_a_static_wav(dsd_qt::AppPrefs& prefs, Sessions& sessions, const QString& staticWav) {
    prefs.setExtraArgs(QStringLiteral("-w ") + staticWav);
    bool started = false;
    LiveSession live(prefs, sessions, &started);
    prefs.setExtraArgs(QString());
    expect("a session recording to a static WAV starts", started && live.decoder.opts->static_wav_file == 1);
    dsd_qt::CommandBridge bridge;
    dsd_qt::RecordingStatus status;
    bridge.setRecordingStatus(&status);
    status.setSessionActive(true);
    status.setRunUp(true);
    expect("Record calls on is refused", !bridge.setRecordCalls(true, false));
    status.refresh();
    expect("nothing is sent and the static WAV stays open",
           live.drain() == 0 && live.decoder.opts->static_wav_file == 1 && live.decoder.opts->wav_out_f != nullptr);
    expect("the status says why", status.refusal() == QStringLiteral("static-wav") && status.staticWav());
    const int refused = status.refusalCount();
    // Nor is Record calls off sent: a stop would close the user's file as surely as a start.
    expect("Record calls off is refused too", !bridge.setRecordCalls(false, false));
    expect("nothing is sent and the static WAV stays open",
           live.drain() == 0 && live.decoder.opts->static_wav_file == 1 && live.decoder.opts->wav_out_f != nullptr);
    expect("for the same reason", status.refusal() == QStringLiteral("static-wav"));
    // A refusal for the same reason is still counted, so Settings can tell this switch's refusal from an earlier one.
    expect("and counted again", status.refusalCount() == refused + 1);
}

/* A host whose session a test starts and ends. */
class PhaseHost : public dsd_qt::DecoderHost {
  public:
    SessionState phase = Idle;

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
    start(const QStringList& args) override {
        (void)args;
        return false;
    }

    void
    stop() override {}

    void
    setPhase(SessionState next) {
        phase = next;
        Q_EMIT sessionStateChanged();
    }

    /* As Android's: the run says when it is up, and the state is unknown until a first poll. */
    bool signalsInit = false;
    bool known = true;

    bool
    signalsSessionInitialized() const override {
        return signalsInit;
    }

    bool
    sessionStateKnown() const override {
        return known;
    }

    void
    initialized() {
        Q_EMIT sessionInitialized();
    }

    void
    firstPoll(SessionState state) {
        phase = state;
        Q_EMIT sessionStateChanged();
        known = true;
        Q_EMIT sessionStateKnownChanged();
    }
};

/* Until a session's run is up the recording view is the last run's, so a live switch is refused as "starting",
 * whatever that view says, and nothing is sent: while Starting, while Running before a host that signals it says the
 * run is up, and never for a session the UI found running (its run was up long before). */
void
test_status_waits_for_the_run(dsd_qt::AppPrefs& prefs, Sessions& sessions) {
    bool started = false;
    LiveSession live(prefs, sessions, &started);
    expect("a session starts", started);
    dsd_qt::CommandBridge bridge;
    {
        PhaseHost host;
        dsd_qt::RecordingStatus status;
        bridge.setRecordingStatus(&status);
        dsd_qt::recording_status_follow_host(&status, &host);
        host.setPhase(dsd_qt::DecoderHost::Starting);
        expect("while Starting a switch is refused", !bridge.setRecordCalls(false, false));
        expect("as starting, sending nothing", status.refusal() == QStringLiteral("starting") && live.drain() == 0);
        host.setPhase(dsd_qt::DecoderHost::Running);
        expect("Running, it is sent", bridge.setRecordCalls(false, false) && live.drain() == 2);
    }
    {
        PhaseHost host;
        host.signalsInit = true;
        dsd_qt::RecordingStatus status;
        bridge.setRecordingStatus(&status);
        dsd_qt::recording_status_follow_host(&status, &host);
        host.setPhase(dsd_qt::DecoderHost::Starting);
        host.initialized(); // can come while Starting
        host.setPhase(dsd_qt::DecoderHost::Running);
        expect("a run said up before Running is up", bridge.setRecordCalls(false, false) && live.drain() == 2);
        host.setPhase(dsd_qt::DecoderHost::Idle);
        host.setPhase(dsd_qt::DecoderHost::Running);
        expect("the next session waits for its own", !bridge.setRecordUnknownOnly(true)
                                                         && status.refusal() == QStringLiteral("starting")
                                                         && live.drain() == 0);
        host.initialized();
        expect("until it says so", bridge.setRecordCalls(false, false) && live.drain() == 2);
    }
    {
        PhaseHost host;
        host.signalsInit = true;
        host.known = false;
        dsd_qt::RecordingStatus status;
        bridge.setRecordingStatus(&status);
        dsd_qt::recording_status_follow_host(&status, &host);
        host.firstPoll(dsd_qt::DecoderHost::Running);
        expect("a session the UI found running is up", bridge.setRecordCalls(false, false) && live.drain() == 2);
    }
    bridge.setRecordingStatus(nullptr);
}

/* RecordingStatus is the session's: a start and an end clear its refusal, the command readings show nothing of the
 * last session's until this one sends a switch, and with no session a switch is refused as such. */
void
test_status_follows_the_session(dsd_qt::AppPrefs& prefs) {
    Sessions none; // a session with no storage, so a Record calls on is refused while it runs
    bool started = false;
    LiveSession live(prefs, none, &started);
    expect("a session starts", started);
    PhaseHost host;
    dsd_qt::CommandBridge bridge;
    dsd_qt::RecordingStatus status;
    bridge.setRecordingStatus(&status);
    dsd_qt::recording_status_follow_host(&status, &host);

    expect("with no session, Record calls is refused", !bridge.setRecordCalls(true, false));
    expect("as no session", status.refusal() == QStringLiteral("no-session") && live.drain() == 0);
    expect("and so is unknown-only", !bridge.setRecordUnknownOnly(true) && live.drain() == 0);

    dsd_app_recording_view view{};
    (void)dsd_app_recording_view_get(&view);
    expect("an earlier session left a command outcome behind", view.command_state != DSD_APP_RECORDING_COMMAND_NONE);
    host.setPhase(dsd_qt::DecoderHost::Running);
    status.refresh();
    expect("a start clears the refusal", status.refusal().isEmpty() && status.sessionActive());
    expect("and shows no command of the last session's",
           status.commandState() == QStringLiteral("none") && status.command().isEmpty());

    expect("Record calls on with no storage is refused", !bridge.setRecordCalls(true, false));
    expect("as such", status.refusal() == QStringLiteral("storage-unavailable"));
    host.setPhase(dsd_qt::DecoderHost::Idle);
    expect("an end clears it", status.refusal().isEmpty() && !status.sessionActive());

    host.setPhase(dsd_qt::DecoderHost::Running);
    expect("Record calls off is sent", bridge.setRecordCalls(false, false));
    expect("and this session's command shows at once",
           status.commandState() == QStringLiteral("pending") && status.command() == QStringLiteral("stop"));
    expect("both apply", live.drain() == 2);
    status.refresh();
    expect("then applied", status.commandState() == QStringLiteral("applied"));
    host.setPhase(dsd_qt::DecoderHost::Idle);
    expect("and gone with the session", status.commandState() == QStringLiteral("none"));
}

/* RecordingStatus reads the view as QML gets it: names for the states and failure kinds, one change signal per move. */
void
test_status_readings(void) {
    dsd_qt::RecordingStatus status;
    // As in a session that sent a switch: the command readings are the decoder's.
    status.setSessionActive(true);
    status.setRunUp(true);
    status.noteSent();
    status.apply(dsd_app_recording_view{});
    int changes = 0;
    QObject::connect(&status, &dsd_qt::RecordingStatus::changed, &status, [&changes]() { ++changes; });
    expect("nothing published yet", !status.published() && status.commandState() == "none" && status.journal() == "none"
                                        && status.failures() == 0 && status.lastError().isEmpty()
                                        && status.command().isEmpty());
    dsd_app_recording_view view{};
    view.published = 1;
    view.mode = DSD_APP_RECORDING_MODE_STATIC;
    view.journal_state = DSD_APP_RECORDING_JOURNAL_FAILED;
    view.journal_named = 1;
    view.failures[DSD_WAV_RECORDING_ERROR_WRITE] = 2U;
    view.failures[DSD_WAV_RECORDING_ERROR_PUBLISH] = 1U;
    view.last_error = DSD_WAV_RECORDING_ERROR_PUBLISH;
    view.command_state = DSD_APP_RECORDING_COMMAND_REFUSED;
    view.command_id = DSD_APP_CMD_RECORD_UNKNOWN_ONLY_SET;
    status.apply(view);
    const QVariantMap counts = status.failureCounts();
    expect("each reading named", status.published() && status.staticWav() && !status.perCall() && status.storageNamed()
                                     && status.journal() == "failed" && status.commandState() == "refused"
                                     && status.command() == "unknownOnly" && status.failures() == 3
                                     && status.lastError() == "publish");
    expect("failures by kind", counts.size() == 4 && counts.value("write").toInt() == 2
                                   && counts.value("publish").toInt() == 1 && counts.value("open").toInt() == 0);
    status.apply(view);
    expect("one change for one move", changes == 1);
    view.command_state = DSD_APP_RECORDING_COMMAND_DROPPED;
    view.command_id = DSD_APP_CMD_WAV_STOP;
    status.apply(view);
    const int refusals = status.refusalCount();
    status.noteRefusal(QStringLiteral("static-wav"));
    status.noteRefusal(QStringLiteral("static-wav"));
    // Each refusal is a move of its own, the same reason again included: refusalCount() counts it.
    expect("each move signalled once", changes == 4 && status.commandState() == "dropped" && status.command() == "stop"
                                           && status.refusal() == "static-wav"
                                           && status.refusalCount() == refusals + 2);
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
        test_status_readings();
        test_live_commands(prefs, sessions);
        test_live_without_storage(prefs);
        test_live_beside_a_static_wav(prefs, sessions, dir.filePath(QStringLiteral("static.wav")));
        test_status_follows_the_session(prefs);
        test_status_waits_for_the_run(prefs, sessions);
    }
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("OK");
    return 0;
}
