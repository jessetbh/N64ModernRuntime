#ifndef __RECOMP_ENTRY_PROLOGUE_RULES_HPP__
#define __RECOMP_ENTRY_PROLOGUE_RULES_HPP__

// [wcw fullconv] Entry prologues (aki-roadmap#76): code a port hand-adds at the start of a recompiled function (No
// Mercy's 60 fps matrix capture and RT camera latch in func_80001E08) lives only in the static function. When the live
// recompiler replaces that function (a ROM patch changed it, or a mod hooks it), the port's registered prologue is
// called at the entry of the replacement, so the hand-edit survives. A prologue observes: the registers are restored
// after it runs, and it can't skip the function.
//
// The live generator's entry hook passes one index per function. A prologue index is tagged (top bit) and may carry a
// mod's hook slot as well (the mod's hook runs first, then the prologue, as at the start of the static function).
// Pure rules, unit tested in tests/wcw/entry_prologue_tests.cpp.

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>

namespace recomp {
    namespace mods {
        namespace entry_prologues {
            static_assert(sizeof(size_t) == 8, "entry hook indices pack a prologue and a hook slot into 64 bits");

            constexpr size_t Tag = size_t(1) << 63;
            constexpr size_t HasSlot = size_t(1) << 62;
            constexpr size_t MaxPrologues = size_t(1) << 30;

            // The entry hook index for a prologue, alone or with a mod's hook slot.
            size_t encode(size_t prologue, bool has_slot, size_t slot);

            struct Decoded {
                bool is_prologue = false;   // false: a plain mod hook slot (slot holds it)
                size_t prologue = 0;
                bool has_slot = false;
                size_t slot = 0;
            };
            Decoded decode(size_t hook_index);

            // Adds the prologues to a regenerated list's entry hooks. func_ptrs[i] is the static function that function i
            // replaces; prologue_of maps a static function to its prologue's index. A function a mod already entry-hooks
            // keeps that slot alongside its prologue. Returns how many prologues were added.
            size_t add_to_entry_hooks(std::span<const void* const> func_ptrs, const std::unordered_map<const void*, size_t> &prologue_of,
                std::unordered_map<size_t, size_t> &entry_hooks);
        }
    }
}

#endif
