// [wcw fullconv] Native GameShark codes table (see librecomp/conversion_codes.hpp). Pure: no runtime state, unit
// tested in tests/wcw/conversion_tests.cpp.

#include "librecomp/conversion_codes.hpp"

namespace recomp::mods::conversion::codes {
    namespace {
        constexpr uint32_t RdramStart = 0x80000000u;

        int hex_value(char c) {
            if ((c >= '0') && (c <= '9')) return c - '0';
            if ((c >= 'a') && (c <= 'f')) return c - 'a' + 10;
            if ((c >= 'A') && (c <= 'F')) return c - 'A' + 10;
            return -1;
        }

        char lower(char c) {
            return ((c >= 'A') && (c <= 'Z')) ? char(c - 'A' + 'a') : c;
        }

        bool is_space(char c) {
            return (c == ' ') || (c == '\t') || (c == '\r');
        }

        bool is_conditional(uint32_t type) {
            return (type >= 0xD0) && (type <= 0xD3);
        }

        bool is_write(uint32_t type) {
            return (type == 0x80) || (type == 0x81) || (type == 0xA0) || (type == 0xA1);
        }

        bool is_enabler(uint32_t type) {
            return (type == 0xF0) || (type == 0xF1) || (type == 0xF6) || (type == 0xFF) || (type == 0xEE) || (type == 0xDE) ||
                (type == 0x2A) || (type == 0x3C);
        }

        bool is_prefix(uint32_t type) {
            return (type == 0x50) || is_conditional(type);
        }

        // The tag value: "<game>" or "<game>-rev<N>" ('-', '_' or ' ' before "rev"); nothing else after it.
        bool parse_rom_tag(const char *p, const char *end, const std::string &game_key, Report &r) {
            while ((p < end) && is_space(*p)) p++;
            size_t k = 0;
            while ((k < game_key.size()) && (p < end) && (lower(*p) == game_key[k])) { p++; k++; }
            if (k != game_key.size()) return false;
            if ((p < end) && ((*p == '-') || (*p == '_') || (*p == ' '))) {
                p++;
                if (((end - p) >= 3) && (lower(p[0]) == 'r') && (lower(p[1]) == 'e') && (lower(p[2]) == 'v')) {
                    p += 3;
                    while ((p < end) && is_space(*p)) p++;
                    if ((p < end) && (*p >= '0') && (*p <= '9')) {
                        r.rom_rev = *p - '0';
                        p++;
                    }
                }
            }

            while ((p < end) && is_space(*p)) p++;
            return p == end;
        }

        uint8_t read8(const uint8_t *rdram, uint32_t size, uint32_t addr, bool &ok) {
            const uint32_t offset = addr - RdramStart;
            if ((addr < RdramStart) || (offset >= size)) { ok = false; return 0; }
            return rdram[offset ^ 3];
        }

        void write8(uint8_t *rdram, uint32_t size, uint32_t addr, uint8_t value) {
            const uint32_t offset = addr - RdramStart;
            if ((addr >= RdramStart) && (offset < size)) rdram[offset ^ 3] = value;
        }

        void write_value(uint8_t *rdram, uint32_t size, uint32_t type, uint32_t addr, uint32_t value) {
            if ((type == 0x80) || (type == 0xA0)) {
                write8(rdram, size, addr, uint8_t(value));
            }
            else {
                addr &= ~1u;
                const uint32_t offset = addr - RdramStart;
                if ((addr >= RdramStart) && ((uint64_t(offset) + 2) <= size)) {
                    rdram[offset ^ 3] = uint8_t(value >> 8);
                    rdram[(offset + 1) ^ 3] = uint8_t(value);
                }
            }
        }

        bool condition_holds(const uint8_t *rdram, uint32_t size, const Code &c) {
            bool ok = true;
            uint32_t current;
            uint32_t value = c.value;
            if ((c.type == 0xD0) || (c.type == 0xD2)) {
                current = read8(rdram, size, c.addr, ok);
                value &= 0xFF;
            }
            else {
                const uint32_t addr = c.addr & ~1u;
                current = (uint32_t(read8(rdram, size, addr, ok)) << 8) | read8(rdram, size, addr + 1, ok);
            }

            if (!ok) return false;
            return ((c.type == 0xD0) || (c.type == 0xD1)) ? (current == value) : (current != value);
        }

        size_t unit_end(const std::vector<Code> &codes, size_t i) {
            while ((i < codes.size()) && is_prefix(codes[i].type)) i++;
            return (i < codes.size()) ? (i + 1) : codes.size();
        }
    }

    void split_rom_tag(const std::string &tag, std::string &game_key, int &rev) {
        const size_t at = tag.find("-rev");
        game_key = tag.substr(0, at);
        rev = 0;
        if ((at != std::string::npos) && ((at + 4) < tag.size()) && (tag[at + 4] >= '0') && (tag[at + 4] <= '9')) {
            rev = tag[at + 4] - '0';
        }
    }

    bool parse_file(std::string_view text, const std::string &game_key, int native_rev, std::vector<Code> &out, Report &r) {
        out.clear();
        r = Report();
        const char *p = text.data();
        const char *text_end = p + text.size();
        if ((text.size() >= 3) && (uint8_t(p[0]) == 0xEF) && (uint8_t(p[1]) == 0xBB) && (uint8_t(p[2]) == 0xBF)) p += 3;

        // The tag decides everything, so find it first (the first one counts).
        for (const char *q = p; q < text_end;) {
            const char *eol = q;
            while ((eol < text_end) && (*eol != '\n')) eol++;
            const char *a = q;
            while ((a < eol) && is_space(*a)) a++;
            const char *b = eol;
            while ((b > a) && is_space(b[-1])) b--;
            if ((a < b) && (*a == '#')) {
                const char *t = a + 1;
                while ((t < b) && is_space(*t)) t++;
                if (((b - t) >= 4) && (lower(t[0]) == 'r') && (lower(t[1]) == 'o') && (lower(t[2]) == 'm') && (t[3] == ':')) {
                    if (!parse_rom_tag(t + 4, b, game_key, r)) r.rom_mismatch = true;
                    break;
                }
            }

            q = (eol < text_end) ? (eol + 1) : eol;
        }

        if ((r.rom_rev >= 0) && (r.rom_rev != native_rev)) r.rom_mismatch = true;
        if (r.rom_mismatch) return false;

        int line_no = 0;
        while (p < text_end) {
            const char *eol = p;
            while ((eol < text_end) && (*eol != '\n')) eol++;
            line_no++;
            const char *a = p;
            while ((a < eol) && is_space(*a)) a++;
            p = (eol < text_end) ? (eol + 1) : eol;

            uint32_t word = 0;
            int len = 0;
            const char *c = a;
            while ((c < eol) && (hex_value(*c) >= 0) && (len < 13)) { word = (word << 4) | uint32_t(hex_value(*c)); c++; len++; }
            if (len < 8) continue; // comment, header, name, blank
            bool ok = false;
            uint32_t code = 0, value = 0;
            if (len == 12) {
                const char *d = a;
                for (int i = 0; i < 8; i++) code = (code << 4) | uint32_t(hex_value(*d++));
                for (int i = 0; i < 4; i++) value = (value << 4) | uint32_t(hex_value(*d++));
                ok = true;
            }
            else if (len == 8) {
                code = word;
                const char *d = c;
                int seps = 0;
                while ((d < eol) && (is_space(*d) || (*d == ':') || (*d == '-'))) { d++; seps++; }
                int vlen = 0;
                while ((d < eol) && (hex_value(*d) >= 0) && (vlen < 5)) { value = (value << 4) | uint32_t(hex_value(*d)); d++; vlen++; }
                ok = (seps > 0) && (vlen == 4);
                c = d;
            }

            if (ok && (c < eol) && !is_space(*c) && (*c != '#') && !((*c == '/') && ((c + 1) < eol) && (c[1] == '/'))) ok = false;
            if (!ok) {
                const bool looks_like_code = (len >= 8) && ((c >= eol) || is_space(*c) || (*c == ':') || (*c == '-'));
                const char *d = c;
                while ((d < eol) && (is_space(*d) || (*d == ':') || (*d == '-'))) d++;
                if (looks_like_code && ((d >= eol) || (hex_value(*d) >= 0))) {
                    r.malformed++;
                    if (r.first_bad_line == 0) r.first_bad_line = line_no;
                }
                continue;
            }

            const uint32_t type = code >> 24;
            if (is_enabler(type)) { r.enablers++; continue; }
            if ((type == 0x88) || (type == 0x89)) { r.buttons++; continue; }
            if (!is_write(type) && !is_prefix(type)) { r.unknown++; continue; }
            Code entry;
            entry.type = uint8_t(type);
            entry.addr = (type == 0x50) ? (code & 0x00FFFFFF) : (RdramStart | (code & 0x00FFFFFF));
            entry.value = uint16_t(value);
            out.push_back(entry);
        }

        // A trailing conditional or repeater has nothing to govern.
        while (!out.empty() && is_prefix(out.back().type)) out.pop_back();
        for (const Code &c : out) {
            if (is_write(c.type)) r.writes++;
            else if (c.type == 0x50) r.repeaters++;
            else r.conditionals++;
        }

        r.added = int(out.size());
        return true;
    }

    void apply(const std::vector<Code> &codes, uint8_t *rdram, uint32_t rdram_size) {
        size_t i = 0;
        while (i < codes.size()) {
            const Code &c = codes[i];
            if (is_conditional(c.type)) {
                i = condition_holds(rdram, rdram_size, c) ? (i + 1) : unit_end(codes, i + 1);
            }
            else if (c.type == 0x50) {
                if ((i + 1) >= codes.size()) break;
                const Code &next = codes[i + 1];
                const uint32_t count = (c.addr >> 8) & 0xFF;
                const uint32_t step = c.addr & 0xFF;
                if (is_write(next.type)) {
                    for (uint32_t k = 0; k < count; k++) {
                        write_value(rdram, rdram_size, next.type, next.addr + k * step, uint32_t(next.value) + k * c.value);
                    }

                    i += 2;
                }
                else {
                    i = unit_end(codes, i + 1); // 50 before a conditional isn't a GameShark form: skip both
                }
            }
            else {
                write_value(rdram, rdram_size, c.type, c.addr, c.value);
                i++;
            }
        }
    }
}
