#ifndef __RECOMP_PACKAGES_HPP__
#define __RECOMP_PACKAGES_HPP__

// [wcw] One package format for every kind of mod (aki-roadmap#154): a zip or folder with aki-package.json at its root
// next to the payload, which stays in the formats the game already reads (.nrm, .rtz, .akimodel folders, a conversion
// folder holding its aki.json). The registry, packages.json in the game's data folder, records what each installed
// package put down and the content hash of every file at install time, so upgrades and removals know what is theirs
// and whether the player has edited it since. The launcher and the game's drop folder share these rules.
//
// This header holds the manifest and the registry: pure rules (parse, validate, registry updates; unit tested in
// tests/wcw/packages_tests.cpp) and the little file IO they need (hash an installed path, load/save the registry).
// Installing, upgrading and the drop folder build on it.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace recomp {
    namespace packages {
        constexpr const char *ManifestFile = "aki-package.json";
        constexpr const char *RegistryFile = "packages.json";
        constexpr int ManifestFormat = 1;
        constexpr int RegistryFormat = 1;

        enum class Kind { Addon, TexturePack, ModelPack, Conversion };
        const char *kind_name(Kind kind);                                   // "addon", "texture-pack", ...
        std::optional<Kind> parse_kind(const std::string &name);

        // A package id: [a-z0-9][a-z0-9_.-]*, at most 48 characters (it names the install file or folder and, for a
        // conversion, the save folder, so it must also be a valid conversion id). One namespace per game, all kinds.
        bool valid_id(const std::string &id);
        // major.minor.patch with an optional -prerelease ("1.2.0", "2.0.0-beta.1").
        bool valid_version(const std::string &version);
        // Negative: a < b. Numbers part by part, then a release sorts after its prereleases, which compare as text.
        int compare_versions(const std::string &a, const std::string &b);

        // The runtime's mod game ids (mod.json game_id, the port's mod_game_id) and the short keys the launcher and
        // AkiMods use ("wwfnomercy" <-> "nomercy"). Empty when unknown.
        std::string game_id_for_key(const std::string &short_key);
        std::string key_for_game_id(const std::string &game_id);

        // A path inside a package or under the data folder: relative, '/'-separated, no empty, '.' or '..' parts, no
        // drive or backslash. The installer refuses a package that names any other kind of path.
        bool safe_relative_path(const std::string &path);

        struct Problem {
            bool error = true;      // false: a warning (the package still installs)
            std::string field;      // "id", "payload.rtz", ...
            std::string message;
        };

        struct Manifest {
            int format = 0;
            Kind kind = Kind::Addon;
            std::string id;
            std::string version;
            std::string game_id;
            std::string display_name;
            std::vector<std::string> authors;
            std::string short_description;
            std::string description;
            std::string thumb;                      // default "thumb.png" (may be absent)
            std::string license;
            std::string source;
            struct Permission {
                std::string redistribute, modify;   // yes / no / ask / unknown ("" = not given)
                std::string statement, url;
            } permission;
            std::vector<std::string> base_rom_sha1; // lower-case hex; required for conversions
            std::string base_rom_name;
            std::string requires_launcher;
            std::string requires_recomp;            // requires.minimum_recomp_version
            std::vector<std::string> requires_packages;
            std::vector<std::string> replaces;

            // The payload, by kind; paths relative to the package root.
            std::string nrm;                        // addon
            std::string rtz;                        // texture-pack: an .rtz ...
            std::string folder;                     // ... or a loose pack folder; conversion: the conversion folder
            std::vector<std::string> packs;         // model-pack: <id>.akimodel folders
            std::vector<std::string> hd;            // model-pack: their -hd.rtz texture packs
            std::vector<std::string> addons;        // conversion: texture packs installed with it and tied to it
        };

        // Parses and checks aki-package.json. Returns false when there is any error (then the package can't be
        // installed); warnings alone return true. Every problem found is appended, not only the first.
        bool parse_manifest(const std::string &json_text, Manifest &out, std::vector<Problem> &problems);
        // Every payload path of a parsed manifest, in manifest order.
        std::vector<std::string> payload_paths(const Manifest &manifest);

        struct InstalledFile {
            std::string path;       // relative to the data folder, '/'-separated ("mods/aki_camera.nrm", "conversions/nmp_v3")
            uint64_t hash = 0;      // content hash at install time (hash_path)
        };
        struct Entry {
            std::string id;
            Kind kind = Kind::Addon;
            std::string version;
            std::string display_name;
            std::string source;     // launcher / dropped / bundled / migrated
            std::string installed;  // UTC, ISO 8601
            std::string parent;     // the conversion a tied add-on came with ("" = none)
            std::vector<InstalledFile> files;
        };
        struct Registry {
            std::vector<Entry> packages;            // kept sorted by id
        };

        // packages.json -> Registry. A missing, unreadable or newer-format file gives an empty registry and false.
        // Entries with a bad id, kind or path are dropped (and make it return false), the rest are kept.
        bool parse_registry(const std::string &json_text, Registry &out);
        std::string format_registry(const Registry &registry);   // stable: sorted by id, files in install order

        const Entry *find(const Registry &registry, const std::string &id);
        void upsert(Registry &registry, Entry entry);            // replaces the entry with that id, or adds it
        std::optional<Entry> remove(Registry &registry, const std::string &id);
        // The id of the package that installed that path (or a folder containing it), or "" for a file no package
        // claims (a manual install).
        std::string owner_of(const Registry &registry, const std::string &path);
        // Why a package can't take this id and these paths: "" when it can. An id held by another kind, or a path
        // another package installed, is a conflict (one namespace per game; one owner per file).
        std::string conflict(const Registry &registry, const std::string &id, Kind kind, const std::vector<std::string> &paths);
        // Whether an installed file changed since install: its current hash (nullopt = gone) against the record.
        enum class FileState { Unchanged, Edited, Missing };
        FileState file_state(const InstalledFile &file, std::optional<uint64_t> current_hash);

        // File IO.
        // The content hash of a file or folder (the bundled packs' pack_hash: FNV-1a 64 over '/'-separated relative
        // paths and contents, text files CRLF-blind; a file hashes as one entry named by its file name). nullopt
        // when the path doesn't exist or can't be read.
        std::optional<uint64_t> hash_path(const std::filesystem::path &path);
        // <data>/packages.json; an absent file is an empty registry (true).
        bool load_registry(const std::filesystem::path &data_dir, Registry &out);
        // Writes <data>/packages.json through a temporary file and a rename, so a crash never leaves half a file.
        bool save_registry(const std::filesystem::path &data_dir, const Registry &registry);
    }
}

#endif
