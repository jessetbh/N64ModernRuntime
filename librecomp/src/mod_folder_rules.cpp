#include "librecomp/mod_folder_rules.hpp"

#include <algorithm>
#include <filesystem>

std::vector<size_t> recomp::mods::folder_rules::manifestless_folder_types(const std::vector<Container> &containers,
    const std::vector<std::string> &content_filenames, const std::function<bool(const std::string &)> &has_file) {
    std::vector<bool> allowed(content_filenames.size(), false);
    for (const Container &container : containers) {
        if (container.requires_manifest) {
            continue;
        }

        for (size_t type : container.content_types) {
            if (type < allowed.size()) {
                allowed[type] = true;
            }
        }
    }

    std::vector<size_t> types;
    for (size_t type = 0; type < content_filenames.size(); type++) {
        if (allowed[type] && !content_filenames[type].empty() && has_file(content_filenames[type])) {
            types.emplace_back(type);
        }
    }

    return types;
}

std::string recomp::mods::folder_rules::manifestless_mod_id(const std::string &file_name, bool is_directory) {
    if (is_directory) {
        return file_name;
    }

    return std::filesystem::path(file_name).stem().string();
}
