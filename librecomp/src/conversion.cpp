// [wcw fullconv] The active conversion (AKI_CONVERSION): read once at the first mod scan. See conversion.hpp.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>

#include "librecomp/conversion.hpp"
#include "librecomp/conversion_codes.hpp"
#include "librecomp/files.hpp"
#include "ultramodern/error_handling.hpp"

namespace {
    std::mutex conversion_mutex;
    std::optional<recomp::mods::conversion::Conversion> active;
    std::filesystem::path active_folder;
    std::string port_rom_revision;
    std::string cheats_mod_id = "aki_cheats";

    std::string read_text(const std::filesystem::path &path) {
        std::ifstream file(path, std::ios::binary);
        std::stringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }
}

namespace {
    // A conversion that can't run must not start the game: its saves folder or mods would silently become the
    // player's normal ones.
    [[noreturn]] void refuse(const std::string &message) {
        fprintf(stderr, "[conversion] %s\n", message.c_str());
        ultramodern::error_handling::message_box((message + "\n\nThe game will close; your normal saves were not touched.").c_str());
        ULTRAMODERN_QUICK_EXIT();
    }
}

namespace recomp::mods {
    // Called by scan_mods: loads <config>/conversions/<AKI_CONVERSION>/aki.json and returns the folders whose mods the
    // session runs (the conversion's own, plus its texture dependency package when installed). Empty: no conversion.
    std::vector<std::filesystem::path> load_active_conversion(const std::filesystem::path &config_path) {
        std::lock_guard lock(conversion_mutex);
        active.reset();
        active_folder.clear();
        const char *id = std::getenv(conversion::EnvVar);
        if ((id == nullptr) || (id[0] == '\0')) {
            return {};
        }

        if (!conversion::valid_id(id)) {
            refuse(std::string(conversion::EnvVar) + "=" + id + " is not a valid conversion id.");
        }

        const std::filesystem::path folder = config_path / conversion::Folder / id;
        const std::string text = read_text(folder / conversion::ManifestFile);
        conversion::Conversion c;
        std::string error;
        if (text.empty() || !conversion::parse(text, id, c, error)) {
            refuse("Can't start conversion \"" + std::string(id) + "\": " + (text.empty() ? std::string("it isn't installed (no ") +
                (folder / conversion::ManifestFile).string() + ")" : error) + ".");
        }

        std::vector<std::filesystem::path> folders{ folder };
        if (!c.textures_package.empty()) {
            const std::filesystem::path package = config_path / conversion::Folder / conversion::PackagesFolder / c.textures_package;
            std::error_code ec;
            if (std::filesystem::is_directory(package, ec)) {
                folders.emplace_back(package);
            }
            else {
                fprintf(stderr, "[conversion] texture package %s isn't installed; running without it\n", c.textures_package.c_str());
            }
        }
        if (!c.textures_dir.empty()) {
            // An inline pack opens as a manifest-less mod folder (rt64.json at its root) from a scan of its parent.
            const std::filesystem::path pack = folder / std::filesystem::u8path(c.textures_dir);
            std::error_code ec;
            if (!std::filesystem::is_directory(pack, ec)) {
                fprintf(stderr, "[conversion] texture folder %s is missing; running without it\n", c.textures_dir.c_str());
            }
            else {
                const bool legacyNames = (c.textures_naming == "rice") || (c.textures_naming == "jabo");
                if (legacyNames && !std::filesystem::exists(pack / "rt64.json", ec)) {
                    // A pack as its author shipped it (Rice / GLideN64 or Jabo Direct3D8 names): rt64 matches the names
                    // at runtime, so the database needs no entries.
                    std::ofstream out(pack / "rt64.json", std::ios::binary);
                    out << R"({ "configuration": { "configurationVersion": 3, "hashVersion": 5, "autoPath": ")" <<
                        ((c.textures_naming == "rice") ? "rice" : "rt64") << R"(", "defaultShift": "none" }, "textures": [] })";
                }
                if (pack.parent_path() != folder) {
                    folders.emplace_back(pack.parent_path());
                }
            }
        }

        fprintf(stderr, "[conversion] running %s (\"%s\"):%s%s%s%s%s; saves in saves/%s\n", c.id.c_str(), c.name.c_str(),
            c.has_patch ? " patch" : "", c.has_codes ? " codes" : "", c.has_saves ? " saves" : "", c.has_textures ? " textures" : "",
            c.has_rt ? " rt" : "", c.id.c_str());
        active = std::move(c);
        active_folder = folder;
        return folders;
    }

    const conversion::Conversion *active_conversion() {
        std::lock_guard lock(conversion_mutex);
        return active ? &*active : nullptr;
    }

    std::string active_conversion_codes() {
        std::lock_guard lock(conversion_mutex);
        // The codes file's own "# rom:" tag is the authority: the cheats engine refuses another game's codes and
        // translates another revision's (AkiMods aki_cheats). The manifest's tag only feeds the launcher's notes.
        if (!active || !active->has_codes) {
            return {};
        }

        return read_text(active_folder / active->codes_file);
    }

    bool active_conversion_codes_runnable() {
        std::lock_guard lock(conversion_mutex);
        return active && active->has_codes;
    }

    std::string active_conversion_cheats_mod() {
        std::lock_guard lock(conversion_mutex);
        return cheats_mod_id;
    }

    void set_port_rom_revision(const std::string &tag) {
        std::lock_guard lock(conversion_mutex);
        port_rom_revision = tag;
    }

    void set_cheats_mod_id(const std::string &mod_id) {
        std::lock_guard lock(conversion_mutex);
        cheats_mod_id = mod_id;
    }

    namespace {
        std::once_flag native_codes_once;
        std::vector<conversion::codes::Code> native_codes;
        bool native_codes_ready = false;
    }

    bool active_native_codes_ready() {
        std::call_once(native_codes_once, []() {
            std::string text = active_conversion_codes();
            std::string port;
            {
                std::lock_guard lock(conversion_mutex);
                port = port_rom_revision;
            }

            if (text.empty() || port.empty()) {
                return;
            }

            std::string game_key;
            int native_rev = 0;
            conversion::codes::split_rom_tag(port, game_key, native_rev);
            conversion::codes::Report r;
            if (!conversion::codes::parse_file(text, game_key, native_rev, native_codes, r)) {
                return; // another revision: the cheats mod translates those
            }

            native_codes_ready = true;
            fprintf(stderr, "[conversion] codes: %d active (native table: %d writes, %d conditionals, %d repeaters, %zu KB of text)",
                r.added, r.writes, r.conditionals, r.repeaters, text.size() / 1024);
            if (r.enablers > 0) fprintf(stderr, ", %d enabler codes ignored (not needed)", r.enablers);
            if (r.buttons > 0) fprintf(stderr, ", %d button codes (88/89) ignored", r.buttons);
            if (r.unknown > 0) fprintf(stderr, ", %d unknown code types ignored", r.unknown);
            if (r.malformed > 0) fprintf(stderr, ", %d malformed lines, first at line %d", r.malformed, r.first_bad_line);
            fprintf(stderr, "\n");
        });

        return native_codes_ready;
    }

    void apply_active_native_codes(uint8_t *rdram, uint32_t rdram_size) {
        if (active_native_codes_ready()) {
            conversion::codes::apply(native_codes, rdram, rdram_size);
        }
    }
}
