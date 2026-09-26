#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <dsd-neo/protocol/tetra/tetra_fec.h>
#if defined(_WIN32)
# include <direct.h>
# define MKDIR(p) _mkdir(p)
#else
# include <sys/stat.h>
# define MKDIR(p) mkdir(p, 0755)
#endif

struct conv_enc_state { uint8_t delayed[4]; };
int conv_enc_init(struct conv_enc_state *ces);
int conv_enc_input(struct conv_enc_state *ces, uint8_t *in, int len, uint8_t *out);
uint32_t viterbi_decode(uint8_t* out, const uint16_t* in, const uint16_t len);

static int run_diag_for_punct(int p_id) {
    const int coded_bits = 432;
    const int tail_bits = 4;

    /* To avoid i_func-derived k accessing beyond the generated mother buffer,
     * generate a larger mother buffer. Some puncturers map into a mother-code
     * space larger than the nominal `coded_bits` (e.g. type2_len * 4 > 432).
     * Use a safe multiplier to produce enough mother-code symbols for mapping.
     */
    const int mother_len = coded_bits * 3; /* safe upper bound for current tests */
    const int total_info = (mother_len / 4);
    const int info_bits = total_info - tail_bits;

    uint8_t *src = (uint8_t*)malloc(total_info);
    if (!src) return 1;
    for (int i = 0; i < info_bits; i++) src[i] = (i & 1);
    for (int i = info_bits; i < total_info; i++) src[i] = 0;

    uint8_t *coded = (uint8_t*)malloc(mother_len);
    if (!coded) return 2;
    memset(coded, 0, mother_len);
    struct conv_enc_state ces;
    conv_enc_init(&ces);
    conv_enc_input(&ces, src, total_info, coded);

    /* produce puncted output for p_id */
    uint8_t *puncted = (uint8_t*)malloc(coded_bits);
    if (!puncted) return 3;
    int rc = get_punctured_rate(p_id, coded, coded_bits, puncted);
    printf("punct_id=%d get_punctured_rate rc=%d\n", p_id, rc);
    if (rc != 0) return 4;

    /* mark mapped k by iterating j=1..coded_bits */
    int *mapped_k = calloc(coded_bits + 1, sizeof(int));
    for (int j = 1; j <= coded_bits; j++) {
        int k = tetra_rcpc_map_j_to_k(p_id, (uint32_t)j);
        if (k >= 1 && k <= coded_bits) mapped_k[k] = 1;
    }
    int unmapped = 0;
    for (int k = 1; k <= coded_bits; k++) if (!mapped_k[k]) unmapped++;
    printf("  coded_bits=%d mapped=%d unmapped=%d\n", coded_bits, coded_bits - unmapped, unmapped);

    printf("  sample unmapped k:");
    int printed = 0;
    for (int k = 1; k <= coded_bits && printed < 64; k++) {
        if (!mapped_k[k]) { printf(" %d", k); printed++; }
    }
    printf("\n\n");

    /* convert coded bits to mother soft-costs and run puncture->depuncture roundtrip */
    uint16_t *mother_costs = (uint16_t*)malloc(sizeof(uint16_t) * coded_bits);
    for (int i = 0; i < coded_bits; i++) mother_costs[i] = coded[i] ? 0xFFFF : 0x0000;

    int puncted_len = coded_bits;
    uint16_t *punct_in_costs = (uint16_t*)malloc(sizeof(uint16_t) * puncted_len);
    for (int i = 0; i < puncted_len; i++) punct_in_costs[i] = (puncted[i] ? 0xFFFF : 0x0000);

    uint16_t *recon_costs = (uint16_t*)malloc(sizeof(uint16_t) * coded_bits);
    for (int i = 0; i < coded_bits; i++) recon_costs[i] = 0x7FFF;

    int dr = tetra_rcpc_depuncture_by_id(p_id, punct_in_costs, puncted_len, recon_costs, coded_bits);
    if (dr < 0) {
        printf("depuncture failed rc=%d\n", dr);
        return 5;
    }

    /* convert recon_costs to hard bits */
    uint8_t *recon_bits = (uint8_t*)malloc(coded_bits);
    for (int i = 0; i < coded_bits; i++) recon_bits[i] = (recon_costs[i] > 0x7FFF) ? 1 : 0;

    printf("first 64 coded bits:\n");
    for (int i = 0; i < 64 && i < coded_bits; i++) printf("%u", coded[i]);
    printf("\nfirst 64 recon bits:\n");
    for (int i = 0; i < 64 && i < coded_bits; i++) printf("%u", recon_bits[i]);
    printf("\n\n");

    /* print first 64 j->k mappings */
    printf("puncturer j->k mapping (first 64):\n");
    for (int j = 1; j <= 64; j++) {
        int k = tetra_rcpc_map_j_to_k(p_id, (uint32_t)j);
        printf(" j=%3d -> k=%4d\n", j, k);
    }

    /* detailed trace for first few mismatches */
    printf("\nDetailed mismatches (first 32):\n");
    int mism_out = 0;
    for (int idx = 0; idx < coded_bits && mism_out < 32; idx++) {
        if (recon_bits[idx] == coded[idx]) continue;
        /* find j mapping */
        int found_j = -1;
        for (int j = 1; j <= puncted_len; j++) {
            int k = tetra_rcpc_map_j_to_k(p_id, (uint32_t)j);
            if (k == idx+1) { found_j = j; break; }
        }
        if (found_j < 0) {
            printf(" mism_idx=%3d: coded=%u recon=%u (UNMAPPED k=%d)\n", idx, coded[idx], recon_bits[idx], idx+1);
        } else {
            int j = found_j;
            int k = tetra_rcpc_map_j_to_k(p_id, (uint32_t)j);
            uint8_t puncted_val = puncted[j-1];
            uint16_t punct_cost = punct_in_costs[j-1];
            uint16_t soft_deint_val = mother_costs[k-1];
            printf(" mism_idx=%3d: coded=%u recon=%u | j=%3d k=%3d puncted[j]=%u punct_cost=0x%04x soft_deint[k]=0x%04x\n",
                idx, coded[idx], recon_bits[idx], j, k, puncted_val, punct_cost, soft_deint_val);
        }
        mism_out++;
    }

    /* prepare artifacts directory for this punct_id */
    char artdir[256];
    snprintf(artdir, sizeof(artdir), "artifacts/punct_%d", p_id);
    MKDIR("artifacts");
    MKDIR(artdir);

    /* write full unmapped list to file inside artifacts dir */
    char unmapped_fname[320];
    snprintf(unmapped_fname, sizeof(unmapped_fname), "%s/tetra_unmapped_k.txt", artdir);
    FILE *uf = fopen(unmapped_fname, "w");
    if (uf) {
        fprintf(uf, "# unmapped k indices for coded_bits=%d\n", coded_bits);
        fprintf(uf, "mapped_count=%d unmapped_count=%d\n", coded_bits - unmapped, unmapped);
        for (int k = 1; k <= coded_bits; k++) if (!mapped_k[k]) fprintf(uf, "%d\n", k);
        fclose(uf);
        printf("wrote %s\n", unmapped_fname);
    }

    /* Cross-check: run viterbi on recon_costs, then feed depunctured soft-costs into osmo shim
     * and produce per-bit comparison report for the first `total_info` bits.
     */
    /* allocate viterbi_out in outer scope so tuning sweep can reuse it */
    uint8_t viterbi_out[256];
    memset(viterbi_out, 0, sizeof(viterbi_out));
    uint32_t v_errs = viterbi_decode(viterbi_out, recon_costs, (uint16_t)coded_bits);
    {
        /* viterbi decode of depunctured soft-costs (already performed into viterbi_out) */
        printf("viterbi_decode returned cost=%u\n", v_errs);
        printf("viterbi_decode returned cost=%u\n", v_errs);

        /* prepare osmo-style input and call shim */
        extern int conv_cch_decode(int8_t *input, uint8_t *output, int n);
        int8_t *osmo_in = (int8_t*)malloc(coded_bits);
        uint8_t osmo_out[256]; memset(osmo_out, 0, sizeof(osmo_out));
        if (osmo_in) {
            for (int i = 0; i < coded_bits; i++) {
                uint16_t c = recon_costs[i];
                if (c > 0x7FFF) osmo_in[i] = 127;    /* strong 1 */
                else if (c == 0) osmo_in[i] = 0;    /* strong 0 */
                else osmo_in[i] = -127;             /* neutral */
            }
            int sym_count = coded_bits / 4;
            int rc2 = conv_cch_decode(osmo_in, osmo_out, sym_count);
            printf("osmo shim conv_cch_decode rc=%d\n", rc2);

            /* write decoded bitstrings (first total_info bits) to files in artifacts */
            int total_info = (coded_bits / 4);
            char vfname[320]; snprintf(vfname, sizeof(vfname), "%s/viterbi_decoded_bits.txt", artdir);
            char ofname[320]; snprintf(ofname, sizeof(ofname), "%s/osmo_shim_decoded_bits.txt", artdir);
            FILE *f_v = fopen(vfname, "w");
            FILE *f_o = fopen(ofname, "w");
            if (f_v && f_o) {
                for (int i = 0; i < total_info; i++) {
                    int bit_idx = i;
                    int bytepos = bit_idx / 8;
                    int bitpos = 7 - (bit_idx % 8);
                    uint8_t vb = (viterbi_out[bytepos] >> bitpos) & 1;
                    uint8_t ob = (osmo_out[bytepos] >> bitpos) & 1;
                    fputc(vb ? '1' : '0', f_v);
                    fputc(ob ? '1' : '0', f_o);
                }
                fclose(f_v);
                fclose(f_o);
            }

            /* produce per-bit diff report; mask diffs that originate from any
             * punctured coded positions (each info symbol corresponds to 4 coded bits).
             */
            char diff_fname[320];
            snprintf(diff_fname, sizeof(diff_fname), "%s/viterbi_vs_osmo_diff.txt", artdir);
            FILE *fd = fopen(diff_fname, "w");
            if (fd) {
                fprintf(fd, "# viterbi vs osmo_shim bitwise diff (first %d bits)\n", total_info);
                fprintf(fd, "# idx viterbi osmo equal masked\n");
                int diffs = 0;
                for (int i = 0; i < total_info; i++) {
                    int start_k = i * 4 + 1;
                    int mask_skip = 0;
                    for (int kk = start_k; kk <= start_k + 3; kk++) {
                        if (kk >= 1 && kk <= coded_bits) {
                            if (!mapped_k[kk]) { mask_skip = 1; break; }
                        }
                    }
                    int bytepos = i / 8;
                    int bitpos = 7 - (i % 8);
                    uint8_t vb = (viterbi_out[bytepos] >> bitpos) & 1;
                    uint8_t ob = (osmo_out[bytepos] >> bitpos) & 1;
                    int eq = (vb == ob);
                    if (!mask_skip && !eq) diffs++;
                    fprintf(fd, "%4d %d %d %s %s\n", i, vb, ob, eq ? "=" : "DIFF", mask_skip ? "MASKED" : "");
                }
                fprintf(fd, "# total_diffs=%d\n", diffs);
                fclose(fd);
                printf("wrote %s (diffs=%d)\n", diff_fname, diffs);
            }

            free(osmo_in);
        }
    }

    /* Mapping sweep: try several (high,low) threshold pairs and record diffs */
    {
        char tune_fname[128];
        snprintf(tune_fname, sizeof(tune_fname), "shim_tune_report_%d.txt", p_id);
        char tune_path[320]; snprintf(tune_path, sizeof(tune_path), "%s/%s", artdir, tune_fname);
        FILE *rf = fopen(tune_path, "w");
        if (!rf) {
            printf("failed to open %s for writing\n", tune_path);
        } else {
            const uint16_t highs[] = {0xFFFF, 0xC000, 0x8000, 0x7FFF};
            const uint16_t lows[]  = {0x0000, 0x1000, 0x2000};
            int nh = sizeof(highs)/sizeof(highs[0]);
            int nl = sizeof(lows)/sizeof(lows[0]);
            fprintf(rf, "# shim tuning report: try (high_threshold, low_threshold) -> diffs (first %d bits)\n", (coded_bits/4));
            for (int ih = 0; ih < nh; ih++) for (int il = 0; il < nl; il++) {
                uint16_t high = highs[ih];
                uint16_t low = lows[il];
                /* build osmo_in for this mapping */
                int8_t *osmo_in2 = (int8_t*)malloc(coded_bits);
                if (!osmo_in2) continue;
                for (int i = 0; i < coded_bits; i++) {
                    uint16_t c = recon_costs[i];
                    if (c >= high) osmo_in2[i] = 127;
                    else if (c <= low) osmo_in2[i] = 0;
                    else osmo_in2[i] = -127;
                }
                uint8_t osmo_out2[256]; memset(osmo_out2, 0, sizeof(osmo_out2));
                int sym_count = coded_bits / 4;
                extern int conv_cch_decode(int8_t *input, uint8_t *output, int n);
                int rcx = conv_cch_decode(osmo_in2, osmo_out2, sym_count);

                /* run viterbi on recon_costs (already done above as viterbi_out) -- compare bits */
                int total_info = coded_bits / 4;
                int diffs = 0;
                for (int i = 0; i < total_info; i++) {
                    int bytepos = i / 8;
                    int bitpos = 7 - (i % 8);
                    uint8_t vb = (viterbi_out[bytepos] >> bitpos) & 1;
                    uint8_t ob = (osmo_out2[bytepos] >> bitpos) & 1;
                    if (vb != ob) diffs++;
                }
                fprintf(rf, "high=0x%04X low=0x%04X rc=%d diffs=%d\n", high, low, rcx, diffs);
                free(osmo_in2);
            }
            fclose(rf);
            printf("wrote %s\n", tune_path);
        }
    }

    /* mapping validation: check for out-of-range k and duplicate mappings */
    {
        char mapcheck_fname[320];
        snprintf(mapcheck_fname, sizeof(mapcheck_fname), "%s/mapping_check.txt", artdir);
        FILE *mf = fopen(mapcheck_fname, "w");
        if (mf) {
            fprintf(mf, "# mapping check for punct_id=%d coded_bits=%d\n", p_id, coded_bits);
            int out_of_range = 0;
            int duplicates = 0;
            int *seen = calloc(coded_bits + 1, sizeof(int));
            for (int j = 1; j <= puncted_len; j++) {
                int k = tetra_rcpc_map_j_to_k(p_id, (uint32_t)j);
                if (k < 1 || k > coded_bits) {
                    fprintf(mf, "OUT_OF_RANGE j=%d k=%d\n", j, k);
                    out_of_range++;
                } else {
                    if (seen[k]) {
                        fprintf(mf, "DUPLICATE k=%d first_j=%d another_j=%d\n", k, seen[k]-1, j);
                        duplicates++;
                    } else {
                        seen[k] = j+1; /* store j+1 to allow 0 as sentinel */
                    }
                }
            }
            fprintf(mf, "summary: out_of_range=%d duplicates=%d mapped_count=%d unmapped_count=%d\n", out_of_range, duplicates, coded_bits - unmapped, unmapped);
            free(seen);
            fclose(mf);
            printf("wrote %s\n", mapcheck_fname);
        }
    }

    free(src);
    free(coded);
    free(puncted);
    free(mapped_k);
    free(mother_costs);
    free(punct_in_costs);
    free(recon_costs);
    free(recon_bits);

    return 0;
}

int main(int argc, char **argv) {
    /* If no args, run a full sweep across puncturer ids 0..6. If an integer
     * argument is provided, run only that puncturer id. */
    if (argc == 1) {
        for (int pid = 0; pid <= 6; pid++) {
            printf("\n===== Running diagnostics for punct_id=%d =====\n", pid);
            int rc = run_diag_for_punct(pid);
            if (rc != 0) fprintf(stderr, "diagnostic for punct_id=%d returned %d\n", pid, rc);
        }
        return 0;
    } else {
        int pid = atoi(argv[1]);
        return run_diag_for_punct(pid);
    }
}
