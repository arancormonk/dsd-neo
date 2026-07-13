// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/sps_filters.h>
#include <math.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "m17_rrc_taps.h"

#define FIR_MAX_TAPS          1024
#define SPS_FIR_DESIGN_INTERP 0
#define SPS_FIR_DESIGN_RRC    1

typedef struct {
    const float* base; /* design taps at base_sps */
    int base_len;
    int base_sps;
    int design_kind; /* SPS_FIR_DESIGN_* */
    float rrc_alpha; /* used when design_kind == SPS_FIR_DESIGN_RRC */
    float taps[FIR_MAX_TAPS];
    float hist[FIR_MAX_TAPS];
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
        f->hist[n] = 0.0f;
    }
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

    int head = f->head + 1;
    if (head >= f->taps_len) {
        head = 0;
    }
    f->hist[head] = sample;
    f->head = head;

    float acc = 0.0f;
    int zeros = f->taps_len - 1;
    for (int i = 0; i <= zeros; i++) {
        int idx = head - (zeros - i);
        if (idx < 0) {
            idx += f->taps_len;
        }
        acc += f->taps[i] * f->hist[idx];
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
#define M17_BASE_SPS     10
#define M17_BASE_TAP_LEN (int)(sizeof(dsd_m17_rrc_48khz_taps) / sizeof(dsd_m17_rrc_48khz_taps[0]))

// DMR filter F4EXB - root raised cosine alpha=0.7 at 48 kHz (sps=10)
static const float dmrcoeffs[61] = {
    0.0301506278f,  0.0269200615f,  0.0159662432f,  -0.0013114705f, -0.0216605133f, -0.0404938748f, -0.0528141756f,
    -0.0543747957f, -0.0428325003f, -0.0186176083f, 0.0147202645f,  0.0508418571f,  0.0816392577f,  0.0988113688f,
    0.0957187780f,  0.0691512084f,  0.0206194642f,  -0.0431564563f, -0.1107569268f, -0.1675773224f, -0.1981519842f,
    -0.1889130786f, -0.1308939560f, -0.0218608492f, 0.1325685970f,  0.3190962499f,  0.5182530574f,  0.7070497652f,
    0.8623526878f,  0.9644213921f,  1.0000000000f,  0.9644213921f,  0.8623526878f,  0.7070497652f,  0.5182530574f,
    0.3190962499f,  0.1325685970f,  -0.0218608492f, -0.1308939560f, -0.1889130786f, -0.1981519842f, -0.1675773224f,
    -0.1107569268f, -0.0431564563f, 0.0206194642f,  0.0691512084f,  0.0957187780f,  0.0988113688f,  0.0816392577f,
    0.0508418571f,  0.0147202645f,  -0.0186176083f, -0.0428325003f, -0.0543747957f, -0.0528141756f, -0.0404938748f,
    -0.0216605133f, -0.0013114705f, 0.0159662432f,  0.0269200615f,  0.0301506278f};
#define DMR_BASE_SPS     10
#define DMR_BASE_TAP_LEN (int)(sizeof(dmrcoeffs) / sizeof(dmrcoeffs[0]))

// NXDN48/96 filter - original version (designed for 48 kHz, sps=20)
static const float nxcoeffs[135] = {
    +0.031462429f, +0.031747267f, +0.030401148f, +0.027362877f, +0.022653298f, +0.016379869f, +0.008737200f,
    +0.000003302f, -0.009468531f, -0.019262057f, -0.028914291f, -0.037935027f, -0.045828927f, -0.052119261f,
    -0.056372283f, -0.058221106f, -0.057387924f, -0.053703443f, -0.047122444f, -0.037734535f, -0.025769308f,
    -0.011595336f, +0.004287292f, +0.021260954f, +0.038610717f, +0.055550276f, +0.071252765f, +0.084885375f,
    +0.095646450f, +0.102803611f, +0.105731303f, +0.103946126f, +0.097138329f, +0.085197939f, +0.068234131f,
    +0.046586711f, +0.020828821f, -0.008239664f, -0.039608255f, -0.072081234f, -0.104311776f, -0.134843790f,
    -0.162160200f, -0.184736015f, -0.201094346f, -0.209863285f, -0.209831516f, -0.200000470f, -0.179630919f,
    -0.148282051f, -0.105841323f, -0.052543664f, +0.011020985f, +0.083912428f, +0.164857408f, +0.252278939f,
    +0.344336996f, +0.438979335f, +0.534000832f, +0.627109358f, +0.715995947f, +0.798406824f, +0.872214756f,
    +0.935487176f, +0.986548646f, +1.024035395f, +1.046939951f, +1.054644241f, +1.046939951f, +1.024035395f,
    +0.986548646f, +0.935487176f, +0.872214756f, +0.798406824f, +0.715995947f, +0.627109358f, +0.534000832f,
    +0.438979335f, +0.344336996f, +0.252278939f, +0.164857408f, +0.083912428f, +0.011020985f, -0.052543664f,
    -0.105841323f, -0.148282051f, -0.179630919f, -0.200000470f, -0.209831516f, -0.209863285f, -0.201094346f,
    -0.184736015f, -0.162160200f, -0.134843790f, -0.104311776f, -0.072081234f, -0.039608255f, -0.008239664f,
    +0.020828821f, +0.046586711f, +0.068234131f, +0.085197939f, +0.097138329f, +0.103946126f, +0.105731303f,
    +0.102803611f, +0.095646450f, +0.084885375f, +0.071252765f, +0.055550276f, +0.038610717f, +0.021260954f,
    +0.004287292f, -0.011595336f, -0.025769308f, -0.037734535f, -0.047122444f, -0.053703443f, -0.057387924f,
    -0.058221106f, -0.056372283f, -0.052119261f, -0.045828927f, -0.037935027f, -0.028914291f, -0.019262057f,
    -0.009468531f, +0.000003302f, +0.008737200f, +0.016379869f, +0.022653298f, +0.027362877f, +0.030401148f,
    +0.031747267f, +0.031462429f};
#define NXDN_BASE_SPS     20
#define NXDN_BASE_TAP_LEN (int)(sizeof(nxcoeffs) / sizeof(nxcoeffs[0]))

// dPMR filter - root raised cosine alpha=0.2 at 48 kHz (sps=20)
static const float dpmrcoeffs[135] = {
    -0.0000983004f, 0.0058388841f,  0.0119748846f,  0.0179185547f,  0.0232592816f,  0.0275919612f,  0.0305433586f,
    0.0317982965f,  0.0311240307f,  0.0283911865f,  0.0235897433f,  0.0168387650f,  0.0083888763f,  -0.0013831396f,
    -0.0119878087f, -0.0228442151f, -0.0333082708f, -0.0427067804f, -0.0503756642f, -0.0557003599f, -0.0581561791f,
    -0.0573462646f, -0.0530347941f, -0.0451732069f, -0.0339174991f, -0.0196350217f, -0.0028997157f, 0.0155246961f,
    0.0347134030f,  0.0536202583f,  0.0711271166f,  0.0861006725f,  0.0974542022f,  0.1042112035f,  0.1055676660f,
    0.1009496091f,  0.0900625944f,  0.0729301774f,  0.0499186839f,  0.0217462748f,  -0.0105250265f, -0.0455148664f,
    -0.0815673067f, -0.1168095612f, -0.1492246435f, -0.1767350726f, -0.1972941202f, -0.2089805758f, -0.2100926829f,
    -0.1992367833f, -0.1754063031f, -0.1380470370f, -0.0871052089f, -0.0230554989f, 0.0530929052f,  0.1398131936f,
    0.2351006721f,  0.3365341927f,  0.4413570929f,  0.5465745033f,  0.6490630781f,  0.7456885564f,  0.8334261381f,
    0.9094784589f,  0.9713859928f,  1.0171250045f,  1.0451886943f,  1.0546479089f,  1.0451886943f,  1.0171250045f,
    0.9713859928f,  0.9094784589f,  0.8334261381f,  0.7456885564f,  0.6490630781f,  0.5465745033f,  0.4413570929f,
    0.3365341927f,  0.2351006721f,  0.1398131936f,  0.0530929052f,  -0.0230554989f, -0.0871052089f, -0.1380470370f,
    -0.1754063031f, -0.1992367833f, -0.2100926829f, -0.2089805758f, -0.1972941202f, -0.1767350726f, -0.1492246435f,
    -0.1168095612f, -0.0815673067f, -0.0455148664f, -0.0105250265f, 0.0217462748f,  0.0499186839f,  0.0729301774f,
    0.0900625944f,  0.1009496091f,  0.1055676660f,  0.1042112035f,  0.0974542022f,  0.0861006725f,  0.0711271166f,
    0.0536202583f,  0.0347134030f,  0.0155246961f,  -0.0028997157f, -0.0196350217f, -0.0339174991f, -0.0451732069f,
    -0.0530347941f, -0.0573462646f, -0.0581561791f, -0.0557003599f, -0.0503756642f, -0.0427067804f, -0.0333082708f,
    -0.0228442151f, -0.0119878087f, -0.0013831396f, 0.0083888763f,  0.0168387650f,  0.0235897433f,  0.0283911865f,
    0.0311240307f,  0.0317982965f,  0.0305433586f,  0.0275919612f,  0.0232592816f,  0.0179185547f,  0.0119748846f,
    0.0058388841f,  -0.0000983004f};
#define DPMR_BASE_SPS     20
#define DPMR_BASE_TAP_LEN (int)(sizeof(dpmrcoeffs) / sizeof(dpmrcoeffs[0]))

// P25 C4FM RX de-emphasis filter - OP25 compatible (transfer_function_rx)
// Sinc-based filter that inverts the C4FM transmitter's preemphasis (t/sin(t)).
// Parameters: sample_rate=48000, symbol_rate=4800, span=9, sps=10
// Generated using IFFT of sinc(pi*f/rate) transfer function
// See: https://github.com/boatbod/op25/blob/master/op25/gr-op25_repeater/apps/tx/op25_c4fm_mod.py
#define P25_BASE_SPS      10
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
static sps_fir g_fir_dmr = {.base = dmrcoeffs,
                            .base_len = DMR_BASE_TAP_LEN,
                            .base_sps = DMR_BASE_SPS,
                            .design_kind = SPS_FIR_DESIGN_RRC,
                            .rrc_alpha = 0.7f};
static sps_fir g_fir_nxdn = {
    .base = nxcoeffs, .base_len = NXDN_BASE_TAP_LEN, .base_sps = NXDN_BASE_SPS, .design_kind = SPS_FIR_DESIGN_INTERP};
static sps_fir g_fir_dpmr = {.base = dpmrcoeffs,
                             .base_len = DPMR_BASE_TAP_LEN,
                             .base_sps = DPMR_BASE_SPS,
                             .design_kind = SPS_FIR_DESIGN_RRC,
                             .rrc_alpha = 0.2f};
static sps_fir g_fir_m17 = {.base = dsd_m17_rrc_48khz_taps,
                            .base_len = M17_BASE_TAP_LEN,
                            .base_sps = M17_BASE_SPS,
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
m17_filter(float sample, int samples_per_symbol) {
    return apply_sps_fir(&g_fir_m17, sample, samples_per_symbol);
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
