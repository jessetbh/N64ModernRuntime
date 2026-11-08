//
// librecomp
//

// [wcw fullconv] Unit tests for librecomp/entry_prologue_rules (aki-roadmap#76): the port's hand-added prologues reach
// the live recompiler's entry hooks, with or without a mod's own hook on the same function. Built and run as part of
// every librecomp build (LIBRECOMP_WCW_TESTS); a failure fails the build.

#include "librecomp/entry_prologue_rules.hpp"

#include <vector>

#include "wcw_test.h"

using namespace recomp::mods::entry_prologues;

namespace {
    int stock_a, stock_b, stock_c;
}

TEST_CASE("entry prologues: a plain mod hook slot decodes as a slot, not a prologue") {
    Decoded d = decode(7);
    CHECK(!d.is_prologue);
    CHECK(d.has_slot && d.slot == 7);
}

TEST_CASE("entry prologues: encode and decode round-trip, with and without a slot") {
    Decoded alone = decode(encode(3, false, 0));
    CHECK(alone.is_prologue && alone.prologue == 3 && !alone.has_slot);

    Decoded both = decode(encode(5, true, 0));
    CHECK(both.is_prologue && both.prologue == 5 && both.has_slot && both.slot == 0);

    Decoded big = decode(encode(MaxPrologues - 1, true, 0xFFFFFFFEu));
    CHECK(big.is_prologue && big.prologue == MaxPrologues - 1 && big.has_slot && big.slot == 0xFFFFFFFEu);
}

TEST_CASE("entry prologues: only functions with a prologue get one") {
    const void* funcs[] = { &stock_a, &stock_b, &stock_c };
    std::unordered_map<const void*, size_t> prologue_of{ { &stock_b, 0 } };
    std::unordered_map<size_t, size_t> hooks{};
    CHECK(add_to_entry_hooks(funcs, prologue_of, hooks) == 1);
    CHECK(hooks.size() == 1);
    CHECK(hooks.count(1) == 1);
    Decoded d = decode(hooks[1]);
    CHECK(d.is_prologue && d.prologue == 0 && !d.has_slot);
}

TEST_CASE("entry prologues: a mod's entry hook on the same function is kept alongside the prologue") {
    const void* funcs[] = { &stock_a, &stock_b };
    std::unordered_map<const void*, size_t> prologue_of{ { &stock_a, 2 }, { &stock_b, 1 } };
    std::unordered_map<size_t, size_t> hooks{ { 0, 9 } };
    CHECK(add_to_entry_hooks(funcs, prologue_of, hooks) == 2);
    Decoded a = decode(hooks[0]);
    CHECK(a.is_prologue && a.prologue == 2 && a.has_slot && a.slot == 9);
    Decoded b = decode(hooks[1]);
    CHECK(b.is_prologue && b.prologue == 1 && !b.has_slot);
}

TEST_CASE("entry prologues: adding twice doesn't stack or lose the mod slot") {
    const void* funcs[] = { &stock_a };
    std::unordered_map<const void*, size_t> prologue_of{ { &stock_a, 4 } };
    std::unordered_map<size_t, size_t> hooks{ { 0, 3 } };
    CHECK(add_to_entry_hooks(funcs, prologue_of, hooks) == 1);
    CHECK(add_to_entry_hooks(funcs, prologue_of, hooks) == 0);
    Decoded d = decode(hooks[0]);
    CHECK(d.is_prologue && d.prologue == 4 && d.has_slot && d.slot == 3);
}

TEST_CASE("entry prologues: no prologues registered leaves the hooks untouched") {
    const void* funcs[] = { &stock_a, &stock_b };
    std::unordered_map<const void*, size_t> prologue_of{};
    std::unordered_map<size_t, size_t> hooks{ { 1, 0 } };
    CHECK(add_to_entry_hooks(funcs, prologue_of, hooks) == 0);
    CHECK(hooks.size() == 1 && hooks[1] == 0);
}
