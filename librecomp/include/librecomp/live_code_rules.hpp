#ifndef __RECOMP_LIVE_CODE_RULES_HPP__
#define __RECOMP_LIVE_CODE_RULES_HPP__

// [wcw fullconv] GameShark codes that rewrite game instructions while the game runs (aki-roadmap#187). Recompiled code
// never reads its instructions back, so such a write would do nothing. After each pass of the native codes table the
// runtime compares the words the table can write inside known functions with the ROM. A function that differs is
// recompiled from RDRAM, cached by the values of those words, and the static function's entry jumps to that version
// until the words match the ROM again.
//
// This file holds the decisions and the arithmetic, unit tested in tests/wcw/live_code_rules_tests.cpp. The runtime
// half (compiling, patching entries) is in mods.cpp.

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_set>
#include <vector>

#include "librecomp/conversion_codes.hpp"

namespace recomp::mods::live_codes {
    struct FunctionSpan {
        uint32_t vram;
        uint32_t rom;
        uint32_t size;
        size_t id;       // the caller's handle for this function
    };

    struct Watch {
        FunctionSpan func;
        std::vector<uint32_t> words; // addresses of the words the table can write, sorted
    };

    // Every word address a table can write, repeaters expanded: sorted, no duplicates. Conditions aren't evaluated, so
    // this is what the table could ever write.
    std::vector<uint32_t> written_words(const std::vector<conversion::codes::Code> &codes);

    // The functions those words fall in. Overlays share addresses, so one word can land in one function per overlay.
    // Functions with no ROM code (size 0) and no written word are left out.
    std::vector<Watch> build_watches(const std::vector<uint32_t> &words, std::span<const FunctionSpan> functions);

    // Do the watched words in RDRAM (host-order words) equal the big-endian ROM? False if any is out of range.
    bool watched_match_rom(const Watch &watch, const uint8_t *rdram, uint32_t rdram_size, std::span<const uint8_t> rom);

    // Does every word of the function the table can't write equal the ROM? False means something other than this
    // function's code is there, so compiling it would be wrong.
    bool others_match_rom(const Watch &watch, const uint8_t *rdram, uint32_t rdram_size, std::span<const uint8_t> rom);

    // A range of addresses holding known code: [start, end).
    struct CodeRange {
        uint32_t start;
        uint32_t end;
    };

    bool in_code_ranges(uint32_t addr, std::span<const CodeRange> code);

    // The words a codes table can write as ranges, for checking a hook's target. Unsorted, repeats are fine.
    std::vector<CodeRange> ranges_of_words(const std::vector<uint32_t> &words);

    // Does a watched word now leave the function in a way the ROM's word didn't: a j or jal outside these ranges, a
    // branch outside the function, or a jr or jalr through a register other than ra? Such a word hooks into code the
    // pack puts in RAM. A j or jal into `pack_writes` (what the table can write) is that code, as long as something is
    // there: a hook can go live before its code does. Anywhere else nothing says it's code, and a hook with old-revision
    // addresses lands there (aki-roadmap#205), so the version isn't used. A jr built from lui/ori is caught by its jr only
    // if the pack writes the jr.
    bool hooks_outside_code(const Watch &watch, const uint8_t *rdram, uint32_t rdram_size, std::span<const uint8_t> rom, std::span<const CodeRange> code,
        std::span<const CodeRange> pack_writes = {});

    // What to do about a version's exits, given every section's code (all) and what's in memory now (loaded).
    enum class ExitCheck {
        Fine,     // run it
        NotNow,   // it jumps to an overlay that isn't loaded: not now, but this content may run later
        Never,    // it jumps outside all known code: this content can't run
    };

    // A jump into pack_writes with nothing there yet is NotNow: the table may write it later.
    ExitCheck check_exits(const Watch &watch, const uint8_t *rdram, uint32_t rdram_size, std::span<const uint8_t> rom,
        std::span<const CodeRange> all, std::span<const CodeRange> loaded, std::span<const CodeRange> pack_writes = {});

    // A key for the current values of the watched words. Never 0 (0 means "the static function").
    uint64_t content_key(const Watch &watch, const uint8_t *rdram, uint32_t rdram_size);

    struct FunctionState {
        uint64_t current = 0;                  // the key of the version running; 0 = static
        std::unordered_set<uint64_t> compiled; // keys with a compiled version
        std::unordered_set<uint64_t> failed;   // keys that can't run (a hook, or it won't compile): not tried again
        bool refused = false;                  // never switched (mod hook, entry prologue, repaired under Refuse...)
    };

    constexpr size_t MaxVersionsPerFunction = 256;
    constexpr int MaxCompilesPerPass = 8;

    enum class Action {
        None,          // already right, or nothing may be done
        ToStatic,      // the words match the ROM, the section isn't loaded, or this content can't run: back to static
        UseCompiled,   // a compiled version exists for this content
        Compile,       // new content, compile it
        Deferred,      // new content, but this pass has used its compile budget: try on the next pass
        AtCap,         // new content, but MaxVersionsPerFunction contents were tried: stay on the current one
    };

    Action decide(const FunctionState &state, bool section_loaded, bool at_rom, uint64_t key, int compiles_left);

    // x86-64: entry patch and stub. The patch is 5 bytes so it can't cover a suspended caller's return address: the
    // shortest call as a function's first instruction returns just past it.
    constexpr size_t EntryPatchSize = 5;
    constexpr size_t StubSize = 16;
    constexpr size_t StubTargetOffset = 8;

    // jmp rel32 at 'from' reaching 'to'. False if out of range.
    bool encode_entry_jump(uintptr_t from, uintptr_t to, uint8_t out[EntryPatchSize]);

    // jmp qword ptr [rip+2], two int3, the target: the target is 8-byte aligned when the stub is, so it can be swapped
    // with one store.
    void encode_stub(uint64_t target, uint8_t out[StubSize]);

    // Runtime half (mods.cpp): call after each pass of the table, on the game thread that ran it.
    void after_apply(const std::vector<conversion::codes::Code> &codes, uint8_t *rdram, uint32_t rdram_size);

    // For the stall watchdog (WCW_STALL_DUMP). version_at: the function whose compiled version holds this native address.
    // version_bounds: the address range the compiled versions span, 0 if there are none. Neither locks: they're
    // for the watchdog's capture, with the game threads suspended, so they read a fixed-size table.
    bool version_at(uint64_t addr, uint32_t &vram);
    void version_bounds(uintptr_t &low, uintptr_t &high);
    void print_stall_state();
}

#endif
