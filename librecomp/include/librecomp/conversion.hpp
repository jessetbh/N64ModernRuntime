#ifndef __RECOMP_CONVERSION_HPP__
#define __RECOMP_CONVERSION_HPP__

// [wcw fullconv] Conversions: one package per full-conversion mod (a ROM patch, GameShark codes, saves, a texture pack
// and ray-tracing data, in any mix), installed in <config>/conversions/<id>/ with an aki.json manifest
// (plans/fullconv-manifest.md). AKI_CONVERSION=<id> (set by AKI Launcher) runs the game with exactly that conversion:
// its mods on, every other mod off for the session, its saves in saves/<id>/. The parsing and the enable rule are pure
// and unit tested (tests/wcw/conversion_tests.cpp).

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace recomp {
    namespace mods {
        namespace conversion {
            // The env var AKI Launcher sets.
            constexpr const char *EnvVar = "AKI_CONVERSION";
            // Install folder under the config folder, and the shared folder for dependency packages (texture packs).
            constexpr const char *Folder = "conversions";
            constexpr const char *PackagesFolder = "_packages";
            constexpr const char *ManifestFile = "aki.json";

            struct Conversion {
                std::string id;
                std::string name;
                std::string game;
                bool has_patch = false;
                bool has_codes = false;
                bool has_saves = false;
                bool has_textures = false;
                bool has_rt = false;
                // Relative to the conversion folder.
                std::string codes_file;
                // "nomercy-rev0" / "nomercy-rev1" / "" (unknown): codes only run when they match the port.
                std::string codes_rom;
                // Texture dependency package ("Team-Name"), when the pack isn't inline.
                std::string textures_package;
                // Inline pack: a folder in the conversion ("textures"), and how its files are named ("rice" / "rt64").
                std::string textures_dir;
                std::string textures_naming;
                std::string rt_lights_file;
                std::string rt_materials_file;
            };

            // [a-z0-9_.-]{1,48}, not starting with '.' or '_': ids are folder names (and '_packages' is reserved).
            bool valid_id(const std::string &id);

            // Parses aki.json. Returns false with error set when it isn't a valid v1 manifest, or when its id doesn't
            // match expected_id (the folder it was found in; empty to skip the check).
            bool parse(const std::string &json_text, const std::string &expected_id, Conversion &out, std::string &error);

            // The mods a session with this conversion enables: every mod found in its folders, plus the cheats
            // engine when it has codes that can run (codes_runnable). Every other mod is off for the session.
            std::unordered_set<std::string> session_enabled_mods(const std::vector<std::string> &conversion_mod_ids,
                bool codes_runnable, const std::string &cheats_mod_id);

            // The save subfolder (saves/<subfolder>/): a conversion (AKI_CONVERSION, env_value) wins over the ROM-patch
            // mod's id; "" = the normal saves. Returns false for an invalid conversion id: the game must not start
            // then (falling back to the normal saves would let a conversion overwrite the player's progress).
            bool resolve_save_subfolder(const char *env_value, const std::string &patch_mod_id, std::string &out);

            // Whether codes written for codes_rom may run on this port: the same game (the cheats engine translates another
            // revision's addresses itself); "" (unknown) and other games are refused.
            bool codes_runnable(const std::string &codes_rom, const std::string &port_rom);
        }

        // The conversion this session runs (AKI_CONVERSION), or nullptr. Set by scan_mods.
        const conversion::Conversion *active_conversion();
        // The active conversion's codes file text, or "" (no conversion, no codes, or codes for another revision).
        std::string active_conversion_codes();
        // The ROM revision tag codes must have to run on this port (e.g. "nomercy-rev1"); set by the game.
        void set_port_rom_revision(const std::string &tag);
        // The cheats engine's mod id (AkiMods cheats), enabled with a conversion that has codes; set by the game.
        void set_cheats_mod_id(const std::string &mod_id);
        // The active conversion's codes as a native table (librecomp/conversion_codes.hpp), parsed on the first call:
        // true when its codes file is written for this port's revision (or untagged). Those codes then run from
        // apply_active_native_codes, every frame, with no size cap; the game hands the cheats mod no text for them.
        // False: no codes, or codes for another revision, which stay with the cheats mod (it translates them).
        bool active_native_codes_ready();
        // Runs the native table once over RDRAM (rdram_size bytes from 0x80000000). No-op when there is none.
        void apply_active_native_codes(uint8_t *rdram, uint32_t rdram_size);
    }

    // The save subfolder the game uses (conversion::resolve_save_subfolder): the active conversion's id, else the
    // ROM-patch mod's id, else "" (saves/ itself). Also used for the save backups before the game starts.
    std::u8string get_save_subfolder();
}

#endif
