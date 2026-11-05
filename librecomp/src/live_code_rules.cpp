// [wcw fullconv] Decisions for codes that rewrite instructions at run time (see librecomp/live_code_rules.hpp). Pure: no
// runtime state, unit tested in tests/wcw/live_code_rules_tests.cpp.

#include "librecomp/live_code_rules.hpp"

#include <algorithm>
#include <cstring>

namespace recomp::mods::live_codes {
    namespace {
        constexpr uint32_t RdramStart = 0x80000000u;

        bool is_write(uint8_t type) {
            return (type == 0x80) || (type == 0x81) || (type == 0xA0) || (type == 0xA1);
        }

        bool read_rdram_word(const uint8_t *rdram, uint32_t rdram_size, uint32_t addr, uint32_t &out) {
            const uint32_t offset = addr - RdramStart;
            if ((addr < RdramStart) || (offset > rdram_size) || ((rdram_size - offset) < 4)) return false;
            std::memcpy(&out, rdram + offset, 4);
            return true;
        }

        bool read_rom_word(std::span<const uint8_t> rom, uint32_t offset, uint32_t &out) {
            if ((offset > rom.size()) || ((rom.size() - offset) < 4)) return false;
            out = (uint32_t(rom[offset]) << 24) | (uint32_t(rom[offset + 1]) << 16) | (uint32_t(rom[offset + 2]) << 8) | uint32_t(rom[offset + 3]);
            return true;
        }
    }

    std::vector<uint32_t> written_words(const std::vector<conversion::codes::Code> &codes) {
        std::vector<uint32_t> words;
        for (size_t i = 0; i < codes.size(); i++) {
            const conversion::codes::Code &c = codes[i];
            if (is_write(c.type)) {
                words.push_back(c.addr & ~3u);
            }
            else if ((c.type == 0x50) && ((i + 1) < codes.size()) && is_write(codes[i + 1].type)) {
                const uint32_t count = (c.addr >> 8) & 0xFF;
                const uint32_t step = c.addr & 0xFF;
                for (uint32_t k = 0; k < count; k++) {
                    words.push_back((codes[i + 1].addr + k * step) & ~3u);
                }

                i++;
            }
        }

        std::sort(words.begin(), words.end());
        words.erase(std::unique(words.begin(), words.end()), words.end());
        return words;
    }

    std::vector<Watch> build_watches(const std::vector<uint32_t> &words, std::span<const FunctionSpan> functions) {
        std::vector<Watch> watches;
        for (const FunctionSpan &f : functions) {
            if (f.size == 0) continue;
            auto first = std::lower_bound(words.begin(), words.end(), f.vram);
            auto last = std::lower_bound(first, words.end(), f.vram + f.size);
            if (first != last) {
                watches.push_back(Watch{ f, std::vector<uint32_t>(first, last) });
            }
        }

        return watches;
    }

    bool watched_match_rom(const Watch &watch, const uint8_t *rdram, uint32_t rdram_size, std::span<const uint8_t> rom) {
        for (uint32_t addr : watch.words) {
            uint32_t ram_word, rom_word;
            if (!read_rdram_word(rdram, rdram_size, addr, ram_word) || !read_rom_word(rom, watch.func.rom + (addr - watch.func.vram), rom_word)) {
                return false;
            }

            if (ram_word != rom_word) return false;
        }

        return true;
    }

    bool others_match_rom(const Watch &watch, const uint8_t *rdram, uint32_t rdram_size, std::span<const uint8_t> rom) {
        size_t next_watched = 0;
        for (uint32_t offset = 0; (offset + 4) <= watch.func.size; offset += 4) {
            const uint32_t addr = watch.func.vram + offset;
            while ((next_watched < watch.words.size()) && (watch.words[next_watched] < addr)) next_watched++;
            if ((next_watched < watch.words.size()) && (watch.words[next_watched] == addr)) continue;
            uint32_t ram_word, rom_word;
            if (!read_rdram_word(rdram, rdram_size, addr, ram_word) || !read_rom_word(rom, watch.func.rom + offset, rom_word)) {
                return false;
            }

            if (ram_word != rom_word) return false;
        }

        return true;
    }

    bool in_code_ranges(uint32_t addr, std::span<const CodeRange> code) {
        for (const CodeRange &range : code) {
            if ((addr >= range.start) && (addr < range.end)) return true;
        }

        return false;
    }

    std::vector<CodeRange> ranges_of_words(const std::vector<uint32_t> &words) {
        std::vector<uint32_t> sorted = words;
        std::sort(sorted.begin(), sorted.end());
        std::vector<CodeRange> ranges;
        for (uint32_t word : sorted) {
            if (!ranges.empty() && (word <= ranges.back().end)) {
                ranges.back().end = std::max(ranges.back().end, word + 4);
            }
            else {
                ranges.push_back(CodeRange{ word, word + 4 });
            }
        }

        return ranges;
    }

    bool hooks_outside_code(const Watch &watch, const uint8_t *rdram, uint32_t rdram_size, std::span<const uint8_t> rom, std::span<const CodeRange> code,
        std::span<const CodeRange> pack_writes) {
        for (uint32_t addr : watch.words) {
            uint32_t w, rom_word;
            if (!read_rdram_word(rdram, rdram_size, addr, w) || !read_rom_word(rom, watch.func.rom + (addr - watch.func.vram), rom_word)) {
                continue;
            }

            if (w == rom_word) continue;
            const uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, funct = w & 0x3F;
            if ((op == 2) || (op == 3)) {
                const uint32_t target = ((addr + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
                if (!in_code_ranges(target, code)) {
                    uint32_t there = 0;
                    if (!in_code_ranges(target, pack_writes) || !read_rdram_word(rdram, rdram_size, target, there) || (there == 0)) return true;
                }
            }
            else if ((op == 0) && ((funct == 0x08) || (funct == 0x09))) {
                if (rs != 31) return true;
            }
            else if (((op >= 4) && (op <= 7)) || ((op >= 0x14) && (op <= 0x17)) || ((op == 1) && ((rt <= 3) || ((rt >= 0x10) && (rt <= 0x13)))) || ((op == 0x11) && (rs == 8))) {
                const uint32_t target = addr + 4 + uint32_t(int32_t(int16_t(w & 0xFFFF)) * 4);
                if ((target < watch.func.vram) || (target >= (watch.func.vram + watch.func.size))) return true;
            }
        }

        return false;
    }

    ExitCheck check_exits(const Watch &watch, const uint8_t *rdram, uint32_t rdram_size, std::span<const uint8_t> rom,
        std::span<const CodeRange> all, std::span<const CodeRange> loaded, std::span<const CodeRange> pack_writes)
    {
        std::vector<CodeRange> known(all.begin(), all.end());
        known.insert(known.end(), pack_writes.begin(), pack_writes.end());
        if (hooks_outside_code(watch, rdram, rdram_size, rom, known)) return ExitCheck::Never;
        return hooks_outside_code(watch, rdram, rdram_size, rom, loaded, pack_writes) ? ExitCheck::NotNow : ExitCheck::Fine;
    }

    uint64_t content_key(const Watch &watch, const uint8_t *rdram, uint32_t rdram_size) {
        uint64_t h = 0xCBF29CE484222325ull;
        for (uint32_t addr : watch.words) {
            uint32_t word = 0;
            read_rdram_word(rdram, rdram_size, addr, word);
            for (int shift = 0; shift < 32; shift += 8) {
                h = (h ^ ((word >> shift) & 0xFF)) * 0x100000001B3ull;
            }
        }

        return (h == 0) ? 1 : h;
    }

    Action decide(const FunctionState &state, bool section_loaded, bool at_rom, uint64_t key, int compiles_left) {
        if (state.refused) return Action::None;
        if (!section_loaded || at_rom) {
            return (state.current != 0) ? Action::ToStatic : Action::None;
        }

        if (state.failed.contains(key)) {
            return (state.current != 0) ? Action::ToStatic : Action::None;
        }

        if (key == state.current) return Action::None;
        if (state.compiled.contains(key)) return Action::UseCompiled;
        if ((state.compiled.size() + state.failed.size()) >= MaxVersionsPerFunction) return Action::AtCap;
        return (compiles_left > 0) ? Action::Compile : Action::Deferred;
    }

    bool encode_entry_jump(uintptr_t from, uintptr_t to, uint8_t out[EntryPatchSize]) {
        const int64_t distance = int64_t(to) - int64_t(from + EntryPatchSize);
        if ((distance < INT32_MIN) || (distance > INT32_MAX)) return false;
        const int32_t rel = int32_t(distance);
        out[0] = 0xE9;
        std::memcpy(out + 1, &rel, 4);
        return true;
    }

    void encode_stub(uint64_t target, uint8_t out[StubSize]) {
        static const uint8_t head[StubTargetOffset] = { 0xFF, 0x25, 0x02, 0x00, 0x00, 0x00, 0xCC, 0xCC };
        std::memcpy(out, head, sizeof(head));
        std::memcpy(out + StubTargetOffset, &target, 8);
    }
}
