// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Issue #899: a generated mixed scan list on a running receiver. The list is the one the app builds (scan_list_targets()
 * from a DMR and an NFM frequency entry, started with session_args_scan_build_lists() and the Settings default
 * squelch, so --squelch-analog-default noise+10), run by the real engine over rtl_tcp against an in-process server that
 * plays committed fixtures for the frequency the stream tunes (rtl_tcp_fixture_server.h): a DMR voice call then noise
 * on the DMR row, receiver noise then NFM speech then noise on the NFM row, each from its start at every landing.
 *
 * The real trunk-scan coordinator rotates the targets on its own dwell and hold, retuning the stream. The host counts,
 * per visit, what the decoder read (each sample's squelch flag, the receiver's dynamic-squelch status after the read)
 * and what reached the speaker (the UDP audio hooks, digital voice and analog monitor apart), and stops the engine once
 * two visits of each row have been seen. It asserts:
 * - DMR row (every visit): DMR voice decodes (a level gate left on it would zero its samples), and the receiver runs
 *   no dynamic squelch there and flags no sample closed;
 * - NFM row (every visit, the second one being the default's return after the DMR row): the receiver runs the noise
 *   squelch, the receiver noise the visit starts on is flagged closed and none of it plays, and the speech after it
 *   plays.
 * With --saved-digital-under-level (ENGINE_TRUNK_SCAN_RUNNING_RECEIVER_LEVEL) the digital row comes from a saved DMR
 * system whose saved squelch is dynamic, so the generator gives the row --squelch off (B13), and the session carries
 * an Advanced --squelch -1, which ends the analog default. Then the DMR row runs its own off and decodes, and the plain
 * NFM row inherits the session's -1 dB level: no dynamic squelch, the default not in force, and nothing plays (the
 * level is over every sample of the fixture, speech included).
 *
 * The NFM row carries a dwell of its own, 3.5 s: a session's first landing on an analog row brings its first samples
 * 0.8 to 2 s late in a Debug build (the demodulator designs the noise squelch's plan, tens of ms when optimized; later
 * landings take it from the cache and play in about 0.15 s), and a shorter dwell can expire before its speech.
 * Real time, about 30 s. A receiver that delivers no samples, or a scan that never leaves its first row, fails on any
 * host. A host too slow for the real-time dwell (a sanitizer build), measured before the session by timing a noise
 * squelch plan design and not from anything the receiver or the scan did, is reported as a skip (77). POSIX only (the
 * server's loopback sockets).
 */
#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QIODevice>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "rtl_tcp_fixture_server.h"
#include "scan_list_targets.h"
#include "session_args.h"
extern "C" {
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/nfm_noise_squelch.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/squelch.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
}

namespace {

constexpr uint32_t kDmrHz = 461112500U;
constexpr uint32_t kNfmHz = 154430000U;
/* The receiver noise each NFM visit starts on, and the part of it a visit must show gated: the rest allows for the
   samples a landing flushes and the stream's start. */
constexpr double kNfmNoiseLeadS = 0.8;
constexpr double kNfmGatedAtLeastS = 0.4;
constexpr double kOutputRateHz = 48000.0;
constexpr uint64_t kDeadlineMs = 45000U;
/* The longest plan design (planDesignMs()) under which the timing-bound checks mean anything: the scan's dwell and hold
   run on the real clock, so a host that takes longer (a sanitizer build, about 400 ms here, against 104 ms for a
   Debug build) leaves the first analog row before its air plays (a first landing waiting 3.5 s or more). */
constexpr uint64_t kPlanDesignMaxMs = 200U;
/* CTest's SKIP_RETURN_CODE for this test. */
constexpr int kSkipped = 77;

int failures;
/* The second scenario: a saved digital system's own off under an Advanced session level (see the top). */
bool g_saved_level;

void
expect(bool ok, const char* what, int visit) {
    if (!ok) {
        ++failures;
        DSD_FPRINTF(stderr, "running receiver: %s (visit %d)\n", what, visit);
    }
}

/* What one visit of a row showed, from the decoder's thread. */
struct Visit {
    uint32_t freq_hz = 0U;
    uint64_t flagged = 0U;            /* samples read with their squelch flags */
    uint64_t closed = 0U;             /* of those, flagged closed */
    uint64_t closed_before_open = 0U; /* closed samples before the first open one */
    bool opened = false;
    uint64_t reads = 0U;
    uint64_t samples = 0U;                  /* every sample the decoder read */
    uint64_t noise_squelch_reads = 0U;      /* reads after which the receiver ran the noise squelch */
    uint64_t dynamic_reads = 0U;            /* reads after which it ran a dynamic squelch of any kind */
    uint64_t digital_audio = 0U;            /* decoded voice samples played */
    uint64_t analog_audio = 0U;             /* monitor samples played */
    uint64_t analog_audio_while_gated = 0U; /* monitor samples played before the first open sample */
    uint64_t off_reads = 0U;                /* reads under a LEVEL setting that is off */
    uint64_t session_level_reads = 0U;      /* reads under the session's -1 dB level, the default not in force */
    uint64_t first_ms = 0U;                 /* real time of the visit's first read, empty ones too */
    uint64_t first_data_ms = 0U;            /* and of its first and last read that brought samples */
    uint64_t last_ms = 0U;
};

std::mutex g_lock;
std::vector<Visit> g_visits;
const dsd_opts* g_opts;

Visit*
current_visit(uint32_t freq_hz) {
    if (g_visits.empty() || g_visits.back().freq_hz != freq_hz) {
        g_visits.emplace_back();
        g_visits.back().freq_hz = freq_hz;
        g_visits.back().first_ms = dsd_realtime_mono_ms();
    }
    return &g_visits.back();
}

uint32_t
tuned_row(void) {
    return g_opts != nullptr ? static_cast<uint32_t>(g_opts->rtlsdr_center_freq) : 0U;
}

/* The squelch the options hold at a read: an off level, or the session's -1 dB with the default not in force. */
void
note_setting(Visit* v) {
    if (g_opts != nullptr && g_opts->rtl_squelch_mode == DSD_SQUELCH_MODE_LEVEL) {
        const double minus1 = dsd_squelch_level_from_sql(-1.0);
        v->off_reads += dsd_squelch_is_off(g_opts->rtl_squelch_level) ? 1U : 0U;
        v->session_level_reads += (std::fabs(g_opts->rtl_squelch_level - minus1) <= minus1 * 1e-9
                                   && g_opts->squelch_default_state != DSD_SQUELCH_DEFAULT_APPLIED)
                                      ? 1U
                                      : 0U;
    }
}

/* After each read: the samples' flags and the receiver's dynamic squelch, on the row the decoder is on. */
void
note_read(int rc, const uint8_t* flags, const int* out_got) {
    if (rc < 0 || out_got == nullptr || *out_got <= 0) {
        std::lock_guard<std::mutex> hold(g_lock);
        (void)current_visit(tuned_row());
        return;
    }
    rtl_stream_squelch_status st{};
    const bool have_status = rtl_stream_get_squelch_status(&st) == 0;
    std::lock_guard<std::mutex> hold(g_lock);
    Visit* v = current_visit(tuned_row());
    ++v->reads;
    v->last_ms = dsd_realtime_mono_ms();
    if (v->samples == 0U) {
        v->first_data_ms = v->last_ms;
    }
    v->samples += static_cast<uint64_t>(*out_got);
    note_setting(v);
    if (have_status && st.active) {
        ++v->dynamic_reads;
        v->noise_squelch_reads += st.noise ? 1U : 0U;
    }
    if (flags == nullptr) {
        return;
    }
    for (int i = 0; i < *out_got; ++i) {
        ++v->flagged;
        if ((flags[i] & DSD_SQUELCH_FLAG_CLOSED) != 0U) {
            ++v->closed;
            v->closed_before_open += v->opened ? 0U : 1U;
        } else {
            v->opened = true;
        }
    }
}

int
counted_read(void* ctx, float* out, size_t count, int* out_got) {
    const int rc = rtl_stream_read(static_cast<RtlSdrContext*>(ctx), out, count, out_got);
    note_read(rc, nullptr, out_got);
    return rc;
}

int
counted_read_ex(void* ctx, float* out, uint8_t* flags, size_t count, int* out_got) {
    const int rc = rtl_stream_read_ex(static_cast<RtlSdrContext*>(ctx), out, flags, count, out_got);
    note_read(rc, flags, out_got);
    return rc;
}

int
counted_read_ex2(void* ctx, float* out, uint8_t* flags, size_t count, int* out_got, int wants) {
    const int rc = rtl_stream_read_ex2(static_cast<RtlSdrContext*>(ctx), out, flags, count, out_got, wants);
    note_read(rc, flags, out_got);
    return rc;
}

int
counted_read_lanes(void* ctx, float* out, float* voice, uint8_t* flags, size_t count, int* out_got, int wants) {
    const int rc = rtl_stream_read_lanes(static_cast<RtlSdrContext*>(ctx), out, voice, flags, count, out_got, wants);
    note_read(rc, flags, out_got);
    return rc;
}

int
squelch_status(const void* ctx, dsd_rtl_squelch_status* out) {
    (void)ctx;
    rtl_stream_squelch_status st{};
    if (rtl_stream_get_squelch_status(&st) != 0) {
        return -1;
    }
    out->active = st.active;
    out->noise = st.noise;
    out->quieting_valid = st.quieting_valid;
    out->quieting_db = st.quieting_db;
    out->state = st.state;
    out->gate_open = st.gate_open;
    out->plan_valid = st.plan_valid;
    out->floor_power = st.floor_power;
    out->window_power = st.window_power;
    return 0;
}

double
return_pwr(const void* ctx) {
    return rtl_stream_return_pwr(static_cast<const RtlSdrContext*>(ctx));
}

/* The speaker: decoded voice (float or s16 blocks) and the analog monitor (s16), counted on the row on air. */
void
play_digital(const dsd_opts* opts, dsd_state* state, size_t nbytes, const void* data) {
    (void)state;
    if (data == nullptr || nbytes == 0U) {
        return;
    }
    std::lock_guard<std::mutex> hold(g_lock);
    current_visit(static_cast<uint32_t>(opts->rtlsdr_center_freq))->digital_audio += nbytes / sizeof(short);
}

void
play_analog(const dsd_opts* opts, dsd_state* state, size_t nbytes, const void* data) {
    (void)state;
    if (data == nullptr || nbytes == 0U) {
        return;
    }
    const auto* pcm = static_cast<const short*>(data);
    uint64_t audible = 0U;
    for (size_t i = 0; i < nbytes / sizeof(short); ++i) {
        audible += pcm[i] != 0 ? 1U : 0U;
    }
    std::lock_guard<std::mutex> hold(g_lock);
    Visit* v = current_visit(static_cast<uint32_t>(opts->rtlsdr_center_freq));
    v->analog_audio += audible;
    v->analog_audio_while_gated += v->opened ? 0U : audible;
}

/* The monitor's UDP socket: the capture above stands in for it, so there is nothing to open. */
int
open_analog(dsd_opts* opts) {
    (void)opts;
    return 0;
}

/* After the engine installed its own hooks: the same stream reads, counted, and the speaker captured. */
int
on_start(dsd_opts* opts, dsd_state* state, void* context) {
    (void)state;
    (void)context;
    g_opts = opts;
    opts->audio_out = 1;
    opts->audio_out_type = 8;
    dsd_udp_audio_hooks audio{};
    audio.blast = play_digital;
    audio.blast_analog = play_analog;
    audio.connect_analog = open_analog;
    dsd_udp_audio_hooks_set(audio);
    dsd_rtl_stream_io_hooks io{};
    io.read = counted_read;
    io.read_ex = counted_read_ex;
    io.read_ex2 = counted_read_ex2;
    io.read_lanes = counted_read_lanes;
    io.return_pwr = return_pwr;
    io.squelch_status = squelch_status;
    io.no_stream_since_ms = rtl_stream_no_stream_since_ms;
    io.set_voice_lane = rtl_stream_set_voice_lane;
    io.voice_lane_available = rtl_stream_voice_lane_available;
    dsd_rtl_stream_io_hooks_set(&io);
    return 0;
}

void
on_stop(dsd_opts* opts, dsd_state* state, void* context) {
    (void)opts;
    (void)state;
    (void)context;
    g_opts = nullptr;
}

/* The visits of the two rows, in order. */
std::vector<Visit>
settled_visits(void) {
    std::lock_guard<std::mutex> hold(g_lock);
    std::vector<Visit> out;
    for (const auto& v : g_visits) {
        if (v.freq_hz == kDmrHz || v.freq_hz == kNfmHz) {
            out.push_back(v);
        }
    }
    return out;
}

/* Enough seen: two finished visits of each row (the last NFM one followed by another landing), or the deadline. */
void
watch(std::atomic<int>* done) {
    const uint64_t start = dsd_realtime_mono_ms();
    while (done->load() == 0) {
        dsd_sleep_ms(100U);
        const std::vector<Visit> seen = settled_visits();
        int dmr = 0;
        int nfm = 0;
        for (size_t i = 0; i + 1U < seen.size(); ++i) {
            dmr += seen[i].freq_hz == kDmrHz ? 1 : 0;
            nfm += seen[i].freq_hz == kNfmHz ? 1 : 0;
        }
        if ((dmr >= 2 && nfm >= 2) || dsd_realtime_mono_ms() - start > kDeadlineMs) {
            DSD_FPRINTF(stderr, "running receiver: stopping after %llu ms\n",
                        static_cast<unsigned long long>(dsd_realtime_mono_ms() - start));
            dsd_exitflag_store(1);
            return;
        }
    }
}

QString
writeList(const QTemporaryDir& dir, const QVariantMap& list, const QVariantList& systems) {
    const auto generated = dsd_qt::scan_list_targets(list, systems);
    if (!generated.ok || generated.targetCount != 2) {
        DSD_FPRINTF(stderr, "running receiver: the list did not generate\n");
        return QString();
    }
    DSD_FPRINTF(stderr, "running receiver: generated list:\n%s", generated.csv.constData());
    QString path = dir.filePath(QStringLiteral("rotation.csv"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(generated.csv) != generated.csv.size()) {
        return QString();
    }
    return path;
}

/* How long a visit was read for (0 for one that never brought samples). */
uint64_t
visitMs(const Visit& v) {
    return v.samples != 0U ? v.last_ms - v.first_ms : 0U;
}

void
printVisit(const Visit& v, int n, const char* note) {
    DSD_FPRINTF(
        stderr,
        "visit %d%s: %u Hz %llu ms (first samples after %llu ms), samples %llu, reads %llu flagged %llu "
        "closed %llu (before open %llu) noise-squelch reads %llu dynamic reads %llu voice %llu monitor %llu "
        "(while gated %llu)\n",
        n, note, v.freq_hz, static_cast<unsigned long long>(visitMs(v)),
        static_cast<unsigned long long>(v.samples != 0U ? v.first_data_ms - v.first_ms : 0U),
        static_cast<unsigned long long>(v.samples), static_cast<unsigned long long>(v.reads),
        static_cast<unsigned long long>(v.flagged), static_cast<unsigned long long>(v.closed),
        static_cast<unsigned long long>(v.closed_before_open), static_cast<unsigned long long>(v.noise_squelch_reads),
        static_cast<unsigned long long>(v.dynamic_reads), static_cast<unsigned long long>(v.digital_audio),
        static_cast<unsigned long long>(v.analog_audio), static_cast<unsigned long long>(v.analog_audio_while_gated));
    DSD_FPRINTF(stderr, "visit %d: reads under an off level %llu, under the session's -1 dB level %llu\n", n,
                static_cast<unsigned long long>(v.off_reads), static_cast<unsigned long long>(v.session_level_reads));
}

/* The samples the decoder read over the whole run, every visit included. */
uint64_t
samplesRead(void) {
    const std::lock_guard<std::mutex> hold(g_lock);
    uint64_t samples = 0U;
    for (const auto& v : g_visits) {
        samples += v.samples;
    }
    return samples;
}

/* The host's speed, measured apart from the receiver and the scan: the time this build takes to design the noise
   squelch's plan for a 48 kHz channel (its 2 s calibration on fixed-seed noise), the work the session's first landing
   on an analog row waits for (about 8 to 13 times this, with the receiver's cascade and threads). The median of three
   designs, so one stall of a busy host neither skips nor admits a run. */
uint64_t
planDesignMs(void) {
    const auto plan = std::make_unique<dsd_noise_squelch_plan>();
    uint64_t ms[3] = {0U, 0U, 0U};
    for (uint64_t& one : ms) {
        const uint64_t start = dsd_realtime_mono_ms();
        (void)dsd_noise_squelch_plan_design(plan.get(), nullptr, 0, nullptr, 0, 48000);
        one = dsd_realtime_mono_ms() - start;
    }
    std::sort(std::begin(ms), std::end(ms));
    return ms[1];
}

void
checkVisits(const std::vector<Visit>& seen) {
    int dmr = 0;
    int nfm = 0;
    for (size_t i = 0; i < seen.size(); ++i) {
        const Visit& v = seen[i];
        const int n = static_cast<int>(i);
        /* The last visit was cut short by the stop: shown, not judged. */
        if (i + 1U == seen.size()) {
            printVisit(v, n, " (cut by the stop)");
            break;
        }
        printVisit(v, n, "");
        if (v.freq_hz == kDmrHz) {
            ++dmr;
            expect(v.digital_audio > 0U, "the DMR row decodes voice", n);
            expect(v.reads > 0U && v.off_reads == v.reads, "the DMR row runs off", n);
            expect(v.dynamic_reads == 0U, "the DMR row runs no dynamic squelch", n);
            expect(v.closed == 0U, "the DMR row flags no sample closed", n);
            expect(v.analog_audio == 0U, "the DMR row plays no monitor audio", n);
        } else if (g_saved_level) {
            ++nfm;
            expect(v.reads > 0U && v.session_level_reads == v.reads,
                   "the NFM row runs the session's -1 dB level, the default ended", n);
            expect(v.dynamic_reads == 0U, "the NFM row runs no dynamic squelch", n);
            expect(v.analog_audio == 0U, "nothing on the NFM row plays under -1 dB", n);
        } else {
            ++nfm;
            expect(v.noise_squelch_reads > 0U && v.noise_squelch_reads == v.dynamic_reads,
                   "the NFM row runs the noise squelch (the analog default)", n);
            expect(static_cast<double>(v.closed_before_open) >= kNfmGatedAtLeastS * kOutputRateHz,
                   "the NFM row's idle receiver noise is gated", n);
            expect(v.analog_audio_while_gated == 0U, "none of the gated noise plays", n);
            expect(v.opened && static_cast<double>(v.analog_audio) >= 1.0 * kOutputRateHz, "the NFM speech plays", n);
        }
    }
    expect(dmr >= 2 && nfm >= 2, "two visits of each row in time", static_cast<int>(seen.size()));
}

/* The session the app starts for the list: the generated CSV and the scan list's argv, with the Settings default
   squelch (noise+10); empty when it does not build. */
QStringList
sessionArgs(const QTemporaryDir& dir, int port) {
    /* The digital row: a plain DMR frequency, or a saved DMR system whose saved squelch is dynamic. */
    const QVariantMap plainDmr{{"uid", "plant"}, {"kind", "freq"}, {"protocol", "dmr"}, {"freqMhz", "461.1125"}};
    const QVariantMap savedDmr{{"uid", "plant"}, {"kind", "system"}, {"systemUid", "plant-dmr"}};
    const QVariantList systems{QVariantMap{{"uid", "plant-dmr"},
                                           {"name", "Plant DMR"},
                                           {"decodeFlag", "-fs"},
                                           {"trunking", false},
                                           {"freqMhz", "461.1125"},
                                           {"radioSettings", QVariantMap{{"squelch", "auto+12"}}}}};
    const QVariantList entries{
        g_saved_level ? savedDmr : plainDmr,
        QVariantMap{{"uid", "ops"}, {"kind", "freq"}, {"protocol", "nfm"}, {"freqMhz", "154.43"}, {"dwellMs", 3500}}};
    const QVariantMap list{{"sourceType", "rtltcp"}, {"host", "127.0.0.1"}, {"port", port},
                           {"gainDb", 22},           {"bandwidthKhz", 48},  {"defaultDwellMs", 1000},
                           {"defaultHoldMs", 500},   {"entries", entries}};
    const QString path = writeList(dir, list, systems);
    dsd_qt::SessionArgPrefs prefs;
    if (g_saved_level) {
        prefs.extraArgs = QStringLiteral("--squelch -1");
    }
    QString error;
    QStringList args =
        dsd_qt::session_args_scan_build_lists(list, QStringLiteral("461.1125"), QStringList{path}, prefs, &error);
    if (path.isEmpty() || args.isEmpty() || !error.isEmpty()) {
        DSD_FPRINTF(stderr, "running receiver: no session: %s\n", error.toUtf8().constData());
        return {};
    }
    /* No sound card here: the speaker is the UDP hook the start installs. */
    const qsizetype out = args.indexOf(QStringLiteral("-o"));
    if (out >= 0 && out + 1 < args.size()) {
        args[out + 1] = QStringLiteral("null");
    }
    return args;
}

/* Bootstrap and run the engine on @p args until the watcher stops it; the engine's result. */
int
runSession(const QStringList& args) {
    DSD_FPRINTF(stderr, "running receiver: dsd-neo %s\n", args.join(QLatin1Char(' ')).toUtf8().constData());
    QList<QByteArray> storage{QByteArray("dsd-neo")};
    for (const auto& arg : args) {
        storage << arg.toUtf8();
    }
    std::vector<char*> argvv;
    for (auto& arg : storage) {
        argvv.push_back(arg.data());
    }
    argvv.push_back(nullptr);
    auto* opts = static_cast<dsd_opts*>(std::calloc(1, sizeof(dsd_opts)));
    auto* state = static_cast<dsd_state*>(std::calloc(1, sizeof(dsd_state)));
    int rc = 1;
    if (opts != nullptr && state != nullptr) {
        initOpts(opts);
        initState(state);
        std::atomic<int> done{0};
        std::thread watcher(watch, &done);
        const dsd_engine_lifecycle_hooks hooks = {on_start, on_stop, nullptr};
        if (dsd_runtime_bootstrap(static_cast<int>(storage.size()), argvv.data(), opts, state, nullptr, &rc)
            == DSD_BOOTSTRAP_CONTINUE) {
            rc = dsd_engine_run_with_lifecycle(opts, state, &hooks);
        } else {
            DSD_FPRINTF(stderr, "running receiver: the session did not start (%d)\n", rc);
            rc = 1;
        }
        done.store(1);
        watcher.join();
        freeState(state);
    }
    std::free(state);
    std::free(opts);
    return rc;
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    g_saved_level = argc > 1 && std::strcmp(argv[1], "--saved-digital-under-level") == 0;
    /* Measured before the session starts, so nothing else of the test competes with it. */
    const uint64_t design_ms = planDesignMs();
    DSD_FPRINTF(stderr, "running receiver: a noise squelch plan design takes %llu ms here\n",
                static_cast<unsigned long long>(design_ms));
    if (dsd_socket_init() != 0) {
        return 1;
    }
    /* A DMR voice call, then noise; receiver noise, NFM speech, then noise. */
    const std::vector<dsd_test::FixtureRow> rows{
        {kDmrHz, {{"dmr_voice_synth", 0.0, 0.0}, {"noise_floor", 0.0, 6.0}}},
        {kNfmHz, {{"noise_floor", 2.0, kNfmNoiseLeadS}, {"speech_nfm_synth", 0.0, 2.0}, {"noise_floor", 4.0, 4.0}}}};
    dsd_test::RtlTcpFixtureServer server;
    std::string why;
    if (!server.start(DSD_NEO_TEST_IQ_FIXTURE_DIR, rows, {{"noise_floor", 0.0, 0.0}}, &why)) {
        DSD_FPRINTF(stderr, "running receiver: fixture server: %s\n", why.c_str());
        return 1;
    }
    QTemporaryDir dir;
    const QStringList args = sessionArgs(dir, server.port());
    const int rc = args.isEmpty() ? 1 : runSession(args);
    server.stop();
    dsd_socket_cleanup();
    if (rc != 0) {
        DSD_FPRINTF(stderr, "running receiver: the engine ended with %d\n", rc);
        return 1;
    }
    const std::vector<Visit> seen = settled_visits();
    const uint64_t samples = samplesRead();
    /* A receiver that delivers nothing, or a scan that never leaves its first row, fails whatever the host's speed. */
    if (samples == 0U || seen.size() < 2U) {
        for (size_t i = 0; i < seen.size(); ++i) {
            printVisit(seen[i], static_cast<int>(i), "");
        }
        DSD_FPRINTF(stderr, "running receiver: %s\n",
                    samples == 0U ? "the receiver delivered no samples" : "the scan never left its first row");
        return 1;
    }
    if (design_ms > kPlanDesignMaxMs) {
        for (size_t i = 0; i < seen.size(); ++i) {
            printVisit(seen[i], static_cast<int>(i), "");
        }
        DSD_FPRINTF(stderr,
                    "running receiver: SKIPPED: this host designs a noise squelch plan in %llu ms (real time needs "
                    "%llu ms or less)\n",
                    static_cast<unsigned long long>(design_ms), static_cast<unsigned long long>(kPlanDesignMaxMs));
        return kSkipped;
    }
    checkVisits(seen);
    return failures == 0 ? 0 : 1;
}
