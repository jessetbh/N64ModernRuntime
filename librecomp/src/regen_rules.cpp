#include "librecomp/regen_rules.hpp"

namespace recomp::mods::regen_rules {

bool has_delay_slot(uint32_t instr) {
    const uint32_t op = instr >> 26;
    switch (op) {
        case 0x00: { // SPECIAL: jr, jalr
            const uint32_t funct = instr & 0x3F;
            return funct == 0x08 || funct == 0x09;
        }
        case 0x01: { // REGIMM: bltz, bgez, bltzl, bgezl, bltzal, bgezal, bltzall, bgezall
            const uint32_t rt = (instr >> 16) & 0x1F;
            return (rt & ~0x13u) == 0; // rt 0x00-0x03, 0x10-0x13 (not tgei etc. at 0x08-0x0E)
        }
        case 0x02: case 0x03:              // j, jal
        case 0x04: case 0x05: case 0x06: case 0x07: // beq, bne, blez, bgtz
        case 0x14: case 0x15: case 0x16: case 0x17: // the likely forms
            return true;
        case 0x11: // COP1: bc1f, bc1t, bc1fl, bc1tl
            return ((instr >> 21) & 0x1F) == 0x08;
        default:
            return false;
    }
}

size_t words_to_decode(size_t num_words, uint32_t last_instr, size_t available) {
    if (num_words != 0 && available != 0 && has_delay_slot(last_instr)) {
        return num_words + 1;
    }
    return num_words;
}

}
