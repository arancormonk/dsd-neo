// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The symbol grid reads the raw discriminator until a sync names a protocol and
 * the matched filter's output afterwards, and those describe different instants:
 * a symmetric FIR of L taps reports the signal (L-1)/2 samples in the past. These
 * cases drive getSymbol() from a WAV across every switch the grid can make --
 * raw to a filter, a filter to a shorter one, to a longer one, and back to raw --
 * and check one property at each: the content position the grid reads continues
 * without a rewind or a skip, and what it reads there is what a filter that had
 * been running all along would have produced at that position (issue #444).
 *
 * The switch is keyed on lastsynctype, which frame sync sets at an accept and
 * noCarrier clears, always between symbols; flipping it between getSymbol()
 * calls is the real sequence. have_sync is 1 throughout so the inter-frame
 * timing nudge stays out of the way and the grid moves one span per symbol.
 */

#include <assert.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/sps_filters.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/shutdown.h>
#include <math.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

dsd_socket_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
Connect(char* hostname, int portno) {
    (void)hostname;
    (void)portno;
    return (dsd_socket_t)0;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
openAudioInput(dsd_opts* opts) {
    (void)opts;
    return -1;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_audio_reconfigure_output_for_input_policy(dsd_opts* opts) {
    (void)opts;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_request_shutdown(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    exitflag = 1;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_audio_rescale_symbol_timing(dsd_state* state, int old_rate_hz, int new_rate_hz) {
    (void)state;
    (void)old_rate_hz;
    (void)new_rate_hz;
}

double
// NOLINTNEXTLINE(misc-use-internal-linkage)
pwr_to_dB(double mean_power) {
    (void)mean_power;
    return 0.0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
lpf_f(dsd_state* state, float* input, int len) {
    (void)state;
    (void)input;
    (void)len;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
hpf_f(dsd_state* state, float* input, int len) {
    (void)state;
    (void)input;
    (void)len;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
pbf_f(dsd_state* state, float* input, int len) {
    (void)state;
    (void)input;
    (void)len;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
analog_gain_f(const dsd_opts* opts, dsd_state* state, float* input, int len) {
    (void)opts;
    (void)state;
    (void)input;
    (void)len;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
agsm_f(dsd_opts* opts, dsd_state* state, float* input, int len) {
    (void)opts;
    (void)state;
    (void)input;
    (void)len;
}

#define SPS        10
#define CENTER     4
#define TOTAL      6000 /* samples in the WAV: far more than any sequence reads */
#define PHASE_SYMS 20   /* symbols read in each phase of a sequence */
#define MAX_KINDS  3

/* Deterministic and non-repeating, so a rewind or a skip of one sample shows. */
static short
stimulus(int n) {
    const float v = sinf(0.13f * (float)n) * 6000.0f + cosf(0.021f * (float)n) * 2500.0f + 11.0f * (float)(n % 7);
    return (short)lrintf(v);
}

static short g_raw[TOTAL];
/* What each filter would have produced at every content position, had it been
   running from the start. Index is the position, not the feed count. */
static float g_ref[MAX_KINDS][TOTAL];

static int
kind_slot(dsd_sps_filter_kind kind) {
    switch (kind) {
        case DSD_SPS_FILTER_NONE: return 0;
        case DSD_SPS_FILTER_P25: return 1;
        case DSD_SPS_FILTER_DMR: return 2;
        default: assert(0); return 0;
    }
}

static const char*
kind_name(dsd_sps_filter_kind kind) {
    switch (kind) {
        case DSD_SPS_FILTER_P25: return "p25";
        case DSD_SPS_FILTER_DMR: return "dmr";
        default: return "raw";
    }
}

static void
build_references(void) {
    for (int n = 0; n < TOTAL; n++) {
        g_raw[n] = stimulus(n);
        g_ref[0][n] = (float)g_raw[n];
    }
    const dsd_sps_filter_kind kinds[] = {DSD_SPS_FILTER_P25, DSD_SPS_FILTER_DMR};
    for (unsigned k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        const int delay = dsd_sps_filter_group_delay(kinds[k], SPS);
        assert(delay > 0);
        init_rrc_filter_memory();
        for (int n = 0; n < TOTAL; n++) {
            /* Feeding sample n yields the filtered signal at position n - delay. */
            const float y = dsd_sps_filter_apply(kinds[k], (float)g_raw[n], SPS);
            if (n - delay >= 0) {
                g_ref[kind_slot(kinds[k])][n - delay] = y;
            }
        }
    }
    init_rrc_filter_memory();
}

static int
write_wav(char* out_path, size_t out_path_size) {
    int fd = dsd_test_mkstemp(out_path, out_path_size, "dsdneo_seam");
    if (fd < 0) {
        return -1;
    }
    dsd_close(fd);
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    info.samplerate = 48000;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* wav = sf_open(out_path, SFM_WRITE, &info);
    if (wav == NULL) {
        return -1;
    }
    const int ok = sf_write_short(wav, g_raw, TOTAL) == TOTAL;
    sf_close(wav);
    return ok ? 0 : -1;
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    int position; /* content position of the next symbol's first sample */
} grid;

static void
grid_open(grid* g, dsd_opts* opts, dsd_state* state, const char* wav_path) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_file_info = (SF_INFO*)calloc(1, sizeof(*opts->audio_in_file_info));
    assert(opts->audio_in_file_info != NULL);
    opts->audio_in_file = sf_open(wav_path, SFM_READ, opts->audio_in_file_info);
    assert(opts->audio_in_file != NULL);
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->audio_out_type = 1;
    opts->input_volume_multiplier = 1;
    opts->wav_sample_rate = 48000;
    opts->use_cosine_filter = 1;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "%s", wav_path);

    state->samplesPerSymbol = SPS;
    state->symbolCenter = CENTER;
    state->rf_mod = 0;
    state->center = 0.0f;
    state->min = -32768.0f; /* the synced clip must not touch anything */
    state->max = 32767.0f;
    state->synctype = DSD_SYNC_NONE;
    state->lastsynctype = DSD_SYNC_NONE;
    state->jitter = -1;
    exitflag = 0;
    init_rrc_filter_memory();

    g->opts = opts;
    g->state = state;
    g->position = 0;
}

static void
grid_close(grid* g) {
    if (g->opts->audio_in_file) {
        sf_close(g->opts->audio_in_file);
        g->opts->audio_in_file = NULL;
    }
    free(g->opts->audio_in_file_info);
    g->opts->audio_in_file_info = NULL;
}

/* The C4FM decision window the symbolizer uses for this lastsynctype. */
static void
window_for(int lastsynctype, int* l, int* r) {
    *l = DSD_SYNC_IS_DMR_BS(lastsynctype) ? 1 : 2;
    *r = 2;
}

/*
 * Read @p symbols symbols with the grid told that @p lastsynctype is the stream
 * it is on, and count the ones that are not the mean of @p kind's reference
 * over the decision window at the positions the grid should be reading.
 */
static int
run_symbols(grid* g, int lastsynctype, dsd_sps_filter_kind kind, int symbols, const char* label) {
    g->state->lastsynctype = lastsynctype;
    int l = 0;
    int r = 0;
    window_for(lastsynctype, &l, &r);
    const float* ref = g_ref[kind_slot(kind)];
    int mismatches = 0;
    for (int m = 0; m < symbols; m++) {
        const float got = getSymbol(g->opts, g->state, 1);
        assert(exitflag == 0);
        float want = 0.0f;
        for (int i = CENTER - l; i <= CENTER + r; i++) {
            want += ref[g->position + i];
        }
        want /= (float)(l + r + 1);
        const float tol = 1e-3f * (fabsf(want) + 1.0f);
        if (fabsf(got - want) > tol) {
            if (mismatches < 3) {
                DSD_FPRINTF(stderr, "  %s: %s symbol %d at position %d: got %.2f want %.2f\n", label, kind_name(kind),
                            m, g->position, (double)got, (double)want);
            }
            mismatches++;
        }
        g->position += SPS;
    }
    DSD_FPRINTF(stderr, "%s: %s phase, %d of %d symbols off\n", label, kind_name(kind), mismatches, symbols);
    return mismatches;
}

static int
run_phase(grid* g, int lastsynctype, dsd_sps_filter_kind kind, const char* label) {
    return run_symbols(g, lastsynctype, kind, PHASE_SYMS, label);
}

/* Sanity: with no filter in play the grid reads contiguous raw positions, which
   is what every other case's bookkeeping assumes. */
static void
test_raw_grid_reads_contiguous_positions(const char* wav_path) {
    static dsd_opts opts;
    static dsd_state state;
    grid g;
    grid_open(&g, &opts, &state, wav_path);
    assert(run_phase(&g, DSD_SYNC_NONE, DSD_SPS_FILTER_NONE, "raw only") == 0);
    grid_close(&g);
}

/* Raw to a filter: the classic switch-on at a sync accept. The first symbol
   after it must already be the filtered signal at the position the grid was
   about to read, from a full window of real history. */
static void
test_switch_on_from_raw_continues_the_position(const char* wav_path) {
    static dsd_opts opts;
    static dsd_state state;
    grid g;
    grid_open(&g, &opts, &state, wav_path);
    assert(run_phase(&g, DSD_SYNC_NONE, DSD_SPS_FILTER_NONE, "switch-on") == 0);
    assert(run_phase(&g, DSD_SYNC_P25P1_POS, DSD_SPS_FILTER_P25, "switch-on") == 0);
    grid_close(&g);
}

/* A filter back to raw: noCarrier. The filter was reporting the signal a group
   delay in the past, so the raw stream has to resume from there, not from the
   live sample, or the grid skips that much content. */
static void
test_switch_off_to_raw_continues_the_position(const char* wav_path) {
    static dsd_opts opts;
    static dsd_state state;
    grid g;
    grid_open(&g, &opts, &state, wav_path);
    assert(run_phase(&g, DSD_SYNC_NONE, DSD_SPS_FILTER_NONE, "switch-off") == 0);
    assert(run_phase(&g, DSD_SYNC_P25P1_POS, DSD_SPS_FILTER_P25, "switch-off") == 0);
    assert(run_phase(&g, DSD_SYNC_NONE, DSD_SPS_FILTER_NONE, "switch-off") == 0);
    grid_close(&g);
}

/* A longer filter to a shorter one, which AUTO does when the sync it accepts
   changes protocol: the shorter one reports less far back, so the difference
   has to be re-read, not skipped. */
static void
test_switch_to_shorter_filter_continues_the_position(const char* wav_path) {
    static dsd_opts opts;
    static dsd_state state;
    grid g;
    grid_open(&g, &opts, &state, wav_path);
    assert(run_phase(&g, DSD_SYNC_NONE, DSD_SPS_FILTER_NONE, "shorter") == 0);
    assert(run_phase(&g, DSD_SYNC_P25P1_POS, DSD_SPS_FILTER_P25, "shorter") == 0);
    assert(run_phase(&g, DSD_SYNC_DMR_BS_VOICE_POS, DSD_SPS_FILTER_DMR, "shorter") == 0);
    grid_close(&g);
}

/* A shorter filter to a longer one: only the difference in delay is owed. */
static void
test_switch_to_longer_filter_continues_the_position(const char* wav_path) {
    static dsd_opts opts;
    static dsd_state state;
    grid g;
    grid_open(&g, &opts, &state, wav_path);
    assert(run_phase(&g, DSD_SYNC_NONE, DSD_SPS_FILTER_NONE, "longer") == 0);
    assert(run_phase(&g, DSD_SYNC_DMR_BS_VOICE_POS, DSD_SPS_FILTER_DMR, "longer") == 0);
    assert(run_phase(&g, DSD_SYNC_P25P1_POS, DSD_SPS_FILTER_P25, "longer") == 0);
    grid_close(&g);
}

/* A sync accept can land while a switch-off is still handing samples back: the
   P25 filter's 45 span four and a half symbols, and noCarrier clears the sync
   between frames. The filter switching on then has to prime from before the
   samples still owed and consume them, in order, as part of its catch-up. */
static void
test_switch_on_while_handing_back_continues_the_position(const char* wav_path) {
    static dsd_opts opts;
    static dsd_state state;
    grid g;
    grid_open(&g, &opts, &state, wav_path);
    assert(run_phase(&g, DSD_SYNC_NONE, DSD_SPS_FILTER_NONE, "handing back") == 0);
    assert(run_phase(&g, DSD_SYNC_P25P1_POS, DSD_SPS_FILTER_P25, "handing back") == 0);
    assert(run_symbols(&g, DSD_SYNC_NONE, DSD_SPS_FILTER_NONE, 2, "handing back") == 0);
    assert(g.state->matched_filter.replay > 0);
    assert(run_phase(&g, DSD_SYNC_P25P1_POS, DSD_SPS_FILTER_P25, "handing back") == 0);
    assert(run_symbols(&g, DSD_SYNC_NONE, DSD_SPS_FILTER_NONE, 1, "handing back") == 0);
    assert(g.state->matched_filter.replay > 0);
    assert(run_phase(&g, DSD_SYNC_DMR_BS_VOICE_POS, DSD_SPS_FILTER_DMR, "handing back") == 0);
    grid_close(&g);
}

typedef struct {
    const char* name;
    void (*run)(const char* wav_path);
} seam_case;

static const seam_case kCases[] = {
    {"raw", test_raw_grid_reads_contiguous_positions},
    {"switch-on", test_switch_on_from_raw_continues_the_position},
    {"switch-off", test_switch_off_to_raw_continues_the_position},
    {"shorter", test_switch_to_shorter_filter_continues_the_position},
    {"longer", test_switch_to_longer_filter_continues_the_position},
    {"handing-back", test_switch_on_while_handing_back_continues_the_position},
};

/* An optional argument names one case to run alone, for watching it fail. */
int
main(int argc, char** argv) {
    build_references();
    char wav_path[DSD_TEST_PATH_MAX];
    assert(write_wav(wav_path, sizeof(wav_path)) == 0);

    int ran = 0;
    for (unsigned c = 0; c < sizeof(kCases) / sizeof(kCases[0]); c++) {
        if (argc > 1 && strcmp(argv[1], kCases[c].name) != 0) {
            continue;
        }
        kCases[c].run(wav_path);
        ran++;
    }
    assert(ran > 0);

    remove(wav_path);
    DSD_FPRINTF(stderr, "symbol matched filter seam: OK\n");
    return 0;
}
