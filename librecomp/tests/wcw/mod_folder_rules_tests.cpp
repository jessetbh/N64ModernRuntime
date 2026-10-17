//
// librecomp
//

// [wcw] Unit tests for librecomp/mod_folder_rules (texture pack folders without a mod.json, aki-roadmap#48). Built and run
// as part of every librecomp build (LIBRECOMP_WCW_TESTS); a failure fails the build.

#include "librecomp/mod_folder_rules.hpp"

#include <set>
#include <string>
#include <vector>

#include "wcw_test.h"

using namespace recomp::mods::folder_rules;

namespace {
    // The games' registrations: 0 = binary_syms (.nrm code), 1 = rom_patch, 2 = rt64.json texture packs.
    const std::vector<std::string> ContentFiles = { "mod_binary.bin", "patch.bps", "rt64.json" };
    // .nrm needs a mod.json and may hold anything; .rtz holds texture packs and needs none.
    const std::vector<Container> Containers = { Container{ {}, true }, Container{ { 2 }, false } };

    std::vector<size_t> typesFor(const std::set<std::string> &files, const std::vector<Container> &containers = Containers) {
        return manifestless_folder_types(containers, ContentFiles, [&](const std::string &name) { return files.count(name) > 0; });
    }
}

TEST_CASE("mod folders: a folder with rt64.json and no mod.json opens as a texture pack") {
    const std::vector<size_t> types = typesFor({ "rt64.json", "0123456789abcdef.png" });
    CHECK_EQ(types.size(), size_t(1));
    CHECK(!types.empty() && types[0] == 2);
}

TEST_CASE("mod folders: a folder without a manifest-less content file is not such a mod (it still needs a mod.json)") {
    CHECK(typesFor({ "readme.txt", "0123456789abcdef.png" }).empty());
    CHECK(typesFor({}).empty());
}

TEST_CASE("mod folders: code content only counts with a mod.json") {
    // mod_binary.bin belongs to a container that requires a manifest.
    CHECK(typesFor({ "mod_binary.bin" }).empty());
    const std::vector<size_t> types = typesFor({ "mod_binary.bin", "rt64.json" });
    CHECK_EQ(types.size(), size_t(1));
    CHECK(!types.empty() && types[0] == 2);
}

TEST_CASE("mod folders: no manifest-less container registered means no change from upstream") {
    CHECK(typesFor({ "rt64.json" }, { Container{ {}, true } }).empty());
    // A container that allows rt64.json but needs a mod.json doesn't make a bare folder a mod.
    CHECK(typesFor({ "rt64.json" }, { Container{ { 2 }, true } }).empty());
    // A manifest-less container that allows any content has no content file to look for.
    CHECK(typesFor({ "rt64.json" }, { Container{ {}, false } }).empty());
}

TEST_CASE("mod folders: a type allowed by two containers appears once, out-of-range ids are ignored") {
    const std::vector<Container> containers = { Container{ { 2, 9 }, false }, Container{ { 2 }, false } };
    const std::vector<size_t> types = typesFor({ "rt64.json" }, containers);
    CHECK_EQ(types.size(), size_t(1));
}
