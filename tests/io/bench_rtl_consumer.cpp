// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Opt-in consumer benchmark: the decoder's symbol reads (getSymbol()) through the production live reader of a
 * running RTL stream, as the engine installs it. An in-process rtl_tcp server on loopback paces cu8 I/Q to the sample
 * rate the stream asks for, the stream's own TCP reader, demod and output ring run as in a session, and this thread
 * reads symbols until the run ends. --path picks the reader:
 *
 *   cqpsk    the symbol cache on the CQPSK symbol output (P25 CQPSK)
 *   fsk      the symbol cache on the FSK discriminator output (DMR C4FM)
 *   monitor  the one-sample reader on the analog monitor output
 *
 * After --warmup seconds it measures for --seconds and reports the reading thread's CPU time per symbol and the whole
 * process's (server, TCP reader, demod and reader) per symbol:
 *
 *   cmake --preset dev-release -DBUILD_TESTING=ON
 *   cmake --build --preset dev-release --target dsd-neo_bench_rtl_consumer -j
 *   build/dev-release/tests/dsd-neo_bench_rtl_consumer --path fsk --seconds 20
 */

#include <arpa/inet.h>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/io/rtl_stream_fwd.h"

extern "C" {
#include "engine_hooks_install.h"
}

namespace {

struct BenchOptions {
    const char* path = "fsk";
    uint32_t seconds = 20U;
    uint32_t warmup = 2U;
};

/* The loopback rtl_tcp server: a standard 12-byte header (no gain table), then cu8 I/Q paced to the rate the client
 * last set (0x02), in chunks; the client's other commands are read and ignored. */
struct Server {
    int listen_fd = -1;
    int port = 0;
    std::mutex client_lock;
    int client_fd = -1;
    int small_send_buffer = 0;
    std::atomic<int> stop{0};
    std::atomic<uint32_t> rate{1536000U};
    dsd_thread_t thread{};
    std::vector<uint8_t> iq;
};

constexpr size_t kChunkBytes = 16384U;

/* A carrier 10 kHz off centre at the nominal rate, with a little noise: the same bytes in every run. */
void
make_iq(std::vector<uint8_t>* iq) {
    const size_t pairs = 1U << 18;
    iq->resize(pairs * 2U);
    uint32_t lcg = 12345U;
    for (size_t i = 0; i < pairs; ++i) {
        const double phase = 2.0 * 3.14159265358979323846 * 10000.0 * (double)i / 1536000.0;
        lcg = lcg * 1664525U + 1013904223U;
        const double noise_i = ((double)((lcg >> 16) & 0xFFU) - 127.5) / 32.0;
        lcg = lcg * 1664525U + 1013904223U;
        const double noise_q = ((double)((lcg >> 16) & 0xFFU) - 127.5) / 32.0;
        (*iq)[2U * i] = (uint8_t)lround(127.5 + 60.0 * cos(phase) + noise_i);
        (*iq)[2U * i + 1U] = (uint8_t)lround(127.5 + 60.0 * sin(phase) + noise_q);
    }
}

/* Read what the client sent, without waiting; a sample-rate command paces the stream from then on. */
void
server_take_commands(Server* srv, int fd, std::vector<uint8_t>* pending) {
    uint8_t buf[256];
    for (;;) {
        const ssize_t n = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
        if (n <= 0) {
            break;
        }
        pending->insert(pending->end(), buf, buf + n);
    }
    size_t at = 0U;
    while (pending->size() - at >= 5U) {
        const uint8_t* cmd = pending->data() + at;
        const uint32_t param =
            ((uint32_t)cmd[1] << 24) | ((uint32_t)cmd[2] << 16) | ((uint32_t)cmd[3] << 8) | (uint32_t)cmd[4];
        if (cmd[0] == 0x02 && param > 0U) {
            srv->rate.store(param, std::memory_order_relaxed);
        }
        at += 5U;
    }
    pending->erase(pending->begin(), pending->begin() + (std::ptrdiff_t)at);
}

void
server_close_client(Server* srv) {
    std::lock_guard<std::mutex> lock(srv->client_lock);
    if (srv->client_fd >= 0) {
        close(srv->client_fd);
        srv->client_fd = -1;
    }
}

DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
serve(void* arg) {
    auto* srv = static_cast<Server*>(arg);
    const int fd = accept(srv->listen_fd, nullptr, nullptr);
    if (fd < 0) {
        DSD_THREAD_RETURN;
    }
    {
        std::lock_guard<std::mutex> lock(srv->client_lock);
        if (srv->stop.load(std::memory_order_relaxed)) {
            close(fd);
            DSD_THREAD_RETURN;
        }
        srv->client_fd = fd;
    }
    if (srv->small_send_buffer) {
        const int bytes = 1024;
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof bytes);
    }
    const uint8_t header[12] = {'R', 'T', 'L', '0', 0, 0, 0, 5, 0, 0, 0, 0};
    if (send(fd, header, sizeof header, MSG_NOSIGNAL) != (ssize_t)sizeof header) {
        server_close_client(srv);
        DSD_THREAD_RETURN;
    }
    std::vector<uint8_t> pending;
    size_t offset = 0U;
    uint64_t sent_pairs = 0U;
    uint32_t paced_rate = 0U;
    uint64_t paced_since_ns = 0U;
    while (!srv->stop.load(std::memory_order_relaxed)) {
        server_take_commands(srv, fd, &pending);
        const uint32_t rate = srv->rate.load(std::memory_order_relaxed);
        if (rate != paced_rate) {
            paced_rate = rate;
            paced_since_ns = dsd_realtime_mono_ns();
            sent_pairs = 0U;
        }
        const uint64_t due_ns = paced_since_ns + sent_pairs * 1000000000ULL / paced_rate;
        const uint64_t now_ns = dsd_realtime_mono_ns();
        if (due_ns > now_ns) {
            dsd_sleep_ns(due_ns - now_ns);
        }
        if (offset + kChunkBytes > srv->iq.size()) {
            offset = 0U;
        }
        if (send(fd, srv->iq.data() + offset, kChunkBytes, MSG_NOSIGNAL) != (ssize_t)kChunkBytes) {
            break;
        }
        offset += kChunkBytes;
        sent_pairs += kChunkBytes / 2U;
    }
    server_close_client(srv);
    DSD_THREAD_RETURN;
}

int
server_start(Server* srv) {
    make_iq(&srv->iq);
    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) {
        return -1;
    }
    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    socklen_t len = sizeof addr;
    if (bind(srv->listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) != 0
        || listen(srv->listen_fd, 1) != 0
        || getsockname(srv->listen_fd, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0) {
        close(srv->listen_fd);
        return -1;
    }
    srv->port = ntohs(addr.sin_port);
    const int rc = dsd_thread_create(&srv->thread, serve, srv);
    if (rc != 0) {
        close(srv->listen_fd);
    }
    return rc;
}

void
server_stop(Server* srv) {
    srv->stop.store(1, std::memory_order_relaxed);
    (void)shutdown(srv->listen_fd, SHUT_RDWR);
    {
        std::lock_guard<std::mutex> lock(srv->client_lock);
        if (srv->client_fd >= 0) {
            (void)shutdown(srv->client_fd, SHUT_RDWR);
        }
    }
    dsd_thread_join(srv->thread);
    close(srv->listen_fd);
}

int
check_server_stop(void) {
    for (int connected = 0; connected < 2; connected++) {
        Server server;
        server.small_send_buffer = connected;
        if (server_start(&server) != 0) {
            return 1;
        }
        int peer = -1;
        if (connected) {
            peer = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons((uint16_t)server.port);
            if (peer < 0 || connect(peer, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
                if (peer >= 0) {
                    close(peer);
                }
                server_stop(&server);
                return 1;
            }
            dsd_sleep_ms(250U);
        }
        const uint64_t start = dsd_realtime_mono_ms();
        server_stop(&server);
        if (peer >= 0) {
            close(peer);
        }
        if (dsd_realtime_mono_ms() - start >= 1000U) {
            return 1;
        }
    }
    return 0;
}

/* The decode modes that give each reader its output (rtl_demod_config.cpp, demod_apply_output_kind()). */
int
configure_path(dsd_opts* opts, const char* path) {
    opts->frame_dmr = 0;
    opts->frame_p25p1 = 0;
    opts->frame_p25p2 = 0;
    opts->frame_nxdn48 = 0;
    opts->frame_nxdn96 = 0;
    opts->frame_dstar = 0;
    opts->frame_dpmr = 0;
    opts->frame_provoice = 0;
    opts->frame_x2tdma = 0;
    opts->frame_ysf = 0;
    opts->frame_m17 = 0;
    opts->mod_c4fm = 0;
    opts->mod_qpsk = 0;
    opts->mod_gfsk = 0;
    opts->analog_only = 0;
    if (std::strcmp(path, "cqpsk") == 0) {
        opts->frame_p25p1 = 1;
        opts->mod_qpsk = 1;
        return 0;
    }
    if (std::strcmp(path, "fsk") == 0) {
        opts->frame_dmr = 1;
        opts->mod_c4fm = 1;
        return 0;
    }
    if (std::strcmp(path, "monitor") == 0) {
        opts->analog_only = 1;
        return 0;
    }
    return -1;
}

int
parse_options(int argc, char** argv, BenchOptions* options) {
    for (int i = 1; i < argc; i += 2) {
        const char* arg = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
        if (std::strcmp(arg, "--path") == 0 && value) {
            options->path = value;
        } else if (std::strcmp(arg, "--seconds") == 0 && value) {
            if (dsd_parse_uint32_strict(value, 10, 3600U, &options->seconds) != 0 || options->seconds == 0U) {
                return -1;
            }
        } else if (std::strcmp(arg, "--warmup") == 0 && value) {
            if (dsd_parse_uint32_strict(value, 10, 600U, &options->warmup) != 0) {
                return -1;
            }
        } else {
            return -1;
        }
    }
    return 0;
}

uint64_t
thread_cpu_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) {
        return 0U;
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t
process_cpu_ns(void) {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) {
        return 0U;
    }
    const uint64_t user = (uint64_t)ru.ru_utime.tv_sec * 1000000000ULL + (uint64_t)ru.ru_utime.tv_usec * 1000ULL;
    const uint64_t sys = (uint64_t)ru.ru_stime.tv_sec * 1000000000ULL + (uint64_t)ru.ru_stime.tv_usec * 1000ULL;
    return user + sys;
}

/* Read symbols until @p until_ns; the count read. An interruption a read raised (an idle monitor read) is taken as
 * the engine loop takes it, and counted in @p interruptions. */
uint64_t
read_symbols(dsd_opts* opts, dsd_state* state, uint64_t until_ns, float* sink, uint64_t* interruptions) {
    uint64_t symbols = 0U;
    while (dsd_realtime_mono_ns() < until_ns && !dsd_exitflag_load()) {
        for (int i = 0; i < 64; ++i) {
            *sink += getSymbol(opts, state, 0);
        }
        symbols += 64U;
        if (state->input_interrupted) {
            state->input_interrupted = 0;
            ++*interruptions;
        }
    }
    return symbols;
}

} // namespace

int
main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--check-stop") == 0) {
        return check_server_stop();
    }
    BenchOptions options;
    if (parse_options(argc, argv, &options) != 0) {
        std::printf("Usage: %s [--path cqpsk|fsk|monitor] [--seconds N] [--warmup N]\n", argv[0]);
        return 2;
    }
    auto opts = std::make_unique<dsd_opts>();
    auto state = std::make_unique<dsd_state>();
    initOpts(opts.get());
    initState(state.get());
    if (configure_path(opts.get(), options.path) != 0) {
        std::printf("Unknown path: %s\n", options.path);
        return 2;
    }

    Server srv;
    if (server_start(&srv) != 0) {
        DSD_FPRINTF(stderr, "bench_rtl_consumer: could not start the rtl_tcp server\n");
        return 1;
    }
    opts->audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtltcp");
    opts->rtltcp_enabled = 1;
    DSD_SNPRINTF(opts->rtltcp_hostname, sizeof opts->rtltcp_hostname, "%s", "127.0.0.1");
    opts->rtltcp_portno = srv.port;
    opts->rtlsdr_center_freq = 851012500U;
    opts->rtl_dsp_bw_khz = 48;
    opts->rtl_gain_value = 24;

    dsd_engine_rtl_stream_io_hooks_install();
    dsd_engine_rtl_stream_metrics_hooks_install();
    RtlSdrContext* ctx = nullptr;
    if (rtl_stream_create(opts.get(), &ctx) != 0 || !ctx || rtl_stream_start(ctx) != 0) {
        DSD_FPRINTF(stderr, "bench_rtl_consumer: the stream did not start\n");
        if (ctx) {
            rtl_stream_destroy(ctx);
        }
        server_stop(&srv);
        return 1;
    }
    state->rtl_ctx = ctx;

    float sink = 0.0f;
    uint64_t interruptions = 0U;
    (void)read_symbols(opts.get(), state.get(), dsd_realtime_mono_ns() + (uint64_t)options.warmup * 1000000000ULL,
                       &sink, &interruptions);
    interruptions = 0U;
    const uint64_t wall0 = dsd_realtime_mono_ns();
    const uint64_t thread0 = thread_cpu_ns();
    const uint64_t process0 = process_cpu_ns();
    const uint64_t symbols =
        read_symbols(opts.get(), state.get(), wall0 + (uint64_t)options.seconds * 1000000000ULL, &sink, &interruptions);
    const uint64_t thread_ns = thread_cpu_ns() - thread0;
    const uint64_t process_ns = process_cpu_ns() - process0;
    const uint64_t wall_ns = dsd_realtime_mono_ns() - wall0;

    dsd_exitflag_store(1);
    (void)rtl_stream_stop(ctx);
    (void)rtl_stream_destroy(ctx);
    state->rtl_ctx = nullptr;
    server_stop(&srv);
    dsd_exitflag_store(0);
    freeState(state.get());

    const double per_symbol = symbols > 0U ? 1.0 / (double)symbols : 0.0;
    std::printf("path=%s symbols=%llu symbols_per_s=%.0f reader_cpu_ns_per_symbol=%.1f "
                "process_cpu_ns_per_symbol=%.1f process_cpu_pct=%.1f interruptions=%llu sink=%g\n",
                options.path, (unsigned long long)symbols, (double)symbols * 1e9 / (double)wall_ns,
                (double)thread_ns * per_symbol, (double)process_ns * per_symbol,
                100.0 * (double)process_ns / (double)wall_ns, (unsigned long long)interruptions, (double)sink);
    return 0;
}
