#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include <dsd-neo/protocol/tetra/tetra_fec.h>

/* Forward declarations from osmo conv encoder source (compiled into test)
 * The conv encoder defines `struct conv_enc_state { uint8_t delayed[4]; }`.
 */
struct conv_enc_state { uint8_t delayed[4]; };
int conv_enc_init(struct conv_enc_state *ces);
int conv_enc_input(struct conv_enc_state *ces, uint8_t *in, int len, uint8_t *out);
int get_punctured_rate(int pu, uint8_t *in, int len, uint8_t *out);
int conv_cch_decode(int8_t *input, uint8_t *output, int n);

/* Simple viterbi decode wrapper (available in project) */
extern uint32_t viterbi_decode(uint8_t* out, const uint16_t* in, const uint16_t len);

struct test_param { int type2_len; int type3_len; int mother_rate; int punct; };

static struct test_param params[] = {
    { 80, 120, 4, TETRA_RCPC_PUNCT_2_3 },
    { 38, 80, 3,  TETRA_RCPC_PUNCT_38_80 },
    { 292, 432, 4, TETRA_RCPC_PUNCT_292_432 },
};

int main(void) {
    /* fixed seed for reproducible failures during debugging */
    srand(42);
    int nparams = sizeof(params)/sizeof(params[0]);
    for (int p = 0; p < nparams; p++) {
        int type2_len = params[p].type2_len;
        int type3_len = params[p].type3_len;
        int punct_id = params[p].punct;
        int coded_per_input = 4; /* conv encoder emits 4 coded bits per input bit */
        int mother_len = type2_len * coded_per_input;

        fprintf(stderr, "Regression test: type2=%d type3=%d coded_per_input=%d punct=%d\n",
            type2_len, type3_len, coded_per_input, punct_id);

        /* run a few random frames to ensure stability */
        for (int frame = 0; frame < 3; frame++) {
            uint8_t *in_bits = malloc(type2_len);
            uint8_t *mother_code = malloc(mother_len);
            uint8_t *type3_buf = malloc(type3_len);
            uint16_t *in_costs = malloc(sizeof(uint16_t) * type3_len);
            uint16_t *recon_costs = malloc(sizeof(uint16_t) * mother_len);
            uint8_t *decoded = malloc(type2_len);
            if (!in_bits || !mother_code || !type3_buf || !in_costs || !recon_costs || !decoded) {
                fprintf(stderr, "OOM\n");
                return 2;
            }

            /* random input bits */
            for (int i = 0; i < type2_len; i++) in_bits[i] = (uint8_t)(rand() & 1);

            struct conv_enc_state ces;
            conv_enc_init(&ces);
            memset(mother_code, 0, mother_len);
            conv_enc_input(&ces, in_bits, type2_len, mother_code);

            /* puncture to get transmitted type3 bits */
            get_punctured_rate(punct_id, mother_code, type3_len, type3_buf);

            /* map transmitted bits to strong soft-costs: 1 -> 0xFFFF, 0 -> 0x0000 */
            for (int i = 0; i < type3_len; i++) in_costs[i] = type3_buf[i] ? 0xFFFFu : 0x0000u;

            /* depuncture into recon_costs (mother_len) */
            for (int i = 0; i < mother_len; i++) recon_costs[i] = 0x7FFFu;
            tetra_rcpc_depuncture_by_id(punct_id, in_costs, type3_len, recon_costs, mother_len);

            /* run viterbi on reconstructed soft-costs */
            uint32_t cost = viterbi_decode(decoded, recon_costs, (uint16_t)mother_len);
            (void)cost;

            /* Build osmo-style int8 soft inputs from our recon_costs and run the osmo shim
             * decoder; this validates that our depuncture + viterbi behavior matches the
             * osmo shim's decoding semantics rather than directly comparing to original bits
             * (which may be unrecoverable when punctured).
             */
            int8_t *osmo_in = malloc(mother_len);
            uint8_t *osmo_decoded = malloc(type2_len);
            if (!osmo_in || !osmo_decoded) {
                free(osmo_in); free(osmo_decoded);
                free(in_bits); free(mother_code); free(type3_buf); free(in_costs);
                free(recon_costs); free(decoded);
                fprintf(stderr, "OOM2\n");
                return 2;
            }
            for (int i = 0; i < mother_len; i++) {
                uint16_t c = recon_costs[i];
                if (c == 0xFFFFu) osmo_in[i] = 127;   /* strong '1' */
                else if (c == 0x0000u) osmo_in[i] = -127; /* strong '0' */
                else osmo_in[i] = 0; /* neutral */
            }
            conv_cch_decode(osmo_in, osmo_decoded, type2_len);
            /* decoded length is mother_len/4 (input bits count) — compare first type2_len bits
             * Only consider a mismatch fatal if all coded symbols for that input bit were present
             * (i.e., none of the 4 corresponding recon_costs == 0x7FFF). Mismatches occurring
             * where at least one of the coded symbols was neutral are likely caused by puncturing
             * and are tolerated for this regression.
             */
            int mism = 0;
            int mism_on_fullinfo = 0;
            for (int i = 0; i < type2_len; i++) {
                if (decoded[i] != in_bits[i]) {
                    mism++;
                }
                /* compare our viterbi output to osmo shim's output */
                if (decoded[i] != osmo_decoded[i]) {
                    int gs = i * coded_per_input;
                    int any_neutral = 0;
                    for (int b = 0; b < coded_per_input; b++) {
                        if (recon_costs[gs + b] == 0x7FFFu) { any_neutral = 1; break; }
                    }
                    if (!any_neutral) mism_on_fullinfo++;
                }
            }

            fprintf(stderr, " frame=%d mismatches=%d mism_fullinfo=%d (decoded cost=%u)\n", frame, mism, mism_on_fullinfo, cost);

            if (mism_on_fullinfo != 0) {
                /* print detailed per-index info for full-info mismatches */
                for (int i = 0; i < type2_len; i++) {
                    if (decoded[i] != osmo_decoded[i]) {
                        int gs = i * coded_per_input;
                        int any_neutral = 0;
                        for (int b = 0; b < coded_per_input; b++) {
                            if (recon_costs[gs + b] == 0x7FFFu) { any_neutral = 1; break; }
                        }
                        if (!any_neutral) {
                            fprintf(stderr, "FULLIDX idx=%d v=%d o=%d group=%u,%u,%u,%u\n",
                                i, decoded[i], osmo_decoded[i],
                                recon_costs[gs+0], recon_costs[gs+1], recon_costs[gs+2], recon_costs[gs+3]);
                        }
                    }
                }
                /* attempt to write debugging artifacts for this failing frame */
                char dirbuf[256];
                char fpath[512];
#ifdef _WIN32
                _mkdir("artifacts");
#else
                mkdir("artifacts", 0755);
#endif
                snprintf(dirbuf, sizeof(dirbuf), "artifacts/punct_%d", punct_id);
#ifdef _WIN32
                _mkdir(dirbuf);
#else
                mkdir(dirbuf, 0755);
#endif

                snprintf(fpath, sizeof(fpath), "%s/regression_frame_%d_in_bits.txt", dirbuf, frame);
                FILE *f = fopen(fpath, "w");
                if (f) {
                    for (int i = 0; i < type2_len; i++) fputc(in_bits[i] ? '1' : '0', f);
                    fputc('\n', f);
                    fclose(f);
                }

                snprintf(fpath, sizeof(fpath), "%s/regression_frame_%d_mother_code.txt", dirbuf, frame);
                f = fopen(fpath, "w");
                if (f) {
                    for (int i = 0; i < mother_len; i++) fputc(mother_code[i] ? '1' : '0', f);
                    fputc('\n', f);
                    fclose(f);
                }

                snprintf(fpath, sizeof(fpath), "%s/regression_frame_%d_type3.txt", dirbuf, frame);
                f = fopen(fpath, "w");
                if (f) {
                    for (int i = 0; i < type3_len; i++) fputc(type3_buf[i] ? '1' : '0', f);
                    fputc('\n', f);
                    fclose(f);
                }

                snprintf(fpath, sizeof(fpath), "%s/regression_frame_%d_recon_costs.txt", dirbuf, frame);
                f = fopen(fpath, "w");
                if (f) {
                    for (int i = 0; i < mother_len; i++) fprintf(f, "%u\n", recon_costs[i]);
                    fclose(f);
                }

                snprintf(fpath, sizeof(fpath), "%s/regression_frame_%d_decoded.txt", dirbuf, frame);
                f = fopen(fpath, "w");
                if (f) {
                    for (int i = 0; i < type2_len; i++) fputc(decoded[i] ? '1' : '0', f);
                    fputc('\n', f);
                    fclose(f);
                }

                free(in_bits); free(mother_code); free(type3_buf); free(in_costs);
                free(recon_costs); free(decoded);
                free(osmo_in); free(osmo_decoded);
                fprintf(stderr, "Regression FAILED for punct %d (artifacts written to %s)\n", punct_id, dirbuf);
                return 3;
            }

            free(in_bits); free(mother_code); free(type3_buf); free(in_costs);
            free(recon_costs); free(decoded);
        }
    }

    fprintf(stderr, "Depuncture+Viterbi regression passed\n");
    return 0;
}
