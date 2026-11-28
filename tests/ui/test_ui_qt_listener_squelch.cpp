// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #899, end to end: what a listener hears from a session the app starts with its default squelch, which the app
 * passes as --squelch-analog-default and the decoder runs on each analog channel while nothing sets a squelch. Each
 * case builds the session's argv with the app's own builder (SessionArgsBuilder, or ScanListStarter for a scan list),
 * starts it through dsd_runtime_bootstrap() as the decoder host does, and runs the real engine over a committed
 * capture.
 *
 * The receiver is an I/Q replay, but the app's radio spec is kept: `--iq-replay` alone would put its own input in place
 * of the `-i rtl:...` spec. So the spec is read as the engine reads it when the radio opens
 * (dsd_rtl_input_spec_apply()); its squelch field (always 0, no squelch) and volume field stay in force, and its tuning
 * fields are the capture's own (851.375 MHz, 48 kHz), which the replay then keeps.
 *
 * The capture is made from the committed fixtures: 3 s of receiver noise (noise_floor), the 9.3 s NFM speech
 * transmission (speech_nfm_synth, a carrier from its first sample, the talker from 0.25 s), then 2 s of noise again.
 * The monitor's audio is captured where the analog replay host takes it (the UDP analog hook) and scored per block at
 * -40 dBFS RMS, on the stream time the decoder has read, as tests/engine/analog_replay.c does.
 *
 * Cases (one per process, argv[1]):
 *   saved-nfm         an NFM saved system with no squelch of its own: the decoder runs noise+10; the idle channel
 *                     plays nothing, the transmission plays from its talker's start, nothing plays after it. With
 *                     per-call recording on (-P), the transmission is one recording and the idle channel none; the
 *                     speech level at the opening is the level later in the call (no AGC trained on muted noise).
 *   saved-nfm-off     the same with the Settings default Off: the idle channel plays its hiss (the control, which shows
 *                     the scoring hears hiss), and analog calls are paused (no recording).
 *   explore-live-nfm  Explore on Auto, then NFM picked 1 s in through the real command bridge and queue, and another
 *                     UI command 200 ms of stream time later: the switch carries the default with it (one command, the
 *                     receiver told before the channel), so the same as saved-nfm once switched.
 *   scan-row          a scan list's NFM row as ScanListStarter generates it (no squelch of its own in its options cell,
 *                     read by the row parser the scan loader uses and committed as the coordinator commits a row): the
 *                     decoder runs the default for it, so the same audio, and the scanners' hold signal
 *                     (dsd_scan_analog_carrier_open()) never up on the idle channel, up through the transmission and
 *                     down again after it.
 *   scan-row-off      the same list with the Settings default Off: the row runs no squelch and the idle channel holds
 *                     the scan (the control).
 *   opening-nfm       the NFM saved system over the opening capture (the transmission from its first sample, then 2 s
 *                     of noise): the receiver runs the resolved gate before its first block, so the opening plays from
 *                     its talker's start; nothing the engine starts between the stream and its main loop mutes it.
 *   opening-nfm-off   the same with the Settings default Off.
 *   second-session-nfm  opening-nfm-off with a receive request queued as the engine stops (the stopped stream never
 *                     takes it), then opening-nfm in the same process: that request cannot land on the new stream, so
 *                     the new session's start is no closed start (round 10).
 *
 * Nothing in the fixture sets the startup squelch: the squelch the channel runs is read at the lifecycle's start (the
 * engine's setup and its stream open done, before the decoder's first settle), where the receiver must already run it,
 * the setting its next block takes (rtl_stream_get_squelch_running()), and not a closed start; then, as
 * the receiver produced the samples the decoder's first read returned (its counts of closed-start and unjudged samples,
 * kept where it produces them), and apart from that, from the dynamic squelch's status at that read. So the production
 * startup path is what each case checks.
 *
 * Not covered here: the scan coordinator stepping between rows (an I/Q replay cannot retune; the rows' settings on air
 * through the real coordinator are ENGINE_TRUNK_SCAN_SCAN_LIST's), and the stream after a live switch back to a
 * digital decode (UI_QT_SESSION_ARGS_PARSE checks the squelch is then exactly off).
 */

#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QPair>
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
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/input_spec.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/squelch.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <memory>
#include <stdint.h>
#include <utility>
#include "../test_support/qt_test_paths.h"
#include "app_prefs.h"
#include "command_bridge.h"
#include "decoder_host.h"
#include "scan_list_starter.h"
#include "scan_lists_model.h"
#include "session_args.h"

#ifdef USE_RADIO
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/engine/scan_voice_gate.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/rtl_stream_fwd.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#endif

#ifdef USE_RADIO
namespace {

int g_failures = 0;

void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

/* The made capture, in stream milliseconds: the idle channel, the transmission, the idle channel again. */
constexpr double kIdleMs = 3000.0;
constexpr double kCarrierMs = 9300.0;
constexpr double kAfterMs = 2000.0;
constexpr double kCarrierEndMs = kIdleMs + kCarrierMs;
/* The squelch's closing tail and the monitor's latency after the carrier ends: not judged. */
constexpr double kCloseMs = 100.0;
constexpr double kAudibleDbfs = -40.0;
/* How far the speech level in the first 500 ms a call plays may sit from its level 2-6 s on: an AGC trained on the
   muted idle channel would start the call far off it. */
constexpr double kOnsetLevelDb = 6.0;
constexpr int kRateHz = 48000;

enum Segment { kIdle = 0, kCarrier = 1, kAfter = 2, kSegments = 3 };

/* What the listener heard, and what held a scanner, by stream time. */
struct Heard {
    double readMs = 0.0;
    /* Each audible block's level, with the stream time it was heard at: for the speech level over the call. */
    QList<QPair<double, double>> audibleDbfs;
    double audibleMs[kSegments] = {0.0, 0.0, 0.0};
    double firstAudibleMs = -1.0;
    double lastAudibleMs = -1.0;
    int holdSeen[kSegments] = {0, 0, 0};
    int holdAtEnd = 0;
    unsigned int blocks = 0U;
};

Heard g_heard;
const dsd_opts* g_opts = nullptr;
const dsd_state* g_state = nullptr;
/* The live decode switch: the bridge it goes through, and the stream time it is made at (< 0: none); a later UI
   command follows it 200 ms of stream time after. */
dsd_qt::CommandBridge* g_bridge = nullptr;
double g_switchAtMs = -1.0;
int g_switched = 0;
int g_followed = 0;
constexpr double kFollowMs = 200.0;

/* Where the transmission starts in the capture a case listens to: after the idle channel, or (the opening capture) at
   its first sample. */
double g_carrierStartMs = kIdleMs;

/* Which part of the capture @p ms falls in; -1 inside the closing tail after the carrier. */
int
segmentAt(double ms) {
    if (ms < g_carrierStartMs) {
        return kIdle;
    }
    if (ms < g_carrierStartMs + kCarrierMs) {
        return kCarrier;
    }
    return ms < g_carrierStartMs + kCarrierMs + kCloseMs ? -1 : kAfter;
}

/* The decoder's reads, counted into stream time at the output rate in force when each was made; the scanners' hold
   signal is sampled after each one, and the live switch is made once its stream time has come. */
/* What the receiver ran on the first block the decoder read: the dynamic squelch's status as the demod thread published
   it, so the production startup path is what is seen, not a value the fixture set. */
int g_firstReadSeen = 0;
rtl_stream_squelch_status g_firstReadStatus = {};
/* The samples the decoder's first read returned. The receiver counts where it produces them (the replay produces a
   block only once the decoder reads) the analog monitor samples a closed start muted and those it produced open with
   no dynamic squelch (rtl_stream_squelch_status::closed_samples, unjudged_samples): gate evidence for the samples the
   read returned, apart from the status, which is the demodulator's latest. */
int g_firstReadGot = 0;

int
countingReadEx(void* ctx, float* out, uint8_t* flags, size_t count, int* outGot) {
    const int rc = rtl_stream_read_ex(static_cast<RtlSdrContext*>(ctx), out, flags, count, outGot);
    const unsigned int rate = dsd_rtl_stream_metrics_hook_output_rate_hz();
    if (rc >= 0 && outGot != nullptr && *outGot > 0 && !g_firstReadSeen) {
        g_firstReadSeen = 1;
        g_firstReadGot = *outGot;
        (void)rtl_stream_get_squelch_status(&g_firstReadStatus);
    }
    if (rc >= 0 && outGot != nullptr && *outGot > 0 && rate > 0U) {
        g_heard.readMs += static_cast<double>(*outGot) * 1000.0 / static_cast<double>(rate);
        const int hold = dsd_scan_analog_carrier_open(g_opts, g_state);
        const int at = segmentAt(g_heard.readMs);
        if (at >= 0 && hold) {
            g_heard.holdSeen[at] = 1;
        }
        g_heard.holdAtEnd = hold;
        if (g_bridge != nullptr && !g_switched && g_switchAtMs >= 0.0 && g_heard.readMs >= g_switchAtMs) {
            g_switched = 1;
            expect("NFM is picked live", g_bridge->setDecodeMode(DSDCFG_MODE_ANALOG));
        }
        if (g_bridge != nullptr && g_switched && !g_followed && g_heard.readMs >= g_switchAtMs + kFollowMs) {
            g_followed = 1;
            expect("another UI command follows", g_bridge->setRelaxedCrc(false));
        }
    }
    return rc;
}

int
countingRead(void* ctx, float* out, size_t count, int* outGot) {
    return countingReadEx(ctx, out, nullptr, count, outGot);
}

double
returnPwr(const void* ctx) {
    return rtl_stream_return_pwr(static_cast<const RtlSdrContext*>(ctx));
}

/* In place of the UDP analog blaster: one monitor block of int16 mono samples at the analog sink rate. */
void
captureBlast(const dsd_opts* opts, dsd_state* state, size_t nbytes, const void* data) {
    (void)state;
    const size_t n = nbytes / sizeof(short);
    if (data == nullptr || n == 0U) {
        return;
    }
    const short* pcm = static_cast<const short*>(data);
    double energy = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double s = static_cast<double>(pcm[i]) / 32768.0;
        energy += s * s;
    }
    const double rms = std::sqrt(energy / static_cast<double>(n));
    const double dbfs = rms > 0.0 ? 20.0 * std::log10(rms) : -200.0;
    g_heard.blocks++;
    if (!(dbfs > kAudibleDbfs)) {
        return;
    }
    const double blockMs = static_cast<double>(n) * 1000.0 / static_cast<double>(dsd_opts_analog_sink_rate_hz(opts));
    const int at = segmentAt(g_heard.readMs);
    if (at >= 0) {
        g_heard.audibleMs[at] += blockMs;
    }
    if (g_heard.firstAudibleMs < 0.0) {
        g_heard.firstAudibleMs = g_heard.readMs;
    }
    g_heard.lastAudibleMs = g_heard.readMs;
    g_heard.audibleDbfs.append(qMakePair(g_heard.readMs, dbfs));
}

/* The squelch the session's channel ran when the lifecycle started, the engine's setup and the stream's open done. */
int g_startMode = -1;
int g_startMargin = -1;
double g_startLevel = -1.0;
int g_startDefault = -1;
/* Whether the receiver ran a closed start then: after the stream started, with the audio output, rigctl and the
   frontends' start between it and the decoder's main loop. A live receiver produces from its start, so what it runs
   here is what its first blocks run. */
int g_startClosed = -1;
int g_startAnalog = -1;
/* The setting the receiver's next block takes then (rtl_stream_get_squelch_running()): what its first blocks run, before
   the decoder's first settle could correct it. */
dsd_squelch_setting g_startRuns = {DSD_SQUELCH_MODE_LEVEL, -1.0, 0};
/* A receive request queued as the engine stops (second-session-nfm's first session), and whether it was still pending
   then: the stopped stream never takes it. */
bool g_queueAtStop = false;
int g_stopQueuedPending = -1;

/* After the engine installed its hooks and opened its (null) output: the monitor goes to the capture. */
int
listenerStart(dsd_opts* opts, dsd_state* state, void* context) {
    (void)state;
    (void)context;
    g_startMode = opts->rtl_squelch_mode;
    g_startMargin = opts->rtl_squelch_margin_db;
    g_startLevel = opts->rtl_squelch_level;
    g_startDefault = opts->squelch_default_state == DSD_SQUELCH_DEFAULT_APPLIED ? 1 : 0;
    g_startClosed = rtl_stream_gate_closed_start();
    g_startAnalog = dsd_opts_is_analog_family(opts) ? 1 : 0;
    (void)rtl_stream_get_squelch_running(&g_startRuns);
    opts->audio_out = 1;
    opts->audio_out_type = 8;
    dsd_udp_audio_hooks audio = {};
    audio.blast_analog = captureBlast;
    dsd_udp_audio_hooks_set(audio);
    dsd_rtl_stream_io_hooks io = {};
    io.read = countingRead;
    io.read_ex = countingReadEx;
    io.return_pwr = returnPwr;
    dsd_rtl_stream_io_hooks_set(&io);
    return 0;
}

void
listenerStop(dsd_opts* opts, dsd_state* state, void* context) {
    (void)opts;
    (void)state;
    (void)context;
    dsd_udp_audio_hooks_set(dsd_udp_audio_hooks{});
    if (g_queueAtStop) {
        /* A frame-sync hunt's symbol request, as the decoder makes one: the replay is at its end, so the demod thread
           takes no block before the stop. */
        (void)rtl_stream_request_demod_profile(0, 4800, 4, DSD_CH_LPF_PROFILE_12K5, 10, 0);
        g_stopQueuedPending =
            rtl_stream_receive_request_outcome(rtl_stream_receive_request_seq()) == RTL_STREAM_RX_REQUEST_PENDING ? 1
                                                                                                                  : 0;
    }
}

/* @p bytes of the committed capture @p name, from @p from. */
QByteArray
fixtureBytes(const char* name, qsizetype from, qsizetype bytes) {
    QFile file(QStringLiteral(DSD_NEO_TEST_IQ_FIXTURE_DIR "/%1.iq").arg(QString::fromLatin1(name)));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray all = file.readAll();
    return bytes < 0 ? all.mid(from) : all.mid(from, bytes);
}

/* The listening capture in @p dir: idle noise, the transmission, idle noise; its sidecar's path, or "" on failure. */
QString
makeCapture(const QTemporaryDir& dir) {
    const qsizetype perMs = 2 * kRateHz / 1000; // cu8 I/Q
    const QByteArray idle = fixtureBytes("noise_floor", 0, static_cast<qsizetype>(kIdleMs) * perMs);
    const QByteArray speech = fixtureBytes("speech_nfm_synth", 0, -1);
    const QByteArray after =
        fixtureBytes("noise_floor", static_cast<qsizetype>(kIdleMs) * perMs, static_cast<qsizetype>(kAfterMs) * perMs);
    QFile meta(QStringLiteral(DSD_NEO_TEST_IQ_FIXTURE_DIR "/noise_floor.iq.json"));
    if (idle.size() != static_cast<qsizetype>(kIdleMs) * perMs
        || speech.size() != static_cast<qsizetype>(kCarrierMs) * perMs
        || after.size() != static_cast<qsizetype>(kAfterMs) * perMs || !meta.open(QIODevice::ReadOnly)) {
        return {};
    }
    QJsonObject sidecar = QJsonDocument::fromJson(meta.readAll()).object();
    const QByteArray data = idle + speech + after;
    sidecar.insert(QStringLiteral("data_file"), QStringLiteral("listening.iq"));
    sidecar.insert(QStringLiteral("data_bytes"), static_cast<qint64>(data.size()));
    sidecar.insert(QStringLiteral("notes"), QStringLiteral("noise_floor 3 s, speech_nfm_synth, noise_floor 2 s"));
    QFile out(dir.filePath(QStringLiteral("listening.iq")));
    QFile outMeta(dir.filePath(QStringLiteral("listening.iq.json")));
    const bool written = out.open(QIODevice::WriteOnly) && out.write(data) == data.size()
                         && outMeta.open(QIODevice::WriteOnly) && outMeta.write(QJsonDocument(sidecar).toJson()) > 0;
    return written ? outMeta.fileName() : QString();
}

/* The opening capture in @p dir: the transmission from the capture's first sample, then idle noise; its sidecar's
   path, or "" on failure. */
QString
makeOpeningCapture(const QTemporaryDir& dir) {
    const qsizetype perMs = 2 * kRateHz / 1000; // cu8 I/Q
    const QByteArray speech = fixtureBytes("speech_nfm_synth", 0, -1);
    const QByteArray after = fixtureBytes("noise_floor", 0, static_cast<qsizetype>(kAfterMs) * perMs);
    QFile meta(QStringLiteral(DSD_NEO_TEST_IQ_FIXTURE_DIR "/noise_floor.iq.json"));
    if (speech.size() != static_cast<qsizetype>(kCarrierMs) * perMs
        || after.size() != static_cast<qsizetype>(kAfterMs) * perMs || !meta.open(QIODevice::ReadOnly)) {
        return {};
    }
    QJsonObject sidecar = QJsonDocument::fromJson(meta.readAll()).object();
    const QByteArray data = speech + after;
    sidecar.insert(QStringLiteral("data_file"), QStringLiteral("opening.iq"));
    sidecar.insert(QStringLiteral("data_bytes"), static_cast<qint64>(data.size()));
    sidecar.insert(QStringLiteral("notes"), QStringLiteral("speech_nfm_synth, noise_floor 2 s"));
    QFile out(dir.filePath(QStringLiteral("opening.iq")));
    QFile outMeta(dir.filePath(QStringLiteral("opening.iq.json")));
    const bool written = out.open(QIODevice::WriteOnly) && out.write(data) == data.size()
                         && outMeta.open(QIODevice::WriteOnly) && outMeta.write(QJsonDocument(sidecar).toJson()) > 0;
    return written ? outMeta.fileName() : QString();
}

/* The stream time the capture a case listens to lasts: the listening capture's, or the opening capture's. */
double g_captureMs = kCarrierEndMs + kAfterMs;

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

/* The squelch the options hold: @p mode, with @p margin for a dynamic one; a LEVEL here is always off. */
bool
squelchIs(const dsd_opts* opts, int mode, int margin) {
    if (opts->rtl_squelch_mode != mode) {
        return false;
    }
    return mode == DSD_SQUELCH_MODE_LEVEL ? !(opts->rtl_squelch_level > 0.0) : opts->rtl_squelch_margin_db == margin;
}

/* What to do between the session's start-up and the engine run: commit a scan row. */
using BeforeRun = bool (*)(dsd_opts* opts, dsd_state* state, const QString& rowOptions);

/* The session's opts and state; the state's buffers (the recorded command line among them) are freed with it. */
struct Session {
    std::unique_ptr<dsd_opts> opts = std::make_unique<dsd_opts>();
    std::unique_ptr<dsd_state> state = std::make_unique<dsd_state>();
    bool stateInitialized = false;

    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    ~Session() {
        if (stateInitialized) {
            freeState(state.get());
        }
    }
};

/* Starts @p appArgs as the decoder host does, over the replay of @p capture with the app's radio spec kept, and runs
   the engine to the capture's end. @p want_mode / @p want_margin: the squelch the session's channel runs once the
   engine has set it up (after @p before), the analog default when @p by_default. Returns whether it ran. */
bool
listen(const QStringList& appArgs, const QString& capture, int wantMode, int wantMargin, bool byDefault,
       BeforeRun before, const QString& rowOptions, Session& session) {
    const qsizetype at = appArgs.indexOf(QStringLiteral("-i"));
    const QString spec = at >= 0 ? appArgs.value(at + 1) : QString();
    DSD_FPRINTF(stderr, "app argv: %s\n", qPrintable(appArgs.join(QLatin1Char(' '))));
    expect("the app built a radio spec", spec.startsWith(QStringLiteral("rtl:")));
    QStringList args = appArgs;
    args << QStringLiteral("--iq-replay") << capture << QStringLiteral("-o") << QStringLiteral("null");
    QList<QByteArray> storage{QByteArray("dsd-neo")};
    for (const QString& arg : args) {
        storage.append(arg.toUtf8());
    }
    QList<char*> argv;
    for (QByteArray& arg : storage) {
        argv.append(arg.data());
    }
    argv.append(nullptr);
    /* What the last session in this process heard and saw goes. */
    g_heard = Heard{};
    g_firstReadSeen = 0;
    g_firstReadGot = 0;
    g_firstReadStatus = rtl_stream_squelch_status{};
    g_startClosed = -1;
    g_startRuns = dsd_squelch_setting{DSD_SQUELCH_MODE_LEVEL, -1.0, 0};
    dsd_opts* opts = session.opts.get();
    dsd_state* state = session.state.get();
    initOpts(opts);
    initState(state);
    session.stateInitialized = true;
    int effective = 0;
    int exitCode = 0;
    if (dsd_runtime_bootstrap(static_cast<int>(storage.size()), argv.data(), opts, state, &effective, &exitCode)
            != DSD_BOOTSTRAP_CONTINUE
        || exitCode != 0) {
        expect("the session starts", false);
        return false;
    }
    // The app's radio spec, read as the engine reads it when the radio opens; the replay keeps its own tuning.
    char replayDev[sizeof opts->audio_in_dev];
    DSD_SNPRINTF(replayDev, sizeof replayDev, "%s", opts->audio_in_dev);
    const uint32_t replayFreq = opts->rtlsdr_center_freq;
    const int replayPpm = opts->rtlsdr_ppm_error;
    const int replayGain = opts->rtl_gain_value;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", spec.toUtf8().constData());
    const int applied = dsd_rtl_input_spec_apply(opts);
    expect("the app's radio spec is read", applied == 1);
    expect("on the capture's own frequency and bandwidth",
           opts->rtlsdr_center_freq == replayFreq && opts->rtl_dsp_bw_khz == 48);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", replayDev);
    opts->rtlsdr_center_freq = replayFreq;
    opts->rtlsdr_ppm_error = replayPpm;
    opts->rtl_gain_value = replayGain;
    expect("the app's spec sets no squelch", spec.contains(QStringLiteral(":48:0:2")));
    if (before != nullptr && !before(opts, state, rowOptions)) {
        return false;
    }
    /* Nothing here sets the startup squelch: the engine's setup and its stream open resolve it, seen at the lifecycle's
       start and, as the receiver ran it, at the first read. */
    g_opts = opts;
    g_state = state;
    dsd_app_frontend_runtime_start(opts, state);
    const dsd_engine_lifecycle_hooks hooks = {listenerStart, listenerStop, nullptr};
    const int rc = dsd_engine_run_with_lifecycle(opts, state, &hooks);
    dsd_app_frontend_runtime_stop();
    dsd_rtl_stream_io_hooks_set(nullptr);
    DSD_FPRINTF(stderr,
                "heard: read %.0f ms, %u blocks; audible %.0f / %.0f / %.0f ms (idle / transmission / after), first "
                "%.0f ms, last %.0f ms; held %d / %d / %d, at the end %d\n",
                g_heard.readMs, g_heard.blocks, g_heard.audibleMs[kIdle], g_heard.audibleMs[kCarrier],
                g_heard.audibleMs[kAfter], g_heard.firstAudibleMs, g_heard.lastAudibleMs, g_heard.holdSeen[kIdle],
                g_heard.holdSeen[kCarrier], g_heard.holdSeen[kAfter], g_heard.holdAtEnd);
    expect("the engine runs the capture to its end", rc == 0 && g_heard.readMs > g_captureMs - 100.0);
    const auto at_start = std::make_unique<dsd_opts>();
    at_start->rtl_squelch_mode = g_startMode;
    at_start->rtl_squelch_margin_db = g_startMargin;
    at_start->rtl_squelch_level = g_startLevel;
    const bool inDefault = g_startDefault == 1;
    if (!squelchIs(at_start.get(), wantMode, wantMargin) || inDefault != byDefault) {
        DSD_FPRINTF(stderr, "  squelch mode %d margin %d level %g default %d, want mode %d margin %d default %d\n",
                    g_startMode, g_startMargin, g_startLevel, g_startDefault, wantMode, wantMargin, byDefault ? 1 : 0);
    }
    expect("the channel runs the squelch it should when the engine starts",
           squelchIs(at_start.get(), wantMode, wantMargin) && inDefault == byDefault);
    /* The receiver ran the resolved gate before its first block (round 9): an analog start with no tune outstanding is
       no closed start, so nothing the engine does between the stream's start and its main loop mutes what the
       receiver produces meanwhile. */
    if (g_startClosed != 0) {
        DSD_FPRINTF(stderr, "  at the lifecycle's start the receiver runs a closed start (%d)\n", g_startClosed);
    }
    expect("the receiver runs the gate before its first block, not a closed start", g_startClosed == 0);
    /* And the setting its first blocks take is the channel's own, before the decoder's first settle: the analog gate
       on the analog monitor (noise +10 under the default), not the digital family's (the session's Off). */
    const bool runsWanted = wantMode == DSD_SQUELCH_MODE_LEVEL
                                ? dsd_squelch_setting_is_off(&g_startRuns)
                                : g_startRuns.mode == wantMode && g_startRuns.margin_db == wantMargin;
    if (!runsWanted) {
        DSD_FPRINTF(stderr, "  at the lifecycle's start the receiver's next block takes mode %d margin %d level %g\n",
                    g_startRuns.mode, g_startRuns.margin_db, g_startRuns.level);
    }
    expect("the receiver's first blocks take the channel's own squelch, before the first settle", runsWanted);
    /* Gate evidence for the samples the first read returned, as the receiver produced them: none muted by a closed
       start, and under a noise setting none produced open without it; under Off on the analog monitor they are
       produced open with no dynamic squelch. */
    const bool wantNoise = wantMode == DSD_SQUELCH_MODE_NOISE;
    const uint64_t got = static_cast<uint64_t>(g_firstReadGot);
    const bool produced = g_firstReadSeen && g_firstReadStatus.closed_starts == 0
                          && g_firstReadStatus.closed_samples == 0U
                          && (wantNoise ? g_firstReadStatus.unjudged_samples == 0U
                                        : g_startAnalog != 1 || g_firstReadStatus.unjudged_samples >= got);
    if (!produced) {
        DSD_FPRINTF(stderr,
                    "  first read %d (%d samples): closed starts %d, closed samples %llu, unjudged samples %llu, want "
                    "mode %d\n",
                    g_firstReadSeen, g_firstReadGot, g_firstReadStatus.closed_starts,
                    static_cast<unsigned long long>(g_firstReadStatus.closed_samples),
                    static_cast<unsigned long long>(g_firstReadStatus.unjudged_samples), wantMode);
    }
    expect("the samples the first read returned were produced under that gate", produced);
    /* And, separately, the status the demodulator published by then: the noise squelch for a noise setting on the FM
       monitor, nothing dynamic under a level or Off. */
    if (!g_firstReadSeen || (g_firstReadStatus.active == 1) != (wantMode != DSD_SQUELCH_MODE_LEVEL)
        || (wantNoise && g_firstReadStatus.noise != 1)) {
        DSD_FPRINTF(stderr, "  first read %d: receiver squelch active %d noise %d, want mode %d\n", g_firstReadSeen,
                    g_firstReadStatus.active, g_firstReadStatus.noise, wantMode);
        expect("the receiver's status at the first read names that squelch", false);
    }
    return true;
}

/* The squelch the default leaves the listener: nothing on the idle channel before or after, the transmission from
   its talker's start (0.25 s into the carrier; the noise squelch decides on its first 40 ms window) and most of it. */
void
expectQuietThenPlays() {
    expect("the idle channel plays nothing", !(g_heard.audibleMs[kIdle] > 0.0));
    expect("the transmission plays from its talker's start",
           g_heard.firstAudibleMs >= kIdleMs && g_heard.firstAudibleMs <= kIdleMs + 400.0);
    expect("and most of it plays", g_heard.audibleMs[kCarrier] >= 4000.0);
    expect("nothing plays once it ends",
           !(g_heard.audibleMs[kAfter] > 0.0) && g_heard.lastAudibleMs <= kCarrierEndMs + kCloseMs);
}

/* The squelch off: the idle channel's hiss plays (what the default removes), so the scoring hears it. */
void
expectHiss() {
    expect("with the squelch off the idle channel plays its hiss", g_heard.audibleMs[kIdle] >= 1000.0);
}

QVariantMap
nfmSystem() {
    return {{QStringLiteral("uid"), QStringLiteral("sys-nfm")},
            {QStringLiteral("name"), QStringLiteral("Simplex")},
            {QStringLiteral("sourceType"), QStringLiteral("usb")},
            {QStringLiteral("freqMhz"), QStringLiteral("851.375")},
            {QStringLiteral("decodeFlag"), QStringLiteral("-fA")}};
}

/* The value the argv gives --squelch-analog-default, the one the parser keeps; empty for none. */
QString
squelchDefaultArg(const QStringList& argv) {
    const qsizetype at = argv.lastIndexOf(QStringLiteral("--squelch-analog-default"));
    return at >= 0 ? argv.value(at + 1) : QString();
}

/* The length of the WAV at @p path, from its header's rate, block size and data size; -1 when it reads as no WAV. */
double
wavDurationMs(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return -1.0;
    }
    const QByteArray bytes = file.readAll();
    const auto u32 = [&bytes](qsizetype at) {
        return static_cast<quint32>(static_cast<unsigned char>(bytes.at(at)))
               | (static_cast<quint32>(static_cast<unsigned char>(bytes.at(at + 1))) << 8)
               | (static_cast<quint32>(static_cast<unsigned char>(bytes.at(at + 2))) << 16)
               | (static_cast<quint32>(static_cast<unsigned char>(bytes.at(at + 3))) << 24);
    };
    const qsizetype data = bytes.indexOf("data", 12);
    if (bytes.size() < 44 || !bytes.startsWith("RIFF") || data < 0 || data + 8 > bytes.size()) {
        return -1.0;
    }
    const quint32 rate = u32(24);
    const quint32 blockAlign = u32(32) & 0xFFFFU;
    const quint32 size = u32(data + 4);
    if (rate == 0U || blockAlign == 0U) {
        return -1.0;
    }
    const quint32 frames = size / blockAlign;
    return static_cast<double>(frames) * 1000.0 / rate;
}

/* The mean level, in dBFS, of the audible blocks heard in [@p fromMs, @p toMs) of stream time; -200 for none. */
double
meanDbfs(double fromMs, double toMs) {
    double sum = 0.0;
    int n = 0;
    for (const auto& block : g_heard.audibleDbfs) {
        if (block.first >= fromMs && block.first < toMs) {
            sum += block.second;
            n++;
        }
    }
    return n > 0 ? sum / n : -200.0;
}

int
caseSavedNfm(dsd_qt::AppPrefs& prefs, const QString& capture, const QTemporaryDir& dir, bool off) {
    prefs.setSquelch(off ? QStringLiteral("off") : QStringLiteral("noise+10"));
    dsd_qt::SessionArgsBuilder builder(&prefs);
    Host host;
    expect("the NFM saved system starts",
           builder.start(nfmSystem(), &host, QStringLiteral("saved")).value("started").toBool());
    expect("it passes the Settings default (no option for Off), its spec setting no squelch",
           squelchDefaultArg(host.argv) == (off ? QString() : QStringLiteral("noise+10"))
               && host.argv.contains(QStringLiteral("rtl:0:851.375M:30:0:48:0:2")));
    // Per-call recording, as Settings > Record calls runs it: does the squelch admit the call, and only the call?
    const QString calls = dir.filePath(QStringLiteral("calls"));
    QStringList args = host.argv;
    args << QStringLiteral("-P") << QStringLiteral("-7") << calls;
    Session session;
    if (listen(args, capture, off ? DSD_SQUELCH_MODE_LEVEL : DSD_SQUELCH_MODE_NOISE, off ? 0 : 10, !off, nullptr,
               QString(), session)) {
        const QStringList wavs = QDir(calls).entryList({QStringLiteral("*.wav")}, QDir::Files, QDir::Name);
        const double recordedMs = wavs.isEmpty() ? -1.0 : wavDurationMs(QDir(calls).filePath(wavs.first()));
        DSD_FPRINTF(stderr, "recordings: %lld, the first %.0f ms\n", static_cast<long long>(wavs.size()), recordedMs);
        if (off) {
            expectHiss();
            expect("analog calls are paused with the squelch off on a radio", wavs.isEmpty());
        } else {
            expectQuietThenPlays();
            expect("the transmission is one recording, the idle channel none", wavs.size() == 1);
            expect("which holds the call", recordedMs >= 4000.0 && recordedMs <= kCarrierMs + 500.0);
            const double onset = meanDbfs(g_heard.firstAudibleMs, g_heard.firstAudibleMs + 500.0);
            const double steady = meanDbfs(g_heard.firstAudibleMs + 2000.0, g_heard.firstAudibleMs + 6000.0);
            DSD_FPRINTF(stderr, "speech level: %.1f dBFS at the opening, %.1f dBFS later\n", onset, steady);
            expect("the speech level at the opening is the level later in the call",
                   onset > kAudibleDbfs && std::fabs(onset - steady) <= kOnsetLevelDb);
        }
    }
    return 0;
}

/* Round 9: a transmission on the air from the stream's first sample, on the NFM saved system (the Settings default,
   or Off). The receiver runs the resolved gate before it produces anything, so the opening plays from its talker's
   start (0.25 s in) whatever the engine did between the stream's start and its main loop: the decoder's first read
   holds no sample a closed start muted. */
int
caseOpeningNfm(dsd_qt::AppPrefs& prefs, const QString& capture, bool off) {
    prefs.setSquelch(off ? QStringLiteral("off") : QStringLiteral("noise+10"));
    dsd_qt::SessionArgsBuilder builder(&prefs);
    Host host;
    expect("the NFM saved system starts",
           builder.start(nfmSystem(), &host, QStringLiteral("saved")).value("started").toBool());
    g_captureMs = kCarrierMs + kAfterMs;
    g_carrierStartMs = 0.0;
    Session session;
    if (listen(host.argv, capture, off ? DSD_SQUELCH_MODE_LEVEL : DSD_SQUELCH_MODE_NOISE, off ? 0 : 10, !off, nullptr,
               QString(), session)) {
        expect("the opening transmission plays from its talker's start",
               g_heard.firstAudibleMs >= 0.0 && g_heard.firstAudibleMs <= 400.0);
        /* Inside the transmission only: the noise after it (hiss under Off) is no part of it. */
        expect("and most of it plays", g_heard.audibleMs[kCarrier] >= 4000.0);
    }
    return 0;
}

/* Round 10: a session that stops with a receive request its stream never took (a hunt's, queued as the engine stops),
   then a new NFM session in the same process. The request cannot land on the new stream; it must not hold that
   session's start closed. */
int
caseSecondSessionNfm(dsd_qt::AppPrefs& prefs, const QString& capture) {
    g_queueAtStop = true;
    (void)caseOpeningNfm(prefs, capture, true);
    g_queueAtStop = false;
    expect("the first session stopped with a receive request its stream never took", g_stopQueuedPending == 1);
    return caseOpeningNfm(prefs, capture, false);
}

/* Explore starts digital (Auto decode), so the monitor plays nothing until NFM is picked. */
bool
startsDigital(dsd_opts* opts, dsd_state* state, const QString& rowOptions) {
    (void)state;
    (void)rowOptions;
    expect("Explore starts on a digital decode", opts->analog_only == 0);
    return true;
}

int
caseExploreLiveNfm(dsd_qt::AppPrefs& prefs, const QString& capture) {
    prefs.setSquelch(QStringLiteral("noise+10"));
    dsd_qt::CommandBridge bridge;
    dsd_qt::SessionArgsBuilder builder(&prefs);
    Host host;
    const QVariantMap explore{{QStringLiteral("sourceType"), QStringLiteral("usb")},
                              {QStringLiteral("freqMhz"), QStringLiteral("851.375")}};
    expect("Explore starts", builder.start(explore, &host, QStringLiteral("explore")).value("started").toBool());
    bool decodeFlag = false;
    for (const QString& arg : host.argv) {
        decodeFlag = decodeFlag || arg.startsWith(QStringLiteral("-f"));
    }
    expect("on Auto decode (no -f), passing the default",
           !decodeFlag && squelchDefaultArg(host.argv) == QStringLiteral("noise+10"));
    g_bridge = &bridge;
    g_switchAtMs = 1000.0;
    Session session;
    if (listen(host.argv, capture, DSD_SQUELCH_MODE_LEVEL, 0, false, startsDigital, QString(), session)) {
        expect("NFM was picked 1 s into the run, another command after it", g_switched == 1 && g_followed == 1);
        expect("and the app's default came in with it",
               session.opts->analog_only == 1 && squelchIs(session.opts.get(), DSD_SQUELCH_MODE_NOISE, 10)
                   && session.opts->squelch_default_state == DSD_SQUELCH_DEFAULT_APPLIED);
        expectQuietThenPlays();
    }
    g_bridge = nullptr;
    return 0;
}

/* Commits an nfm row with @p rowOptions (its options cell) as the scan coordinator commits one: the cell through the
   row parser the scan loader uses, then the row's class and its options. */
bool
commitNfmRow(dsd_opts* opts, dsd_state* state, const QString& rowOptions) {
    const QByteArray cell = rowOptions.toUtf8();
    dsd_scan_options options = {};
    char error[192] = "";
    const bool parsed =
        dsd_scan_options_parse_cell(cell.constData(), nullptr, static_cast<size_t>(cell.size()),
                                    static_cast<unsigned int>(DSD_SCAN_MODE_NFM), 1, &options, error, sizeof error)
        == 0;
    if (!parsed) {
        DSD_FPRINTF(stderr, "  row options '%s': %s\n", cell.constData(), error);
    }
    expect("the generated row's options parse", parsed);
    dsd_scan_settings prepared;
    const bool committed = parsed
                           && dsd_scan_mode_prepare(opts, state, DSD_SCAN_MODE_NFM, &options.values, &prepared) == 0
                           && dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_NFM) == 0
                           && dsd_scan_mode_options(opts, state, &options.values) == 0;
    expect("the nfm row is committed", committed);
    return committed;
}

/* The options cell of the generated target CSV's first row, from the path after --trunk-scan in @p argv. */
QString
firstRowOptions(const QStringList& argv) {
    const qsizetype at = argv.indexOf(QStringLiteral("--trunk-scan"));
    QFile csv(argv.value(at + 1));
    if (at < 0 || !csv.open(QIODevice::ReadOnly)) {
        return QStringLiteral("<no target list>");
    }
    const QStringList lines = QString::fromUtf8(csv.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    const QStringList header = lines.value(0).trimmed().split(QLatin1Char(','));
    const qsizetype column = header.indexOf(QStringLiteral("options"));
    const QString row = lines.value(1).trimmed();
    DSD_FPRINTF(stderr, "target row: %s\n", qPrintable(row));
    return column < 0 ? QStringLiteral("<no options column>") : row.section(QLatin1Char(','), column, column);
}

/* The session argv without the target list, which the replay cannot scan (it cannot retune). */
QStringList
withoutTargetList(QStringList argv) {
    const qsizetype at = argv.indexOf(QStringLiteral("--trunk-scan"));
    if (at >= 0) {
        argv.remove(at, 2);
    }
    return argv;
}

int
caseScanRow(dsd_qt::AppPrefs& prefs, const QString& capture, bool off) {
    prefs.setSquelch(off ? QStringLiteral("off") : QStringLiteral("noise+10"));
    dsd_qt::ScanListsModel lists;
    expect("the scan list is added", lists.add({{"name", "Simplex"},
                                                {"sourceType", "usb"},
                                                {"entries", QVariantList{QVariantMap{{"kind", "freq"},
                                                                                     {"name", "Ops"},
                                                                                     {"protocol", "nfm"},
                                                                                     {"freqMhz", "851.375"},
                                                                                     {"enabled", true}}}}}));
    dsd_qt::ScanListStarter starter(&prefs, nullptr);
    Host host;
    expect("the scan list starts", starter.start(lists.get(lists.count() - 1), &host).value("started").toBool());
    const QString rowOptions = firstRowOptions(host.argv);
    expect("the generated nfm row has no squelch of its own", !rowOptions.contains(QStringLiteral("--squelch")));
    expect("and the session passes the Settings default (no option for Off)",
           squelchDefaultArg(host.argv) == (off ? QString() : QStringLiteral("noise+10")));
    Session session;
    if (listen(withoutTargetList(host.argv), capture, off ? DSD_SQUELCH_MODE_LEVEL : DSD_SQUELCH_MODE_NOISE,
               off ? 0 : 10, !off, commitNfmRow, rowOptions, session)) {
        if (off) {
            expectHiss();
            expect("and the idle channel holds the scan", g_heard.holdSeen[kIdle] == 1);
        } else {
            expectQuietThenPlays();
            expect("the idle channel never holds the scan", g_heard.holdSeen[kIdle] == 0);
            expect("the transmission holds it", g_heard.holdSeen[kCarrier] == 1);
            expect("and it is let go after the transmission", g_heard.holdAtEnd == 0);
        }
    }
    dsd_scan_mode_leave(session.opts.get(), session.state.get());
    return 0;
}

} // namespace
#endif

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("dsd-neo-test"));
    app.setApplicationName(QStringLiteral("listener-squelch-%1").arg(QCoreApplication::applicationPid()));
    dsd_test_qt_isolate_paths();
    const QString which = argc > 1 ? QString::fromLatin1(argv[1]) : QString();
#ifdef USE_RADIO
    QTemporaryDir dir;
    const bool opening = which.startsWith(QStringLiteral("opening-")) || which == QStringLiteral("second-session-nfm");
    const QString capture = dir.isValid() ? (opening ? makeOpeningCapture(dir) : makeCapture(dir)) : QString();
    expect("the listening capture is made", !capture.isEmpty());
    if (!capture.isEmpty()) {
        dsd_qt::AppPrefs prefs;
        if (which == QStringLiteral("saved-nfm")) {
            caseSavedNfm(prefs, capture, dir, false);
        } else if (which == QStringLiteral("saved-nfm-off")) {
            caseSavedNfm(prefs, capture, dir, true);
        } else if (which == QStringLiteral("explore-live-nfm")) {
            caseExploreLiveNfm(prefs, capture);
        } else if (which == QStringLiteral("scan-row")) {
            caseScanRow(prefs, capture, false);
        } else if (which == QStringLiteral("scan-row-off")) {
            caseScanRow(prefs, capture, true);
        } else if (which == QStringLiteral("opening-nfm")) {
            caseOpeningNfm(prefs, capture, false);
        } else if (which == QStringLiteral("opening-nfm-off")) {
            caseOpeningNfm(prefs, capture, true);
        } else if (which == QStringLiteral("second-session-nfm")) {
            caseSecondSessionNfm(prefs, capture);
        } else {
            expect("a known case", false);
        }
    }
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("OK");
    return 0;
#else
    (void)which;
    std::puts("SKIP: no radio pipeline in this build");
    return 77;
#endif
}
