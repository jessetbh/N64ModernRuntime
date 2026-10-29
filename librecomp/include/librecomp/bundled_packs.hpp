#ifndef __RECOMP_BUNDLED_PACKS_HPP__
#define __RECOMP_BUNDLED_PACKS_HPP__

// [wcw] The model packs a game ships for aki_modelimport (assets/mods/models, aki-roadmap#145) and how a later bundle
// updates them (aki-roadmap#149). A bundled pack is installed into the player's mods/models once; mods/models/.bundled
// records it, so a pack the player deleted stays deleted. A newer bundle replaces an installed copy only when that copy
// is an earlier bundled version the player hasn't edited: its contents hash to one of the pack's earlier versions in
// bundled-history.json (written by AkiMods mods/modelimport/tools/pack_history.py) and its pack.json version is older.
// A pack the player made or changed is never touched. The rules are pure and unit tested
// (tests/wcw/bundled_packs_tests.cpp); install() does the file work.

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace recomp {
    namespace mods {
        namespace bundled_packs {
            constexpr const char *RecordFile = ".bundled";
            constexpr const char *HistoryFile = "bundled-history.json";

            // FNV-1a 64 over a pack's files ('/'-separated paths relative to the pack, contents), sorted by path, each
            // as path bytes, 0x00, contents, 0x00. Text files (.json, .md, .txt) count CRLF as LF, so the line endings
            // a checkout gave them don't change the hash. Must match pack_history.py.
            uint64_t pack_hash(std::vector<std::pair<std::string, std::string>> files);
            std::string hash_hex(uint64_t hash);   // 16 lower-case hex digits

            // Dotted versions compared part by part as numbers ("1.10.0" > "1.9.0"); a missing part or one that isn't a
            // number counts as 0. Negative: a < b.
            int compare_versions(const std::string &a, const std::string &b);
            // pack.json's "version" ("" when it has none or isn't JSON).
            std::string pack_version(const std::string &pack_json_text);

            enum class Action {
                Install,  // not there and never installed: copy it in
                Replace,  // an earlier bundled version, unedited: replace it with this one
                Keep,     // up to date, edited, or the player's own pack of that name: leave it
                Skip      // installed before and since deleted by the player: don't bring it back
            };
            struct Installed {
                bool recorded = false;     // named in .bundled
                bool present = false;      // a folder of that name is in mods/models
                uint64_t hash = 0;         // its pack_hash, when present
                std::string version;       // its pack.json version, when present
            };
            Action decide(const Installed &installed, uint64_t bundled_hash, const std::string &bundled_version,
                const std::vector<uint64_t> &earlier_hashes);

            // .bundled: one pack folder name per line (CR and trailing spaces ignored).
            std::set<std::string> parse_record(const std::string &text);
            std::string format_record(const std::set<std::string> &names);
            // bundled-history.json: {"previous": {"<id>": ["<16 hex>", ...]}} -> id -> earlier hashes. Bad input: empty.
            std::map<std::string, std::vector<uint64_t>> parse_history(const std::string &json_text);

            // Installs/updates the packs in src (assets/mods/models) into dst (the player's mods/models) by the rules
            // above, copies CREDITS.md when it differs, and prints what it did ("[wcw] bundled model pack ...").
            void install(const std::filesystem::path &src, const std::filesystem::path &dst);
        }
    }
}

#endif
