#ifndef __RECOMP_CONVERSION_HPP__
#define __RECOMP_CONVERSION_HPP__

// [wcw fullconv] Conversions: one package per full-conversion mod (a ROM patch, GameShark codes, saves, a texture pack
// and ray-tracing data, in any mix), installed in <config>/conversions/<id>/ with an aki.json manifest
// (plans/fullconv-manifest.md). AKI_CONVERSION=<id> (set by AKI Launcher) runs the game with exactly that conversion:
// its mods on, every other mod off for the session, its saves in saves/<id>/. The parsing and the enable rule are pure
// and unit tested (tests/wcw/conversion_tests.cpp).

#include <cstdint>
#include <filesystem>
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
            // The conversion picked in the game's Mods menu, for the next starts (aki-roadmap#57): one id in
            // <config>/conversions/active.txt. AKI_CONVERSION (the launcher) wins over it.
            constexpr const char *ChoiceFile = "active.txt";
            // The player's own add-on choices for one conversion (aki-roadmap#154): conversions/<id>/addons.json.
            constexpr const char *AddonsFile = "addons.json";

            // Which of the player's add-ons a conversion lets run beside it (aki.json "addons", aki-roadmap#154).
            enum class AddonsAllow {
                None,   // only the conversion's own mods
                Data,   // data packs the player has enabled (texture packs); code add-ons only when listed (default)
                All     // everything the player has enabled
            };

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
                // aki.json "models": {"dir": "models"} (aki-roadmap#226): a folder in the conversion holding model packs
                // (<pack>.akimodel/ folders) and the conversion's own models.txt. While the conversion runs, the game's
                // model host reads that folder instead of <mods>/models, and the session turns on the model import mod
                // (ModelsModId). An optional v1 component: older builds ignore it and run the conversion without models.
                bool has_models = false;
                std::string models_dir;
                // Not from aki.json: set at the mod scan (models_refusal of the patch's relink.json). Non-empty: the
                // conversion's models can't load in this session, and why.
                std::string models_refusal;
                // aki.json "addons": {"allow": "none" | "data" | "all", "mods": ["aki_camera", ...]}. Listed code add-ons
                // run under "data" too (the conversion's allow-list).
                AddonsAllow addons_allow = AddonsAllow::Data;
                std::vector<std::string> addons_mods;
            };

            // The model import mod (AkiMods aki_modelimport), on for a session whose conversion has usable models.
            constexpr const char *ModelsModId = "aki_modelimport";
            // Model packs load into the N64's Expansion RAM window, physical 0x00400000-0x00800000 (AkiMods
            // mods/modelimport/mod.psd1: MI_MEM_BASE .. MI_SCRATCH + 256 KB). A relinked hack whose relink.json puts
            // code, data, preloads or zeroed ranges there owns that memory (Born to be Wired, No Mercy Plus, Legends).
            constexpr uint32_t ModelsWindowStart = 0x00400000;
            constexpr uint32_t ModelsWindowEnd = 0x00800000;
            // Why a conversion's models can't load beside its patch, "" when they can (aki-roadmap#226). relink_json:
            // the text of the patch's relink.json, "" when it has none (a plain patch: always fine). Refused: any
            // code_ranges / data_ranges / preload / zero range overlapping the models window, or a relink.json that
            // doesn't parse. Moving the packs above 8 MB (aki-roadmap#156) lifts this.
            std::string models_refusal(const std::string &relink_json);

            // [a-z0-9_.-]{1,48}, not starting with '.' or '_': ids are folder names (and '_packages' is reserved).
            bool valid_id(const std::string &id);

            // Parses aki.json. Returns false with error set when it isn't a valid v1 manifest, or when its id doesn't
            // match expected_id (the folder it was found in; empty to skip the check).
            bool parse(const std::string &json_text, const std::string &expected_id, Conversion &out, std::string &error);

            // The mods a session with this conversion enables: every mod found in its folders, plus the cheats
            // engine when it has codes that can run (codes_runnable). Every other mod is off for the session.
            std::unordered_set<std::string> session_enabled_mods(const std::vector<std::string> &conversion_mod_ids,
                bool codes_runnable, const std::string &cheats_mod_id);

            // A mod from the player's mods folder, as a session sees it.
            struct PlayerMod {
                std::string id;
                bool code = false;              // a code add-on; false (and not a ROM patch): a data pack (texture pack)
                bool rom_patch = false;         // another ROM hack's patch: never beside the conversion's own
                bool enabled = false;           // enabled in the player's mod configuration
            };
            // conversions/<id>/addons.json: {"enabled": [ids], "disabled": [ids]}. The player's word wins over the
            // conversion's: enabled runs even a code add-on the conversion doesn't list, disabled turns one off (an id
            // in both lists is off). It can't turn off the conversion's own mods or turn on another ROM hack.
            struct PlayerAddons {
                std::vector<std::string> enabled, disabled;
            };
            PlayerAddons parse_player_addons(const std::string &json_text);   // bad input: none
            // The session rule with add-ons (aki-roadmap#154 step 4): the conversion's own mods and the cheats engine
            // (as above), plus each of the player's mods that the player overrides on, or that the player has
            // enabled and the conversion allows: data packs unless addons.allow is none, code add-ons when
            // addons.allow is all or the conversion lists them. Never: another ROM hack's patch (two patches can't
            // load together), and the cheats engine without runnable conversion codes unless the player turned it
            // on here (the player's stock-game codes would poke a hack's memory). why (optional) gets one line per
            // player mod. A mod enabled here can still pull in its dependencies when it loads (dependency auto-enable).
            std::unordered_set<std::string> session_enabled_mods(const std::vector<std::string> &conversion_mod_ids,
                bool codes_runnable, const std::string &cheats_mod_id, const std::vector<PlayerMod> &player_mods,
                const Conversion &conversion, const PlayerAddons &player, std::vector<std::string> *why = nullptr);

            // The save subfolder (saves/<subfolder>/): a conversion (AKI_CONVERSION, env_value) wins over the ROM-patch
            // mod's id; "" = the normal saves. Returns false for an invalid conversion id: the game must not start
            // then (falling back to the normal saves would let a conversion overwrite the player's progress).
            bool resolve_save_subfolder(const char *env_value, const std::string &patch_mod_id, std::string &out);

            // Which conversion a session starts with (aki-roadmap#57). The launcher's AKI_CONVERSION (env_value) wins and is
            // strict: an id that can't run refuses to start, as before. Otherwise the Mods-menu choice (choice_file_text, the
            // text of conversions/active.txt; its first line, trimmed) is lenient: if it can't run the game starts
            // normally instead (see lenient). Both empty: no conversion.
            struct StartupChoice {
                std::string id;
                bool from_launcher = false;
                bool lenient() const { return !from_launcher && !id.empty(); }
            };
            StartupChoice choose_startup(const char *env_value, const std::string &choice_file_text);

            // Why a lenient (Mods-menu) choice can't run, "" when it can (out is then its parsed aki.json). manifest_text:
            // the text of <config>/conversions/<id>/aki.json, "" when it isn't installed.
            std::string check_choice(const std::string &id, const std::string &manifest_text, Conversion &out);

            // Whether codes written for codes_rom may run on this port: the same game (the cheats engine translates another
            // revision's addresses itself); "" (unknown) and other games are refused.
            bool codes_runnable(const std::string &codes_rom, const std::string &port_rom);
        }

        // The conversion this session runs (AKI_CONVERSION or the Mods-menu choice), or nullptr. Set by scan_mods.
        const conversion::Conversion *active_conversion();
        // Its id ("" = none), decided once per session (conversion::choose_startup); the save folder follows it.
        std::string session_conversion_id();
        // Whether AKI Launcher picked it (AKI_CONVERSION): a restart from the launcher passes the same id again, so a
        // Mods-menu choice only applies to starts without it.
        bool session_conversion_from_launcher();

        // The Mods menu's conversion picker (aki-roadmap#57). An installed conversion: its parsed aki.json, or error set
        // (id and name are then the folder name).
        struct InstalledConversion {
            conversion::Conversion conversion;
            std::filesystem::path folder;
            std::string error;
        };
        // The conversions installed in <config>/conversions/, by name.
        std::vector<InstalledConversion> list_installed_conversions();
        // The id in conversions/active.txt ("" = none): the conversion the next start runs when the launcher sets none.
        std::string chosen_conversion();
        // Writes (or, for "", removes) conversions/active.txt. False for an invalid id or a failed write.
        bool set_chosen_conversion(const std::string &id);
        // Why this session's Mods-menu choice couldn't run and was cleared ("" = it ran, or there was none).
        std::string conversion_choice_notice();
        // The active conversion's codes file text, or "" (no conversion, no codes, or codes for another revision).
        std::string active_conversion_codes();
        // aki-roadmap#226: records why the active conversion's models can't load (conversion::models_refusal), "" when
        // they can. Called at the mod scan, before the session's mods are decided. No-op without a conversion.
        void set_active_conversion_models_refusal(const std::string &reason);
        // The folder the game's model host reads while the active conversion runs: <conversion>/<models dir>. Empty
        // when there is no conversion, it has no models component, or its models are refused (then the host must
        // read nothing at all, not the player's own packs, which would land in the same memory).
        std::filesystem::path active_conversion_models_dir();
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
