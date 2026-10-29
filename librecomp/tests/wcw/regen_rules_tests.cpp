//
// librecomp
//

// [wcw fullconv] Unit tests for librecomp/regen_rules (aki-roadmap#76): a regenerated function whose span ends on a
// jump or branch takes in its delay slot. Built and run as part of every librecomp build (LIBRECOMP_WCW_TESTS).

#include "librecomp/regen_rules.hpp"

#include "wcw_test.h"

using namespace recomp::mods::regen_rules;

TEST_CASE("regen rules: jumps and branches have delay slots") {
    CHECK(has_delay_slot(0x00200008u));  // jr $at (No Mercy Plus V3 func_80126FFC's last word)
    CHECK(has_delay_slot(0x03E00008u));  // jr $ra
    CHECK(has_delay_slot(0x0320F809u));  // jalr $t9
    CHECK(has_delay_slot(0x0C0502F5u));  // jal (mid_80110B04's last word)
    CHECK(has_delay_slot(0x08049FC6u));  // j
    CHECK(has_delay_slot(0x04410003u));  // bgez $v0, +3 (mid_8011BC04's last word)
    CHECK(has_delay_slot(0x04110001u));  // bal (bgezal $zero)
    CHECK(has_delay_slot(0x1000FFFFu));  // b
    CHECK(has_delay_slot(0x5440FFFBu));  // bnel
    CHECK(has_delay_slot(0x45000002u));  // bc1f
    CHECK(has_delay_slot(0x45030002u));  // bc1tl
}

TEST_CASE("regen rules: other instructions don't") {
    CHECK(!has_delay_slot(0x00000000u)); // nop
    CHECK(!has_delay_slot(0x00461021u)); // addu $v0, $v0, $a2
    CHECK(!has_delay_slot(0x0000000Du)); // break
    CHECK(!has_delay_slot(0x0000000Cu)); // syscall
    CHECK(!has_delay_slot(0x04080001u)); // tgei (REGIMM, rt 0x08)
    CHECK(!has_delay_slot(0x44820000u)); // mtc1
    CHECK(!has_delay_slot(0x42000018u)); // eret (no delay slot)
    CHECK(!has_delay_slot(0x8FBF0014u)); // lw $ra
}

TEST_CASE("regen rules: a span ending on a jump or branch takes in one more word, when there is one") {
    CHECK(words_to_decode(19, 0x00200008u, 1) == 20);
    CHECK(words_to_decode(19, 0x00200008u, 0) == 19);   // nothing follows (end of the ROM)
    CHECK(words_to_decode(19, 0x00461021u, 4) == 19);   // ends on an ordinary instruction
    CHECK(words_to_decode(0, 0x00200008u, 4) == 0);
}
