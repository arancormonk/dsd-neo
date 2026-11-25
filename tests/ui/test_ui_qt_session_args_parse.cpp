// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #708: the call recording a session the app starts asks for, through the real CLI parser. Each start path (a
 * saved system, Explore, a scan list) goes through its starter with a recording folder from a stand-in store, and the
 * argv the host receives is parsed as the decoder parses it: the folder, the journal beside it, per-call recording and
 * unknown-only land in the options, and nothing deletes a recording. Then Settings' live switches go through the real
 * command bridge and queue into those options, and the extra options the app refuses are refused where the parser
 * would honour them.
 */

#include <QByteArray>
#include <QByteArrayView>
#include <QChar>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QLatin1String>
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
#include <Qt>
#include <QtGlobal>
#include <cmath>
#include <cstdio>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/recording_view.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/wav_recording.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/input_spec.h>
#include <dsd-neo/runtime/squelch.h>
#include <dsd-neo/runtime/telemetry.h>
#include <memory>
#include <qtenvironmentvariables.h>
#include <stdint.h>
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

#ifdef DSD_NEO_TEST_SUBMIT_WRAP
/* The queue refuses a submission only for a session that closed between two of the bridge's; this stands in for that
   race: the bridge's submission of g_refuse_cmd is refused, every other one goes to the real queue. */
namespace {
int g_refuse_cmd = 0;
} // namespace

// GNU ld --wrap entry points must keep the reserved __wrap_* and __real_* symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
extern "C" int __real_dsd_app_command_submit(int cmd_id, const void* payload, size_t payload_sz);
extern "C" int __real_dsd_app_command_set_i32(int cmd_id, int32_t value);
extern "C" int __wrap_dsd_app_command_submit(int cmd_id, const void* payload, size_t payload_sz);
extern "C" int __wrap_dsd_app_command_set_i32(int cmd_id, int32_t value);

int
__wrap_dsd_app_command_submit(int cmd_id, const void* payload, size_t payload_sz) {
    return cmd_id == g_refuse_cmd ? DSD_APP_COMMAND_SUBMIT_REJECTED
                                  : __real_dsd_app_command_submit(cmd_id, payload, payload_sz);
}

int
__wrap_dsd_app_command_set_i32(int cmd_id, int32_t value) {
    return cmd_id == g_refuse_cmd ? DSD_APP_COMMAND_SUBMIT_REJECTED : __real_dsd_app_command_set_i32(cmd_id, value);
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
#endif

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
    prefs.setAnalogMaxGapMs(2500);
    prefs.setAnalogPrerollMs(300);
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
    expect("with the analog pause hold Settings shows (issue #756)", decoder.opts->analog_record_max_gap_ms == 2500);
    expect("and the NFM pre-roll (issue #668)", decoder.opts->analog_record_preroll_ms == 300);
    // One in the extra options comes later and wins.
    prefs.setExtraArgs(QStringLiteral("--analog-record-max-gap-ms 4000 --analog-record-preroll-ms 700"));
    Host extraHost;
    expect("a saved system with an extra hold starts",
           builder.start(system, &extraHost, QStringLiteral("saved")).value("started").toBool());
    Decoder extraDecoder;
    expect("its argv parses", parse(extraHost.argv, extraDecoder));
    expect("and the extra hold wins", extraDecoder.opts->analog_record_max_gap_ms == 4000);
    expect("and so does the extra pre-roll", extraDecoder.opts->analog_record_preroll_ms == 700);
    prefs.setExtraArgs(QString());
    prefs.setRecordCalls(false);
    prefs.setRecordUnknownOnly(false);
    prefs.setAnalogMaxGapMs(200);
    prefs.setAnalogPrerollMs(DSD_ANALOG_RECORD_PREROLL_DEFAULT_MS);
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

/* Issue #899: a generated scan list's plain nfm row carries no squelch of its own (it inherits), and the session's argv
   passes the app's default for the decoder to run on it, as every radio start does; an Advanced --squelch comes after
   it and is the session's explicit squelch. The decoder reads both as the command line says. */
void
test_scan_list_rows_inherit(dsd_qt::AppPrefs& prefs, Sessions& sessions) {
    const struct {
        const char* squelch;
        const char* extra;
        int mode;
        int ended;
    } cases[] = {
        {"noise+10", "", DSD_SQUELCH_MODE_NOISE, 0},
        {"auto+14", "", DSD_SQUELCH_MODE_AUTO, 0},
        {"noise+10", "--squelch -1", DSD_SQUELCH_MODE_NOISE, 1},
        {"off", "", DSD_SQUELCH_MODE_LEVEL, 0},
    };

    dsd_qt::ScanListsModel lists;
    expect("the nfm scan list is added", lists.add({{"name", "Simplex"},
                                                    {"sourceType", "usb"},
                                                    {"entries", QVariantList{QVariantMap{{"kind", "freq"},
                                                                                         {"name", "Ops"},
                                                                                         {"protocol", "nfm"},
                                                                                         {"freqMhz", "154.43"},
                                                                                         {"enabled", true}}}}}));
    const QVariantMap list = lists.get(lists.count() - 1);
    for (const auto& c : cases) {
        prefs.setSquelch(QString::fromLatin1(c.squelch));
        prefs.setExtraArgs(QString::fromLatin1(c.extra));
        dsd_qt::ScanListStarter starter(&prefs, nullptr);
        starter.setRecordingSessions(&sessions);
        Host host;
        expect("the scan list starts", starter.start(list, &host).value("started").toBool());
        const qsizetype at = host.argv.indexOf(QStringLiteral("--trunk-scan"));
        QFile csv(host.argv.value(at + 1));
        const bool read = at >= 0 && csv.open(QIODevice::ReadOnly);
        const QString row = read ? QString::fromUtf8(csv.readAll()).trimmed().section(QLatin1Char('\n'), 1) : QString();
        Decoder decoder;
        const bool parsed = parse(host.argv, decoder);
        const dsd_opts* opts = decoder.opts.get();
        const bool ok = row.contains(QStringLiteral("nfm-conventional,154430000,"))
                        && !row.contains(QStringLiteral("--squelch")) && parsed && opts->squelch_default_mode == c.mode
                        && (opts->squelch_default_state == DSD_SQUELCH_DEFAULT_ENDED) == (c.ended != 0);
        if (!ok) {
            DSD_FPRINTF(stderr, "  squelch=%s extra=%s row=%s parsed=%d mode=%d state=%d\n", c.squelch, c.extra,
                        qPrintable(row), parsed ? 1 : 0, opts->squelch_default_mode, opts->squelch_default_state);
        }
        expect("the nfm row inherits, and the decoder holds the app's default", ok);
    }
    expect("the scan list is removed", lists.remove(lists.count() - 1));
    prefs.setSquelch(QStringLiteral("noise+10"));
    prefs.setExtraArgs(QString());
}

/* Start @p args as the decoder host does (dsd_runtime_bootstrap(): a config file named with --config loads first, then
   the command line goes over it), into @p decoder. */
bool
bootstrap(const QStringList& args, Decoder& decoder) {
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
    return dsd_runtime_bootstrap(static_cast<int>(storage.size()), argv.data(), decoder.opts.get(), decoder.state.get(),
                                 &effective, &exitCode)
               == DSD_BOOTSTRAP_CONTINUE
           && exitCode == 0;
}

/* Issue #834: a scan list's checkboxes, Voice only and Skip silent channels, own their settings over a config file the
   extra options load with --config: an INI that turns both on runs the scan with each off when its box is unchecked,
   or when the list was saved before the setting existed, and on when it is checked. */
void
test_scan_list_switches_over_a_config(dsd_qt::AppPrefs& prefs, Sessions& sessions, const QString& scratch) {
    expect("the config's folder", QDir().mkpath(scratch));
    const QString ini = scratch + QStringLiteral("/switches-on.ini");
    QFile file(ini);
    expect("the config is written",
           file.open(QIODevice::WriteOnly | QIODevice::Truncate)
               && file.write("[trunking]\nscan_voice_only = true\nscan_skip_silent = true\n") > 0);
    file.close();
    qputenv("DSD_NEO_NO_BOOTSTRAP", "1");
    qunsetenv("DSD_NEO_CONFIG");
    prefs.setExtraArgs(QStringLiteral("--config ") + ini);

    const struct {
        const char* what;
        const char* field;
        QVariant value;
        int want;
    } cases[] = {
        {"an unchecked Voice only box runs the scan with voice-only off over the config's on", "voiceOnly",
         QVariant(false), 0},
        {"a list saved before Voice only runs it off over the config's on", "voiceOnly", QVariant(), 0},
        {"a checked Voice only box runs it on", "voiceOnly", QVariant(true), 1},
        {"an unchecked Skip silent box runs the scan with skip-silent off over the config's on", "skipSilent",
         QVariant(false), 0},
        {"a list saved before Skip silent runs it off over the config's on", "skipSilent", QVariant(), 0},
        {"a checked Skip silent box runs it on", "skipSilent", QVariant(true), 1},
    };

    for (const auto& c : cases) {
        QVariantMap list{{"name", "Local"},
                         {"sourceType", "usb"},
                         {"entries", QVariantList{QVariantMap{{"kind", "freq"},
                                                              {"name", "Simplex"},
                                                              {"protocol", "p25"},
                                                              {"freqMhz", "851.5"},
                                                              {"enabled", true}}}}};
        if (c.value.isValid()) {
            list.insert(QString::fromLatin1(c.field), c.value);
        }
        dsd_qt::ScanListsModel lists;
        expect("the scan list is added", lists.add(list));
        const int row = lists.count() - 1;
        dsd_qt::ScanListStarter starter(&prefs, nullptr);
        starter.setRecordingSessions(&sessions);
        Host host;
        expect("the scan list starts", starter.start(lists.get(row), &host).value("started").toBool());
        expect("the scan list is removed", lists.remove(row));
        Decoder decoder;
        const bool started = bootstrap(host.argv, decoder);
        const int got = QLatin1String(c.field) == QLatin1String("voiceOnly") ? decoder.opts->scan_voice_only
                                                                             : decoder.opts->scan_skip_silent;
        if (!started || got != c.want) {
            DSD_FPRINTF(stderr, "  started=%d %s=%d argv=%s\n", started, c.field, got,
                        qPrintable(host.argv.join(QLatin1Char(' '))));
        }
        expect(c.what, started && got == c.want);
    }
    prefs.setExtraArgs(QString());
    qunsetenv("DSD_NEO_NO_BOOTSTRAP");
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

/* The squelch the decoder holds now: its mode, its margin for a dynamic one, its level in dB (0 for off). */
bool
squelchIs(const dsd_opts* opts, int mode, int margin, double db) {
    if (opts->rtl_squelch_mode != mode) {
        return false;
    }
    if (mode != DSD_SQUELCH_MODE_LEVEL) {
        return opts->rtl_squelch_margin_db == margin;
    }
    const double level = dsd_squelch_level_from_sql(db);
    return db == 0.0 ? !(opts->rtl_squelch_level > 0.0) : std::fabs(opts->rtl_squelch_level - level) <= level * 1e-9;
}

/* A session started from @p system through the real builder, parsed and with its radio spec read as the engine reads
   it when the input opens (a radio input), the squelch it opens with settled as the engine's setup settles it, the
   frontend runtime up on it. */
struct LiveStart {
    Decoder decoder;

    LiveStart(dsd_qt::AppPrefs& prefs, const QVariantMap& system, const QString& kind, bool* ok) {
        dsd_qt::SessionArgsBuilder builder(&prefs);
        Host host;
        *ok = builder.start(system, &host, kind).value("started").toBool() && parse(host.argv, decoder)
              && dsd_rtl_input_spec_apply(decoder.opts.get()) == 1;
        decoder.opts->audio_in_type = AUDIO_IN_RTL; // as the engine sets it when the radio opens
        (void)dsd_squelch_analog_default_sync(decoder.opts.get(), decoder.state.get());
        dsd_app_frontend_runtime_start(decoder.opts.get(), decoder.state.get());
    }

    ~LiveStart() { dsd_app_frontend_runtime_stop(); }

    LiveStart(const LiveStart&) = delete;
    LiveStart& operator=(const LiveStart&) = delete;

    int
    drain() {
        return dsd_app_drain_cmds(decoder.opts.get(), decoder.state.get());
    }
};

/* The decoder's notice line now. */
QString
uiMessage(const LiveStart& live) {
    return QString::fromUtf8(live.decoder.state->ui_msg);
}

/* Issue #899: the dB choice puts back the level kept beneath a dynamic squelch and says what it asked for, tagged, so
 * the Radio sheet saves that level and only on a snapshot taken after the decoder answered the request. */
void
test_restore_level_says_what_it_asked(dsd_qt::AppPrefs& prefs) {
    dsd_qt::CommandBridge bridge;
    const QVariantMap explore{{QStringLiteral("sourceType"), QStringLiteral("usb")},
                              {QStringLiteral("freqMhz"), QStringLiteral("851.375")}};
    bool ok = false;
    LiveStart live(prefs, explore, QStringLiteral("explore"), &ok);
    dsd_opts* opts = live.decoder.opts.get();
    expect("an Explore start parses", ok);
    expect("a level, then noise above it", bridge.setSquelchDb(-60.0) && live.drain() == 1 && bridge.setSquelchNoise(12)
                                               && live.drain() == 1
                                               && squelchIs(opts, DSD_SQUELCH_MODE_NOISE, 12, 0.0));
    dsd_telemetry_publish_opts_snapshot(opts);
    dsd_telemetry_publish_snapshot(live.decoder.state.get());
    const QVariant asked = bridge.restoreSquelchLevel();
    const QVariantMap level = asked.toMap();
    const int request = level.value(QStringLiteral("request")).toInt();
    expect("the restore says the level it asked for",
           asked.isValid() && request > 0 && !level.value(QStringLiteral("off")).toBool()
               && level.value(QStringLiteral("whole")).toBool() && level.value(QStringLiteral("db")).toInt() == -60);
    const dsd_state* state = live.decoder.state.get();
    expect("not answered before the decoder handles it",
           state->squelch_request_answered != static_cast<uint32_t>(request));
    expect("the decoder handles it", live.drain() == 1 && squelchIs(opts, DSD_SQUELCH_MODE_LEVEL, 0, -60.0));
    expect("and answers its tag, applied",
           state->squelch_request_answered == static_cast<uint32_t>(request) && state->squelch_request_applied == 1U);
    expect("off beneath noise",
           bridge.setSquelchDb(0.0) && live.drain() == 1 && bridge.setSquelchNoise(12) && live.drain() == 1);
    dsd_telemetry_publish_opts_snapshot(opts);
    const QVariantMap off = bridge.restoreSquelchLevel().toMap();
    expect("comes back as off with a new tag",
           off.value(QStringLiteral("off")).toBool() && off.value(QStringLiteral("request")).toInt() != request
               && live.drain() == 1 && squelchIs(opts, DSD_SQUELCH_MODE_LEVEL, 0, 0.0));
}

/* Issue #899 (final-diff review r2): every squelch edit the Radio and Squelch sheets save is tagged, not only a
 * restore: each setter returns its request's tag, which the decoder answers once it handles that request, so the
 * sheets save a setting only on the decoder's answer to it. A level goes as the whole setting, stored as the dB setter
 * always stored it (0 dB or more is off). */
void
test_every_squelch_edit_is_tagged(dsd_qt::AppPrefs& prefs) {
    dsd_qt::CommandBridge bridge;
    const QVariantMap explore{{QStringLiteral("sourceType"), QStringLiteral("usb")},
                              {QStringLiteral("freqMhz"), QStringLiteral("851.375")}};
    bool ok = false;
    LiveStart live(prefs, explore, QStringLiteral("explore"), &ok);
    dsd_opts* opts = live.decoder.opts.get();
    expect("an Explore start parses", ok);
    const dsd_state* state = live.decoder.state.get();
    const auto answered = [state](int request) {
        return state->squelch_request_answered == static_cast<uint32_t>(request)
               && state->squelch_request_applied == 1U;
    };
    const int level = bridge.setSquelchDb(-60.0);
    expect("a level edit is tagged", level > 0 && state->squelch_request_answered != static_cast<uint32_t>(level));
    expect("and answered once handled",
           live.drain() == 1 && squelchIs(opts, DSD_SQUELCH_MODE_LEVEL, 0, -60.0) && answered(level));
    const int noise = bridge.setSquelchNoise(12);
    expect("a noise edit, with a tag of its own", noise > level);
    expect("answered", live.drain() == 1 && squelchIs(opts, DSD_SQUELCH_MODE_NOISE, 12, -60.0) && answered(noise));
    const int automatic = bridge.setSquelchAuto(14);
    expect("an auto edit too", automatic > noise && live.drain() == 1
                                   && squelchIs(opts, DSD_SQUELCH_MODE_AUTO, 14, -60.0) && answered(automatic));
    const int off = bridge.setSquelchDb(0.0);
    expect("0 dB is off",
           off > automatic && live.drain() == 1 && squelchIs(opts, DSD_SQUELCH_MODE_LEVEL, 14, 0.0) && answered(off));
    expect("a margin the decoder cannot take is not sent",
           bridge.setSquelchNoise(31) == 0 && bridge.setSquelchAuto(2) == 0 && live.drain() == 0);
    expect("nor a level that is no number", bridge.setSquelchDb(std::nan("")) == 0 && live.drain() == 0);
}

/* Issue #899: the app's default squelch follows a live decode switch on the decoder, with the switch itself: one
 * command, through the real bridge and queue. Explore starts on Auto with the squelch off; picking NFM brings Noise +10
 * in, picking P25 again leaves it exactly off; once anyone sets one (the app, the control API, a config), or when a
 * saved system or the Advanced arguments name one, no switch touches it, an Off or a value equal to the default as
 * much as another. The switch's own message stands. */
void
test_live_decode_switch_squelch(dsd_qt::AppPrefs& prefs) {
    dsd_qt::CommandBridge bridge;
    const QVariantMap explore{{QStringLiteral("sourceType"), QStringLiteral("usb")},
                              {QStringLiteral("freqMhz"), QStringLiteral("851.375")}};
    {
        bool ok = false;
        LiveStart live(prefs, explore, QStringLiteral("explore"), &ok);
        dsd_opts* opts = live.decoder.opts.get();
        expect("an Explore start parses", ok);
        expect("and starts with the squelch off", squelchIs(opts, DSD_SQUELCH_MODE_LEVEL, 0, 0.0));
        expect("picking NFM is one command", bridge.setDecodeMode(DSDCFG_MODE_ANALOG) && live.drain() == 1);
        expect("and the app's default comes in with it",
               opts->analog_only == 1 && squelchIs(opts, DSD_SQUELCH_MODE_NOISE, 10, 0.0));
        expect("the switch's message stands", uiMessage(live) == QStringLiteral("Decoding Analog"));
        expect("picking AM keeps the configured noise (AM runs it as auto)",
               bridge.setDecodeMode(DSDCFG_MODE_AM) && live.drain() == 1 && opts->analog_demod == DSD_ANALOG_DEMOD_AM
                   && squelchIs(opts, DSD_SQUELCH_MODE_NOISE, 10, 0.0) && uiMessage(live).startsWith("Decoding AM"));
        expect("and back to NFM runs noise", bridge.setDecodeMode(DSDCFG_MODE_ANALOG) && live.drain() == 1
                                                 && squelchIs(opts, DSD_SQUELCH_MODE_NOISE, 10, 0.0));
        expect("picking P25 again", bridge.setDecodeMode(DSDCFG_MODE_P25P1) && live.drain() == 1);
        expect("leaves the squelch exactly off", opts->analog_only == 0
                                                     && squelchIs(opts, DSD_SQUELCH_MODE_LEVEL, 0, 0.0)
                                                     && uiMessage(live).startsWith("Decoding P25"));
        expect("picking DMR leaves it off", bridge.setDecodeMode(DSDCFG_MODE_DMR) && live.drain() == 1
                                                && squelchIs(opts, DSD_SQUELCH_MODE_LEVEL, 0, 0.0));
        expect("Explore Auto to AM brings the default in as noise",
               bridge.setDecodeMode(DSDCFG_MODE_AM) && live.drain() == 1
                   && squelchIs(opts, DSD_SQUELCH_MODE_NOISE, 10, 0.0) && uiMessage(live).startsWith("Decoding AM"));
        // TETRA's rate advice is the switch's message. A build without a radio front end plans no DSP rate, so gives
        // no advice: the plain switch message stands there.
        expect("picking TETRA takes it out", bridge.setDecodeMode(DSDCFG_MODE_TETRA) && live.drain() == 1
                                                 && squelchIs(opts, DSD_SQUELCH_MODE_LEVEL, 0, 0.0));
#ifdef USE_RADIO
        expect("and its rate advice stands", uiMessage(live).startsWith("Decoding TETRA: "));
#else
        expect("and its message stands", uiMessage(live) == QStringLiteral("Decoding TETRA"));
#endif
        expect("NFM again brings it in again", bridge.setDecodeMode(DSDCFG_MODE_ANALOG) && live.drain() == 1
                                                   && squelchIs(opts, DSD_SQUELCH_MODE_NOISE, 10, 0.0));
        expect("the user's own squelch is sent", bridge.setSquelchAuto(15) && live.drain() == 1);
        expect("and a switch to P25 leaves it", bridge.setDecodeMode(DSDCFG_MODE_P25P1) && live.drain() == 1
                                                    && squelchIs(opts, DSD_SQUELCH_MODE_AUTO, 15, 0.0));
        expect("as does one back to NFM", bridge.setDecodeMode(DSDCFG_MODE_ANALOG) && live.drain() == 1
                                              && squelchIs(opts, DSD_SQUELCH_MODE_AUTO, 15, 0.0));
    }
#ifdef USE_RADIO
    {
        // A switch the decoder refuses (AM's 6 kHz channel does not fit a 6 kHz DSP bandwidth) leaves the squelch off,
        // and its refusal is the message the operator reads. Only the radio front end checks the bandwidth.
        QVariantMap narrow = explore;
        narrow.insert(QStringLiteral("bandwidthKhz"), 6);
        bool ok = false;
        LiveStart live(prefs, narrow, QStringLiteral("explore"), &ok);
        dsd_opts* opts = live.decoder.opts.get();
        expect("an Explore start at 6 kHz parses", ok && opts->rtl_dsp_bw_khz == 6);
        expect("picking AM there is sent", bridge.setDecodeMode(DSDCFG_MODE_AM) && live.drain() == 1);
        const QString refusal = uiMessage(live);
        expect("and refused: the decoder stays digital, the squelch exactly off",
               opts->analog_only == 0 && squelchIs(opts, DSD_SQUELCH_MODE_LEVEL, 0, 0.0));
        expect("with the refusal shown", refusal.startsWith(QStringLiteral("Failed: AM")));
        if (!refusal.startsWith(QStringLiteral("Failed: AM"))) {
            DSD_FPRINTF(stderr, "  ui_msg=%s\n", qPrintable(refusal));
        }
    }
#endif
    {
        // A squelch set elsewhere (the control API, a config) is explicit: no switch replaces it, an Off or the
        // default's own value as much as a level.
        const struct {
            int mode;
            int margin;
            double db;
            int first;
            int second;
        } outside[] = {
            {DSD_SQUELCH_MODE_LEVEL, 0, -70.0, DSDCFG_MODE_ANALOG, DSDCFG_MODE_P25P1},
            {DSD_SQUELCH_MODE_LEVEL, 0, 0.0, DSDCFG_MODE_ANALOG, DSDCFG_MODE_AM},
            {DSD_SQUELCH_MODE_NOISE, 10, 0.0, DSDCFG_MODE_P25P1, DSDCFG_MODE_ANALOG},
            {DSD_SQUELCH_MODE_NOISE, 14, 0.0, DSDCFG_MODE_P25P1, DSDCFG_MODE_DMR},
        };

        for (const auto& c : outside) {
            bool ok = false;
            LiveStart live(prefs, explore, QStringLiteral("explore"), &ok);
            dsd_opts* opts = live.decoder.opts.get();
            expect("an Explore start parses again", ok);
            dsd_app_squelch_setting_payload api = {};
            api.mode = c.mode;
            api.margin_db = c.margin;
            api.level = c.db < 0.0 ? dsd_squelch_level_from_sql(c.db) : 0.0;
            expect("a squelch from outside the app applies",
                   dsd_app_command_submit(DSD_APP_CMD_RTL_SET_SQL_SETTING, &api, sizeof api) >= 0 && live.drain() == 1
                       && squelchIs(opts, c.mode, c.margin, c.db));
            expect("and no switch replaces it", bridge.setDecodeMode(c.first) && live.drain() == 1
                                                    && squelchIs(opts, c.mode, c.margin, c.db)
                                                    && bridge.setDecodeMode(c.second) && live.drain() == 1
                                                    && squelchIs(opts, c.mode, c.margin, c.db));
        }
    }
    {
        prefs.setSquelch(QStringLiteral("auto+12"));
        QVariantMap nfm = explore;
        nfm.insert(QStringLiteral("decodeFlag"), QStringLiteral("-fA"));
        bool ok = false;
        LiveStart live(prefs, nfm, QStringLiteral("saved"), &ok);
        dsd_opts* opts = live.decoder.opts.get();
        expect("an NFM start parses", ok);
        expect("and starts with the Settings default", squelchIs(opts, DSD_SQUELCH_MODE_AUTO, 12, 0.0));
        expect("a switch to AM keeps it", bridge.setDecodeMode(DSDCFG_MODE_AM) && live.drain() == 1
                                              && squelchIs(opts, DSD_SQUELCH_MODE_AUTO, 12, 0.0));
        expect("a switch to P25 takes it out", bridge.setDecodeMode(DSDCFG_MODE_P25P1) && live.drain() == 1
                                                   && squelchIs(opts, DSD_SQUELCH_MODE_LEVEL, 0, 0.0));
        prefs.setSquelch(QStringLiteral("noise+10"));
    }
    for (const auto& own : {QVariantMap{{QStringLiteral("radioSettings"),
                                         QVariantMap{{QStringLiteral("squelch"), QStringLiteral("-60")}}}},
                            QVariantMap{{QStringLiteral("extraArgs"), QStringLiteral("--squelch -60")}}}) {
        QVariantMap system = explore;
        system.insert(own);
        bool ok = false;
        LiveStart live(prefs, system, QStringLiteral("saved"), &ok);
        dsd_opts* opts = live.decoder.opts.get();
        expect("a start with its own squelch parses", ok && squelchIs(opts, DSD_SQUELCH_MODE_LEVEL, 0, -60.0));
        expect("no switch touches it", bridge.setDecodeMode(DSDCFG_MODE_ANALOG) && live.drain() == 1
                                           && bridge.setDecodeMode(DSDCFG_MODE_P25P1) && live.drain() == 1
                                           && squelchIs(opts, DSD_SQUELCH_MODE_LEVEL, 0, -60.0));
    }
}

/* Issue #899: the decoder owns where the default stands, so a UI recreated over a running session (Android's Activity,
 * the decoder in its service) needs nothing to take back: a new bridge's switch brings it in, and after anyone's edit no
 * bridge's switch ever does. */
void
test_reattached_decode_switch_squelch(dsd_qt::AppPrefs& prefs) {
    const QVariantMap explore{{QStringLiteral("sourceType"), QStringLiteral("usb")},
                              {QStringLiteral("freqMhz"), QStringLiteral("851.375")}};
    bool ok = false;
    LiveStart live(prefs, explore, QStringLiteral("explore"), &ok);
    dsd_opts* opts = live.decoder.opts.get();
    expect("an Explore start parses for the recreated UI", ok);
    {
        dsd_qt::CommandBridge recreated;
        expect("a recreated UI's switch to NFM brings the default in",
               recreated.setDecodeMode(DSDCFG_MODE_ANALOG) && live.drain() == 1
                   && squelchIs(opts, DSD_SQUELCH_MODE_NOISE, 10, 0.0));
        expect("and back to P25 takes it out", recreated.setDecodeMode(DSDCFG_MODE_P25P1) && live.drain() == 1
                                                   && squelchIs(opts, DSD_SQUELCH_MODE_LEVEL, 0, 0.0));
    }
    {
        dsd_qt::CommandBridge first;
        expect("a user edit", first.setSquelchNoise(18) && live.drain() == 1);
    }
    dsd_qt::CommandBridge recreated;
    expect("ends it for every later bridge", recreated.setDecodeMode(DSDCFG_MODE_P25P1) && live.drain() == 1
                                                 && recreated.setDecodeMode(DSDCFG_MODE_ANALOG) && live.drain() == 1
                                                 && squelchIs(opts, DSD_SQUELCH_MODE_NOISE, 18, 0.0));
}

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

    expect("the analog pause hold while Record calls is off sends nothing", bridge.setAnalogRecordMaxGap(3000));
    expect("nothing queued for it", live.drain() == 0 && opts->analog_record_max_gap_ms == 200);
    expect("the NFM pre-roll while Record calls is off sends nothing", bridge.setAnalogRecordPreroll(500));
    expect("nothing queued for the pre-roll",
           live.drain() == 0 && opts->analog_record_preroll_ms == DSD_ANALOG_RECORD_PREROLL_DEFAULT_MS);

    expect("Record calls on, unknown-only on, is queued", bridge.setRecordCalls(true, true, 1500, 300));
    status.refresh();
    expect("and shows applying", status.commandState() == "pending" && status.command() == "start");
    expect("the pause hold, the pre-roll, unknown-only, then the start, apply", live.drain() == 4);
    status.refresh();
    expect("the session now records each call, unknown talkgroups only",
           opts->dmr_stereo_wav == 1 && opts->wav_out_f != nullptr && opts->record_unknown_only == 1 && status.perCall()
               && status.unknownOnly() && status.commandState() == "applied" && status.refusal().isEmpty());
    expect("with the pause hold sent with it",
           opts->analog_record_max_gap_ms == 1500 && status.analogMaxGapMs() == 1500);
    expect("and the pre-roll", opts->analog_record_preroll_ms == 300 && status.analogPrerollMs() == 300);
    expect("into the session's folder", slotFilesIn(sessions.folder) > 0);

    expect("unknown-only off while recording is queued", bridge.setRecordUnknownOnly(false));
    expect("and applied", live.drain() == 1 && opts->record_unknown_only == 0);

    // Issue #756: the analog pause hold while recording goes live, reads as itself, and a value the decoder refuses
    // fails without changing the session's.
    expect("the pause hold while recording is queued", bridge.setAnalogRecordMaxGap(10000));
    status.refresh();
    expect("and shows as itself, applying",
           status.commandState() == "pending" && status.command() == QStringLiteral("analogMaxGap"));
    expect("it applies", live.drain() == 1 && opts->analog_record_max_gap_ms == 10000);
    status.refresh();
    expect("and shows applied", status.commandState() == "applied" && status.analogMaxGapMs() == 10000);
    expect("one past the range is queued", bridge.setAnalogRecordMaxGap(10001));
    expect("and refused by the decoder", live.drain() == 1 && opts->analog_record_max_gap_ms == 10000);
    status.refresh();
    expect("which the status shows", status.commandState() == "failed" && status.command() == "analogMaxGap");
    expect("off is queued", bridge.setAnalogRecordMaxGap(200) && live.drain() == 1);
    status.refresh();
    expect("and applies", opts->analog_record_max_gap_ms == 200 && status.analogMaxGapMs() == 200);

    // Issue #668: the NFM pre-roll the same way.
    expect("the pre-roll while recording is queued", bridge.setAnalogRecordPreroll(1000));
    status.refresh();
    expect("and shows as itself, applying",
           status.commandState() == "pending" && status.command() == QStringLiteral("analogPreroll"));
    expect("it applies", live.drain() == 1 && opts->analog_record_preroll_ms == 1000);
    status.refresh();
    expect("and shows applied", status.commandState() == "applied" && status.analogPrerollMs() == 1000);
    expect("one past its range is queued", bridge.setAnalogRecordPreroll(1001));
    expect("and refused by the decoder", live.drain() == 1 && opts->analog_record_preroll_ms == 1000);
    status.refresh();
    expect("which the status shows", status.commandState() == "failed" && status.command() == "analogPreroll");
    expect("off is queued", bridge.setAnalogRecordPreroll(0) && live.drain() == 1);
    status.refresh();
    expect("and applies", opts->analog_record_preroll_ms == 0 && status.analogPrerollMs() == 0);

    expect("Record calls off is queued", bridge.setRecordCalls(false, true, 200, 0));
    status.refresh();
    // The status is about the switch the user made, the stop, not the unknown-only clear sent after it.
    expect("and shows the stop applying", status.commandState() == "pending" && status.command() == "stop");
    expect("unknown-only off and the stop apply", live.drain() == 2);
    status.refresh();
    expect("the session stops recording", opts->record_unknown_only == 0 && opts->dmr_stereo_wav == 0
                                              && opts->wav_out_f == nullptr && opts->wav_out_fR == nullptr
                                              && !status.perCall() && !status.unknownOnly());
    expect("the pause hold and the pre-roll are left as they were",
           opts->analog_record_max_gap_ms == 200 && opts->analog_record_preroll_ms == 0);
    expect("and shows the stop applied", status.commandState() == "applied" && status.command() == "stop");

    // Unknown-only sent later by something else (the TCP API, say) reads as itself, not as the stop.
    expect("a later unknown-only is queued",
           dsd_app_command_set_i32(DSD_APP_CMD_RECORD_UNKNOWN_ONLY_SET, 1) == DSD_APP_COMMAND_SUBMIT_QUEUED);
    status.refresh();
    expect("and shows as itself", status.commandState() == "pending" && status.command() == "unknownOnly");
    expect("it applies", live.drain() == 1);
    status.refresh();
    expect("and reads as itself once applied", status.command() == "unknownOnly");
}

/* A session started with Record calls on, whose first live switch is unknown-only (D8): the status shows it applying,
 * then applied, though no Record calls switch was sent in the session before it. */
void
test_live_unknown_only_first(dsd_qt::AppPrefs& prefs, Sessions& sessions) {
    prefs.setRecordCalls(true);
    bool started = false;
    LiveSession live(prefs, sessions, &started);
    expect("a session recording each call starts", started && live.decoder.opts->dmr_stereo_wav == 1);
    dsd_qt::CommandBridge bridge;
    dsd_qt::RecordingStatus status;
    bridge.setRecordingStatus(&status);
    status.setSessionActive(true);
    status.setRunUp(true);
    status.refresh();
    expect("the status has nothing sent yet", status.perCall() && status.commandState() == "none");
    expect("unknown-only on is queued", bridge.setRecordUnknownOnly(true));
    status.refresh();
    expect("and shows applying", status.commandState() == "pending" && status.command() == "unknownOnly");
    expect("it applies", live.drain() == 1 && live.decoder.opts->record_unknown_only == 1);
    status.refresh();
    expect("and shows applied", status.commandState() == "applied" && status.unknownOnly());
    prefs.setRecordCalls(false);
}

/* Issue #756: with the analog pause hold in the extra options, Settings sends no hold with Record calls on (a negative
 * one), so the session keeps the hold its extra options set; issue #668: the NFM pre-roll the same way. */
void
test_live_record_on_keeps_extra_hold(dsd_qt::AppPrefs& prefs, Sessions& sessions) {
    prefs.setExtraArgs(QStringLiteral("--analog-record-max-gap-ms 4000 --analog-record-preroll-ms 700"));
    prefs.setAnalogMaxGapMs(2000);
    prefs.setAnalogPrerollMs(300);
    bool started = false;
    LiveSession live(prefs, sessions, &started);
    const dsd_opts* opts = live.decoder.opts.get();
    expect("a session with an extra hold and pre-roll starts",
           started && opts->analog_record_max_gap_ms == 4000 && opts->analog_record_preroll_ms == 700);
    dsd_qt::CommandBridge bridge;
    dsd_qt::RecordingStatus status;
    bridge.setRecordingStatus(&status);
    status.setSessionActive(true);
    status.setRunUp(true);
    expect("Record calls on with no hold and no pre-roll is queued", bridge.setRecordCalls(true, false, -1, -1));
    expect("only unknown-only and the start apply", live.drain() == 2);
    expect("the session records with the extra hold and pre-roll", opts->dmr_stereo_wav == 1
                                                                       && opts->analog_record_max_gap_ms == 4000
                                                                       && opts->analog_record_preroll_ms == 700);
    prefs.setExtraArgs(QString());
    prefs.setAnalogMaxGapMs(200);
    prefs.setAnalogPrerollMs(DSD_ANALOG_RECORD_PREROLL_DEFAULT_MS);
}

/* Settings' bridge and status on a session whose run is up. */
struct LiveBridge {
    dsd_qt::CommandBridge bridge;
    dsd_qt::RecordingStatus status;

    LiveBridge() {
        bridge.setRecordingStatus(&status);
        status.setSessionActive(true);
        status.setRunUp(true);
    }
};

/* A setting changed twice while Record calls on still waits in the queue (a carrier holds it; nothing drains): both
 * changes go, so the session ends at the last one. The first change is then the view's latest command, not the start,
 * and the second was lost for the analog pause hold (issue #756) and Only unknown talkgroups (issue #708) alike. The NFM
 * pre-roll (issue #668) goes the same way behind them. */
void
test_live_settings_behind_a_queued_start(dsd_qt::AppPrefs& prefs, Sessions& sessions) {
    {
        bool started = false;
        LiveSession live(prefs, sessions, &started);
        const dsd_opts* opts = live.decoder.opts.get();
        expect("a session recording nothing starts", started && opts->dmr_stereo_wav == 0);
        LiveBridge live_bridge;
        expect("Record calls on is queued", live_bridge.bridge.setRecordCalls(true, false, 200, 0));
        expect("a pause hold behind it is sent", live_bridge.bridge.setAnalogRecordMaxGap(2000));
        expect("and so is a second one", live_bridge.bridge.setAnalogRecordMaxGap(3000));
        live_bridge.status.refresh();
        expect("the status shows the hold applying",
               live_bridge.status.commandState() == "pending" && live_bridge.status.command() == "analogMaxGap");
        expect("a pre-roll behind them is sent too", live_bridge.bridge.setAnalogRecordPreroll(300));
        live_bridge.status.refresh();
        expect("the status shows the pre-roll applying",
               live_bridge.status.commandState() == "pending" && live_bridge.status.command() == "analogPreroll");
        (void)live.drain();
        live_bridge.status.refresh();
        expect("the session records with the second hold and the pre-roll",
               opts->dmr_stereo_wav == 1 && opts->analog_record_max_gap_ms == 3000
                   && opts->analog_record_preroll_ms == 300);
        expect("which the status shows applied", live_bridge.status.commandState() == "applied"
                                                     && live_bridge.status.analogMaxGapMs() == 3000
                                                     && live_bridge.status.analogPrerollMs() == 300);
    }
    {
        bool started = false;
        LiveSession live(prefs, sessions, &started);
        const dsd_opts* opts = live.decoder.opts.get();
        const LiveBridge live_bridge;
        expect("Record calls on, every call, is queued",
               started && live_bridge.bridge.setRecordCalls(true, false, 200, 0));
        expect("unknown-only on behind it is sent", live_bridge.bridge.setRecordUnknownOnly(true));
        expect("and off again", live_bridge.bridge.setRecordUnknownOnly(false));
        (void)live.drain();
        expect("the session records every call", opts->dmr_stereo_wav == 1 && opts->record_unknown_only == 0);
    }
}

/* A setting changed while Record calls off still waits in the queue sends nothing: the session will not record, and
 * the next Record calls on sends the setting. A start queued behind the stop counts again. */
void
test_live_settings_behind_a_queued_stop(dsd_qt::AppPrefs& prefs, Sessions& sessions) {
    prefs.setRecordCalls(true);
    {
        bool started = false;
        LiveSession live(prefs, sessions, &started);
        const dsd_opts* opts = live.decoder.opts.get();
        expect("a session recording each call starts", started && opts->dmr_stereo_wav == 1);
        const LiveBridge live_bridge;
        expect("Record calls off is queued", live_bridge.bridge.setRecordCalls(false, false, 200, 0));
        const int preroll = opts->analog_record_preroll_ms;
        expect("a pause hold behind it sends nothing", live_bridge.bridge.setAnalogRecordMaxGap(3000));
        expect("nor does a pre-roll", live_bridge.bridge.setAnalogRecordPreroll(preroll == 300 ? 400 : 300));
        expect("nor does unknown-only", live_bridge.bridge.setRecordUnknownOnly(true));
        expect("only the stop and its unknown-only clear apply", live.drain() == 2);
        expect("the session stops, its settings as they were",
               opts->dmr_stereo_wav == 0 && opts->analog_record_max_gap_ms == 200
                   && opts->analog_record_preroll_ms == preroll && opts->record_unknown_only == 0);

        expect("Record calls off and on again are queued",
               live_bridge.bridge.setRecordCalls(false, false, 200, 0)
                   && live_bridge.bridge.setRecordCalls(true, false, 200, 0));
        expect("a pause hold behind the start is sent", live_bridge.bridge.setAnalogRecordMaxGap(3000));
        expect("and a pre-roll", live_bridge.bridge.setAnalogRecordPreroll(500));
        (void)live.drain();
        expect("the session records with them", opts->dmr_stereo_wav == 1 && opts->analog_record_max_gap_ms == 3000
                                                    && opts->analog_record_preroll_ms == 500);
    }
    prefs.setRecordCalls(false);
}

/* Record calls on that the session's end drops: a setting changed after it sends nothing, as the session records
 * nothing. */
void
test_live_setting_after_a_dropped_start(dsd_qt::AppPrefs& prefs, Sessions& sessions) {
    bool started = false;
    LiveSession live(prefs, sessions, &started);
    const dsd_opts* opts = live.decoder.opts.get();
    LiveBridge live_bridge;
    expect("Record calls on is queued", started && live_bridge.bridge.setRecordCalls(true, false, 200, 0));
    // The run ends and the next comes up: its stop drops what was queued.
    dsd_app_frontend_runtime_stop();
    dsd_app_frontend_runtime_start(live.decoder.opts.get(), live.decoder.state.get());
    live_bridge.status.refresh();
    expect("the start reads as dropped", live_bridge.status.commandState() == "dropped");
    const int preroll = opts->analog_record_preroll_ms;
    expect("a pause hold after it sends nothing", live_bridge.bridge.setAnalogRecordMaxGap(3000));
    expect("a pre-roll neither", live_bridge.bridge.setAnalogRecordPreroll(preroll == 300 ? 400 : 300));
    expect("unknown-only neither", live_bridge.bridge.setRecordUnknownOnly(true));
    expect("nothing is queued", live.drain() == 0);
    expect("the session records nothing, its settings as they were",
           opts->dmr_stereo_wav == 0 && opts->analog_record_max_gap_ms == 200
               && opts->analog_record_preroll_ms == preroll && opts->record_unknown_only == 0);
}

#ifdef DSD_NEO_TEST_SUBMIT_WRAP
/* Record calls off where one of its two submissions is refused: the session never records more than it did. A refused
 * stop sends nothing (unknown-only stays on); a refused unknown-only after the stop leaves the session stopped. */
void
test_live_record_off_half_sent(dsd_qt::AppPrefs& prefs, Sessions& sessions) {
    prefs.setRecordCalls(true);
    prefs.setRecordUnknownOnly(true);
    for (const int refused :
         {static_cast<int>(DSD_APP_CMD_WAV_STOP), static_cast<int>(DSD_APP_CMD_RECORD_UNKNOWN_ONLY_SET)}) {
        const bool stop_refused = refused == DSD_APP_CMD_WAV_STOP;
        bool started = false;
        LiveSession live(prefs, sessions, &started);
        const dsd_opts* opts = live.decoder.opts.get();
        expect("a session recording unknown talkgroups starts",
               started && opts->dmr_stereo_wav == 1 && opts->record_unknown_only == 1);
        dsd_qt::CommandBridge bridge;
        dsd_qt::RecordingStatus status;
        bridge.setRecordingStatus(&status);
        status.setSessionActive(true);
        status.setRunUp(true);
        g_refuse_cmd = refused;
        const bool sent = bridge.setRecordCalls(false, true, 200, 0);
        g_refuse_cmd = 0;
        (void)live.drain();
        if (stop_refused) {
            expect("a refused stop reads as not sent", !sent);
            expect("and leaves the session as it was, unknown talkgroups only",
                   opts->dmr_stereo_wav == 1 && opts->record_unknown_only == 1);
        } else {
            expect("the stop went in", sent);
            expect("and the session stops recording", opts->dmr_stereo_wav == 0);
        }
    }
    prefs.setRecordCalls(false);
    prefs.setRecordUnknownOnly(false);
}

/* Record calls on where one of its four submissions is refused (issues #756, #668): the pause hold goes first, so a
 * refused hold sends nothing after it and the session records nothing; a refused pre-roll sends nothing after it
 * either; a refused start leaves the hold, the pre-roll and unknown-only sent, which change nothing while no call
 * records. */
void
test_live_record_on_half_sent(dsd_qt::AppPrefs& prefs, Sessions& sessions) {
    for (const int refused : {
             static_cast<int>(DSD_APP_CMD_ANALOG_RECORD_MAX_GAP_SET),
             static_cast<int>(DSD_APP_CMD_ANALOG_RECORD_PREROLL_SET),
             static_cast<int>(DSD_APP_CMD_WAV_START),
         }) {
        const bool hold_refused = refused == DSD_APP_CMD_ANALOG_RECORD_MAX_GAP_SET;
        const bool preroll_refused = refused == DSD_APP_CMD_ANALOG_RECORD_PREROLL_SET;
        bool started = false;
        LiveSession live(prefs, sessions, &started);
        const dsd_opts* opts = live.decoder.opts.get();
        expect("a session recording nothing starts", started && opts->dmr_stereo_wav == 0);
        dsd_qt::CommandBridge bridge;
        dsd_qt::RecordingStatus status;
        bridge.setRecordingStatus(&status);
        status.setSessionActive(true);
        status.setRunUp(true);
        g_refuse_cmd = refused;
        const bool sent = bridge.setRecordCalls(true, true, 3000, 400);
        g_refuse_cmd = 0;
        // With no start on its way, a pause hold changed while the rest still waits in the queue sends nothing.
        expect("a pause hold after it answers", bridge.setAnalogRecordMaxGap(5000));
        const int drained = live.drain();
        expect("a half-sent start reads as not sent", !sent && opts->dmr_stereo_wav == 0);
        const int preroll_before = DSD_ANALOG_RECORD_PREROLL_DEFAULT_MS;
        if (hold_refused) {
            expect("a refused hold sends nothing after it", drained == 0 && opts->analog_record_max_gap_ms == 200
                                                                && opts->analog_record_preroll_ms == preroll_before
                                                                && opts->record_unknown_only == 0);
        } else if (preroll_refused) {
            expect("a refused pre-roll sends nothing after it, the hold before it sent",
                   drained == 1 && opts->analog_record_max_gap_ms == 3000
                       && opts->analog_record_preroll_ms == preroll_before && opts->record_unknown_only == 0);
        } else {
            expect("a refused start leaves the hold, the pre-roll and unknown-only sent, and no later hold",
                   drained == 3 && opts->analog_record_max_gap_ms == 3000 && opts->analog_record_preroll_ms == 400
                       && opts->record_unknown_only == 1);
        }
    }
}
#endif

/* A switch while the process's first session is still starting: the decoder has published no recording view yet, and
 * the refusal says the session is starting, not that none runs. Runs before any session publishes a view. */
void
test_first_session_starting() {
    dsd_qt::CommandBridge bridge;
    dsd_qt::RecordingStatus status;
    bridge.setRecordingStatus(&status);
    status.setSessionActive(true);
    dsd_app_recording_view view{};
    expect("no view published yet", dsd_app_recording_view_get(&view) == 0);
    expect("Record calls on is refused", !bridge.setRecordCalls(true, false, 200, 0));
    expect("as a session still starting", status.refusal() == QStringLiteral("starting"));
    expect("unknown-only too", !bridge.setRecordUnknownOnly(true) && status.refusal() == QStringLiteral("starting"));
    status.setSessionActive(false);
    expect("with no session it is refused", !bridge.setRecordCalls(true, false, 200, 0));
    expect("as no session", status.refusal() == QStringLiteral("no-session"));
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
    expect("Record calls on is refused", !bridge.setRecordCalls(true, false, 200, 0));
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
    expect("Record calls on is refused", !bridge.setRecordCalls(true, false, 200, 0));
    status.refresh();
    expect("nothing is sent and the static WAV stays open",
           live.drain() == 0 && live.decoder.opts->static_wav_file == 1 && live.decoder.opts->wav_out_f != nullptr);
    expect("the status says why", status.refusal() == QStringLiteral("static-wav") && status.staticWav());
    const int refused = status.refusalCount();
    // Nor is Record calls off sent: a stop would close the user's file as surely as a start.
    expect("Record calls off is refused too", !bridge.setRecordCalls(false, false, 200, 0));
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
        expect("while Starting a switch is refused", !bridge.setRecordCalls(false, false, 200, 0));
        expect("as starting, sending nothing", status.refusal() == QStringLiteral("starting") && live.drain() == 0);
        host.setPhase(dsd_qt::DecoderHost::Running);
        expect("Running, it is sent", bridge.setRecordCalls(false, false, 200, 0) && live.drain() == 2);
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
        expect("a run said up before Running is up", bridge.setRecordCalls(false, false, 200, 0) && live.drain() == 2);
        host.setPhase(dsd_qt::DecoderHost::Idle);
        host.setPhase(dsd_qt::DecoderHost::Running);
        expect("the next session waits for its own", !bridge.setRecordUnknownOnly(true)
                                                         && status.refusal() == QStringLiteral("starting")
                                                         && live.drain() == 0);
        host.initialized();
        expect("until it says so", bridge.setRecordCalls(false, false, 200, 0) && live.drain() == 2);
    }
    {
        PhaseHost host;
        host.signalsInit = true;
        host.known = false;
        dsd_qt::RecordingStatus status;
        bridge.setRecordingStatus(&status);
        dsd_qt::recording_status_follow_host(&status, &host);
        host.firstPoll(dsd_qt::DecoderHost::Running);
        expect("a session the UI found running is up",
               bridge.setRecordCalls(false, false, 200, 0) && live.drain() == 2);
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

    expect("with no session, Record calls is refused", !bridge.setRecordCalls(true, false, 200, 0));
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

    expect("Record calls on with no storage is refused", !bridge.setRecordCalls(true, false, 200, 0));
    expect("as such", status.refusal() == QStringLiteral("storage-unavailable"));
    host.setPhase(dsd_qt::DecoderHost::Idle);
    expect("an end clears it", status.refusal().isEmpty() && !status.sessionActive());

    host.setPhase(dsd_qt::DecoderHost::Running);
    expect("Record calls off is sent", bridge.setRecordCalls(false, false, 200, 0));
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
    // Issue #756: the analog pause hold alone moving is a move too, so Settings follows a hold another frontend set.
    view.analog_max_gap_ms = 2000;
    status.apply(view);
    expect("the pause hold moving is a change", changes == 5 && status.analogMaxGapMs() == 2000);
    // Issue #668: and so is the NFM pre-roll.
    view.analog_preroll_ms = 700;
    status.apply(view);
    expect("the pre-roll moving is a change", changes == 6 && status.analogPrerollMs() == 700);
}

enum class Applied { Journal, DeleteAfterUpload, Folder, StaticWav, PerCall, SystemByFrequency };

/* Whether the parse in @p opts applied the recording option @p what, with its value under @p scratch. */
bool
applied(Applied what, const dsd_opts& opts, const QString& scratch) {
    switch (what) {
        case Applied::Journal: return QString::fromUtf8(opts.wav_journal_file) == scratch + QStringLiteral("/j.jsonl");
        case Applied::DeleteAfterUpload: return opts.rdio_api_delete_after_upload != 0;
        case Applied::Folder: return QString::fromUtf8(opts.wav_out_dir) == scratch;
        case Applied::StaticWav: return opts.static_wav_file != 0;
        case Applied::PerCall: return opts.dmr_stereo_wav != 0;
        case Applied::SystemByFrequency: return opts.rdio_system_by_frequency != 0;
    }
    return false;
}

/*
 * The call recording options the app owns are refused wherever the parser would honour them, and only there: the
 * parser takes its long options out of the whole command line first (a short option's value position included), and
 * getopt then reads what is left. Each case is parsed as the decoder parses it, to show what it honours, and then
 * judged by session_args_recording_extra_error(). %1 is a scratch folder (-7 makes it, -w writes there).
 */
void
test_extras_judged_as_the_parser_reads_them(const QString& scratch) {
    using dsd_qt::session_args_recording_extra_error;
    using dsd_qt::SessionArgsError;

    struct Case {
        const char* extras;
        bool honoured; // the parser applies the recording option
        SessionArgsError refusal;
        bool recordCalls;
        bool noStorage;
        Applied what;
    };

    const Case cases[] = {
        // A long option where a short option's value would be: the parser takes it out first and honours it.
        {"-o --wav-journal %1/j.jsonl null", true, SessionArgsError::CallRecordingOption, false, false,
         Applied::Journal},
        {"-o --rdio-api-delete-after-upload null", true, SessionArgsError::CallRecordingOption, false, false,
         Applied::DeleteAfterUpload},
        // Once it is out, the short options after it pair up anew: -7, -w and -P are options again.
        {"-o --strict-crc -o -7 %1", true, SessionArgsError::CallRecordingOption, false, false, Applied::Folder},
        {"-o --strict-crc -o -w %1/out.wav", true, SessionArgsError::CallRecordingStaticWav, true, false,
         Applied::StaticWav},
        {"-o --strict-crc -o -P", true, SessionArgsError::CallRecordingNoStorage, false, true, Applied::PerCall},
        // And the other way: what becomes a short option's value is no option.
        {"-o --strict-crc -7 %1", false, SessionArgsError::None, false, false, Applied::Folder},
        {"-o --strict-crc -w %1/out.wav", false, SessionArgsError::None, true, false, Applied::StaticWav},
        {"-o --strict-crc -P", false, SessionArgsError::None, false, true, Applied::PerCall},
        // A "--" that is a value ends nothing: getopt reads on after it, as the long-option pass does.
        {"-o -- -7 %1 -o null", true, SessionArgsError::CallRecordingOption, false, false, Applied::Folder},
        {"--key-profile-ref -- -7 %1", true, SessionArgsError::CallRecordingOption, false, false, Applied::Folder},
        // One that is no value ends the options: what follows is an operand.
        {"-o null -- -7 %1", false, SessionArgsError::None, false, false, Applied::Folder},
        {"-o -- -- -7 %1", false, SessionArgsError::None, false, false, Applied::Folder},
        {"-- -w %1/out.wav", false, SessionArgsError::None, true, false, Applied::StaticWav},
        // Issue #931: the per-frequency rdio-scanner system switches take no value. They pass through, the last one
        // given winning, and the option after either is still an option.
        {"-o null --rdio-system-by-frequency", true, SessionArgsError::None, false, false, Applied::SystemByFrequency},
        {"--rdio-system-by-frequency --no-rdio-system-by-frequency -o null", false, SessionArgsError::None, false,
         false, Applied::SystemByFrequency},
        {"--rdio-system-by-frequency -7 %1", true, SessionArgsError::CallRecordingOption, false, false,
         Applied::Folder},
        {"--no-rdio-system-by-frequency -7 %1", true, SessionArgsError::CallRecordingOption, false, false,
         Applied::Folder},
    };
    for (const Case& c : cases) {
        const QString extras = QString::fromLatin1(c.extras).arg(scratch);
        Decoder decoder;
        const bool parsed = parse(extras.split(QLatin1Char(' '), Qt::SkipEmptyParts), decoder);
        expect(qPrintable(QStringLiteral("the parser %1: %2")
                              .arg(c.honoured ? QStringLiteral("honours") : QStringLiteral("ignores"), extras)),
               parsed && applied(c.what, *decoder.opts, scratch) == c.honoured);
        expect(qPrintable(QStringLiteral("%1: %2").arg(
                   c.honoured ? QStringLiteral("refused") : QStringLiteral("accepted"), extras)),
               session_args_recording_extra_error(extras, c.recordCalls, c.noStorage) == c.refusal);
    }
}

/*
 * Every short option that takes a value, by the parser's own option string, with a refused option where its value
 * goes: as its value the option is none (-L -7 <dir>); after a "--" that is the value, it is one again
 * (-L -- -7 <dir>); after a second "--", the terminator, it is an operand (-L -- -- -7 <dir>). Each is parsed as the
 * decoder parses it, in a scratch working folder (some options make files there), and then judged. A parse that fails
 * at the value before the refused option is reached applies nothing; the refusal stands there too.
 */
void
test_refused_option_after_each_value_option(const QString& scratch) {
    using dsd_qt::session_args_recording_extra_error;
    using dsd_qt::SessionArgsError;

    const QTemporaryDir cwd;
    const QString was = QDir::currentPath();
    expect("matrix: a scratch working folder", cwd.isValid() && QDir::setCurrent(cwd.path()));
    int letters = 0;
    int honoured = 0;
    for (const char* at = dsd_cli_short_options(); *at != '\0'; ++at) {
        const char letter = *at;
        if (letter == ':' || letter == '7' || dsd_cli_short_option_takes_value(letter) == 0) {
            continue;
        }
        letters++;
        const QString option = QStringLiteral("-") + QLatin1Char(letter);
        const QString asValue = QStringLiteral("%1 -7 %2").arg(option, scratch);
        const QString afterValue = QStringLiteral("%1 -- -7 %2").arg(option, scratch);
        const QString afterEnd = QStringLiteral("%1 -- -- -7 %2").arg(option, scratch);

        Decoder valueDecoder;
        (void)parse(asValue.split(QLatin1Char(' ')), valueDecoder);
        expect(qPrintable(QStringLiteral("matrix: the parser ignores: %1").arg(asValue)),
               !applied(Applied::Folder, *valueDecoder.opts, scratch));
        expect(qPrintable(QStringLiteral("matrix: accepted: %1").arg(asValue)),
               session_args_recording_extra_error(asValue, false) == SessionArgsError::None);

        Decoder afterDecoder;
        const bool parsed = parse(afterValue.split(QLatin1Char(' ')), afterDecoder);
        const bool honours = applied(Applied::Folder, *afterDecoder.opts, scratch);
        honoured += honours ? 1 : 0;
        expect(qPrintable(QStringLiteral("matrix: the parser honours, or fails before it: %1").arg(afterValue)),
               honours || !parsed);
        expect(qPrintable(QStringLiteral("matrix: refused: %1").arg(afterValue)),
               session_args_recording_extra_error(afterValue, false) == SessionArgsError::CallRecordingOption);

        Decoder endDecoder;
        (void)parse(afterEnd.split(QLatin1Char(' ')), endDecoder);
        expect(qPrintable(QStringLiteral("matrix: the parser ignores: %1").arg(afterEnd)),
               !applied(Applied::Folder, *endDecoder.opts, scratch));
        expect(qPrintable(QStringLiteral("matrix: accepted: %1").arg(afterEnd)),
               session_args_recording_extra_error(afterEnd, false) == SessionArgsError::None);
    }
    expect("matrix: every value option was tried", letters >= 30);
    // Those whose value is free text (-i, -o, -w, file names); the others refuse "--" and fail the parse there.
    expect("matrix: a \"--\" value lets the option after it apply", honoured > 0);
    expect("matrix: back in the working folder", QDir::setCurrent(was));
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
        test_first_session_starting();
        test_saved_system(prefs, sessions);
        test_explore(prefs, sessions);
        test_scan_list(prefs, sessions);
        test_scan_list_rows_inherit(prefs, sessions);
        test_scan_list_switches_over_a_config(prefs, sessions, dir.filePath(QStringLiteral("config")));
        test_status_readings();
        test_live_commands(prefs, sessions);
        test_live_decode_switch_squelch(prefs);
        test_restore_level_says_what_it_asked(prefs);
        test_every_squelch_edit_is_tagged(prefs);
        test_reattached_decode_switch_squelch(prefs);
        test_live_unknown_only_first(prefs, sessions);
        test_live_record_on_keeps_extra_hold(prefs, sessions);
        test_live_settings_behind_a_queued_start(prefs, sessions);
        test_live_settings_behind_a_queued_stop(prefs, sessions);
        test_live_setting_after_a_dropped_start(prefs, sessions);
#ifdef DSD_NEO_TEST_SUBMIT_WRAP
        test_live_record_off_half_sent(prefs, sessions);
        test_live_record_on_half_sent(prefs, sessions);
#endif
        test_live_without_storage(prefs);
        test_live_beside_a_static_wav(prefs, sessions, dir.filePath(QStringLiteral("static.wav")));
        test_status_follows_the_session(prefs);
        test_status_waits_for_the_run(prefs, sessions);
        test_extras_judged_as_the_parser_reads_them(dir.filePath(QStringLiteral("extras")));
        test_refused_option_after_each_value_option(dir.filePath(QStringLiteral("extras")));
    }
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("OK");
    return 0;
}
