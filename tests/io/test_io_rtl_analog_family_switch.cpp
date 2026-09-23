// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A live receive-family switch must leave the demodulator exactly where a fresh
 * stream open of the new mode would: digital -> analog -> digital ends on the
 * fresh digital configuration, the analog leg matches a fresh -fA open, the
 * output ring is cleared and the stream generation bumps at each switch, and a
 * request made while the stream runs waits for the demod thread to consume it.
 * The session being left has its carrier and timing loops pulled well away from
 * their start values first, so each switch has to reset them the way an open does.
 * The fresh baselines run the same demodulator configuration functions as
 * dsd_rtl_stream_open() (see family_test_seed_open()).
 */

#include <cstdio>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/config.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "rtl_stream_test_support.h"

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_fields_equal(const char* label, const rtl_stream_test_demod_fields& got,
                    const rtl_stream_test_demod_fields& want) {
    int rc = 0;
    char name[160];
#define FIELD(f)                                                                                                       \
    do {                                                                                                               \
        DSD_SNPRINTF(name, sizeof name, "%s: " #f, label);                                                             \
        rc |= expect_int(name, got.f, want.f);                                                                         \
    } while (0)
    FIELD(output_kind);
    FIELD(cqpsk_enable);
    FIELD(demod_is_fm);
    FIELD(demod_is_qpsk);
    FIELD(deemph);
    FIELD(deemph_a_q15);
    FIELD(audio_lpf_enable);
    FIELD(channel_lpf_enable);
    FIELD(channel_lpf_profile);
    FIELD(channel_lpf_width_hz);
    FIELD(analog_family);
    FIELD(analog_demod);
    FIELD(symbol_rate_hz);
    FIELD(symbol_levels);
    FIELD(ted_enabled);
    FIELD(ted_sps);
    FIELD(resamp_enabled);
    FIELD(resamp_l);
    FIELD(resamp_m);
    FIELD(output_rate);
    FIELD(fsk_sample_rate_hz);
    FIELD(fsk_symbol_rate_hz);
    FIELD(fsk_levels);
    FIELD(fsk_channel_profile);
    FIELD(costas_freq_urad);
    FIELD(costas_phase_urad);
    FIELD(fll_freq_urad);
    FIELD(fll_phase_urad);
    FIELD(ted_awaiting_init);
#undef FIELD
    return rc;
}

/* The analog leg: the symbol clock and FSK modem are unused on the monitor path, so
 * only the fields that shape monitor audio are held to the fresh -fA open. */
static int
expect_analog_fields_equal(const char* label, const rtl_stream_test_demod_fields& got,
                           const rtl_stream_test_demod_fields& want) {
    int rc = 0;
    char name[160];
#define FIELD(f)                                                                                                       \
    do {                                                                                                               \
        DSD_SNPRINTF(name, sizeof name, "%s: " #f, label);                                                             \
        rc |= expect_int(name, got.f, want.f);                                                                         \
    } while (0)
    FIELD(output_kind);
    FIELD(cqpsk_enable);
    FIELD(demod_is_fm);
    FIELD(demod_is_qpsk);
    FIELD(deemph);
    FIELD(deemph_a_q15);
    FIELD(audio_lpf_enable);
    FIELD(channel_lpf_enable);
    FIELD(channel_lpf_profile);
    FIELD(channel_lpf_width_hz);
    FIELD(analog_family);
    FIELD(analog_demod);
    FIELD(ted_enabled);
    FIELD(resamp_enabled);
    FIELD(resamp_l);
    FIELD(resamp_m);
    FIELD(output_rate);
    FIELD(costas_freq_urad);
    FIELD(costas_phase_urad);
    FIELD(fll_freq_urad);
    FIELD(fll_phase_urad);
    FIELD(ted_awaiting_init);
#undef FIELD
    return rc;
}

namespace {

struct family_case {
    const char* name;
    void (*configure)(dsd_opts*);
    rtl_stream_test_digital_request request;
};

} // namespace

static void
p25_c4fm(dsd_opts* o) {
    o->frame_p25p1 = 1;
    o->mod_c4fm = 1;
}

static void
p25_cqpsk(dsd_opts* o) {
    o->frame_p25p1 = 1;
    o->mod_qpsk = 1;
}

static void
dmr(dsd_opts* o) {
    o->frame_dmr = 1;
    o->mod_c4fm = 1;
}

static void
nxdn48(dsd_opts* o) {
    o->frame_nxdn48 = 1;
    o->mod_c4fm = 1;
}

static void
dpmr(dsd_opts* o) {
    o->frame_dpmr = 1;
    o->mod_c4fm = 1;
}

static int
run_case(const family_case& c, int rate_hz, int nfm_width_hz) {
    static dsd_opts digital;
    static dsd_opts analog;
    DSD_MEMSET(&digital, 0, sizeof digital);
    DSD_MEMSET(&analog, 0, sizeof analog);
    c.configure(&digital);
    analog.analog_only = 1;
    analog.monitor_input_audio = 1;
    analog.analog_demod = DSD_ANALOG_DEMOD_FM;
    analog.analog_nfm_bandwidth_hz = nfm_width_hz;

    rtl_stream_test_family_switch_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    int rc = expect_int(c.name, rtl_stream_test_analog_family_switch(&digital, &analog, rate_hz, &c.request, &r), 0);
    char label[128];

    DSD_SNPRINTF(label, sizeof label, "%s@%d -> analog", c.name, rate_hz);
    rc |= expect_int(label, r.analog_request_rc, 0);
    rc |= expect_int("request waits for the demod thread", r.analog_deferred_until_consume, 1);
    rc |= expect_analog_fields_equal(label, r.switched_analog, r.fresh_analog);
    rc |= expect_int("analog switch bumps the generation", r.generation_after_analog != r.generation_before, 1);
    rc |= expect_int("analog switch clears the ring", (int)r.used_after_analog, 0);
    rc |= expect_int("seeded ring was not empty", r.used_before > 0U, 1);
    rc |= expect_int("analog profile published", r.published_analog_rc, 1);
    rc |= expect_int("published kind", r.published_kind, DSD_ANALOG_DEMOD_FM);

    DSD_SNPRINTF(label, sizeof label, "%s@%d -> digital", c.name, rate_hz);
    rc |= expect_int(label, r.digital_request_rc, 0);
    rc |= expect_fields_equal(label, r.switched_digital, r.fresh_digital);
    rc |= expect_int("digital switch bumps the generation", r.generation_after_digital != r.generation_after_analog, 1);
    rc |= expect_int("digital switch clears the ring", (int)r.used_after_digital, 0);
    rc |= expect_int("analog profile withdrawn", r.published_after_digital_rc, 0);
    /* The decoder sets symbol timing before a deferred switch lands, from this prediction. */
    rc |=
        expect_int("predicted analog output rate", (int)r.predicted_analog_output_rate, r.switched_analog.output_rate);
    rc |= expect_int("predicted digital output rate", (int)r.predicted_digital_output_rate,
                     r.switched_digital.output_rate);
    return rc;
}

int
main(void) {
    dsd_neo_config_init();
    const family_case cases[] = {
        {"P25 C4FM", p25_c4fm, {0, 4800, 4, RTL_STREAM_CHANNEL_PROFILE_P25_C4FM, 10}},
        {"P25 CQPSK", p25_cqpsk, {1, 4800, 4, RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK, 10}},
        {"DMR", dmr, {0, 4800, 4, RTL_STREAM_CHANNEL_PROFILE_12K5, 10}},
        {"NXDN48", nxdn48, {0, 2400, 4, RTL_STREAM_CHANNEL_PROFILE_6K25, 20}},
        {"dPMR", dpmr, {0, 2400, 4, RTL_STREAM_CHANNEL_PROFILE_6K25, 20}},
    };
    int rc = 0;
    for (const family_case& c : cases) {
        rc |= run_case(c, 48000, 0);
    }
    /* 24 kHz: the analog leg resamples its audio to 48 kHz while the digital
     * discriminator stream does not, so the output rate has to follow the family. */
    family_case dmr24 = cases[2];
    dmr24.request.ted_sps = 5;
    rc |= run_case(dmr24, 24000, 0);
    /* An explicit width travels with the analog request. */
    rc |= run_case(cases[0], 48000, 12500);

    rtl_stream_test_family_switch_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    static dsd_opts digital;
    static dsd_opts analog;
    DSD_MEMSET(&digital, 0, sizeof digital);
    DSD_MEMSET(&analog, 0, sizeof analog);
    dmr(&digital);
    analog.analog_only = 1;
    analog.monitor_input_audio = 1;
    analog.analog_nfm_bandwidth_hz = 12500;
    rc |= expect_int("explicit width run",
                     rtl_stream_test_analog_family_switch(&digital, &analog, 48000, &cases[2].request, &r), 0);
    rc |= expect_int("explicit width reaches the filter", r.switched_analog.channel_lpf_width_hz, 12500);
    rc |= expect_int("explicit width published", r.published_width_hz, 12500);
    rc |= expect_int("explicit width published as filtered", r.published_lpf_on, 1);

    /* Requests the front end cannot honour are refused up front. */
    rc |= expect_int("AM refused", rtl_stream_request_analog_profile(DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_AM, 0), -1);
    rc |= expect_int("bad family refused", rtl_stream_request_analog_profile(7, DSD_ANALOG_DEMOD_FM, 0), -1);
    rc |= expect_int("out-of-range width refused",
                     rtl_stream_request_analog_profile(DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_FM, 30000), -1);
    if (rc == 0) {
        std::printf("IO_RTL_ANALOG_FAMILY_SWITCH: OK\n");
    }
    return rc;
}
