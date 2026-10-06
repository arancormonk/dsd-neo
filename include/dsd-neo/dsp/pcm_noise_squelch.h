// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The PCM noise squelch (`--squelch noise[+N]` on audio input, issue #628): it opens when the noise above the
 * voice band of externally demodulated FM audio (a scanner's discriminator tap, an SDR program's or rtl_fm's FM output)
 * quiets N dB under the noise-only reading it learns from the input itself.
 *
 * It runs on the PCM samples the analog monitor reads, at the rate it reads them (staged 8-24 kHz input arrives at
 * 48 kHz), on the decoder thread. It is pure: no clocks, no I/O, no allocation, and every decision is taken at a
 * sample-exact boundary, so the output depends only on the samples, never on how they are cut into blocks.
 *
 * Why it learns (tools/pcm_noise_squelch_model.py, docs/testing.md "PCM noise squelch design gate"): the audio arrives
 * at an unknown gain through unknown filters, so nothing can calibrate what noise alone reads. But the discriminator
 * sees phase only, so that reading belongs to the source chain (its gain, filters and channel width), the same on
 * every channel; and a carrier can only quiet it, so noise alone is the loudest the band reads.
 *
 * Band: B = [3.8 kHz, hi], hi = min(6.5 kHz, 0.45 x the native rate), at least 1200 Hz (8 and 9.6 kHz sources have
 * none: NO_ROOM). It is cut into K sub-bands of about DSD_PCM_NOISE_SQUELCH_SUB_BAND_HZ (at most
 * DSD_PCM_NOISE_SQUELCH_MAX_SUB_BANDS) and DSD_PCM_NOISE_SQUELCH_SETS - 1 staggered sets of K - 1 more, each shifted by
 * a further 1/SETS of a sub-band, so a line on a boundary of one set sits inside a band-pass of another. Each band-pass
 * is a Butterworth band-pass of prototype order 4 (four biquads), as are the voice band [400, 2600] Hz and its
 * DSD_PCM_NOISE_SQUELCH_VOICE_PARTS parts (400-800, 800-1300, 1300-1900 and 1900-2600 Hz).
 *
 * Windows: 40 ms (two 20 ms halves, boundary k at floor(k fs / 50) samples), taken every 20 ms. Each window gives each
 * band-pass's mean power p_k, the voice band's V and its parts' V_j, and the input's mean square E. A window whose E is
 * 0 (digital silence) closes the gate and ends the stretch, as a restart does; nothing is learned from it.
 *
 * Learning (from the last M = 4 windows): the run is steady when their above-band powers A = sum p_k (k < K, in dB)
 * all sit within 1.5 dB of their median; its levels are the means over those windows (sp, sa, sv, sv_j).
 *  - LEARNING: the first steady run held for 3 windows becomes the reference r_k (PROVISIONAL). The gate is closed
 *    until then. A carrier taken as the reference only mutes; the first noise corrects it. (A steady tone strong enough
 *    to leave nothing above voice can read NO_BAND instead, which plays it.)
 *  - A steady run 4 dB or more from the current stretch, held for 3 windows, is a transition. Louder and up within
 *    4 dB of the reference: that side is noise and becomes the reference (KNOWN, when the voice band did not move with
 *    it). A louder run well under the reference is the carrier's modulation or level changing (speech after a pause, a
 *    fading carrier), and the reference stays. Quieter, from a stretch at the reference, with the voice band stationary
 *    and moving by the same amount (within 1.5 dB): a gain step, taken after 200 ms more of it if three windows in four
 *    keep noise's spectrum (below) with the voice band stationary: the reference moves by their mean step. Otherwise
 *    a carrier keyed (KNOWN).
 *  - Noise come back at a lower gain (the source turned down in noise, during a transmission or across a pause): a
 *    run more than 1.5 dB under the reference has noise's spectrum against it when the voice band within 1.5 dB and
 *    each voice part within 2.5 dB moved as far as the band (its participating sub-bands' median move, which a spur in
 *    one or two does not shift) and the sub-bands tilt by at most 2.5 dB end to end (a least-squares line across those
 *    within 3 dB of that move). The stretch's runs gather that evidence at their level (a level that moves 1.5 dB
 *    starts over); once 0.4 s of them hold it in three of four with the voice band steady, moved 1.5 dB or more on
 *    average, the reference moves by their mean step. A carrier's noise falls toward the low voice parts, and a tone
 *    or speech fills some parts and not others, so neither gathers it.
 *  - A downward step (gain or lowered) keeps the reference from before it (from before the first, on a step already
 *    held) until a new one replaces it; the cache keeps it with the reference. Eight of ten runs less than N dB under
 *    the stepped reference (every run the gate keeps shut, and those its hysteresis still holds open, so a carrier
 *    fading toward it refutes it before the gate shuts) with a voice band clearly louder than noise's (by twice those
 *    tolerances: a carrier relaying noise that matched noise's spectrum, now carrying speech; a carrier the stale rule
 *    took, now modulated), or more than 1.5 dB above it without noise's spectrum (no carrier reads louder than noise
 *    at its gain, and noise turned back up keeps its spectrum), refute the steps: the old reference comes back. The
 *    stretch then steps down again only on 0.6 s of evidence with no such modulation in it, doubled for each further
 *    refutation in the stretch. While a step is held, only a stretch with noise's spectrum replaces the reference or
 *    is tracked.
 *  - The step refuted is kept too (and cached), until a new reference replaces it: 0.8 s of runs in a row within
 *    1.5 dB of its level, three in four with its spectrum against it and the voice band steady, take it back
 *    (modulation against it starts over), the reference it replaces is held as its prior, and the stretch's
 *    refutations end. A carrier's own shorter pauses leave the step refuted. Weak traffic over noise genuinely turned
 *    down refutes a step just as a stronger carrier over relayed noise taken for noise does, and nothing tells the two
 *    apart: the traffic plays, and the noise after it takes the step back within 1 s.
 *  - A reference a refutation brought back is proven, and a step held over it (taken back, or stepped down again) is
 *    contested: eight of ten runs less than N dB under it with a voice band louder than noise's by more than the
 *    shape tolerances alone, and not steady, refute it too (a carrier that relayed noise, carrying speech more lightly
 *    than before). A filter switched on in noise changes its spectrum steadily, and refutes nothing.
 *  - A steady run at or above the reference less 1.5 dB tracks it with a 1 s time constant (slow drift).
 *  - Stale quieting: the gate open for 5 s on one stretch whose voice band held stationary in 90 % of its windows reads
 *    a level that dropped (a source's volume lowered out of clipping, its audio low-pass switched on), not speech: the
 *    stretch becomes the reference, as a held step (above). A dead or steady-tone carrier is muted after it too, until
 *    it shows modulation.
 *  - Participation: a sub-band takes part when its power per Hz sits within 30 dB of the voice band's (the reference's
 *    for Q; the run's for the no-band test); a staggered band-pass when both sub-bands it straddles do. A reference
 *    with under 1200 Hz taking part does not gate (closed).
 *  - NO_BAND: 1 s of runs at the reference (or before one), the voice band stationary, with under 1200 Hz of the band
 *    taking part: the source low-passed its audio. The squelch is then unavailable (off). 1 s of steady runs with a
 *    band and a stationary voice band leaves it again (PROVISIONAL).
 *
 * Quieting and gate: Q = max(Q_sum, Q_max - 6 dB) against the reference, over the band-passes taking part, Q_k = 10
 * log10(r_k / p_k). The gate opens at a window whose Q reaches N and closes at one under max(N - 3, 1.5) dB. Each
 * sample's flag is the gate as it stood when the sample arrived: a decision applies from the sample after the window
 * that took it.
 *
 * References are per source context (dsd_pcm_noise_squelch_key): a new source, native rate or input volume multiplier
 * forgets them all; another rigctl passband stores the current one and takes that passband's (a cache of
 * DSD_PCM_NOISE_SQUELCH_CACHE_SIZE), or learns. An unknown passband is never stored.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_PCM_NOISE_SQUELCH_H_
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_PCM_NOISE_SQUELCH_H_

#include <dsd-neo/dsp/nfm_noise_squelch.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /** Most sub-bands a plan cuts its band into (K). */
    DSD_PCM_NOISE_SQUELCH_MAX_SUB_BANDS = 16,
    /** Sets of band-passes: the K sub-bands and SETS - 1 staggered sets of K - 1. */
    DSD_PCM_NOISE_SQUELCH_SETS = 3,
    /** Most band-passes a plan runs. */
    DSD_PCM_NOISE_SQUELCH_MAX_BANDS = DSD_PCM_NOISE_SQUELCH_MAX_SUB_BANDS
        + ((DSD_PCM_NOISE_SQUELCH_SETS - 1) * (DSD_PCM_NOISE_SQUELCH_MAX_SUB_BANDS - 1)),
    /** Windows a steady run spans (M). */
    DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS = 4,
    /** Source contexts whose references are remembered. */
    DSD_PCM_NOISE_SQUELCH_CACHE_SIZE = 8,
    /** A sub-band's width, Hz (about: the band is cut evenly). */
    DSD_PCM_NOISE_SQUELCH_SUB_BAND_HZ = 200,
    /** Parts of the voice band the learner compares with the reference's. */
    DSD_PCM_NOISE_SQUELCH_VOICE_PARTS = 4,
};

/** A passband no reference is stored for (the rigctl peer's state is not known). */
#define DSD_PCM_NOISE_SQUELCH_PASSBAND_UNKNOWN INT32_MIN

/** @brief What the squelch knows (dsd_pcm_noise_squelch_status::state). */
typedef enum {
    DSD_PCM_NOISE_SQUELCH_LEARNING = 0,    /**< no reference yet: gate closed */
    DSD_PCM_NOISE_SQUELCH_PROVISIONAL = 1, /**< a reference from the first steady run */
    DSD_PCM_NOISE_SQUELCH_KNOWN = 2,       /**< a reference a transition confirmed as noise */
    DSD_PCM_NOISE_SQUELCH_NO_BAND = 3,     /**< nothing above voice to measure: unavailable */
    DSD_PCM_NOISE_SQUELCH_NO_ROOM = 4,     /**< the native rate leaves no band (or no plan): unavailable */
} dsd_pcm_noise_squelch_state;

/** @brief The band-passes for one monitor rate and native rate. A zeroed plan is not valid (NO_ROOM). */
typedef struct {
    int valid;
    int rate_hz;        /**< the rate the samples arrive at */
    int native_rate_hz; /**< the source's own rate (bounds the band) */
    double lo_hz;       /**< the band's edges */
    double hi_hz;
    double step_hz;    /**< a sub-band's width */
    int sub_bands;     /**< K */
    int bands;         /**< K + (SETS - 1)(K - 1): the sub-bands (indices 0..K-1), then the staggered sets */
    double band_bw_db; /**< 10 log10(step_hz) */
    double voice_bw_db;
    dsd_noise_squelch_biquad section[DSD_PCM_NOISE_SQUELCH_MAX_BANDS][DSD_NOISE_SQUELCH_SECTIONS];
    dsd_noise_squelch_biquad voice[DSD_NOISE_SQUELCH_SECTIONS];
    dsd_noise_squelch_biquad voice_part[DSD_PCM_NOISE_SQUELCH_VOICE_PARTS][DSD_NOISE_SQUELCH_SECTIONS];
} dsd_pcm_noise_squelch_plan;

/** @brief The source context references belong to. */
typedef struct {
    uint32_t source;        /**< the PCM stream generation: a new one forgets every reference */
    int32_t native_rate_hz; /**< the source's own rate */
    int32_t volume;         /**< the input volume multiplier */
    int32_t passband_hz;    /**< the rigctl peer's passband (0: its own), or DSD_PCM_NOISE_SQUELCH_PASSBAND_UNKNOWN */
} dsd_pcm_noise_squelch_key;

/** @brief A reference held beside the current one: the one from before a downward step, or a step a refutation
 * undid. */
typedef struct {
    int valid;
    double ref[DSD_PCM_NOISE_SQUELCH_MAX_BANDS];
    double v_ref_db;
    double vp_ref_db[DSD_PCM_NOISE_SQUELCH_VOICE_PARTS];
    int proven; /**< one a refutation brought back: a step held over it is contested */
} dsd_pcm_noise_squelch_held;

/** @brief A remembered reference. */
typedef struct {
    int used;
    int32_t passband_hz;
    int state;
    double ref[DSD_PCM_NOISE_SQUELCH_MAX_BANDS];
    double v_ref_db;
    double vp_ref_db[DSD_PCM_NOISE_SQUELCH_VOICE_PARTS];
    dsd_pcm_noise_squelch_held prior; /**< a downward step held: the reference from before it */
    dsd_pcm_noise_squelch_held alt;   /**< a step a refutation undid */
    int ref_proven;                   /**< the reference is one a refutation brought back */
    uint64_t stamp;                   /**< LRU */
} dsd_pcm_noise_squelch_cache_entry;

/** @brief What the squelch publishes. */
typedef struct {
    int state;          /**< dsd_pcm_noise_squelch_state */
    int available;      /**< 1 in LEARNING, PROVISIONAL or KNOWN: the squelch gates */
    int gate_open;      /**< 1 open (and 1 when unavailable) */
    int quieting_valid; /**< 1 once a window was read against a reference */
    double quieting_db; /**< the last such window's Q */
    double usable_hz;   /**< the reference's band taking part; 0 without one */
    uint64_t windows;   /**< windows evaluated since the last restart */
} dsd_pcm_noise_squelch_status;

/** @brief The squelch. Plain data: a zeroed one has no plan (NO_ROOM, unavailable). */
typedef struct {
    dsd_pcm_noise_squelch_plan plan;
    dsd_pcm_noise_squelch_key key;
    int have_key;
    int threshold_db;
    double open_db;
    double close_db;
    /* Filter state (transposed direct form II). */
    double s1[DSD_PCM_NOISE_SQUELCH_MAX_BANDS][DSD_NOISE_SQUELCH_SECTIONS];
    double s2[DSD_PCM_NOISE_SQUELCH_MAX_BANDS][DSD_NOISE_SQUELCH_SECTIONS];
    double v1[DSD_NOISE_SQUELCH_SECTIONS];
    double v2[DSD_NOISE_SQUELCH_SECTIONS];
    double vp1[DSD_PCM_NOISE_SQUELCH_VOICE_PARTS][DSD_NOISE_SQUELCH_SECTIONS];
    double vp2[DSD_PCM_NOISE_SQUELCH_VOICE_PARTS][DSD_NOISE_SQUELCH_SECTIONS];
    /* Sample-exact half-window boundaries since the last restart. */
    uint64_t samples;
    uint64_t half_index;
    uint64_t half_end;
    /* The open half and the one before it: per band-pass energy, voice and voice-part energy, input energy and sample
       count. */
    double half_e[DSD_PCM_NOISE_SQUELCH_MAX_BANDS];
    double prev_e[DSD_PCM_NOISE_SQUELCH_MAX_BANDS];
    double half_v, prev_v;
    double half_vp[DSD_PCM_NOISE_SQUELCH_VOICE_PARTS];
    double prev_vp[DSD_PCM_NOISE_SQUELCH_VOICE_PARTS];
    double half_x, prev_x;
    double half_n, prev_n;
    int have_prev;
    /* The last RUN_WINDOWS windows: band powers, voice and voice-part powers and their dB, newest at run_pos - 1. */
    double run_p[DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS][DSD_PCM_NOISE_SQUELCH_MAX_BANDS];
    double run_v[DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS];
    double run_vp[DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS][DSD_PCM_NOISE_SQUELCH_VOICE_PARTS];
    double run_a_db[DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS];
    double run_v_db[DSD_PCM_NOISE_SQUELCH_RUN_WINDOWS];
    int run_pos;
    int run_len; /**< windows since the last silent window or restart */
    /* The reference. */
    int state;
    double ref[DSD_PCM_NOISE_SQUELCH_MAX_BANDS];
    double v_ref_db;
    double vp_ref_db[DSD_PCM_NOISE_SQUELCH_VOICE_PARTS];
    unsigned char part[DSD_PCM_NOISE_SQUELCH_MAX_BANDS];
    double usable_hz;
    /* The reference from before the first downward step still held, until a new reference replaces it (a carrier under
       the gate's threshold at the stepped reference refutes the steps and brings it back), and the step a refutation
       undid, until noise at its level takes it back or a new reference replaces it. */
    dsd_pcm_noise_squelch_held prior;
    dsd_pcm_noise_squelch_held alt;
    int ref_proven; /**< the reference is one a refutation brought back */
    /* The stretch: anchor level, voice power sum, windows, voice-steady windows, whether it began at the reference, a
       pending gain step (its runs, those with noise's spectrum and their step sum), the gate's open windows on it, the
       evidence for noise at a lower gain (its level, runs, those with noise's spectrum and their step sum), the last
       runs at the stepped reference (a bit each, set for a carrier's), the steps refuted in the stretch, and the runs
       at a refuted step's level (those with its spectrum). */
    int have_stretch;
    double st_a_db;
    double st_v_sum;
    int st_n;
    int st_vs;
    int st_at_ref;
    int st_pending;
    int st_pn;
    int st_ps;
    double st_psum;
    int st_on;
    int st_os;
    int st_acc;
    double st_acc_db;
    int st_an;
    int st_as;
    double st_asum;
    unsigned int st_ring;
    int st_ring_n;
    int st_refutes;
    int st_bn;
    int st_bs;
    /* A new level being confirmed, no-band evidence and its exit. */
    int have_cand;
    double cand_a_db;
    int cand_n;
    int nb;
    int exit_n;
    /* Gate and readout. */
    int gate_open;
    int quieting_valid;
    double quieting_db;
    uint64_t windows;
    /* Remembered references. */
    dsd_pcm_noise_squelch_cache_entry cache[DSD_PCM_NOISE_SQUELCH_CACHE_SIZE];
    uint64_t cache_clock;
} dsd_pcm_noise_squelch;

/**
 * @brief Design the plan for samples at @p rate_hz from a source at @p native_rate_hz: the band, its band-passes
 * (sub-bands and staggered sets) and the voice band-passes.
 * @return 0, or -1 (plan not valid: NO_ROOM) when less than 1200 Hz of band fits under 0.45 of the native rate.
 */
int dsd_pcm_noise_squelch_plan_design(dsd_pcm_noise_squelch_plan* out, int rate_hz, int native_rate_hz);

/** @brief Whether @p a and @p b are the same plan (rates, band and band-passes). */
int dsd_pcm_noise_squelch_plan_equal(const dsd_pcm_noise_squelch_plan* a, const dsd_pcm_noise_squelch_plan* b);

/** @brief Take @p plan; another plan forgets every reference (dsd_pcm_noise_squelch_forget()). */
void dsd_pcm_noise_squelch_set_plan(dsd_pcm_noise_squelch* t, const dsd_pcm_noise_squelch_plan* plan);

/** @brief Open at @p threshold_db (clamped to 3..30) of quieting; close below max(N - 3, 1.5) dB. */
void dsd_pcm_noise_squelch_set_threshold(dsd_pcm_noise_squelch* t, int threshold_db);

/**
 * @brief Move to source context @p key. Another source, native rate or volume forgets every reference; another
 * passband stores the current reference under the old one (unless unknown) and takes the new one's, or learns. Either
 * restarts the windows. No-op for the same context.
 */
void dsd_pcm_noise_squelch_set_key(dsd_pcm_noise_squelch* t, const dsd_pcm_noise_squelch_key* key);

/** @brief Start the windows over (a retune, a stream pause, the squelch turned on): gate closed until the first window
 * decides; the reference and what it knows stay. */
void dsd_pcm_noise_squelch_restart(dsd_pcm_noise_squelch* t);

/** @brief Forget every reference and the cache: LEARNING (or NO_ROOM without a plan), windows restarted. */
void dsd_pcm_noise_squelch_forget(dsd_pcm_noise_squelch* t);

/**
 * @brief Run @p count samples (@p pcm, int16 scale) through the band-passes, the windows, the learner and the gate,
 * writing each sample's flag (DSD_SQUELCH_FLAG_CLOSED when the gate was closed and the squelch available) to
 * @p flags (may be NULL).
 */
void dsd_pcm_noise_squelch_process(dsd_pcm_noise_squelch* t, const float* pcm, int count, uint8_t* flags);

/** @brief What @p t publishes. */
void dsd_pcm_noise_squelch_get_status(const dsd_pcm_noise_squelch* t, dsd_pcm_noise_squelch_status* out);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_PCM_NOISE_SQUELCH_H_ */
