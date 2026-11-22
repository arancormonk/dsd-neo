// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * An in-process rtl_tcp server on loopback that plays committed I/Q fixtures as the air a tuner would hear (issue #899).
 *
 * It sends the 12-byte "RTL0" header (an R820T with 29 gains), reads the 5-byte commands it is sent and keeps the
 * frequency and sample rate they set, and streams u8 I/Q at that rate, paced to real time. Each row names a channel
 * frequency and a program of 48 kHz cu8 fixtures (tests/fixtures/iq, cu8 at 48 kHz); the server plays the program of
 * the row within half the sample rate of the tuned frequency, held up to the rate (every rtl rate the stream asks for
 * is a multiple of 48 kHz) and shifted to the row's offset from the tuned centre (the stream tunes the dongle off the
 * channel), and plays the noise program off every row. A retune (a frequency or rate command) starts the program again
 * from its start, so each visit of a scan hears the same air from its landing. A program loops at its end.
 *
 * POSIX test hosts only (loopback sockets through <dsd-neo/platform/sockets.h>).
 */
#ifndef DSD_NEO_TESTS_RTL_TCP_FIXTURE_SERVER_H
#define DSD_NEO_TESTS_RTL_TCP_FIXTURE_SERVER_H

#include <atomic>
#include <complex>
#include <cstdint>
#include <dsd-neo/platform/sockets.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dsd_test {

/* A stretch of one committed fixture: @p name without its extension, from @p start_s for @p length_s seconds (0 or
   less: to its end). */
struct FixtureSegment {
    std::string name;
    double start_s = 0.0;
    double length_s = 0.0;
};

/* A channel on the air: its frequency and what plays there, the segments in order. */
struct FixtureRow {
    uint32_t freq_hz = 0U;
    std::vector<FixtureSegment> program;
};

class RtlTcpFixtureServer {
  public:
    RtlTcpFixtureServer() = default;
    ~RtlTcpFixtureServer();
    RtlTcpFixtureServer(const RtlTcpFixtureServer&) = delete;
    RtlTcpFixtureServer& operator=(const RtlTcpFixtureServer&) = delete;

    /* Load every row's program and @p noise (what plays off every row) from @p fixture_dir and listen on an ephemeral
       loopback port. false with @p error set when a fixture cannot be read or the listener cannot open. */
    bool start(const std::string& fixture_dir, const std::vector<FixtureRow>& rows,
               const std::vector<FixtureSegment>& noise, std::string* error);
    /* Stop serving and close the listener; the client sees its connection end. */
    void stop();

    int
    port() const {
        return m_port;
    }

    /* What the client last tuned and asked for, and how many programs the server started (one per retune). */
    uint32_t tuned_hz() const;
    uint32_t rate_hz() const;
    uint64_t programs_started() const;
    /* The frequency of the row whose program plays now, 0 for the noise. */
    uint32_t serving_row_hz() const;

  private:
    struct Program {
        uint32_t row_hz = 0U;
        std::vector<std::complex<float>> samples; /* 48 kHz, about the cu8 midpoint */
    };

    void serve_main();
    void serve_connection(dsd_socket_t sock);
    bool read_commands(dsd_socket_t sock);
    const Program& program_for(uint32_t tuned_hz, uint32_t rate_hz) const;

    std::vector<Program> m_rows;
    Program m_noise;
    dsd_socket_t m_listener = DSD_INVALID_SOCKET;
    int m_port = 0;
    std::thread m_thread;
    std::atomic<int> m_quit{0};
    mutable std::mutex m_lock;
    uint32_t m_tuned_hz = 100000000U;
    uint32_t m_rate_hz = 2048000U;
    uint64_t m_settings_generation = 0U;
    uint64_t m_programs_started = 0U;
    uint32_t m_serving_row_hz = 0U;
    unsigned char m_carry[5] = {0, 0, 0, 0, 0};
    size_t m_carry_len = 0U;
};

} // namespace dsd_test

#endif
