//
// librecomp
//

// [wcw fullconv] Unit tests for the BPS patcher's source ROM check (aki-roadmap#173). Built and run as part of every
// librecomp build (LIBRECOMP_WCW_TESTS); a failure fails the build.

#include "librecomp/patcher.hpp"
#include "recompiler/jump_table_bound.h"
#include "recompiler/runs_off_end.h"
#include "recompiler/constant_ra.h"

#include <cstdint>
#include <string>
#include <vector>

#include "wcw_test.h"

using recomp::patcher::PatcherResult;
using recomp::patcher::patch_rom;

namespace {
    using Bytes = std::vector<uint8_t>;

    // Bitwise CRC-32, independent of the patcher's table (check value: "123456789" -> 0xCBF43926).
    uint32_t crc32_of(const Bytes &data) {
        uint32_t crc = 0xFFFFFFFF;
        for (uint8_t byte : data) {
            crc ^= byte;
            for (int bit = 0; bit < 8; bit++) {
                crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
            }
        }
        return ~crc;
    }

    void put_number(Bytes &out, uint64_t value) {
        while (true) {
            uint8_t low = value & 0x7f;
            value >>= 7;
            if (value == 0) {
                out.push_back(low | 0x80);
                return;
            }
            out.push_back(low);
            value--;
        }
    }

    void put_u32(Bytes &out, uint32_t value) {
        for (int i = 0; i < 4; i++) {
            out.push_back((value >> (8 * i)) & 0xff);
        }
    }

    // SourceRead of the first `keep` bytes, TargetRead of `tail`, the way a BPS stores one changed byte.
    Bytes make_patch(const Bytes &source, size_t keep, const Bytes &tail) {
        Bytes target(source.begin(), source.begin() + keep);
        target.insert(target.end(), tail.begin(), tail.end());

        Bytes patch{ 'B', 'P', 'S', '1' };
        put_number(patch, source.size());
        put_number(patch, target.size());
        put_number(patch, 0);
        put_number(patch, ((keep - 1) << 2) | 0);
        put_number(patch, ((tail.size() - 1) << 2) | 1);
        patch.insert(patch.end(), tail.begin(), tail.end());
        put_u32(patch, crc32_of(source));
        put_u32(patch, crc32_of(target));
        put_u32(patch, crc32_of(patch));
        return patch;
    }
}

TEST_CASE("patcher_crc_helper_matches_the_standard_check_value") {
    const std::string text = "123456789";
    CHECK(crc32_of(Bytes(text.begin(), text.end())) == 0xCBF43926u);
}

TEST_CASE("patcher_applies_a_patch_to_the_rom_it_was_built_on") {
    const Bytes rom{ 0x90, 0xE7, 0xA1, 0x70 };
    const Bytes patch = make_patch(rom, 3, { 0xF0 });
    Bytes out;
    CHECK(patch_rom(rom, patch, out) == PatcherResult::Success);
    CHECK((out == Bytes{ 0x90, 0xE7, 0xA1, 0xF0 }));
}

// aki-roadmap#173: a patch built on WM2000's "V1.2" hack, applied to the retail ROM, turned 90E7A1F0 into 000000F0 (the
// kept bytes came from retail's zeros) and the game hit a trap at boot.
TEST_CASE("patcher_refuses_a_patch_built_on_another_rom") {
    const Bytes built_on{ 0x90, 0xE7, 0xA1, 0x70 };
    const Bytes players_rom{ 0x00, 0x00, 0x00, 0x00 };
    const Bytes patch = make_patch(built_on, 3, { 0xF0 });
    Bytes out;
    CHECK(patch_rom(players_rom, patch, out) == PatcherResult::WrongRom);
    CHECK(out.empty());
}

TEST_CASE("patcher_refuses_a_one_byte_difference_in_the_source") {
    const Bytes built_on{ 1, 2, 3, 4, 5, 6, 7, 8 };
    Bytes other = built_on;
    other[7] ^= 1;
    const Bytes patch = make_patch(built_on, 4, { 9 });
    Bytes out;
    CHECK(patch_rom(other, patch, out) == PatcherResult::WrongRom);
}

TEST_CASE("patcher_still_reports_a_bad_header_as_invalid") {
    const Bytes rom{ 1, 2, 3, 4 };
    Bytes patch = make_patch(rom, 3, { 9 });
    patch[0] = 'X';
    Bytes out;
    CHECK(patch_rom(rom, patch, out) == PatcherResult::InvalidPatchFile);
}

// aki-roadmap#173: House Party Mixtape's hook returns to 0x8001E340, one word before func_8001E344. With no tail call the
// generated code ran into whatever follows the batch (the first switch error stub).
// A hand-written switch head, as Function::words holds it: in ROM byte order (aki-roadmap#206).
static uint32_t rom_order(uint32_t w) {
    return (w >> 24) | ((w >> 8) & 0xFF00) | ((w << 8) & 0xFF0000) | (w << 24);
}

TEST_CASE("jump_table_bound_reads_words_in_rom_byte_order") {
    using N64Recomp::find_jump_table_bound;
    const uint32_t v0 = 2;
    const uint32_t words[] = {
        rom_order(0x2C62000Bu),  // sltiu $v0, $v1, 11
        rom_order(0x10400010u),  // beqz $v0
        rom_order(0x00031080u),  // sll $v0, $v1, 2
        rom_order(0x3C018013u),  // lui $at, 0x8013
        rom_order(0x00220821u),  // addu $at, $at, $v0
    };
    auto found = find_jump_table_bound(words, 5, 4, v0);
    CHECK(found.has_value());
    CHECK(found.value_or(0) == 11);
}

TEST_CASE("jump_table_bound_needs_the_whole_pattern") {
    using N64Recomp::find_jump_table_bound;
    const uint32_t v0 = 2;
    const uint32_t no_guard[] = { rom_order(0x00031080u), rom_order(0x3C018013u), rom_order(0x00220821u) };
    CHECK(!find_jump_table_bound(no_guard, 3, 2, v0).has_value());
    const uint32_t index_rewritten[] = {
        rom_order(0x2C62000Bu),  // sltiu $v0, $v1, 11
        rom_order(0x24630001u),  // addiu $v1, $v1, 1: not the index the guard checked
        rom_order(0x00031080u), rom_order(0x3C018013u), rom_order(0x00220821u),
    };
    CHECK(!find_jump_table_bound(index_rewritten, 5, 4, v0).has_value());
    CHECK(!find_jump_table_bound(no_guard, 3, 3, v0).has_value());   // addu index past the function
}

TEST_CASE("runs_off_end_counts_a_one_word_function") {
    using N64Recomp::function_runs_off_end;
    const uint32_t slti = 0x2A220004, nop = 0, j = 0x08000000, jr_ra = 0x03E00008, b = 0x10000002, eret = 0x42000018;
    CHECK(function_runs_off_end(1, slti, 0, false));
    CHECK(function_runs_off_end(1, nop, 0, false));
    CHECK(!function_runs_off_end(1, j, 0, false));
    CHECK(!function_runs_off_end(1, jr_ra, 0, false));
    CHECK(!function_runs_off_end(1, eret, 0, false));
    CHECK(!function_runs_off_end(0, 0, 0, false));
    // The rules for longer functions stay: a delay slot after a jump ends, a branch landing on it runs on (aki-roadmap#76).
    CHECK(function_runs_off_end(2, slti, nop, false));
    CHECK(!function_runs_off_end(2, nop, j, false));
    CHECK(function_runs_off_end(2, nop, j, true));
    CHECK(!function_runs_off_end(2, b, nop, true));
}

// No Mercy Plus V3, Jericho's entrance: payload 0x804715EC builds ra = 0x8010F8FC and its jr ra at 0x80471634 jumps
// there (the middle of ovl_b 0x8010F794). Compiled as a plain return, 0x8010F794 returned without its epilogue and the
// main loop's s2 came back 0 (crash). constant_ra.h decides when a jr ra is that jump.
TEST_CASE("constant_ra_pair_finds_a_hacks_fake_return_address") {
    using N64Recomp::constant_ra_pair;
    uint32_t v = 0;
    CHECK(constant_ra_pair(0x3C1F8011u, 0x27FFF8FCu, v));          // lui $ra, 0x8011; addiu $ra, $ra, -0x704
    CHECK(v == 0x8010F8FCu);
    CHECK(constant_ra_pair(0x3C1F8010u, 0x37FFF8FCu, v));          // ori $ra, $ra, 0xF8FC
    CHECK(v == 0x8010F8FCu);
    CHECK(!constant_ra_pair(0x3C088011u, 0x27FFF8FCu, v));         // the lui writes $t0, not $ra
    CHECK(!constant_ra_pair(0x3C1F8011u, 0x2508F8FCu, v));         // addiu $t0, $t0: not $ra
    CHECK(!constant_ra_pair(0x3C1F8011u, 0x27E8F8FCu, v));         // addiu $t0, $ra: $ra itself unchanged
    CHECK(!constant_ra_pair(0x00000000u, 0x27FFF8FCu, v));         // no lui before it
    CHECK(!constant_ra_pair(0x3C1F0000u, 0x27FF1234u, v));         // not a KSEG0 code address
    CHECK(!constant_ra_pair(0x3C1F8011u, 0x27FFF8FEu, v));         // misaligned
    CHECK(!constant_ra_pair(0x3C1F8011u, 0x8FBF0024u, v));         // lw $ra, 0x24($sp)
}

TEST_CASE("constant_ra_pair_edges") {
    using N64Recomp::constant_ra_pair;
    uint32_t v = 0;
    CHECK(constant_ra_pair(0x3C1F8080u, 0x27FFFFFCu, v));          // 0x807FFFFC: the last code address taken
    CHECK(v == 0x807FFFFCu);
    CHECK(!constant_ra_pair(0x3C1F8080u, 0x27FF0000u, v));         // 0x80800000: past RDRAM
    CHECK(!constant_ra_pair(0x3C3F8011u, 0x27FFF8FCu, v));         // rs set in the lui: not a plain lui
}

TEST_CASE("take_constant_ra_matches_the_noting_frame_only") {
    using namespace N64Recomp;
    const uintptr_t frame = 0x1000, deeper = 0x0F00, shallower = 0x1100;
    ConstantRa s{};
    CHECK(!take_constant_ra(s, 0x8010F8FCu, frame));               // nothing noted: a plain return
    note_constant_ra(s, 0x8010F8FCu, frame);
    CHECK(take_constant_ra(s, 0x8010F8FCu, frame));                // same frame (the hack's j chain), ra = T: jump
    CHECK(!take_constant_ra(s, 0x8010F8FCu, frame));               // taken: the next jr ra is a return again
    // A function called from the noting one (calls don't write ra, so it still sees T) returns normally and keeps
    // the note for the noting frame's own jr ra.
    note_constant_ra(s, 0x8010F8FCu, frame);
    CHECK(!take_constant_ra(s, 0x8010F8FCu, deeper));
    CHECK(take_constant_ra(s, 0x8010F8FCu, frame));
    // ra changed since: a return, and the note is gone
    note_constant_ra(s, 0x8010F8FCu, frame);
    CHECK(!take_constant_ra(s, 0x80026A20u, frame));
    CHECK(!take_constant_ra(s, 0x8010F8FCu, frame));
    // the noting frame returned another way: a shallower jr ra drops the note, even with ra still T
    note_constant_ra(s, 0x8010F8FCu, frame);
    CHECK(!take_constant_ra(s, 0x8010F8FCu, shallower));
    CHECK(!take_constant_ra(s, 0x8010F8FCu, frame));
}
