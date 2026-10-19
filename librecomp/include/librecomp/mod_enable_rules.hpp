#ifndef __RECOMP_MOD_ENABLE_RULES_HPP__
#define __RECOMP_MOD_ENABLE_RULES_HPP__

// [wcw] Pure rule for the mods that enabling a mod auto-enables (its dependencies, aki-roadmap#100). Unit tested in
// tests/wcw/mod_enable_rules_tests.cpp.

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace recomp {
    namespace mods {
        namespace enable_rules {
            // A dependency as the walk sees it: the mod it names, and whether it's skipped (optional, or deprecated
            // at the version asked for).
            struct Dependency {
                std::string mod_id;
                bool skipped = false;
            };

            // The mods that enabling mod_id auto-enables, in the order they're found: its dependencies that aren't
            // skipped, then theirs, leaving out any already in auto_enabled (each one found is added to it).
            // dependencies_of(id) is empty for a mod that isn't opened. on_enabled fires for each mod returned, with
            // that mod's own handle: it used to fire with the handle of the mod that depends on it.
            std::vector<std::string> newly_auto_enabled(const std::string &mod_id,
                const std::function<std::vector<Dependency>(const std::string &)> &dependencies_of, std::unordered_set<std::string> &auto_enabled);
        }
    }
}

#endif
