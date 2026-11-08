#include "librecomp/entry_prologue_rules.hpp"

namespace recomp::mods::entry_prologues {

size_t encode(size_t prologue, bool has_slot, size_t slot) {
    size_t index = Tag | ((prologue & (MaxPrologues - 1)) << 32);
    if (has_slot) {
        index |= HasSlot | (slot & 0xFFFFFFFFu);
    }
    return index;
}

Decoded decode(size_t hook_index) {
    Decoded d{};
    if ((hook_index & Tag) == 0) {
        d.has_slot = true;
        d.slot = hook_index;
        return d;
    }
    d.is_prologue = true;
    d.prologue = (hook_index >> 32) & (MaxPrologues - 1);
    d.has_slot = (hook_index & HasSlot) != 0;
    d.slot = d.has_slot ? (hook_index & 0xFFFFFFFFu) : 0;
    return d;
}

size_t add_to_entry_hooks(std::span<const void* const> func_ptrs, const std::unordered_map<const void*, size_t> &prologue_of,
    std::unordered_map<size_t, size_t> &entry_hooks)
{
    size_t added = 0;
    for (size_t i = 0; i < func_ptrs.size(); i++) {
        auto found = prologue_of.find(func_ptrs[i]);
        if (found == prologue_of.end()) {
            continue;
        }
        auto hooked = entry_hooks.find(i);
        if (hooked == entry_hooks.end()) {
            entry_hooks[i] = encode(found->second, false, 0);
        }
        else if ((hooked->second & Tag) == 0) {
            hooked->second = encode(found->second, true, hooked->second);
        }
        else {
            continue; // already carries a prologue
        }
        added++;
    }
    return added;
}

}
