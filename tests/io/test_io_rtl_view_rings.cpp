// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Regression test: the eye/constellation SNR fallbacks cache their estimate per
 * thread, and a cached answer must be bit-for-bit what a fresh estimate over
 * the same rings and inputs returns: after every append batch, a TED SPS
 * change, a bias input change (rate, channel profile) and a ring clear. A call
 * with nothing changed must not recompute, a read that a write batch raced
 * must never be cached, and threads keep their own caches. The threaded case
 * runs writers and readers together so the TSan preset checks the
 * synchronization.
 */

// LLVM 22/GCC 16 misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/snr_bias.h>

#include "rtl_view_rings.h"

namespace {

constexpr double kInvalidDb = -100.0;
constexpr double kValidTolDb = 1.0; /* a real estimate sits well above the -100 dB "no data" answer */

uint64_t
bits_of(double v) {
    uint64_t b = 0;
    DSD_MEMCPY(&b, &v, sizeof b);
    return b;
}

bool
same_bits(double a, double b) {
    return bits_of(a) == bits_of(b);
}

bool
is_estimate(double v) {
    return v > kInvalidDb + kValidTolDb;
}

struct Lcg {
    uint32_t s;

    float
    uniform() { /* [-1, 1) */
        s = s * 1664525u + 1013904223u;
        return static_cast<float>((s >> 8) & 0xFFFFFFu) / 8388608.0f - 1.0f;
    }

    int
    level4() {
        s = s * 1664525u + 1013904223u;
        return static_cast<int>((s >> 16) & 3u);
    }
};

/* Interleaved I/Q of rectangular 4-level symbols (I) with noise, Q zero. */
std::vector<float>
eye_block(Lcg* rng, int symbols, int sps, float noise) {
    static const float kLevels[4] = {-3.0f, -1.0f, 1.0f, 3.0f};
    std::vector<float> iq(static_cast<size_t>(symbols) * static_cast<size_t>(sps) * 2u);
    size_t k = 0;
    for (int s = 0; s < symbols; s++) {
        const float level = kLevels[rng->level4()];
        for (int j = 0; j < sps; j++) {
            iq[k++] = level + noise * rng->uniform();
            iq[k++] = 0.0f;
        }
    }
    return iq;
}

/* Interleaved QPSK points (+-1, +-1) with noise. */
std::vector<float>
qpsk_block(Lcg* rng, int pairs, float noise) {
    std::vector<float> iq(static_cast<size_t>(pairs) * 2u);
    for (int n = 0; n < pairs; n++) {
        const int sym = rng->level4();
        iq[static_cast<size_t>(n) * 2u] = ((sym & 1) ? 1.0f : -1.0f) + noise * rng->uniform();
        iq[static_cast<size_t>(n) * 2u + 1u] = ((sym & 2) ? 1.0f : -1.0f) + noise * rng->uniform();
    }
    return iq;
}

void
append_eye(Lcg* rng, int symbols, int sps, float noise) {
    const std::vector<float> iq = eye_block(rng, symbols, sps, noise);
    rtl_view_eye_append_i_chan(iq.data(), static_cast<int>(iq.size()));
}

void
append_qpsk(Lcg* rng, int pairs, float noise) {
    const std::vector<float> iq = qpsk_block(rng, pairs, noise);
    rtl_view_constellation_append(iq.data(), static_cast<int>(iq.size()), 1);
}

/* One cached call equals a fresh estimate, recomputing exactly `expect_computes` times; returns the estimate. */
double
check_c4fm(const rtl_view_snr_inputs* in, uint64_t expect_computes) {
    const uint64_t before = rtl_view_snr_test_compute_count();
    const double cached = rtl_view_snr_c4fm_eye(in);
    assert(rtl_view_snr_test_compute_count() - before == expect_computes);
    const double fresh = rtl_view_snr_c4fm_eye_uncached(in);
    assert(same_bits(cached, fresh));
    return cached;
}

double
check_gfsk(const rtl_view_snr_inputs* in, uint64_t expect_computes) {
    const uint64_t before = rtl_view_snr_test_compute_count();
    const double cached = rtl_view_snr_gfsk_eye(in);
    assert(rtl_view_snr_test_compute_count() - before == expect_computes);
    const double fresh = rtl_view_snr_gfsk_eye_uncached(in);
    assert(same_bits(cached, fresh));
    return cached;
}

double
check_qpsk(const rtl_view_snr_inputs* in, uint64_t expect_computes) {
    const uint64_t before = rtl_view_snr_test_compute_count();
    const double cached = rtl_view_snr_qpsk_const(in);
    assert(rtl_view_snr_test_compute_count() - before == expect_computes);
    const double fresh = rtl_view_snr_qpsk_const_uncached(in);
    assert(same_bits(cached, fresh));
    return cached;
}

void
test_eye_cache_tracks_inputs(void) {
    Lcg rng = {12345u};
    rtl_view_snr_inputs in = {10, 48000, DSD_CH_LPF_PROFILE_P25_C4FM, 0};
    rtl_view_eye_clear();
    append_eye(&rng, 1700, in.ted_sps, 0.4f); /* fills the 16384-sample ring */

    const double c4fm = check_c4fm(&in, 1u);
    assert(is_estimate(c4fm));
    const double gfsk = check_gfsk(&in, 1u);
    assert(is_estimate(gfsk));
    /* Nothing changed: both answer from their caches. */
    assert(same_bits(check_c4fm(&in, 0u), c4fm));
    assert(same_bits(check_gfsk(&in, 0u), gfsk));

    /* A new batch moves the window the estimates read. */
    append_eye(&rng, 200, in.ted_sps, 0.9f);
    const double c4fm_batch = check_c4fm(&in, 1u);
    assert(!same_bits(c4fm_batch, c4fm));
    (void)check_gfsk(&in, 1u);
    (void)check_c4fm(&in, 0u);

    /* TED SPS moves the eye symbol centers and the bias. */
    in.ted_sps = 9;
    const double c4fm_sps = check_c4fm(&in, 1u);
    assert(!same_bits(c4fm_sps, c4fm_batch));
    (void)check_gfsk(&in, 1u);
    (void)check_c4fm(&in, 0u);

    /* Bias inputs alone. */
    in.rate_out = 24000;
    const double c4fm_rate = check_c4fm(&in, 1u);
    assert(!same_bits(c4fm_rate, c4fm_sps));
    (void)check_gfsk(&in, 1u);
    in.channel_profile = DSD_CH_LPF_PROFILE_6K25;
    const double c4fm_profile = check_c4fm(&in, 1u);
    assert(!same_bits(c4fm_profile, c4fm_rate));
    (void)check_gfsk(&in, 1u);
    (void)check_gfsk(&in, 0u);
    /* The TETRA stage keys the TETRA profile's noise bandwidth. */
    in.channel_profile = DSD_CH_LPF_PROFILE_TETRA;
    const double c4fm_tetra_off = check_c4fm(&in, 1u);
    in.tetra_stage = DSD_SNR_TETRA_STAGE_CHAIN;
    const double c4fm_tetra_on = check_c4fm(&in, 1u);
    assert(!same_bits(c4fm_tetra_on, c4fm_tetra_off));
    (void)check_gfsk(&in, 1u);
    (void)check_gfsk(&in, 0u);
    /* The TETRA channel filter alone (an uplink session) is a third bandwidth. */
    in.tetra_stage = DSD_SNR_TETRA_STAGE_CHANNEL;
    const double c4fm_tetra_channel = check_c4fm(&in, 1u);
    assert(!same_bits(c4fm_tetra_channel, c4fm_tetra_on) && !same_bits(c4fm_tetra_channel, c4fm_tetra_off));
    (void)check_gfsk(&in, 1u);
    (void)check_gfsk(&in, 0u);

    /* Returning to earlier inputs recomputes too: the cache holds one entry. */
    in = {10, 48000, DSD_CH_LPF_PROFILE_P25_C4FM, 0};
    assert(same_bits(check_c4fm(&in, 1u), c4fm_batch));

    /* A clear empties the ring: no estimate, and not the cached one. */
    rtl_view_eye_clear();
    assert(same_bits(check_c4fm(&in, 1u), kInvalidDb));
    assert(same_bits(check_gfsk(&in, 1u), kInvalidDb));
    (void)check_c4fm(&in, 0u);

    /* Refilled after the clear. */
    append_eye(&rng, 1700, in.ted_sps, 0.4f);
    assert(is_estimate(check_c4fm(&in, 1u)));

    /* No SPS published yet: no estimate. */
    in.ted_sps = 0;
    assert(same_bits(check_c4fm(&in, 1u), kInvalidDb));
    (void)check_c4fm(&in, 0u);
    assert(same_bits(rtl_view_snr_c4fm_eye(nullptr), kInvalidDb));
}

void
test_qpsk_cache_tracks_inputs(void) {
    Lcg rng = {777u};
    rtl_view_snr_inputs in = {8, 48000, DSD_CH_LPF_PROFILE_P25_CQPSK, 0};
    rtl_view_constellation_clear();
    append_qpsk(&rng, 8192, 0.2f);

    const double base = check_qpsk(&in, 1u);
    assert(is_estimate(base));
    (void)check_qpsk(&in, 0u);

    append_qpsk(&rng, 1000, 0.6f);
    const double batch = check_qpsk(&in, 1u);
    assert(!same_bits(batch, base));
    (void)check_qpsk(&in, 0u);

    in.ted_sps = 10; /* only the bias reads it here */
    assert(!same_bits(check_qpsk(&in, 1u), batch));
    in.rate_out = 96000;
    (void)check_qpsk(&in, 1u);
    in.channel_profile = DSD_CH_LPF_PROFILE_6K25;
    (void)check_qpsk(&in, 1u);
    (void)check_qpsk(&in, 0u);

    /* An eye batch leaves the constellation estimate cached, and the reverse. */
    append_eye(&rng, 10, 10, 0.1f);
    (void)check_qpsk(&in, 0u);
    rtl_view_snr_inputs eye_in = {10, 48000, DSD_CH_LPF_PROFILE_P25_C4FM, 0};
    (void)check_c4fm(&eye_in, 1u);
    append_qpsk(&rng, 10, 0.1f);
    (void)check_c4fm(&eye_in, 0u);
    (void)check_qpsk(&in, 1u);

    rtl_view_constellation_clear();
    assert(same_bits(check_qpsk(&in, 1u), kInvalidDb));
    (void)check_qpsk(&in, 0u);
}

void
test_raced_read_not_cached(void) {
    Lcg rng = {4242u};
    const rtl_view_snr_inputs in = {10, 48000, DSD_CH_LPF_PROFILE_P25_C4FM, 0};
    rtl_view_eye_clear();
    append_eye(&rng, 1700, in.ted_sps, 0.4f);
    rtl_view_constellation_clear();
    append_qpsk(&rng, 8192, 0.2f);

    rtl_view_snr_test_set_tear(1);
    /* Each read races a batch: answered, never cached. */
    (void)check_c4fm(&in, 1u);
    (void)check_c4fm(&in, 1u);
    (void)check_gfsk(&in, 1u);
    (void)check_gfsk(&in, 1u);
    (void)check_qpsk(&in, 1u);
    (void)check_qpsk(&in, 1u);
    rtl_view_snr_test_set_tear(0);
    (void)check_c4fm(&in, 1u);
    (void)check_c4fm(&in, 0u);
    (void)check_qpsk(&in, 1u);
    (void)check_qpsk(&in, 0u);
}

void
test_threads_keep_own_cache(void) {
    Lcg rng = {99u};
    const rtl_view_snr_inputs in = {10, 48000, DSD_CH_LPF_PROFILE_P25_C4FM, 0};
    rtl_view_eye_clear();
    append_eye(&rng, 1700, in.ted_sps, 0.4f);
    const double main_value = check_c4fm(&in, 1u);
    (void)check_c4fm(&in, 0u);
    double other_value = 0.0;
    uint64_t other_computes = 0;
    std::thread t([&]() {
        const uint64_t before = rtl_view_snr_test_compute_count();
        other_value = rtl_view_snr_c4fm_eye(&in);
        (void)rtl_view_snr_c4fm_eye(&in);
        other_computes = rtl_view_snr_test_compute_count() - before;
    });
    t.join();
    assert(other_computes == 1u);
    assert(same_bits(other_value, main_value));
    (void)check_c4fm(&in, 0u);
}

/* Writers append (and now and then clear) while two readers poll; once the writers stop, each reader's cached answer
 * equals a fresh estimate. Under TSan this also checks the batch bracketing for data races. */
void
test_concurrent_writers_and_readers(void) {
    const rtl_view_snr_inputs in = {10, 48000, DSD_CH_LPF_PROFILE_P25_C4FM, 0};
    rtl_view_eye_clear();
    rtl_view_constellation_clear();
    std::atomic<int> writers_done{0};
    std::thread writer([&]() {
        Lcg rng = {31337u};
        for (int b = 0; b < 300; b++) {
            append_eye(&rng, 48, in.ted_sps, 0.3f);
            append_qpsk(&rng, 480, 0.2f);
            if ((b % 97) == 96) {
                rtl_view_eye_clear();
            }
        }
        writers_done.fetch_add(1, std::memory_order_release);
    });
    std::thread clearer([&]() {
        for (int b = 0; b < 5; b++) {
            rtl_view_constellation_clear();
            std::this_thread::yield();
        }
        writers_done.fetch_add(1, std::memory_order_release);
    });
    /* Quiet rings: a cached answer (whatever raced while it was stored) must match a fresh estimate. */
    auto settle = [&]() {
        assert(same_bits(rtl_view_snr_c4fm_eye(&in), rtl_view_snr_c4fm_eye_uncached(&in)));
        assert(same_bits(rtl_view_snr_gfsk_eye(&in), rtl_view_snr_gfsk_eye_uncached(&in)));
        assert(same_bits(rtl_view_snr_qpsk_const(&in), rtl_view_snr_qpsk_const_uncached(&in)));
    };
    auto reader = [&]() {
        double sink = 0.0;
        while (writers_done.load(std::memory_order_acquire) < 2) {
            sink += rtl_view_snr_c4fm_eye(&in);
            sink += rtl_view_snr_gfsk_eye(&in);
            sink += rtl_view_snr_qpsk_const(&in);
        }
        (void)sink;
        settle();
    };
    std::thread r1(reader);
    std::thread r2(reader);
    writer.join();
    clearer.join();
    r1.join();
    r2.join();
    settle();
}

} // namespace

int
main(void) {
    test_eye_cache_tracks_inputs();
    test_qpsk_cache_tracks_inputs();
    test_raced_read_not_cached();
    test_threads_keep_own_cache();
    test_concurrent_writers_and_readers();
    std::printf("IO_RTL_VIEW_RINGS: OK\n");
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
