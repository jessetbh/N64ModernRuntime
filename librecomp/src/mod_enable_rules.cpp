#include "librecomp/mod_enable_rules.hpp"

std::vector<std::string> recomp::mods::enable_rules::newly_auto_enabled(const std::string &mod_id,
    const std::function<std::vector<Dependency>(const std::string &)> &dependencies_of, std::unordered_set<std::string> &auto_enabled) {
    std::vector<std::string> found;
    std::vector<std::string> mod_stack;
    mod_stack.emplace_back(mod_id);
    while (!mod_stack.empty()) {
        std::string mod_from_stack = std::move(mod_stack.back());
        mod_stack.pop_back();

        for (const Dependency &dependency : dependencies_of(mod_from_stack)) {
            if (!dependency.skipped && !auto_enabled.contains(dependency.mod_id)) {
                auto_enabled.emplace(dependency.mod_id);
                mod_stack.emplace_back(dependency.mod_id);
                found.emplace_back(dependency.mod_id);
            }
        }
    }

    return found;
}
