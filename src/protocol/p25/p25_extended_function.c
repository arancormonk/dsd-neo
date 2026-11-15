// SPDX-License-Identifier: ISC
#include "p25_extended_function.h"

const char*
p25_extended_function_class0_operand_label(uint8_t operand) {
    switch (operand & 0x7Fu) {
        case 0x00: return "Radio Check";
        case 0x7D: return "Radio Detach";
        case 0x7E: return "Radio Uninhibit";
        case 0x7F: return "Radio Inhibit";
        default: return "Reserved";
    }
}

int
p25_extended_function_operand_is_ack(uint8_t operand) {
    return (operand & 0x80u) != 0u;
}

/* TIA-102.AABC-D §2.3.15 Table 2.3.15-1, Class $00 (control command - unit): $00 / $7D / $7E / $7F are the commands,
   $80 / $FD / $FE / $FF their acknowledgements, every other operand reserved. */
const char*
p25_extended_function_class0_label(uint8_t operand) {
    switch (operand) {
        case 0x00: return "Radio Check";
        case 0x7D: return "Radio Detach";
        case 0x7E: return "Radio Uninhibit";
        case 0x7F: return "Radio Inhibit";
        case 0x80: return "Radio Check Ack";
        case 0xFD: return "Radio Detach Ack";
        case 0xFE: return "Radio Uninhibit Ack";
        case 0xFF: return "Radio Inhibit Ack";
        default: return "Reserved";
    }
}
