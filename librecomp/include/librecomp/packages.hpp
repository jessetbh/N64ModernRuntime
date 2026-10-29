#ifndef __RECOMP_PACKAGES_HPP__
#define __RECOMP_PACKAGES_HPP__

// [wcw] One package format for every kind of mod (aki-roadmap#154): a zip or folder with aki-package.json at its root
// next to the payload, which stays in the formats the game already reads (.nrm, .rtz, .akimodel folders, a conversion
// folder holding its aki.json). The registry, packages.json in the game's data folder, records what each installed
// package put down and the content hash of every file at install time, so upgrades and removals know what is theirs
// and whether the player has edited it since. The launcher and the game's drop folder share these rules and the file.
//
// This header holds the manifest and the registry: pure rules (parse, validate, registry updates; unit tested in
// tests/wcw/packages_tests.cpp) and the little file IO they need (hash an installed path, load/save the registry).
// Installing, upgrading and the drop folder build on it.

#include <cstddef>
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

        // Unknown: a registry entry of a kind this build doesn't know (written by a newer launcher or game). It is
        // kept, still owns its files, and is written back unchanged; manifests never parse to it.
        enum class Kind { Addon, TexturePack, ModelPack, Conversion, Unknown };
        const char *kind_name(Kind kind);                                   // "addon", "texture-pack", ...
        std::optional<Kind> parse_kind(const std::string &name);            // never Unknown

        // A package id: [a-z0-9][a-z0-9_.-]*, at most 48 characters, not ending in '.', and not a Windows device
        // name (con, nul, aux, com1, ...). It names the install file or folder and, for a conversion, the save
        // folder, so it must also be a valid conversion id. One namespace per game, all kinds.
        bool valid_id(const std::string &id);
        // major.minor.patch with an optional -prerelease ("1.2.0", "2.0.0-beta.1").
        bool valid_version(const std::string &version);
        // SemVer precedence. Negative: a < b. Numbers compare as numbers of any length; a release sorts after its
        // prereleases; prerelease identifiers compare numerically when both are numbers, as text otherwise, a number
        // before text, and a shorter list first when one is a prefix of the other (beta.2 < beta.10 < beta.10.1).
        int compare_versions(const std::string &a, const std::string &b);

        // The runtime's mod game ids (mod.json game_id, the port's mod_game_id) and the short keys the launcher and
        // AkiMods use ("wwfnomercy" <-> "nomercy"). Empty when unknown.
        std::string game_id_for_key(const std::string &short_key);
        std::string key_for_game_id(const std::string &game_id);

        // A path inside a package or under the data folder: relative, '/'-separated, at most one trailing '/'; no
        // empty, '.' or '..' parts; no part ending in '.' or ' ' (Windows strips them, so two names would alias);
        // no Windows device name as a part's stem (aux.rtz, nul/); no control characters or \ : * ? " < > |.
        // Non-ASCII (UTF-8) names are allowed as they are; they are not normalised.
        bool safe_relative_path(const std::string &path);
        // The form paths are stored and compared in: no trailing '/'. Comparisons (owner_of, conflict) also ignore
        // ASCII case, since the data folder is on a case-insensitive file system on Windows.
        std::string normal_path(const std::string &path);

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
            std::string thumb;                      // as given; "" means thumb.png, if the package has one
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

            // The payload, by kind; paths relative to the package root, distinct and not nested in each other.
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
            std::string path;       // relative to the data folder, normal_path ("mods/aki_camera.nrm", "conversions/nmp_v3")
            uint64_t hash = 0;      // content hash of the INSTALLED path at install time (hash_path; a file's hash
                                    // includes its file name, so hash what was installed, not the source)
        };
        struct Entry {
            std::string id;
            Kind kind = Kind::Addon;
            std::string kind_text;  // the kind as written (kept for Unknown)
            std::string version;
            std::string display_name;
            std::string source;     // launcher / dropped / bundled / migrated
            std::string installed;  // UTC, ISO 8601
            std::string parent;     // the conversion a tied add-on came with ("" = none)
            std::vector<InstalledFile> files;
            std::string extra;      // fields this build doesn't know, as a JSON object; written back unchanged
        };
        struct Registry {
            std::vector<Entry> packages;            // kept sorted by id
        };

        enum class RegistryStatus {
            Ok,         // read as written (or absent: an empty registry)
            Damaged,    // readable, but some entries were dropped (bad id, path, hash, or a path two entries claim)
            Corrupt,    // not a registry at all (not JSON, no packages list): treated as empty
            Newer,      // a newer registry format: treated as empty for reading
            IoError     // couldn't be read
        };
        // Whether a registry read with that status may be written back. Never over a Newer, Corrupt or unreadable
        // file: that would lose what a newer launcher or game recorded. (A Corrupt file needs the player's say.)
        bool may_save(RegistryStatus status);

        RegistryStatus parse_registry(const std::string &json_text, Registry &out);
        std::string format_registry(const Registry &registry);   // stable: sorted by id, files in install order

        const Entry *find(const Registry &registry, const std::string &id);
        void upsert(Registry &registry, Entry entry);            // replaces the entry with that id, or adds it
        std::optional<Entry> remove(Registry &registry, const std::string &id);
        // The id of the package that installed that path (or a folder containing it), or "" for a file no package
        // claims (a manual install).
        std::string owner_of(const Registry &registry, const std::string &path);
        // Why a package can't take this id and these paths: "" when it can. An id held by another kind, or a path
        // another package installed (or a folder holding one), is a conflict: one namespace per game, one owner per
        // file. The packages it replaces (manifest "replaces") don't count; they're removed first.
        std::string conflict(const Registry &registry, const std::string &id, Kind kind, const std::vector<std::string> &paths,
            const std::vector<std::string> &replacing = {});
        // Whether an installed file changed since install: its current hash (nullopt = gone) against the record.
        enum class FileState { Unchanged, Edited, Missing };
        FileState file_state(const InstalledFile &file, std::optional<uint64_t> current_hash);

        // Hashing (the bundled packs' pack_hash: FNV-1a 64 over '/'-separated relative paths and contents, sorted by
        // path, text files CRLF-blind; a single file hashes as one entry named by its file name).
        // The hash of one file's contents held in memory, under that file name.
        uint64_t hash_bytes(const std::string &file_name, const std::string &data);
        // The hash of a file or folder on disk, streamed in chunk_size pieces (it never holds a whole file in
        // memory). A folder hashes its regular files; empty subfolders don't count, and symlinked folders aren't
        // followed (a symlinked file counts with its target's contents). nullopt when the path doesn't exist or
        // something in it can't be read.
        std::optional<uint64_t> hash_path(const std::filesystem::path &path, size_t chunk_size = size_t(1) << 20);

        // <data>/packages.json; an absent file is an empty registry (Ok).
        RegistryStatus load_registry(const std::filesystem::path &data_dir, Registry &out);
        // Writes <data>/packages.json through a temporary file (flushed to disk) and a rename, retried briefly while
        // another program holds the file open, so a crash never leaves half a file. Check may_save first.
        bool save_registry(const std::filesystem::path &data_dir, const Registry &registry);
    }
}

#endif
