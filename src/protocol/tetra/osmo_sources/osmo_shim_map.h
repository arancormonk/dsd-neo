#ifndef TETRA_OSMO_SHIM_MAP_H
#define TETRA_OSMO_SHIM_MAP_H

#include <stdint.h>

#define OSO_SHIM_POS_THRESH 64
#define OSO_SHIM_NEG_THRESH -64

/* Map an osmo int8 soft-symbol to a 16-bit viterbi cost.
 * Returns 0xFFFF for strong '1', 0x0000 for strong '0', 0x7FFF for neutral.
 */
uint16_t osmo_soft_to_cost(int8_t v);

#endif /* TETRA_OSMO_SHIM_MAP_H */
