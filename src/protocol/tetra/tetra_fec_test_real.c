#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

/* Declarations from tetra_fec.c */
int tetra_rcpc_depuncture_by_id(int punct_id, const uint16_t* in_costs, int in_len, uint16_t* out_costs, int out_len);
/* Use osmo-tetra's puncture function for exact behavior */
int get_punctured_rate(int pu, const uint8_t *in, int len, uint8_t *out);

/* External prototypes from core */
void trellis_encode(uint8_t result[], const uint8_t source[], int result_len, int reg);
uint32_t viterbi_decode(uint8_t* out, const uint16_t* in, const uint16_t len);

/* osmo conv encoder prototypes (from osmo source copied into repo) */
struct conv_enc_state { uint8_t delayed[4]; };
int conv_enc_init(struct conv_enc_state *ces);
int conv_enc_input(struct conv_enc_state *ces, uint8_t *in, int len, uint8_t *out);

int main(void) {
    fprintf(stderr, "[TETRA TEST REAL] Starting depuncture roundtrip test...\n");

    const int mother_len = 512;
    uint8_t *mother = (uint8_t*)malloc(mother_len);
    for (int i = 0; i < mother_len; i++) mother[i] = (uint8_t)(i & 0xFF);

    int punct_id = 2; /* punct_292_432 */
    const int puncted_len = 432;
    uint8_t *puncted = (uint8_t*)malloc(puncted_len);
    int wrote = tetra_rcpc_puncture_by_id(punct_id, mother, mother_len, puncted, puncted_len);
    if (wrote != puncted_len) {
        fprintf(stderr, "[TETRA TEST REAL] unexpected puncture length %d (wanted %d)\n", wrote, puncted_len);
        return 3;
    }

    uint16_t *in_costs = (uint16_t*)malloc(sizeof(uint16_t) * puncted_len);
    for (int i = 0; i < puncted_len; i++) in_costs[i] = (uint16_t)puncted[i];

    uint16_t *out_costs = (uint16_t*)malloc(sizeof(uint16_t) * mother_len);
    for (int i = 0; i < mother_len; i++) out_costs[i] = 0x7FFF;

    int ret = tetra_rcpc_depuncture_by_id(punct_id, in_costs, puncted_len, out_costs, mother_len);
    if (ret < 0) {
        fprintf(stderr, "[TETRA TEST REAL] depuncture returned error\n");
        return 4;
    }

    int mismatches = 0;
    int mapped = 0;
    for (int i = 0; i < mother_len; i++) {
        if (out_costs[i] != 0x7FFF) {
            mapped++;
            if ((uint8_t)out_costs[i] != mother[i]) {
                mismatches++;
                if (mismatches < 8) {
                    fprintf(stderr, "[TETRA TEST REAL] mismatch at pos %d: got %u expected %u\n", i, out_costs[i], mother[i]);
                }
            }
        }
    }

    fprintf(stderr, "[TETRA TEST REAL] puncted_len=%d mapped_positions=%d mismatches=%d\n", puncted_len, mapped, mismatches);
    if (mismatches == 0 && mapped > 0) {
        fprintf(stderr, "[TETRA TEST REAL] Roundtrip OK\n");
    } else {
        fprintf(stderr, "[TETRA TEST REAL] Roundtrip FAILED\n");
        return 5;
    }

    free(mother);
    free(puncted);
    free(in_costs);
    free(out_costs);

    /* End-to-end encode->puncture->depuncture->viterbi test */
    fprintf(stderr, "[TETRA TEST REAL] Starting encode->puncture->depuncture->viterbi test...\n");
    /* Use coded_bits matching type-3 length K=432 so puncturer mapping aligns
     * The osmo conv encoder produces 4 output bits per input bit, so
     * total_info (input bits to conv encoder) = coded_bits / 4.
     */
    const int tail_bits = 4; /* flush/termination bits for trellis */
    const int coded_bits = 432; /* mother-code length (type-3) */
    const int total_info = (coded_bits / 4); /* input bits for conv encoder */
    const int info_bits = total_info - tail_bits; /* actual payload bits */
    uint8_t *src = (uint8_t*)malloc(total_info);
    for (int i = 0; i < info_bits; i++) src[i] = (i & 1);
    for (int i = info_bits; i < total_info; i++) src[i] = 0; /* tail zeros */

    uint8_t *coded = (uint8_t*)malloc(coded_bits);
    for (int i = 0; i < coded_bits; i++) coded[i] = 0;
    /* Use osmo conv encoder to produce mother-code bits in the same ordering as get_punctured_rate() expects */
    struct conv_enc_state ces;
    conv_enc_init(&ces);
    conv_enc_input(&ces, src, total_info, coded);

    /* Convert coded bits to mother soft-costs (0 -> 0, 1 -> 0xFFFF) */
    uint16_t *mother_costs = (uint16_t*)malloc(sizeof(uint16_t) * coded_bits);
    for (int i = 0; i < coded_bits; i++) mother_costs[i] = coded[i] ? 0xFFFF : 0;

    /* Direct unpunctured Viterbi decode for sanity check */
    uint8_t direct_out[256];
    for (int i = 0; i < (int)sizeof(direct_out); i++) direct_out[i] = 0;
    uint32_t direct_err = viterbi_decode(direct_out, mother_costs, (uint16_t)coded_bits);
    fprintf(stderr, "[TETRA TEST REAL] direct viterbi decode cost=%u first 32 bits:\n", direct_err);
    for (int i = 0; i < 32 && i < total_info; i++) {
        int bytepos = i / 8;
        int bitpos = 7 - (i % 8);
        uint8_t bit = (direct_out[bytepos] >> bitpos) & 1;
        fprintf(stderr, "%u", bit);
    }
    fprintf(stderr, "\n");

    /* Puncture/depuncture test across puncturer ids: validate mapping for multiple modes */
    int p_ids[] = { 0, 1, 2, 3, 4, 5, 6 };
    const int num_p = sizeof(p_ids)/sizeof(p_ids[0]);
    const int puncted_request_len = 432;
    uint8_t *puncted2 = (uint8_t*)malloc(puncted_request_len);
    uint16_t *punct_in_costs = (uint16_t*)malloc(sizeof(uint16_t) * puncted_request_len);
    uint16_t *recon_costs = (uint16_t*)malloc(sizeof(uint16_t) * coded_bits);
    int *imap = (int*)malloc(sizeof(int) * (puncted_request_len + 1));

    for (int pi = 0; pi < num_p; pi++) {
        int p_id = p_ids[pi];
        fprintf(stderr, "\n[TETRA TEST REAL] === Testing puncturer id %d ===\n", p_id);
        int K = puncted_request_len;
        int rows = 0, cols = 0;
        if (tetra_get_interleaver_dims(p_id, K, &rows, &cols) == 0) {
            fprintf(stderr, "[TETRA TEST REAL] interleaver dims for K=%d -> a=%d cols=%d\n", K, rows, cols);
        } else {
            rows = 11; cols = (K + rows - 1) / rows;
            fprintf(stderr, "[TETRA TEST REAL] interleaver dims unknown for K=%d, fallback a=%d cols=%d\n", K, rows, cols);
        }
        for (int i = 1; i <= K; i++) imap[i] = 1 + ((rows * i) % K);
        fprintf(stderr, "[TETRA TEST REAL] calling get_punctured_rate() p_id=%d coded_bits=%d\n", p_id, coded_bits);
        int rc = get_punctured_rate(p_id, (uint8_t*)coded, puncted_request_len, puncted2);
        fprintf(stderr, "[TETRA TEST REAL] get_punctured_rate returned %d\n", rc);
        if (rc != 0) {
            fprintf(stderr, "[TETRA TEST REAL] puncture failed (rc=%d)\n", rc);
            continue;
        }

        int p_wrote = puncted_request_len;
        for (int i = 0; i < p_wrote; i++) punct_in_costs[i] = (puncted2[i] ? 0xFFFF : 0x0000);
        for (int i = 0; i < coded_bits; i++) recon_costs[i] = 0x7FFF;

        fprintf(stderr, "[TETRA TEST REAL] puncture wrote=%d\n", p_wrote);
        fprintf(stderr, "[TETRA TEST REAL] first 64 puncted bytes:\n");
        for (int i = 0; i < 64 && i < p_wrote; i++) fprintf(stderr, "%u", puncted2[i]);
        fprintf(stderr, "\n[TETRA TEST REAL] first 64 punct_in_costs:\n");
        for (int i = 0; i < 64 && i < p_wrote; i++) fprintf(stderr, "%u", (punct_in_costs[i] > 0x7FFF) ? 1 : 0);
        fprintf(stderr, "\n");

        int dr = tetra_rcpc_depuncture_by_id(p_id, punct_in_costs, p_wrote, recon_costs, coded_bits);
        if (dr < 0) {
            fprintf(stderr, "[TETRA TEST REAL] depuncture (e2e) failed for p_id=%d\n", p_id);
            continue;
        }
    /* Diagnostic: convert recon_costs to hard bits and compare to original coded bits */
    uint8_t *recon_bits = (uint8_t*)malloc(coded_bits);
    for (int i = 0; i < coded_bits; i++) recon_bits[i] = (recon_costs[i] > 0x7FFF) ? 1 : 0;

    fprintf(stderr, "[TETRA TEST REAL] first 64 coded bits:\n");
    for (int i = 0; i < 64 && i < coded_bits; i++) fprintf(stderr, "%u", coded[i]);
    fprintf(stderr, "\n[TETRA TEST REAL] first 64 recon bits:\n");
    for (int i = 0; i < 64 && i < coded_bits; i++) fprintf(stderr, "%u", recon_bits[i]);
    fprintf(stderr, "\n");

    /* Print first mappings j->k for the puncturer to help debug alignment */
        int recon_mism = 0;
        fprintf(stderr, "[TETRA TEST REAL] recon vs coded mismatches (first 32):\n");
        for (int i = 0; i < 32 && i < coded_bits; i++) {
            if (recon_bits[i] != coded[i]) {
                if (recon_mism < 16) fprintf(stderr, " pos=%3d coded=%u recon=%u\n", i, coded[i], recon_bits[i]);
                recon_mism++;
            }
        }
        fprintf(stderr, "[TETRA TEST REAL] recon mismatches count (first32)=%d\n", recon_mism);
    fprintf(stderr, "[TETRA TEST REAL] puncturer j->k mapping (first 64):\n");
    for (int j = 1; j <= 64; j++) {
        int k = tetra_rcpc_map_j_to_k(p_id, (uint32_t)j);
        fprintf(stderr, " j=%3d -> k=%4d\n", j, k);
    }

    /* Trace some mismatched positions through the full pipeline for debugging. */
    int sample_idxs[] = {2,3,5,6,11,13,15,19,21,23,27,29,31};
    fprintf(stderr, "[TETRA TEST REAL] Detailed trace for sample mismatches:\n");
    for (size_t si = 0; si < sizeof(sample_idxs)/sizeof(sample_idxs[0]); si++) {
        int idx = sample_idxs[si];
        if (idx < 0 || idx >= coded_bits) continue;
        int coded_bit = coded[idx];
        int recon_bit = recon_bits[idx];
        uint16_t recon_cost = recon_costs[idx];
        /* Find j such that map_j_to_k(j) == idx+1 */
        int found_j = -1;
        for (int j = 1; j <= p_wrote; j++) {
            int k = tetra_rcpc_map_j_to_k(p_id, (uint32_t)j);
            if (k == idx+1) { found_j = j; break; }
        }
        if (found_j < 0) {
            fprintf(stderr, " idx=%3d: coded=%u recon=%u recon_cost=0x%04x (no j found)\n", idx, coded_bit, recon_bit, recon_cost);
            continue;
        }
        int j = found_j;
        int k = tetra_rcpc_map_j_to_k(p_id, (uint32_t)j);
        uint8_t puncted_val = puncted2[j-1];
        uint16_t punct_cost = punct_in_costs[j-1];
        uint16_t soft_deint_val = (mother_costs != NULL && k-1 < coded_bits) ? mother_costs[k-1] : 0xFFFF;
        fprintf(stderr, " idx=%3d: coded=%u recon=%u recon_cost=0x%04x | j=%3d k=%3d puncted[j]=%u punct_cost=0x%04x soft_deint[k]=0x%04x\n",
            idx, coded_bit, recon_bit, recon_cost, j, k, puncted_val, punct_cost, soft_deint_val);
    }

    /* Comprehensive mapping for all mismatches (limit output to first 128 mismatches) */
    fprintf(stderr, "[TETRA TEST REAL] Comprehensive mismatch mapping (first 128):\n");
    int mism_out = 0;
    for (int idx = 0; idx < coded_bits && mism_out < 128; idx++) {
        if (recon_bits[idx] == coded[idx]) continue;
        int found_j = -1;
        for (int j = 1; j <= p_wrote; j++) {
            int k = tetra_rcpc_map_j_to_k(p_id, (uint32_t)j);
            if (k == idx+1) { found_j = j; break; }
        }
        if (found_j < 0) {
            fprintf(stderr, " mism_idx=%3d: coded=%u recon=%u (UNMAPPED k=%d)\n", idx, coded[idx], recon_bits[idx], idx+1);
            mism_out++;
            continue;
        }
        int j = found_j;
        int k = tetra_rcpc_map_j_to_k(p_id, (uint32_t)j);
        uint8_t puncted_val = puncted2[j-1];
        uint16_t punct_cost = punct_in_costs[j-1];
        uint16_t soft_deint_val = (mother_costs != NULL && k-1 < coded_bits) ? mother_costs[k-1] : 0xFFFF;
        fprintf(stderr, " mism_idx=%3d: coded=%u recon=%u | j=%3d k=%3d puncted[j]=%u punct_cost=0x%04x soft_deint[k]=0x%04x\n",
            idx, coded[idx], recon_bits[idx], j, k, puncted_val, punct_cost, soft_deint_val);
        mism_out++;
    }

    /* Now print only the mapped mismatches (those with non-neutral recon_costs) */
    fprintf(stderr, "[TETRA TEST REAL] Mapped mismatches (first 64):\n");
    int mapped_mism = 0;
    for (int idx = 0; idx < coded_bits && mapped_mism < 64; idx++) {
        if (recon_bits[idx] == coded[idx]) continue;
        if (recon_costs[idx] == 0x7FFF) continue; /* punctured/unmapped */
        /* find j mapping */
        int found_j = -1;
        for (int j = 1; j <= p_wrote; j++) {
            int k = tetra_rcpc_map_j_to_k(p_id, (uint32_t)j);
            if (k == idx+1) { found_j = j; break; }
        }
        if (found_j < 0) {
            fprintf(stderr, " mapped_idx=%3d: coded=%u recon=%u (no j found, but recon_cost not neutral)\n", idx, coded[idx], recon_bits[idx]);
            mapped_mism++;
            continue;
        }
        int j = found_j;
        int k = tetra_rcpc_map_j_to_k(p_id, (uint32_t)j);
        uint8_t puncted_val = puncted2[j-1];
        uint16_t punct_cost = punct_in_costs[j-1];
        uint16_t soft_deint_val = (mother_costs != NULL && k-1 < coded_bits) ? mother_costs[k-1] : 0xFFFF;
        fprintf(stderr, " mapped_idx=%3d: coded=%u recon=%u | j=%3d k=%3d puncted[j]=%u punct_cost=0x%04x soft_deint[k]=0x%04x recon_cost=0x%04x\n",
            idx, coded[idx], recon_bits[idx], j, k, puncted_val, punct_cost, soft_deint_val, recon_costs[idx]);
        mapped_mism++;
    }

    /* Step 1: list all mother-code positions k that were NOT mapped by any j (i.e., punctured)
     * Build mapped_k boolean array by iterating j=1..p_wrote and marking k = map(j).
     */
    int *mapped_k = (int*)calloc(coded_bits + 1, sizeof(int));
    for (int j = 1; j <= p_wrote; j++) {
        int k = tetra_rcpc_map_j_to_k(p_id, (uint32_t)j);
        if (k >= 1 && k <= coded_bits) mapped_k[k] = 1;
    }
    int unmapped_count = 0;
    for (int k = 1; k <= coded_bits; k++) if (!mapped_k[k]) unmapped_count++;
    fprintf(stderr, "[TETRA TEST REAL] total coded K=%d mapped_count=%d unmapped_count=%d\n", coded_bits, coded_bits - unmapped_count, unmapped_count);
    /* Print first 64 unmapped k indices for inspection */
    int printed = 0;
    fprintf(stderr, "[TETRA TEST REAL] sample unmapped k indices (first 64):\n");
    for (int k = 1; k <= coded_bits && printed < 64; k++) {
        if (!mapped_k[k]) {
            fprintf(stderr, " k=%4d\n", k);
            printed++;
        }
    }
    /* Also write full unmapped list to file for external inspection */
    FILE *uf = fopen("tetra_unmapped_k.txt", "w");
    if (uf) {
        fprintf(uf, "# unmapped k indices for coded_bits=%d\n", coded_bits);
        fprintf(uf, "mapped_count=%d unmapped_count=%d\n", coded_bits - unmapped_count, unmapped_count);
        for (int k = 1; k <= coded_bits; k++) {
            if (!mapped_k[k]) fprintf(uf, "%d\n", k);
        }
        fclose(uf);
        fprintf(stderr, "[TETRA TEST REAL] wrote unmapped k list to tetra_unmapped_k.txt\n");
    } else {
        fprintf(stderr, "[TETRA TEST REAL] failed to open tetra_unmapped_k.txt for writing\n");
    }
    /* Also show how many of the original mismatch positions fall into unmapped k */
    int mism_in_unmapped = 0;
    for (int i = 0; i < 32 && i < coded_bits; i++) {
        if (recon_bits[i] != coded[i]) {
            if (!mapped_k[i+1]) mism_in_unmapped++;
        }
    }
    fprintf(stderr, "[TETRA TEST REAL] of first-32 mismatches, %d are unmapped (punctured) positions\n", mism_in_unmapped);
    /* Print a sample of mapped mother-code positions and their soft-costs for inspection */
    fprintf(stderr, "[TETRA TEST REAL] Sample mapped positions (first 32):\n");
    int printed_m = 0;
    for (int k = 1; k <= coded_bits && printed_m < 32; k++) {
        if (!mapped_k[k]) continue;
        uint16_t m_cost = mother_costs[k-1];
        uint16_t r_cost = recon_costs[k-1];
        /* find j mapping to this k */
        int found_j = -1;
        for (int j = 1; j <= p_wrote; j++) {
            int kk = tetra_rcpc_map_j_to_k(p_id, (uint32_t)j);
            if (kk == k) { found_j = j; break; }
        }
        if (found_j < 0) {
            fprintf(stderr, " k=%4d mapped but no j found | mother_cost=0x%04x recon_cost=0x%04x\n", k, m_cost, r_cost);
        } else {
            fprintf(stderr, " k=%4d <- j=%3d | mother_cost=0x%04x recon_cost=0x%04x puncted[j]=%u punct_in_cost=0x%04x\n",
                k, found_j, m_cost, r_cost, puncted2[found_j-1], punct_in_costs[found_j-1]);
        }
        printed_m++;
    }
    free(mapped_k);

    uint8_t out_bytes[256];
    for (int i = 0; i < (int)sizeof(out_bytes); i++) out_bytes[i] = 0;
    uint32_t errs = viterbi_decode(out_bytes, recon_costs, (uint16_t)coded_bits);

    /* Unpack bits and compare first info_bits to src (try small align offsets) */
    int best_offset = 0;
    int best_mism = info_bits + 1;
    for (int off = 0; off <= 8; off++) {
        int mism_t = 0;
        for (int i = 0; i < info_bits; i++) {
            int bit_idx = i + off;
            int bytepos = bit_idx / 8;
            int bitpos = 7 - (bit_idx % 8);
            uint8_t bit = (out_bytes[bytepos] >> bitpos) & 1;
            if (bit != src[i]) mism_t++;
        }
        if (mism_t < best_mism) { best_mism = mism_t; best_offset = off; }
    }

    fprintf(stderr, "[TETRA TEST REAL] best alignment offset=%d mismatches=%d viterbi_err_cost=%u\n", best_offset, best_mism, errs);

    free(src);
    free(coded);
    free(mother_costs);
    free(puncted2);
    free(punct_in_costs);
    free(recon_costs);
    free(recon_bits);

    if (best_mism == 0) {
        fprintf(stderr, "[TETRA TEST REAL] Encode->puncture->depuncture->viterbi OK\n");
        return 0;
    }
    return 8;
}
