#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <dsd-neo/protocol/tetra/tetra_fec.h>

/* osmo conv encoder prototypes (from osmo source copied into repo) */
struct conv_enc_state { uint8_t delayed[4]; };
int conv_enc_init(struct conv_enc_state *ces);
int conv_enc_input(struct conv_enc_state *ces, uint8_t *in, int len, uint8_t *out);

int main(void) {
    const int coded_bits = 432;
    const int total_info = coded_bits / 4;
    const int info_bits = total_info - 4;

    uint8_t *src = malloc(total_info);
    if (!src) return 1;
    for (int i = 0; i < info_bits; i++) src[i] = (i & 1);
    for (int i = info_bits; i < total_info; i++) src[i] = 0;

    uint8_t *coded = malloc(coded_bits);
    if (!coded) return 2;
    memset(coded, 0, coded_bits);
    struct conv_enc_state ces;
    conv_enc_init(&ces);
    conv_enc_input(&ces, src, total_info, coded);

    int p_ids[] = {0,1,2,3,4,5,6};
    const int num_p = sizeof(p_ids)/sizeof(p_ids[0]);

    for (int pi = 0; pi < num_p; pi++) {
        int p_id = p_ids[pi];
        uint8_t *puncted = malloc(coded_bits);
        if (!puncted) return 3;
        int rc = get_punctured_rate(p_id, coded, coded_bits, puncted);
        printf("punct_id=%d get_punctured_rate rc=%d\n", p_id, rc);
        if (rc != 0) { free(puncted); continue; }

        /* mark mapped k by iterating j=1..coded_bits */
        int *mapped_k = calloc(coded_bits + 1, sizeof(int));
        for (int j = 1; j <= coded_bits; j++) {
            int k = tetra_rcpc_map_j_to_k(p_id, (uint32_t)j);
            if (k >= 1 && k <= coded_bits) mapped_k[k] = 1;
        }
        int unmapped = 0;
        for (int k = 1; k <= coded_bits; k++) if (!mapped_k[k]) unmapped++;
        printf("  coded_bits=%d mapped=%d unmapped=%d\n", coded_bits, coded_bits - unmapped, unmapped);

        /* print first 20 unmapped k */
        int printed = 0;
        printf("  sample unmapped k:");
        for (int k = 1; k <= coded_bits && printed < 20; k++) {
            if (!mapped_k[k]) { printf(" %d", k); printed++; }
        }
        printf("\n\n");

        free(mapped_k);
        free(puncted);
    }

    free(src);
    free(coded);
    return 0;
}
