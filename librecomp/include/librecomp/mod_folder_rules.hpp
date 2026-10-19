#ifndef __RECOMP_MOD_FOLDER_RULES_HPP__
#define __RECOMP_MOD_FOLDER_RULES_HPP__

// [wcw] Pure rule for folders in the mods folder that have no mod.json (aki-roadmap#48): a folder holding the content
// file of a container type that needs no manifest (a texture pack's rt64.json, the .rtz container) opens as that kind
// of mod, named after the folder, exactly as the zipped container would. Unit tested in tests/wcw/mod_folder_rules_tests.cpp.

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace recomp {
    namespace mods {
        namespace folder_rules {
            // A registered container type: the content types it may hold (empty = any) and whether it needs a mod.json.
            struct Container {
                std::vector<size_t> content_types;
                bool requires_manifest = true;
            };

            // The content types a folder without a mod.json opens with: every type allowed by a manifest-less container
            // whose content file (content_filenames[type]) the folder holds. Empty = the folder isn't such a mod (it
            // then needs a mod.json, as before). Containers that allow any content don't count (no content file to
            // look for), and each type appears once, in registration order.
            std::vector<size_t> manifestless_folder_types(const std::vector<Container> &containers,
                const std::vector<std::string> &content_filenames, const std::function<bool(const std::string &)> &has_file);

            // The mod id (and display name) of a mod without a mod.json, from its file or folder name: a folder keeps
            // its whole name ("pack.v2"), a container file drops its extension ("pack.v2.rtz" is "pack.v2"). Folders
            // used to lose everything after their last dot (aki-roadmap#100).
            std::string manifestless_mod_id(const std::string &file_name, bool is_directory);
        }
    }
}

#endif
