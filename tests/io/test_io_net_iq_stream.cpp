// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The raw I/Q network input as a stream backend (issue #743), through a real stream (rtl_stream_create() with an
 * iqtcp:/iqudp: spec) whose reader talks to senders on loopback in this test.
 *
 * Pinned: every block the demod takes is exactly one block of dsd_iq_live_block_samples() under a backlog, across the
 * ring's wrap, with the reader's resampler (whose plan the demodulator's squelches model, issue #899) and after a gap;
 * a ring the demod leaves full drops whole blocks with a gap mark only; a TCP reconnect, a UDP sender's re-lock and a
 * resume after a stall run the stream boundary (the discontinuity count and the output generation move, the resampler
 * starts over, the demod resets with StreamGap);
 * the stream has no stream while it reconnects or stalls, so a read waiting for samples gives way to a queued command,
 * and has one again once the first block after the boundary is written; stops are prompt in a silent read, a backoff
 * and a reconnect that hangs; every tuner setter refuses (an unchanged PPM too) and a tune fails; the samples are
 * centred, the centre is FREQ or 0, the clip level follows the format; the level is the sender's; and the reader thread
 * applies the session's floating-point mode, across a restart, which reconnects.
 *
 * Real: the stream, the device backend, the socket reader. Simulated: the senders, and the clock the reader declares a
 * stall on (rtl_net_iq_test_set_clock()), which a case advances by hand.
 */

#include <algorithm>
#include <assert.h>
#include <atomic>
#include <cstddef>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/input_level.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/halfband.h>
#include <dsd-neo/dsp/iq_resampler.h>
#include <dsd-neo/dsp/simd_fir.h>
#include <dsd-neo/io/iq_net.h>
#include <dsd-neo/io/iq_net_spec.h>
#include <dsd-neo/io/rate_plan.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/fp_mode.h>
#include <dsd-neo/runtime/input_failure.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <math.h>
#include <memory>
#include <mutex>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <utility>
#include <vector>
#include "demod_pipeline_test_support.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/io/rtl_stream_fwd.h"
#include "dsd-neo/platform/platform.h"
#include "rtl_net_iq.h"
#include "rtl_stream_test_support.h"
#include "test_env.h"

#if !DSD_PLATFORM_WIN_NATIVE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#endif

#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
#include <xmmintrin.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

struct rtl_device;

// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

namespace {

constexpr uint64_t kWaitMs = 10000U;
/* A stop or exit where every wait is a 200 ms slice: well inside this even under a sanitizer on a loaded runner. */
constexpr uint64_t kPromptMs = 1500U;
constexpr uint32_t kRate = 48000U;
constexpr uint32_t kBlock = 256U; /* dsd_iq_live_block_samples(48000) */
constexpr size_t kBlockFloats = (size_t)kBlock * 2U;

uint64_t
now_ms(void) {
    return dsd_realtime_mono_ms();
}

template <typename Pred>
bool
wait_until(Pred pred, uint64_t timeout_ms) {
    const uint64_t deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline) {
        if (pred()) {
            return true;
        }
        dsd_sleep_ms(1U);
    }
    return pred();
}

void
start_thread(dsd_thread_t* thread, dsd_thread_fn fn, void* arg) {
    const int created = dsd_thread_create(thread, fn, arg);
    assert(created == 0);
}

/* ---- Logger: counts the reader's and the stream's lines and keeps the test's output clean ---- */

enum LogKind : int {
    kLogStall,
    kLogResume,
    kLogLost,
    kLogReconnected,
    kLogMalformed,
    kLogRefused,
    kLogBindRefused,
    kLogNoPlan,
    kLogAboveCeiling,
    kLogKinds,
};

const char* const kLogTexts[kLogKinds] = {
    "I/Q stream: no samples from",
    "I/Q stream: samples from",
    "I/Q stream: lost the connection",
    "I/Q stream: reconnected to",
    "does not hold whole cs16 samples; set the sending program's packet size to a multiple of 4 bytes",
    "I/Q stream: cannot connect to",
    "I/Q stream: cannot receive on",
    "I/Q stream: cannot run at the 144000 Hz DSP rate TETRA needs",
    "the stream's rate (3200001 Hz) is above 3200000 Hz; send one channel (the sending program's VFO mode)",
};

std::atomic<int> g_log_counts[kLogKinds];

int
log_count(LogKind kind) {
    return g_log_counts[kind].load();
}

} // namespace

void
dsd_neo_log_write(dsd_neo_log_level_t level, const char* format, ...) {
    (void)level;
    char text[1024];
    va_list ap;
    va_start(ap, format);
    (void)DSD_VSNPRINTF(text, sizeof text, format, ap);
    va_end(ap);
    for (int k = 0; k < kLogKinds; k++) {
        if (strstr(text, kLogTexts[k]) != nullptr) {
            g_log_counts[k].fetch_add(1);
        }
    }
}

namespace {

/* ---- The clock the reader declares stalls on ---- */

std::atomic<uint64_t> g_clock_ms{1000000U};

uint64_t
fake_clock(void* ctx) {
    (void)ctx;
    return g_clock_ms.load();
}

/* ---- Senders ---- */

struct sockaddr_in
loopback(int port) {
    struct sockaddr_in sa;
    DSD_MEMSET(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return sa;
}

int
failure_kind(void) {
    dsd_input_failure f{};
    dsd_input_failure_get(&f);
    return f.kind;
}

int
local_port(dsd_socket_t sock) {
    struct sockaddr_in sa;
#if DSD_PLATFORM_WIN_NATIVE
    int len = (int)sizeof sa;
#else
    socklen_t len = (socklen_t)sizeof sa;
#endif
    assert(getsockname(sock, (struct sockaddr*)&sa, &len) == 0);
    return (int)ntohs(sa.sin_port);
}

/* A socket bound to an ephemeral loopback port. */
dsd_socket_t
bound_socket(int type, int* out_port) {
    const dsd_socket_t sock = dsd_socket_create(AF_INET, type, 0);
    assert(sock != DSD_INVALID_SOCKET);
    struct sockaddr_in sa = loopback(0);
    assert(dsd_socket_bind(sock, (struct sockaddr*)&sa, (int)sizeof sa) == 0);
    *out_port = local_port(sock);
    return sock;
}

/* A TCP listener on an ephemeral loopback port. */
dsd_socket_t
open_listener(int backlog, int* out_port) {
    const dsd_socket_t sock = bound_socket(SOCK_STREAM, out_port);
    assert(dsd_socket_listen(sock, backlog) == 0);
    return sock;
}

dsd_socket_t
accept_within(dsd_socket_t listener, uint64_t ms) {
    const uint64_t deadline = now_ms() + ms;
    while (now_ms() < deadline) {
        if (dsd_socket_wait(listener, DSD_SOCKET_WAIT_READ, 20U) > 0) {
            const dsd_socket_t sock = dsd_socket_accept(listener, nullptr, nullptr);
            if (sock != DSD_INVALID_SOCKET) {
                return sock;
            }
        }
    }
    return DSD_INVALID_SOCKET;
}

void
send_all(dsd_socket_t sock, const uint8_t* p, size_t n) {
    while (n > 0U) {
        const int k = dsd_socket_send(sock, p, n, MSG_NOSIGNAL);
        assert(k > 0);
        p += k;
        n -= (size_t)k;
    }
}

/* @p pairs cs16 samples of a slow tone at a quarter of full scale, continuing from *phase. */
std::vector<uint8_t>
cs16_tone(size_t pairs, uint64_t* phase) {
    std::vector<uint8_t> out(pairs * 4U);
    for (size_t k = 0U; k < pairs; k++) {
        const double a = 0.01 * (double)(*phase)++;
        const int16_t i = (int16_t)lrint(8192.0 * cos(a));
        const int16_t q = (int16_t)lrint(8192.0 * sin(a));
        out[k * 4U + 0U] = (uint8_t)((uint16_t)i & 0xFFU);
        out[k * 4U + 1U] = (uint8_t)((uint16_t)i >> 8);
        out[k * 4U + 2U] = (uint8_t)((uint16_t)q & 0xFFU);
        out[k * 4U + 3U] = (uint8_t)((uint16_t)q >> 8);
    }
    return out;
}

struct udp_sender {
    dsd_socket_t sock = DSD_INVALID_SOCKET;
    struct sockaddr_in to{};
};

udp_sender
udp_sender_open(int port) {
    udp_sender s;
    s.sock = dsd_socket_create(AF_INET, SOCK_DGRAM, 0);
    assert(s.sock != DSD_INVALID_SOCKET);
    s.to = loopback(port);
    return s;
}

void
udp_send(const udp_sender& s, const uint8_t* p, size_t n) {
    assert(dsd_socket_sendto(s.sock, p, n, 0, (const struct sockaddr*)&s.to, (int)sizeof s.to) == (int)n);
}

/* A UDP port free right now (bound and closed again): the stream binds it next. */
int
free_udp_port(void) {
    int port = 0;
    (void)dsd_socket_close(bound_socket(SOCK_DGRAM, &port));
    return port;
}

/* ---- What the demod takes ---- */

struct block_log {
    std::mutex lock;
    std::vector<size_t> floats; /* each block's float count */
    std::vector<float> samples; /* every block's floats, in order, while keep is set */
    std::atomic<int> keep{0};
    std::atomic<int> paused{0}; /* the demod waits in its next block until this is 0 */
    std::atomic<int> waiting{0};
};

block_log g_blocks;

void
on_block(const rtl_stream_test_replay_block* block, void* ctx) {
    (void)ctx;
    {
        const std::lock_guard<std::mutex> hold(g_blocks.lock);
        g_blocks.floats.push_back(block->float_count);
        if (g_blocks.keep.load() != 0) {
            g_blocks.samples.insert(g_blocks.samples.end(), block->p1, block->p1 + block->n1);
            if (block->p2 != nullptr) {
                g_blocks.samples.insert(g_blocks.samples.end(), block->p2, block->p2 + block->n2);
            }
        }
    }
    if (g_blocks.paused.load() != 0) {
        g_blocks.waiting.store(1);
        while (g_blocks.paused.load() != 0) {
            dsd_sleep_ms(1U);
        }
        g_blocks.waiting.store(0);
    }
}

/* Every block's samples after the half-band decimation (dsd_demod_test_set_post_step_tap()), on the demod thread, while
   g_blocks.keep is set. */
std::mutex g_cascade_lock;
std::vector<float> g_cascade;

void
on_cascade(const float* iq, int pairs, void* ctx) {
    (void)ctx;
    if (g_blocks.keep.load() == 0 || iq == nullptr || pairs <= 0) {
        return;
    }
    const std::lock_guard<std::mutex> hold(g_cascade_lock);
    g_cascade.insert(g_cascade.end(), iq, iq + (2U * (size_t)pairs));
}

void
blocks_reset(void) {
    const std::lock_guard<std::mutex> hold(g_blocks.lock);
    g_blocks.floats.clear();
    g_blocks.samples.clear();
    const std::lock_guard<std::mutex> cascade(g_cascade_lock);
    g_cascade.clear();
}

size_t
blocks_taken(void) {
    const std::lock_guard<std::mutex> hold(g_blocks.lock);
    return g_blocks.floats.size();
}

/* Every block taken since blocks_reset() is exactly @p floats floats. */
void
expect_blocks_exactly(size_t floats) {
    const std::lock_guard<std::mutex> hold(g_blocks.lock);
    assert(!g_blocks.floats.empty());
    for (size_t f : g_blocks.floats) {
        if (f != floats) {
            DSD_FPRINTF(stderr, "a block of %zu floats, not %zu\n", f, floats);
        }
        assert(f == floats);
    }
}

/* ---- The decoder: reads the output, gives way to a queued command ---- */

std::atomic<int> g_command_queued{0};

int
command_queued(void) {
    return g_command_queued.load();
}

struct output_reader {
    RtlSdrContext* ctx = nullptr;
    dsd_thread_t thread{};
    int started = 0;
    std::atomic<int> quit{0};
    std::atomic<int> done{0};
    std::atomic<int> yields{0};
    std::atomic<uint64_t> samples{0U};
};

DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
read_output(void* arg) {
    auto* reader = static_cast<output_reader*>(arg);
    std::vector<float> block(4096U);
    while (reader->quit.load() == 0) {
        int got = 0;
        const int rc = rtl_stream_read(reader->ctx, block.data(), block.size(), &got);
        if (rc == DSD_RTL_READ_YIELD) {
            /* The decoder applies the queued command here. */
            reader->yields.fetch_add(1);
            g_command_queued.store(0);
            continue;
        }
        if (rc != 0 || got < 0) {
            break;
        }
        reader->samples.fetch_add((uint64_t)got);
    }
    reader->done.store(1);
    DSD_THREAD_RETURN;
}

/* ---- One stream ---- */

struct stream_rig {
    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = nullptr;
    rtl_device* dev = nullptr;
    output_reader reader;
};

void
stream_open(stream_rig& r, const char* spec, int reader, void (*decoder)(dsd_opts*) = nullptr) {
    r.opts = std::make_unique<dsd_opts>();
    dsd_opts* opts = r.opts.get();
    DSD_MEMSET(opts, 0, sizeof *opts);
    opts->audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", spec);
    /* What an earlier input would have left: the stream takes its FREQ instead. */
    opts->rtlsdr_center_freq = 851012500U;
    opts->rtl_dsp_bw_khz = 48;
    opts->rtl_gain_value = 24;
    opts->frame_dmr = 1;
    if (decoder != nullptr) {
        decoder(opts);
    }
    dsd_input_failure_clear();
    dsd_exitflag_store(0);
    rtl_stream_test_set_live_block_hook(on_block, nullptr);
    assert(rtl_stream_create(opts, &r.ctx) == 0 && r.ctx != nullptr);
    assert(rtl_stream_start(r.ctx) == 0);
    r.dev = rtl_stream_test_device();
    assert(r.dev != nullptr);
    assert(rtl_device_net_iq(r.dev) != nullptr);
    if (reader != 0) {
        r.reader.ctx = r.ctx;
        start_thread(&r.reader.thread, read_output, &r.reader);
        r.reader.started = 1;
    }
}

void
stop_reader(stream_rig& r) {
    if (r.reader.started == 0) {
        return;
    }
    /* A read inside a frame waits for samples with no bound while the stream has one: the exit flag ends it (and the
       stream's reader with it; the stop follows at once). */
    r.reader.quit.store(1);
    dsd_exitflag_store(1);
    assert(wait_until([&] { return r.reader.done.load() != 0; }, kWaitMs));
    dsd_thread_join(r.reader.thread);
    r.reader.started = 0;
    r.reader.quit.store(0);
    r.reader.done.store(0);
}

/* Stop and destroy; returns how long the stop took, in ms. */
uint64_t
stream_close(stream_rig& r) {
    g_blocks.paused.store(0);
    stop_reader(r);
    const uint64_t t0 = now_ms();
    (void)rtl_stream_stop(r.ctx);
    const uint64_t took = now_ms() - t0;
    (void)rtl_stream_destroy(r.ctx);
    dsd_exitflag_store(0);
    r.ctx = nullptr;
    r.dev = nullptr;
    rtl_stream_test_set_live_block_hook(nullptr, nullptr);
    return took;
}

rtl_net_iq_state
ingest(const stream_rig& r) {
    rtl_net_iq_state st;
    rtl_net_iq_get_state(rtl_device_net_iq(r.dev), &st);
    return st;
}

std::string
tcp_spec(int port, uint32_t rate, const char* format, uint32_t freq) {
    char spec[128];
    if (freq != 0U) {
        DSD_SNPRINTF(spec, sizeof spec, "iqtcp:127.0.0.1:%d:%u:%s:%u", port, (unsigned)rate, format, (unsigned)freq);
    } else {
        DSD_SNPRINTF(spec, sizeof spec, "iqtcp:127.0.0.1:%d:%u:%s", port, (unsigned)rate, format);
    }
    return spec;
}

std::string
udp_spec(int port, uint32_t rate, const char* format) {
    char spec[128];
    DSD_SNPRINTF(spec, sizeof spec, "iqudp:127.0.0.1:%d:%u:%s", port, (unsigned)rate, format);
    return spec;
}

/* A TCP stream with its sender: the stream connects during its open, the sender accepts after. */
struct tcp_rig {
    stream_rig s;
    dsd_socket_t listener = DSD_INVALID_SOCKET;
    dsd_socket_t conn = DSD_INVALID_SOCKET;
    int port = 0;
    uint64_t phase = 0U;
};

void
tcp_open(tcp_rig& t, uint32_t rate, uint32_t freq, int reader) {
    t.listener = open_listener(8, &t.port);
    stream_open(t.s, tcp_spec(t.port, rate, "cs16", freq).c_str(), reader);
    t.conn = accept_within(t.listener, kWaitMs);
    assert(t.conn != DSD_INVALID_SOCKET);
}

void
tcp_send_pairs(tcp_rig& t, size_t pairs) {
    const std::vector<uint8_t> bytes = cs16_tone(pairs, &t.phase);
    send_all(t.conn, bytes.data(), bytes.size());
}

void
tcp_close(tcp_rig& t) {
    (void)stream_close(t.s);
    if (t.conn != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(t.conn);
    }
    if (t.listener != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(t.listener);
    }
    t.conn = DSD_INVALID_SOCKET;
    t.listener = DSD_INVALID_SOCKET;
}

/* ---- Cases ---- */

/* blocks_exactly_one_chunk: a backlog the demod drains (it paused while the sender sent 1.5 s), the ring's wrap (6 s
 * through a 2 s ring), and bursts of every size: each block the demod takes is one whole block, 2N floats. */
void
test_blocks_exactly_one_chunk(void) {
    tcp_rig t;
    tcp_open(t, kRate, 0U, 1);
    assert(rtl_net_iq_block_samples(rtl_device_net_iq(t.s.dev)) == kBlock);
    blocks_reset();
    g_blocks.paused.store(1);
    tcp_send_pairs(t, 1U);
    tcp_send_pairs(t, kBlock); /* the first block, which the demod takes and pauses in */
    assert(wait_until([] { return g_blocks.waiting.load() != 0; }, kWaitMs));
    tcp_send_pairs(t, kRate * 3U / 2U);
    assert(wait_until([&] { return ingest(t.s).blocks_written >= (uint64_t)(kRate * 3U / 2U / kBlock); }, kWaitMs));
    g_blocks.paused.store(0);
    /* Odd sizes, up to well past the ring, while the demod runs: each round waits until the demod is within 100 blocks
       of what was sent, so the ring (375 blocks) never fills however fast this build demodulates or how late the
       reader is scheduled. Measured against the blocks the reader wrote, a reader that fell behind found the rounds
       sent meanwhile waiting in the socket buffer (up to 4 MiB) and wrote them at once, past a starved demod: the full
       ring dropped blocks (on a loaded runner). */
    const size_t sizes[] = {1U, 7U, 255U, 257U, 1000U, 4096U, 30000U};
    size_t sent = 1U + kBlock + kRate * 3U / 2U;
    while (sent < (size_t)kRate * 6U) {
        assert(wait_until([&] { return blocks_taken() + 100U >= sent / kBlock; }, kWaitMs));
        for (size_t n : sizes) {
            tcp_send_pairs(t, n);
            sent += n;
        }
    }
    const uint64_t want = sent / kBlock;
    if (!wait_until([&] { return ingest(t.s).blocks_written >= want; }, kWaitMs)) {
        const rtl_net_iq_state late = ingest(t.s);
        DSD_FPRINTF(stderr,
                    "sent %zu pairs (%llu blocks): read %llu samples, %llu bytes; written %llu, dropped %llu, "
                    "boundaries %llu, restarts %llu, stalls %llu; demod took %zu\n",
                    sent, (unsigned long long)want, (unsigned long long)late.reader.samples,
                    (unsigned long long)late.reader.bytes, (unsigned long long)late.blocks_written,
                    (unsigned long long)late.blocks_dropped, (unsigned long long)late.boundaries,
                    (unsigned long long)late.input_restarts, (unsigned long long)late.reader.stalls, blocks_taken());
        assert(!"every block sent was written");
    }
    assert(wait_until([&] { return blocks_taken() >= (size_t)want; }, kWaitMs));
    expect_blocks_exactly(kBlockFloats);
    const rtl_net_iq_state st = ingest(t.s);
    assert(st.blocks_dropped == 0U);
    assert(st.boundaries == 0U);
    /* The level the stream publishes is the sender's: each component peaks at a quarter of full scale (-12.0 dBFS)
       and its RMS is 3 dB under that. */
    dsd_input_level_snapshot level{};
    assert(rtl_stream_test_input_level_read(&level) == 1);
    assert(level.source == DSD_INPUT_LEVEL_SOURCE_NET_IQ);
    assert(fabs(level.peak_dbfs - (-12.04)) < 0.1);
    assert(fabs(level.rms_dbfs - (-15.05)) < 0.1);
    tcp_close(t);
}

/* level_waits_for_a_block: a read of a few samples publishes no level of its own: a lone full-scale pair waits for the
 * reads after it, and the level comes once they span a block, measured over all of it. */
void
test_level_waits_for_a_block(void) {
    tcp_rig t;
    tcp_open(t, kRate, 0U, 1);
    const uint8_t rail[4] = {0xFFU, 0x7FU, 0xFFU, 0x7FU};
    send_all(t.conn, rail, sizeof rail);
    assert(wait_until([&] { return ingest(t.s).reader.samples >= 1U; }, kWaitMs));
    dsd_input_level_snapshot level{};
    assert(rtl_stream_test_input_level_read(&level) == 0);
    tcp_send_pairs(t, kBlock - 1U);
    assert(wait_until([&] { return rtl_stream_test_input_level_read(&level) == 1; }, kWaitMs));
    assert(level.source == DSD_INPUT_LEVEL_SOURCE_NET_IQ);
    assert(level.sample_count == (uint64_t)kBlockFloats);
    assert(fabs(level.peak_dbfs) < 0.01);
    assert(level.clip_pct > 0.0 && level.clip_pct < 1.0);
    tcp_close(t);
}

/* blocks_after_upsampling_and_gaps: a 24 kHz stream resampled to the 48 kHz DSP rate makes blocks of N, and a UDP
 * datagram the locked sender lost (not whole samples) leaves a gap mark that resets the demod at the next block, which
 * is still a whole block, while the resampler starts over. */
void
test_blocks_after_upsampling_and_gaps(void) {
    const int port = free_udp_port();
    stream_rig s;
    stream_open(s, udp_spec(port, 24000U, "cs16").c_str(), 1);
    int l = 0;
    int m = 0;
    assert(rtl_net_iq_resample_info(rtl_device_net_iq(s.dev), &l, &m, nullptr) == 1);
    assert(l == 2 && m == 1);
    assert(rtl_device_rate_plan_backend(s.dev) == DSD_RATE_PLAN_BACKEND_REPLAY_RESAMPLED);
    /* The demodulator's squelches model the stream reader's resampler (issue #899): published at the open, with no
       model on this DMR session's digital chain, which never runs a dynamic squelch. */
    rtl_stream_test_replay_state opened{};
    assert(dsd_rtl_stream_test_get_replay_state(&opened) == 0);
    assert(opened.reader_in_hz == 24000 && opened.reader_out_hz == 48000 && opened.reader_model == 0);
    blocks_reset();
    udp_sender tx = udp_sender_open(port);
    uint64_t phase = 0U;
    for (int i = 0; i < 40; i++) {
        const std::vector<uint8_t> d = cs16_tone(300U, &phase);
        udp_send(tx, d.data(), d.size());
    }
    assert(wait_until([&] { return ingest(s).blocks_written >= 80U; }, kWaitMs));
    rtl_stream_test_replay_state before{};
    assert(dsd_rtl_stream_test_get_replay_state(&before) == 0);
    const uint64_t gap_resets = before.input_gap_reset_count;
    const uint8_t odd[6] = {1, 2, 3, 4, 5, 6};
    udp_send(tx, odd, sizeof odd);
    for (int i = 0; i < 40; i++) {
        const std::vector<uint8_t> d = cs16_tone(300U, &phase);
        udp_send(tx, d.data(), d.size());
    }
    assert(wait_until([&] { return ingest(s).known_losses == 1U && ingest(s).blocks_written >= 170U; }, kWaitMs));
    assert(wait_until([&] { return blocks_taken() >= 170U; }, kWaitMs));
    rtl_stream_test_replay_state after{};
    assert(dsd_rtl_stream_test_get_replay_state(&after) == 0);
    assert(after.input_gap_reset_count > gap_resets);
    const rtl_net_iq_state st = ingest(s);
    assert(st.resampler_clears == 1U && st.boundaries == 0U);
    expect_blocks_exactly(kBlockFloats);
    /* A switch to the NFM monitor prepares the model before it is queued, with the demod parked, so the monitor's
       first blocks find it built. */
    assert(rtl_stream_request_analog_profile(DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_FM, 12500) == 0);
    rtl_stream_test_replay_state switched{};
    assert(dsd_rtl_stream_test_get_replay_state(&switched) == 0);
    assert(switched.reader_in_hz == 24000 && switched.reader_model == 1);
    (void)stream_close(s);
    (void)dsd_socket_close(tx.sock);
}

/* A ProVoice-only session (9600 bit/s, two levels), opened with CQPSK on by DSD_NEO_CQPSK=1. */
void
provoice_cqpsk(dsd_opts* opts) {
    opts->frame_dmr = 0;
    opts->frame_provoice = 1;
}

/* cqpsk_off_onto_fsk9600_prepares_the_model: a resampled ProVoice stream opened on CQPSK (DSD_NEO_CQPSK=1) runs no
 * dynamic squelch, so
 * the commit builds no reader model (issue #899). A request that only turns CQPSK off ((0, 0, 0), as a CQPSK toggle
 * queues it) keeps the 9600/2 profile and so puts the FSK path, which runs one, in force: the model is built before
 * the request is queued, whatever the request leaves out. */
void
test_cqpsk_off_onto_fsk9600_prepares_the_model(void) {
    assert(dsd_test_setenv("DSD_NEO_CQPSK", "1", 1) == 0);
    dsd_neo_config_init();
    const int port = free_udp_port();
    stream_rig s;
    stream_open(s, udp_spec(port, 24000U, "cs16").c_str(), 1, provoice_cqpsk);
    int rate = 0;
    int levels = 0;
    assert(rtl_stream_get_symbol_profile_full(&rate, &levels, nullptr) == 0);
    rtl_stream_test_replay_state opened{};
    assert(dsd_rtl_stream_test_get_replay_state(&opened) == 0);
    if (rate != 9600 || levels != 2 || opened.reader_in_hz != 24000 || opened.reader_model != 0) {
        DSD_FPRINTF(stderr, "ProVoice on CQPSK: profile %d/%d, reader %d Hz, model %d (want 9600/2, 24000, 0)\n", rate,
                    levels, opened.reader_in_hz, opened.reader_model);
        assert(0);
    }
    assert(rtl_stream_request_decode_demod_profile(0, 0, 0, -1, -1, 0) == 0);
    rtl_stream_test_replay_state switched{};
    assert(dsd_rtl_stream_test_get_replay_state(&switched) == 0);
    if (switched.reader_model != 1) {
        DSD_FPRINTF(stderr, "CQPSK off onto 9600/2: the reader model was not built before the request was queued\n");
        assert(0);
    }
    (void)stream_close(s);
    assert(dsd_test_unsetenv("DSD_NEO_CQPSK") == 0);
    dsd_neo_config_init();
}

/* hunt_over_a_replaced_cqpsk_request_prepares_the_model: two requests inside one demod block. On a resampled DMR stream
 * (4800/4, CQPSK off, no reader model), with the demod held between blocks, a CQPSK-on request is queued and a hunt
 * step (-1, 9600, 2) replaces it before the demod takes it. The demod keeps its own CQPSK (off) for the -1, so the
 * step lands the 9600 bit/s FSK path: the model must be built when the step returns, not judged against the CQPSK-on
 * request the step replaced (issue #899). */
void
test_hunt_over_a_replaced_cqpsk_request_prepares_the_model(void) {
    const int port = free_udp_port();
    stream_rig s;
    stream_open(s, udp_spec(port, 24000U, "cs16").c_str(), 1);
    rtl_stream_test_replay_state opened{};
    assert(dsd_rtl_stream_test_get_replay_state(&opened) == 0);
    assert(opened.reader_in_hz == 24000 && opened.reader_model == 0);
    blocks_reset();
    g_blocks.paused.store(1);
    udp_sender tx = udp_sender_open(port);
    uint64_t phase = 0U;
    for (int i = 0; i < 4 && g_blocks.waiting.load() == 0; i++) {
        const std::vector<uint8_t> d = cs16_tone(300U, &phase);
        udp_send(tx, d.data(), d.size());
    }
    assert(wait_until([] { return g_blocks.waiting.load() != 0; }, kWaitMs));
    assert(rtl_stream_request_demod_profile(1, 0, 0, -1, -1, 0) == 0);
    assert(rtl_stream_request_demod_profile(-1, 9600, 2, -1, -1, 0) == 0);
    rtl_stream_test_replay_state queued{};
    assert(dsd_rtl_stream_test_get_replay_state(&queued) == 0);
    const int prepared = queued.reader_model;
    g_blocks.paused.store(0);
    for (int i = 0; i < 40; i++) {
        const std::vector<uint8_t> d = cs16_tone(300U, &phase);
        udp_send(tx, d.data(), d.size());
    }
    int rate = 0;
    int levels = 0;
    int cqpsk = 1;
    const bool landed = wait_until(
        [&] {
            (void)rtl_stream_get_symbol_profile_full(&rate, &levels, nullptr);
            (void)rtl_stream_get_cqpsk_status(&cqpsk, nullptr);
            return rate == 9600 && levels == 2 && cqpsk == 0;
        },
        kWaitMs);
    if (!landed || prepared != 1) {
        DSD_FPRINTF(stderr,
                    "hunt step over a replaced CQPSK-on request: landed %d/%d CQPSK %d, model %d (want 9600/2, 0, 1)\n",
                    rate, levels, cqpsk, prepared);
        assert(0);
    }
    (void)stream_close(s);
    (void)dsd_socket_close(tx.sock);
}

/* What hunt_over_a_taken_cqpsk_off_request_prepares_the_model holds the demod on. */
std::atomic<int> g_taken_held{0};
std::atomic<int> g_idle_wait_begun{0};

/* The demod took a symbol profile: it waits there, the request taken and not applied, until a thread waits for it to
   go idle (a reader model build parking it), or 3 s. */
void
hold_taken_request(int has_demod, void* ctx) {
    (void)ctx;
    if (!has_demod || g_taken_held.exchange(1) != 0) {
        return;
    }
    const uint64_t until = now_ms() + 3000U;
    while (g_idle_wait_begun.load() == 0 && now_ms() < until) {
        dsd_sleep_ms(1U);
    }
}

void
note_idle_wait(int event, void* ctx) {
    (void)ctx;
    if (event == RTL_STREAM_TEST_DEMOD_IDLE_WAIT_BEGIN) {
        g_idle_wait_begun.store(1);
    }
}

/* hunt_over_a_taken_cqpsk_off_request_prepares_the_model: a request taken and not yet applied counts (issue #899).
 * On a resampled DMR stream opened on CQPSK (DSD_NEO_CQPSK=1: 4800/4, no reader model), a CQPSK-off request (0, 0, 0)
 * is queued and the demod takes it, then holds it unapplied (rtl_stream_test_set_profile_taken_hook()). A hunt step
 * (-1, 9600, 2) made then keeps the demod's CQPSK, which that request turns off, so it lands the 9600 bit/s FSK path:
 * the published CQPSK still reads on, and the step must build the model all the same, before it returns. Its build
 * parks the demod, whose wait releases the held request (the idle-wait probe). */
void
test_hunt_over_a_taken_cqpsk_off_request_prepares_the_model(void) {
    assert(dsd_test_setenv("DSD_NEO_CQPSK", "1", 1) == 0);
    dsd_neo_config_init();
    const int port = free_udp_port();
    stream_rig s;
    stream_open(s, udp_spec(port, 24000U, "cs16").c_str(), 1);
    int cqpsk = 0;
    (void)rtl_stream_get_cqpsk_status(&cqpsk, nullptr);
    rtl_stream_test_replay_state opened{};
    assert(dsd_rtl_stream_test_get_replay_state(&opened) == 0);
    assert(cqpsk == 1 && opened.reader_in_hz == 24000 && opened.reader_model == 0);
    g_taken_held.store(0);
    g_idle_wait_begun.store(0);
    rtl_stream_test_set_profile_taken_hook(hold_taken_request, nullptr);
    rtl_stream_test_set_demod_idle_wait_probe(note_idle_wait, nullptr);
    udp_sender tx = udp_sender_open(port);
    uint64_t phase = 0U;
    assert(rtl_stream_request_demod_profile(0, 0, 0, -1, -1, 0) == 0);
    assert(wait_until(
        [&] {
            const std::vector<uint8_t> d = cs16_tone(300U, &phase);
            udp_send(tx, d.data(), d.size());
            return g_taken_held.load() != 0;
        },
        kWaitMs));
    assert(rtl_stream_request_demod_profile(-1, 9600, 2, -1, -1, 0) == 0);
    rtl_stream_test_replay_state queued{};
    assert(dsd_rtl_stream_test_get_replay_state(&queued) == 0);
    const int prepared = queued.reader_model;
    const int released = g_idle_wait_begun.load();
    rtl_stream_test_set_profile_taken_hook(nullptr, nullptr);
    rtl_stream_test_set_demod_idle_wait_probe(nullptr, nullptr);
    int rate = 0;
    int levels = 0;
    const bool landed = wait_until(
        [&] {
            const std::vector<uint8_t> d = cs16_tone(300U, &phase);
            udp_send(tx, d.data(), d.size());
            (void)rtl_stream_get_symbol_profile_full(&rate, &levels, nullptr);
            (void)rtl_stream_get_cqpsk_status(&cqpsk, nullptr);
            return rate == 9600 && levels == 2 && cqpsk == 0;
        },
        kWaitMs);
    if (!landed || prepared != 1 || released != 1) {
        DSD_FPRINTF(stderr,
                    "hunt step over a taken CQPSK-off request: landed %d/%d CQPSK %d, model %d, demod released by the "
                    "build %d (want 9600/2, 0, 1, 1)\n",
                    rate, levels, cqpsk, prepared, released);
        assert(0);
    }
    (void)stream_close(s);
    (void)dsd_socket_close(tx.sock);
    assert(dsd_test_unsetenv("DSD_NEO_CQPSK") == 0);
    dsd_neo_config_init();
}

/* hunt_behind_a_digital_landing_prepares_the_model: a request the take pairs with a queued digital landing has its
 * CQPSK resolved as the landing's (issue #899). With DSD_NEO_CQPSK=0 a resampled DMR stream turned onto CQPSK by an
 * explicit request runs 4800/4 on CQPSK with no reader model. With the demod held between blocks, a digital family
 * landing (not the target's own CQPSK) is queued, then a hunt step (-1, 9600, 2): the consume resolves the step's -1
 * through DSD_NEO_CQPSK to off and lands the 9600 bit/s FSK path, so the step must build the model, though the stream
 * still publishes CQPSK on. */
void
test_hunt_behind_a_digital_landing_prepares_the_model(void) {
    assert(dsd_test_setenv("DSD_NEO_CQPSK", "0", 1) == 0);
    dsd_neo_config_init();
    const int port = free_udp_port();
    stream_rig s;
    stream_open(s, udp_spec(port, 24000U, "cs16").c_str(), 1);
    udp_sender tx = udp_sender_open(port);
    uint64_t phase = 0U;
    assert(rtl_stream_request_demod_profile(1, 0, 0, -1, -1, 0) == 0);
    int cqpsk = 0;
    assert(wait_until(
        [&] {
            const std::vector<uint8_t> d = cs16_tone(300U, &phase);
            udp_send(tx, d.data(), d.size());
            (void)rtl_stream_get_cqpsk_status(&cqpsk, nullptr);
            return cqpsk == 1;
        },
        kWaitMs));
    rtl_stream_test_replay_state opened{};
    assert(dsd_rtl_stream_test_get_replay_state(&opened) == 0);
    assert(opened.reader_in_hz == 24000 && opened.reader_model == 0);
    blocks_reset();
    g_blocks.paused.store(1);
    assert(wait_until(
        [&] {
            const std::vector<uint8_t> d = cs16_tone(300U, &phase);
            udp_send(tx, d.data(), d.size());
            return g_blocks.waiting.load() != 0;
        },
        kWaitMs));
    assert(rtl_stream_request_digital_family_landing(0) == 0);
    assert(rtl_stream_request_demod_profile(-1, 9600, 2, -1, -1, 0) == 0);
    rtl_stream_test_replay_state queued{};
    assert(dsd_rtl_stream_test_get_replay_state(&queued) == 0);
    const int prepared = queued.reader_model;
    g_blocks.paused.store(0);
    int rate = 0;
    int levels = 0;
    const bool landed = wait_until(
        [&] {
            const std::vector<uint8_t> d = cs16_tone(300U, &phase);
            udp_send(tx, d.data(), d.size());
            (void)rtl_stream_get_symbol_profile_full(&rate, &levels, nullptr);
            (void)rtl_stream_get_cqpsk_status(&cqpsk, nullptr);
            return rate == 9600 && levels == 2 && cqpsk == 0;
        },
        kWaitMs);
    if (!landed || prepared != 1) {
        DSD_FPRINTF(stderr, "hunt step behind a digital landing: landed %d/%d CQPSK %d, model %d (want 9600/2, 0, 1)\n",
                    rate, levels, cqpsk, prepared);
        assert(0);
    }
    (void)stream_close(s);
    (void)dsd_socket_close(tx.sock);
    assert(dsd_test_unsetenv("DSD_NEO_CQPSK") == 0);
    dsd_neo_config_init();
}

/* hunt_over_a_taken_digital_landing_prepares_the_model: the same resolution for a landing the demod took and holds
 * unapplied (rtl_stream_test_set_profile_taken_hook()). With DSD_NEO_CQPSK=0, on the CQPSK-on DMR stream, a digital
 * landing (not the target's own CQPSK) and a profile (1, 0, 0) are taken together: the consume will resolve that 1 to
 * off. A hunt step (-1, 9600, 2) made meanwhile keeps that, so it lands the FSK path and must build the model. */
void
test_hunt_over_a_taken_digital_landing_prepares_the_model(void) {
    assert(dsd_test_setenv("DSD_NEO_CQPSK", "0", 1) == 0);
    dsd_neo_config_init();
    const int port = free_udp_port();
    stream_rig s;
    stream_open(s, udp_spec(port, 24000U, "cs16").c_str(), 1);
    udp_sender tx = udp_sender_open(port);
    uint64_t phase = 0U;
    assert(rtl_stream_request_demod_profile(1, 0, 0, -1, -1, 0) == 0);
    int cqpsk = 0;
    assert(wait_until(
        [&] {
            const std::vector<uint8_t> d = cs16_tone(300U, &phase);
            udp_send(tx, d.data(), d.size());
            (void)rtl_stream_get_cqpsk_status(&cqpsk, nullptr);
            return cqpsk == 1;
        },
        kWaitMs));
    g_taken_held.store(0);
    g_idle_wait_begun.store(0);
    rtl_stream_test_set_profile_taken_hook(hold_taken_request, nullptr);
    rtl_stream_test_set_demod_idle_wait_probe(note_idle_wait, nullptr);
    assert(rtl_stream_request_digital_family_landing(0) == 0);
    assert(rtl_stream_request_demod_profile(1, 0, 0, -1, -1, 0) == 0);
    assert(wait_until(
        [&] {
            const std::vector<uint8_t> d = cs16_tone(300U, &phase);
            udp_send(tx, d.data(), d.size());
            return g_taken_held.load() != 0;
        },
        kWaitMs));
    assert(rtl_stream_request_demod_profile(-1, 9600, 2, -1, -1, 0) == 0);
    rtl_stream_test_replay_state queued{};
    assert(dsd_rtl_stream_test_get_replay_state(&queued) == 0);
    const int prepared = queued.reader_model;
    rtl_stream_test_set_profile_taken_hook(nullptr, nullptr);
    rtl_stream_test_set_demod_idle_wait_probe(nullptr, nullptr);
    int rate = 0;
    int levels = 0;
    const bool landed = wait_until(
        [&] {
            const std::vector<uint8_t> d = cs16_tone(300U, &phase);
            udp_send(tx, d.data(), d.size());
            (void)rtl_stream_get_symbol_profile_full(&rate, &levels, nullptr);
            (void)rtl_stream_get_cqpsk_status(&cqpsk, nullptr);
            return rate == 9600 && levels == 2 && cqpsk == 0;
        },
        kWaitMs);
    if (!landed || prepared != 1) {
        DSD_FPRINTF(stderr,
                    "hunt step over a taken digital landing: landed %d/%d CQPSK %d, model %d (want 9600/2, 0, 1)\n",
                    rate, levels, cqpsk, prepared);
        assert(0);
    }
    (void)stream_close(s);
    (void)dsd_socket_close(tx.sock);
    assert(dsd_test_unsetenv("DSD_NEO_CQPSK") == 0);
    dsd_neo_config_init();
}

/* Send tone blocks until @p done holds (kWaitMs). */
template <typename Done>
bool
send_until(udp_sender& tx, uint64_t* phase, Done done) {
    return wait_until(
        [&] {
            const std::vector<uint8_t> d = cs16_tone(300U, phase);
            udp_send(tx, d.data(), d.size());
            return done();
        },
        kWaitMs);
}

/* Whether the stream runs @p rate / @p levels with CQPSK @p cqpsk_want. */
bool
profile_is(int rate_want, int levels_want, int cqpsk_want) {
    int rate = 0;
    int levels = 0;
    int cqpsk = -1;
    (void)rtl_stream_get_symbol_profile_full(&rate, &levels, nullptr);
    (void)rtl_stream_get_cqpsk_status(&cqpsk, nullptr);
    return rate == rate_want && levels == levels_want && cqpsk == cqpsk_want;
}

/* ted_only_onto_fsk9600_prepares_the_model: a request that sets only the TED (-1, 0, 0, -1, 5, 0) changes the symbol
 * rate too: both TED setters set the profile to the complex rate over the TED, keeping two levels two (issue #899).
 * On a resampled DMR stream (24 kHz read up to a 48 kHz demod) put on 4800/2 with CQPSK off (TED 10, no reader model:
 * 4800/2 runs no dynamic squelch), the request lands 48000 / 5 = 9600/2, the FSK path that runs one: the model must be
 * built before the request returns. */
void
test_ted_only_onto_fsk9600_prepares_the_model(void) {
    const int port = free_udp_port();
    stream_rig s;
    stream_open(s, udp_spec(port, 24000U, "cs16").c_str(), 1);
    udp_sender tx = udp_sender_open(port);
    uint64_t phase = 0U;
    assert(rtl_stream_request_demod_profile(-1, 4800, 2, -1, -1, 0) == 0);
    assert(send_until(tx, &phase, [] { return profile_is(4800, 2, 0); }));
    rtl_stream_test_replay_state opened{};
    assert(dsd_rtl_stream_test_get_replay_state(&opened) == 0);
    assert(opened.reader_in_hz == 24000 && opened.reader_model == 0 && rtl_stream_get_ted_sps() == 10);
    assert(rtl_stream_request_demod_profile(-1, 0, 0, -1, 5, 0) == 0);
    rtl_stream_test_replay_state queued{};
    assert(dsd_rtl_stream_test_get_replay_state(&queued) == 0);
    const int prepared = queued.reader_model;
    const bool landed = send_until(tx, &phase, [] { return profile_is(9600, 2, 0); });
    if (!landed || prepared != 1) {
        DSD_FPRINTF(stderr, "TED 5 onto 4800/2 at 48 kHz: landed 9600/2 %d, model %d (want 1, 1)\n", landed ? 1 : 0,
                    prepared);
        assert(0);
    }
    (void)stream_close(s);
    (void)dsd_socket_close(tx.sock);
}

/* cqpsk_off_over_a_taken_ted_only_request_prepares_the_model: the same rate for a TED-only request the demod took and
 * holds unapplied (issue #899). On the resampled DMR stream put on CQPSK at 4800/2 ((1, 4800, 2): CQPSK on, so no
 * reader model), a TED-only request (-1, 0, 0, -1, 5, 0) keeps CQPSK on and lands no FSK path; the demod takes it and
 * holds it. A CQPSK-off request (0, 0, 0) made then keeps the rate that request lands, 9600/2, so it lands the FSK
 * path: the published profile still reads 4800, and the request must build the model all the same. */
void
test_cqpsk_off_over_a_taken_ted_only_request_prepares_the_model(void) {
    const int port = free_udp_port();
    stream_rig s;
    stream_open(s, udp_spec(port, 24000U, "cs16").c_str(), 1);
    udp_sender tx = udp_sender_open(port);
    uint64_t phase = 0U;
    assert(rtl_stream_request_demod_profile(1, 4800, 2, -1, -1, 0) == 0);
    assert(send_until(tx, &phase, [] { return profile_is(4800, 2, 1); }));
    rtl_stream_test_replay_state opened{};
    assert(dsd_rtl_stream_test_get_replay_state(&opened) == 0);
    assert(opened.reader_in_hz == 24000 && opened.reader_model == 0);
    g_taken_held.store(0);
    g_idle_wait_begun.store(0);
    rtl_stream_test_set_profile_taken_hook(hold_taken_request, nullptr);
    rtl_stream_test_set_demod_idle_wait_probe(note_idle_wait, nullptr);
    assert(rtl_stream_request_demod_profile(-1, 0, 0, -1, 5, 0) == 0);
    rtl_stream_test_replay_state ted_queued{};
    assert(dsd_rtl_stream_test_get_replay_state(&ted_queued) == 0);
    assert(ted_queued.reader_model == 0);
    assert(send_until(tx, &phase, [] { return g_taken_held.load() != 0; }));
    assert(rtl_stream_request_demod_profile(0, 0, 0, -1, -1, 0) == 0);
    rtl_stream_test_replay_state queued{};
    assert(dsd_rtl_stream_test_get_replay_state(&queued) == 0);
    const int prepared = queued.reader_model;
    const int released = g_idle_wait_begun.load();
    rtl_stream_test_set_profile_taken_hook(nullptr, nullptr);
    rtl_stream_test_set_demod_idle_wait_probe(nullptr, nullptr);
    const bool landed = send_until(tx, &phase, [] { return profile_is(9600, 2, 0); });
    if (!landed || prepared != 1 || released != 1) {
        DSD_FPRINTF(stderr,
                    "CQPSK off over a taken TED 5 request: landed 9600/2 %d, model %d, demod released by the build %d "
                    "(want 1, 1, 1)\n",
                    landed ? 1 : 0, prepared, released);
        assert(0);
    }
    (void)stream_close(s);
    (void)dsd_socket_close(tx.sock);
}

/* ring_full_drops_whole_blocks: a demod that stops taking input leaves the ring full; the reader drops whole blocks,
 * each a gap mark only: no boundary, no discontinuity, no output generation move, the resampler kept. Blocks taken after
 * are still whole. */
void
test_ring_full_drops_whole_blocks(void) {
    tcp_rig t;
    tcp_open(t, kRate, 0U, 1);
    blocks_reset();
    g_blocks.paused.store(1);
    tcp_send_pairs(t, kBlock);
    assert(wait_until([] { return g_blocks.waiting.load() != 0; }, kWaitMs));
    const uint64_t seq = rtl_stream_discontinuity_seq();
    const uint32_t gen = rtl_stream_output_generation();
    rtl_stream_test_live_sequence live_before{};
    rtl_stream_test_get_live_sequence(&live_before);
    rtl_stream_test_replay_state gaps_before{};
    assert(dsd_rtl_stream_test_get_replay_state(&gaps_before) == 0);
    tcp_send_pairs(t, (size_t)kRate * 3U);
    assert(wait_until([&] { return ingest(t.s).blocks_dropped > 0U; }, kWaitMs));
    rtl_net_iq_state st = ingest(t.s);
    /* The ring holds 2 s in whole blocks, the paused one aside. */
    assert(st.blocks_written <= (uint64_t)(2U * kRate / kBlock) + 2U);
    assert(st.boundaries == 0U && st.input_restarts == 0U);
    assert(rtl_stream_discontinuity_seq() == seq);
    assert(rtl_stream_output_generation() == gen);
    rtl_stream_test_live_sequence live_after{};
    rtl_stream_test_get_live_sequence(&live_after);
    assert(live_after.producer_drops >= live_before.producer_drops + kBlockFloats * st.blocks_dropped);
    /* Once the demod drained the ring, the blocks after the drops are whole too. */
    g_blocks.paused.store(0);
    assert(wait_until([&] { return blocks_taken() >= (size_t)ingest(t.s).blocks_written; }, kWaitMs));
    const size_t taken = blocks_taken();
    tcp_send_pairs(t, kRate / 2U);
    assert(wait_until([&] { return blocks_taken() >= taken + (size_t)(kRate / 2U / kBlock); }, kWaitMs));
    expect_blocks_exactly(kBlockFloats);
    /* The drops left a gap mark, and the demod reset at it once it drained to it. */
    rtl_stream_test_replay_state gaps_after{};
    assert(dsd_rtl_stream_test_get_replay_state(&gaps_after) == 0);
    assert(gaps_after.input_gap_reset_count > gaps_before.input_gap_reset_count);
    tcp_close(t);
}

/* The NFM monitor at a 16 kHz DSP bandwidth: a 112 kHz stream is read into 64 kHz ahead of two passes. */
void
nfm_16k(dsd_opts* opts) {
    opts->frame_dmr = 0;
    opts->analog_only = 1;
    opts->rtl_dsp_bw_khz = 16;
}

/* @p pairs complex cf32 samples of noise from @p seed (xorshift, uniform in +-0.25), appended to @p src. */
void
cf32_noise(std::vector<float>* src, size_t pairs, uint64_t* seed) {
    for (size_t k = 0U; k < 2U * pairs; k++) {
        *seed ^= *seed << 13;
        *seed ^= *seed >> 7;
        *seed ^= *seed << 17;
        src->push_back((float)((double)(*seed >> 11) * (1.0 / 9007199254740992.0) - 0.5) * 0.5f);
    }
}

void
tcp_send_floats(tcp_rig& t, const float* f, size_t pairs) {
    send_all(t.conn, reinterpret_cast<const uint8_t*>(f), pairs * 2U * sizeof(float));
}

/* The ingest done with @p pairs received samples: their callbacks returned, so the counts are final. */
rtl_net_iq_state
ingest_after(const stream_rig& s, uint64_t pairs) {
    assert(wait_until([&] { return ingest(s).samples_ingested >= pairs; }, kWaitMs));
    return ingest(s);
}

/* A reader of @p plan started fresh (cleared, as the ingest restarts it) on @p src from pair @p from, cut as the ingest
   cuts it: its first @p n outputs into @p out. @p r is configured for the plan once and reused. */
void
fresh_reader(dsd_iq_resampler* r, const std::vector<float>& src, size_t from, size_t n, std::vector<float>* out) {
    dsd_iq_resampler_clear(r);
    out->clear();
    std::vector<float> buf(2U * dsd_iq_resampler_max_out(r, 4096U));
    size_t at = from;
    while (at < src.size() / 2U && out->size() < 2U * n) {
        /* Enough inputs for the outputs still wanted, at most a slice: the output bits do not depend on the cut. */
        const size_t want_in =
            (((n - out->size() / 2U) * (size_t)r->M) / (size_t)r->L) + (size_t)r->taps_per_phase + 2U;
        const size_t k = std::min<size_t>(std::min<size_t>(4096U, want_in), src.size() / 2U - at);
        const int made = dsd_iq_resampler_process(r, src.data() + 2U * at, k, buf.data(), buf.size() / 2U);
        assert(made >= 0);
        out->insert(out->end(), buf.begin(), buf.begin() + (std::ptrdiff_t)(2U * (size_t)made));
        at += k;
    }
    if (out->size() > 2U * n) {
        out->resize(2U * n);
    }
}

/* The decoder's half-band cascade of @p passes passes (0 to 2) from its start (zero history, nothing pending), fed block
   by block as the demod is (its SIMD sums round by where a block ends). */
struct FreshCascade {
    int passes = 0;
    float hist_i[2][30] = {}; /* the longest stage, 31 taps */
    float hist_q[2][30] = {};
    int pending[2] = {0, 0};

    explicit FreshCascade(int n) : passes(n) { assert(n >= 0 && n <= 2); }

    void
    run(const float* in, size_t floats, std::vector<float>* out) {
        std::vector<float> cur(in, in + floats);
        for (int i = 0; i < passes; i++) {
            std::vector<float> next(cur.size() / 2U + 2U);
            const int len =
                simd_hb_decim2_complex(cur.data(), (int)cur.size(), next.data(), hist_i[i], hist_q[i], &pending[i],
                                       i == 0 ? hb31_q15_taps : hb_q15_taps, i == 0 ? 31 : HB_TAPS);
            assert(len >= 0);
            next.resize((size_t)len);
            cur.swap(next);
        }
        out->insert(out->end(), cur.begin(), cur.end());
    }
};

/* Where the ingest last started @p r over when it wrote @p written blocks of @p block_pairs from @p inputs inputs of
   @p src and dropped every full block after them: the input whose outputs a dropped block ends inside (its outputs
   past the block start the next one), else the input after the block. Fed one input at a time, as the slices end where
   a block does. */
size_t
last_restart_input(dsd_iq_resampler* r, const std::vector<float>& src, size_t inputs, size_t block_pairs,
                   uint64_t written) {
    dsd_iq_resampler_clear(r);
    std::vector<float> buf(2U * dsd_iq_resampler_max_out(r, 1U));
    size_t fill = 0U;
    uint64_t blocks = 0U;
    size_t restart = 0U;
    size_t i = 0U;
    while (i < inputs) {
        const int made = dsd_iq_resampler_process(r, src.data() + 2U * i, 1U, buf.data(), buf.size() / 2U);
        assert(made >= 0);
        if (fill + (size_t)made < block_pairs) {
            fill += (size_t)made;
            i++;
            continue;
        }
        const size_t surplus = fill + (size_t)made - block_pairs;
        blocks++;
        fill = surplus;
        if (blocks > written) {
            dsd_iq_resampler_clear(r);
            restart = surplus > 0U ? i : i + 1U;
            if (surplus > 0U) {
                fill = 0U;
                continue; /* the cleared reader takes this input again */
            }
        }
        i++;
    }
    return restart;
}

/* One ring-full restart case: a reader of @p in_hz into @p out_hz (L/M as given) ahead of @p passes half-bands, @p held
   pairs of noise sent while the demod is held. */
struct ring_case {
    uint32_t in_hz;
    int out_hz;
    int L;
    int M;
    int passes;
    void (*decoder)(dsd_opts*);
    size_t held;
};

/* Each block the full ring drops starts the reader over (issue #899), as the demod starts its half-bands over at the
 * drop's gap mark. With the demod held, noise fills the ring and drops blocks; once it drains, known noise follows.
 * Everything the demod takes after the drops' gap mark is then the output of a reader started fresh on the source where
 * the last drop left it, sample for sample, which a reader that kept running across the drops is not: the decimators,
 * started over at the mark, keep the reader phases the squelch's model describes. The counts are read once every
 * callback returned: every drop started the reader over and cost its block alone, with no boundary, restart or output
 * generation move. */
void
ring_full_restart_case(const ring_case& c) {
    tcp_rig t;
    t.listener = open_listener(8, &t.port);
    dsd_demod_test_set_post_step_tap(on_cascade, nullptr);
    stream_open(t.s, tcp_spec(t.port, c.in_hz, "cf32", 0U).c_str(), 1, c.decoder);
    t.conn = accept_within(t.listener, kWaitMs);
    assert(t.conn != DSD_INVALID_SOCKET);
    rtl_stream_test_replay_state opened{};
    assert(dsd_rtl_stream_test_get_replay_state(&opened) == 0);
    assert(opened.reader_in_hz == (int)c.in_hz && opened.reader_out_hz == c.out_hz);
    dsd_iq_reader_plan plan;
    assert(
        dsd_iq_reader_plan_for(&plan, c.in_hz, (uint32_t)c.out_hz, (uint64_t)((uint32_t)c.out_hz >> (unsigned)c.passes))
            == 0
        && plan.enabled);
    dsd_iq_resampler reader;
    dsd_iq_resampler_init(&reader);
    assert(dsd_iq_reader_plan_configure(&plan, &reader, 4096U) == DSD_IQ_RESAMPLER_OK && reader.L == c.L
           && reader.M == c.M);
    const size_t block_pairs = (size_t)rtl_net_iq_block_samples(rtl_device_net_iq(t.s.dev));
    const size_t block_floats = 2U * block_pairs;
    rtl_stream_test_live_sequence live_before{};
    rtl_stream_test_get_live_sequence(&live_before);

    std::vector<float> src;
    uint64_t seed = 0x899C5EEDULL;
    blocks_reset();
    g_blocks.keep.store(1);
    g_blocks.paused.store(1);
    cf32_noise(&src, 4096U, &seed);
    tcp_send_floats(t, src.data(), 4096U);
    assert(wait_until([] { return g_blocks.waiting.load() != 0; }, kWaitMs));
    const uint64_t seq = rtl_stream_discontinuity_seq();
    const uint32_t gen = rtl_stream_output_generation();
    rtl_stream_test_replay_state gaps_before{};
    assert(dsd_rtl_stream_test_get_replay_state(&gaps_before) == 0);
    cf32_noise(&src, c.held, &seed);
    tcp_send_floats(t, src.data() + ((size_t)2U * 4096U), c.held);
    const rtl_net_iq_state st = ingest_after(t.s, 4096U + c.held);
    if (st.blocks_dropped == 0U || st.drop_clears != st.blocks_dropped) {
        DSD_FPRINTF(stderr, "resampled stream, ring full: %llu blocks dropped, the resampler started over %llu times\n",
                    (unsigned long long)st.blocks_dropped, (unsigned long long)st.drop_clears);
        assert(0);
    }
    assert(st.boundaries == 0U && st.input_restarts == 0U && st.resampler_clears == 0U);
    /* Each drop cost its block alone: the resampler's calls end where a block does, and an input whose outputs run
       past a dropped block starts the next one, so nothing after the block went too. */
    rtl_stream_test_live_sequence live_after{};
    rtl_stream_test_get_live_sequence(&live_after);
    if (live_after.producer_drops - live_before.producer_drops != st.blocks_dropped * block_floats) {
        DSD_FPRINTF(stderr, "%llu blocks dropped took %llu floats, not %llu\n", (unsigned long long)st.blocks_dropped,
                    (unsigned long long)(live_after.producer_drops - live_before.producer_drops),
                    (unsigned long long)(st.blocks_dropped * block_floats));
        assert(0);
    }
    assert(rtl_stream_discontinuity_seq() == seq);
    assert(rtl_stream_output_generation() == gen);
    /* Every block written before the first drop, taken; then the known noise after the drops. */
    const size_t before_gap = (size_t)st.blocks_written * block_floats;
    g_blocks.paused.store(0);
    assert(wait_until([&] { return blocks_taken() >= (size_t)ingest(t.s).blocks_written; }, kWaitMs));
    const size_t tail = c.in_hz / 2U;
    const size_t known_from = src.size() / 2U;
    cf32_noise(&src, tail, &seed);
    tcp_send_floats(t, src.data() + 2U * known_from, tail);
    const rtl_net_iq_state after = ingest_after(t.s, 4096U + c.held + tail);
    assert(after.blocks_dropped == st.blocks_dropped);
    assert(wait_until([&] { return blocks_taken() >= (size_t)ingest(t.s).blocks_written; }, kWaitMs));
    std::vector<float> taken;
    std::vector<size_t> cuts;
    {
        const std::lock_guard<std::mutex> hold(g_blocks.lock);
        taken = g_blocks.samples;
        cuts = g_blocks.floats;
    }
    assert(taken.size() > before_gap + ((size_t)2U * 2048U));
    const std::vector<float> post(taken.begin() + (std::ptrdiff_t)before_gap, taken.end());
    /* The reader started over where the last drop left it, and less than a block followed it before the known noise (a
       whole one would have dropped too): search that one partial block's inputs, at the reader's M/L inputs per
       output, then hold the whole post-gap stream to the start found, which must be the one the drops make. */
    std::vector<float> fresh;
    size_t found = SIZE_MAX;
    const size_t span = (block_pairs * (size_t)reader.M) / (size_t)reader.L + 2U;
    for (size_t from = known_from > span ? known_from - span : 0U; from <= known_from && found == SIZE_MAX; from++) {
        fresh_reader(&reader, src, from, 32U, &fresh);
        if (fresh.size() == 64U && std::equal(fresh.begin(), fresh.end(), post.begin())) {
            found = from;
        }
    }
    const size_t restart = last_restart_input(&reader, src, known_from, block_pairs, st.blocks_written);
    if (found != restart) {
        DSD_FPRINTF(stderr,
                    "after the drops the demod took a fresh reader's output from input %zu, not %zu (SIZE_MAX: none, "
                    "the reader kept running)\n",
                    found, restart);
        assert(0);
    }
    fresh_reader(&reader, src, found, post.size() / 2U, &fresh);
    assert(fresh.size() == post.size() && std::equal(fresh.begin(), fresh.end(), post.begin()));
    rtl_stream_test_replay_state gaps_after{};
    assert(dsd_rtl_stream_test_get_replay_state(&gaps_after) == 0);
    assert(gaps_after.input_gap_reset_count > gaps_before.input_gap_reset_count);
    /* And the half-bands started over at that first sample, keeping the reader phases a fresh start keeps: the demod's
       post-decimation output from the gap on is a fresh cascade over the fresh reader, sample for sample, right after
       the outputs the pre-gap input makes from the stream's start. A reset a block late would keep another phase. The
       expected outputs come from the blocks' cuts; the tap runs after the block hook, so the copy waits for it to have
       published that many. */
    FreshCascade from_open(c.passes);
    FreshCascade from_gap(c.passes);
    std::vector<float> pre_cascade;
    std::vector<float> want;
    size_t at = 0U;
    for (const size_t k : cuts) {
        if (at < before_gap) {
            assert(at + k <= before_gap); /* a read stops at the gap mark */
            from_open.run(taken.data() + at, k, &pre_cascade);
        } else {
            from_gap.run(fresh.data() + (at - before_gap), k, &want);
        }
        at += k;
    }
    const size_t pre_out = pre_cascade.size();
    const size_t expected = pre_out + want.size();
    (void)wait_until(
        [&] {
            const std::lock_guard<std::mutex> hold(g_cascade_lock);
            return g_cascade.size() >= expected;
        },
        kWaitMs);
    std::vector<float> cascade;
    {
        const std::lock_guard<std::mutex> hold(g_cascade_lock);
        cascade = g_cascade;
    }
    assert(cascade.size() >= pre_out && std::equal(pre_cascade.begin(), pre_cascade.end(), cascade.begin()));
    const size_t have = cascade.size() > pre_out ? cascade.size() - pre_out : 0U;
    if (have < want.size() || want.size() < (size_t)2U * 512U
        || !std::equal(want.begin(), want.end(), cascade.begin() + (std::ptrdiff_t)pre_out)) {
        DSD_FPRINTF(stderr,
                    "the decimated stream after the gap (%zu floats from %zu) is not a fresh cascade over the fresh "
                    "reader (%zu floats)\n",
                    have, pre_out, want.size());
        assert(0);
    }
    g_blocks.keep.store(0);
    dsd_iq_resampler_free(&reader);
    tcp_close(t);
    dsd_demod_test_set_post_step_tap(nullptr, nullptr);
}

/* ring_full_restarts_the_reader: a reader whose decimation keeps a coset of its phases (112 kHz read into 64 kHz ahead
 * of two passes: L/M 4/7, every fourth output kept). Its slices never make more than the block's room. */
void
test_ring_full_restarts_the_reader(void) {
    ring_full_restart_case(ring_case{112000U, 64000, 4, 7, 2, nfm_16k, (size_t)112000U * 3U});
}

/* ring_full_restarts_an_upsampling_reader: a reader that makes six outputs of each input (8 kHz read into 48 kHz for a
 * ProVoice session, its FSK path at 9600 bit/s, no half-band pass). A 256-pair block ends inside an input two times in
 * three: those outputs past the block start the next one, from the reader started over on that input, so a drop costs
 * its block alone (not its block and the 2 or 4 outputs after it) and the input is not lost. */
void
test_ring_full_restarts_an_upsampling_reader(void) {
    ring_full_restart_case(ring_case{8000U, 48000, 6, 1, 0, provoice_cqpsk, (size_t)8000U * 8U});
}

/* What a boundary must have moved since @p seq / @p gen. */
void
expect_boundary(const stream_rig& s, uint64_t seq, uint32_t gen, uint64_t boundaries) {
    assert(wait_until([&] { return ingest(s).boundaries == boundaries; }, kWaitMs));
    assert(rtl_stream_discontinuity_seq() > seq);
    assert(rtl_stream_output_generation() != gen);
    rtl_stream_test_reset_plan plan{};
    assert(rtl_stream_test_get_last_reset_plan(&plan) == 0);
    assert(strcmp(plan.reason, "stream-gap") == 0);
}

/* The read waits for samples while the stream has one, whatever is queued, and gives way to a queued command while it
 * has none. */
void
expect_yields_without_stream(stream_rig& s) {
    const int yields = s.reader.yields.load();
    g_command_queued.store(1);
    assert(wait_until([&] { return s.reader.yields.load() > yields; }, kWaitMs));
    assert(g_command_queued.load() == 0);
}

/* tcp_reconnect_runs_the_boundary: the sender drops the connection: no stream until the new one is up and has written
 * a block (a queued command runs meanwhile); the reconnect's boundary moves the count and the generation and the
 * resampler starts over; then blocks go on whole. */
void
test_tcp_reconnect_runs_the_boundary(void) {
    tcp_rig t;
    t.listener = open_listener(8, &t.port);
    stream_open(t.s, tcp_spec(t.port, 24000U, "cs16", 426700000U).c_str(), 1);
    t.conn = accept_within(t.listener, kWaitMs);
    assert(t.conn != DSD_INVALID_SOCKET);
    blocks_reset();
    tcp_send_pairs(t, 4800U);
    assert(wait_until([&] { return rtl_device_has_stream(t.s.dev) == 1; }, kWaitMs));
    /* The demod takes everything before the loss, so the ring is empty at the boundary. */
    assert(wait_until([&] { return ingest(t.s).reader.samples >= 4800U; }, kWaitMs));
    assert(wait_until([&] { return blocks_taken() >= (size_t)ingest(t.s).blocks_written; }, kWaitMs));
    const uint64_t seq = rtl_stream_discontinuity_seq();
    const uint32_t gen = rtl_stream_output_generation();
    (void)dsd_socket_close(t.conn);
    assert(wait_until([&] { return rtl_device_has_stream(t.s.dev) == 0; }, kWaitMs));
    expect_yields_without_stream(t.s);
    t.conn = accept_within(t.listener, kWaitMs);
    assert(t.conn != DSD_INVALID_SOCKET);
    expect_boundary(t.s, seq, gen, 1U);
    /* Up, but nothing written since the boundary: still no stream. */
    assert(rtl_device_has_stream(t.s.dev) == 0);
    const rtl_net_iq_state st = ingest(t.s);
    assert(st.resampler_clears >= 1U);
    /* The ring was empty at the boundary, so its purge has nothing to discard: every block written after it reaches
       the demod, those queued while the demod holds the first one included (controller_net_iq_boundary() lets the
       purge go; left pending, the demod would apply it after that first block and drop the rest). */
    blocks_reset();
    g_blocks.paused.store(1);
    const uint64_t samples_before = st.reader.samples;
    tcp_send_pairs(t, 4800U);
    assert(wait_until([] { return g_blocks.waiting.load() != 0; }, kWaitMs));
    /* Every sample read and most of its blocks queued behind the one the demod holds. */
    assert(wait_until([&] { return ingest(t.s).reader.samples >= samples_before + 4800U; }, kWaitMs));
    assert(wait_until([&] { return ingest(t.s).blocks_written >= st.blocks_written + 30U; }, kWaitMs));
    g_blocks.paused.store(0);
    /* A purge left pending would discard the queued blocks, and the demod would never catch up with the reader. */
    assert(wait_until([&] { return blocks_taken() == (size_t)(ingest(t.s).blocks_written - st.blocks_written); },
                      kWaitMs));
    assert(wait_until([&] { return rtl_device_has_stream(t.s.dev) == 1; }, kWaitMs));
    expect_blocks_exactly(kBlockFloats);
    uint32_t centre = 0U;
    assert(rtl_stream_get_last_applied_freq(&centre) == 0 && centre == 426700000U);
    tcp_close(t);
}

/* level_starts_over_at_a_reconnect: the samples a level block was collecting before a reconnect are not the new
 * connection's: a full-scale pair read before the drop is let go at the boundary, so the first level after it waits for
 * a whole block of the new connection's samples and reports their level, unclipped. Before, the pair stayed in the
 * block, which the new connection completed a sample early and published clipped at 0 dBFS. */
void
test_level_starts_over_at_a_reconnect(void) {
    tcp_rig t;
    tcp_open(t, kRate, 0U, 1);
    const uint8_t rail[4] = {0xFFU, 0x7FU, 0xFFU, 0x7FU};
    send_all(t.conn, rail, sizeof rail);
    assert(wait_until([&] { return ingest(t.s).reader.samples >= 1U; }, kWaitMs));
    const uint64_t seq = rtl_stream_discontinuity_seq();
    const uint32_t gen = rtl_stream_output_generation();
    (void)dsd_socket_close(t.conn);
    t.conn = accept_within(t.listener, kWaitMs);
    assert(t.conn != DSD_INVALID_SOCKET);
    expect_boundary(t.s, seq, gen, 1U);
    const uint64_t samples_before = ingest(t.s).reader.samples;
    dsd_input_level_snapshot level{};
    tcp_send_pairs(t, kBlock - 1U);
    assert(wait_until([&] { return ingest(t.s).reader.samples >= samples_before + kBlock - 1U; }, kWaitMs));
    assert(rtl_stream_test_input_level_read(&level) == 0);
    tcp_send_pairs(t, 1U);
    assert(wait_until([&] { return rtl_stream_test_input_level_read(&level) == 1; }, kWaitMs));
    assert(level.sample_count == (uint64_t)kBlockFloats);
    assert(fabs(level.peak_dbfs - (-12.04)) < 0.1);
    assert(level.clip_pct < 1e-9);
    tcp_close(t);
}

/* tcp_stall_resume_runs_the_boundary: a connection that stays up but goes quiet for 2 s (the reader's clock) has no
 * stream, so a queued command runs; samples again run the resume boundary first, and the stream is back once a block is
 * written. */
void
test_tcp_stall_resume_runs_the_boundary(void) {
    rtl_net_iq_test_set_clock(fake_clock, nullptr);
    tcp_rig t;
    tcp_open(t, kRate, 0U, 1);
    tcp_send_pairs(t, 4800U);
    /* All of it read before the clock moves on, so none of it arrives after the stall. */
    assert(wait_until([&] { return ingest(t.s).reader.samples >= 4800U; }, kWaitMs));
    assert(wait_until([&] { return rtl_device_has_stream(t.s.dev) == 1; }, kWaitMs));
    /* A queued command waits while the stream only pauses: the read is inside a frame and its samples will come. */
    const int yields = t.s.reader.yields.load();
    g_command_queued.store(1);
    dsd_sleep_ms(300U);
    assert(t.s.reader.yields.load() == yields);
    g_command_queued.store(0);
    const uint64_t seq = rtl_stream_discontinuity_seq();
    const uint32_t gen = rtl_stream_output_generation();
    g_clock_ms.fetch_add(DSD_IQ_NET_STALL_MS);
    assert(wait_until([&] { return rtl_device_has_stream(t.s.dev) == 0; }, kWaitMs));
    expect_yields_without_stream(t.s);
    tcp_send_pairs(t, 4800U);
    expect_boundary(t.s, seq, gen, 1U);
    assert(wait_until([&] { return rtl_device_has_stream(t.s.dev) == 1; }, kWaitMs));
    assert(ingest(t.s).reader.resumes == 1U);
    /* No FREQ: centre 0, never the 851 MHz an earlier input left. */
    uint32_t centre = 1U;
    assert(rtl_stream_get_last_applied_freq(&centre) == 0 && centre == 0U);
    tcp_close(t);
    rtl_net_iq_test_set_clock(nullptr, nullptr);
}

/* udp_silence_and_relock_run_the_boundary: no stream before the first sender, none after its 2 s of silence (a queued
 * command runs both times); the same sender again resumes (net_resume) and another sender after a stall is a new one
 * (net_reconnect), each through the boundary. */
void
test_udp_silence_and_relock_run_the_boundary(void) {
    rtl_net_iq_test_set_clock(fake_clock, nullptr);
    const int port = free_udp_port();
    stream_rig s;
    stream_open(s, udp_spec(port, kRate, "cs16").c_str(), 1);
    assert(rtl_device_has_stream(s.dev) == 0);
    expect_yields_without_stream(s);
    udp_sender a = udp_sender_open(port);
    uint64_t phase = 0U;
    /* Eight datagrams, all of them read before the case moves the clock on, so none arrives after a stall. */
    uint64_t sent = 0U;
    auto send_from = [&](const udp_sender& tx) {
        for (int i = 0; i < 8; i++) {
            const std::vector<uint8_t> d = cs16_tone(360U, &phase);
            udp_send(tx, d.data(), d.size());
        }
        sent += 8U;
        assert(wait_until([&] { return ingest(s).reader.datagrams >= sent; }, kWaitMs));
    };
    send_from(a);
    assert(wait_until([&] { return rtl_device_has_stream(s.dev) == 1; }, kWaitMs));
    uint64_t seq = rtl_stream_discontinuity_seq();
    uint32_t gen = rtl_stream_output_generation();
    g_clock_ms.fetch_add(DSD_IQ_NET_STALL_MS);
    assert(wait_until([&] { return rtl_device_has_stream(s.dev) == 0; }, kWaitMs));
    expect_yields_without_stream(s);
    send_from(a);
    expect_boundary(s, seq, gen, 1U);
    assert(wait_until([&] { return rtl_device_has_stream(s.dev) == 1; }, kWaitMs));
    assert(ingest(s).reader.resumes == 1U);
    seq = rtl_stream_discontinuity_seq();
    gen = rtl_stream_output_generation();
    g_clock_ms.fetch_add(DSD_IQ_NET_STALL_MS);
    assert(wait_until([&] { return rtl_device_has_stream(s.dev) == 0; }, kWaitMs));
    udp_sender b = udp_sender_open(port);
    send_from(b);
    expect_boundary(s, seq, gen, 2U);
    assert(wait_until([&] { return rtl_device_has_stream(s.dev) == 1; }, kWaitMs));
    assert(ingest(s).reader.reconnects == 1U);
    (void)stream_close(s);
    (void)dsd_socket_close(a.sock);
    (void)dsd_socket_close(b.sock);
    rtl_net_iq_test_set_clock(nullptr, nullptr);
}

/* stops_are_prompt: a stop during a silent read, during a reconnect's backoff (nothing listens), during a reconnect
 * whose connect hangs (the listener's queue is full), and once the process exit flag is set, each return promptly. */
void
test_stops_are_prompt(void) {
    {
        tcp_rig t;
        tcp_open(t, kRate, 0U, 0);
        dsd_sleep_ms(300U);
        assert(stream_close(t.s) < kPromptMs);
        (void)dsd_socket_close(t.conn);
        (void)dsd_socket_close(t.listener);
    }
    {
        tcp_rig t;
        tcp_open(t, kRate, 0U, 0);
        (void)dsd_socket_close(t.listener);
        (void)dsd_socket_close(t.conn);
        /* Refused reconnects: the reader backs off 0.2, 0.4, 0.6 s ... Windows takes about 2 s to refuse each connect on
           loopback, so the four attempts get three times the usual wait. */
        assert(wait_until([&] { return ingest(t.s).reader.connect_attempts >= 4U; }, 3U * kWaitMs));
        assert(stream_close(t.s) < kPromptMs);
    }
    {
        tcp_rig t;
        t.listener = open_listener(0, &t.port);
        stream_open(t.s, tcp_spec(t.port, kRate, "cs16", 0U).c_str(), 0);
        t.conn = accept_within(t.listener, kWaitMs);
        assert(t.conn != DSD_INVALID_SOCKET);
        /* Fill the accept queue so the reconnect's handshake waits. That is where the platform drops the SYN (Linux,
           macOS); where it answers a full queue with a reset (Windows) the reconnect is refused and the stop lands in a
           connect or its backoff instead, which must be as prompt. */
        std::vector<dsd_socket_t> fillers;
        for (int i = 0; i < 4; i++) {
            const dsd_socket_t sock = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
            assert(sock != DSD_INVALID_SOCKET);
            struct sockaddr_in sa = loopback(t.port);
            (void)dsd_socket_set_nonblocking(sock, 1);
            (void)dsd_socket_connect(sock, (struct sockaddr*)&sa, (int)sizeof sa);
            fillers.push_back(sock);
        }
        dsd_sleep_ms(100U);
        const uint64_t attempts = ingest(t.s).reader.connect_attempts;
        (void)dsd_socket_close(t.conn);
        t.conn = DSD_INVALID_SOCKET;
        assert(wait_until([&] { return ingest(t.s).reader.connect_attempts > attempts; }, kWaitMs));
        dsd_sleep_ms(300U);
        assert(stream_close(t.s) < kPromptMs);
        for (dsd_socket_t sock : fillers) {
            (void)dsd_socket_close(sock);
        }
        (void)dsd_socket_close(t.listener);
    }
    {
        tcp_rig t;
        tcp_open(t, kRate, 0U, 0);
        /* The exit flag ends the reader's silent read by itself; the stop then only joins. */
        dsd_exitflag_store(1);
        dsd_sleep_ms(500U);
        assert(stream_close(t.s) < kPromptMs);
        dsd_exitflag_store(0);
        (void)dsd_socket_close(t.conn);
        (void)dsd_socket_close(t.listener);
    }
}

/* setters_refuse_and_tune_fails: every tuner setter refuses the stream (DSD_ERR_NOT_SUPPORTED), an unchanged value
 * (its own rate and centre, PPM 0) too; the getters report the stream; a tune fails rather than defers. The samples are
 * centred (no Fs/4 rotation), and the clip level is the format's. */
void
test_setters_refuse_and_tune_fails(void) {
    tcp_rig t;
    tcp_open(t, kRate, 162475000U, 1);
    rtl_device* dev = t.s.dev;
    int selected = 12345;
    assert(rtl_device_set_frequency(dev, 162475000U) == DSD_ERR_NOT_SUPPORTED);
    assert(rtl_device_set_frequency(dev, 851012500U) == DSD_ERR_NOT_SUPPORTED);
    assert(rtl_device_set_sample_rate(dev, kRate) == DSD_ERR_NOT_SUPPORTED);
    assert(rtl_device_set_gain(dev, 200) == DSD_ERR_NOT_SUPPORTED);
    assert(rtl_device_set_gain_nearest(dev, 200, &selected) == DSD_ERR_NOT_SUPPORTED && selected == 12345);
    assert(rtl_device_set_ppm(dev, 0) == DSD_ERR_NOT_SUPPORTED);
    assert(rtl_device_set_ppm(dev, 5) == DSD_ERR_NOT_SUPPORTED);
    assert(rtl_device_set_direct_sampling(dev, 0) == DSD_ERR_NOT_SUPPORTED);
    assert(rtl_device_set_offset_tuning_enabled(dev, 0) == DSD_ERR_NOT_SUPPORTED);
    assert(rtl_device_set_tuner_bandwidth(dev, 0U) == DSD_ERR_NOT_SUPPORTED);
    assert(rtl_device_set_bias_tee(dev, 0) == DSD_ERR_NOT_SUPPORTED);
    assert(rtl_device_set_xtal_freq(dev, 0U, 0U) == DSD_ERR_NOT_SUPPORTED);
    assert(rtl_device_set_testmode(dev, 0) == DSD_ERR_NOT_SUPPORTED);
    assert(rtl_device_set_if_gain(dev, 1, 0) == DSD_ERR_NOT_SUPPORTED);
    assert(rtl_device_set_tcp_autotune(dev, 1) == DSD_ERR_NOT_SUPPORTED);
    int gain = 0;
    assert(rtl_device_get_tuner_gain(dev, &gain) == DSD_ERR_NOT_SUPPORTED);
    /* Not a control that waits for the stream: there is none, connected or not. */
    assert(rtl_device_control_unavailable(dev) == 0);
    assert(rtl_device_supports_ppm(dev) == 0);
    assert(rtl_device_get_sample_rate(dev) == (int)kRate);
    assert(rtl_device_signal_rotation_hz(dev, 1, kRate) == 0);
    assert(fabsf(rtl_device_signal_clip_level(dev, 1) - dsd_iq_net_format_clip_level(DSD_IQ_NET_FORMAT_CS16)) < 1e-6f);
    assert(rtl_stream_tune(t.s.ctx, 851012500U) == RTL_STREAM_TUNE_FAILED);
    uint32_t ticket = 0U;
    assert(rtl_stream_tune_async(851012500U, 7U, &ticket) == RTL_STREAM_TUNE_FAILED);
    assert(dsd_rtl_stream_test_request_retune(851012500L, 0) == -1);
    uint32_t centre = 0U;
    assert(rtl_stream_get_last_applied_freq(&centre) == 0 && centre == 162475000U);
    /* The chain runs from the DSP input rate, the dongle's rate here. */
    rtl_stream_test_running_chain chain{};
    assert(rtl_stream_test_get_running_chain(&chain) == 0);
    assert(chain.capture_rate_hz == kRate);
    /* Centred I/Q: the running stream applies no Fs/4 shift (dongle.centered_iq). */
    assert(chain.fs4_rotation == 0);
    tcp_close(t);
}

/* gap_reasons_reset_as_stream_gaps: the stream's two reasons reset the demod as a stream gap, as rtl_tcp's do, never as
 * a frequency retune (an unknown reason). */
void
test_gap_reasons_reset_as_stream_gaps(void) {
    for (const char* reason : {RTL_STREAM_GAP_REASON_NET_RECONNECT, RTL_STREAM_GAP_REASON_NET_RESUME}) {
        rtl_stream_test_fll_mixer_retune_result out{};
        assert(rtl_stream_test_fll_mixer_retune(reason, 0, 0.01f, 0.02f, 426700000U, 426700000U, 48000, 48000, &out)
               == 0);
        assert(strcmp(out.reason, "stream-gap") == 0);
    }
}

/* fp_mode_applied_on_the_reader (G9): the reader and the demod apply the run's FTZ/DAZ mode at their entry, though the
 * thread that opens the stream holds the opposite (not inheritance), and a restart, which reconnects, applies the new
 * run's; under INHERIT both keep the opener's where a new thread starts with its creator's MXCSR (not on Windows, where
 * every thread starts at the default, so INHERIT leaves them there). */
std::mutex g_fp_seen_lock;
std::vector<std::pair<std::string, unsigned int>> g_fp_seen;

void
fp_observe(const char* thread_tag, unsigned int csr) {
    const std::lock_guard<std::mutex> hold(g_fp_seen_lock);
    g_fp_seen.emplace_back(thread_tag, csr & DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS);
}

void
fp_set_caller_bits(unsigned int bits) {
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    _mm_setcsr((_mm_getcsr() & ~DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS) | bits);
#else
    (void)bits;
#endif
}

void
fp_stream_run(tcp_rig& t, dsd_fp_mode mode, unsigned int caller_bits, unsigned int want_bits) {
    {
        const std::lock_guard<std::mutex> hold(g_fp_seen_lock);
        g_fp_seen.clear();
    }
    dsd_runtime_fp_mode_publish(mode);
    fp_set_caller_bits(caller_bits);
    stream_open(t.s, tcp_spec(t.port, kRate, "cs16", 0U).c_str(), 1);
    t.conn = accept_within(t.listener, kWaitMs);
    assert(t.conn != DSD_INVALID_SOCKET);
    tcp_send_pairs(t, 4800U);
    assert(wait_until([&] { return t.s.reader.samples.load() > 1000U; }, kWaitMs));
    (void)stream_close(t.s);
    (void)dsd_socket_close(t.conn);
    t.conn = DSD_INVALID_SOCKET;
    const std::lock_guard<std::mutex> hold(g_fp_seen_lock);
    for (const char* tag : {"iq_net", "rtl_demod"}) {
        int seen = 0;
        for (const auto& entry : g_fp_seen) {
            if (entry.first == tag) {
                seen++;
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
                assert(entry.second == want_bits);
#else
                (void)want_bits;
#endif
            }
        }
        if (seen == 0) {
            DSD_FPRINTF(stderr, "no FTZ/DAZ apply seen on %s\n", tag);
        }
        assert(seen > 0);
    }
}

void
test_fp_mode_applied_on_the_reader(void) {
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    const unsigned int original = _mm_getcsr();
#endif
    tcp_rig t;
    t.listener = open_listener(8, &t.port);
    dsd_runtime_fp_mode_set_observer(fp_observe);
    fp_stream_run(t, DSD_FP_MODE_ON, 0U, DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS);
    /* The restart reconnects: a new connection, the new run's mode. */
    fp_stream_run(t, DSD_FP_MODE_OFF, DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS, 0U);
#if !DSD_PLATFORM_WIN_NATIVE
    fp_stream_run(t, DSD_FP_MODE_INHERIT, DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS, DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS);
#endif
    dsd_runtime_fp_mode_set_observer(nullptr);
    dsd_runtime_fp_mode_publish(DSD_FP_MODE_INHERIT);
    (void)dsd_socket_close(t.listener);
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    _mm_setcsr(original);
#endif
}

/* open_failures_fail_the_start: nothing listens (TCP), or the port is taken (UDP): the start fails, reported. */
void
test_open_failures_fail_the_start(void) {
    int port = 0;
    (void)dsd_socket_close(open_listener(1, &port));
    auto opts = std::make_unique<dsd_opts>();
    DSD_MEMSET(opts.get(), 0, sizeof(dsd_opts));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtl_dsp_bw_khz = 48;
    opts->frame_dmr = 1;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", tcp_spec(port, kRate, "cs16", 0U).c_str());
    RtlSdrContext* ctx = nullptr;
    dsd_input_failure_clear();
    assert(rtl_stream_create(opts.get(), &ctx) == 0);
    assert(rtl_stream_start(ctx) != 0);
    assert(failure_kind() == DSD_INPUT_FAILURE_REFUSED);
    (void)rtl_stream_destroy(ctx);
    int held_port = 0;
    const dsd_socket_t held = bound_socket(SOCK_DGRAM, &held_port);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", udp_spec(held_port, kRate, "cs16").c_str());
    ctx = nullptr;
    assert(rtl_stream_create(opts.get(), &ctx) == 0);
    assert(rtl_stream_start(ctx) != 0);
    (void)rtl_stream_destroy(ctx);
    (void)dsd_socket_close(held);
    /* A RATE no chain reaches the target from (8 kHz to TETRA's 144 kHz is past 9x) fails before any socket opens. */
    opts->frame_dmr = 0;
    opts->frame_tetra = 1;
    opts->rtl_dsp_bw_khz = 144;
    opts->rtl_dsp_bw_source = DSD_RTL_DSP_BW_SOURCE_EXPLICIT;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", tcp_spec(port, 8000U, "cs16", 0U).c_str());
    ctx = nullptr;
    dsd_input_failure_clear();
    assert(rtl_stream_create(opts.get(), &ctx) == 0);
    assert(rtl_stream_start(ctx) != 0);
    assert(failure_kind() == DSD_INPUT_FAILURE_NONE);
    (void)rtl_stream_destroy(ctx);
    /* A RATE above the live-device capture ceiling fails before any socket opens too, whatever the target, rather than
       sizing a ring and a chain for it (a typo such as 24M for 240k). */
    opts->frame_tetra = 0;
    opts->frame_dmr = 1;
    opts->rtl_dsp_bw_khz = 48;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", tcp_spec(port, 3200001U, "cs16", 0U).c_str());
    ctx = nullptr;
    assert(rtl_stream_create(opts.get(), &ctx) == 0);
    assert(rtl_stream_start(ctx) != 0);
    assert(failure_kind() == DSD_INPUT_FAILURE_NONE);
    (void)rtl_stream_destroy(ctx);
}

struct named_test {
    const char* name;
    void (*run)(void);
};

const named_test kTests[] = {
    {"blocks_exactly_one_chunk", test_blocks_exactly_one_chunk},
    {"level_waits_for_a_block", test_level_waits_for_a_block},
    {"blocks_after_upsampling_and_gaps", test_blocks_after_upsampling_and_gaps},
    {"ring_full_drops_whole_blocks", test_ring_full_drops_whole_blocks},
    {"ring_full_restarts_the_reader", test_ring_full_restarts_the_reader},
    {"ring_full_restarts_an_upsampling_reader", test_ring_full_restarts_an_upsampling_reader},
    {"cqpsk_off_onto_fsk9600_prepares_the_model", test_cqpsk_off_onto_fsk9600_prepares_the_model},
    {"hunt_over_a_replaced_cqpsk_request_prepares_the_model",
     test_hunt_over_a_replaced_cqpsk_request_prepares_the_model},
    {"hunt_over_a_taken_cqpsk_off_request_prepares_the_model",
     test_hunt_over_a_taken_cqpsk_off_request_prepares_the_model},
    {"hunt_behind_a_digital_landing_prepares_the_model", test_hunt_behind_a_digital_landing_prepares_the_model},
    {"hunt_over_a_taken_digital_landing_prepares_the_model", test_hunt_over_a_taken_digital_landing_prepares_the_model},
    {"ted_only_onto_fsk9600_prepares_the_model", test_ted_only_onto_fsk9600_prepares_the_model},
    {"cqpsk_off_over_a_taken_ted_only_request_prepares_the_model",
     test_cqpsk_off_over_a_taken_ted_only_request_prepares_the_model},
    {"tcp_reconnect_runs_the_boundary", test_tcp_reconnect_runs_the_boundary},
    {"level_starts_over_at_a_reconnect", test_level_starts_over_at_a_reconnect},
    {"tcp_stall_resume_runs_the_boundary", test_tcp_stall_resume_runs_the_boundary},
    {"udp_silence_and_relock_run_the_boundary", test_udp_silence_and_relock_run_the_boundary},
    {"stops_are_prompt", test_stops_are_prompt},
    {"setters_refuse_and_tune_fails", test_setters_refuse_and_tune_fails},
    {"gap_reasons_reset_as_stream_gaps", test_gap_reasons_reset_as_stream_gaps},
    {"fp_mode_applied_on_the_reader", test_fp_mode_applied_on_the_reader},
    {"open_failures_fail_the_start", test_open_failures_fail_the_start},
};

} // namespace

/* With no argument every case runs, in order; with one, only the case of that name. */
int
main(int argc, char** argv) {
    const char* only = (argc > 1) ? argv[1] : nullptr;
#if !DSD_PLATFORM_WIN_NATIVE
    (void)signal(SIGPIPE, SIG_IGN);
#endif
    assert(dsd_socket_init() == 0);
    dsd_neo_config_init();
    dsd_runtime_set_controls_pending(command_queued);
    int ran = 0;
    for (const named_test& t : kTests) {
        if (only != nullptr && strcmp(only, t.name) != 0) {
            continue;
        }
        DSD_FPRINTF(stderr, "-- %s\n", t.name);
        t.run();
        ran++;
    }
    assert(ran > 0);
    dsd_runtime_set_controls_pending(nullptr);
    dsd_socket_cleanup();
    if (only == nullptr) {
        /* The lines the cases' paths log, each counted, none printed. */
        assert(log_count(kLogStall) >= 3 && log_count(kLogResume) >= 2);
        assert(log_count(kLogLost) >= 1 && log_count(kLogReconnected) >= 1);
        assert(log_count(kLogMalformed) == 1);
        assert(log_count(kLogRefused) == 1 && log_count(kLogBindRefused) == 1 && log_count(kLogNoPlan) == 1);
        assert(log_count(kLogAboveCeiling) == 1);
    }
    DSD_FPRINTF(stderr, "IO_NET_IQ_STREAM: OK\n");
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
