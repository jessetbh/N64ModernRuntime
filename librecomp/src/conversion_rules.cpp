#include "librecomp/conversion.hpp"

#include "json/json.hpp"

namespace recomp::mods::conversion {
    bool valid_id(const std::string &id) {
        if (id.empty() || (id.size() > 48) || (id[0] == '.') || (id[0] == '_')) {
            return false;
        }

        for (char c : id) {
            const bool ok = ((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')) || (c == '_') || (c == '.') || (c == '-');
            if (!ok) {
                return false;
            }
        }

        return true;
    }

    bool parse(const std::string &json_text, const std::string &expected_id, Conversion &out, std::string &error) {
        using json = nlohmann::json;
        const json root = json::parse(json_text, nullptr, false);
        if (root.is_discarded() || !root.is_object()) {
            error = "aki.json is not valid JSON";
            return false;
        }

        if ((root.value("format", std::string()) != "aki-conversion") || (root.value("format_version", 0) != 1)) {
            error = "not an aki-conversion v1 manifest";
            return false;
        }

        Conversion c;
        c.id = root.value("id", std::string());
        c.name = root.value("name", c.id);
        c.game = root.value("game", std::string());
        if (!valid_id(c.id)) {
            error = "invalid conversion id \"" + c.id + "\"";
            return false;
        }

        if (!expected_id.empty() && (c.id != expected_id)) {
            error = "aki.json id \"" + c.id + "\" doesn't match its folder \"" + expected_id + "\"";
            return false;
        }

        const auto components = root.find("components");
        if ((components == root.end()) || !components->is_object() || components->empty()) {
            error = "no components";
            return false;
        }

        auto object = [&](const char *key) -> const json * {
            const auto it = components->find(key);
            return ((it != components->end()) && (it->is_object() || it->is_array())) ? &*it : nullptr;
        };

        if (const json *patch = object("patch")) {
            c.has_patch = patch->is_object() && patch->contains("file");
        }

        if (const json *codes = object("codes")) {
            c.codes_file = codes->value("file", std::string());
            c.codes_rom = codes->value("rom", codes->value("rom_revision", std::string()));
            c.has_codes = !c.codes_file.empty();
        }

        if (const json *saves = object("saves")) {
            c.has_saves = saves->is_array() && !saves->empty();
        }

        if (const json *textures = object("textures")) {
            c.textures_package = textures->value("package", std::string());
            c.textures_dir = textures->value("dir", std::string());
            while (!c.textures_dir.empty() && ((c.textures_dir.back() == '/') || (c.textures_dir.back() == '\\'))) {
                c.textures_dir.pop_back();
            }
            c.textures_naming = textures->value("naming", std::string());
            c.has_textures = textures->contains("file") || !c.textures_package.empty() || !c.textures_dir.empty();
        }

        if (const json *rt = object("rt")) {
            c.rt_lights_file = rt->value("lights", std::string());
            c.rt_materials_file = rt->value("materials", std::string());
            c.has_rt = !c.rt_lights_file.empty() || !c.rt_materials_file.empty();
        }

        // Paths stay inside the conversion folder.
        for (const std::string *path : { &c.codes_file, &c.textures_dir, &c.rt_lights_file, &c.rt_materials_file }) {
            if ((path->find("..") != std::string::npos) || (!path->empty() && ((path->front() == '/') || (path->front() == '\\') ||
                (path->find(':') != std::string::npos)))) {
                error = "path leaves the conversion folder: " + *path;
                return false;
            }
        }

        if (!c.has_patch && !c.has_codes && !c.has_saves && !c.has_textures && !c.has_rt) {
            error = "no usable components";
            return false;
        }

        out = std::move(c);
        return true;
    }

    std::unordered_set<std::string> session_enabled_mods(const std::vector<std::string> &conversion_mod_ids,
        bool codes_runnable, const std::string &cheats_mod_id)
    {
        std::unordered_set<std::string> enabled(conversion_mod_ids.begin(), conversion_mod_ids.end());
        if (codes_runnable && !cheats_mod_id.empty()) {
            enabled.emplace(cheats_mod_id);
        }

        return enabled;
    }

    bool resolve_save_subfolder(const char *env_value, const std::string &patch_mod_id, std::string &out) {
        if ((env_value == nullptr) || (env_value[0] == '\0')) {
            out = patch_mod_id;
            return true;
        }

        if (!valid_id(env_value)) {
            out.clear();
            return false;
        }

        out = env_value;
        return true;
    }

    bool codes_runnable(const std::string &codes_rom, const std::string &port_rom) {
        // Codes for the same game run: the cheats engine (AkiMods aki_cheats) reads the file's "# rom:" tag and
        // translates another revision's addresses as it applies them (No Mercy Rev 0 -> Rev 1), dropping codes it
        // can't map. Untagged codes and other games' codes stay off (Rev 0 codes written as they are corrupt Rev 1).
        auto game = [](const std::string &tag) { return tag.substr(0, tag.find("-rev")); };
        return !codes_rom.empty() && !port_rom.empty() && (game(codes_rom) == game(port_rom));
    }
}
