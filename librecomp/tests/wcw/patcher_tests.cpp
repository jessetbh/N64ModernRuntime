//
// librecomp
//

// [wcw fullconv] Unit tests for the BPS patcher's source ROM check (aki-roadmap#173). Built and run as part of every
// librecomp build (LIBRECOMP_WCW_TESTS); a failure fails the build.

#include "librecomp/patcher.hpp"
#include "recompiler/runs_off_end.h"

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
