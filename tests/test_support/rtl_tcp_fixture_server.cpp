// SPDX-License-Identifier: GPL-3.0-or-later
/* The fixture-playing rtl_tcp server (rtl_tcp_fixture_server.h, issue #899). */

#include "rtl_tcp_fixture_server.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace dsd_test {

namespace {

constexpr uint32_t kFixtureRateHz = 48000U;
/* The I/Q a send carries, in 5 ms of the rate: small enough that the pacing stays close to real time. */
constexpr uint32_t kChunksPerSecond = 200U;
constexpr double kTwoPi = 6.283185307179586;

/* Read @p seg of a committed cu8 fixture into @p out, about the cu8 midpoint. */
bool
load_segment(const std::string& dir, const FixtureSegment& seg, std::vector<std::complex<float>>* out,
             std::string* error) {
    const std::string path = dir + "/" + seg.name + ".iq";
    FILE* f = dsd_fopen_existing_regular_file(path.c_str(), "rb");
    if (f == nullptr) {
        *error = "cannot open " + path;
        return false;
    }
    std::vector<unsigned char> bytes;
    unsigned char buf[65536];
    size_t n = 0U;
    while ((n = std::fread(buf, 1U, sizeof buf, f)) > 0U) {
        bytes.insert(bytes.end(), buf, buf + n);
    }
    (void)std::fclose(f);
    const size_t pairs = bytes.size() / 2U;
    const size_t first = static_cast<size_t>(std::llround(seg.start_s * kFixtureRateHz));
    size_t last = pairs;
    if (seg.length_s > 0.0) {
        const size_t want = first + static_cast<size_t>(std::llround(seg.length_s * kFixtureRateHz));
        last = want < pairs ? want : pairs;
    }
    if (first >= last) {
        *error = "empty stretch of " + seg.name;
        return false;
    }
    for (size_t i = first; i < last; ++i) {
        out->emplace_back(static_cast<float>(bytes[2U * i]) - 127.5f, static_cast<float>(bytes[2U * i + 1U]) - 127.5f);
    }
    return true;
}

bool
load_program(const std::string& dir, const std::vector<FixtureSegment>& program, std::vector<std::complex<float>>* out,
             std::string* error) {
    for (const auto& seg : program) {
        if (!load_segment(dir, seg, out, error)) {
            return false;
        }
    }
    if (out->empty()) {
        *error = "empty program";
        return false;
    }
    return true;
}

unsigned char
to_cu8(float v) {
    const float shifted = std::nearbyint(v + 127.5f);
    return static_cast<unsigned char>(shifted < 0.0f ? 0.0f : (shifted > 255.0f ? 255.0f : shifted));
}

/* Send all of @p len bytes; false when the connection ended. */
bool
send_all(dsd_socket_t sock, const unsigned char* buf, size_t len, const std::atomic<int>& quit) {
    size_t sent = 0U;
    while (sent < len && quit.load() == 0) {
        const int n = dsd_socket_send(sock, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        const int e = dsd_socket_get_error();
        if (n < 0 && (e == EAGAIN || e == EWOULDBLOCK || e == EINTR)) {
            (void)dsd_socket_wait(sock, DSD_SOCKET_WAIT_WRITE, 20U);
            continue;
        }
        return false;
    }
    return sent == len;
}

} // namespace

RtlTcpFixtureServer::~RtlTcpFixtureServer() { stop(); }

bool
RtlTcpFixtureServer::start(const std::string& fixture_dir, const std::vector<FixtureRow>& rows,
                           const std::vector<FixtureSegment>& noise, std::string* error) {
    std::string why;
    for (const auto& row : rows) {
        Program p;
        p.row_hz = row.freq_hz;
        if (!load_program(fixture_dir, row.program, &p.samples, &why)) {
            *error = why;
            return false;
        }
        m_rows.push_back(std::move(p));
    }
    if (!load_program(fixture_dir, noise, &m_noise.samples, &why)) {
        *error = why;
        return false;
    }
    m_listener = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    if (m_listener == DSD_INVALID_SOCKET) {
        *error = "no socket";
        return false;
    }
    int one = 1;
    (void)dsd_socket_setsockopt(m_listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(0x7F000001U);
    socklen_t len = sizeof addr;
    if (dsd_socket_bind(m_listener, reinterpret_cast<const struct sockaddr*>(&addr), static_cast<int>(sizeof addr)) != 0
        || dsd_socket_listen(m_listener, 2) != 0
        || getsockname(m_listener, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0) {
        (void)dsd_socket_close(m_listener);
        m_listener = DSD_INVALID_SOCKET;
        *error = "cannot listen on loopback";
        return false;
    }
    m_port = static_cast<int>(ntohs(addr.sin_port));
    m_thread = std::thread([this] { serve_main(); });
    return true;
}

void
RtlTcpFixtureServer::stop() {
    m_quit.store(1);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_listener != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(m_listener);
        m_listener = DSD_INVALID_SOCKET;
    }
}

uint32_t
RtlTcpFixtureServer::tuned_hz() const {
    std::lock_guard<std::mutex> hold(m_lock);
    return m_tuned_hz;
}

uint32_t
RtlTcpFixtureServer::rate_hz() const {
    std::lock_guard<std::mutex> hold(m_lock);
    return m_rate_hz;
}

uint64_t
RtlTcpFixtureServer::programs_started() const {
    std::lock_guard<std::mutex> hold(m_lock);
    return m_programs_started;
}

uint32_t
RtlTcpFixtureServer::serving_row_hz() const {
    std::lock_guard<std::mutex> hold(m_lock);
    return m_serving_row_hz;
}

void
RtlTcpFixtureServer::serve_main() {
    while (m_quit.load() == 0) {
        if (dsd_socket_wait(m_listener, DSD_SOCKET_WAIT_READ, 20U) <= 0) {
            continue;
        }
        const dsd_socket_t sock = dsd_socket_accept(m_listener, nullptr, nullptr);
        if (sock == DSD_INVALID_SOCKET) {
            continue;
        }
        /* Nonblocking, so a client that stops reading cannot hold a send, and stop(), past its bounded wait
           (send_all()). */
        if (dsd_socket_set_nonblocking(sock, 1) != 0) {
            (void)dsd_socket_close(sock);
            continue;
        }
        serve_connection(sock);
        (void)dsd_socket_shutdown(sock, SHUT_RDWR);
        (void)dsd_socket_close(sock);
    }
}

/* Take every command the client has sent; false when the connection ended. */
bool
RtlTcpFixtureServer::read_commands(dsd_socket_t sock) {
    unsigned char buf[512];
    while (dsd_socket_wait(sock, DSD_SOCKET_WAIT_READ, 0U) > 0) {
        const int n = dsd_socket_recv(sock, buf, sizeof buf, 0);
        if (n < 0) {
            const int e = dsd_socket_get_error();
            if (e == EAGAIN || e == EWOULDBLOCK || e == EINTR) {
                return true;
            }
        }
        if (n <= 0) {
            return false;
        }
        std::lock_guard<std::mutex> hold(m_lock);
        for (int i = 0; i < n; ++i) {
            m_carry[m_carry_len++] = buf[i];
            if (m_carry_len < sizeof m_carry) {
                continue;
            }
            m_carry_len = 0U;
            const uint32_t param = (static_cast<uint32_t>(m_carry[1]) << 24) | (static_cast<uint32_t>(m_carry[2]) << 16)
                                   | (static_cast<uint32_t>(m_carry[3]) << 8) | static_cast<uint32_t>(m_carry[4]);
            if (m_carry[0] == 0x01U) {
                m_tuned_hz = param;
                ++m_settings_generation;
            } else if (m_carry[0] == 0x02U) {
                m_rate_hz = param;
                ++m_settings_generation;
            }
        }
    }
    return true;
}

const RtlTcpFixtureServer::Program&
RtlTcpFixtureServer::program_for(uint32_t tuned_hz, uint32_t rate_hz) const {
    for (const auto& row : m_rows) {
        const int64_t off = static_cast<int64_t>(row.row_hz) - static_cast<int64_t>(tuned_hz);
        if (std::llabs(off) < static_cast<int64_t>(rate_hz / 2U)) {
            return row;
        }
    }
    return m_noise;
}

void
RtlTcpFixtureServer::serve_connection(dsd_socket_t sock) {
    {
        std::lock_guard<std::mutex> hold(m_lock);
        m_carry_len = 0U;
    }
    const unsigned char header[12] = {'R', 'T', 'L', '0', 0, 0, 0, 5, 0, 0, 0, 29};
    if (!send_all(sock, header, sizeof header, m_quit)) {
        return;
    }
    uint64_t generation = UINT64_MAX;
    const Program* program = &m_noise;
    uint32_t rate = 0U;
    uint32_t hold_factor = 1U;
    std::complex<double> step(1.0, 0.0);
    std::complex<double> phasor(1.0, 0.0);
    size_t in_pos = 0U;
    uint32_t held = 0U;
    uint64_t sent_samples = 0U;
    uint64_t started_ms = 0U;
    std::vector<unsigned char> out;
    while (m_quit.load() == 0) {
        if (!read_commands(sock)) {
            return;
        }
        uint64_t now_generation = 0U;
        uint32_t tuned = 0U;
        {
            std::lock_guard<std::mutex> hold(m_lock);
            now_generation = m_settings_generation;
            tuned = m_tuned_hz;
            rate = m_rate_hz;
        }
        if (now_generation != generation) {
            generation = now_generation;
            program = &program_for(tuned, rate);
            hold_factor = rate >= kFixtureRateHz ? rate / kFixtureRateHz : 1U;
            const double offset_hz =
                program->row_hz != 0U ? static_cast<double>(program->row_hz) - static_cast<double>(tuned) : 0.0;
            step = std::polar(1.0, kTwoPi * offset_hz / static_cast<double>(rate));
            phasor = std::complex<double>(1.0, 0.0);
            in_pos = 0U;
            held = 0U;
            sent_samples = 0U;
            started_ms = dsd_realtime_mono_ms();
            std::lock_guard<std::mutex> hold(m_lock);
            ++m_programs_started;
            m_serving_row_hz = program->row_hz;
        }
        const uint32_t chunk = rate / kChunksPerSecond;
        out.resize(2U * static_cast<size_t>(chunk));
        for (uint32_t k = 0; k < chunk; ++k) {
            const std::complex<float> x = program->samples[in_pos];
            const std::complex<double> y = std::complex<double>(x.real(), x.imag()) * phasor;
            out[2U * k] = to_cu8(static_cast<float>(y.real()));
            out[2U * k + 1U] = to_cu8(static_cast<float>(y.imag()));
            phasor *= step;
            if (++held == hold_factor) {
                held = 0U;
                if (++in_pos == program->samples.size()) {
                    in_pos = 0U;
                }
            }
        }
        /* Keep the rotation on the unit circle over minutes of samples. */
        phasor /= std::abs(phasor);
        if (!send_all(sock, out.data(), out.size(), m_quit)) {
            return;
        }
        sent_samples += chunk;
        const uint64_t due_ms = started_ms + sent_samples * 1000U / (rate != 0U ? rate : 1U);
        const uint64_t now_ms = dsd_realtime_mono_ms();
        if (due_ms > now_ms) {
            dsd_sleep_ms(static_cast<unsigned int>(due_ms - now_ms));
        }
    }
}

} // namespace dsd_test
