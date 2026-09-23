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
 * their start values, and its monitor audio (de-emphasis, DC, audio LPF, squelch
 * envelope) and channel, half-band and resampler delay lines filled with stale
 * values first, so each switch has to reset them the way an open does. The fresh
 * baselines run the same demodulator configuration functions as
 * dsd_rtl_stream_open() (see family_test_seed_open()), including at a demod rate
 * the device forces, where the digital resampler follows the symbol profile.
 *
 * Leaving analog lands on the symbol profile queued after the family request,
 * even when the demod thread reaches a block boundary between the two requests.
 *
 * A width-only change on a running analog stream stays inside the family: it
 * drops the channel plan and the channel/half-band histories and nothing else.
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
    FIELD(deemph_avg_u);
    FIELD(dc_avg_u);
    FIELD(audio_lpf_state_u);
    FIELD(squelch_env_u);
    FIELD(squelch_gate_open);
    FIELD(channel_hist_clear);
    FIELD(hb_hist_clear);
    FIELD(resamp_hist_clear);
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
    FIELD(deemph_avg_u);
    FIELD(dc_avg_u);
    FIELD(audio_lpf_state_u);
    FIELD(squelch_env_u);
    FIELD(squelch_gate_open);
    FIELD(channel_hist_clear);
    FIELD(hb_hist_clear);
    FIELD(resamp_hist_clear);
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
run_case(const family_case& c, int rate_hz, int forced_rate_out_hz, int nfm_width_hz) {
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
    int rc = expect_int(
        c.name, rtl_stream_test_analog_family_switch(&digital, &analog, rate_hz, forced_rate_out_hz, &c.request, &r),
        0);
    const int demod_rate_hz = forced_rate_out_hz > 0 ? forced_rate_out_hz : rate_hz;
    char label[128];

    DSD_SNPRINTF(label, sizeof label, "%s@%d -> analog", c.name, demod_rate_hz);
    rc |= expect_int(label, r.analog_request_rc, 0);
    rc |= expect_int("request waits for the demod thread", r.analog_deferred_until_consume, 1);
    rc |= expect_analog_fields_equal(label, r.switched_analog, r.fresh_analog);
    rc |= expect_int("analog switch bumps the generation", r.generation_after_analog != r.generation_before, 1);
    rc |= expect_int("analog switch clears the ring", (int)r.used_after_analog, 0);
    rc |= expect_int("seeded ring was not empty", r.used_before > 0U, 1);
    rc |= expect_int("analog profile published", r.published_analog_rc, 1);
    rc |= expect_int("published kind", r.published_kind, DSD_ANALOG_DEMOD_FM);

    DSD_SNPRINTF(label, sizeof label, "%s@%d -> digital", c.name, demod_rate_hz);
    rc |= expect_int(label, r.digital_request_rc, 0);
    rc |= expect_fields_equal(label, r.switched_digital, r.fresh_digital);
    rc |= expect_int("digital switch bumps the generation", r.generation_after_digital != r.generation_after_analog, 1);
    rc |= expect_int("digital switch clears the ring", (int)r.used_after_digital, 0);
    rc |= expect_int("analog profile withdrawn", r.published_after_digital_rc, 0);
    if (c.request.boundary_between_requests) {
        rc |= expect_int("digital family waits for its symbol profile", r.digital_held_until_profile, 1);
    }
    /* The decoder sets symbol timing before a deferred switch lands, from this prediction. */
    rc |=
        expect_int("predicted analog output rate", (int)r.predicted_analog_output_rate, r.switched_analog.output_rate);
    rc |= expect_int("predicted digital output rate", (int)r.predicted_digital_output_rate,
                     r.switched_digital.output_rate);
    return rc;
}

/* A width-only request on a running analog stream: the new width reaches the filter at the next block boundary, the
 * old plan and the channel/half-band delay lines are dropped, and nothing a family switch does happens (the ring and
 * the output generation are untouched). Asking again for the width already running changes nothing. */
static int
test_width_only_change(void) {
    rtl_stream_test_width_change_result w;
    DSD_MEMSET(&w, 0, sizeof w);
    int rc = expect_int("width change run", rtl_stream_test_analog_width_change(48000, 16000, 12500, &w), 0);
    rc |= expect_int("width request accepted", w.request_rc, 0);
    rc |= expect_int("width request waits for the demod thread", w.deferred_until_consume, 1);
    rc |= expect_int("new width reaches the filter", w.width_after, 12500);
    rc |= expect_int("explicit width keeps the filter on", w.lpf_enable_after, 1);
    rc |= expect_int("still the monitor output", w.output_kind_after, RTL_STREAM_OUTPUT_AUDIO_MONITOR);
    rc |= expect_int("still the analog family", w.analog_family_after, 1);
    rc |= expect_int("old plan dropped", w.plan_invalidated, 1);
    rc |= expect_int("channel history cleared", w.channel_hist_cleared, 1);
    rc |= expect_int("half-band history cleared", w.hb_hist_cleared, 1);
    rc |= expect_int("new width published", w.published_width_hz, 12500);
    rc |= expect_int("new width published as filtered", w.published_lpf_on, 1);
    rc |= expect_int("width change keeps the generation", w.generation_after == w.generation_before, 1);
    rc |= expect_int("width change keeps the ring", (int)w.used_after, (int)w.used_before);
    rc |= expect_int("seeded ring was not empty", w.used_before > 0U, 1);
    rc |= expect_int("same width accepted", w.same_width_rc, 0);
    rc |= expect_int("same width keeps the histories", w.same_width_kept_histories, 1);
    rc |= expect_int("same width keeps the plan", w.same_width_kept_plan, 1);

    /* From the unset default (legacy enable rule) to an explicit width at 24 kHz. */
    DSD_MEMSET(&w, 0, sizeof w);
    rc |= expect_int("default to explicit run", rtl_stream_test_analog_width_change(24000, 0, 8000, &w), 0);
    rc |= expect_int("default to explicit width", w.width_after, 8000);
    rc |= expect_int("default to explicit plan dropped", w.plan_invalidated, 1);
    rc |= expect_int("default to explicit histories cleared", w.channel_hist_cleared && w.hb_hist_cleared, 1);
    rc |= expect_int("default to explicit published", w.published_width_hz, 8000);
    return rc;
}

/* With no stream there is no published rate to check a width against: the next open validates it against the rate it
 * actually delivers, so a width a previous session's rate could not fit is not refused, while the kind and range
 * rules still apply. */
static int
test_requests_without_stream(void) {
    int retune_rc = 99;
    int rc =
        expect_int("no-stream width ignores a stale 12 kHz rate",
                   rtl_stream_test_analog_request_without_stream(12000, DSD_ANALOG_DEMOD_FM, 16000, &retune_rc), 0);
    rc |= expect_int("no-stream retune width ignores a stale 12 kHz rate", retune_rc, 0);
    retune_rc = 99;
    rc |= expect_int("no-stream out-of-range width refused",
                     rtl_stream_test_analog_request_without_stream(48000, DSD_ANALOG_DEMOD_FM, 30000, &retune_rc), -1);
    rc |= expect_int("no-stream out-of-range retune width refused", retune_rc, -1);
    retune_rc = 99;
    rc |= expect_int("no-stream AM refused",
                     rtl_stream_test_analog_request_without_stream(48000, DSD_ANALOG_DEMOD_AM, 0, &retune_rc), -1);
    rc |= expect_int("no-stream AM retune refused", retune_rc, -1);
    return rc;
}

int
main(void) {
    dsd_neo_config_init();
    const family_case cases[] = {
        {"P25 C4FM", p25_c4fm, {0, 4800, 4, RTL_STREAM_CHANNEL_PROFILE_P25_C4FM, 10, 0}},
        {"P25 CQPSK", p25_cqpsk, {1, 4800, 4, RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK, 10, 0}},
        {"DMR", dmr, {0, 4800, 4, RTL_STREAM_CHANNEL_PROFILE_12K5, 10, 0}},
        {"NXDN48", nxdn48, {0, 2400, 4, RTL_STREAM_CHANNEL_PROFILE_6K25, 20, 0}},
        {"dPMR", dpmr, {0, 2400, 4, RTL_STREAM_CHANNEL_PROFILE_6K25, 20, 0}},
    };
    int rc = 0;
    for (const family_case& c : cases) {
        rc |= run_case(c, 48000, 0, 0);
    }
    /* 24 kHz: the analog leg resamples its audio to 48 kHz while the digital
     * discriminator stream does not, so the output rate has to follow the family. */
    family_case dmr24 = cases[2];
    dmr24.request.ted_sps = 5;
    rc |= run_case(dmr24, 24000, 0, 0);
    /* An explicit width travels with the analog request. */
    rc |= run_case(cases[0], 48000, 0, 12500);

    /* Rates a device forces (Airspy 2.5 MS/s -> 78125 Hz; a 60 kHz grid). The digital
     * resampler decision depends on the symbol profile the switch lands on, so it has to be
     * made for that profile, not for the analog monitor's placeholder: CQPSK never resamples,
     * a 2400 sym/s profile divides 60 kHz evenly, and a 4800 sym/s one at 78125 Hz resamples
     * to 48 kHz with the same ratio the analog monitor already runs. */
    family_case cqpsk78 = cases[1];
    cqpsk78.request.ted_sps = 16;
    rc |= run_case(cqpsk78, 48000, 78125, 0);
    family_case c4fm78 = cases[0];
    rc |= run_case(c4fm78, 48000, 78125, 0);
    family_case nxdn60 = cases[3];
    nxdn60.request.ted_sps = 25;
    rc |= run_case(nxdn60, 48000, 60000, 0);
    family_case dpmr60 = cases[4];
    dpmr60.request.ted_sps = 25;
    rc |= run_case(dpmr60, 48000, 60000, 0);

    /* The family request and its symbol profile are two requests, and the demod thread can reach a block boundary
     * between them. The switch must still land on the profile: at 60 kHz a 4800 sym/s placeholder would resample a
     * 2400 sym/s stream to 48 kHz, and at 78125 Hz it would resample a CQPSK stream that never resamples. */
    family_case nxdn60_split = nxdn60;
    nxdn60_split.request.boundary_between_requests = 1;
    rc |= run_case(nxdn60_split, 48000, 60000, 0);
    family_case dpmr60_split = dpmr60;
    dpmr60_split.request.boundary_between_requests = 1;
    rc |= run_case(dpmr60_split, 48000, 60000, 0);
    family_case cqpsk78_split = cqpsk78;
    cqpsk78_split.request.boundary_between_requests = 1;
    rc |= run_case(cqpsk78_split, 48000, 78125, 0);
    family_case dmr48_split = cases[2];
    dmr48_split.request.boundary_between_requests = 1;
    rc |= run_case(dmr48_split, 48000, 0, 0);

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
                     rtl_stream_test_analog_family_switch(&digital, &analog, 48000, 0, &cases[2].request, &r), 0);
    rc |= expect_int("explicit width reaches the filter", r.switched_analog.channel_lpf_width_hz, 12500);
    rc |= expect_int("explicit width published", r.published_width_hz, 12500);
    rc |= expect_int("explicit width published as filtered", r.published_lpf_on, 1);

    rc |= test_width_only_change();
    rc |= test_requests_without_stream();

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
