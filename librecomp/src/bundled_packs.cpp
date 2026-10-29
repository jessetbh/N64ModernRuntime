// [wcw] Installs the bundled model packs (see bundled_packs.hpp): the file work around the pure rules.

#include "librecomp/bundled_packs.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

namespace recomp::mods::bundled_packs {
    namespace fs = std::filesystem;

    static std::string read_file(const fs::path &path) {
        std::ifstream f(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    // A pack folder's files as (relative '/' path, contents). A walk error ends the walk (the hash then won't match).
    static std::vector<std::pair<std::string, std::string>> read_pack(const fs::path &folder) {
        std::vector<std::pair<std::string, std::string>> files;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code file_ec;
            if (!it->is_regular_file(file_ec)) {
                continue;
            }
            const std::u8string rel = it->path().lexically_relative(folder).generic_u8string();
            files.emplace_back(std::string(rel.begin(), rel.end()), read_file(it->path()));
        }
        return files;
    }

    // Copies src into dst via a temporary folder; replacing: the old copy moves aside first and comes back on failure.
    static bool copy_pack(const fs::path &src, const fs::path &dst, bool replace, std::error_code &ec) {
        fs::path temp = dst;
        temp += ".installing";
        fs::path old = dst;
        old += ".replaced";
        std::error_code cleanup;
        fs::remove_all(temp, cleanup);
        fs::copy(src, temp, fs::copy_options::recursive, ec);
        if (ec) {
            fs::remove_all(temp, cleanup);
            return false;
        }
        if (replace) {
            fs::remove_all(old, cleanup);
            fs::rename(dst, old, ec);
            if (ec) {
                fs::remove_all(temp, cleanup);
                return false;
            }
        }
        fs::rename(temp, dst, ec);
        if (ec) {
            fs::remove_all(temp, cleanup);
            if (replace) {
                fs::rename(old, dst, cleanup);   // the player keeps the copy they had
            }
            return false;
        }
        if (replace) {
            fs::remove_all(old, cleanup);
        }
        return true;
    }

    void install(const fs::path &src, const fs::path &dst) {
        std::error_code ec;
        if (!fs::is_directory(src, ec)) {
            return;
        }
        fs::create_directories(dst, ec);
        const fs::path record_path = dst / RecordFile;
        std::set<std::string> record = parse_record(read_file(record_path));
        const std::map<std::string, std::vector<uint64_t>> history = parse_history(read_file(src / HistoryFile));
        bool record_changed = false;

        std::error_code walk_ec;
        for (fs::directory_iterator it(src, walk_ec), end; !walk_ec && it != end; it.increment(walk_ec)) {
            const fs::directory_entry &entry = *it;
            const std::string name = entry.path().filename().string();
            const fs::path target = dst / entry.path().filename();
            std::error_code entry_ec;
            if (entry.is_regular_file(entry_ec)) {
                if (name == "CREDITS.md" && (!fs::exists(target, entry_ec) || read_file(target) != read_file(entry.path()))) {
                    fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, entry_ec);
                }
                continue;
            }
            if (!entry.is_directory(entry_ec) || entry.path().extension() != ".akimodel") {
                continue;
            }

            // A replace cut short between its two renames leaves the copy under .replaced: put it back first, or the
            // pack would count as deleted by the player.
            fs::path replaced = target;
            replaced += ".replaced";
            if (!fs::exists(target, entry_ec) && fs::is_directory(replaced, entry_ec)) {
                fs::rename(replaced, target, entry_ec);
                fprintf(stderr, "[wcw] bundled model pack %s: restored from an interrupted update%s\n", name.c_str(),
                    entry_ec ? " (failed)" : "");
            }

            const uint64_t bundled_hash = pack_hash(read_pack(entry.path()));
            const std::string bundled_version = pack_version(read_file(entry.path() / "pack.json"));
            Installed installed;
            installed.recorded = record.count(name) != 0;
            installed.present = fs::exists(target, entry_ec);
            if (installed.present) {
                installed.hash = pack_hash(read_pack(target));
                installed.version = pack_version(read_file(target / "pack.json"));
            }
            const std::string id = name.substr(0, name.size() - std::string(".akimodel").size());
            const auto hist = history.find(id);
            const Action action = decide(installed, bundled_hash, bundled_version,
                (hist != history.end()) ? hist->second : std::vector<uint64_t>{});

            if (action == Action::Install || action == Action::Replace) {
                std::error_code copy_ec;
                const bool ok = copy_pack(entry.path(), target, action == Action::Replace, copy_ec);
                fprintf(stderr, "[wcw] bundled model pack %s: %s%s\n", name.c_str(),
                    ok ? (action == Action::Replace ? "updated to " : "installed ") : "",
                    ok ? (bundled_version.empty() ? "" : bundled_version.c_str()) : copy_ec.message().c_str());
                if (ok && !installed.recorded) {
                    record.insert(name);
                    record_changed = true;
                }
            }
            else if (action == Action::Keep && !installed.recorded) {
                record.insert(name);   // the player's own pack of that name, or one already up to date: never touched
                record_changed = true;
            }
        }

        if (record_changed) {
            std::ofstream out(record_path, std::ios::binary | std::ios::trunc);
            out << format_record(record);
        }
    }
}
