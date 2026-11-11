// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/sps_filters.h>
#include <math.h>
#include <stddef.h>
#include "dsd-neo/core/safe_api.h"
#include "m17_rrc_taps.h"

#define FIR_MAX_TAPS          DSD_SPS_FILTER_MAX_TAPS
#define SPS_FIR_DESIGN_INTERP 0
#define SPS_FIR_DESIGN_RRC    1

typedef struct {
    const float* base; /* design taps at base_sps */
    int base_len;
    int base_sps;
    int design_kind; /* SPS_FIR_DESIGN_* */
    float rrc_alpha; /* used when design_kind == SPS_FIR_DESIGN_RRC */
    float taps[FIR_MAX_TAPS];
    /* Two copies of the ring make every oldest-to-newest window contiguous. */
    float hist[2 * FIR_MAX_TAPS];
    int taps_len;
    int head;
    int last_sps;
    int ready;
} sps_fir;

static inline void
reset_sps_fir(sps_fir* f) {
    if (!f) {
        return;
    }
    DSD_MEMSET(f->taps, 0, sizeof(f->taps));
    DSD_MEMSET(f->hist, 0, sizeof(f->hist));
    f->taps_len = 0;
    f->head = -1;
    f->last_sps = 0;
    f->ready = 0;
}

static float
interp_base(const float* base, int len, float idx) {
    if (idx < 0.0f || idx > (float)(len - 1)) {
        return 0.0f;
    }
    int i0 = (int)idx;
    int i1 = i0 + 1;
    if (i1 >= len) {
        i1 = len - 1;
    }
    float frac = idx - (float)i0;
    return base[i0] + frac * (base[i1] - base[i0]);
}

/*
 * Root-raised-cosine impulse response h(t), T=1 symbol.
 * Matches the standard closed-form expression, including singularities at
 * t = 0 and t = ±1/(4*alpha).
 */
static float
rrc_impulse(float t_sym, float alpha) {
    const float pi = 3.14159265358979323846f;
    const float eps = 1e-6f;
    if (alpha <= 0.0f || alpha > 1.0f) {
        if (fabsf(t_sym) < eps) {
            return 1.0f;
        }
        float x = pi * t_sym;
        return sinf(x) / x;
    }

    if (fabsf(t_sym) < eps) {
        return 1.0f + alpha * ((4.0f / pi) - 1.0f);
    }

    float four_a_t = 4.0f * alpha * t_sym;
    if (fabsf(fabsf(four_a_t) - 1.0f) < 1e-4f) {
        float a = pi / (4.0f * alpha);
        float t1 = (1.0f + (2.0f / pi)) * sinf(a);
        float t2 = (1.0f - (2.0f / pi)) * cosf(a);
        return (alpha / 1.41421356237309504880f) * (t1 + t2);
    }

    float num = sinf(pi * t_sym * (1.0f - alpha)) + (four_a_t * cosf(pi * t_sym * (1.0f + alpha)));
    float den = pi * t_sym * (1.0f - (four_a_t * four_a_t));
    if (fabsf(den) < eps) {
        return 0.0f;
    }
    return num / den;
}

static int
sps_fir_compute_taps_len(const sps_fir* f, int sps) {
    int taps_len = 0;
    if (sps == f->base_sps && f->base_len <= FIR_MAX_TAPS) {
        /* Exact base design when no SPS change is requested. */
        taps_len = f->base_len;
    } else {
        double span = (double)(f->base_len - 1) / (double)f->base_sps;
        double desired = span * (double)sps;
        taps_len = (int)(desired + 0.5) + 1; /* preserve span + center tap */
    }
    if (taps_len < 3) {
        taps_len = 3;
    }
    if ((taps_len & 1) == 0) {
        taps_len += 1; /* prefer odd length for symmetry */
    }
    if (taps_len > FIR_MAX_TAPS) {
        /* Clamp to max and force odd tap count for symmetric center tap. */
        taps_len = FIR_MAX_TAPS - 1;
    }
    return taps_len;
}

static void
sps_fir_design_taps(sps_fir* f, int sps, int taps_len) {
    if (sps == f->base_sps && taps_len == f->base_len) {
        DSD_MEMCPY(f->taps, f->base, (size_t)taps_len * sizeof(float));
    } else {
        float mid_new = 0.5f * (float)(taps_len - 1);
        float mid_base = 0.5f * (float)(f->base_len - 1);
        for (int n = 0; n < taps_len; n++) {
            float t_sym = ((float)n - mid_new) / (float)sps;
            if (f->design_kind == SPS_FIR_DESIGN_RRC) {
                f->taps[n] = rrc_impulse(t_sym, f->rrc_alpha);
            } else {
                float base_idx = t_sym * (float)f->base_sps + mid_base;
                f->taps[n] = interp_base(f->base, f->base_len, base_idx);
            }
        }
    }
}

static void
sps_fir_normalize_and_clear(sps_fir* f, int taps_len) {
    double sum = 0.0;
    for (int n = 0; n < taps_len; n++) {
        sum += f->taps[n];
    }
    if (fabs(sum) < 1e-12) {
        sum = 1.0;
    }
    float inv_sum = (float)(1.0 / sum);
    for (int n = 0; n < taps_len; n++) {
        f->taps[n] *= inv_sum;
    }
    DSD_MEMSET(f->hist, 0, sizeof(f->hist));
}

static void
design_sps_fir(sps_fir* f, int sps) {
    if (!f || !f->base || f->base_len <= 0 || f->base_sps <= 0 || sps <= 1) {
        if (f) {
            f->ready = 0;
        }
        return;
    }

    int taps_len = sps_fir_compute_taps_len(f, sps);
    sps_fir_design_taps(f, sps, taps_len);
    sps_fir_normalize_and_clear(f, taps_len);

    f->taps_len = taps_len;
    f->head = -1;
    f->last_sps = sps;
    f->ready = 1;
}

static void
sps_fir_push(sps_fir* f, float sample) {
    int head = f->head + 1;
    if (head >= f->taps_len) {
        head = 0;
    }
    f->hist[head] = sample;
    f->hist[head + f->taps_len] = sample;
    f->head = head;
}

static float
apply_sps_fir(sps_fir* f, float sample, int sps) {
    if (!f || sps <= 1) {
        return sample;
    }
    if (!f->ready || sps != f->last_sps) {
        design_sps_fir(f, sps);
    }
    if (!f->ready || f->taps_len <= 0) {
        return sample;
    }

    sps_fir_push(f, sample);
    const float* history = f->hist + f->head + 1;
    float acc = 0.0f;
    for (int i = 0; i < f->taps_len; i++) {
        acc += f->taps[i] * history[i];
    }
    return acc;
}

// M17 Filter -- RRC Alpha = 0.5 at 48 kHz (sps=10)
const float dsd_m17_rrc_48khz_taps[DSD_M17_RRC_48KHZ_TAP_COUNT] = {
    -0.003195702904062073f, -0.002930279157647190f, -0.001940667871554463f, -0.000356087678023658f,
    0.001547011339077758f,  0.003389554791179751f,  0.004761898604225673f,  0.005310860846138910f,
    0.004824746306020221f,  0.003297923526848786f,  0.000958710871218619f,  -0.001749908029791816f,
    -0.004238694106631223f, -0.005881783042101693f, -0.006150256456781309f, -0.004745376707651645f,
    -0.001704189656473565f, 0.002547854551539951f,  0.007215575568844704f,  0.011231038205363532f,
    0.013421952197060707f,  0.012730475385624438f,  0.008449554307303753f,  0.000436744366018287f,
    -0.010735380379191660f, -0.023726883538258272f, -0.036498030780605324f, -0.046500883189991064f,
    -0.050979050575999614f, -0.047340680079891187f, -0.033554880492651755f, -0.008513823955725943f,
    0.027696543159614194f,  0.073664520037517042f,  0.126689053778116234f,  0.182990955139333916f,
    0.238080025892859704f,  0.287235637987091563f,  0.326040247765297220f,  0.350895727088112619f,
    0.359452932027607974f,  0.350895727088112619f,  0.326040247765297220f,  0.287235637987091563f,
    0.238080025892859704f,  0.182990955139333916f,  0.126689053778116234f,  0.073664520037517042f,
    0.027696543159614194f,  -0.008513823955725943f, -0.033554880492651755f, -0.047340680079891187f,
    -0.050979050575999614f, -0.046500883189991064f, -0.036498030780605324f, -0.023726883538258272f,
    -0.010735380379191660f, 0.000436744366018287f,  0.008449554307303753f,  0.012730475385624438f,
    0.013421952197060707f,  0.011231038205363532f,  0.007215575568844704f,  0.002547854551539951f,
    -0.001704189656473565f, -0.004745376707651645f, -0.006150256456781309f, -0.005881783042101693f,
    -0.004238694106631223f, -0.001749908029791816f, 0.000958710871218619f,  0.003297923526848786f,
    0.004824746306020221f,  0.005310860846138910f,  0.004761898604225673f,  0.003389554791179751f,
    0.001547011339077758f,  -0.000356087678023658f, -0.001940667871554463f, -0.002930279157647190f,
    -0.003195702904062073f};

/*
 * DMR and YSF send root-raised-cosine pulses of roll-off 0.2 at 4800 symbol/s: ETSI TS 102 361-1 clause 10.2.2.2 splits
 * a Nyquist raised cosine between the transmitter and "the receiver to reject noise", and MMDVM receives DMR and YSF
 * through the same RRC 0.2 (IO.cpp, rcosdesign(0.2, 8, 5, 'sqrt')). This is that receive half at 48 kHz (sps 10),
 * four symbols either side, sampled from rrc_impulse(); other rates get the same design. Three symbols either side
 * cut its sidelobes short enough to lose bursts on an off-air recording that four decode. The table it replaces was
 * an RRC 0.2 for about 7.2 samples per symbol, a low-pass flat to 3 kHz that passed the discriminator's noise up to
 * 3.3 kHz and cost an ideal DMR signal about 1.5 dB (#834).
 */
static const float dmr_rrc_taps[81] = {
    0.0273581115f,  0.0190617252f,  0.0070635427f,  -0.0075369306f, -0.0231679822f, -0.0379343591f, -0.0498223859f,
    -0.0569413909f, -0.0577750618f, -0.0514128374f, -0.0377312525f, -0.0174985561f, 0.0076172472f,  0.0351470884f,
    0.0620248049f,  0.0848826363f,  0.1004128792f,  0.1057607842f,  0.0989075145f,  0.0789999118f,  0.0465864194f,
    0.0037257690f,  -0.0460535952f, -0.0979509333f, -0.1462389372f, -0.1847331396f, -0.2073460175f, -0.2086758164f,
    -0.1845738160f, -0.1326330018f, -0.0525461921f, 0.0537077188f,  0.1818755267f,  0.3256472245f,  0.4770672269f,
    0.6271081565f,  0.7663595022f,  0.8857712693f,  0.9773862095f,  1.0349940969f,  1.0546479089f,  1.0349940969f,
    0.9773862095f,  0.8857712693f,  0.7663595022f,  0.6271081565f,  0.4770672269f,  0.3256472245f,  0.1818755267f,
    0.0537077188f,  -0.0525461921f, -0.1326330018f, -0.1845738160f, -0.2086758164f, -0.2073460175f, -0.1847331396f,
    -0.1462389372f, -0.0979509333f, -0.0460535952f, 0.0037257690f,  0.0465864194f,  0.0789999118f,  0.0989075145f,
    0.1057607842f,  0.1004128792f,  0.0848826363f,  0.0620248049f,  0.0351470884f,  0.0076172472f,  -0.0174985561f,
    -0.0377312525f, -0.0514128374f, -0.0577750618f, -0.0569413909f, -0.0498223859f, -0.0379343591f, -0.0231679822f,
    -0.0075369306f, 0.0070635427f,  0.0190617252f,  0.0273581115f};
#define DMR_BASE_SPS     10
#define DMR_BASE_TAP_LEN (int)(sizeof(dmr_rrc_taps) / sizeof(dmr_rrc_taps[0]))

/*
 * NXDN and dPMR send a square root raised cosine of roll-off 0.2 times the sinc of a symbol held for its period, and
 * receive through the same square root raised cosine times the inverse sinc pi f T / sin(pi f T), for |f| below
 * (1 + 0.2) / 2T: NXDN TS 1-A version 1.3 clauses 3.4 and 3.5 (H(f) P(f) out, H(f) D(f) in), ETSI TS 102 658 clause
 * 14.2.2.2 (the same H(f) D(f) receive filter at 2400 symbol/s); MMDVM receives NXDN through RRC 0.2 then an inverse
 * sinc (IO.cpp). The overall response is the Nyquist raised cosine. This is that receive filter at 48 kHz for 2400
 * symbol/s (sps 20, NXDN48 and dPMR), four symbols either side (3.35 lost frames on an off-air NXDN96 recording),
 * from the inverse Fourier transform of H(f) D(f); NXDN96 at 4800 symbol/s and other rates read the same pulse scaled
 * in symbols. The tables it replaces were RRC 0.2 pulses for 16 (NXDN) and about 14.5 (dPMR) samples per symbol, a
 * quarter or more too wide in frequency and without the inverse sinc, and NXDN96 ran DMR's.
 */
static const float nxdn_dpmr_rx_taps[161] = {
    0.0437529310f,  0.0404354274f,  0.0355881155f,  0.0292797519f,  0.0216296211f,  0.0128062578f,  0.0030245643f,
    -0.0074586521f, -0.0183506526f, -0.0293301297f, -0.0400557916f, -0.0501759572f, -0.0593389108f, -0.0672037344f,
    -0.0734513084f, -0.0777951567f, -0.0799918025f, -0.0798503048f, -0.0772406558f, -0.0721007426f, -0.0644416064f,
    -0.0543507738f, -0.0419934821f, -0.0276116751f, -0.0115207072f, 0.0058962439f,  0.0241959867f,  0.0428852170f,
    0.0614323087f,  0.0792807389f,  0.0958638297f,  0.1106204454f,  0.1230112429f,  0.1325350427f,  0.1387448724f,
    0.1412632256f,  0.1397960892f,  0.1341453076f,  0.1242188875f,  0.1100388881f,  0.0917465984f,  0.0696047647f,
    0.0439967075f,  0.0154222422f,  -0.0155095961f, -0.0480909394f, -0.0815284160f, -0.1149591698f, -0.1474692399f,
    -0.1781138996f, -0.2059394935f, -0.2300062585f, -0.2494115793f, -0.2633131009f, -0.2709511151f, -0.2716696401f,
    -0.2649356335f, -0.2503558180f, -0.2276906445f, -0.1968649838f, -0.1579752118f, -0.1112924392f, -0.0572617268f,
    0.0035027722f,  0.0702267003f,  0.1419882739f,  0.2177347066f,  0.2963019453f,  0.3764372948f,  0.4568244330f,
    0.5361102547f,  0.6129329271f,  0.6859505106f,  0.7538694718f,  0.8154724181f,  0.8696443952f,  0.9153971192f,
    0.9518905627f,  0.9784513760f,  0.9945876981f,  1.0000000000f,  0.9945876981f,  0.9784513760f,  0.9518905627f,
    0.9153971192f,  0.8696443952f,  0.8154724181f,  0.7538694718f,  0.6859505106f,  0.6129329271f,  0.5361102547f,
    0.4568244330f,  0.3764372948f,  0.2963019453f,  0.2177347066f,  0.1419882739f,  0.0702267003f,  0.0035027722f,
    -0.0572617268f, -0.1112924392f, -0.1579752118f, -0.1968649838f, -0.2276906445f, -0.2503558180f, -0.2649356335f,
    -0.2716696401f, -0.2709511151f, -0.2633131009f, -0.2494115793f, -0.2300062585f, -0.2059394935f, -0.1781138996f,
    -0.1474692399f, -0.1149591698f, -0.0815284160f, -0.0480909394f, -0.0155095961f, 0.0154222422f,  0.0439967075f,
    0.0696047647f,  0.0917465984f,  0.1100388881f,  0.1242188875f,  0.1341453076f,  0.1397960892f,  0.1412632256f,
    0.1387448724f,  0.1325350427f,  0.1230112429f,  0.1106204454f,  0.0958638297f,  0.0792807389f,  0.0614323087f,
    0.0428852170f,  0.0241959867f,  0.0058962439f,  -0.0115207072f, -0.0276116751f, -0.0419934821f, -0.0543507738f,
    -0.0644416064f, -0.0721007426f, -0.0772406558f, -0.0798503048f, -0.0799918025f, -0.0777951567f, -0.0734513084f,
    -0.0672037344f, -0.0593389108f, -0.0501759572f, -0.0400557916f, -0.0293301297f, -0.0183506526f, -0.0074586521f,
    0.0030245643f,  0.0128062578f,  0.0216296211f,  0.0292797519f,  0.0355881155f,  0.0404354274f,  0.0437529310f};
#define NXDN_DPMR_BASE_SPS     20
#define NXDN_DPMR_BASE_TAP_LEN (int)(sizeof(nxdn_dpmr_rx_taps) / sizeof(nxdn_dpmr_rx_taps[0]))

// P25 C4FM RX de-emphasis filter - OP25 compatible (transfer_function_rx)
// Sinc-based filter that inverts the C4FM transmitter's preemphasis (t/sin(t)).
// Parameters: sample_rate=48000, symbol_rate=4800, span=9, sps=10
// Generated using IFFT of sinc(pi*f/rate) transfer function
// See: https://github.com/boatbod/op25/blob/master/op25/gr-op25_repeater/apps/tx/op25_c4fm_mod.py
#define P25_BASE_SPS           10
static const float p25_base_coeffs[91] = {
    2.5136032328468223e-04f,  2.0151157806489389e-04f,  6.5614198114868653e-05f,  -1.1016182521584872e-04f,
    -2.5912718500147703e-04f, -3.1854080534864679e-04f, -2.5499192412346209e-04f, -8.0811341618403253e-05f,
    1.4587749827445741e-04f,  3.3924177267442553e-04f,  4.1684342104421064e-04f,  3.3300701153002625e-04f,
    1.0159288265298235e-04f,  -2.0205207165260435e-04f, -4.6331126549667987e-04f, -5.6903817400881962e-04f,
    -4.5325595471553667e-04f, -1.3054623551357765e-04f, 2.9771613892566937e-04f,  6.7070306752558281e-04f,
    8.2344079518134800e-04f,  6.5286953790319577e-04f,  1.7063335852203476e-04f,  -4.8022995636450586e-04f,
    -1.0576277820236342e-03f, -1.2984272199126820e-03f, -1.0210516145569011e-03f, -2.1847321285187830e-04f,
    8.9570866257743969e-04f,  1.9159656053379041e-03f,  2.3554729602435079e-03f,  1.8191510018527107e-03f,
    1.9485965513348435e-04f,  -2.1959672874350021e-03f, -4.5451112342681493e-03f, -5.6388787968222262e-03f,
    -4.0771412352218865e-03f, 1.4061759104066391e-03f,  1.1591443394554960e-02f,  2.6478859157413236e-02f,
    4.5134880973750763e-02f,  6.5735325813782261e-02f,  8.5815567034273302e-02f,  1.0268680032016260e-01f,
    1.1393334551393806e-01f,  1.1788155507005367e-01f,  1.1393334551393806e-01f,  1.0268680032016260e-01f,
    8.5815567034273302e-02f,  6.5735325813782275e-02f,  4.5134880973750756e-02f,  2.6478859157413232e-02f,
    1.1591443394554955e-02f,  1.4061759104066391e-03f,  -4.0771412352218891e-03f, -5.6388787968222288e-03f,
    -4.5451112342681519e-03f, -2.1959672874350068e-03f, 1.9485965513348224e-04f,  1.8191510018527107e-03f,
    2.3554729602435071e-03f,  1.9159656053379041e-03f,  8.9570866257743925e-04f,  -2.1847321285187735e-04f,
    -1.0210516145569026e-03f, -1.2984272199126855e-03f, -1.0576277820236353e-03f, -4.8022995636450938e-04f,
    1.7063335852203265e-04f,  6.5286953790319588e-04f,  8.2344079518134519e-04f,  6.7070306752557924e-04f,
    2.9771613892566959e-04f,  -1.3054623551357652e-04f, -4.5325595471553477e-04f, -5.6903817400881995e-04f,
    -4.6331126549668057e-04f, -2.0205207165260435e-04f, 1.0159288265298099e-04f,  3.3300701153002587e-04f,
    4.1684342104421167e-04f,  3.3924177267442786e-04f,  1.4587749827445716e-04f,  -8.0811341618404093e-05f,
    -2.5499192412346220e-04f, -3.1854080534864690e-04f, -2.5912718500147660e-04f, -1.1016182521584800e-04f,
    6.5614198114868558e-05f,  2.0151157806489272e-04f,  2.5136032328468153e-04f};
#define P25_BASE_TAP_LEN (int)(sizeof(p25_base_coeffs) / sizeof(p25_base_coeffs[0]))

static sps_fir g_fir_p25 = {.base = p25_base_coeffs,
                            .base_len = P25_BASE_TAP_LEN,
                            .base_sps = P25_BASE_SPS,
                            .design_kind = SPS_FIR_DESIGN_INTERP};
static sps_fir g_fir_dmr = {.base = dmr_rrc_taps,
                            .base_len = DMR_BASE_TAP_LEN,
                            .base_sps = DMR_BASE_SPS,
                            .design_kind = SPS_FIR_DESIGN_RRC,
                            .rrc_alpha = 0.2f};
static sps_fir g_fir_nxdn = {.base = nxdn_dpmr_rx_taps,
                             .base_len = NXDN_DPMR_BASE_TAP_LEN,
                             .base_sps = NXDN_DPMR_BASE_SPS,
                             .design_kind = SPS_FIR_DESIGN_INTERP};
static sps_fir g_fir_dpmr = {.base = nxdn_dpmr_rx_taps,
                             .base_len = NXDN_DPMR_BASE_TAP_LEN,
                             .base_sps = NXDN_DPMR_BASE_SPS,
                             .design_kind = SPS_FIR_DESIGN_INTERP};
/* M17 sends root-raised-cosine pulses of roll-off 0.5; the receive half is the same filter, so the encoder's transmit
   taps are its matched filter. */
static sps_fir g_fir_m17 = {.base = dsd_m17_rrc_48khz_taps,
                            .base_len = DSD_M17_RRC_48KHZ_TAP_COUNT,
                            .base_sps = 10,
                            .design_kind = SPS_FIR_DESIGN_RRC,
                            .rrc_alpha = 0.5f};

float
dmr_filter(float sample, int samples_per_symbol) {
    return apply_sps_fir(&g_fir_dmr, sample, samples_per_symbol);
}

float
nxdn_filter(float sample, int samples_per_symbol) {
    return apply_sps_fir(&g_fir_nxdn, sample, samples_per_symbol);
}

float
dpmr_filter(float sample, int samples_per_symbol) {
    return apply_sps_fir(&g_fir_dpmr, sample, samples_per_symbol);
}

float
p25_filter(float sample, int samples_per_symbol) {
    return apply_sps_fir(&g_fir_p25, sample, samples_per_symbol);
}

void
init_rrc_filter_memory(void) {
    reset_sps_fir(&g_fir_p25);
    reset_sps_fir(&g_fir_dmr);
    reset_sps_fir(&g_fir_nxdn);
    reset_sps_fir(&g_fir_dpmr);
    reset_sps_fir(&g_fir_m17);
}

/*
 * The symbol grid runs on the unfiltered stream until a sync names a protocol,
 * and on the filtered one afterwards. Those two streams are not the same signal:
 * a symmetric FIR of L taps reports the signal (L-1)/2 samples in the past, so
 * the moment the filter switches on the decoder would start re-reading content
 * it has already consumed. The symbolizer pays that off at the switch (issue
 * #444); what it needs from here is the delay, stated without disturbing a
 * running filter, and a way to hand a filter the history it missed.
 */
static sps_fir*
sps_fir_for_kind(dsd_sps_filter_kind kind) {
    switch (kind) {
        case DSD_SPS_FILTER_DMR: return &g_fir_dmr;
        case DSD_SPS_FILTER_P25: return &g_fir_p25;
        case DSD_SPS_FILTER_NXDN: return &g_fir_nxdn;
        case DSD_SPS_FILTER_DPMR: return &g_fir_dpmr;
        case DSD_SPS_FILTER_M17: return &g_fir_m17;
        case DSD_SPS_FILTER_NONE:
        default: return NULL;
    }
}

static int
sps_fir_can_design(const sps_fir* f, int samples_per_symbol) {
    return f && f->base && f->base_len > 0 && f->base_sps > 0 && samples_per_symbol > 1;
}

float
dsd_sps_filter_apply(dsd_sps_filter_kind kind, float sample, int samples_per_symbol) {
    sps_fir* f = sps_fir_for_kind(kind);
    if (!f) {
        return sample;
    }
    return apply_sps_fir(f, sample, samples_per_symbol);
}

int
dsd_sps_filter_group_delay(dsd_sps_filter_kind kind, int samples_per_symbol) {
    const sps_fir* f = sps_fir_for_kind(kind);
    if (!sps_fir_can_design(f, samples_per_symbol)) {
        return 0;
    }
    /* The same arithmetic the design uses, so the answer matches what apply()
       will run, without designing: that would clear a filter still in use. */
    const int taps_len = sps_fir_compute_taps_len(f, samples_per_symbol);
    return taps_len > 0 ? (taps_len - 1) / 2 : 0;
}

void
dsd_sps_filter_prime(dsd_sps_filter_kind kind, float sample, int samples_per_symbol) {
    sps_fir* f = sps_fir_for_kind(kind);
    if (!sps_fir_can_design(f, samples_per_symbol)) {
        return;
    }
    if (!f->ready || samples_per_symbol != f->last_sps) {
        design_sps_fir(f, samples_per_symbol);
    }
    if (!f->ready || f->taps_len <= 0) {
        return;
    }
    sps_fir_push(f, sample);
}
