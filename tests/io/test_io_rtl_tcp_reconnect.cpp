// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * rtl_tcp connection handling (issue #518 B3, B4, B13, B16, B17), against a scripted fake rtl_tcp server on loopback.
 *
 * The server sends the 12-byte dongle_info_t header ("RTL0", tuner type, gain count, all it sends before I/Q) and then
 * u8 I/Q in a pattern that names each byte's place in its pair (I bytes at or above 0x80, Q bytes below) and the phase
 * it was sent in ("first" right after the header, "pre" before the server got any command on the connection, "post"
 * after). It parses the 5-byte commands it is sent and keeps the state a real server would (frequency, rate, gain mode
 * and gain, register 0x19 as test mode and the digital AGC last wrote it, ...), starting each connection from
 * defaults unlike a fresh server's, as a server restarted with other settings would.
 *
 * Device level, the test standing in for the stream's controller (it takes, publishes and releases each reconnected
 * connection the reader hands over): the exact header parser keeps the first I/Q bytes; the session's first open
 * succeeds and is cancellable; stops at every point of the reconnect join promptly; setters fail with no connection,
 * and the reconnect replays the last effective settings and the rtl_tcp gain policy; sends are whole and bound to their
 * connection; the discard path keeps I/Q pairing in the ring and the capture.
 *
 * Stream level, through a real stream and its controller thread: commands apply while the server is gone, the
 * pre-configuration I/Q of a reconnect never reaches the capture, stops with the hand-off queued or taken join, a
 * controller that cannot start fails the start, and an exit request during an outage ends the session promptly.
 */

#include <assert.h>
#include <atomic>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/input_failure.h>
#include <dsd-neo/runtime/input_ring.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <errno.h>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "rtl_stream_test_support.h"
#include "test_support.h"

#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

namespace {

constexpr uint64_t kWaitMs = 5000U;
/* rtl_device_set_gain()'s automatic gain (the device's AUTO_GAIN). */
constexpr int kAutoGain = -100;
/* The receive timeout and the run of timeouts that make a stall here (DSD_NEO_TCP_RCVTIMEO, _MAX_TIMEOUTS). */
constexpr unsigned int kRcvTimeoMs = 200U;

/* The I/Q pattern: each byte's value names its place in the pair and the phase it was sent in. */
enum : int { PHASE_FIRST = 0, PHASE_PRE = 1, PHASE_POST = 2 };

unsigned char
pattern_byte(int phase, uint64_t offset) {
    static const unsigned char kI[3] = {0xF0U, 0xC0U, 0xE0U};
    static const unsigned char kQ[3] = {0x10U, 0x40U, 0x20U};
    return (offset & 1U) == 0U ? kI[phase] : kQ[phase];
}

int
byte_phase(unsigned char b) {
    switch (b) {
        case 0xF0U:
        case 0x10U: return PHASE_FIRST;
        case 0xC0U:
        case 0x40U: return PHASE_PRE;
        case 0xE0U:
        case 0x20U: return PHASE_POST;
        default: return -1;
    }
}

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

/* Start a thread (dsd_thread_create() expands at its call site, so once here). */
void
start_thread(dsd_thread_t* thread, dsd_thread_fn fn, void* arg) {
    const int created = dsd_thread_create(thread, fn, arg);
    assert(created == 0);
}

/* ---- The fake rtl_tcp server ---- */

struct cmd_rec {
    uint8_t id;
    uint32_t param;
};

/* What a real server would hold after the commands it got. */
struct server_model {
    uint32_t freq = 100000000U;
    uint32_t rate = 2400000U;
    int gain_manual = 1;
    int gain = 496;
    int reg19 = 0x25; /* digital AGC on */
    int ppm = 17;
    int bias = 1;
    int direct_sampling = 0;
    int offset = 0;
};

void
model_apply(server_model* m, uint8_t id, uint32_t p) {
    switch (id) {
        case 0x01: m->freq = p; break;
        case 0x02: m->rate = p; break;
        case 0x03: m->gain_manual = (int)p; break;
        case 0x04: m->gain = (int)p; break;
        case 0x05: m->ppm = (int)p; break;
        case 0x07: m->reg19 = p != 0U ? 0x03 : 0x05; break; /* rtlsdr_set_testmode() */
        case 0x08: m->reg19 = p != 0U ? 0x25 : 0x05; break; /* rtlsdr_set_agc_mode() */
        case 0x09: m->direct_sampling = (int)p; break;
        case 0x0A: m->offset = (int)p; break;
        case 0x0E: m->bias = (int)p; break;
        default: break;
    }
}

/* How the server treats one connection. */
struct conn_script {
    int send_header = 1;
    int magic_ok = 1;
    uint32_t gain_count = 29U;
    std::vector<size_t> header_parts; /* the header in these parts (empty: whole) */
    unsigned int header_gap_ms = 0U;
    size_t first_bytes = 0U;   /* this many I/Q bytes in the "first" phase after the header */
    int pre_until_command = 0; /* stream the "pre" phase until the first command arrives */
    /* When set: these "pre" fragments once, then nothing until the first command arrives (then chunks). */
    std::vector<size_t> pre_chunks;
    int stream = 1;
    std::vector<size_t> chunks; /* the I/Q in chunks of these sizes, in turn (empty: 4096) */
    unsigned int chunk_gap_ms = 2U;
    int read_commands = 1;
    int rcvbuf = 0;
};

struct server_conn {
    std::vector<cmd_rec> cmds;
    server_model model;
    uint64_t sent = 0U;
    uint64_t pre_sent = 0U;
    int ended = 0;
};

struct fake_server {
    dsd_mutex_t lock{};
    int port = 0;
    dsd_socket_t listener = DSD_INVALID_SOCKET;
    std::atomic<int> quit{0};
    std::atomic<int> up{1};
    std::atomic<int> kill{0};
    std::atomic<int> paused{0};    /* the connection stays, its I/Q stops */
    std::atomic<int> listening{0}; /* the listener is open */
    std::atomic<int> connected{0}; /* serving a connection */
    std::vector<conn_script> scripts;
    conn_script fallback;
    std::vector<server_conn> conns;
    dsd_thread_t thread{};
};

fake_server g_srv;

dsd_socket_t
open_listener(int port, int* out_port) {
    const dsd_socket_t s = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    if (s == DSD_INVALID_SOCKET) {
        return s;
    }
    int one = 1;
    (void)dsd_socket_setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(0x7F000001U);
    if (dsd_socket_bind(s, reinterpret_cast<const struct sockaddr*>(&addr), (int)sizeof addr) != 0
        || dsd_socket_listen(s, 4) != 0) {
        dsd_socket_close(s);
        return DSD_INVALID_SOCKET;
    }
    if (out_port != nullptr) {
        socklen_t len = sizeof addr;
        assert(getsockname(s, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0);
        *out_port = (int)ntohs(addr.sin_port);
    }
    return s;
}

bool
server_stop_serving(void) {
    return g_srv.quit.load() != 0 || g_srv.kill.load() != 0;
}

/* Send all of @p len bytes unless the connection ends or is to end. */
bool
server_send(dsd_socket_t sock, const unsigned char* buf, size_t len, size_t* out_sent) {
    size_t sent = 0U;
    while (sent < len && !server_stop_serving()) {
        const int n = dsd_socket_send(sock, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        const int e = dsd_socket_get_error();
        if (n < 0 && (e == EAGAIN || e == EWOULDBLOCK || e == EINTR)) {
            continue;
        }
        if (out_sent != nullptr) {
            *out_sent = sent;
        }
        return false;
    }
    if (out_sent != nullptr) {
        *out_sent = sent;
    }
    return sent == len;
}

void
server_serve(dsd_socket_t sock, const conn_script& sc, size_t idx) {
    (void)dsd_socket_set_send_timeout(sock, 20U);
    if (sc.rcvbuf > 0) {
        (void)dsd_socket_setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &sc.rcvbuf, sizeof sc.rcvbuf);
    }
    if (sc.send_header != 0) {
        unsigned char hdr[12] = {'R', 'T', 'L', '0', 0, 0, 0, 5, 0, 0, 0, 0};
        if (sc.magic_ok == 0) {
            hdr[0] = 'X';
        }
        hdr[8] = (unsigned char)(sc.gain_count >> 24);
        hdr[9] = (unsigned char)(sc.gain_count >> 16);
        hdr[10] = (unsigned char)(sc.gain_count >> 8);
        hdr[11] = (unsigned char)sc.gain_count;
        const std::vector<size_t> parts = sc.header_parts.empty() ? std::vector<size_t>{sizeof hdr} : sc.header_parts;
        size_t off = 0U;
        for (size_t i = 0; i < parts.size() && off < sizeof hdr; ++i) {
            if (i > 0U) {
                dsd_sleep_ms(sc.header_gap_ms);
            }
            if (!server_send(sock, hdr + off, parts[i], nullptr)) {
                break;
            }
            off += parts[i];
        }
    }
    std::vector<unsigned char> buf(65536U);
    unsigned char carry[5];
    size_t carry_len = 0U;
    size_t chunk_idx = 0U;
    size_t pre_idx = 0U;
    while (!server_stop_serving()) {
        if (sc.read_commands != 0) {
            for (;;) {
                if (dsd_socket_wait(sock, DSD_SOCKET_WAIT_READ, 0U) <= 0) {
                    break;
                }
                const int n = dsd_socket_recv(sock, buf.data(), 4096U, 0);
                if (n <= 0) {
                    goto done;
                }
                dsd_mutex_lock(&g_srv.lock);
                for (int i = 0; i < n; ++i) {
                    carry[carry_len++] = buf[(size_t)i];
                    if (carry_len == 5U) {
                        const uint32_t p = ((uint32_t)carry[1] << 24) | ((uint32_t)carry[2] << 16)
                                           | ((uint32_t)carry[3] << 8) | (uint32_t)carry[4];
                        g_srv.conns[idx].cmds.push_back({carry[0], p});
                        model_apply(&g_srv.conns[idx].model, carry[0], p);
                        carry_len = 0U;
                    }
                }
                dsd_mutex_unlock(&g_srv.lock);
            }
        }
        dsd_mutex_lock(&g_srv.lock);
        const uint64_t base = g_srv.conns[idx].sent;
        const int no_command = g_srv.conns[idx].cmds.empty() ? 1 : 0;
        dsd_mutex_unlock(&g_srv.lock);
        size_t want = 0U;
        int pre = 0;
        if (!sc.pre_chunks.empty() && no_command != 0) {
            want = pre_idx < sc.pre_chunks.size() ? sc.pre_chunks[pre_idx++] : 0U;
            pre = 1;
        } else if (sc.stream != 0 && g_srv.paused.load() == 0) {
            want = sc.chunks.empty() ? 4096U : sc.chunks[chunk_idx++ % sc.chunks.size()];
            pre = (sc.pre_until_command != 0 && no_command != 0) ? 1 : 0;
        }
        if (want > 0U) {
            for (size_t i = 0; i < want; ++i) {
                const uint64_t k = base + i;
                int phase = (pre != 0) ? PHASE_PRE : PHASE_POST;
                if (k < sc.first_bytes) {
                    phase = PHASE_FIRST;
                }
                buf[i] = pattern_byte(phase, k);
            }
            /* Counted before it goes out, so the client never reads a byte the count does not hold yet. */
            dsd_mutex_lock(&g_srv.lock);
            g_srv.conns[idx].sent += want;
            if (pre != 0) {
                g_srv.conns[idx].pre_sent += want;
            }
            dsd_mutex_unlock(&g_srv.lock);
            size_t sent = 0U;
            const bool ok = server_send(sock, buf.data(), want, &sent);
            dsd_mutex_lock(&g_srv.lock);
            g_srv.conns[idx].sent -= want - sent;
            if (pre != 0) {
                g_srv.conns[idx].pre_sent -= want - sent;
            }
            dsd_mutex_unlock(&g_srv.lock);
            if (!ok) {
                if (server_stop_serving()) {
                    break;
                }
                goto done;
            }
        }
        dsd_sleep_ms(sc.stream != 0 ? sc.chunk_gap_ms : 2U);
    }
done:
    dsd_socket_shutdown(sock, SHUT_RDWR);
    dsd_socket_close(sock);
    dsd_mutex_lock(&g_srv.lock);
    g_srv.conns[idx].ended = 1;
    dsd_mutex_unlock(&g_srv.lock);
}

DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
// NOLINTNEXTLINE(misc-const-correctness) -- The signature is a thread entry's.
server_main(void* arg) {
    (void)arg;
    while (g_srv.quit.load() == 0) {
        if (g_srv.up.load() != 0 && g_srv.listener == DSD_INVALID_SOCKET) {
            g_srv.listener = open_listener(g_srv.port, nullptr);
        }
        if (g_srv.up.load() == 0 && g_srv.listener != DSD_INVALID_SOCKET) {
            dsd_socket_close(g_srv.listener);
            g_srv.listener = DSD_INVALID_SOCKET;
        }
        g_srv.listening.store(g_srv.listener != DSD_INVALID_SOCKET ? 1 : 0);
        if (g_srv.listener == DSD_INVALID_SOCKET || dsd_socket_wait(g_srv.listener, DSD_SOCKET_WAIT_READ, 5U) <= 0) {
            dsd_sleep_ms(2U);
            continue;
        }
        const dsd_socket_t sock = dsd_socket_accept(g_srv.listener, nullptr, nullptr);
        if (sock == DSD_INVALID_SOCKET) {
            continue;
        }
        dsd_mutex_lock(&g_srv.lock);
        conn_script sc = g_srv.fallback;
        if (!g_srv.scripts.empty()) {
            sc = g_srv.scripts.front();
            g_srv.scripts.erase(g_srv.scripts.begin());
        }
        g_srv.conns.emplace_back();
        const size_t idx = g_srv.conns.size() - 1U;
        dsd_mutex_unlock(&g_srv.lock);
        g_srv.kill.store(0);
        g_srv.connected.store(1);
        server_serve(sock, sc, idx);
        g_srv.connected.store(0);
    }
    if (g_srv.listener != DSD_INVALID_SOCKET) {
        dsd_socket_close(g_srv.listener);
        g_srv.listener = DSD_INVALID_SOCKET;
    }
    DSD_THREAD_RETURN;
}

void
server_start(void) {
    assert(dsd_mutex_init(&g_srv.lock) == 0);
    g_srv.listener = open_listener(0, &g_srv.port);
    assert(g_srv.listener != DSD_INVALID_SOCKET && g_srv.port > 0);
    start_thread(&g_srv.thread, server_main, nullptr);
}

void
server_shutdown(void) {
    g_srv.quit.store(1);
    dsd_thread_join(g_srv.thread);
    dsd_mutex_destroy(&g_srv.lock);
}

void server_kill(void);

/* Forget every connection and script; @p fallback serves every connection with no script queued. */
void
server_reset(const conn_script& fallback) {
    if (g_srv.connected.load() != 0) {
        server_kill();
    }
    g_srv.up.store(1);
    assert(wait_until([] { return g_srv.listening.load() != 0; }, kWaitMs));
    dsd_mutex_lock(&g_srv.lock);
    g_srv.scripts.clear();
    g_srv.fallback = fallback;
    g_srv.conns.clear();
    dsd_mutex_unlock(&g_srv.lock);
    g_srv.kill.store(0);
}

void
server_queue(const conn_script& sc) {
    dsd_mutex_lock(&g_srv.lock);
    g_srv.scripts.push_back(sc);
    dsd_mutex_unlock(&g_srv.lock);
}

size_t
server_conn_count(void) {
    dsd_mutex_lock(&g_srv.lock);
    const size_t n = g_srv.conns.size();
    dsd_mutex_unlock(&g_srv.lock);
    return n;
}

server_conn
server_conn_get(size_t idx) {
    dsd_mutex_lock(&g_srv.lock);
    assert(idx < g_srv.conns.size());
    server_conn c = g_srv.conns[idx];
    dsd_mutex_unlock(&g_srv.lock);
    return c;
}

/* End the connection being served, as a server that went away does. */
void
server_kill(void) {
    const size_t n = server_conn_count();
    assert(n > 0U);
    g_srv.kill.store(1);
    assert(wait_until([&] { return server_conn_get(n - 1U).ended != 0; }, kWaitMs));
}

/* Commands with id @p id the connection @p idx got. */
std::vector<uint32_t>
server_params(size_t idx, uint8_t id) {
    std::vector<uint32_t> out;
    for (const cmd_rec& c : server_conn_get(idx).cmds) {
        if (c.id == id) {
            out.push_back(c.param);
        }
    }
    return out;
}

/* ---- A device on its own, the test playing its controller ---- */

struct dev_rig {
    input_ring_state ring{};
    rtl_device* dev = nullptr;
    /* The controller: takes, publishes and releases every hand-off, unless held. */
    std::atomic<int> hold{0};
    std::atomic<int> quit{0};
    std::atomic<int> taken{0};
    std::atomic<int> published{0};
    std::atomic<int> failed{0};
    std::atomic<int> released{0};
    std::atomic<int> resume_rc{0};
    dsd_thread_t controller{};
    /* The ring's reader: every I/Q pair checked (I above zero, Q below), every float's phase kept. */
    std::atomic<int> drain_quit{0};
    std::atomic<uint64_t> floats{0U};
    std::atomic<uint64_t> misaligned{0U};
    dsd_mutex_t phases_lock{};
    std::vector<int> phases;
    dsd_thread_t drainer{};
    /* The I/Q capture */
    dsd_iq_capture_writer* writer = nullptr;
    char dir[DSD_TEST_PATH_MAX] = {};
    char data_path[DSD_TEST_PATH_MAX] = {};
    char meta_path[DSD_TEST_PATH_MAX] = {};
};

void
// NOLINTNEXTLINE(misc-const-correctness) -- The signature is the recovery callback's.
rig_post(void* ctx) {
    (void)ctx;
}

DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
rig_controller(void* arg) {
    auto* r = static_cast<dev_rig*>(arg);
    while (r->quit.load() == 0) {
        if (r->hold.load() == 0 && rtl_device_tcp_handoff_take(r->dev) != 0) {
            r->taken.fetch_add(1);
            char why[128] = {};
            if (rtl_device_tcp_publish_handoff(r->dev, why, sizeof why) == 0) {
                r->published.fetch_add(1);
                r->resume_rc.store(rtl_device_resume_stream(r->dev));
                r->released.fetch_add(1);
            } else {
                r->failed.fetch_add(1);
            }
            continue;
        }
        dsd_sleep_ms(1U);
    }
    DSD_THREAD_RETURN;
}

int
float_phase(float v) {
    const float a = v < 0.0f ? -v : v;
    const float levels[3] = {(240.0f - 127.5f) / 127.5f, (192.0f - 127.5f) / 127.5f, (224.0f - 127.5f) / 127.5f};
    for (int p = 0; p < 3; ++p) {
        if (a > levels[p] - 0.01f && a < levels[p] + 0.01f) {
            return p;
        }
    }
    return -1;
}

DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
rig_drain(void* arg) {
    auto* r = static_cast<dev_rig*>(arg);
    std::vector<float> block(8192U);
    uint64_t index = 0U;
    while (r->drain_quit.load() == 0) {
        if (input_ring_used(&r->ring) == 0U) {
            dsd_sleep_ms(1U);
            continue;
        }
        const int got = input_ring_read_block(&r->ring, block.data(), block.size());
        if (got <= 0) {
            continue;
        }
        dsd_mutex_lock(&r->phases_lock);
        for (int i = 0; i < got; ++i, ++index) {
            const float v = block[(size_t)i];
            const bool is_i = (index & 1U) == 0U;
            if ((is_i && !(v > 0.0f)) || (!is_i && !(v < 0.0f))) {
                r->misaligned.fetch_add(1U);
            }
            if (r->phases.size() < (size_t)8U * 1024U * 1024U) {
                r->phases.push_back(float_phase(v));
            }
        }
        dsd_mutex_unlock(&r->phases_lock);
        r->floats.fetch_add((uint64_t)got);
    }
    DSD_THREAD_RETURN;
}

void
rig_open_capture(dev_rig& r) {
    assert(dsd_test_mkdtemp(r.dir, sizeof r.dir, "dsdneo_rtl_tcp") != nullptr);
    assert(dsd_test_path_join(r.data_path, sizeof r.data_path, r.dir, "capture.iq") == 0);
    assert(dsd_test_path_join(r.meta_path, sizeof r.meta_path, r.dir, "capture.iq.json") == 0);
    dsd_iq_capture_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    DSD_SNPRINTF(cfg.data_path, sizeof cfg.data_path, "%s", r.data_path);
    DSD_SNPRINTF(cfg.metadata_path, sizeof cfg.metadata_path, "%s", r.meta_path);
    cfg.format = DSD_IQ_FORMAT_CU8;
    DSD_SNPRINTF(cfg.capture_stage, sizeof cfg.capture_stage, "%s", "post_mute_pre_widen");
    cfg.sample_rate_hz = 960000U;
    cfg.center_frequency_hz = 851012500U;
    cfg.capture_center_frequency_hz = 851012500U;
    cfg.rtl_dsp_bw_khz = 48;
    cfg.base_decimation = 16U;
    cfg.post_downsample = 1U;
    cfg.demod_rate_hz = 60000U;
    cfg.muted_bytes_excluded = 1;
    DSD_SNPRINTF(cfg.source_backend, sizeof cfg.source_backend, "%s", "rtl_tcp");
    DSD_SNPRINTF(cfg.source_args, sizeof cfg.source_args, "%s", "127.0.0.1");
    char err[256] = {};
    const int rc = dsd_iq_capture_open(&cfg, &r.writer, err, sizeof err);
    if (rc != DSD_IQ_OK || r.writer == nullptr) {
        DSD_FPRINTF(stderr, "capture open: %s\n", err);
    }
    assert(rc == DSD_IQ_OK && r.writer != nullptr);
}

/* Open the device on the fake server: its first connection, the controller, the ring's reader and, with
 * @p capture, an I/Q capture. With @p offset hardware offset tuning is on, so nothing rotates the ring's I/Q. */
void
rig_open(dev_rig& r, int capture, int offset) {
    assert(input_ring_init(&r.ring, 1U << 20) == 0);
    assert(dsd_mutex_init(&r.phases_lock) == 0);
    r.dev = rtl_device_create_tcp("127.0.0.1", g_srv.port, &r.ring, 0);
    assert(r.dev != nullptr);
    rtl_device_set_recovery_callback(r.dev, rig_post, &r);
    if (offset != 0) {
        assert(rtl_device_set_offset_tuning_enabled(r.dev, 1) == 0);
    }
    if (capture != 0) {
        rig_open_capture(r);
        rtl_device_set_iq_capture_writer(r.dev, r.writer);
    }
    start_thread(&r.controller, rig_controller, &r);
    start_thread(&r.drainer, rig_drain, &r);
}

void
rig_start(const dev_rig& r) {
    assert(rtl_device_start_async(r.dev, 16384U) == 0);
}

/* Stop the device (within @p bound_ms) and take everything down. */
void
rig_close(dev_rig& r, uint64_t bound_ms = 2000U) {
    const uint64_t t0 = now_ms();
    (void)rtl_device_stop_async(r.dev);
    assert(now_ms() - t0 < bound_ms);
    r.quit.store(1);
    dsd_thread_join(r.controller);
    r.drain_quit.store(1);
    dsd_thread_join(r.drainer);
    rtl_device_set_recovery_callback(r.dev, nullptr, nullptr);
    rtl_device_set_iq_capture_writer(r.dev, nullptr);
    rtl_device_destroy(r.dev);
    r.dev = nullptr;
    if (r.writer != nullptr) {
        const dsd_iq_capture_final_stats stats{};
        dsd_iq_capture_close(r.writer, &stats);
        r.writer = nullptr;
    }
    input_ring_destroy(&r.ring);
    dsd_mutex_destroy(&r.phases_lock);
}

std::vector<unsigned char>
read_file(const char* path) {
    std::vector<unsigned char> out;
    FILE* f = dsd_fopen_private(path, "rb");
    assert(f != nullptr);
    unsigned char buf[65536];
    size_t n = 0U;
    while ((n = fread(buf, 1U, sizeof buf, f)) > 0U) {
        out.insert(out.end(), buf, buf + n);
    }
    (void)fclose(f);
    return out;
}

const char* const kCaptureFiles[] = {"capture.iq", "capture.iq.json", nullptr};

void
rig_remove_capture(dev_rig& r) {
    assert(dsd_test_remove_temp_dir(r.dir, kCaptureFiles) == 0);
}

uint64_t
rig_generation(const dev_rig& r) {
    rtl_device_test_tcp_state st{};
    rtl_device_test_tcp_state_get(r.dev, &st);
    return st.generation;
}

/* Wait until a connection with generation @p gen is published and released to the reader. */
void
wait_generation(const dev_rig& r, uint64_t gen) {
    assert(wait_until(
        [&] {
            rtl_device_test_tcp_state st{};
            rtl_device_test_tcp_state_get(r.dev, &st);
            return st.generation == gen && st.handoff == RTL_TEST_TCP_HANDOFF_NONE;
        },
        kWaitMs));
}

/* Wait until the server's connection @p idx exists and got @p n commands. */
void
wait_commands(size_t idx, size_t n) {
    assert(wait_until([&] { return server_conn_count() > idx && server_conn_get(idx).cmds.size() >= n; }, kWaitMs));
}

/* Wait until the server's connection @p idx holds a state @p pred accepts (its commands are parsed as they come), and
 * return it. */
template <typename Pred>
server_model
wait_model(size_t idx, Pred pred) {
    server_model m;
    const bool ok = wait_until(
        [&] {
            if (server_conn_count() <= idx) {
                return false;
            }
            m = server_conn_get(idx).model;
            return pred(m);
        },
        kWaitMs);
    assert(ok);
    return m;
}

/* Wait until the server's connection @p idx parsed every command sent before now: one more (a PPM value of its own)
 * goes out behind them; returns the state then. */
server_model
sync_server(rtl_device* dev, size_t idx) {
    static int marker = 40;
    const int ppm = ++marker;
    assert(rtl_device_set_ppm(dev, ppm) == 0);
    return wait_model(idx, [&](const server_model& m) { return m.ppm == ppm; });
}

/* Lose the connection and wait until the device let it go: no stream, or (the controller being quick) another
 * connection published already. */
void
lose_connection(const dev_rig& r) {
    const uint64_t gen = rig_generation(r);
    server_kill();
    assert(wait_until([&] { return rig_generation(r) != gen; }, kWaitMs));
}

conn_script
default_script(void) {
    return conn_script{};
}

/* ---- Header ---- */

/* header_has_no_gain_table: standard rtl_tcp sends the 12-byte header and then I/Q. With a nonzero gain count in it,
 * the first I/Q bytes (here 116, the gain count times four, which a gain-table skip would eat) reach the capture and
 * the ring intact, in pairs. */
void
test_header_has_no_gain_table(void) {
    conn_script sc;
    sc.gain_count = 29U;
    sc.first_bytes = (size_t)29U * 4U;
    server_reset(sc);
    dev_rig r;
    rig_open(r, 1, 1);
    rig_start(r);
    assert(wait_until([&] { return r.floats.load() >= 65536U; }, kWaitMs));
    rig_close(r);
    const std::vector<unsigned char> data = read_file(r.data_path);
    assert(data.size() >= 4096U);
    for (size_t i = 0; i < sc.first_bytes; ++i) {
        assert(data[i] == pattern_byte(PHASE_FIRST, i));
    }
    assert(data[sc.first_bytes] == pattern_byte(PHASE_POST, sc.first_bytes));
    assert(r.phases.size() >= sc.first_bytes);
    for (size_t i = 0; i < sc.first_bytes; ++i) {
        assert(r.phases[i] == PHASE_FIRST);
    }
    assert(r.misaligned.load() == 0U);
    rig_remove_capture(r);
}

/* header_fragmented_across_timeouts: the header comes in three parts, each gap longer than the receive timeout; it is
 * read whole and the first I/Q byte after it is the first in the capture. A connection that ends inside the header,
 * or a header without the magic, fails the open. */
int g_header_cancel = 0;

/* Once a connection is served, end it 100 ms later. */
DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
// NOLINTNEXTLINE(misc-const-correctness) -- The signature is a thread entry's.
kill_when_served(void* arg) {
    (void)arg;
    (void)wait_until([] { return g_srv.connected.load() != 0; }, kWaitMs);
    dsd_sleep_ms(100U);
    g_srv.kill.store(1);
    DSD_THREAD_RETURN;
}

/* Once a connection is served, raise the exit flag 200 ms later. */
DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
// NOLINTNEXTLINE(misc-const-correctness) -- The signature is a thread entry's.
exit_when_served(void* arg) {
    (void)arg;
    (void)wait_until([] { return g_srv.connected.load() != 0; }, kWaitMs);
    dsd_sleep_ms(200U);
    dsd_exitflag_store(1);
    DSD_THREAD_RETURN;
}

int
// NOLINTNEXTLINE(misc-const-correctness) -- The signature is the cancellation test's.
header_cancelled(void* ctx) {
    (void)ctx;
    return g_header_cancel;
}

void
test_header_fragmented_across_timeouts(void) {
    conn_script sc;
    sc.header_parts = {5U, 4U, 3U};
    sc.header_gap_ms = kRcvTimeoMs + 150U;
    sc.first_bytes = 64U;
    server_reset(sc);
    dev_rig r;
    rig_open(r, 1, 1);
    rig_start(r);
    assert(wait_until([&] { return r.floats.load() >= 16384U; }, kWaitMs));
    rig_close(r);
    const std::vector<unsigned char> data = read_file(r.data_path);
    assert(data.size() >= 64U);
    for (size_t i = 0; i < 64U; ++i) {
        assert(data[i] == pattern_byte(PHASE_FIRST, i));
    }
    rig_remove_capture(r);

    /* Not an rtl_tcp server: no device. */
    conn_script bad;
    bad.magic_ok = 0;
    server_reset(bad);
    input_ring_state ring{};
    assert(input_ring_init(&ring, 4096U) == 0);
    assert(rtl_device_create_tcp("127.0.0.1", g_srv.port, &ring, 0) == nullptr);
    /* The connection ends inside the header. */
    conn_script cut;
    cut.header_parts = {7U};
    cut.stream = 0;
    cut.read_commands = 0;
    server_reset(cut);
    dsd_thread_t killer{};
    start_thread(&killer, kill_when_served, nullptr);
    assert(rtl_device_create_tcp("127.0.0.1", g_srv.port, &ring, 0) == nullptr);
    dsd_thread_join(killer);
    input_ring_destroy(&ring);

    /* The parser on its own: the results it names. */
    conn_script silent;
    silent.send_header = 0;
    silent.stream = 0;
    server_reset(silent);
    const dsd_socket_t s = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    assert(dsd_socket_resolve("127.0.0.1", g_srv.port, &addr) == 0);
    assert(dsd_socket_connect(s, reinterpret_cast<const struct sockaddr*>(&addr), (int)sizeof addr) == 0);
    uint32_t tuner = 0U;
    uint32_t gains = 0U;
    assert(rtl_device_test_tcp_read_header((intptr_t)s, 300U, header_cancelled, nullptr, &tuner, &gains)
           == RTL_TEST_TCP_HEADER_TIMEOUT);
    g_header_cancel = 1;
    assert(rtl_device_test_tcp_read_header((intptr_t)s, 3000U, header_cancelled, nullptr, &tuner, &gains)
           == RTL_TEST_TCP_HEADER_CANCELLED);
    g_header_cancel = 0;
    dsd_socket_close(s);
}

/* ---- Open and stop ---- */

/* initial_open_succeeds_and_cancelled_initial_open_stops: the session's first open runs before any reader (run 0); it
 * succeeds against a server, and an exit request cancels it while it waits for a silent server's header. */
void
test_initial_open_succeeds_and_cancelled_initial_open_stops(void) {
    server_reset(default_script());
    dev_rig r;
    rig_open(r, 0, 0);
    assert(rtl_device_has_stream(r.dev) == 1);
    assert(rig_generation(r) == 1U);
    rig_close(r);

    conn_script silent;
    silent.send_header = 0;
    silent.stream = 0;
    server_reset(silent);
    input_ring_state ring{};
    assert(input_ring_init(&ring, 4096U) == 0);
    dsd_thread_t quitter{};
    start_thread(&quitter, exit_when_served, nullptr);
    const uint64_t t0 = now_ms();
    assert(rtl_device_create_tcp("127.0.0.1", g_srv.port, &ring, 0) == nullptr);
    const uint64_t took = now_ms() - t0;
    dsd_thread_join(quitter);
    assert(took < 1500U);
    dsd_exitflag_store(0);
    input_ring_destroy(&ring);
}

/* stop_before_successful_connect: the server is gone and the reader keeps reconnecting (refused, backing off longer
 * each time); a stop joins it within a slice or two, wherever it was. */
void
test_stop_before_successful_connect(void) {
    server_reset(default_script());
    dev_rig r;
    rig_open(r, 0, 0);
    rig_start(r);
    g_srv.up.store(0);
    lose_connection(r);
    dsd_sleep_ms(2500U);
    const uint64_t t0 = now_ms();
    (void)rtl_device_stop_async(r.dev);
    assert(now_ms() - t0 < 400U);
    rig_close(r);
    g_srv.up.store(1);
}

/* stop_during_silent_header: the reconnect reached a server that never sends its header; a stop joins promptly and
 * the connection is closed. */
void
test_stop_during_silent_header(void) {
    server_reset(default_script());
    conn_script silent;
    silent.send_header = 0;
    silent.stream = 0;
    silent.read_commands = 1;
    dev_rig r;
    rig_open(r, 0, 0);
    rig_start(r);
    server_queue(silent);
    lose_connection(r);
    assert(wait_until([] { return server_conn_count() >= 2U && g_srv.connected.load() != 0; }, kWaitMs));
    dsd_sleep_ms(300U);
    const uint64_t t0 = now_ms();
    (void)rtl_device_stop_async(r.dev);
    assert(now_ms() - t0 < 400U);
    assert(wait_until([] { return server_conn_get(1).ended != 0; }, kWaitMs));
    rig_close(r);
}

/* ---- Settings across a reconnect (D6) ---- */

/* The session's settings on the first connection. */
void
configure(rtl_device* dev) {
    assert(rtl_device_set_frequency(dev, 851012500U) == 0);
    assert(rtl_device_set_sample_rate(dev, 960000U) == 0);
    assert(rtl_device_set_ppm(dev, 0) == 0);
    assert(rtl_device_set_bias_tee(dev, 0) == 0);
    assert(rtl_device_set_gain(dev, kAutoGain) == 0);
}

/* setting_change_while_disconnected_fails: with no connection a setter fails as "not connected" and changes nothing;
 * the server reconnected to ends with the last effective setting, not the refused one. */
void
test_setting_change_while_disconnected_fails(void) {
    server_reset(default_script());
    dev_rig r;
    rig_open(r, 0, 0);
    configure(r.dev);
    rig_start(r);
    r.hold.store(1);
    lose_connection(r);
    assert(rtl_device_has_stream(r.dev) == 0);
    assert(rtl_device_set_frequency(r.dev, 852000000U) == RTL_DEVICE_ENOTCONN);
    assert(rtl_device_set_sample_rate(r.dev, 1200000U) == RTL_DEVICE_ENOTCONN);
    assert(rtl_device_get_sample_rate(r.dev) == 960000);
    assert(rtl_device_set_gain(r.dev, 280) == RTL_DEVICE_ENOTCONN);
    assert(rtl_device_is_auto_gain(r.dev) == 1);
    assert(rtl_device_set_bias_tee(r.dev, 1) == RTL_DEVICE_ENOTCONN);
    r.hold.store(0);
    wait_generation(r, 2U);
    (void)wait_model(1, [](const server_model& m) {
        return m.freq == 851012500U && m.rate == 960000U && m.gain_manual == 0 && m.reg19 == 0x25 && m.bias == 0;
    });
    rig_close(r);
}

/* latest_effective_settings_replayed: every control that took is replayed as last taken, those turned back to the
 * fresh-server value included, onto a server restarted with other defaults; nothing replays twice. */
void
test_latest_effective_settings_replayed(void) {
    server_reset(default_script());
    dev_rig r;
    rig_open(r, 0, 0);
    configure(r.dev);
    assert(rtl_device_set_ppm(r.dev, 5) == 0);
    assert(rtl_device_set_bias_tee(r.dev, 1) == 0);
    assert(rtl_device_set_frequency(r.dev, 853000000U) == 0);
    assert(rtl_device_set_gain(r.dev, 280) == 0);
    /* Turned back to what a fresh server holds: still replayed, since this server holds something else. */
    assert(rtl_device_set_ppm(r.dev, 0) == 0);
    assert(rtl_device_set_bias_tee(r.dev, 0) == 0);
    assert(rtl_device_set_frequency(r.dev, 854000000U) == 0);
    rig_start(r);
    lose_connection(r);
    wait_generation(r, 2U);
    (void)wait_model(1, [](const server_model& m) {
        return m.freq == 854000000U && m.rate == 960000U && m.ppm == 0 && m.bias == 0 && m.gain_manual == 1
               && m.gain == 280 && m.reg19 == 0x05;
    });
    assert(server_params(1, 0x01).size() == 1U);
    rig_close(r);
}

/* replay_failure_not_published: a replay that fails part way publishes nothing (no stream, setters still refused),
 * and the reader reconnects again, whose replay is published. */
void
test_replay_failure_not_published(void) {
    server_reset(default_script());
    dev_rig r;
    rig_open(r, 0, 0);
    configure(r.dev);
    rig_start(r);
    rtl_device_test_tcp_fail_command(0x02);
    lose_connection(r);
    assert(wait_until([&] { return r.failed.load() >= 1; }, kWaitMs));
    assert(r.published.load() == 0);
    rtl_device_test_tcp_state st{};
    rtl_device_test_tcp_state_get(r.dev, &st);
    assert(st.generation != 2U);
    wait_generation(r, 3U);
    assert(r.published.load() == 1);
    assert(rtl_device_has_stream(r.dev) == 1);
    assert(server_conn_get(1).ended != 0);
    (void)wait_model(2, [](const server_model& m) { return m.rate == 960000U; });
    rig_close(r);
}

/* ---- Sends ---- */

struct sender_ctx {
    rtl_device* dev;
    int which;
    std::atomic<int> failures{0};
};

DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
sender_main(void* arg) {
    auto* c = static_cast<sender_ctx*>(arg);
    for (int i = 1; i <= 150; ++i) {
        const int rc = c->which == 0 ? rtl_device_set_ppm(c->dev, i) : rtl_device_set_frequency(c->dev, 851000000U + i);
        if (rc != 0) {
            c->failures.fetch_add(1);
        }
    }
    DSD_THREAD_RETURN;
}

/* concurrent_partial_writes_never_interleave: two threads send commands at once, each send call writing one byte at a
 * time; the server reads only whole commands, each as sent. */
void
test_concurrent_partial_writes_never_interleave(void) {
    server_reset(default_script());
    dev_rig r;
    rig_open(r, 0, 0);
    rig_start(r);
    const size_t before = server_conn_get(0).cmds.size();
    rtl_device_test_tcp_set_send_chunk(1U);
    sender_ctx a{r.dev, 0};
    sender_ctx b{r.dev, 1};
    dsd_thread_t ta{};
    dsd_thread_t tb{};
    start_thread(&ta, sender_main, &a);
    start_thread(&tb, sender_main, &b);
    dsd_thread_join(ta);
    dsd_thread_join(tb);
    rtl_device_test_tcp_set_send_chunk(0U);
    assert(a.failures.load() == 0 && b.failures.load() == 0);
    wait_commands(0, before + 300U);
    const server_conn c = server_conn_get(0);
    assert(c.cmds.size() == before + 300U);
    int ppm_next = 1;
    uint32_t freq_next = 851000001U;
    for (size_t i = before; i < c.cmds.size(); ++i) {
        if (c.cmds[i].id == 0x05) {
            assert(c.cmds[i].param == (uint32_t)ppm_next);
            ppm_next++;
        } else {
            assert(c.cmds[i].id == 0x01 && c.cmds[i].param == freq_next);
            freq_next++;
        }
    }
    rig_close(r);
}

/* old_sender_does_not_touch_replacement: a setter holding the first connection when it is lost and replaced fails on
 * it; it neither takes the replacement down nor records anything, and the replacement never gets its command. */
struct old_sender {
    dev_rig* rig;
    std::atomic<int> armed{0};
    std::atomic<int> held{0};
    std::atomic<int> rc{1};
};

old_sender g_old;

void
// NOLINTNEXTLINE(misc-const-correctness) -- The signature is the send hook's.
hold_first_generation(uint64_t generation, void* ctx) {
    (void)ctx;
    if (generation != 1U || g_old.armed.exchange(0) == 0) {
        return;
    }
    g_old.held.store(1);
    lose_connection(*g_old.rig);
    wait_generation(*g_old.rig, 2U);
}

void
test_old_sender_does_not_touch_replacement(void) {
    server_reset(default_script());
    dev_rig r;
    rig_open(r, 0, 0);
    configure(r.dev);
    rig_start(r);
    g_old.rig = &r;
    g_old.armed.store(1);
    rtl_device_test_tcp_set_send_hook(hold_first_generation, nullptr);
    const int rc = rtl_device_set_frequency(r.dev, 860000000U);
    rtl_device_test_tcp_set_send_hook(nullptr, nullptr);
    assert(g_old.held.load() == 1);
    assert(rc == RTL_DEVICE_ENOTCONN);
    assert(rtl_device_has_stream(r.dev) == 1);
    assert(rig_generation(r) == 2U);
    (void)wait_model(1, [](const server_model& m) { return m.freq == 851012500U; });
    /* Nothing was recorded: a later reconnect replays the frequency that took. */
    lose_connection(r);
    wait_generation(r, 3U);
    (void)wait_model(2, [](const server_model& m) { return m.freq == 851012500U; });
    for (size_t i = 0; i < 3U; ++i) {
        for (const uint32_t f : server_params(i, 0x01)) {
            assert(f != 860000000U);
        }
    }
    rig_close(r);
}

/* stop_during_replay: a stop that comes while the replay runs ends it: nothing is published, the connection is closed,
 * and the reader joins. */
std::atomic<int> g_replay_stop_armed{0};
dev_rig* g_replay_rig = nullptr;

DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
// NOLINTNEXTLINE(misc-const-correctness) -- The signature is a thread entry's.
stopper_main(void* arg) {
    const auto* r = static_cast<const dev_rig*>(arg);
    (void)rtl_device_stop_async(r->dev);
    DSD_THREAD_RETURN;
}

dsd_thread_t g_stopper{};

void
// NOLINTNEXTLINE(misc-const-correctness) -- The signature is the send hook's.
stop_at_replay(uint64_t generation, void* ctx) {
    (void)ctx;
    if (generation != 2U || g_replay_stop_armed.exchange(0) == 0) {
        return;
    }
    start_thread(&g_stopper, stopper_main, g_replay_rig);
    assert(wait_until(
        [] {
            rtl_device_test_tcp_state st{};
            rtl_device_test_tcp_state_get(g_replay_rig->dev, &st);
            return st.stop_requested != 0;
        },
        kWaitMs));
}

void
test_stop_during_replay(void) {
    server_reset(default_script());
    dev_rig r;
    rig_open(r, 0, 0);
    configure(r.dev);
    rig_start(r);
    g_replay_rig = &r;
    g_replay_stop_armed.store(1);
    rtl_device_test_tcp_set_send_hook(stop_at_replay, nullptr);
    lose_connection(r);
    assert(wait_until([] { return g_replay_stop_armed.load() == 0; }, kWaitMs));
    const uint64_t t0 = now_ms();
    dsd_thread_join(g_stopper);
    assert(now_ms() - t0 < 1000U);
    rtl_device_test_tcp_set_send_hook(nullptr, nullptr);
    assert(r.published.load() == 0);
    assert(rtl_device_has_stream(r.dev) == 0);
    assert(wait_until([] { return server_conn_count() >= 2U && server_conn_get(1).ended != 0; }, kWaitMs));
    rig_close(r);
}

/* stop_during_backpressured_send: the server stops reading commands until a setter blocks in its send; a stop wakes
 * it (it fails) and joins the reader promptly. */
std::atomic<uint64_t> g_send_calls{0U};

void
// NOLINTNEXTLINE(misc-const-correctness) -- The signature is the send hook's.
count_sends(uint64_t generation, void* ctx) {
    (void)generation;
    (void)ctx;
    g_send_calls.fetch_add(1U);
}

struct flood_ctx {
    rtl_device* dev;
    std::atomic<int> last_rc{0};
    std::atomic<int> done{0};
};

DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
flood_main(void* arg) {
    auto* c = static_cast<flood_ctx*>(arg);
    for (int i = 1; i < 10000000; ++i) {
        const int rc = rtl_device_set_ppm(c->dev, (i & 1) != 0 ? 3 : 4);
        if (rc != 0) {
            c->last_rc.store(rc);
            break;
        }
    }
    c->done.store(1);
    DSD_THREAD_RETURN;
}

void
test_stop_during_backpressured_send(void) {
    conn_script sc;
    sc.read_commands = 0;
    sc.rcvbuf = 4096;
    server_reset(sc);
    rtl_device_test_tcp_set_sndbuf(4096);
    dev_rig r;
    rig_open(r, 0, 0);
    rig_start(r);
    rtl_device_test_tcp_set_send_hook(count_sends, nullptr);
    flood_ctx fc{r.dev};
    dsd_thread_t flood{};
    start_thread(&flood, flood_main, &fc);
    /* Blocked: no new send for 300 ms, and no failure yet. */
    uint64_t seen = 0U;
    uint64_t since = now_ms();
    const bool blocked = wait_until(
        [&] {
            const uint64_t n = g_send_calls.load();
            if (n == 0U || n != seen) {
                seen = n;
                since = now_ms();
                return false;
            }
            return now_ms() - since >= 300U;
        },
        kWaitMs);
    assert(blocked);
    DSD_FPRINTF(stderr, "backpressure: %llu sends, done=%d rc=%d\n", (unsigned long long)seen, fc.done.load(),
                fc.last_rc.load());
    assert(fc.done.load() == 0);
    const uint64_t t0 = now_ms();
    (void)rtl_device_stop_async(r.dev);
    assert(now_ms() - t0 < 500U);
    assert(wait_until([&] { return fc.done.load() != 0; }, 500U));
    assert(fc.last_rc.load() != 0);
    dsd_thread_join(flood);
    rtl_device_test_tcp_set_send_hook(nullptr, nullptr);
    rtl_device_test_tcp_set_sndbuf(0);
    rig_close(r);
}

/* ---- Discard path ---- */

/* discard_release_keeps_pair_alignment: while the hand-off waits, the reconnected server sends odd-sized fragments
 * (4107 bytes in all) of I/Q from before its configuration, and the release lands while the reader is inside the
 * receive of the last one; the reader drops the odd byte that leaves (a Q byte), so the ring and the capture hold whole
 * pairs again, starting on an I byte, and none of the discarded I/Q. */
std::atomic<int> g_release_in_receive{0};
dev_rig* g_discard_rig = nullptr;
constexpr uint64_t kDiscardBytes = 4095U + 3U + 1U + 7U + 1U;

void
// NOLINTNEXTLINE(misc-const-correctness) -- The signature is the discard hook's.
release_during_receive(void* ctx) {
    (void)ctx;
    if (g_release_in_receive.load() != 1 || server_conn_get(1).sent < kDiscardBytes) {
        return;
    }
    g_release_in_receive.store(2);
    /* The controller (the rig's thread) takes, publishes and releases while this receive is on its way. */
    g_discard_rig->hold.store(0);
    dsd_sleep_ms(50U);
}

void
test_discard_release_keeps_pair_alignment(void) {
    server_reset(default_script());
    conn_script odd;
    odd.pre_chunks = {4095U, 3U, 1U, 7U, 1U};
    odd.chunk_gap_ms = 20U;
    server_queue(conn_script{});
    server_queue(odd);
    dev_rig r;
    g_discard_rig = &r;
    rig_open(r, 1, 1);
    configure(r.dev);
    rig_start(r);
    assert(wait_until([&] { return r.floats.load() >= 32768U; }, kWaitMs));
    const uint64_t drops_before = rtl_device_test_tcp_odd_drops();
    r.hold.store(1);
    g_release_in_receive.store(1);
    rtl_device_test_tcp_set_discard_hook(release_during_receive, nullptr);
    lose_connection(r);
    wait_generation(r, 2U);
    assert(g_release_in_receive.load() == 2);
    assert(rtl_device_test_tcp_odd_drops() == drops_before + 1U);
    const uint64_t floats_at_release = r.floats.load();
    assert(wait_until([&] { return r.floats.load() >= floats_at_release + 65536U; }, kWaitMs));
    rig_close(r);
    rtl_device_test_tcp_set_discard_hook(nullptr, nullptr);
    g_release_in_receive.store(0);
    assert(server_conn_get(1).pre_sent == kDiscardBytes);
    assert(r.misaligned.load() == 0U);
    for (const int phase : r.phases) {
        assert(phase != PHASE_PRE);
    }
    const std::vector<unsigned char> data = read_file(r.data_path);
    assert(data.size() > 65536U);
    for (size_t i = 0; i < data.size(); ++i) {
        assert(((i & 1U) == 0U) == (data[i] >= 0x80U));
        assert(byte_phase(data[i]) != PHASE_PRE);
    }
    rig_remove_capture(r);
}

/* ---- Gain over rtl_tcp (B9, B16) ---- */

/* nearest_manual_gain_sends_agc_off_and_survives_reconnect: from automatic gain with the digital AGC on, the autogain
 * supervisor's nearest manual gain turns the digital AGC off with manual mode and the gain; the device and the
 * published telemetry say manual, and a reconnect restores manual with the digital AGC off. */
void
test_nearest_manual_gain_sends_agc_off_and_survives_reconnect(void) {
    server_reset(default_script());
    dev_rig r;
    rig_open(r, 0, 0);
    configure(r.dev);
    rig_start(r);
    (void)wait_model(0, [](const server_model& m) { return m.gain_manual == 0 && m.reg19 == 0x25; });
    int published_rc = -1;
    int published_gain = 0;
    int published_auto = -1;
    assert(rtl_stream_test_apply_gain(r.dev, RTL_STREAM_TEST_GAIN_AUTOGAIN, 200, &published_rc, &published_gain,
                                      &published_auto)
           == 0);
    assert(published_rc == 0 && published_gain == 200 && published_auto == 0);
    assert(rtl_device_is_auto_gain(r.dev) == 0);
    int gain = 0;
    assert(rtl_device_get_tuner_gain(r.dev, &gain) == 0 && gain == 200);
    server_model m = sync_server(r.dev, 0);
    assert(m.gain_manual == 1 && m.gain == 200 && m.reg19 == 0x05);
    lose_connection(r);
    wait_generation(r, 2U);
    m = sync_server(r.dev, 1);
    assert(m.gain_manual == 1 && m.gain == 200 && m.reg19 == 0x05);
    rig_close(r);
}

/* testmode_wins_over_gain_over_tcp: with test mode on, automatic and manual gain changes keep register 0x19 in test
 * mode, and a reconnect restores it; turned off, the digital AGC is back as the gain state wants it. */
void
test_testmode_wins_over_gain_over_tcp(void) {
    server_reset(default_script());
    dev_rig r;
    rig_open(r, 0, 0);
    configure(r.dev);
    rig_start(r);
    assert(rtl_device_set_testmode(r.dev, 1) == 0);
    assert(rtl_device_set_gain(r.dev, kAutoGain) == 0);
    server_model m = sync_server(r.dev, 0);
    assert(m.gain_manual == 0 && m.reg19 == 0x03);
    assert(rtl_device_set_gain(r.dev, 280) == 0);
    m = sync_server(r.dev, 0);
    assert(m.gain_manual == 1 && m.gain == 280 && m.reg19 == 0x03);
    int selected = 0;
    assert(rtl_device_set_gain_nearest(r.dev, 197, &selected) == 0 && selected == 197);
    m = sync_server(r.dev, 0);
    assert(m.gain_manual == 1 && m.gain == 197 && m.reg19 == 0x03);
    lose_connection(r);
    wait_generation(r, 2U);
    m = sync_server(r.dev, 1);
    assert(m.gain_manual == 1 && m.gain == 197 && m.reg19 == 0x03);
    assert(rtl_device_set_testmode(r.dev, 0) == 0);
    m = sync_server(r.dev, 1);
    assert(m.reg19 == 0x05);
    assert(rtl_device_set_gain(r.dev, kAutoGain) == 0);
    m = sync_server(r.dev, 1);
    assert(m.gain_manual == 0 && m.reg19 == 0x25);
    rig_close(r);
}

/* each_gain_command_failure_propagates: each command of a gain change failing (the connection breaking there) fails
 * the change for its caller (the autogain supervisor, a retune's automatic gain), records nothing (the device and the
 * published telemetry keep the gain state from before), and the reconnect restores that state on the server. */
void
gain_failure_case(const dev_rig& r, int from_auto, int fail_cmd, uint64_t* gen) {
    assert(rtl_device_set_gain(r.dev, from_auto ? kAutoGain : 197) == 0);
    rtl_device_test_tcp_fail_command(fail_cmd);
    int published_rc = -1;
    int published_gain = -1;
    int published_auto = -1;
    const int rc = from_auto != 0 ? rtl_stream_test_apply_gain(r.dev, RTL_STREAM_TEST_GAIN_AUTOGAIN, 280, &published_rc,
                                                               &published_gain, &published_auto)
                                  : rtl_stream_test_apply_gain(r.dev, RTL_STREAM_TEST_GAIN_RETUNE_AUTO, 0,
                                                               &published_rc, &published_gain, &published_auto);
    rtl_device_test_tcp_fail_command(-1);
    assert(rc != 0);
    assert(rtl_device_is_auto_gain(r.dev) == (from_auto ? 1 : 0));
    assert(published_rc == 0 && published_auto == (from_auto ? 1 : 0));
    if (from_auto == 0) {
        assert(published_gain == 197);
    }
    *gen += 1U;
    wait_generation(r, *gen);
    (void)wait_model((size_t)(*gen - 1U), [&](const server_model& m) {
        return from_auto ? (m.gain_manual == 0 && m.reg19 == 0x25)
                         : (m.gain_manual == 1 && m.gain == 197 && m.reg19 == 0x05);
    });
}

void
test_each_gain_command_failure_propagates(void) {
    server_reset(default_script());
    dev_rig r;
    rig_open(r, 0, 0);
    configure(r.dev);
    rig_start(r);
    uint64_t gen = 1U;
    gain_failure_case(r, 1, 0x08, &gen);
    gain_failure_case(r, 1, 0x03, &gen);
    gain_failure_case(r, 1, 0x04, &gen);
    gain_failure_case(r, 0, 0x03, &gen);
    gain_failure_case(r, 0, 0x08, &gen);
    rig_close(r);
}

/* ---- Stream level ---- */

struct output_reader {
    RtlSdrContext* ctx = nullptr;
    std::atomic<uint64_t> samples{0U};
    std::atomic<int> yields{0};
    std::atomic<int> quit{0};
    std::atomic<int> done{0};
    std::atomic<int> last_rc{0};
    int started = 0;
    dsd_thread_t thread{};
};

std::atomic<int> g_command_queued{0};

int
command_queued(void) {
    return g_command_queued.load();
}

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
            reader->last_rc.store(rc);
            break;
        }
        reader->samples.fetch_add((uint64_t)got);
    }
    reader->done.store(1);
    DSD_THREAD_RETURN;
}

struct stream_rig {
    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = nullptr;
    rtl_device* dev = nullptr;
    output_reader reader;
    char dir[DSD_TEST_PATH_MAX] = {};
    char data_path[DSD_TEST_PATH_MAX] = {};
    char meta_path[DSD_TEST_PATH_MAX] = {};
};

void
stream_opts(stream_rig& r, int capture) {
    r.opts = std::make_unique<dsd_opts>();
    dsd_opts* opts = r.opts.get();
    DSD_MEMSET(opts, 0, sizeof *opts);
    opts->audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtltcp");
    opts->rtltcp_enabled = 1;
    DSD_SNPRINTF(opts->rtltcp_hostname, sizeof opts->rtltcp_hostname, "%s", "127.0.0.1");
    opts->rtltcp_portno = g_srv.port;
    opts->rtlsdr_center_freq = 851012500U;
    opts->rtl_dsp_bw_khz = 48;
    opts->rtl_gain_value = 24;
    opts->frame_dmr = 1;
    if (capture != 0) {
        assert(dsd_test_mkdtemp(r.dir, sizeof r.dir, "dsdneo_rtl_tcp_stream") != nullptr);
        assert(dsd_test_path_join(r.data_path, sizeof r.data_path, r.dir, "capture.iq") == 0);
        assert(dsd_test_path_join(r.meta_path, sizeof r.meta_path, r.dir, "capture.iq.json") == 0);
        opts->iq_capture_requested = 1;
        opts->iq_capture_format = DSD_IQ_FORMAT_CU8;
        DSD_SNPRINTF(opts->iq_capture_path, sizeof opts->iq_capture_path, "%s", r.data_path);
    }
}

void
stream_open(stream_rig& r, int capture, int reader) {
    stream_opts(r, capture);
    dsd_input_failure_clear();
    dsd_exitflag_store(0);
    assert(rtl_stream_create(r.opts.get(), &r.ctx) == 0 && r.ctx != nullptr);
    assert(rtl_stream_start(r.ctx) == 0);
    r.dev = rtl_stream_test_device();
    assert(r.dev != nullptr);
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
    r.reader.quit.store(1);
    assert(wait_until([&] { return r.reader.done.load() != 0; }, kWaitMs));
    dsd_thread_join(r.reader.thread);
    r.reader.started = 0;
}

void
stream_close(stream_rig& r) {
    stop_reader(r);
    (void)rtl_stream_stop(r.ctx);
    (void)rtl_stream_destroy(r.ctx);
    r.ctx = nullptr;
    r.dev = nullptr;
}

/* The stream's device, lost and with no stream yet (its reader is reconnecting or its hand-off waits). */
void
stream_lose_connection(const stream_rig& r) {
    server_kill();
    assert(wait_until([&] { return rtl_device_has_stream(r.dev) == 0; }, kWaitMs));
}

/* unavailable_server_command_applies (B3): while the server is gone the decoder's read gives way to a queued command
 * at once, and a connected stream that only pauses never does. */
void
test_unavailable_server_command_applies(void) {
    const conn_script sc;
    server_reset(sc);
    dsd_runtime_set_controls_pending(command_queued);
    stream_rig r;
    stream_open(r, 0, 1);
    assert(wait_until([&] { return r.reader.samples.load() > 4096U; }, kWaitMs));
    /* Connected but paused (well inside the stall rule): a queued command waits. */
    g_srv.paused.store(1);
    dsd_sleep_ms(100U);
    g_command_queued.store(1);
    dsd_sleep_ms(150U);
    assert(r.reader.yields.load() == 0);
    g_command_queued.store(0);
    g_srv.paused.store(0);
    /* Gone: it applies within a few read slices. */
    g_srv.up.store(0);
    stream_lose_connection(r);
    g_command_queued.store(1);
    assert(wait_until([&] { return r.reader.yields.load() >= 1; }, 300U));
    assert(g_command_queued.load() == 0);
    g_srv.up.store(1);
    const uint64_t before = r.reader.samples.load();
    assert(wait_until([&] { return r.reader.samples.load() > before + 4096U; }, kWaitMs));
    stream_close(r);
    dsd_runtime_set_controls_pending(nullptr);
}

/* preconfig_backlog_discarded: the controller is held behind a retune while the reconnected server streams far more
 * pre-configuration I/Q than the post-reset mute covers; none of it reaches the capture (which sees what reaches the
 * rings), and the capture after the gap holds whole pairs of the server's configured I/Q. */
std::atomic<int> g_hold_retune{0};
std::atomic<int> g_retune_held{0};

void
// NOLINTNEXTLINE(misc-const-correctness) -- The signature is the controller work hook's.
hold_retune(int kind, void* ctx) {
    (void)ctx;
    if (kind != RTL_STREAM_TEST_WORK_RETUNE || g_hold_retune.load() == 0) {
        return;
    }
    g_retune_held.store(1);
    while (g_hold_retune.load() != 0) {
        dsd_sleep_ms(1U);
    }
}

void
test_preconfig_backlog_discarded(void) {
    server_reset(default_script());
    conn_script pre;
    pre.pre_until_command = 1;
    server_queue(conn_script{});
    server_queue(pre);
    rtl_stream_test_set_controller_work_hook(hold_retune, nullptr);
    stream_rig r;
    stream_open(r, 1, 1);
    assert(wait_until([&] { return r.reader.samples.load() > 4096U; }, kWaitMs));
    g_hold_retune.store(1);
    assert(dsd_rtl_stream_test_request_retune(852000000L, 0) == 0);
    assert(wait_until([] { return g_retune_held.load() != 0; }, kWaitMs));
    stream_lose_connection(r);
    /* The reconnected server streams pre-configuration I/Q while the hand-off waits behind the retune. */
    assert(wait_until([] { return server_conn_count() >= 2U && server_conn_get(1).pre_sent > 1500000U; }, kWaitMs));
    rtl_device_test_tcp_state st{};
    rtl_device_test_tcp_state_get(r.dev, &st);
    assert(st.handoff == RTL_TEST_TCP_HANDOFF_QUEUED);
    g_hold_retune.store(0);
    assert(wait_until([&] { return rtl_device_has_stream(r.dev) == 1; }, kWaitMs));
    const uint64_t before = r.reader.samples.load();
    assert(wait_until([&] { return r.reader.samples.load() > before + 65536U; }, kWaitMs));
    stream_close(r);
    rtl_stream_test_set_controller_work_hook(nullptr, nullptr);
    g_retune_held.store(0);

    dsd_iq_replay_config cfg{};
    char err[256] = {};
    assert(dsd_iq_replay_read_metadata(r.meta_path, &cfg, err, sizeof err) == 0);
    uint64_t gap_offset = 0U;
    for (uint32_t i = 0; i < cfg.event_count; ++i) {
        if (cfg.events[i].kind == DSD_IQ_EVENT_RESET
            && strcmp(cfg.events[i].reason, RTL_STREAM_GAP_REASON_TCP_RECONNECT) == 0) {
            gap_offset = cfg.events[i].byte_offset;
        }
    }
    dsd_iq_replay_config_clear(&cfg);
    assert(gap_offset > 0U);
    const std::vector<unsigned char> data = read_file(r.data_path);
    assert(data.size() > gap_offset + 65536U);
    for (size_t i = 0; i < data.size(); ++i) {
        assert(((i & 1U) == 0U) == (data[i] >= 0x80U));
        assert(byte_phase(data[i]) != PHASE_PRE);
    }
    assert(dsd_test_remove_temp_dir(r.dir, kCaptureFiles) == 0);
}

/* stop_with_handoff_queued_and_claimed: a stop with the hand-off queued behind a retune, and one with the hand-off
 * taken (the controller inside its stream boundary), joins; the reconnected connection is closed. */
std::atomic<int> g_stop_at_gate{0};
RtlSdrContext* g_stop_ctx = nullptr;
std::atomic<int> g_stop_done{0};

DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
// NOLINTNEXTLINE(misc-const-correctness) -- The signature is a thread entry's.
stream_stopper(void* arg) {
    (void)arg;
    (void)rtl_stream_stop(g_stop_ctx);
    g_stop_done.store(1);
    DSD_THREAD_RETURN;
}

dsd_thread_t g_stream_stopper{};

void
// NOLINTNEXTLINE(misc-const-correctness) -- The signature is the boundary probe's.
stop_at_gate(int step, void* ctx) {
    (void)ctx;
    if (step != RTL_STREAM_TEST_BOUNDARY_GATE || g_stop_at_gate.exchange(0) == 0) {
        return;
    }
    rtl_device* dev = rtl_stream_test_device();
    start_thread(&g_stream_stopper, stream_stopper, nullptr);
    assert(wait_until(
        [&] {
            rtl_device_test_tcp_state st{};
            rtl_device_test_tcp_state_get(dev, &st);
            return st.stop_requested != 0;
        },
        kWaitMs));
}

void
test_stop_with_handoff_queued_and_claimed(void) {
    /* Queued behind a held retune. */
    server_reset(default_script());
    rtl_stream_test_set_controller_work_hook(hold_retune, nullptr);
    stream_rig r;
    stream_open(r, 0, 0);
    g_hold_retune.store(1);
    assert(dsd_rtl_stream_test_request_retune(852000000L, 0) == 0);
    assert(wait_until([] { return g_retune_held.load() != 0; }, kWaitMs));
    stream_lose_connection(r);
    assert(wait_until(
        [&] {
            rtl_device_test_tcp_state st{};
            rtl_device_test_tcp_state_get(r.dev, &st);
            return st.handoff == RTL_TEST_TCP_HANDOFF_QUEUED;
        },
        kWaitMs));
    g_stop_ctx = r.ctx;
    g_stop_done.store(0);
    start_thread(&g_stream_stopper, stream_stopper, nullptr);
    /* The device stop cancels the hand-off and joins the reader, which closes the connection; then the held controller
     * is let go and joined. */
    assert(wait_until([] { return server_conn_get(1).ended != 0; }, kWaitMs));
    g_hold_retune.store(0);
    assert(wait_until([] { return g_stop_done.load() != 0; }, kWaitMs));
    dsd_thread_join(g_stream_stopper);
    (void)rtl_stream_destroy(r.ctx);
    r.ctx = nullptr;
    rtl_stream_test_set_controller_work_hook(nullptr, nullptr);
    g_retune_held.store(0);
    assert(server_conn_count() == 2U);

    /* Taken: the stop comes at the boundary's gate. */
    server_reset(default_script());
    stream_rig r2;
    stream_open(r2, 0, 0);
    g_stop_ctx = r2.ctx;
    g_stop_done.store(0);
    g_stop_at_gate.store(1);
    rtl_stream_test_set_stream_boundary_probe(stop_at_gate, nullptr);
    server_kill();
    assert(wait_until([] { return g_stop_done.load() != 0; }, kWaitMs));
    dsd_thread_join(g_stream_stopper);
    rtl_stream_test_set_stream_boundary_probe(nullptr, nullptr);
    assert(g_stop_at_gate.load() == 0);
    assert(wait_until([] { return server_conn_count() >= 2U && server_conn_get(1).ended != 0; }, kWaitMs));
    (void)rtl_stream_destroy(r2.ctx);
    r2.ctx = nullptr;
    assert(server_conn_count() == 2U);
}

/* controller_create_failure_fails_start (B17): a controller thread that cannot start fails the rtl_tcp stream's start
 * and leaves nothing behind. */
int g_create_calls = 0;

int
fail_first_create(dsd_thread_t* thread, dsd_thread_fn entry, void* ctx) {
    if (g_create_calls++ == 0) {
        return -1;
    }
    return dsd_thread_create(thread, entry, ctx);
}

void
test_controller_create_failure_fails_start(void) {
    server_reset(default_script());
    stream_rig r;
    stream_opts(r, 0);
    g_create_calls = 0;
    rtl_stream_test_set_thread_create(fail_first_create);
    assert(rtl_stream_create(r.opts.get(), &r.ctx) == 0 && r.ctx != nullptr);
    assert(rtl_stream_start(r.ctx) != 0);
    rtl_stream_test_set_thread_create(nullptr);
    assert(g_create_calls >= 1);
    assert(rtl_stream_test_has_resources() == 0);
    (void)rtl_stream_destroy(r.ctx);
    assert(wait_until([] { return server_conn_count() >= 1U && server_conn_get(0).ended != 0; }, kWaitMs));
}

/* quit_during_outage_exits: with the server gone for a while (the reconnect backing off), an exit request (QUIT
 * raises it at submit) ends the decoder's read and the stream's stop promptly. */
void
test_quit_during_outage_exits(void) {
    server_reset(default_script());
    stream_rig r;
    stream_open(r, 0, 1);
    assert(wait_until([&] { return r.reader.samples.load() > 4096U; }, kWaitMs));
    g_srv.up.store(0);
    stream_lose_connection(r);
    dsd_sleep_ms(2500U);
    const uint64_t t0 = now_ms();
    dsd_exitflag_store(1);
    assert(wait_until([&] { return r.reader.done.load() != 0; }, 500U));
    dsd_thread_join(r.reader.thread);
    r.reader.started = 0;
    (void)rtl_stream_stop(r.ctx);
    assert(now_ms() - t0 < 600U);
    (void)rtl_stream_destroy(r.ctx);
    r.ctx = nullptr;
    dsd_exitflag_store(0);
    g_srv.up.store(1);
}

struct named_test {
    const char* name;
    void (*run)(void);
};

const named_test kTests[] = {
    {"header_has_no_gain_table", test_header_has_no_gain_table},
    {"header_fragmented_across_timeouts", test_header_fragmented_across_timeouts},
    {
        "initial_open_succeeds_and_cancelled_initial_open_stops",
        test_initial_open_succeeds_and_cancelled_initial_open_stops,
    },
    {"stop_before_successful_connect", test_stop_before_successful_connect},
    {"stop_during_silent_header", test_stop_during_silent_header},
    {"setting_change_while_disconnected_fails", test_setting_change_while_disconnected_fails},
    {"latest_effective_settings_replayed", test_latest_effective_settings_replayed},
    {"replay_failure_not_published", test_replay_failure_not_published},
    {"concurrent_partial_writes_never_interleave", test_concurrent_partial_writes_never_interleave},
    {"old_sender_does_not_touch_replacement", test_old_sender_does_not_touch_replacement},
    {"stop_during_replay", test_stop_during_replay},
    {"stop_during_backpressured_send", test_stop_during_backpressured_send},
    {"discard_release_keeps_pair_alignment", test_discard_release_keeps_pair_alignment},
    {
        "nearest_manual_gain_sends_agc_off_and_survives_reconnect",
        test_nearest_manual_gain_sends_agc_off_and_survives_reconnect,
    },
    {"testmode_wins_over_gain_over_tcp", test_testmode_wins_over_gain_over_tcp},
    {"each_gain_command_failure_propagates", test_each_gain_command_failure_propagates},
    {"unavailable_server_command_applies", test_unavailable_server_command_applies},
    {"preconfig_backlog_discarded", test_preconfig_backlog_discarded},
    {"stop_with_handoff_queued_and_claimed", test_stop_with_handoff_queued_and_claimed},
    {"controller_create_failure_fails_start", test_controller_create_failure_fails_start},
    {"quit_during_outage_exits", test_quit_during_outage_exits},
};

} // namespace

/* With no argument every case runs, in order; with one, only the case of that name. */
int
// NOLINTNEXTLINE(misc-const-correctness) -- main's signature.
main(int argc, char** argv) {
    (void)signal(SIGPIPE, SIG_IGN);
    assert(dsd_socket_init() == 0);
    assert(dsd_test_setenv("DSD_NEO_TCP_RCVTIMEO", "200", 1) == 0);
    assert(dsd_test_setenv("DSD_NEO_TCP_MAX_TIMEOUTS", "2", 1) == 0);
    assert(dsd_test_setenv("DSD_NEO_TCP_PREBUF_MS", "5", 1) == 0);
    dsd_neo_config_init();
    server_start();

    int ran = 0;
    for (const named_test& t : kTests) {
        if (argc > 1 && strcmp(argv[1], t.name) != 0) {
            continue;
        }
        DSD_FPRINTF(stderr, "-- %s\n", t.name);
        t.run();
        ran++;
    }
    assert(ran > 0);

    server_shutdown();
    dsd_socket_cleanup();
    DSD_FPRINTF(stderr, "IO_RTL_TCP_RECONNECT: OK\n");
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
