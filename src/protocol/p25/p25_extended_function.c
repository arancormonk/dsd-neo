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
