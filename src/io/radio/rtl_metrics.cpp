// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * RTL-SDR metrics and auto-PPM helpers.
 *
 * Houses spectrum/SNR-based auto-PPM supervision state, spectrum and
 * carrier diagnostics, and the public query/toggle helpers used by
 * the UI and protocol code.
 */

#include <dsd-neo/io/rtl_metrics.h>

#include <atomic>
#include <cmath>
#include <dsd-neo/dsp/demod_state.h>
#include <string.h>

/* Spectrum capture and carrier diagnostics shared with RTL orchestrator. */
static const int kSpecMaxN = 1024; /* Max FFT size (power of two) */
float g_spec_db[kSpecMaxN];
std::atomic<int> g_spec_rate_hz{0};
std::atomic<int> g_spec_ready{0};
std::atomic<int> g_spec_N{256}; /* default N */
/* Carrier diagnostics (updated alongside spectrum) */
static std::atomic<double> g_cfo_nco_hz{0.0};
static std::atomic<double> g_resid_cfo_spec_hz{0.0};
static std::atomic<int> g_carrier_lock{0};
static std::atomic<int> g_nco_q15{0};
static std::atomic<int> g_demod_rate_hz{0};
static std::atomic<int> g_costas_err_avg_q14{0};

/* Demodulator state (defined in rtl_sdr_fm.cpp) used for CFO/Costas metrics. */
extern demod_state demod;

/* SNR estimates from demod thread (defined in rtl_sdr_fm.cpp). */
extern std::atomic<double> g_snr_c4fm_db;
extern std::atomic<double> g_snr_qpsk_db;
extern std::atomic<double> g_snr_gfsk_db;

/* Supervisory tuner autogain gate (0/1), controlled via env/UI. */
std::atomic<int> g_tuner_autogain_on{0};

/* Auto-PPM status (spectrum-based). */
std::atomic<int> g_auto_ppm_enabled{0};
/* User override for auto-PPM: -1 = follow env/opts; 0 = force off; 1 = force on. */
std::atomic<int> g_auto_ppm_user_en{-1};
std::atomic<int> g_auto_ppm_locked{0};
std::atomic<int> g_auto_ppm_training{0};
std::atomic<int> g_auto_ppm_lock_ppm{0};
std::atomic<double> g_auto_ppm_lock_snr_db{-100.0};
std::atomic<double> g_auto_ppm_lock_df_hz{0.0};
std::atomic<double> g_auto_ppm_snr_db{-100.0};
std::atomic<double> g_auto_ppm_df_hz{0.0};
std::atomic<double> g_auto_ppm_est_ppm{0.0};
std::atomic<int> g_auto_ppm_last_dir{0};
std::atomic<int> g_auto_ppm_cooldown{0};

/* Internal FFT helper (radix-2, in-place). */
static inline void
fft_rad2(float* xr, float* xi, int N) {
    int j = 0;
    for (int i = 1; i < N; i++) {
        int bit = N >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j &= ~bit;
        }
        j |= bit;
        if (i < j) {
            float tr = xr[i];
            float ti = xi[i];
            xr[i] = xr[j];
            xi[i] = xi[j];
            xr[j] = tr;
            xi[j] = ti;
        }
    }
    for (int len = 2; len <= N; len <<= 1) {
        float ang = -2.0f * static_cast<float>(M_PI) / static_cast<float>(len);
        float wlen_r = cosf(ang);
        float wlen_i = sinf(ang);
        for (int i = 0; i < N; i += len) {
            float wr = 1.0f;
            float wi = 0.0f;
            int half = len >> 1;
            for (int k = 0; k < half; k++) {
                int j0 = i + k;
                int j1 = j0 + half;
                float ur = xr[j0];
                float ui = xi[j0];
                float vr = xr[j1] * wr - xi[j1] * wi;
                float vi = xr[j1] * wi + xi[j1] * wr;
                xr[j0] = ur + vr;
                xi[j0] = ui + vi;
                xr[j1] = ur - vr;
                xi[j1] = ui - vi;
                float nwr = wr * wlen_r - wi * wlen_i;
                wi = wr * wlen_i + wi * wlen_r;
                wr = nwr;
            }
        }
    }
}

void
rtl_metrics_update_spectrum_from_iq(const int16_t* iq_interleaved, int len_interleaved, int out_rate_hz) {
    if (!iq_interleaved || len_interleaved < 2) {
        return;
    }
    const int pairs = len_interleaved >> 1;
    /* Use current FFT size; clamp to bounds */
    int N = g_spec_N.load(std::memory_order_relaxed);
    if (N < 64) {
        N = 64;
    }
    if (N > kSpecMaxN) {
        N = kSpecMaxN;
    }
    /* Prepare last N complex samples (I/Q) with DC removal and Hann window */
    static float xr[kSpecMaxN];
    static float xi[kSpecMaxN];
    int take = (pairs >= N) ? N : pairs;
    int start = (pairs - take);
    double sumI = 0.0;
    double sumQ = 0.0;
    for (int n = 0; n < take; n++) {
        int idx = start + n;
        int16_t I = iq_interleaved[(size_t)(idx << 1) + 0];
        int16_t Q = iq_interleaved[(size_t)(idx << 1) + 1];
        sumI += static_cast<double>(I);
        sumQ += static_cast<double>(Q);
    }
    float meanI = (take > 0) ? static_cast<float>(sumI / static_cast<double>(take)) : 0.0f;
    float meanQ = (take > 0) ? static_cast<float>(sumQ / static_cast<double>(take)) : 0.0f;
    for (int n = 0; n < N; n++) {
        xr[n] = 0.0f;
        xi[n] = 0.0f;
    }
    for (int n = 0; n < take; n++) {
        float w =
            0.5f * (1.0f - cosf(2.0f * static_cast<float>(M_PI) * static_cast<float>(n) / static_cast<float>(N - 1)));
        int idx = start + n;
        int16_t I = iq_interleaved[(size_t)(idx << 1) + 0];
        int16_t Q = iq_interleaved[(size_t)(idx << 1) + 1];
        xr[n] = w * (static_cast<float>(I) - meanI);
        xi[n] = w * (static_cast<float>(Q) - meanQ);
    }
    fft_rad2(xr, xi, N);
    const float eps = 1e-12f;
    for (int k = 0; k < N; k++) {
        int kk = k + (N >> 1);
        if (kk >= N) {
            kk -= N;
        }
        float re = xr[kk];
        float im = xi[kk];
        float mag2 = re * re + im * im;
        float db = 10.0f * log10f(mag2 + eps);
        float prev = g_spec_db[k];
        if (g_spec_ready.load(std::memory_order_relaxed) == 0) {
            g_spec_db[k] = db;
        } else {
            g_spec_db[k] = 0.8f * prev + 0.2f * db;
        }
    }
    g_spec_rate_hz.store(out_rate_hz, std::memory_order_relaxed);
    g_spec_ready.store(1, std::memory_order_release);

    /* Compute residual CFO from spectrum peak around DC using quadratic interp. */
    int i_max = 0;
    float p_max = -1e30f;
    for (int k = 0; k < N; k++) {
        float v = g_spec_db[k];
        if (v > p_max) {
            p_max = v;
            i_max = k;
        }
    }
    double df_spec_hz = 0.0;
    if (N >= 3 && i_max > 0 && i_max + 1 < N) {
        double p1 = g_spec_db[i_max - 1];
        double p2 = g_spec_db[i_max + 0];
        double p3 = g_spec_db[i_max + 1];
        double denom = (p1 - 2.0 * p2 + p3);
        double delta = 0.0;
        if (fabs(denom) > 1e-9) {
            delta = 0.5 * (p1 - p3) / denom;
            if (delta < -0.5) {
                delta = -0.5;
            }
            if (delta > +0.5) {
                delta = +0.5;
            }
        }
        double center = static_cast<double>(N / 2);
        double k_off = (static_cast<double>(i_max) + delta) - center;
        df_spec_hz = (out_rate_hz > 0) ? (k_off * static_cast<double>(out_rate_hz) / static_cast<double>(N)) : 0.0;
    }
    g_resid_cfo_spec_hz.store(df_spec_hz, std::memory_order_relaxed);

    /* NCO CFO from Costas/FLL (Q15 cycles/sample scaled by Fs) */
    double cfo_hz = 0.0;
    if (out_rate_hz > 0) {
        int fq = demod.fll_freq_q15;
        cfo_hz = (static_cast<double>(fq) * static_cast<double>(out_rate_hz)) / 32768.0;
    }
    g_cfo_nco_hz.store(cfo_hz, std::memory_order_relaxed);
    g_nco_q15.store(demod.fll_freq_q15, std::memory_order_relaxed);
    g_demod_rate_hz.store(out_rate_hz, std::memory_order_relaxed);
    g_costas_err_avg_q14.store(demod.costas_err_avg_q14, std::memory_order_relaxed);

    /* Spectrum-assisted CFO correction for CQPSK:
     * When CQPSK path and FLL are enabled, and we see a reasonably strong
     * QPSK signal, use the residual CFO estimate from the spectrum to gently
     * nudge the FLL NCO toward zero residual. This acts as a slow outer loop
     * around the symbol-domain FLL/Costas, improving pull-in when residual
     * CFO is outside their comfort zone.
     *
     * To avoid loop fighting and NCO oscillation when the inner Costas/FLL
     * combination is already close to lock, we:
     *   - Require the CQPSK acquisition FLL (when enabled) to report locked.
     *   - Ignore very small residuals near DC.
     *   - Use a conservative outer-loop gain.
     */
    if (demod.cqpsk_enable && demod.fll_enabled && out_rate_hz > 0) {
        double snr_qpsk = g_snr_qpsk_db.load(std::memory_order_relaxed);
        double abs_df = fabs(df_spec_hz);
        int acq_ok = (!demod.cqpsk_acq_fll_enable) || demod.cqpsk_acq_fll_locked;
        /* Gate: require reasonable SNR, acquisition FLL lock (when used),
         * and ignore both wildly off and tiny residual estimates. */
        const double k_df_min = 150.0; /* Hz: ignore residuals below ~150 Hz to reduce jitter */
        const double k_df_max = 2500.0;
        if (acq_ok && snr_qpsk > -3.0 && abs_df > k_df_min && abs_df < k_df_max) {
            /* Outer-loop gain: fraction of residual per update. */
            const double k_outer = 0.05;
            /* Residual is after NCO; increase NCO CFO toward signal CFO:
             * freq_new = freq_old + k * residual * (32768/Fs).
             */
            double delta_q15_d = k_outer * df_spec_hz * 32768.0 / static_cast<double>(out_rate_hz);
            int delta_q15 = static_cast<int>(lrint(delta_q15_d));
            if (delta_q15 != 0) {
                const int F_CLAMP = 4096; /* must track FLL clamp */
                int f_old = demod.fll_freq_q15;
                int f_new = f_old + delta_q15;
                if (f_new > F_CLAMP) {
                    f_new = F_CLAMP;
                }
                if (f_new < -F_CLAMP) {
                    f_new = -F_CLAMP;
                }
                int delta_applied = f_new - f_old;
                /* Mirror the outer-loop nudge into the acquisition FLL integrator so that
                 * the PI controller's internal state tracks the new target and does not
                 * immediately pull freq_q15 back toward the old integrator value. */
                int i_old = demod.fll_state.int_q15;
                int i_new = i_old + delta_applied;
                if (i_new > F_CLAMP) {
                    i_new = F_CLAMP;
                }
                if (i_new < -F_CLAMP) {
                    i_new = -F_CLAMP;
                }
                demod.fll_freq_q15 = f_new;
                demod.fll_state.int_q15 = i_new;
                g_nco_q15.store(f_new, std::memory_order_relaxed);
                /* Recompute NCO CFO export after adjustment */
                cfo_hz = (static_cast<double>(f_new) * static_cast<double>(out_rate_hz)) / 32768.0;
                g_cfo_nco_hz.store(cfo_hz, std::memory_order_relaxed);
            }
        }
    }

    /* Simple lock heuristic for CQPSK: small residual df and reasonable SNR */
    int locked = 0;
    if (demod.cqpsk_enable) {
        double snr = g_snr_qpsk_db.load(std::memory_order_relaxed);
        double thr_df = 120.0;
        if (fabs(df_spec_hz) < thr_df && snr > 8.0) {
            locked = 1;
        }
    }
    g_carrier_lock.store(locked, std::memory_order_relaxed);
}

/* Spectrum and carrier diagnostics query helpers. */
extern "C" int
dsd_rtl_stream_spectrum_get(float* out_db, int max_bins, int* out_rate) {
    if (!out_db || max_bins <= 0) {
        return 0;
    }
    if (g_spec_ready.load(std::memory_order_acquire) == 0) {
        return 0;
    }
    int N = g_spec_N.load(std::memory_order_relaxed);
    if (N < 64) {
        N = 64;
    }
    if (N > kSpecMaxN) {
        N = kSpecMaxN;
    }
    int n = (max_bins < N) ? max_bins : N;
    for (int i = 0; i < n; i++) {
        out_db[i] = g_spec_db[i];
    }
    if (out_rate) {
        *out_rate = g_spec_rate_hz.load(std::memory_order_relaxed);
    }
    return n;
}

extern "C" int
dsd_rtl_stream_spectrum_set_size(int n) {
    if (n < 64) {
        n = 64;
    }
    if (n > kSpecMaxN) {
        n = kSpecMaxN;
    }
    int p = 64;
    while (p < n) {
        p <<= 1;
    }
    if (p > kSpecMaxN) {
        p = kSpecMaxN;
    }
    g_spec_N.store(p, std::memory_order_relaxed);
    return p;
}

extern "C" int
dsd_rtl_stream_spectrum_get_size(void) {
    int N = g_spec_N.load(std::memory_order_relaxed);
    if (N < 64) {
        N = 64;
    }
    if (N > kSpecMaxN) {
        N = kSpecMaxN;
    }
    return N;
}

extern "C" double
dsd_rtl_stream_get_cfo_hz(void) {
    return g_cfo_nco_hz.load(std::memory_order_relaxed);
}

extern "C" double
dsd_rtl_stream_get_residual_cfo_hz(void) {
    return g_resid_cfo_spec_hz.load(std::memory_order_relaxed);
}

extern "C" int
dsd_rtl_stream_get_carrier_lock(void) {
    return g_carrier_lock.load(std::memory_order_relaxed) ? 1 : 0;
}

extern "C" int
dsd_rtl_stream_get_nco_q15(void) {
    return g_nco_q15.load(std::memory_order_relaxed);
}

extern "C" int
dsd_rtl_stream_get_demod_rate_hz(void) {
    return g_demod_rate_hz.load(std::memory_order_relaxed);
}

extern "C" int
dsd_rtl_stream_get_costas_err_q14(void) {
    return g_costas_err_avg_q14.load(std::memory_order_relaxed);
}

/* Smoothed SNR exports (for UI and protocol code). */
extern "C" double
rtl_stream_get_snr_c4fm(void) {
    return g_snr_c4fm_db.load(std::memory_order_relaxed);
}

extern "C" double
rtl_stream_get_snr_cqpsk(void) {
    return g_snr_qpsk_db.load(std::memory_order_relaxed);
}

extern "C" double
rtl_stream_get_snr_gfsk(void) {
    return g_snr_gfsk_db.load(std::memory_order_relaxed);
}

/* Blanker and tuner autogain runtime control */
extern "C" int
dsd_rtl_stream_get_blanker(int* out_thr, int* out_win) {
    if (out_thr) {
        *out_thr = demod.blanker_thr;
    }
    if (out_win) {
        *out_win = demod.blanker_win;
    }
    return demod.blanker_enable ? 1 : 0;
}

extern "C" void
dsd_rtl_stream_set_blanker(int enable, int thr, int win) {
    if (enable >= 0) {
        demod.blanker_enable = enable ? 1 : 0;
    }
    if (thr >= 0) {
        if (thr < 0) {
            thr = 0;
        }
        if (thr > 60000) {
            thr = 60000;
        }
        demod.blanker_thr = thr;
    }
    if (win >= 0) {
        if (win < 0) {
            win = 0;
        }
        if (win > 16) {
            win = 16;
        }
        demod.blanker_win = win;
    }
}

extern "C" int
dsd_rtl_stream_get_tuner_autogain(void) {
    return g_tuner_autogain_on.load(std::memory_order_relaxed) ? 1 : 0;
}

extern "C" void
dsd_rtl_stream_set_tuner_autogain(int onoff) {
    g_tuner_autogain_on.store(onoff ? 1 : 0, std::memory_order_relaxed);
}

int
dsd_rtl_stream_auto_ppm_get_status(int* enabled, double* snr_db, double* df_hz, double* est_ppm, int* last_dir,
                                   int* cooldown, int* locked) {
    if (enabled) {
        *enabled = g_auto_ppm_enabled.load(std::memory_order_relaxed);
    }
    if (snr_db) {
        *snr_db = g_auto_ppm_snr_db.load(std::memory_order_relaxed);
    }
    if (df_hz) {
        *df_hz = g_auto_ppm_df_hz.load(std::memory_order_relaxed);
    }
    if (est_ppm) {
        *est_ppm = g_auto_ppm_est_ppm.load(std::memory_order_relaxed);
    }
    if (last_dir) {
        *last_dir = g_auto_ppm_last_dir.load(std::memory_order_relaxed);
    }
    if (cooldown) {
        *cooldown = g_auto_ppm_cooldown.load(std::memory_order_relaxed);
    }
    if (locked) {
        *locked = g_auto_ppm_locked.load(std::memory_order_relaxed);
    }
    return 0;
}

int
dsd_rtl_stream_auto_ppm_training_active(void) {
    return g_auto_ppm_training.load(std::memory_order_relaxed) ? 1 : 0;
}

int
dsd_rtl_stream_auto_ppm_get_lock(int* ppm, double* snr_db, double* df_hz) {
    if (ppm) {
        *ppm = g_auto_ppm_lock_ppm.load(std::memory_order_relaxed);
    }
    if (snr_db) {
        *snr_db = g_auto_ppm_lock_snr_db.load(std::memory_order_relaxed);
    }
    if (df_hz) {
        *df_hz = g_auto_ppm_lock_df_hz.load(std::memory_order_relaxed);
    }
    return 0;
}

void
dsd_rtl_stream_set_auto_ppm(int onoff) {
    g_auto_ppm_user_en.store(onoff ? 1 : 0, std::memory_order_relaxed);
}

int
dsd_rtl_stream_get_auto_ppm(void) {
    int u = g_auto_ppm_user_en.load(std::memory_order_relaxed);
    if (u == 0) {
        return 0;
    }
    if (u == 1) {
        return 1;
    }
    return g_auto_ppm_enabled.load(std::memory_order_relaxed) ? 1 : 0;
}
