#include "librecomp/conversion.hpp"

#include <algorithm>
#include <cstdio>
#include <system_error>

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

    namespace {
        bool pack_id_ok(const std::string &id) {
            if (id.empty() || (id.size() > 64) || (id[0] == '.')) {
                return false;
            }
            for (char ch : id) {
                if (!(((ch >= 'a') && (ch <= 'z')) || ((ch >= 'A') && (ch <= 'Z')) || ((ch >= '0') && (ch <= '9')) ||
                      (ch == '_') || (ch == '-') || (ch == '.'))) {
                    return false;
                }
            }
            return true;
        }

        // "0x0404", "404" (hex, as models.txt and the roster tools write them) or a number.
        bool wrestler_id(const nlohmann::json &v, uint32_t &out) {
            if (v.is_number_unsigned()) {
                out = v.get<uint32_t>();
                return out <= 0xFFFF;
            }
            if (!v.is_string()) {
                return false;
            }
            std::string s = v.get<std::string>();
            if ((s.size() > 2) && (s[0] == '0') && ((s[1] == 'x') || (s[1] == 'X'))) {
                s = s.substr(2);
            }
            if (s.empty() || (s.size() > 4) || (s.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)) {
                return false;
            }
            out = uint32_t(std::stoul(s, nullptr, 16));
            return true;
        }

        // components.models into c. A bad dir leaves has_models false; a bad format or slot keeps the component
        // (the conversion still runs) with models_refusal saying why its models are off.
        void parse_models(const nlohmann::json &models, Conversion &c) {
            if (!models.is_object()) {
                return;
            }
            c.models_dir = models.value("dir", std::string());
            while (!c.models_dir.empty() && ((c.models_dir.back() == '/') || (c.models_dir.back() == '\\'))) {
                c.models_dir.pop_back();
            }
            c.has_models = !c.models_dir.empty();
            if (!c.has_models) {
                return;
            }
            const auto format = models.find("format");
            c.models_format = (format == models.end()) ? 1 : (format->is_number_integer() ? format->get<int>() : -1);
            c.models_roster = models.value("roster", std::string());
            if (c.models_format < 1) {
                c.models_refusal = "its models component has no usable \"format\" (the model pack version, 1 to " +
                    std::to_string(ModelsFormat) + ")";
                return;
            }
            if (c.models_format > ModelsFormat) {
                c.models_refusal = "its models are format " + std::to_string(c.models_format) + ", and this build reads up to " +
                    std::to_string(ModelsFormat) + ": update the game";
                return;
            }
            const auto slots = models.find("slots");
            if (slots == models.end()) {
                return;
            }
            c.models_has_slots = true;
            if (!slots->is_array()) {
                c.models_refusal = "its models \"slots\" is not a list";
                return;
            }
            for (size_t i = 0; i < slots->size(); i++) {
                const nlohmann::json &s = (*slots)[i];
                Conversion::ModelSlot slot;
                const auto wrestler = s.is_object() ? s.find("wrestler") : s.end();
                const auto costume = s.is_object() ? s.find("costume") : s.end();
                const bool ok = s.is_object() && (wrestler != s.end()) && wrestler_id(*wrestler, slot.wrestler) &&
                    ((costume == s.end()) || (costume->is_number_integer() && (costume->get<int>() >= 0) && (costume->get<int>() < 256)));
                slot.pack = ok ? s.value("pack", std::string()) : std::string();
                if (!ok || !pack_id_ok(slot.pack)) {
                    c.models_refusal = "its models slot " + std::to_string(i + 1) + " needs a wrestler id (\"0x0404\"), a pack id and "
                        "an optional costume 0-255";
                    c.models_slots.clear();
                    return;
                }
                slot.costume = (costume == s.end()) ? -1 : costume->get<int>();
                slot.name = s.value("name", std::string());
                c.models_slots.push_back(std::move(slot));
            }
        }
    }

    std::string models_txt(const std::vector<Conversion::ModelSlot> &slots) {
        std::string out = "# from aki.json components.models.slots\n";
        char line[160];
        for (const Conversion::ModelSlot &s : slots) {
            if (s.costume >= 0) {
                snprintf(line, sizeof(line), "%s wrestler 0x%04X costume %d\n", s.pack.c_str(), s.wrestler, s.costume);
            }
            else {
                snprintf(line, sizeof(line), "%s wrestler 0x%04X\n", s.pack.c_str(), s.wrestler);
            }
            out += line;
        }
        return out;
    }

    namespace {
    // parse() without the type guard: nlohmann's value() throws json::type_error on a value of the wrong type.
    bool parse_manifest(const std::string &json_text, const std::string &expected_id, Conversion &out, std::string &error) {
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

        // These components are objects. One written as anything else (a list, a string) is refused, not skipped:
        // the launcher still lists it, so skipping would run the conversion without it (aki-roadmap#243).
        for (const char *key : { "patch", "codes", "textures", "rt", "models" }) {
            const auto it = components->find(key);
            if ((it != components->end()) && !it->is_object()) {
                error = std::string("components.") + key + " must be an object";
                return false;
            }
        }
        auto object = [&](const char *key) -> const json * {
            const auto it = components->find(key);
            return (it != components->end()) ? &*it : nullptr;
        };

        if (const json *patch = object("patch")) {
            c.has_patch = patch->contains("file");
        }

        if (const json *codes = object("codes")) {
            c.codes_file = codes->value("file", std::string());
            c.codes_rom = codes->value("rom", codes->value("rom_revision", std::string()));
            c.has_codes = !c.codes_file.empty();
        }

        if (const json *saves = object("saves")) {
            c.has_saves = saves->is_array() && !saves->empty();
            if (saves->is_array()) {
                for (const json &save : *saves) {
                    if (!save.is_object()) {
                        continue;
                    }
                    const std::string file = save.value("file", std::string());
                    if (!file.empty() && (save.value("mode", std::string("seed")) == "seed")) {
                        c.seed_saves.push_back(file);
                    }
                }
            }
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

        if (const json *models = object("models")) {
            parse_models(*models, c);
        }

        if (const auto addons = root.find("addons"); addons != root.end() && addons->is_object()) {
            const auto allow = addons->find("allow");
            const std::string a = (allow != addons->end() && allow->is_string()) ? allow->get<std::string>() : std::string();
            c.addons_allow = (a == "none") ? AddonsAllow::None : (a == "all") ? AddonsAllow::All : AddonsAllow::Data;
            if (const auto mods = addons->find("mods"); mods != addons->end() && mods->is_array()) {
                for (const json &m : *mods) {
                    if (m.is_string()) {
                        c.addons_mods.push_back(m.get<std::string>());
                    }
                }
            }
        }

        // Paths stay inside the conversion folder.
        for (const std::string *path : { &c.codes_file, &c.textures_dir, &c.rt_lights_file, &c.rt_materials_file, &c.models_dir, &c.models_roster }) {
            if ((path->find("..") != std::string::npos) || (!path->empty() && ((path->front() == '/') || (path->front() == '\\') ||
                (path->find(':') != std::string::npos)))) {
                error = "path leaves the conversion folder: " + *path;
                return false;
            }
        }
        for (const std::string &path : c.seed_saves) {
            if ((path.find("..") != std::string::npos) || (path.front() == '/') || (path.front() == '\\') || (path.find(':') != std::string::npos)) {
                error = "path leaves the conversion folder: " + path;
                return false;
            }
        }

        if (!c.has_patch && !c.has_codes && !c.has_saves && !c.has_textures && !c.has_rt && !c.has_models) {
            error = "no usable components";
            return false;
        }

        out = std::move(c);
        return true;
    }
    }

    bool parse(const std::string &json_text, const std::string &expected_id, Conversion &out, std::string &error) {
        // A wrongly typed value ("id": 5, "codes": { "file": 5 }) refuses the conversion like any other bad
        // manifest. Uncaught, the exception ended the game in list_installed_conversions() (aki-roadmap#243).
        try {
            return parse_manifest(json_text, expected_id, out, error);
        }
        catch (const nlohmann::json::exception &e) {
            error = std::string("aki.json has a value of the wrong type: ") + e.what();
            return false;
        }
    }

    std::string models_refusal(const std::string &relink_json) {
        if (relink_json.empty()) {
            return {};
        }
        const nlohmann::json root = nlohmann::json::parse(relink_json, nullptr, false);
        if (root.is_discarded() || !root.is_object()) {
            return "its patch's relink.json doesn't parse, so the memory it uses is unknown";
        }
        // [start, end) as KSEG0/KSEG1/physical addresses; compared physically.
        auto overlaps = [](uint64_t start, uint64_t end) {
            if (end <= start) {
                return false;
            }
            const uint64_t ps = start & 0x1FFFFFFF;
            const uint64_t pe = ps + (end - start);
            return (ps < ModelsWindowEnd) && (pe > ModelsWindowStart);
        };
        auto number = [](const nlohmann::json &v) -> uint64_t { return v.is_number_unsigned() ? v.get<uint64_t>() : 0; };
        uint64_t hit = 0;
        bool found = false;
        auto check = [&](uint64_t start, uint64_t end) {
            if (!found && overlaps(start, end)) {
                hit = start;
                found = true;
            }
        };
        auto list = [&](const char *key) -> const nlohmann::json & {
            static const nlohmann::json empty = nlohmann::json::array();
            const auto it = root.find(key);
            return ((it != root.end()) && it->is_array()) ? *it : empty;
        };
        for (const char *key : { "code_ranges", "data_ranges" }) {
            for (const nlohmann::json &r : list(key)) {
                if (r.is_array() && (r.size() >= 2)) {
                    check(number(r[0]), number(r[1]));
                }
            }
        }
        for (const char *key : { "preload", "zero" }) {
            for (const nlohmann::json &r : list(key)) {
                if (r.is_object()) {
                    const uint64_t ram = number(r.value("ram", nlohmann::json()));
                    check(ram, ram + number(r.value("size", nlohmann::json())));
                }
            }
        }
        if (!found) {
            return {};
        }
        char address[16];
        snprintf(address, sizeof(address), "0x%08X", uint32_t(hit));
        return std::string("its patch uses Expansion RAM (from ") + address + "), where model packs load; custom models "
            "in relinked conversions need aki-roadmap#156 (after 0.3.0)";
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

    PlayerAddons parse_player_addons(const std::string &json_text) {
        PlayerAddons out;
        const nlohmann::json root = nlohmann::json::parse(json_text, nullptr, false);
        if (root.is_discarded() || !root.is_object()) {
            return out;
        }
        for (auto [key, list] : { std::pair{ "enabled", &out.enabled }, std::pair{ "disabled", &out.disabled } }) {
            const auto it = root.find(key);
            if (it == root.end() || !it->is_array()) {
                continue;
            }
            for (const nlohmann::json &v : *it) {
                if (v.is_string()) {
                    list->push_back(v.get<std::string>());
                }
            }
        }
        return out;
    }

    std::unordered_set<std::string> session_enabled_mods(const std::vector<std::string> &conversion_mod_ids,
        bool codes_runnable, const std::string &cheats_mod_id, const std::vector<PlayerMod> &player_mods,
        const Conversion &conversion, const PlayerAddons &player, std::vector<std::string> *why)
    {
        std::unordered_set<std::string> enabled = session_enabled_mods(conversion_mod_ids, codes_runnable, cheats_mod_id);
        // aki-roadmap#226: the model import mod is part of a conversion with usable models, as the cheats engine is of
        // one with codes. Refused models (models_refusal) leave it to the add-on rules below.
        const bool models_on = conversion.has_models && conversion.models_refusal.empty();
        if (models_on) {
            enabled.emplace(ModelsModId);
        }
        auto listed = [](const std::vector<std::string> &list, const std::string &id) {
            return std::find(list.begin(), list.end(), id) != list.end();
        };
        auto note = [&](const PlayerMod &m, bool on, const char *reason) {
            if (why) {
                why->push_back(m.id + ": " + (on ? "on" : "off") + " (" + reason + ")");
            }
        };
        for (const PlayerMod &m : player_mods) {
            if (enabled.contains(m.id)) {
                if (models_on && (m.id == ModelsModId)) {
                    note(m, true, "model import: the conversion has models");
                }
                continue;   // the conversion's own, the cheats engine for its codes, or model import for its models
            }
            bool on = false;
            const char *reason;
            if (m.rom_patch) {
                reason = "another ROM hack: only the conversion's own patch runs";
            }
            else if (listed(player.disabled, m.id)) {
                reason = "you turned it off for this conversion";
            }
            else if (listed(player.enabled, m.id)) {
                on = true;
                reason = "you turned it on for this conversion";
            }
            else if (!m.enabled) {
                reason = "not enabled";
            }
            else if (conversion.addons_allow == AddonsAllow::None) {
                reason = "this conversion runs no add-ons";
            }
            else if (m.id == cheats_mod_id) {
                reason = "cheats engine: the conversion has no codes for it (turn it on for this conversion to use your own)";
            }
            else if (!m.code) {
                on = true;
                reason = "data pack";
            }
            else if (conversion.addons_allow == AddonsAllow::All || listed(conversion.addons_mods, m.id)) {
                on = true;
                reason = "code add-on this conversion allows";
            }
            else {
                reason = "code add-on this conversion doesn't list";
            }
            if (on) {
                enabled.insert(m.id);
            }
            note(m, on, reason);
        }
        return enabled;
    }

    StartupChoice choose_startup(const char *env_value, const std::string &choice_file_text) {
        StartupChoice choice;
        if ((env_value != nullptr) && (env_value[0] != '\0')) {
            choice.id = env_value;
            choice.from_launcher = true;
            return choice;
        }

        const std::string line = choice_file_text.substr(0, choice_file_text.find_first_of("\r\n"));
        const size_t first = line.find_first_not_of(" \t");
        const size_t last = line.find_last_not_of(" \t");
        choice.id = (first == std::string::npos) ? std::string() : line.substr(first, last - first + 1);
        return choice;
    }

    std::string check_choice(const std::string &id, const std::string &manifest_text, Conversion &out) {
        if (!valid_id(id)) {
            return "\"" + id + "\" is not a valid conversion id";
        }
        if (manifest_text.empty()) {
            return "it isn't installed any more";
        }

        std::string error;
        if (!parse(manifest_text, id, out, error)) {
            return error;
        }
        return {};
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

    std::filesystem::path rt_lights_path(const Conversion &c, const std::filesystem::path &folder, const char *env_value) {
        if (c.rt_lights_file.empty() || ((env_value != nullptr) && (env_value[0] != 0))) {
            return {};
        }
        return folder / std::filesystem::u8path(c.rt_lights_file);
    }

    SeedReport seed_saves(const Conversion &c, const std::filesystem::path &folder, const std::filesystem::path &saves_folder) {
        SeedReport r;
        for (const std::string &file : c.seed_saves) {
            const std::filesystem::path from = folder / std::filesystem::u8path(file);
            const std::filesystem::path name = from.filename();
            std::error_code ec;
            if (name.empty()) {
                r.failed++;
                r.lines.push_back("seed save " + file + " names no file; skipped");
                continue;
            }

            const std::u8string name8 = name.u8string();
            const std::string name_text(reinterpret_cast<const char *>(name8.data()), name8.size());
            const std::filesystem::path to = saves_folder / name;
            if (std::filesystem::exists(to, ec)) {
                r.kept++;
                r.lines.push_back("save " + file + ": kept the existing saves/" + c.id + "/" + name_text);
                continue;
            }
            if (!std::filesystem::is_regular_file(from, ec)) {
                r.failed++;
                r.lines.push_back("seed save " + file + " is missing from the conversion; the game starts without it");
                continue;
            }

            // Copied beside it first, so a failed copy never leaves a half save the game would then read.
            std::filesystem::create_directories(saves_folder, ec);
            const std::filesystem::path temp = saves_folder / (name8 + u8".seed-tmp");
            std::filesystem::remove(temp, ec);
            ec.clear();
            std::filesystem::copy_file(from, temp, ec);
            if (!ec) {
                std::filesystem::rename(temp, to, ec);
            }
            if (ec) {
                std::error_code ignored;
                std::filesystem::remove(temp, ignored);
                r.failed++;
                r.lines.push_back("seed save " + file + " couldn't be copied: " + ec.message());
                continue;
            }
            r.seeded++;
            r.lines.push_back("seeded saves/" + c.id + "/" + name_text + " from " + file);
        }
        return r;
    }

    bool codes_runnable(const std::string &codes_rom, const std::string &port_rom) {
        // Codes for the same game run: the cheats engine (AkiMods aki_cheats) reads the file's "# rom:" tag and
        // translates another revision's addresses as it applies them (No Mercy Rev 0 -> Rev 1), dropping codes it
        // can't map. Untagged codes and other games' codes stay off (Rev 0 codes written as they are corrupt Rev 1).
        auto game = [](const std::string &tag) { return tag.substr(0, tag.find("-rev")); };
        return !codes_rom.empty() && !port_rom.empty() && (game(codes_rom) == game(port_rom));
    }
}
