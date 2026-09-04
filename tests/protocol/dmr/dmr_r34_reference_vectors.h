// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_TESTS_PROTOCOL_DMR_DMR_R34_REFERENCE_VECTORS_H_
#define DSD_NEO_TESTS_PROTOCOL_DMR_DMR_R34_REFERENCE_VECTORS_H_

#include <stdint.h>

typedef struct {
    uint8_t payload[18];
    uint8_t dibits[98];
} dmr_r34_reference_vector;

static const dmr_r34_reference_vector k_dmr_r34_reference_vectors[] = {
    {
        {0x02, 0x55, 0x0B, 0x35, 0x0F, 0x9F, 0x83, 0x82, 0x35, 0xDA, 0x49, 0xFB, 0x52, 0xAC, 0xE4, 0x64, 0x5B, 0xA8},
        {0, 2, 2, 2, 2, 1, 1, 0, 3, 2, 3, 1, 3, 3, 2, 2, 3, 3, 0, 3, 3, 0, 2, 1, 0, 2, 0, 2, 3, 3, 1, 0, 1,
         3, 3, 3, 3, 2, 3, 2, 2, 1, 3, 3, 1, 0, 2, 1, 3, 3, 1, 3, 0, 0, 2, 2, 1, 0, 1, 0, 2, 3, 0, 2, 2, 0,
         1, 1, 0, 3, 3, 2, 3, 1, 1, 1, 2, 0, 2, 2, 1, 1, 2, 3, 3, 1, 2, 2, 3, 1, 2, 2, 2, 2, 2, 0, 0, 3},
    },
    {
        {0x90, 0x32, 0xA5, 0x94, 0x3D, 0x76, 0x39, 0x39, 0xB9, 0x7F, 0xE8, 0x08, 0xAB, 0x27, 0x83, 0xBE, 0x51, 0xF8},
        {1, 3, 2, 1, 1, 2, 2, 2, 2, 0, 0, 3, 2, 0, 0, 2, 2, 0, 2, 1, 3, 0, 0, 3, 0, 2, 2, 2, 1, 3, 1, 1, 3,
         1, 0, 2, 1, 2, 1, 1, 3, 3, 2, 2, 2, 2, 0, 0, 1, 0, 3, 3, 3, 3, 0, 3, 3, 3, 2, 3, 0, 1, 2, 0, 3, 1,
         0, 3, 1, 3, 3, 2, 2, 0, 0, 1, 1, 1, 0, 1, 3, 2, 2, 1, 1, 0, 1, 3, 3, 2, 2, 3, 0, 1, 1, 1, 2, 3},
    },
    {
        {0x3D, 0xD6, 0x40, 0x81, 0x0B, 0x98, 0xBC, 0x52, 0x42, 0x9A, 0x00, 0x25, 0x2F, 0x64, 0xBA, 0x72, 0xE5, 0x96},
        {3, 1, 1, 1, 1, 3, 2, 0, 2, 0, 3, 0, 3, 3, 0, 2, 3, 0, 2, 3, 2, 1, 2, 3, 1, 3, 3, 1, 2, 1, 3, 3, 3,
         3, 0, 0, 0, 1, 1, 2, 0, 2, 2, 0, 1, 2, 0, 3, 0, 3, 3, 1, 3, 2, 3, 2, 1, 0, 2, 3, 3, 2, 0, 2, 1, 3,
         2, 2, 3, 0, 1, 1, 2, 3, 3, 0, 0, 2, 2, 2, 1, 2, 2, 0, 3, 2, 3, 3, 1, 1, 3, 2, 0, 2, 3, 3, 0, 3},
    },
};

enum { DMR_R34_REFERENCE_VECTOR_COUNT = 3 };

#endif /* DSD_NEO_TESTS_PROTOCOL_DMR_DMR_R34_REFERENCE_VECTORS_H_ */
