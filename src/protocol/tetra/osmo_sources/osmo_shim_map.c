#include "osmo_shim_map.h"

uint16_t osmo_soft_to_cost(int8_t v)
{
    if (v >= OSO_SHIM_POS_THRESH) return 0xFFFFu;
    if (v <= OSO_SHIM_NEG_THRESH) return 0x0000u;
    return 0x7FFFu;
}
