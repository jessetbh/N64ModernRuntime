// [wcw fullconv] The active conversion (AKI_CONVERSION): read once at the first mod scan. See conversion.hpp.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>

#include "librecomp/conversion.hpp"
#include "librecomp/conversion_codes.hpp"
#include "librecomp/live_code_rules.hpp"
#include "librecomp/files.hpp"
#include "librecomp/game.hpp"
#include "ultramodern/error_handling.hpp"

extern "C" void wcw_commit_low_user_segment(uint8_t* rdram);   // recomp.cpp

namespace {
    std::mutex conversion_mutex;
    std::optional<recomp::mods::conversion::Conversion> active;
    std::filesystem::path active_folder;
    std::string port_rom_revision;
    std::string cheats_mod_id = "aki_cheats";

    // The session's conversion, decided once (aki-roadmap#57): the mod scan and the save folder both use it.
    bool session_decided = false;
    recomp::mods::conversion::StartupChoice session_choice;
    // A Mods-menu choice's aki.json as checked when the session was decided, so the load can't fail on a second read.
    std::optional<recomp::mods::conversion::Conversion> session_checked;
    std::string choice_notice; // why a Mods-menu choice couldn't run (logged at the mod scan, shown by the Mods menu)

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
    // Decides once which conversion this session runs: the launcher's AKI_CONVERSION, else the Mods-menu choice in
    // conversions/active.txt. A choice that can't run (bad id, not installed, unreadable aki.json) is dropped: the game
    // starts normally, active.txt is removed and the scan shows why. Called with conversion_mutex held.
    static const conversion::StartupChoice &decide_session(const std::filesystem::path &config_path) {
        if (session_decided) {
            return session_choice;
        }

        session_decided = true;
        const std::filesystem::path choice_file = config_path / conversion::Folder / conversion::ChoiceFile;
        session_choice = conversion::choose_startup(std::getenv(conversion::EnvVar), read_text(choice_file));
        if (!session_choice.lenient()) {
            return session_choice;
        }

        const std::string id = session_choice.id;
        conversion::Conversion c;
        const std::string reason = conversion::check_choice(id, conversion::valid_id(id) ?
            read_text(config_path / conversion::Folder / id / conversion::ManifestFile) : std::string(), c);
        if (reason.empty()) {
            session_checked = std::move(c);
        }
        else {
            choice_notice = "The conversion chosen in the Mods menu (" + id + ") can't run: " + reason +
                ". The game starts normally with your own saves, and the choice was cleared.";
            std::error_code ec;
            std::filesystem::remove(choice_file, ec);
            session_choice = conversion::StartupChoice{};
        }

        return session_choice;
    }

    std::string session_conversion_id() {
        std::lock_guard lock(conversion_mutex);
        const std::filesystem::path config_path = recomp::get_config_path();
        if (config_path.empty()) {
            const char *env = std::getenv(conversion::EnvVar);
            return (env != nullptr) ? std::string(env) : std::string();
        }

        return decide_session(config_path).id;
    }

    bool session_conversion_from_launcher() {
        std::lock_guard lock(conversion_mutex);
        const std::filesystem::path config_path = recomp::get_config_path();
        if (config_path.empty()) {
            const char *env = std::getenv(conversion::EnvVar);
            return (env != nullptr) && (env[0] != '\0');
        }

        return decide_session(config_path).from_launcher;
    }

    std::string conversion_choice_notice() {
        std::lock_guard lock(conversion_mutex);
        return choice_notice;
    }

    std::string chosen_conversion() {
        const std::filesystem::path config_path = recomp::get_config_path();
        if (config_path.empty()) {
            return {};
        }

        return conversion::choose_startup(nullptr, read_text(config_path / conversion::Folder / conversion::ChoiceFile)).id;
    }

    bool set_chosen_conversion(const std::string &id) {
        const std::filesystem::path config_path = recomp::get_config_path();
        if (config_path.empty() || (!id.empty() && !conversion::valid_id(id))) {
            return false;
        }

        const std::filesystem::path folder = config_path / conversion::Folder;
        const std::filesystem::path file = folder / conversion::ChoiceFile;
        std::error_code ec;
        if (id.empty()) {
            std::filesystem::remove(file, ec);
            return !ec;
        }

        std::filesystem::create_directories(folder, ec);
        const std::filesystem::path temp = folder / (std::string(conversion::ChoiceFile) + ".tmp");
        bool written;
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out << id << "\n";
            out.close();
            written = !out.fail();
        }

        if (written) {
            std::filesystem::rename(temp, file, ec);
        }
        if (!written || ec) {
            std::filesystem::remove(temp, ec);
            return false;
        }
        return true;
    }

    std::vector<InstalledConversion> list_installed_conversions() {
        std::vector<InstalledConversion> out;
        const std::filesystem::path config_path = recomp::get_config_path();
        if (config_path.empty()) {
            return out;
        }

        std::error_code ec;
        std::filesystem::directory_iterator it(config_path / conversion::Folder, ec);
        for (; !ec && (it != std::filesystem::directory_iterator()); it.increment(ec)) {
            const std::filesystem::directory_entry &entry = *it;
            std::error_code entry_ec;
            if (!entry.is_directory(entry_ec)) {
                continue;
            }

            const std::u8string name8 = entry.path().filename().u8string();
            const std::string id(reinterpret_cast<const char *>(name8.data()), name8.size());
            if (id.empty() || (id[0] == '_') || (id[0] == '.')) {
                continue; // _packages and hidden folders
            }

            InstalledConversion installed;
            installed.folder = entry.path();
            const std::string text = read_text(entry.path() / conversion::ManifestFile);
            if (text.empty()) {
                continue;
            }

            if (!conversion::valid_id(id)) {
                // Listed rather than skipped, so a wrongly named folder (AEW64) doesn't just go missing.
                installed.error = "its folder name isn't a conversion id (lowercase letters, digits, '_', '.' and '-')";
                installed.conversion.id = id;
                installed.conversion.name = id;
            }
            else if (!conversion::parse(text, id, installed.conversion, installed.error)) {
                installed.conversion.id = id;
                installed.conversion.name = id;
            }

            out.emplace_back(std::move(installed));
        }

        std::sort(out.begin(), out.end(), [](const InstalledConversion &a, const InstalledConversion &b) {
            return a.conversion.name < b.conversion.name;
        });
        return out;
    }

    // Called by scan_mods: loads <config>/conversions/<AKI_CONVERSION>/aki.json and returns the folders whose mods the
    // session runs (the conversion's own, plus its texture dependency package when installed). Empty: no conversion.
    std::vector<std::filesystem::path> load_active_conversion(const std::filesystem::path &config_path) {
        std::lock_guard lock(conversion_mutex);
        active.reset();
        active_folder.clear();
        const conversion::StartupChoice &choice = decide_session(config_path);
        if (!choice_notice.empty()) {
            // Not a message box: the game starts normally, so nothing needs confirming before it (a modal here also
            // held up unattended starts). The Mods menu's Conversions dialog shows it.
            fprintf(stderr, "[conversion] %s\n", choice_notice.c_str());
        }

        const std::string id_text = choice.id;
        const char *id = id_text.c_str();
        if (id_text.empty()) {
            return {};
        }

        const std::filesystem::path folder = config_path / conversion::Folder / id;
        conversion::Conversion c;
        if (session_checked) {
            c = *session_checked; // a Mods-menu choice, checked by decide_session
        }
        else {
            if (!conversion::valid_id(id)) {
                refuse(std::string(conversion::EnvVar) + "=" + id + " is not a valid conversion id.");
            }

            const std::string text = read_text(folder / conversion::ManifestFile);
            std::string error;
            if (text.empty() || !conversion::parse(text, id, c, error)) {
                refuse("Can't start conversion \"" + std::string(id) + "\": " + (text.empty() ? std::string("it isn't installed (no ") +
                    (folder / conversion::ManifestFile).string() + ")" : error) + ".");
            }
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

        if (c.has_models) {
            std::error_code ec;
            if (!std::filesystem::is_directory(folder / std::filesystem::u8path(c.models_dir), ec)) {
                fprintf(stderr, "[conversion] models folder %s is missing; running without models\n", c.models_dir.c_str());
            }
            for (const conversion::Conversion::ModelSlot &s : c.models_slots) {
                fprintf(stderr, "[conversion] model slot: wrestler 0x%04X%s%s -> %s\n", s.wrestler, s.name.empty() ? "" : " ",
                    s.name.c_str(), s.pack.c_str());
            }
        }

        fprintf(stderr, "[conversion] running %s (\"%s\"):%s%s%s%s%s%s; saves in saves/%s\n", c.id.c_str(), c.name.c_str(),
            c.has_patch ? " patch" : "", c.has_codes ? " codes" : "", c.has_saves ? " saves" : "", c.has_textures ? " textures" : "",
            c.has_rt ? " rt" : "", c.has_models ? " models" : "", c.id.c_str());
        active = std::move(c);
        active_folder = folder;
        return folders;
    }

    const conversion::Conversion *active_conversion() {
        std::lock_guard lock(conversion_mutex);
        return active ? &*active : nullptr;
    }

    void set_active_conversion_models_refusal(const std::string &reason) {
        std::lock_guard lock(conversion_mutex);
        if (active) {
            active->models_refusal = reason;
        }
    }

    std::filesystem::path active_conversion_models_dir() {
        std::lock_guard lock(conversion_mutex);
        if (!active || !active->has_models || !active->models_refusal.empty()) {
            return {};
        }
        return active_folder / std::filesystem::u8path(active->models_dir);
    }

    bool active_conversion_models_txt(std::string &out) {
        std::lock_guard lock(conversion_mutex);
        if (!active || !active->has_models || !active->models_refusal.empty() || !active->models_has_slots) {
            return false;
        }
        out = conversion::models_txt(active->models_slots);
        return true;
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
            // Packs are tested on lenient emulators, and their codes can drive the game down paths stock never takes: Some No
            // Mercy Mod's Better Managers makes a manager act, whose target slot is 0xFFFF, and stock code then reads through a
            // null pointer (aki-roadmap#194). As for de-relinked hacks, the low user segment reads as zero pages. Stock games,
            // with no codes, keep faulting there.
            static std::once_flag low_segment_once;
            std::call_once(low_segment_once, [rdram]() { wcw_commit_low_user_segment(rdram); });
            conversion::codes::apply(native_codes, rdram, rdram_size);
            live_codes::after_apply(native_codes, rdram, rdram_size);
        }
    }
}
