// SPDX-License-Identifier: GPL-3.0-or-later
/* Grouped lanes agree with independent scalar filters for every supported bank
 * size, including partial groups, warm history, impulses and state flushing. */
#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/nfm_noise_squelch.h>
#include <dsd-neo/dsp/pcm_noise_squelch.h>
#include <math.h>
#include <stdint.h>

#include "noise_squelch_bank.h"

int
main(void) {
    enum { MAX = DSD_PCM_NOISE_SQUELCH_MAX_BANDS, WIDTH = DSD_NOISE_SQUELCH_GROUP_BANDS };

    dsd_noise_squelch_biquad section[MAX][DSD_NOISE_SQUELCH_SECTIONS];
    for (int k = 0; k < MAX; k++) {
        dsd_noise_squelch_bank_design_band_pass(3800.0 + 50.0 * k, 4100.0 + 50.0 * k, 48000.0, section[k]);
    }
    for (int bands = 1; bands <= MAX; bands++) {
        dsd_noise_squelch_band_group bank[(MAX + WIDTH - 1) / WIDTH];
        double s1[MAX][DSD_NOISE_SQUELCH_SECTIONS] = {{0}};
        double s2[MAX][DSD_NOISE_SQUELCH_SECTIONS] = {{0}};
        double want[MAX] = {0}, got[MAX + 1] = {0};
        DSD_MEMSET(bank, 0, sizeof(bank));
        for (int k = 0; k < bands; k++) {
            dsd_noise_squelch_bank_prepare_band(bank, k, section[k]);
        }
        got[bands] = 12345.0;
        uint32_t random = 0xab3841u;
        for (int n = 0; n < 1024; n++) {
            random = random * 1664525u + 1013904223u;
            const double x = n == 0 ? 1.0 : n < 100 ? 0.0 : ((double)(random >> 16) - 32768.0) / 32768.0;
            dsd_noise_squelch_bank_energy(bank, bands, x, got);
            for (int k = 0; k < bands; k++) {
                double y = x;
                for (int s = 0; s < DSD_NOISE_SQUELCH_SECTIONS; s++) {
                    const dsd_noise_squelch_biquad* q = &section[k][s];
                    const double input = y;
                    y = q->gain * input + s1[k][s];
                    s1[k][s] = s2[k][s] - q->a1 * y;
                    s2[k][s] = -q->gain * input - q->a2 * y;
                    assert(fabs(bank[k / WIDTH].s1[s][k % WIDTH] - s1[k][s]) < 1e-12);
                    assert(fabs(bank[k / WIDTH].s2[s][k % WIDTH] - s2[k][s]) < 1e-12);
                }
                want[k] += y * y;
                assert(fabs(got[k] - want[k]) < 1e-12 * (1.0 + want[k]));
            }
            assert(fabs(got[bands] - 12345.0) < 1e-12);
            if (n == 512) {
                dsd_noise_squelch_bank_flush(bank, bands, 1e-4);
                for (int k = 0; k < bands; k++) {
                    for (int s = 0; s < DSD_NOISE_SQUELCH_SECTIONS; s++) {
                        if (fabs(s1[k][s]) < 1e-4) {
                            s1[k][s] = 0.0;
                        }
                        if (fabs(s2[k][s]) < 1e-4) {
                            s2[k][s] = 0.0;
                        }
                    }
                }
            }
        }
    }
    return 0;
}
