#ifndef __RECOMP_REGEN_RULES_HPP__
#define __RECOMP_REGEN_RULES_HPP__

// [wcw fullconv] Rules for the functions the live recompiler regenerates (aki-roadmap#76). A hack can end a function's
// span on a jump or branch, its delay slot the first word of the next span (No Mercy Plus V3 / ECW BTBW: a `jr $at`
// at the end of stock func_80126FFC, the `addu $v0` that makes its return value just past it). N64Recomp drops a
// delay slot outside the function's words, so the span takes in that one word. Unit tested in
// tests/wcw/regen_rules_tests.cpp.

#include <cstddef>
#include <cstdint>

namespace recomp {
    namespace mods {
        namespace regen_rules {
            // Whether an R4300 instruction (host-order word) is a jump or branch, i.e. has a delay slot.
            bool has_delay_slot(uint32_t instr);

            // How many words to decode for a function of num_words words whose last word is last_instr (host order),
            // when available more words follow it: one more when the last one has a delay slot and it's there.
            size_t words_to_decode(size_t num_words, uint32_t last_instr, size_t available);
        }
    }
}

#endif
