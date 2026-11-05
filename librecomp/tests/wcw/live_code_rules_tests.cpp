//
// librecomp
//

// [wcw fullconv] Unit tests for librecomp/live_code_rules (aki-roadmap#187): which words a codes table can write, which
// functions they land in, and when a function switches version. Built and run as part of every librecomp build
// (LIBRECOMP_WCW_TESTS).

#include <cstring>
#include <vector>

#include "librecomp/live_code_rules.hpp"

#include "wcw_test.h"

using namespace recomp::mods::live_codes;
using recomp::mods::conversion::codes::Code;

namespace {
    Code code(uint8_t type, uint32_t addr, uint16_t value = 0) {
        Code c{};
        c.type = type;
        c.addr = addr;
        c.value = value;
        return c;
    }

    // 16 bytes of RDRAM from 0x80000000 and the same words in the big-endian ROM at rom offset 0.
    struct Memory {
        std::vector<uint8_t> rdram = std::vector<uint8_t>(16);
        std::vector<uint8_t> rom = std::vector<uint8_t>(16);
        Memory() {
            const uint32_t words[4] = { 0x27BDFFE0u, 0x3C013E49u, 0xAFBF0018u, 0x03E00008u };
            for (int i = 0; i < 4; i++) {
                set_ram(i, words[i]);
                rom[i * 4 + 0] = uint8_t(words[i] >> 24);
                rom[i * 4 + 1] = uint8_t(words[i] >> 16);
                rom[i * 4 + 2] = uint8_t(words[i] >> 8);
                rom[i * 4 + 3] = uint8_t(words[i]);
            }
        }
        void set_ram(int index, uint32_t word) { std::memcpy(rdram.data() + index * 4, &word, 4); }
    };

    Watch watch_word1() {
        return Watch{ FunctionSpan{ 0x80000000u, 0, 16, 0 }, { 0x80000004u } };
    }
}

TEST_CASE("live codes: writes give their word address, conditions give none") {
    const std::vector<Code> codes = {
        code(0xD1, 0x80159A50, 3),      // a condition reads, it doesn't write
        code(0x81, 0x801132BA, 0x3E49), // Undertaker pin: the low half of the word at 0x801132B8
        code(0x80, 0x801132BB, 0x01),   // same word
        code(0xA0, 0x80100001, 0x01),
        code(0xA1, 0x80100006, 0x01),
    };
    const std::vector<uint32_t> expected = { 0x80100000u, 0x80100004u, 0x801132B8u };
    CHECK(written_words(codes) == expected);
}

TEST_CASE("live codes: repeaters are expanded") {
    const std::vector<Code> codes = {
        code(0x50, 0x00000304, 1), // 3 times, +4
        code(0x81, 0x80100002, 0x10),
    };
    const std::vector<uint32_t> expected = { 0x80100000u, 0x80100004u, 0x80100008u };
    CHECK(written_words(codes) == expected);
}

TEST_CASE("live codes: a repeater before a conditional writes nothing itself") {
    const std::vector<Code> codes = { code(0x50, 0x00000304, 1), code(0xD0, 0x80100000, 1), code(0x80, 0x80200000, 1) };
    const std::vector<uint32_t> expected = { 0x80200000u };
    CHECK(written_words(codes) == expected);
}

TEST_CASE("live codes: a word lands in one function per overlay sharing its address") {
    const std::vector<uint32_t> words = { 0x80100004u };
    const std::vector<FunctionSpan> functions = {
        { 0x80100000u, 0x1000, 0x10, 7 },
        { 0x80100000u, 0x2000, 0x20, 9 },
    };
    const std::vector<Watch> watches = build_watches(words, functions);
    CHECK(watches.size() == 2);
    CHECK(watches[0].func.id == 7);
    CHECK(watches[1].func.id == 9);
}

TEST_CASE("live codes: function ends are exclusive, and untouched or empty functions are left out") {
    const std::vector<uint32_t> words = { 0x80100000u, 0x80100010u };
    const std::vector<FunctionSpan> functions = {
        { 0x80100000u, 0x1000, 0x10, 1 }, // holds 0x80100000, not 0x80100010
        { 0x80100010u, 0x1010, 0x00, 2 }, // no ROM code
        { 0x80100020u, 0x1020, 0x10, 3 }, // nothing written
    };
    const std::vector<Watch> watches = build_watches(words, functions);
    CHECK(watches.size() == 1);
    CHECK(watches[0].func.id == 1);
    CHECK(watches[0].words.size() == 1);
    CHECK(watches[0].words[0] == 0x80100000u);
}

TEST_CASE("live codes: RDRAM words compare with the big-endian ROM") {
    Memory m;
    const Watch w = watch_word1();
    CHECK(watched_match_rom(w, m.rdram.data(), 16, m.rom));
    m.set_ram(1, 0x3C012FE0u);
    CHECK(!watched_match_rom(w, m.rdram.data(), 16, m.rom));
}

TEST_CASE("live codes: an unwatched word is not a code write, it marks foreign content") {
    Memory m;
    const Watch w = watch_word1();
    CHECK(others_match_rom(w, m.rdram.data(), 16, m.rom));
    m.set_ram(2, 0); // unwatched only
    CHECK(watched_match_rom(w, m.rdram.data(), 16, m.rom));
    CHECK(!others_match_rom(w, m.rdram.data(), 16, m.rom));
}

TEST_CASE("live codes: a change in the watched word is still this function's code") {
    Memory m;
    const Watch w = watch_word1();
    m.set_ram(1, 0x3C012FE0u);
    CHECK(others_match_rom(w, m.rdram.data(), 16, m.rom));
    CHECK(!watched_match_rom(w, m.rdram.data(), 16, m.rom));
}

TEST_CASE("live codes: others_match_rom fails on words out of range") {
    Memory m;
    const Watch past_ram{ FunctionSpan{ 0x80000000u, 0, 32, 0 }, { 0x80000004u } };
    CHECK(!others_match_rom(past_ram, m.rdram.data(), 16, m.rom));
    const Watch past_rom{ FunctionSpan{ 0x80000000u, 0xFFFFFFF0u, 16, 0 }, { 0x80000004u } };
    CHECK(!others_match_rom(past_rom, m.rdram.data(), 16, m.rom));
}

TEST_CASE("live codes: nested function spans each get the word (No Mercy 0x80112FE0 holds 0x801131AC)") {
    const std::vector<uint32_t> words = { 0x80113288u };
    const std::vector<FunctionSpan> functions = {
        { 0x80112FE0u, 0x1000, 808, 1 },
        { 0x801131ACu, 0x11CC, 348, 2 },
    };
    const std::vector<Watch> watches = build_watches(words, functions);
    CHECK(watches.size() == 2);
    CHECK(watches[0].func.id == 1);
    CHECK(watches[1].func.id == 2);
}

TEST_CASE("live codes: out-of-range words never match") {
    Memory m;
    const Watch past_ram{ FunctionSpan{ 0x80000000u, 0, 16, 0 }, { 0x80000010u } };
    CHECK(!watched_match_rom(past_ram, m.rdram.data(), 16, m.rom));
    const Watch past_rom{ FunctionSpan{ 0x80000000u, 0xFFFFFFF0u, 16, 0 }, { 0x80000004u } };
    CHECK(!watched_match_rom(past_rom, m.rdram.data(), 16, m.rom));
    const Watch below_ram{ FunctionSpan{ 0x7FFFFFF0u, 0, 16, 0 }, { 0x7FFFFFF4u } };
    CHECK(!watched_match_rom(below_ram, m.rdram.data(), 16, m.rom));
}

TEST_CASE("live codes: the content key follows the watched words only and is never 0") {
    Memory m;
    const Watch w = watch_word1();
    const uint64_t stock = content_key(w, m.rdram.data(), 16);
    CHECK(stock != 0);
    m.set_ram(2, 0);
    CHECK(content_key(w, m.rdram.data(), 16) == stock);
    m.set_ram(1, 0x3C012FE0u);
    const uint64_t other = content_key(w, m.rdram.data(), 16);
    CHECK(other != stock);
    m.set_ram(1, 0x3C013E49u);
    CHECK(content_key(w, m.rdram.data(), 16) == stock);
}

TEST_CASE("live codes: a jump into RAM outside known code is caught") {
    Memory m;
    const Watch w = watch_word1();
    const std::vector<CodeRange> code = { { 0x80000000u, 0x80100000u } };
    CHECK(!hooks_outside_code(w, m.rdram.data(), 16, m.rom, code)); // stock
    m.set_ram(1, 0x080F801Du);                                      // Legends Ring Ropes: j 0x803E0074
    CHECK(hooks_outside_code(w, m.rdram.data(), 16, m.rom, code));
    m.set_ram(1, 0x0C0F801Du);                                      // jal
    CHECK(hooks_outside_code(w, m.rdram.data(), 16, m.rom, code));
    const std::vector<CodeRange> wider = { { 0x80000000u, 0x80100000u }, { 0x803E0000u, 0x803F0000u } };
    CHECK(!hooks_outside_code(w, m.rdram.data(), 16, m.rom, wider)); // the target is known code
    m.set_ram(1, 0x08000010u);                                       // j into known code
    CHECK(!hooks_outside_code(w, m.rdram.data(), 16, m.rom, code));
    m.set_ram(1, 0x3C012FE0u);                                       // an immediate, not a jump
    CHECK(!hooks_outside_code(w, m.rdram.data(), 16, m.rom, code));
}

TEST_CASE("live codes: a jump into what the table writes is the pack's code, once it's there") {
    // 32 bytes of RAM: the watched word at 0x80000004, the pack's code at 0x80000010.
    std::vector<uint8_t> rdram(32);
    std::vector<uint8_t> rom(32);
    const auto set = [&](uint32_t index, uint32_t value) { std::memcpy(rdram.data() + index * 4, &value, 4); };
    Watch w{};
    w.func = FunctionSpan{ 0x80000000u, 0, 16, 0 };
    w.words = { 0x80000004u };
    const std::vector<CodeRange> code = { { 0x80000000u, 0x80000010u } };
    const std::vector<CodeRange> pack = { { 0x80000010u, 0x80000018u } };

    set(1, 0x08000004u);                   // j 0x80000010
    CHECK(hooks_outside_code(w, rdram.data(), 32, rom, code));          // no table: refused
    CHECK(hooks_outside_code(w, rdram.data(), 32, rom, code, pack));    // nothing there yet
    set(4, 0x3C011234u);
    CHECK(!hooks_outside_code(w, rdram.data(), 32, rom, code, pack));   // written: followed
    CHECK(check_exits(w, rdram.data(), 32, rom, code, code, pack) == ExitCheck::Fine);
    set(4, 0);
    CHECK(check_exits(w, rdram.data(), 32, rom, code, code, pack) == ExitCheck::NotNow); // may be written later
    CHECK(check_exits(w, rdram.data(), 32, rom, code, code) == ExitCheck::Never);

    set(4, 0x3C011234u);
    set(1, 0x08000007u);                   // j 0x8000001C: a target the table doesn't write, the old-revision case
    CHECK(hooks_outside_code(w, rdram.data(), 32, rom, code, pack));
    CHECK(check_exits(w, rdram.data(), 32, rom, code, code, pack) == ExitCheck::Never);
    set(1, 0x03E00008u + (1u << 21));      // jr at: still refused, the table says nothing about where that goes
    CHECK(hooks_outside_code(w, rdram.data(), 32, rom, code, pack));
}

TEST_CASE("live codes: written words become ranges") {
    const std::vector<CodeRange> r = ranges_of_words({ 0x80000108u, 0x80000100u, 0x80000104u, 0x80000104u, 0x80000200u });
    REQUIRE(r.size() == 2);
    CHECK(r[0].start == 0x80000100u);
    CHECK(r[0].end == 0x8000010Cu);
    CHECK(r[1].start == 0x80000200u);
    CHECK(r[1].end == 0x80000204u);
    CHECK(ranges_of_words({}).empty());
}

TEST_CASE("live codes: a hook written in two halves is judged by the word it makes") {
    Memory m;
    const Watch w = watch_word1();
    const std::vector<CodeRange> code = { { 0x80000000u, 0x80100000u } };
    m.set_ram(1, 0x080F3E49u); // the high half written, low half stock: j 0x803CF924
    CHECK(hooks_outside_code(w, m.rdram.data(), 16, m.rom, code));
    m.set_ram(1, 0x3C01801Du); // the low half written, high half stock: a lui, no hook yet
    CHECK(!hooks_outside_code(w, m.rdram.data(), 16, m.rom, code));
}

TEST_CASE("live codes: a branch out of the function is a hook, one inside it isn't") {
    Memory m;
    const Watch w = watch_word1(); // 0x80000000..0x80000010, the word at 0x80000004
    const std::vector<CodeRange> code = { { 0x80000000u, 0x80100000u } };
    m.set_ram(1, 0x10000001u); // b +1: 0x8000000C, inside
    CHECK(!hooks_outside_code(w, m.rdram.data(), 16, m.rom, code));
    m.set_ram(1, 0x10000010u); // b past the end, though in known code
    CHECK(hooks_outside_code(w, m.rdram.data(), 16, m.rom, code));
    m.set_ram(1, 0x1000FFF0u); // b back before the start
    CHECK(hooks_outside_code(w, m.rdram.data(), 16, m.rom, code));
    m.set_ram(1, 0x04410010u); // bgez out
    CHECK(hooks_outside_code(w, m.rdram.data(), 16, m.rom, code));
    m.set_ram(1, 0x54400010u); // bnel out
    CHECK(hooks_outside_code(w, m.rdram.data(), 16, m.rom, code));
    m.set_ram(1, 0x45000010u); // bc1f out
    CHECK(hooks_outside_code(w, m.rdram.data(), 16, m.rom, code));
}

TEST_CASE("live codes: jr and jalr count as hooks unless through ra") {
    Memory m;
    const Watch w = watch_word1();
    const std::vector<CodeRange> code = { { 0x80000000u, 0x80100000u } };
    m.set_ram(1, 0x00200008u); // jr at
    CHECK(hooks_outside_code(w, m.rdram.data(), 16, m.rom, code));
    m.set_ram(1, 0x0320F809u); // jalr t9
    CHECK(hooks_outside_code(w, m.rdram.data(), 16, m.rom, code));
    m.set_ram(1, 0x03E00008u); // jr ra: an early return
    CHECK(!hooks_outside_code(w, m.rdram.data(), 16, m.rom, code));
}

TEST_CASE("live codes: hooks_outside_code skips words out of range") {
    Memory m;
    const std::vector<CodeRange> code = { { 0x80000000u, 0x80100000u } };
    const Watch past_ram{ FunctionSpan{ 0x80000000u, 0, 32, 0 }, { 0x80000010u } };
    CHECK(!hooks_outside_code(past_ram, m.rdram.data(), 16, m.rom, code));
}

TEST_CASE("live codes: code ranges include their start and not their end") {
    const std::vector<CodeRange> code = { { 0x80001000u, 0x80002000u }, { 0x80100000u, 0x80100010u } };
    CHECK(in_code_ranges(0x80001000u, code));
    CHECK(in_code_ranges(0x80100008u, code));
    CHECK(!in_code_ranges(0x80002000u, code));
    CHECK(!in_code_ranges(0x803E0074u, code));
    CHECK(!in_code_ranges(0x80001000u, std::vector<CodeRange>{}));
}

TEST_CASE("live codes: a jump to an unloaded overlay is not now, a jump outside all code is never") {
    Memory m;
    const Watch w = watch_word1();
    const std::vector<CodeRange> all = { { 0x80000000u, 0x80100000u }, { 0x80106760u, 0x80140000u } };
    const std::vector<CodeRange> loaded = { { 0x80000000u, 0x80100000u } };
    CHECK(check_exits(w, m.rdram.data(), 16, m.rom, all, loaded) == ExitCheck::Fine); // stock
    m.set_ram(1, 0x0C041DE0u); // jal 0x80107780, in the overlay slot
    CHECK(check_exits(w, m.rdram.data(), 16, m.rom, all, loaded) == ExitCheck::NotNow);
    CHECK(check_exits(w, m.rdram.data(), 16, m.rom, all, all) == ExitCheck::Fine);    // the same word once it's loaded
    m.set_ram(1, 0x080F801Du); // j 0x803E0074, the pack's RAM
    CHECK(check_exits(w, m.rdram.data(), 16, m.rom, all, loaded) == ExitCheck::Never);
    CHECK(check_exits(w, m.rdram.data(), 16, m.rom, all, all) == ExitCheck::Never);
    m.set_ram(1, 0x08000010u); // j into loaded code
    CHECK(check_exits(w, m.rdram.data(), 16, m.rom, all, loaded) == ExitCheck::Fine);
}

TEST_CASE("live codes: an unchanged jump in the ROM isn't a hook") {
    Memory m;
    const uint32_t stock_jump = 0x080F801Du;
    m.set_ram(1, stock_jump);
    m.rom[4] = 0x08; m.rom[5] = 0x0F; m.rom[6] = 0x80; m.rom[7] = 0x1D;
    const std::vector<CodeRange> code = { { 0x80000000u, 0x80100000u } };
    CHECK(!hooks_outside_code(watch_word1(), m.rdram.data(), 16, m.rom, code));
}

TEST_CASE("live codes: nothing happens while a function matches the ROM") {
    FunctionState s;
    CHECK(decide(s, true, true, 5, 8) == Action::None);
    CHECK(decide(s, false, false, 5, 8) == Action::None); // written, but its overlay isn't there
}

TEST_CASE("live codes: new content compiles, then flipping back and forth doesn't compile again") {
    FunctionState s;
    CHECK(decide(s, true, false, 5, 8) == Action::Compile);
    s.compiled.insert(5);
    s.current = 5;
    CHECK(decide(s, true, false, 5, 8) == Action::None);
    CHECK(decide(s, true, true, 5, 8) == Action::ToStatic); // the pack restores the stock word
    s.current = 0;
    CHECK(decide(s, true, false, 5, 8) == Action::UseCompiled);
    CHECK(decide(s, true, false, 5, 0) == Action::UseCompiled); // a cached version costs no compile budget
}

TEST_CASE("live codes: a section that unloads goes back to static at once") {
    FunctionState s;
    s.compiled.insert(5);
    s.current = 5;
    CHECK(decide(s, false, false, 5, 8) == Action::ToStatic);
}

TEST_CASE("live codes: content that can't run isn't retried, and a running version goes back to static") {
    FunctionState s;
    s.failed.insert(5);
    CHECK(decide(s, true, false, 5, 8) == Action::None); // already static
    s.compiled.insert(6);
    s.current = 6;
    CHECK(decide(s, true, false, 5, 8) == Action::ToStatic); // a half-written hook's version doesn't keep running
    s.current = 0;
    CHECK(decide(s, true, false, 6, 8) == Action::UseCompiled); // its other content still runs
}

TEST_CASE("live codes: failed content counts toward the cap") {
    FunctionState s;
    for (uint64_t k = 1; k <= MaxVersionsPerFunction; k++) s.failed.insert(k + 100);
    CHECK(decide(s, true, false, 5, 8) == Action::AtCap);
    s.failed.erase(101);
    CHECK(decide(s, true, false, 5, 8) == Action::Compile);
}

TEST_CASE("live codes: the compile budget defers and the version cap stops") {
    FunctionState s;
    CHECK(decide(s, true, false, 5, 0) == Action::Deferred);
    for (uint64_t k = 1; k <= MaxVersionsPerFunction; k++) s.compiled.insert(k + 100);
    CHECK(decide(s, true, false, 5, 8) == Action::AtCap);
    CHECK(decide(s, true, false, 101, 8) == Action::UseCompiled);
}

TEST_CASE("live codes: a refused function never switches") {
    FunctionState s;
    s.refused = true;
    CHECK(decide(s, true, false, 5, 8) == Action::None);
    CHECK(decide(s, true, true, 5, 8) == Action::None);
    CHECK(decide(s, false, false, 5, 8) == Action::None);
}

TEST_CASE("live codes: the entry jump reaches its target and refuses out of range") {
    uint8_t out[EntryPatchSize];
    CHECK(encode_entry_jump(0x140001000ull, 0x140002000ull, out));
    CHECK(out[0] == 0xE9);
    int32_t rel;
    std::memcpy(&rel, out + 1, 4);
    CHECK(rel == 0x1000 - 5);
    CHECK(encode_entry_jump(0x140002000ull, 0x140001000ull, out));
    std::memcpy(&rel, out + 1, 4);
    CHECK(rel == -0x1000 - 5);
    CHECK(!encode_entry_jump(0x140000000ull, 0x240000000ull, out));
    CHECK(!encode_entry_jump(0x240000000ull, 0x140000000ull, out));
}

TEST_CASE("live codes: the stub jumps through its 8-byte aligned target") {
    uint8_t out[StubSize];
    encode_stub(0x1122334455667788ull, out);
    CHECK(out[0] == 0xFF);
    CHECK(out[1] == 0x25);
    int32_t disp;
    std::memcpy(&disp, out + 2, 4);
    CHECK(6 + disp == int32_t(StubTargetOffset)); // rip is past the 6-byte jmp
    uint64_t target;
    std::memcpy(&target, out + StubTargetOffset, 8);
    CHECK(target == 0x1122334455667788ull);
    CHECK((StubTargetOffset % 8) == 0);
}
