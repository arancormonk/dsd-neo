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
 * their start values, its monitor audio (de-emphasis, DC, audio LPF, squelch
 * envelope) and channel, half-band and resampler delay lines filled with stale
 * values, and its squelch dwell one block short of a multi-frequency hop first,
 * so each switch has to reset them the way an open does. The fresh
 * baselines run the same demodulator configuration functions as
 * dsd_rtl_stream_open() (see family_test_seed_open()), including at a demod rate
 * the device forces, where the digital resampler and the integer-SPS flag follow
 * the symbol profile.
 *
 * Leaving analog lands on the symbol profile queued after the family request,
 * even when the demod thread reaches a block boundary between the two requests,
 * and even when an older CQPSK toggle was still queued before the family request.
 * A retune that settles the device on another demod rate after the decoder
 * timed and queued that profile, before the demod thread consumes it, still
 * lands on a fresh open at the new rate, its CQPSK timing included.
 *
 * A width-only change on a running analog stream stays inside the family: it
 * drops the channel plan and the channel/half-band histories and nothing else.
 *
 * While a stream runs, a width its published demod rate (or a replay's post-demod
 * decimation) cannot realize is refused before it is queued, as a live request
 * and as a retune profile, and the refusal is logged with the validator's text.
 * That holds for a width change on the running monitor and for a digital
 * session's switch onto it alike; the unset NFM default is never refused, and
 * lands on the legacy WIDE design where the rate cannot fit 16 kHz.
 *
 * The matrix covers P25 C4FM/CQPSK, DMR, NXDN48, dPMR and ProVoice, the last also at a 12 kHz DSP rate, where its
 * 9600 sym/s leaves under two samples per symbol and the switch has to land on the two an open clamps the timing to,
 * and D-STAR (4800 sym/s over two levels).
 *
 * A session that started with -fA and then switches to a digital mode lands on
 * a fresh open of that mode too. Throughout, the stream keeps the options
 * snapshot it opened with, as a real session's orchestrator copy does: the
 * family requests and the digital modes the decoder notes are all it learns of a
 * mode change, so after a switch its own record of the family, not that snapshot,
 * decides where a symbol profile without CQPSK lands (a CQPSK profile and back
 * returns to the FSK discriminator), and the noted modes pick the channel profile
 * the DSP menu's CQPSK toggle returns to, as on a fresh open of the mode. Every
 * switch also returns the I/Q DC and balance estimates and a replay's post-demod
 * decimator to their start.
 *
 * A typed digital scan row on an analog session (a -fA open, or a DMR open
 * switched to analog) keeps the monitor output with the row's channel profile and
 * the analog family flag still set; republishing the row's symbol profile changes
 * nothing, and the row's leave restores the analog monitor. A digital family
 * request still leaves the analog family from there, and from a CQPSK toggle
 * under -fA: a -fA session that had either applied before a digital mode is
 * picked lands on the same fresh open of that mode.
 *
 * A live request accepted at the published rate is held again to the rate a
 * retune landed the stream on before the demod thread consumed it, and refused
 * there, as a width change on the running monitor and as a digital session's
 * switch onto it: the front end keeps the receive profile it has, never a
 * monitor running an explicit width without its channel filter.
 *
 * AM (issue #524) switches live the same way: digital -> AM -> digital and an
 * AM start switched to digital each land on a fresh open, the AM detector's
 * carrier estimate included, and a live FM <-> AM switch on the running monitor
 * swaps the detector and de-emphasis and starts the monitor audio, the carrier
 * estimate, the resampler and the channel filter over as a fresh open of the new
 * kind would, dropping the old kind's queued audio. The live output scale (1/pi)
 * applies to FM monitor audio and not to AM's, which normalises its own level.
 * No symbol profile without CQPSK swaps the AM detector for the discriminator:
 * a CQPSK-off toggle, a failed tune's restore and a typed row's profile keep it.
 * The DSP menu's return from CQPSK to the FM or AM monitor is the analog request
 * alone, which turns CQPSK off as it enters the monitor, and which a refusal
 * where it lands leaves on CQPSK.
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/log.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "rtl_stream_test_support.h"

static char g_last_error[512];
static int g_error_count;

static void
capture_error_log(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)ctx;
    if (level == LOG_LEVEL_ERROR && text) {
        DSD_SNPRINTF(g_last_error, sizeof g_last_error, "%s", text);
        g_error_count++;
    }
}

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
    FIELD(demod_is_am);
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
    FIELD(sps_is_integer);
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
    FIELD(squelch_hits);
    FIELD(am_carrier_u);
    FIELD(am_squelched_samples);
    FIELD(channel_hist_clear);
    FIELD(hb_hist_clear);
    FIELD(resamp_hist_clear);
    FIELD(iq_correction_clear);
    FIELD(post_decim_clear);
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
    FIELD(demod_is_am);
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
    FIELD(squelch_hits);
    FIELD(am_carrier_u);
    FIELD(am_squelched_samples);
    FIELD(channel_hist_clear);
    FIELD(hb_hist_clear);
    FIELD(resamp_hist_clear);
    FIELD(iq_correction_clear);
    FIELD(post_decim_clear);
#undef FIELD
    return rc;
}

/* The DSP menu's CQPSK toggle, made twice after the switch, lands where it lands on a fresh open of the mode: turning
 * CQPSK off returns to the FSK channel profile an open picks from the decode modes it runs, not from the options the
 * stream opened with (the -fA monitor's name no digital mode). */
static int
expect_menu_toggles_like_open(const char* label, const rtl_stream_test_family_switch_result& r) {
    int rc = 0;
    char name[192];
    for (int i = 0; i < 2; i++) {
        DSD_SNPRINTF(name, sizeof name, "%s: menu CQPSK toggle %d channel profile", label, i + 1);
        rc |= expect_int(name, r.switched_toggle_channel_profile[i], r.fresh_toggle_channel_profile[i]);
        DSD_SNPRINTF(name, sizeof name, "%s: menu CQPSK toggle %d output kind", label, i + 1);
        rc |= expect_int(name, r.switched_toggle_output_kind[i], r.fresh_toggle_output_kind[i]);
        DSD_SNPRINTF(name, sizeof name, "%s: menu CQPSK toggle %d symbol levels", label, i + 1);
        rc |= expect_int(name, r.switched_toggle_levels[i], r.fresh_toggle_levels[i]);
    }
    /* One of the two toggles turned CQPSK off (a C4FM mode toggles on, then off; a CQPSK mode off first). */
    DSD_SNPRINTF(name, sizeof name, "%s: a menu CQPSK toggle returned to the FSK discriminator", label);
    rc |= expect_int(name,
                     r.switched_toggle_output_kind[0] == RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR
                         || r.switched_toggle_output_kind[1] == RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR,
                     1);
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

static void
provoice(dsd_opts* o) {
    o->frame_provoice = 1;
    o->mod_gfsk = 1;
}

static void
dstar(dsd_opts* o) {
    o->frame_dstar = 1;
    o->mod_gfsk = 1;
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
    rc |= expect_menu_toggles_like_open(label, r);
    rc |= expect_int("CQPSK and back returns to the FSK discriminator", r.output_kind_after_cqpsk_round_trip,
                     RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR);
    return rc;
}

/* A session started with -fA, where the operator then picks a digital mode: the digital state it lands on must equal
 * a fresh open of that mode, with nothing the -fA open chose carried over. */
static int
run_analog_start_case(const family_case& c, int rate_hz, int forced_rate_out_hz) {
    static dsd_opts digital;
    static dsd_opts analog;
    DSD_MEMSET(&digital, 0, sizeof digital);
    DSD_MEMSET(&analog, 0, sizeof analog);
    c.configure(&digital);
    analog.analog_only = 1;
    analog.monitor_input_audio = 1;
    analog.analog_demod = DSD_ANALOG_DEMOD_FM;

    rtl_stream_test_family_switch_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    const int demod_rate_hz = forced_rate_out_hz > 0 ? forced_rate_out_hz : rate_hz;
    char label[128];
    DSD_SNPRINTF(label, sizeof label, "-fA start, %s@%d run", c.name, demod_rate_hz);
    int rc = expect_int(
        label,
        rtl_stream_test_analog_start_family_switch(&digital, &analog, rate_hz, forced_rate_out_hz, &c.request, &r), 0);
    rc |= expect_int("-fA start runs the monitor", r.fresh_analog.output_kind, RTL_STREAM_OUTPUT_AUDIO_MONITOR);
    rc |= expect_int("-fA start publishes the analog profile", r.published_analog_rc, 1);

    DSD_SNPRINTF(label, sizeof label, "-fA start, %s@%d -> digital", c.name, demod_rate_hz);
    rc |= expect_int(label, r.digital_request_rc, 0);
    rc |= expect_fields_equal(label, r.switched_digital, r.fresh_digital);
    rc |= expect_int("-fA start: digital switch bumps the generation",
                     r.generation_after_digital != r.generation_after_analog, 1);
    rc |= expect_int("-fA start: digital switch clears the ring", (int)r.used_after_digital, 0);
    rc |= expect_int("-fA start: seeded ring was not empty", r.used_before > 0U, 1);
    rc |= expect_int("-fA start: analog profile withdrawn", r.published_after_digital_rc, 0);
    if (c.request.boundary_between_requests) {
        rc |= expect_int("-fA start: digital family waits for its symbol profile", r.digital_held_until_profile, 1);
    }
    rc |= expect_int("-fA start: predicted digital output rate", (int)r.predicted_digital_output_rate,
                     r.switched_digital.output_rate);
    /* The -fA session's options snapshot names no digital mode: after the switch, the decode modes the decoder noted
     * pick the FSK channel profile a toggle-off returns to, and the stream's own record of the family has to keep a
     * symbol profile without CQPSK on the FSK discriminator. */
    rc |= expect_menu_toggles_like_open(label, r);
    rc |= expect_int("-fA start: CQPSK and back returns to the FSK discriminator", r.output_kind_after_cqpsk_round_trip,
                     RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR);
    rc |= expect_int("-fA start: the digital family is not the analog family", r.family_active_after_digital, 0);
    return rc;
}

/* Digital -> AM -> digital on a running stream, then an AM start switched to digital and back to AM on that same
 * stream (issue #524). Each AM leg is a fresh AM open: the envelope detector, no de-emphasis, the channel filter on at
 * the AM width and a cold carrier estimate, whatever the digital session and the stale monitor state before it left. */
static int
run_am_case(const family_case& c, int rate_hz, int am_width_hz) {
    static dsd_opts digital;
    static dsd_opts am;
    DSD_MEMSET(&digital, 0, sizeof digital);
    DSD_MEMSET(&am, 0, sizeof am);
    c.configure(&digital);
    am.analog_only = 1;
    am.monitor_input_audio = 1;
    am.analog_demod = DSD_ANALOG_DEMOD_AM;
    am.analog_am_bandwidth_hz = am_width_hz;
    const int want_width = am_width_hz > 0 ? am_width_hz : DSD_ANALOG_AM_WIDTH_DEFAULT_HZ;

    rtl_stream_test_family_switch_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    char label[128];
    DSD_SNPRINTF(label, sizeof label, "%s@%d -> AM", c.name, rate_hz);
    int rc = expect_int(label, rtl_stream_test_analog_family_switch(&digital, &am, rate_hz, 0, &c.request, &r), 0);
    rc |= expect_int(label, r.analog_request_rc, 0);
    rc |= expect_int("a fresh AM open runs the AM detector", r.fresh_analog.demod_is_am, 1);
    rc |= expect_int("a fresh AM open has no de-emphasis", r.fresh_analog.deemph, 0);
    rc |= expect_int("a fresh AM open filters at the AM width", r.fresh_analog.channel_lpf_width_hz, want_width);
    rc |= expect_analog_fields_equal(label, r.switched_analog, r.fresh_analog);
    rc |= expect_int("AM switch clears the ring", (int)r.used_after_analog, 0);
    rc |= expect_int("AM profile published", r.published_analog_rc, 1);
    rc |= expect_int("published AM kind", r.published_kind, DSD_ANALOG_DEMOD_AM);
    rc |= expect_int("published AM width", r.published_width_hz, want_width);
    rc |= expect_int("published AM width is filtered", r.published_lpf_on, 1);
    DSD_SNPRINTF(label, sizeof label, "%s@%d AM -> digital", c.name, rate_hz);
    rc |= expect_int(label, r.digital_request_rc, 0);
    rc |= expect_fields_equal(label, r.switched_digital, r.fresh_digital);

    DSD_MEMSET(&r, 0, sizeof r);
    DSD_SNPRINTF(label, sizeof label, "AM start, %s@%d -> digital", c.name, rate_hz);
    rc |= expect_int(label, rtl_stream_test_analog_start_family_switch(&digital, &am, rate_hz, 0, &c.request, &r), 0);
    rc |= expect_int("AM start runs the AM detector", r.fresh_analog.demod_is_am, 1);
    rc |= expect_int("AM start publishes the AM kind", r.published_kind, DSD_ANALOG_DEMOD_AM);
    rc |= expect_int(label, r.digital_request_rc, 0);
    rc |= expect_fields_equal(label, r.switched_digital, r.fresh_digital);
    rc |= expect_int("AM start: analog profile withdrawn", r.published_after_digital_rc, 0);

    /* ... and AM again on the same running stream: a fresh AM open once more, the detector's carrier estimate and
     * closed-squelch run cold, whatever the AM leg before the digital one and the stale state left. */
    DSD_SNPRINTF(label, sizeof label, "AM start, %s@%d -> digital -> AM", c.name, rate_hz);
    rc |= expect_int(label, r.reentered_analog_request_rc, 0);
    rc |= expect_analog_fields_equal(label, r.reentered_analog, r.fresh_analog);
    rc |= expect_int("AM again runs the AM detector", r.reentered_analog.demod_is_am, 1);
    rc |= expect_int("AM again starts the carrier estimate cold", r.reentered_analog.am_carrier_u, 0);
    rc |= expect_int("AM again publishes the analog profile", r.reentered_published_rc, 1);
    rc |= expect_int("AM again publishes the AM kind", r.reentered_published_kind, DSD_ANALOG_DEMOD_AM);
    rc |= expect_int("AM again publishes the AM width", r.reentered_published_width_hz, want_width);
    return rc;
}

/* A live FM <-> AM switch on the running monitor lands where a fresh open of the new kind does for everything that
 * shapes its audio: the detector, de-emphasis and its coefficient, the audio filters, the channel filter width and a
 * fresh channel plan, and the de-emphasis, DC, audio-LPF, squelch-envelope, resampler and AM carrier state. The old
 * kind's audio queued in the output ring is dropped and the generation moves, so the new kind's first audio does not
 * follow it. Only the new kind's request moves it: until the demod thread consumes it the running kind stays, and so
 * does its queued audio. */
static int
expect_kind_switch(const char* label, const dsd_opts* from, const dsd_opts* to, int rate_hz) {
    rtl_stream_test_kind_switch_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    char name[160];
    int rc = expect_int(label, rtl_stream_test_analog_kind_switch(from, to, rate_hz, &r), 0);
    DSD_SNPRINTF(name, sizeof name, "%s: request accepted", label);
    rc |= expect_int(name, r.request_rc, 0);
    DSD_SNPRINTF(name, sizeof name, "%s: request waits for the demod thread", label);
    rc |= expect_int(name, r.deferred_until_consume, 1);
#define KIND_FIELD(f)                                                                                                  \
    do {                                                                                                               \
        DSD_SNPRINTF(name, sizeof name, "%s: " #f, label);                                                             \
        rc |= expect_int(name, r.switched.f, r.fresh.f);                                                               \
    } while (0)
    KIND_FIELD(output_kind);
    KIND_FIELD(analog_family);
    KIND_FIELD(analog_demod);
    KIND_FIELD(demod_is_fm);
    KIND_FIELD(demod_is_am);
    KIND_FIELD(deemph);
    KIND_FIELD(deemph_a_q15);
    KIND_FIELD(audio_lpf_enable);
    KIND_FIELD(channel_lpf_enable);
    KIND_FIELD(channel_lpf_profile);
    KIND_FIELD(channel_lpf_width_hz);
    KIND_FIELD(deemph_avg_u);
    KIND_FIELD(dc_avg_u);
    KIND_FIELD(audio_lpf_state_u);
    KIND_FIELD(squelch_env_u);
    KIND_FIELD(squelch_gate_open);
    KIND_FIELD(am_carrier_u);
    KIND_FIELD(am_squelched_samples);
    KIND_FIELD(channel_hist_clear);
    KIND_FIELD(hb_hist_clear);
    KIND_FIELD(resamp_enabled);
    KIND_FIELD(resamp_hist_clear);
#undef KIND_FIELD
    DSD_SNPRINTF(name, sizeof name, "%s: published kind", label);
    rc |= expect_int(name, r.published_kind, to->analog_demod);
    DSD_SNPRINTF(name, sizeof name, "%s: published width", label);
    rc |= expect_int(name, r.published_width_hz, r.switched.channel_lpf_width_hz);
    DSD_SNPRINTF(name, sizeof name, "%s: old audio queued before the switch", label);
    rc |= expect_int(name, r.used_before > 0U && r.used_while_pending == r.used_before, 1);
    DSD_SNPRINTF(name, sizeof name, "%s: the switch clears the ring", label);
    rc |= expect_int(name, (int)r.used_after, 0);
    DSD_SNPRINTF(name, sizeof name, "%s: the switch bumps the generation", label);
    rc |= expect_int(name, r.generation_after != r.generation_before, 1);
    return rc;
}

/* A request for the kind already running, at another width, is a width-only change: its audio is the same kind's, so
 * the queued audio and the generation stay. */
static int
expect_same_kind_keeps_audio(const char* label, const dsd_opts* from, const dsd_opts* to, int rate_hz) {
    rtl_stream_test_kind_switch_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    char name[160];
    int rc = expect_int(label, rtl_stream_test_analog_kind_switch(from, to, rate_hz, &r), 0);
    DSD_SNPRINTF(name, sizeof name, "%s: request accepted", label);
    rc |= expect_int(name, r.request_rc, 0);
    DSD_SNPRINTF(name, sizeof name, "%s: new width applied", label);
    rc |= expect_int(name, r.switched.channel_lpf_width_hz, r.fresh.channel_lpf_width_hz);
    DSD_SNPRINTF(name, sizeof name, "%s: ring kept", label);
    rc |= expect_int(name, r.used_before > 0U && r.used_after == r.used_before, 1);
    DSD_SNPRINTF(name, sizeof name, "%s: generation kept", label);
    rc |= expect_int(name, r.generation_after == r.generation_before, 1);
    return rc;
}

static int
test_fm_am_kind_switch(void) {
    static dsd_opts fm;
    static dsd_opts am;
    DSD_MEMSET(&fm, 0, sizeof fm);
    fm.analog_only = 1;
    fm.monitor_input_audio = 1;
    fm.analog_demod = DSD_ANALOG_DEMOD_FM;
    am = fm;
    am.analog_demod = DSD_ANALOG_DEMOD_AM;
    int rc = expect_kind_switch("FM -> AM @48k", &fm, &am, 48000);
    rc |= expect_kind_switch("AM -> FM @48k", &am, &fm, 48000);
    /* An explicit width on each side; 24 kHz resamples the monitor to 48 kHz. */
    fm.analog_nfm_bandwidth_hz = 12500;
    am.analog_am_bandwidth_hz = 10000;
    rc |= expect_kind_switch("FM 12.5k -> AM 10k @24k", &fm, &am, 24000);
    rc |= expect_kind_switch("AM 10k -> FM 12.5k @24k", &am, &fm, 24000);
    static dsd_opts am_wide;
    am_wide = am;
    am_wide.analog_am_bandwidth_hz = 15000;
    rc |= expect_same_kind_keeps_audio("AM 10k -> AM 15k @48k", &am, &am_wide, 48000);
    return rc;
}

/* AM normalises its own level to the carrier, so the output scale a live stream runs (1/pi, which turns FM
 * discriminator radians into audio) is not applied to it, and AM monitor audio is the same live and in I/Q replay,
 * which runs no scale; FM monitor audio is scaled, and digital discriminator output is not. Checked through the output
 * block the demod thread writes, at 48 kHz and at 24 kHz, where the monitor is resampled to 48 kHz. */
static int
expect_output_gain(const char* label, const dsd_opts* opts, int rate_hz, int want_am, float want_gain) {
    rtl_stream_test_output_scale_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    char name[160];
    int rc = expect_int(label, rtl_stream_test_monitor_output_scale(opts, rate_hz, &r), 0);
    DSD_SNPRINTF(name, sizeof name, "%s: AM detector", label);
    rc |= expect_int(name, r.demod_is_am, want_am);
    DSD_SNPRINTF(name, sizeof name, "%s: the ring took the block", label);
    rc |= expect_int(name, r.unscaled_samples > 0 && r.scaled_samples == r.unscaled_samples, 1);
    DSD_SNPRINTF(name, sizeof name, "%s: resampled at 24 kHz", label);
    rc |= expect_int(name, r.resampled, rate_hz == 24000 && r.output_kind == RTL_STREAM_OUTPUT_AUDIO_MONITOR ? 1 : 0);
    if (std::fabs(r.gain - want_gain) > 1e-4f) {
        std::fprintf(stderr, "%s: live output scale gave gain %.6f, want %.6f\n", label, (double)r.gain,
                     (double)want_gain);
        rc = 1;
    }
    return rc;
}

static int
test_monitor_output_scale(void) {
    static dsd_opts fm;
    static dsd_opts am;
    static dsd_opts dmr;
    DSD_MEMSET(&fm, 0, sizeof fm);
    DSD_MEMSET(&dmr, 0, sizeof dmr);
    fm.analog_only = 1;
    fm.monitor_input_audio = 1;
    fm.analog_demod = DSD_ANALOG_DEMOD_FM;
    am = fm;
    am.analog_demod = DSD_ANALOG_DEMOD_AM;
    dmr.frame_dmr = 1;
    dmr.mod_c4fm = 1;
    rtl_stream_test_output_scale_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    int rc = expect_int("output scale: probe", rtl_stream_test_monitor_output_scale(&fm, 48000, &r), 0);
    const float live = r.live_scale;
    rc |= expect_int("output scale: the live scale is 1/pi", std::fabs(live - 0.318309886f) < 1e-6f ? 1 : 0, 1);
    rc |= expect_output_gain("AM @48k is exempt", &am, 48000, 1, 1.0f);
    rc |= expect_output_gain("AM @24k is exempt", &am, 24000, 1, 1.0f);
    rc |= expect_output_gain("FM @48k is scaled", &fm, 48000, 0, live);
    rc |= expect_output_gain("FM @24k is scaled", &fm, 24000, 0, live);
    rc |= expect_output_gain("DMR discriminator is not scaled", &dmr, 48000, 0, 1.0f);
    return rc;
}

/* A symbol profile without CQPSK never swaps the AM monitor's detector for the FM discriminator: only a family switch
 * or an FM <-> AM switch changes the detector. A CQPSK-off toggle on the monitor (CQPSK off already) keeps AM, and so
 * does the restore of the monitor's own profile a failed tune queues after a typed digital row, with a later AM width
 * request applied to the AM detector. A typed row's profile, which moves the channel off the monitor's, is read with
 * the discriminator for as long as it runs, the AM kind kept; the analog profile a row running the analog family
 * queues for its retune brings the AM monitor back on its own channel. */
static int
test_am_monitor_keeps_detector_under_symbol_profiles(void) {
    rtl_stream_test_am_symbol_profile_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    int rc = expect_int("AM symbol profiles run", rtl_stream_test_am_monitor_symbol_profiles(48000, &r), 0);
    rc |= expect_int("AM symbol profiles: open", r.open_rc, 0);
    rc |= expect_int("AM open runs the AM detector", r.am_on_open, 1);
    rc |= expect_int("CQPSK off on the AM monitor keeps the AM detector", r.am_after_cqpsk_off, 1);
    rc |= expect_int("typed row moves the channel off the monitor", r.row_monitor, 0);
    rc |= expect_int("typed row is read with the discriminator", r.row_am, 0);
    rc |= expect_int("typed row keeps the AM kind", r.row_kind, DSD_ANALOG_DEMOD_AM);
    rc |= expect_int("restored monitor profile runs the AM detector", r.am_after_restore, 1);
    rc |= expect_int("AM width request keeps the AM detector", r.am_after_width_request, 1);
    rc |= expect_int("AM width request reaches the filter", r.width_after_request, 10000);
    rc |= expect_int("retuned typed row is read with the discriminator", r.retune_row_am, 0);
    rc |= expect_int("analog row retune runs the AM detector", r.retune_analog_am, 1);
    rc |= expect_int("analog row retune runs the monitor", r.retune_analog_monitor, 1);
    rc |= expect_int("analog row retune filters at the AM default", r.retune_analog_width,
                     DSD_ANALOG_AM_WIDTH_DEFAULT_HZ);
    return rc;
}

/* The DSP menu's CQPSK toggle back to the monitor under -fA or -fM (svc_toggle_rtl_cqpsk()) is the analog request
 * alone: taken, it enters the monitor of the kind asked for at its width with CQPSK off; refused where it lands (a
 * retune moved the rate below what the width needs), it leaves the front end on CQPSK, never on a CQPSK-off profile
 * with the FSK channel filter and the FM discriminator on the monitor output. */
static int
test_monitor_return_from_cqpsk_is_the_analog_request_alone(void) {
    int rc = 0;
    const int kinds[2] = {DSD_ANALOG_DEMOD_FM, DSD_ANALOG_DEMOD_AM};
    for (const int kind : kinds) {
        const char* name = kind == DSD_ANALOG_DEMOD_AM ? "AM" : "NFM";
        char label[128];
        rtl_stream_test_monitor_return_result r;
        DSD_MEMSET(&r, 0, sizeof r);
        DSD_SNPRINTF(label, sizeof label, "%s monitor return run", name);
        rc |= expect_int(label, rtl_stream_test_monitor_return_from_cqpsk(kind, 18000, 12000, &r), 0);
        DSD_SNPRINTF(label, sizeof label, "%s monitor return: open", name);
        rc |= expect_int(label, r.open_rc, 0);
        DSD_SNPRINTF(label, sizeof label, "%s monitor return: CQPSK on first", name);
        rc |= expect_int(label, r.cqpsk_on == 1 && r.cqpsk_output_kind == RTL_STREAM_OUTPUT_SYMBOL_CQPSK, 1);
        DSD_SNPRINTF(label, sizeof label, "%s monitor return: accepted at 48 kHz", name);
        rc |= expect_int(label, r.request_rc, 0);
        DSD_SNPRINTF(label, sizeof label, "%s monitor return: turns CQPSK off", name);
        rc |= expect_int(label, r.accepted_cqpsk == 0 && r.accepted_requested_cqpsk == 0, 1);
        DSD_SNPRINTF(label, sizeof label, "%s monitor return: runs the monitor", name);
        rc |= expect_int(label, r.accepted_monitor, 1);
        DSD_SNPRINTF(label, sizeof label, "%s monitor return: with the detector of its kind", name);
        rc |= expect_int(label, r.accepted_kind_active, 1);
        DSD_SNPRINTF(label, sizeof label, "%s monitor return: at the width asked for", name);
        rc |= expect_int(label, r.accepted_width_hz, 18000);
        DSD_SNPRINTF(label, sizeof label, "%s monitor return at 12 kHz: queued", name);
        rc |= expect_int(label, r.refused_request_rc, 0);
        DSD_SNPRINTF(label, sizeof label, "%s monitor return at 12 kHz: refused where it lands", name);
        rc |= expect_int(label, r.refused_outcome, RTL_STREAM_RX_REQUEST_REFUSED);
        DSD_SNPRINTF(label, sizeof label, "%s monitor return at 12 kHz: kept the analog family and kind", name);
        rc |= expect_int(label, r.refused_kept_analog == 1 && r.refused_kept_kind == kind, 1);
        DSD_SNPRINTF(label, sizeof label, "%s monitor return at 12 kHz: CQPSK stays on", name);
        rc |= expect_int(label,
                         r.refused_cqpsk == 1 && r.refused_output_kind == RTL_STREAM_OUTPUT_SYMBOL_CQPSK
                             && r.refused_requested_cqpsk == 1,
                         1);
        DSD_SNPRINTF(label, sizeof label, "%s monitor return at 12 kHz: keeps the CQPSK channel filter", name);
        rc |= expect_int(label, r.refused_channel_profile, RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    }
    return rc;
}

/* An AM width is held to the running stream's rate like an NFM one: 20 kHz does not fit a 16 kHz DSP rate, whose
 * largest width is 13.2 kHz, and is refused before anything is queued; the AM default fits and is applied, on the
 * running monitor and as a DMR session's switch onto it. */
static int
test_am_requests_against_running_stream(void) {
    rtl_stream_test_live_request_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    g_last_error[0] = '\0';
    int rc = expect_int("AM 20 kHz @16k run",
                        rtl_stream_test_analog_request_with_stream(16000, 1, 1, DSD_ANALOG_DEMOD_AM, 20000, &r), 0);
    rc |= expect_int("AM 20 kHz @16k check refused", r.check_rc, -1);
    rc |= expect_int("AM 20 kHz @16k request refused", r.request_rc, -1);
    rc |= expect_int("AM 20 kHz @16k nothing queued", r.request_queued, 0);
    rc |= expect_int("AM 20 kHz @16k refusal names the fix",
                     std::strstr(g_last_error, "AM bandwidth 20 kHz does not fit the 16 kHz DSP rate") != NULL, 1);
    for (int analog_stream = 0; analog_stream <= 1; analog_stream++) {
        DSD_MEMSET(&r, 0, sizeof r);
        rc |= expect_int(
            "AM default @16k run",
            rtl_stream_test_analog_request_with_stream(16000, analog_stream, 1, DSD_ANALOG_DEMOD_AM, 0, &r), 0);
        rc |= expect_int("AM default @16k check accepted", r.check_rc, 0);
        rc |= expect_int("AM default @16k request accepted", r.request_rc, 0);
        rc |= expect_int("AM default @16k lands on the analog family", r.family_after, 1);
        rc |= expect_int("AM default @16k filters at 6 kHz", r.width_after, DSD_ANALOG_AM_WIDTH_DEFAULT_HZ);
        rc |= expect_int("AM default @16k runs the monitor", r.monitor_after, 1);
    }
    return rc;
}

/* The decoder times the digital profile it queues for the rate the stream published when it picked the mode (the -fA
 * session's 48 kHz: ten samples per 4800 sym/s symbol). A retune can settle the device on another rate before the
 * demod thread reaches the block boundary that consumes the switch, here the 78125 Hz a fixed-grid device forces. The
 * switch still lands where an open of the mode at that rate would, with the timing that rate gives (sixteen samples per
 * symbol), for the CQPSK timing loop and the FSK discriminator alike. */
static int
run_analog_start_across_retune_case(const family_case& c, int landed_rate_out_hz) {
    static dsd_opts digital;
    static dsd_opts analog;
    DSD_MEMSET(&digital, 0, sizeof digital);
    DSD_MEMSET(&analog, 0, sizeof analog);
    c.configure(&digital);
    analog.analog_only = 1;
    analog.monitor_input_audio = 1;
    analog.analog_demod = DSD_ANALOG_DEMOD_FM;

    rtl_stream_test_family_switch_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    char label[160];
    DSD_SNPRINTF(label, sizeof label, "-fA start, %s queued at 48000, landed at %d run", c.name, landed_rate_out_hz);
    int rc = expect_int(label,
                        rtl_stream_test_analog_start_family_switch_across_retune(&digital, &analog, 48000,
                                                                                 landed_rate_out_hz, &c.request, &r),
                        0);
    DSD_SNPRINTF(label, sizeof label, "-fA start, %s queued at 48000, landed at %d -> digital", c.name,
                 landed_rate_out_hz);
    rc |= expect_int(label, r.digital_request_rc, 0);
    rc |= expect_fields_equal(label, r.switched_digital, r.fresh_digital);
    rc |= expect_int("across a retune: the digital family is not the analog family", r.family_active_after_digital, 0);
    return rc;
}

/* A -fA session that had moved its front end off the monitor output with a symbol profile applied on its own, before
 * the operator picks a digital mode: the analog family flag is still set, and the analog profile is no longer
 * published. The digital family request must leave the analog family from there all the same, onto a fresh open of
 * the mode picked, instead of letting that mode's symbol profile land on the -fA session (where a profile without
 * CQPSK puts the front end back on monitor audio). */
static int
run_under_analog_case(const family_case& c, int profile_under_analog, int rate_hz, int forced_rate_out_hz) {
    family_case under = c;
    under.request.profile_under_analog = profile_under_analog;
    static dsd_opts digital;
    static dsd_opts analog;
    DSD_MEMSET(&digital, 0, sizeof digital);
    DSD_MEMSET(&analog, 0, sizeof analog);
    c.configure(&digital);
    analog.analog_only = 1;
    analog.monitor_input_audio = 1;
    analog.analog_demod = DSD_ANALOG_DEMOD_FM;

    rtl_stream_test_family_switch_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    const int demod_rate_hz = forced_rate_out_hz > 0 ? forced_rate_out_hz : rate_hz;
    const char* under_name =
        profile_under_analog == RTL_STREAM_TEST_UNDER_ANALOG_CQPSK_TOGGLE ? "CQPSK toggle" : "typed DMR row";
    char label[160];
    DSD_SNPRINTF(label, sizeof label, "-fA %s, %s@%d run", under_name, c.name, demod_rate_hz);
    int rc = expect_int(
        label,
        rtl_stream_test_analog_start_family_switch(&digital, &analog, rate_hz, forced_rate_out_hz, &under.request, &r),
        0);
    DSD_SNPRINTF(label, sizeof label, "-fA %s: off the monitor output", under_name);
    if (profile_under_analog == RTL_STREAM_TEST_UNDER_ANALOG_CQPSK_TOGGLE) {
        rc |= expect_int(label, r.under_analog_output_kind, RTL_STREAM_OUTPUT_SYMBOL_CQPSK);
    } else {
        rc |= expect_int(label, r.under_analog_channel_profile, RTL_STREAM_CHANNEL_PROFILE_12K5);
    }
    DSD_SNPRINTF(label, sizeof label, "-fA %s: still the analog family", under_name);
    rc |= expect_int(label, r.under_analog_family, 1);
    DSD_SNPRINTF(label, sizeof label, "-fA %s: no analog profile published", under_name);
    rc |= expect_int(label, r.under_analog_published, 0);
    DSD_SNPRINTF(label, sizeof label, "-fA %s: the decoder still sees the analog family", under_name);
    rc |= expect_int(label, r.under_analog_family_active, 1);

    DSD_SNPRINTF(label, sizeof label, "-fA %s, %s@%d -> digital", under_name, c.name, demod_rate_hz);
    rc |= expect_int(label, r.digital_request_rc, 0);
    rc |= expect_fields_equal(label, r.switched_digital, r.fresh_digital);
    DSD_SNPRINTF(label, sizeof label, "-fA %s: digital switch bumps the generation", under_name);
    rc |= expect_int(label, r.generation_after_digital != r.generation_after_analog, 1);
    DSD_SNPRINTF(label, sizeof label, "-fA %s: digital switch clears the ring", under_name);
    rc |= expect_int(label, (int)r.used_after_digital, 0);
    DSD_SNPRINTF(label, sizeof label, "-fA %s: seeded ring was not empty", under_name);
    rc |= expect_int(label, r.used_before > 0U, 1);
    DSD_SNPRINTF(label, sizeof label, "-fA %s: analog family left", under_name);
    rc |= expect_int(label, r.family_active_after_digital, 0);
    if (under.request.boundary_between_requests) {
        DSD_SNPRINTF(label, sizeof label, "-fA %s: digital family waits for its symbol profile", under_name);
        rc |= expect_int(label, r.digital_held_until_profile, 1);
    }
    DSD_SNPRINTF(label, sizeof label, "-fA %s: predicted digital output rate", under_name);
    rc |= expect_int(label, (int)r.predicted_digital_output_rate, r.switched_digital.output_rate);
    DSD_SNPRINTF(label, sizeof label, "-fA %s, %s@%d", under_name, c.name, demod_rate_hz);
    rc |= expect_menu_toggles_like_open(label, r);
    DSD_SNPRINTF(label, sizeof label, "-fA %s: CQPSK and back returns to the FSK discriminator", under_name);
    rc |= expect_int(label, r.output_kind_after_cqpsk_round_trip, RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR);
    return rc;
}

/* The operator toggles CQPSK on a -fA session and picks a digital mode before the demod thread reaches a block
 * boundary, so the toggle is still queued when the family request arrives (both commands drain in one pass of the
 * decoder's command queue). The switch must land on the digital mode's own symbol profile, not on that older toggle,
 * even when a boundary falls between the family request and the profile: at a forced 78125 Hz, a DMR switch decided
 * for the toggle's CQPSK would leave the discriminator unresampled at 78125 Hz, where a fresh DMR open resamples it to
 * 48 kHz, and the DMR profile that follows does not revisit the resampler. */
static int
run_queued_toggle_case(const family_case& c, int rate_hz, int forced_rate_out_hz) {
    family_case queued = c;
    queued.request.profile_under_analog = RTL_STREAM_TEST_UNDER_ANALOG_CQPSK_TOGGLE_QUEUED;
    queued.request.boundary_between_requests = 1;
    static dsd_opts digital;
    static dsd_opts analog;
    DSD_MEMSET(&digital, 0, sizeof digital);
    DSD_MEMSET(&analog, 0, sizeof analog);
    c.configure(&digital);
    analog.analog_only = 1;
    analog.monitor_input_audio = 1;
    analog.analog_demod = DSD_ANALOG_DEMOD_FM;

    rtl_stream_test_family_switch_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    const int demod_rate_hz = forced_rate_out_hz > 0 ? forced_rate_out_hz : rate_hz;
    char label[160];
    DSD_SNPRINTF(label, sizeof label, "-fA queued CQPSK toggle, %s@%d run", c.name, demod_rate_hz);
    int rc = expect_int(
        label,
        rtl_stream_test_analog_start_family_switch(&digital, &analog, rate_hz, forced_rate_out_hz, &queued.request, &r),
        0);
    rc |= expect_int("-fA queued CQPSK toggle: not yet consumed", r.under_analog_output_kind,
                     RTL_STREAM_OUTPUT_AUDIO_MONITOR);

    DSD_SNPRINTF(label, sizeof label, "-fA queued CQPSK toggle, %s@%d -> digital", c.name, demod_rate_hz);
    rc |= expect_int(label, r.digital_request_rc, 0);
    rc |= expect_fields_equal(label, r.switched_digital, r.fresh_digital);
    rc |= expect_int("-fA queued CQPSK toggle: digital family waits for its symbol profile",
                     r.digital_held_until_profile, 1);
    rc |= expect_int("-fA queued CQPSK toggle: digital switch bumps the generation",
                     r.generation_after_digital != r.generation_after_analog, 1);
    rc |= expect_int("-fA queued CQPSK toggle: digital switch clears the ring", (int)r.used_after_digital, 0);
    rc |= expect_int("-fA queued CQPSK toggle: analog family left", r.family_active_after_digital, 0);
    rc |= expect_int("-fA queued CQPSK toggle: predicted digital output rate", (int)r.predicted_digital_output_rate,
                     r.switched_digital.output_rate);
    return rc;
}

/* A typed DMR scan row on an analog session queues only its symbol profile. The stream's options snapshot is the one
 * it opened with (a -fA open, or a DMR open the operator had switched to analog), so the row's profile keeps the
 * monitor output as it always did, but puts the row's channel profile in place of the analog channel: the analog
 * family flag stays set, the width-driven filter and the published analog profile stand down, and the row filters with
 * its own profile. A scoped command republishes only the row's symbol profile (the decoder's configured mode is still
 * analog), which neither clears the ring, bumps the generation nor moves the output rate in the middle of the row, and
 * the row's leave brings the analog monitor back. A digital family request there is a real family switch (the operator
 * picked a digital mode), so it waits for its symbol profile like any other. */
static int
expect_digital_row_on_analog_session(const char* name, int start_digital) {
    rtl_stream_test_digital_row_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    char label[160];
#define ROW_EXPECT(text, got, want)                                                                                    \
    do {                                                                                                               \
        DSD_SNPRINTF(label, sizeof label, "%s: %s", name, text);                                                       \
        rc |= expect_int(label, (got), (want));                                                                        \
    } while (0)
    int rc = 0;
    ROW_EXPECT("digital row run", rtl_stream_test_digital_row_on_analog_session(24000, start_digital, &r), 0);
    ROW_EXPECT("digital row open", r.open_rc, 0);
    ROW_EXPECT("row keeps the monitor output", r.row_output_kind, RTL_STREAM_OUTPUT_AUDIO_MONITOR);
    ROW_EXPECT("row keeps the analog family flag", r.row_analog_family, 1);
    ROW_EXPECT("row publishes no analog profile", r.row_published_family, 0);
    ROW_EXPECT("row keeps the monitor's 48 kHz output", r.row_output_rate, 48000);

    ROW_EXPECT("lone digital request accepted", r.lone_request_rc, 0);
    ROW_EXPECT("lone digital request waits for its symbol profile", r.lone_request_held, 1);

    ROW_EXPECT("row republish accepted", r.republish_rc, 0);
    ROW_EXPECT("row republish keeps the generation", r.generation_after == r.generation_before, 1);
    ROW_EXPECT("row republish keeps the ring", (int)r.used_after, (int)r.used_before);
    ROW_EXPECT("seeded ring was not empty", r.used_before > 0U, 1);
    ROW_EXPECT("row republish keeps the monitor output", r.output_kind_after, RTL_STREAM_OUTPUT_AUDIO_MONITOR);
    ROW_EXPECT("row republish keeps the output rate", r.output_rate_after, r.row_output_rate);
    ROW_EXPECT("row republish keeps the resampler",
               r.resamp_l_after == r.row_resamp_l && r.resamp_m_after == r.row_resamp_m, 1);

    ROW_EXPECT("row leave accepted", r.restore_rc, 0);
    ROW_EXPECT("row leave restores the monitor", r.restored_output_kind, RTL_STREAM_OUTPUT_AUDIO_MONITOR);
    ROW_EXPECT("row leave publishes the analog profile", r.restored_published_family, 1);
    ROW_EXPECT("row leave keeps the monitor rate", r.restored_output_rate, 48000);
    ROW_EXPECT("row leave bumps the generation", r.generation_after_restore != r.generation_after, 1);
#undef ROW_EXPECT
    return rc;
}

static int
test_digital_row_on_analog_session(void) {
    int rc = expect_digital_row_on_analog_session("-fA session", 0);
    rc |= expect_digital_row_on_analog_session("DMR session switched to analog", 1);
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
    rc |= expect_int("no-stream AM accepted",
                     rtl_stream_test_analog_request_without_stream(48000, DSD_ANALOG_DEMOD_AM, 0, &retune_rc), 0);
    rc |= expect_int("no-stream AM retune accepted", retune_rc, 0);
    retune_rc = 99;
    rc |= expect_int("no-stream out-of-range AM width refused",
                     rtl_stream_test_analog_request_without_stream(48000, DSD_ANALOG_DEMOD_AM, 25000, &retune_rc), -1);
    rc |= expect_int("no-stream out-of-range AM retune width refused", retune_rc, -1);
    /* A stale mirror is no rate to hold a request to, for the scanners' width checks either. */
    rc |= expect_int("no-stream request rate", rtl_stream_get_request_rate_hz(), 0);
    return rc;
}

namespace {

struct live_request_case {
    const char* name;
    int rate_hz;
    int analog_stream; /* 0: a DMR session asked to switch to analog */
    int post_downsample;
    int refused_width_hz;  /* in range for NFM, but the running stream cannot realize it */
    int accepted_width_hz; /* what the same stream does realize (0 = the unset default) */
    int accepted_filter_width_hz;
    const char* refusal_text; /* the validator's text the refusal logs */
};

} // namespace

static int
expect_live_refusal(const live_request_case& c) {
    rtl_stream_test_live_request_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    char label[160];
    g_last_error[0] = '\0';
    g_error_count = 0;
    DSD_SNPRINTF(label, sizeof label, "%s: refused %d Hz run", c.name, c.refused_width_hz);
    int rc = expect_int(label,
                        rtl_stream_test_analog_request_with_stream(c.rate_hz, c.analog_stream, c.post_downsample,
                                                                   DSD_ANALOG_DEMOD_FM, c.refused_width_hz, &r),
                        0);
    DSD_SNPRINTF(label, sizeof label, "%s: check refuses it", c.name);
    rc |= expect_int(label, r.check_rc, -1);
    DSD_SNPRINTF(label, sizeof label, "%s: live request refused", c.name);
    rc |= expect_int(label, r.request_rc, -1);
    /* The refusal is reported, not silent: the check logs the validator's text and what stays in place, and neither
     * the live request nor the retune profile for the same width and rate that follow it log it a second time. */
    DSD_SNPRINTF(label, sizeof label, "%s: one refusal logged for the width and rate", c.name);
    rc |= expect_int(label, g_error_count, 1);
    DSD_SNPRINTF(label, sizeof label, "%s: refusal logs the validator's text", c.name);
    rc |= expect_int(label, std::strstr(g_last_error, c.refusal_text) != NULL, 1);
    DSD_SNPRINTF(label, sizeof label, "%s: refusal says the front end keeps its profile", c.name);
    rc |= expect_int(label, std::strstr(g_last_error, "The front end keeps its current receive profile.") != NULL, 1);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "  logged: \"%s\"\n", g_last_error);
    }
    DSD_SNPRINTF(label, sizeof label, "%s: nothing queued", c.name);
    rc |= expect_int(label, r.request_queued, 0);
    DSD_SNPRINTF(label, sizeof label, "%s: family unchanged", c.name);
    rc |= expect_int(label, r.family_after, r.family_before);
    DSD_SNPRINTF(label, sizeof label, "%s: filter width unchanged", c.name);
    rc |= expect_int(label, r.width_after, r.width_before);
    DSD_SNPRINTF(label, sizeof label, "%s: channel plan kept", c.name);
    rc |= expect_int(label, r.plan_kept, 1);
    DSD_SNPRINTF(label, sizeof label, "%s: published width unchanged", c.name);
    rc |= expect_int(label, r.published_width_after, r.published_width_before);
    DSD_SNPRINTF(label, sizeof label, "%s: retune profile refused", c.name);
    rc |= expect_int(label, r.retune_rc, -1);
    DSD_SNPRINTF(label, sizeof label, "%s: no retune profile queued", c.name);
    rc |= expect_int(label, r.retune_queued, 0);
    return rc;
}

static int
expect_live_acceptance(const live_request_case& c) {
    rtl_stream_test_live_request_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    char label[160];
    DSD_SNPRINTF(label, sizeof label, "%s: accepted %d Hz run", c.name, c.accepted_width_hz);
    int rc = expect_int(label,
                        rtl_stream_test_analog_request_with_stream(c.rate_hz, c.analog_stream, c.post_downsample,
                                                                   DSD_ANALOG_DEMOD_FM, c.accepted_width_hz, &r),
                        0);
    DSD_SNPRINTF(label, sizeof label, "%s: request rate is the published DSP rate", c.name);
    rc |= expect_int(label, r.request_rate_hz, c.rate_hz);
    DSD_SNPRINTF(label, sizeof label, "%s: check accepts it", c.name);
    rc |= expect_int(label, r.check_rc, 0);
    DSD_SNPRINTF(label, sizeof label, "%s: realizable request accepted", c.name);
    rc |= expect_int(label, r.request_rc, 0);
    DSD_SNPRINTF(label, sizeof label, "%s: realizable request queued", c.name);
    rc |= expect_int(label, r.request_queued, 1);
    DSD_SNPRINTF(label, sizeof label, "%s: lands on the analog family", c.name);
    rc |= expect_int(label, r.family_after, 1);
    DSD_SNPRINTF(label, sizeof label, "%s: realizable width reaches the filter", c.name);
    rc |= expect_int(label, r.width_after, c.accepted_filter_width_hz);
    DSD_SNPRINTF(label, sizeof label, "%s: realizable retune profile accepted", c.name);
    rc |= expect_int(label, r.retune_rc, 0);
    DSD_SNPRINTF(label, sizeof label, "%s: realizable retune profile queued", c.name);
    rc |= expect_int(label, r.retune_queued, 1);
    return rc;
}

/* While a stream runs, each request is checked against the demod rate the stream publishes, and against the
 * post-demod decimation an I/Q replay sidecar can set: a width in NFM's range that the running stream cannot realize is
 * refused before anything is queued, by the check, as a live request and as a retune profile alike, and the running
 * receive profile stays as it was. Were such a width queued, the demodulator would have no filter design for it and
 * run the channel unfiltered. That holds for a width change on the running monitor and for a DMR session's switch onto
 * it: the switch is refused, and the session stays on the digital family. Beside each refusal a width the same stream
 * does realize is accepted, so the refusal is the rate's doing. */
static int
test_requests_against_running_stream(void) {
    const live_request_case cases[] = {
        /* A DMR session switching to the monitor at a 24 kHz DSP rate: refused the same way. */
        {"24 kHz DMR stream", 24000, 0, 1, 25000, 20000, 20000,
         "NFM bandwidth 25 kHz does not fit the 24 kHz DSP rate (the largest width it fits is 20.4 kHz)"},
        /* 24 kHz fits up to 20.4 kHz: 25 kHz is in range, but its 13.1 kHz cutoff is above 0.45 x 24 kHz. */
        {"24 kHz analog stream", 24000, 1, 1, 25000, 20000, 20000,
         "NFM bandwidth 25 kHz does not fit the 24 kHz DSP rate (the largest width it fits is 20.4 kHz)"},
        /* 16 kHz fits up to 13.2 kHz, so even the 16 kHz default width is out of reach when asked for explicitly. */
        {"16 kHz analog stream", 16000, 1, 1, 16000, 13000, 13000,
         "NFM bandwidth 16 kHz does not fit the 16 kHz DSP rate (the largest width it fits is 13.2 kHz)"},
        /* A replay decimating by 2 after demod runs the channel filter at twice the published rate: every requested
         * width is refused there, while the unset default keeps its legacy design (16 kHz at 48 kHz). */
        {"48 kHz replay with post_downsample 2", 48000, 1, 2, 12500, 0, 16000,
         "NFM bandwidth 12.5 kHz cannot be applied to this I/Q replay: post_downsample 2 runs the channel filter at "
         "96000 Hz"},
    };
    int rc = 0;
    for (const live_request_case& c : cases) {
        rc |= expect_live_refusal(c);
        rc |= expect_live_acceptance(c);
    }
    return rc;
}

/* A live request is checked against the rate the stream published when it was made; a retune that settles the device
 * on another rate can land before the demod thread consumes it. The width is held to the rate it lands on: one that
 * rate cannot realize is refused there, logged with the validator's text, and the front end keeps the receive profile
 * the retune left it on, rather than running the width without its channel filter. That goes for a width change on
 * the running monitor and for a switch that would move a DMR session onto it, which stays on the digital family. One
 * the landed rate still fits is applied, and the unset default lands on the legacy WIDE design where the landed rate
 * cannot fit 16 kHz, published as DSP-limited, with nothing refused. */
static int
test_request_across_rate_change(void) {
    rtl_stream_test_live_request_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    g_last_error[0] = '\0';
    g_error_count = 0;
    int rc = expect_int("16 kHz across 48 -> 16 kHz run",
                        rtl_stream_test_analog_request_across_rate_change(48000, 16000, 16000, 1, &r), 0);
    rc |= expect_int("16 kHz accepted at the published 48 kHz", r.request_rc, 0);
    rc |= expect_int("16 kHz queued", r.request_queued, 1);
    rc |= expect_int("16 kHz refused at the landed rate: width unchanged", r.width_after, r.width_before);
    rc |= expect_int("16 kHz refused at the landed rate: family unchanged", r.family_after, r.family_before);
    rc |= expect_int("16 kHz refused at the landed rate: plan kept", r.plan_kept, 1);
    rc |= expect_int("16 kHz refused at the landed rate: published width unchanged", r.published_width_after,
                     r.published_width_before);
    rc |= expect_int("landed-rate refusal logged once", g_error_count, 1);
    rc |= expect_int("landed-rate refusal logs the validator's text",
                     std::strstr(g_last_error, "NFM bandwidth 16 kHz does not fit the 16 kHz DSP rate (the largest "
                                               "width it fits is 13.2 kHz)")
                         != NULL,
                     1);
    rc |= expect_int("landed-rate refusal says the front end keeps its profile",
                     std::strstr(g_last_error, "The front end keeps its current receive profile.") != NULL, 1);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "  logged: \"%s\"\n", g_last_error);
    }

    DSD_MEMSET(&r, 0, sizeof r);
    rc |= expect_int("12.5 kHz across 48 -> 16 kHz run",
                     rtl_stream_test_analog_request_across_rate_change(48000, 16000, 12500, 1, &r), 0);
    rc |= expect_int("12.5 kHz accepted", r.request_rc, 0);
    rc |= expect_int("12.5 kHz still fits the landed rate and applies", r.width_after, 12500);
    rc |= expect_int("12.5 kHz published", r.published_width_after, 12500);

    DSD_MEMSET(&r, 0, sizeof r);
    g_last_error[0] = '\0';
    g_error_count = 0;
    rc |= expect_int("DMR to 25 kHz across 48 -> 24 kHz run",
                     rtl_stream_test_analog_request_across_rate_change(48000, 24000, 25000, 0, &r), 0);
    rc |= expect_int("DMR to 25 kHz accepted at the published 48 kHz", r.request_rc, 0);
    rc |= expect_int("DMR to 25 kHz queued", r.request_queued, 1);
    rc |= expect_int("DMR to 25 kHz: digital until the boundary", r.family_before, 0);
    rc |= expect_int("DMR to 25 kHz: refused at the landed rate, stays digital", r.family_after, 0);
    rc |= expect_int("DMR to 25 kHz: no monitor output", r.monitor_after, 0);
    rc |= expect_int("DMR to 25 kHz: channel filter unchanged", r.width_after, r.width_before);
    rc |= expect_int("DMR to 25 kHz: channel plan kept", r.plan_kept, 1);
    rc |= expect_int("DMR to 25 kHz: no analog profile published", r.published_width_after, 0);
    rc |= expect_int("DMR to 25 kHz: logged once", g_error_count, 1);
    rc |= expect_int("DMR to 25 kHz: logs the validator's text",
                     std::strstr(g_last_error, "NFM bandwidth 25 kHz does not fit the 24 kHz DSP rate (the largest "
                                               "width it fits is 20.4 kHz)")
                         != NULL,
                     1);
    rc |= expect_int("DMR to 25 kHz: says the front end keeps its profile",
                     std::strstr(g_last_error, "The front end keeps its current receive profile.") != NULL, 1);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "  logged: \"%s\"\n", g_last_error);
    }

    /* The unset default is not a requested width: the switch lands, on the legacy WIDE design at 16 kHz. */
    DSD_MEMSET(&r, 0, sizeof r);
    g_last_error[0] = '\0';
    g_error_count = 0;
    rc |= expect_int("DMR to the default across 48 -> 16 kHz run",
                     rtl_stream_test_analog_request_across_rate_change(48000, 16000, 0, 0, &r), 0);
    rc |= expect_int("DMR to the default: accepted", r.request_rc, 0);
    rc |= expect_int("DMR to the default: lands on the analog family", r.family_after, 1);
    rc |= expect_int("DMR to the default: onto the monitor output", r.monitor_after, 1);
    rc |= expect_int("DMR to the default: legacy WIDE design at 16 kHz", r.width_after, 0);
    rc |= expect_int("DMR to the default: published as DSP-limited", r.published_lpf_on_after, 0);
    rc |= expect_int("DMR to the default: publishes the width 16 kHz leaves", r.published_width_after, 13200);
    rc |= expect_int("DMR to the default: nothing refused", g_error_count, 0);
    return rc;
}

/* A digital session switching to the analog monitor with the unset NFM default at a DSP rate that cannot fit 16 kHz is
 * not refused: the default is not a requested width. The check accepts it, and the switch lands where a -fA open at the
 * same rate does: the historical rule leaves the channel filter off below a 20 kHz DSP rate, so the channel is
 * published as DSP-limited at the DSP rate. */
static int
test_digital_switch_default_at_low_rate(void) {
    rtl_stream_test_live_request_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    g_last_error[0] = '\0';
    g_error_count = 0;
    int rc = expect_int("DMR at 16 kHz to the default width run",
                        rtl_stream_test_analog_request_with_stream(16000, 0, 1, DSD_ANALOG_DEMOD_FM, 0, &r), 0);
    rc |= expect_int("DMR at 16 kHz: the check accepts the default", r.check_rc, 0);
    rc |= expect_int("DMR at 16 kHz: the request is accepted", r.request_rc, 0);
    rc |= expect_int("DMR at 16 kHz: the request is queued", r.request_queued, 1);
    rc |= expect_int("DMR at 16 kHz: digital before the boundary", r.family_before, 0);
    rc |= expect_int("DMR at 16 kHz: lands on the analog family", r.family_after, 1);
    rc |= expect_int("DMR at 16 kHz: onto the monitor output", r.monitor_after, 1);
    rc |= expect_int("DMR at 16 kHz: no width-driven design", r.width_after, 0);
    rc |= expect_int("DMR at 16 kHz: published as DSP-limited", r.published_lpf_on_after, 0);
    rc |= expect_int("DMR at 16 kHz: publishes the DSP rate", r.published_width_after, 16000);
    rc |= expect_int("DMR at 16 kHz: the default retune profile is accepted", r.retune_rc, 0);
    rc |= expect_int("DMR at 16 kHz: nothing refused", g_error_count, 0);
    return rc;
}

/* A decode-mode change to Analog checks the width against the published rate and commits its decoder on the answer,
 * then requests the analog profile. A retune can settle the device on another rate between the two. The request is
 * held to the rate the stream publishes when it is made, as every analog request is, so a width the new rate cannot
 * realize is refused there, logged once with the validator's text, and the front end stays on the digital family
 * rather than running the monitor without its channel filter. A width the new rate still fits lands filtered. */
static int
test_switch_request_after_rate_change(void) {
    rtl_stream_test_live_request_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    g_last_error[0] = '\0';
    g_error_count = 0;
    int rc = expect_int("DMR to 25 kHz, 48 -> 24 kHz before the request run",
                        rtl_stream_test_analog_switch_request_after_rate_change(48000, 24000, 25000, &r), 0);
    rc |= expect_int("checked at the published 48 kHz: accepted", r.check_rc, 0);
    rc |= expect_int("requested after the move to 24 kHz: refused", r.request_rc, -1);
    rc |= expect_int("requested after the move to 24 kHz: nothing queued", r.request_queued, 0);
    rc |= expect_int("digital before the boundary", r.family_before, 0);
    rc |= expect_int("stays on the digital family", r.family_after, 0);
    rc |= expect_int("no monitor output", r.monitor_after, 0);
    rc |= expect_int("channel filter unchanged", r.width_after, r.width_before);
    rc |= expect_int("no analog profile published", r.published_width_after, 0);
    rc |= expect_int("logged once", g_error_count, 1);
    rc |= expect_int("logs the validator's text",
                     std::strstr(g_last_error, "NFM bandwidth 25 kHz does not fit the 24 kHz DSP rate (the largest "
                                               "width it fits is 20.4 kHz)")
                         != NULL,
                     1);
    rc |= expect_int("says the front end keeps its profile",
                     std::strstr(g_last_error, "The front end keeps its current receive profile.") != NULL, 1);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "  logged: \"%s\"\n", g_last_error);
    }

    /* A width the new rate still fits lands filtered. */
    DSD_MEMSET(&r, 0, sizeof r);
    rc |= expect_int("DMR to 12.5 kHz, 48 -> 24 kHz before the request run",
                     rtl_stream_test_analog_switch_request_after_rate_change(48000, 24000, 12500, &r), 0);
    rc |= expect_int("12.5 kHz accepted", r.request_rc, 0);
    rc |= expect_int("12.5 kHz lands on the monitor", r.monitor_after, 1);
    rc |= expect_int("12.5 kHz reaches the filter", r.width_after, 12500);
    rc |= expect_int("12.5 kHz published as filtered", r.published_lpf_on_after, 1);
    return rc;
}

/* The digital leg of a switch under a DSD_NEO_CQPSK override, from a digital session switched to analog and from a -fA
 * start, held to a fresh open of the mode under the same override. @p ted_sps is what the decoder times the profile
 * with at the rate the switch predicts. */
static int
expect_override_digital_leg(const family_case& c, const char* cqpsk_env, int landing_cqpsk, int forced_rate_out_hz,
                            int ted_sps) {
    family_case lands = c;
    lands.request.ted_sps = ted_sps;
    static dsd_opts digital;
    static dsd_opts analog;
    DSD_MEMSET(&digital, 0, sizeof digital);
    DSD_MEMSET(&analog, 0, sizeof analog);
    c.configure(&digital);
    analog.analog_only = 1;
    analog.monitor_input_audio = 1;
    analog.analog_demod = DSD_ANALOG_DEMOD_FM;
    const int demod_rate_hz = forced_rate_out_hz > 0 ? forced_rate_out_hz : 48000;
    const int want_kind = landing_cqpsk ? RTL_STREAM_OUTPUT_SYMBOL_CQPSK : RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
    int rc = 0;
    char label[160];
    for (int from_analog_start = 0; from_analog_start <= 1; from_analog_start++) {
        rtl_stream_test_family_switch_result r;
        DSD_MEMSET(&r, 0, sizeof r);
        DSD_SNPRINTF(label, sizeof label, "DSD_NEO_CQPSK=%s, %s%s@%d run", cqpsk_env,
                     from_analog_start ? "-fA start, " : "", c.name, demod_rate_hz);
        const int run_rc = from_analog_start
                               ? rtl_stream_test_analog_start_family_switch(&digital, &analog, 48000,
                                                                            forced_rate_out_hz, &lands.request, &r)
                               : rtl_stream_test_analog_family_switch(&digital, &analog, 48000, forced_rate_out_hz,
                                                                      &lands.request, &r);
        rc |= expect_int(label, run_rc, 0);
        DSD_SNPRINTF(label, sizeof label, "DSD_NEO_CQPSK=%s, %s%s@%d -> digital", cqpsk_env,
                     from_analog_start ? "-fA start, " : "", c.name, demod_rate_hz);
        rc |= expect_int(label, r.digital_request_rc, 0);
        rc |= expect_int(label, r.fresh_digital.output_kind, want_kind);
        rc |= expect_fields_equal(label, r.switched_digital, r.fresh_digital);
        rc |= expect_int(label, (int)r.predicted_digital_output_rate, r.switched_digital.output_rate);
        /* The analog family never runs CQPSK: a -fA open under the override runs the FM monitor, and so does the
           switch to analog (from the digital session). */
        DSD_SNPRINTF(label, sizeof label, "DSD_NEO_CQPSK=%s, %s@%d: -fA open runs the FM monitor", cqpsk_env, c.name,
                     demod_rate_hz);
        rc |= expect_int(label, r.fresh_analog.output_kind, RTL_STREAM_OUTPUT_AUDIO_MONITOR);
        rc |= expect_int(label, r.fresh_analog.cqpsk_enable, 0);
        rc |= expect_int(label, r.fresh_analog.demod_is_fm, 1);
        rc |= expect_int(label, r.fresh_analog.ted_enabled, 0);
        if (!from_analog_start) {
            DSD_SNPRINTF(label, sizeof label, "DSD_NEO_CQPSK=%s, %s@%d -> analog", cqpsk_env, c.name, demod_rate_hz);
            rc |= expect_analog_fields_equal(label, r.switched_analog, r.fresh_analog);
        }
    }
    return rc;
}

/* DSD_NEO_CQPSK decides the CQPSK family a digital stream opens on, whatever the mode asks for, so a switch from the
 * analog family to a digital mode lands where an open of that mode would under the override: off, a P25 CQPSK mode runs
 * the FSK discriminator (resampled to 48 kHz at a forced 78125 Hz, as a C4FM open is); on, every digital mode runs
 * CQPSK (never resampled). The decoder's output-rate prediction follows the same override. The analog family is not
 * a CQPSK family: a -fA open under the override runs the FM monitor, as the switch to analog does, and the two are
 * held equal as well. */
static int
test_cqpsk_override_lands_like_open(const family_case* p25_c4fm_case, const family_case* p25_cqpsk_case,
                                    const family_case* dmr_case) {
    int rc = 0;
    (void)dsd_setenv("DSD_NEO_CQPSK", "0", 1);
    dsd_neo_config_init();
    rc |= expect_override_digital_leg(*p25_cqpsk_case, "0", 0, 0, 10);
    rc |= expect_override_digital_leg(*p25_cqpsk_case, "0", 0, 78125, 10);
    rc |= expect_override_digital_leg(*p25_c4fm_case, "0", 0, 78125, 10);
    family_case split = *p25_cqpsk_case;
    split.request.boundary_between_requests = 1;
    rc |= expect_override_digital_leg(split, "0", 0, 78125, 10);

    (void)dsd_setenv("DSD_NEO_CQPSK", "1", 1);
    dsd_neo_config_init();
    rc |= expect_override_digital_leg(*p25_c4fm_case, "1", 1, 0, 10);
    rc |= expect_override_digital_leg(*p25_c4fm_case, "1", 1, 78125, 16);
    rc |= expect_override_digital_leg(*dmr_case, "1", 1, 0, 10);
    rc |= expect_override_digital_leg(*p25_cqpsk_case, "1", 1, 78125, 16);

    (void)dsd_unsetenv("DSD_NEO_CQPSK");
    dsd_neo_config_init();
    return rc;
}

/* With the channel filter off (DSD_NEO_CHANNEL_LPF=0, or a DSP rate below 20 kHz under the default rule), a digital
 * open keeps the WIDE channel profile for an FSK mode, since it picks a protocol profile only for a filter that runs
 * (and CQPSK always the P25 CQPSK one). A switch out of the analog family lands on the same profile, and so on the
 * same FSK modem configuration, published bandwidth and SNR correction, not on the one the mode's symbol profile
 * names. */
static int
test_lpf_off_lands_like_open(const family_case* cases, size_t n_cases) {
    static const char* const lpf_env_names[] = {"P25 C4FM, LPF off", "P25 CQPSK, LPF off", "DMR, LPF off",
                                                "NXDN48, LPF off", "dPMR, LPF off"};
    int rc = 0;
    (void)dsd_setenv("DSD_NEO_CHANNEL_LPF", "0", 1);
    dsd_neo_config_init();
    for (size_t i = 0; i < n_cases && i < sizeof lpf_env_names / sizeof lpf_env_names[0]; i++) {
        family_case off = cases[i];
        off.name = lpf_env_names[i];
        rc |= run_case(off, 48000, 0, 0);
        rc |= run_analog_start_case(off, 48000, 0);
    }
    family_case nxdn60 = cases[3];
    nxdn60.name = "NXDN48, LPF off";
    nxdn60.request.ted_sps = 25;
    rc |= run_case(nxdn60, 48000, 60000, 0);
    (void)dsd_unsetenv("DSD_NEO_CHANNEL_LPF");
    dsd_neo_config_init();

    /* A 12 kHz DSP rate: the default rule leaves the filter off below 20 kHz. */
    family_case nxdn12 = cases[3];
    nxdn12.name = "NXDN48, 12 kHz DSP rate";
    nxdn12.request.ted_sps = 5;
    family_case dpmr12 = cases[4];
    dpmr12.name = "dPMR, 12 kHz DSP rate";
    dpmr12.request.ted_sps = 5;
    rc |= run_case(nxdn12, 12000, 0, 0);
    rc |= run_analog_start_case(nxdn12, 12000, 0);
    rc |= run_case(dpmr12, 12000, 0, 0);
    rc |= run_analog_start_case(dpmr12, 12000, 0);
    family_case split12 = nxdn12;
    split12.request.boundary_between_requests = 1;
    rc |= run_case(split12, 12000, 0, 0);
    return rc;
}

/* The decoder reads the output ring while the demod thread switches family and clears it. A read that has copied
 * samples but not yet published its tail when the switch arrives must finish before the clear, or its old tail lands
 * on the cleared indices and the ring reports a backlog of the old family's samples. The switch waits for the read,
 * the read is discarded (the generation moved under it), and the ring comes out empty. */
static int
test_switch_during_live_read(void) {
    rtl_stream_test_read_race_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    int rc = expect_int("read race run", rtl_stream_test_family_switch_during_live_read(100, &r), 0);
    rc |= expect_int("read race: the read stopped before its tail store", r.paused, 1);
    rc |= expect_int("read race: seeded ring was not empty", r.used_before > 0U, 1);
    rc |= expect_int("read race: the switch waits for the read in flight", r.switch_done_during_read, 0);
    rc |= expect_int("read race: the switch landed", r.analog_family_after, 1);
    rc |= expect_int("read race: the switch bumps the generation", r.generation_after != r.generation_before, 1);
    rc |= expect_int("read race: the read in flight is discarded", r.read_got, 0);
    rc |= expect_int("read race: ring empty after the switch", (int)r.used_after, 0);
    rc |= expect_int("read race: indices agree", r.tail_after == r.head_after, 1);
    return rc;
}

/* A read can also load the generation after the switch's clear has bumped it and still reach the ring before the clear
 * takes ready_m. It finishes first, with samples the clear was meant to drop, under the generation it loaded. The
 * stream must not stay on that generation once the ring is cleared, or the decoder takes those samples as the new
 * family's and keeps its caches of them. */
static int
test_read_before_switch_clear(void) {
    rtl_stream_test_clear_race_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    int rc = expect_int("clear race run", rtl_stream_test_live_read_during_family_switch_clear(&r), 0);
    rc |= expect_int("clear race: the switch stopped in its ring clear", r.paused, 1);
    rc |= expect_int("clear race: seeded ring was not empty", r.used_before > 0U, 1);
    rc |= expect_int("clear race: the read ran after the clear's first bump", r.read_generation != r.generation_before,
                     1);
    rc |= expect_int("clear race: the read finished before the clear", r.switch_done_during_read, 0);
    rc |= expect_int("clear race: the read took samples the clear drops", r.read_got, 16);
    rc |= expect_int("clear race: the switch landed", r.analog_family_after, 1);
    rc |= expect_int("clear race: the stream leaves the generation that read ran under",
                     r.generation_after != r.read_generation, 1);
    rc |= expect_int("clear race: ring empty after the switch", (int)r.used_after, 0);
    rc |= expect_int("clear race: indices agree", r.tail_after == r.head_after, 1);
    return rc;
}

/* The decode modes the decoder notes (rtl_stream_set_digital_decode_modes()) decide only once a live switch has moved
 * the stream onto the digital family. A P25 open whose decoder notes D-STAR (a digital-to-digital mode change) keeps
 * picking the FSK channel profile a CQPSK toggle-off returns to from the options it opened with, as it always has; after
 * a switch to analog and back it picks D-STAR's; and a new open drops the note, so the options it opens with decide
 * again. */
static int
test_noted_digital_modes_scope(void) {
    rtl_stream_test_noted_modes_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    int rc = expect_int("noted modes run", rtl_stream_test_noted_digital_modes_scope(&r), 0);
    rc |= expect_int("noted modes: no switch yet, the P25 options decide", r.unswitched_profile,
                     RTL_STREAM_CHANNEL_PROFILE_P25_C4FM);
    rc |= expect_int("noted modes: switched to digital, D-STAR decides", r.switched_profile,
                     RTL_STREAM_CHANNEL_PROFILE_6K25);
    rc |= expect_int("noted modes: a new open drops the note", r.reopened_profile, RTL_STREAM_CHANNEL_PROFILE_P25_C4FM);
    return rc;
}

int
main(void) {
    dsd_neo_log_set_tap(capture_error_log, NULL);
    dsd_neo_config_init();
    const family_case cases[] = {
        {"P25 C4FM",
         p25_c4fm,
         {0, 4800, 4, RTL_STREAM_CHANNEL_PROFILE_P25_C4FM, 10, 0, RTL_STREAM_TEST_UNDER_ANALOG_NONE}},
        {"P25 CQPSK",
         p25_cqpsk,
         {1, 4800, 4, RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK, 10, 0, RTL_STREAM_TEST_UNDER_ANALOG_NONE}},
        {"DMR", dmr, {0, 4800, 4, RTL_STREAM_CHANNEL_PROFILE_12K5, 10, 0, RTL_STREAM_TEST_UNDER_ANALOG_NONE}},
        {"NXDN48", nxdn48, {0, 2400, 4, RTL_STREAM_CHANNEL_PROFILE_6K25, 20, 0, RTL_STREAM_TEST_UNDER_ANALOG_NONE}},
        {"dPMR", dpmr, {0, 2400, 4, RTL_STREAM_CHANNEL_PROFILE_6K25, 20, 0, RTL_STREAM_TEST_UNDER_ANALOG_NONE}},
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

    /* ProVoice (9600 sym/s, 2 levels). At a 12 kHz DSP rate its symbol rate gets under two samples per symbol, and an
     * open times it with the two samples it clamps to, as the decoder does (dsd_opts_compute_sps_rate()); the symbol
     * profile the switch lands on would otherwise leave the one sample the profile setter keeps. */
    const family_case pv = {"ProVoice",
                            provoice,
                            {0, 9600, 2, RTL_STREAM_CHANNEL_PROFILE_PROVOICE, 5, 0, RTL_STREAM_TEST_UNDER_ANALOG_NONE}};
    rc |= run_case(pv, 48000, 0, 0);
    rc |= run_analog_start_case(pv, 48000, 0);
    family_case pv12 = pv;
    pv12.request.ted_sps = 2;
    rc |= run_case(pv12, 12000, 0, 0);
    rc |= run_analog_start_case(pv12, 12000, 0);
    family_case pv12_split = pv12;
    pv12_split.request.boundary_between_requests = 1;
    rc |= run_case(pv12_split, 12000, 0, 0);

    /* D-STAR (4800 sym/s over two levels, the 6.25 kHz channel profile). CQPSK forces four levels, and the DSP menu's
     * toggle turning it off again returns to the channel profile the mode's options pick, not the 12.5 kHz one four
     * levels at 4800 sym/s give on their own; after a -fA start the stream's options name no digital mode at all. */
    const family_case ds = {
        "D-STAR", dstar, {0, 4800, 2, RTL_STREAM_CHANNEL_PROFILE_6K25, 10, 0, RTL_STREAM_TEST_UNDER_ANALOG_NONE}};
    rc |= run_case(ds, 48000, 0, 0);
    rc |= run_analog_start_case(ds, 48000, 0);
    rc |= run_under_analog_case(ds, RTL_STREAM_TEST_UNDER_ANALOG_CQPSK_TOGGLE, 48000, 0);

    /* The same switches from a session that started with -fA rather than from a digital open, so nothing the digital
     * open left behind can stand in for what the switch must set: each mode, the 24 kHz and forced-rate resampler
     * decisions, and the boundary that falls between the family request and its symbol profile. */
    for (const family_case& c : cases) {
        rc |= run_analog_start_case(c, 48000, 0);
    }
    rc |= run_analog_start_case(dmr24, 24000, 0);
    rc |= run_analog_start_case(cqpsk78, 48000, 78125);
    rc |= run_analog_start_case(c4fm78, 48000, 78125);
    rc |= run_analog_start_case(nxdn60, 48000, 60000);
    rc |= run_analog_start_case(dpmr60_split, 48000, 60000);

    /* A retune settles the device on a forced 78125 Hz between the decoder's requests, timed for the 48 kHz the -fA
     * session published, and the demod thread's consume. */
    rc |= run_analog_start_across_retune_case(cases[1], 78125);
    rc |= run_analog_start_across_retune_case(cases[0], 78125);
    family_case cqpsk_split_across = cases[1];
    cqpsk_split_across.request.boundary_between_requests = 1;
    rc |= run_analog_start_across_retune_case(cqpsk_split_across, 78125);

    /* The same switch from a -fA session a CQPSK toggle or a typed DMR row had moved off the monitor output, including
     * the forced-rate resampler decisions and a boundary between the family request and its profile. */
    const int under_analog[] = {RTL_STREAM_TEST_UNDER_ANALOG_CQPSK_TOGGLE, RTL_STREAM_TEST_UNDER_ANALOG_TYPED_ROW};
    for (int under : under_analog) {
        for (const family_case& c : cases) {
            rc |= run_under_analog_case(c, under, 48000, 0);
        }
        rc |= run_under_analog_case(dmr24, under, 24000, 0);
        rc |= run_under_analog_case(cqpsk78, under, 48000, 78125);
        rc |= run_under_analog_case(c4fm78, under, 48000, 78125);
        rc |= run_under_analog_case(dpmr60_split, under, 48000, 60000);
        rc |= run_under_analog_case(dmr48_split, under, 48000, 0);
    }

    /* A CQPSK toggle still queued when the digital mode is picked, with a boundary between the family request and the
     * mode's symbol profile: each mode, the 24 kHz and forced-rate resampler decisions. */
    for (const family_case& c : cases) {
        rc |= run_queued_toggle_case(c, 48000, 0);
    }
    rc |= run_queued_toggle_case(dmr24, 24000, 0);
    rc |= run_queued_toggle_case(cases[2], 48000, 78125);
    rc |= run_queued_toggle_case(c4fm78, 48000, 78125);
    rc |= run_queued_toggle_case(nxdn60, 48000, 60000);

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

    rc |= test_digital_row_on_analog_session();
    rc |= test_width_only_change();
    rc |= test_requests_without_stream();
    rc |= test_requests_against_running_stream();
    rc |= test_request_across_rate_change();
    rc |= test_digital_switch_default_at_low_rate();
    rc |= test_switch_request_after_rate_change();
    rc |= test_cqpsk_override_lands_like_open(&cases[0], &cases[1], &cases[2]);
    rc |= test_lpf_off_lands_like_open(cases, sizeof cases / sizeof cases[0]);
    rc |= test_switch_during_live_read();
    rc |= test_read_before_switch_clear();
    rc |= test_noted_digital_modes_scope();

    /* AM (issue #524): digital <-> AM on running streams, AM start to digital, live FM <-> AM, and AM widths held to
       the running rate. */
    rc |= run_am_case(cases[2], 48000, 0);
    rc |= run_am_case(cases[0], 24000, 20000);
    rc |= test_fm_am_kind_switch();
    rc |= test_monitor_output_scale();
    rc |= test_am_monitor_keeps_detector_under_symbol_profiles();
    rc |= test_monitor_return_from_cqpsk_is_the_analog_request_alone();
    rc |= test_am_requests_against_running_stream();
    rc |= expect_int("AM accepted with no stream",
                     rtl_stream_request_analog_profile(DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_AM, 0), 0);

    /* Requests the front end cannot honour are refused up front. */
    rc |= expect_int("bad family refused", rtl_stream_request_analog_profile(7, DSD_ANALOG_DEMOD_FM, 0), -1);
    rc |= expect_int("out-of-range width refused",
                     rtl_stream_request_analog_profile(DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_FM, 30000), -1);
    rc |= expect_int("negative width refused",
                     rtl_stream_request_analog_profile(DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_FM, -1), -1);
    if (rc == 0) {
        std::printf("IO_RTL_ANALOG_FAMILY_SWITCH: OK\n");
    }
    return rc;
}
