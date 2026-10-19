//
// librecomp
//

// [wcw] Unit tests for librecomp/mod_enable_rules (dependency auto-enable, aki-roadmap#100) and the manifest-less
// mod id rule in mod_folder_rules. Built and run as part of every librecomp build (LIBRECOMP_WCW_TESTS); a failure
// fails the build.

#include "librecomp/mod_enable_rules.hpp"
#include "librecomp/mod_folder_rules.hpp"

#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include "wcw_test.h"

using namespace recomp::mods;

namespace {
    using Graph = std::map<std::string, std::vector<enable_rules::Dependency>>;

    std::vector<std::string> enable(const Graph &graph, const std::string &mod_id, std::unordered_set<std::string> &auto_enabled) {
        return enable_rules::newly_auto_enabled(mod_id, [&](const std::string &id) {
            auto it = graph.find(id);
            return (it != graph.end()) ? it->second : std::vector<enable_rules::Dependency>();
        }, auto_enabled);
    }
}

TEST_CASE("mod enable: enabling a mod auto-enables its dependency, not itself (on_enabled fired with the dependent's handle)") {
    const Graph graph = { { "hd_textures", { { "base_pack" } } }, { "base_pack", {} } };
    std::unordered_set<std::string> auto_enabled;
    const std::vector<std::string> found = enable(graph, "hd_textures", auto_enabled);
    CHECK_EQ(found.size(), size_t(1));
    CHECK(!found.empty() && found[0] == "base_pack");
    CHECK(auto_enabled.contains("base_pack"));
    CHECK(!auto_enabled.contains("hd_textures"));
}

TEST_CASE("mod enable: dependencies of dependencies, each once") {
    const Graph graph = { { "a", { { "b" }, { "c" } } }, { "b", { { "c" }, { "d" } } }, { "c", {} }, { "d", {} } };
    std::unordered_set<std::string> auto_enabled;
    const std::vector<std::string> found = enable(graph, "a", auto_enabled);
    CHECK_EQ(found.size(), size_t(3));
    CHECK(auto_enabled == std::unordered_set<std::string>({ "b", "c", "d" }));
    // Enabling another mod that needs them again auto-enables nothing new (no second on_enabled).
    const Graph more = { { "e", { { "c" } } } };
    CHECK(enable(more, "e", auto_enabled).empty());
}

TEST_CASE("mod enable: optional and deprecated dependencies are skipped; a mod that isn't opened is still noted") {
    const Graph graph = { { "a", { { "opt", true }, { "missing" } } } };
    std::unordered_set<std::string> auto_enabled;
    const std::vector<std::string> found = enable(graph, "a", auto_enabled);
    CHECK_EQ(found.size(), size_t(1));
    CHECK(!found.empty() && found[0] == "missing");
    CHECK(!auto_enabled.contains("opt"));
}

TEST_CASE("mod ids: a folder without a mod.json keeps its whole name (pack.v2 lost its .v2)") {
    CHECK(folder_rules::manifestless_mod_id("pack.v2", true) == "pack.v2");
    CHECK(folder_rules::manifestless_mod_id("My Pack 1.0.1", true) == "My Pack 1.0.1");
    CHECK(folder_rules::manifestless_mod_id("pack", true) == "pack");
}

TEST_CASE("mod ids: a container file without a mod.json drops only its extension") {
    CHECK(folder_rules::manifestless_mod_id("pack.v2.rtz", false) == "pack.v2");
    CHECK(folder_rules::manifestless_mod_id("pack.rtz", false) == "pack");
}
