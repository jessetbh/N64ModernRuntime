#ifndef __RECOMP_CONVERSION_CODES_HPP__
#define __RECOMP_CONVERSION_CODES_HPP__

// [wcw fullconv] A conversion's GameShark codes run natively by the runtime, as one compact table applied once per
// game frame (the host export aki_conversion_apply, called from the AkiMods cheats mod's frame hook).
//
// Why per frame and not a one-time RAM init: a code that writes a constant can't be told apart, statically, from one
// the game overwrites later (Project64 re-applies every cheat continuously, and big legacy sets like Showdown 64's
// rely on that). So nothing is compiled away; what changes is where the codes run: natively, from a table with no
// size cap, instead of the mod's 4096-entry list.
//
// The grammar and the unit rules are the AkiMods cheats engine's (mods/cheats/src/codes_engine.h, README "Codes
// files"), so a file behaves the same on either path:
//   80/A0 8-bit write, 81/A1 16-bit write, D0/D1 run the next unit if the 8/16-bit value equals, D2/D3 if it differs,
//   50 00NNSS IIII repeat the next write NN times (address += SS, value += IIII). Enablers (F0 F1 F6 FF EE DE 2A 3C)
//   and button codes (88/89) are ignored. "The next unit" is a repeater with its write, or a conditional with the unit
//   it governs, so stacked conditionals all have to hold.
// Unlike the mod, this path doesn't translate revisions: a file tagged for another revision of the game is left to
// the mod (which translates No Mercy Rev 0), and aki-conv translates big sets before packaging.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace recomp::mods::conversion::codes {
    struct Code {
        uint32_t addr;   // N64 virtual address (0x80xxxxxx; A0/A1 map to the same RAM)
        uint16_t value;
        uint8_t type;    // 0x80 0x81 0xA0 0xA1 0xD0-0xD3 0x50
        uint8_t pad = 0;
    };

    struct Report {
        int added = 0;
        int writes = 0;
        int conditionals = 0;
        int repeaters = 0;
        int enablers = 0;
        int buttons = 0;
        int unknown = 0;
        int malformed = 0;
        int first_bad_line = 0;
        bool rom_mismatch = false; // the "# rom:" tag names another game or revision
        int rom_rev = -1;          // from the tag; -1 none
    };

    // "nomercy-rev1" -> ("nomercy", 1); "wt-rev0" -> ("wt", 0); "vpw2" -> ("vpw2", 0).
    void split_rom_tag(const std::string &tag, std::string &game_key, int &rev);

    // Parses a codes file for game_key at revision native_rev into 'out' (cleared first). False (and nothing in 'out')
    // when the tag names another game or revision.
    bool parse_file(std::string_view text, const std::string &game_key, int native_rev, std::vector<Code> &out, Report &report);

    // Runs the table once over the recomp's RDRAM (32-bit words in host order: byte a lives at (a ^ 3)), rdram_size
    // bytes from 0x80000000. Writes and reads outside it are skipped.
    void apply(const std::vector<Code> &codes, uint8_t *rdram, uint32_t rdram_size);
}

#endif
