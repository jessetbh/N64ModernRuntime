// [wcw] Migration to the package registry (aki-roadmap#154 step 3): the first start with packages.json records what
// is installed and turns patch-mod ROM hacks into conversions (see packages.hpp).

#include "librecomp/packages.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <exception>
#include <set>
#include <system_error>

#include "librecomp/bundled_packs.hpp"
#include "json/json.hpp"
#include "miniz.h"
#include "miniz_zip.h"

namespace recomp::packages {
    namespace fs = std::filesystem;
    using json = nlohmann::json;

    namespace {
        fs::path u8path(const std::string &s) {
            return fs::path(std::u8string(s.begin(), s.end()));
        }

        std::string u8str(const fs::path &p) {
            const std::u8string s = p.generic_u8string();
            return std::string(s.begin(), s.end());
        }

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return s;
        }

        std::string now_utc() {
            const std::time_t t = std::time(nullptr);
            std::tm tm{};
#ifdef _WIN32
            gmtime_s(&tm, &t);
#else
            gmtime_r(&t, &tm);
#endif
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
            return buf;
        }

        FILE *open_file(const fs::path &path, const wchar_t *wmode, const char *mode) {
            FILE *f = nullptr;
#ifdef _WIN32
            (void)mode;
            if (_wfopen_s(&f, path.c_str(), wmode) != 0) {
                f = nullptr;
            }
#else
            (void)wmode;
            f = std::fopen(path.c_str(), mode);
#endif
            return f;
        }

        std::string read_text(const fs::path &path) {
            FILE *f = open_file(path, L"rb", "rb");
            if (!f) {
                return {};
            }
            std::string out;
            char buf[65536];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
                out.append(buf, n);
            }
            std::fclose(f);
            return out;
        }

        bool write_text(const fs::path &path, const std::string &text) {
            FILE *f = open_file(path, L"wb", "wb");
            if (!f) {
                return false;
            }
            const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
            return (std::fclose(f) == 0) && ok;
        }

        // What an .nrm / .rtz zip holds: its mod.json (text, "" if none) and whether it has patch.bps / a code binary.
        struct ZipInfo {
            bool ok = false;
            std::string mod_json;
            bool patch = false;
            bool binary = false;
        };
        ZipInfo inspect_zip(const fs::path &path) {
            ZipInfo info;
            FILE *f = open_file(path, L"rb", "rb");
            if (!f) {
                return info;
            }
            mz_zip_archive a{};
            if (mz_zip_reader_init_cfile(&a, f, 0, 0)) {
                info.ok = true;
                mz_uint index;
                // Case-sensitive, as the runtime's mod loader looks them up (mod_manifest.cpp).
                info.patch = mz_zip_reader_locate_file_v2(&a, "patch.bps", nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE, &index);
                info.binary = mz_zip_reader_locate_file_v2(&a, "mod_binary.bin", nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE, &index);
                if (mz_zip_reader_locate_file_v2(&a, "mod.json", nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE, &index)) {
                    mz_zip_archive_file_stat st;
                    if (mz_zip_reader_file_stat(&a, index, &st) && st.m_uncomp_size < (1u << 20)) {
                        std::string text(size_t(st.m_uncomp_size), '\0');
                        if (mz_zip_reader_extract_to_mem(&a, index, text.data(), text.size(), 0)) {
                            info.mod_json = std::move(text);
                        }
                    }
                }
                mz_zip_reader_end(&a);
            }
            std::fclose(f);
            return info;
        }

        // A string field, or fallback when it's missing or not a string (json::value throws on a wrong type).
        std::string str(const json &j, const char *key, const std::string &fallback = std::string()) {
            if (!j.is_object()) {
                return fallback;
            }
            const auto it = j.find(key);
            return (it != j.end() && it->is_string()) ? it->get<std::string>() : fallback;
        }

        struct ModFields {
            std::string id, name, version;
            std::vector<std::string> authors;
        };
        ModFields mod_fields(const std::string &mod_json) {
            ModFields m;
            const json j = json::parse(mod_json, nullptr, false);
            if (j.is_discarded() || !j.is_object()) {
                return m;
            }
            m.id = str(j, "id");
            m.name = str(j, "display_name", m.id);
            m.version = str(j, "version");
            if (const auto a = j.find("authors"); a != j.end() && a->is_array()) {
                for (const json &v : *a) {
                    if (v.is_string()) {
                        m.authors.push_back(v.get<std::string>());
                    }
                }
            }
            return m;
        }

        std::string version_or_zero(const std::string &v) {
            return valid_version(v) ? v : std::string("0.0.0");
        }

        // Names migration never records: work folders, kept copies, staging leftovers, hidden or shared folders.
        bool internal_name(const std::string &name) {
            return name.empty() || name[0] == '.' || name[0] == '_' || name == KeptFolder || name == "models" ||
                name.find(".edited-") != std::string::npos || name.ends_with(".installing") || name.ends_with(".replaced") ||
                name.ends_with(".tmp");
        }

        // mods.json: the enabled mod ids in mod order (enabled ones not in the order list go last, sorted).
        std::vector<std::string> enabled_in_order(const fs::path &data_dir) {
            std::vector<std::string> out;
            const json j = json::parse(read_text(data_dir / "mods.json"), nullptr, false);
            if (j.is_discarded() || !j.is_object()) {
                return out;
            }
            std::set<std::string> enabled;
            if (const auto e = j.find("enabled_mods"); e != j.end() && e->is_array()) {
                for (const json &v : *e) {
                    if (v.is_string()) {
                        enabled.insert(v.get<std::string>());
                    }
                }
            }
            if (const auto o = j.find("mod_order"); o != j.end() && o->is_array()) {
                for (const json &v : *o) {
                    if (v.is_string() && enabled.erase(v.get<std::string>())) {
                        out.push_back(v.get<std::string>());
                    }
                }
            }
            out.insert(out.end(), enabled.begin(), enabled.end());
            return out;
        }

        MigrateReport migrate_impl(const fs::path &data_dir, const MigrateOptions &options) {
            MigrateReport report;
            auto log = [&](const std::string &line) { report.lines.push_back(line); };
            std::error_code ec;
            if (migration_done(data_dir)) {
                return report;
            }
            Registry registry;
            const RegistryStatus status = load_registry(data_dir, registry);
            if (!may_save(status)) {
                log("packages.json can't be written safely (newer or unreadable); migration skipped");
                return report;
            }
            report.ran = true;
            const std::string now = options.now.empty() ? now_utc() : options.now;
            const std::string game_key = key_for_game_id(options.game_id);
            const fs::path mods = data_dir / "mods";
            const fs::path conversions = data_dir / "conversions";

            // 1. Patch-mod hacks become conversions.
            std::vector<std::string> moved_enabled_order;
            const std::vector<std::string> enabled = enabled_in_order(data_dir);
            std::vector<fs::path> nrms;
            for (fs::directory_iterator it(mods, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code e;
                if (it->is_regular_file(e) && lower(u8str(it->path().extension())) == ".nrm" && !internal_name(u8str(it->path().filename()))) {
                    nrms.push_back(it->path());
                }
            }
            ec.clear();
            std::sort(nrms.begin(), nrms.end());
            for (const fs::path &nrm : nrms) try {
                const ZipInfo z = inspect_zip(nrm);
                if (!z.ok || !z.patch || z.binary) {
                    continue;
                }
                const std::string file = u8str(nrm.filename());
                const ModFields f = mod_fields(z.mod_json);
                if (!valid_id(f.id)) {
                    log("left " + file + " in mods: its mod id \"" + f.id + "\" can't name a conversion");
                    continue;
                }
                const fs::path target = conversions / u8path(f.id);
                if (fs::exists(target, ec) || find(registry, f.id)) {
                    log("left " + file + " in mods: conversions/" + f.id + " or the id \"" + f.id + "\" is already taken");
                    continue;
                }
                json aki = {
                    { "format", "aki-conversion" }, { "format_version", 1 }, { "id", f.id },
                    { "name", f.name.empty() ? f.id : f.name }, { "version", version_or_zero(f.version) },
                    { "authors", f.authors.empty() ? std::vector<std::string>{ "unknown" } : f.authors },
                    { "game", game_key },
                    { "description", "Moved from mods/" + file + " when ROM hacks became conversions (one at a time, own saves)." },
                    { "components", { { "patch", { { "file", "patch.nrm" }, { "mod_id", f.id } } } } },
                    { "source", { { "mode", "migrated" } } },
                };
                if (!options.rom_sha1.empty()) {
                    std::string upper = options.rom_sha1;
                    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return char(std::toupper(c)); });
                    aki["base_rom"] = { { "sha1", upper } };
                }
                log("moved mods/" + file + " -> conversions/" + f.id + "/patch.nrm (a ROM hack is a conversion now; saves stay in saves/" + f.id + ")");
                report.moved.push_back(f.id);
                if (std::find(enabled.begin(), enabled.end(), f.id) != enabled.end()) {
                    moved_enabled_order.push_back(f.id);
                }
                if (options.dry_run) {
                    continue;
                }
                fs::create_directories(target, ec);
                ec.clear();
                const bool written = write_text(target / "aki.json", aki.dump(2, ' ', false, json::error_handler_t::replace) + "\n");
                if (written) {
                    fs::rename(nrm, target / "patch.nrm", ec);
                }
                if (!written || ec) {
                    log("  couldn't move it (" + (written ? ec.message() : std::string("can't write aki.json")) + "); left in mods");
                    std::error_code e;
                    fs::remove_all(target, e);
                    report.moved.pop_back();
                    std::erase(moved_enabled_order, f.id);
                    continue;
                }
            }
            catch (const std::exception &e) {
                log("skipped " + u8str(nrm.filename()) + ": " + e.what());
            }
            // The order the hacks had in mods.json decides which one runs next; enabled ones keep running.
            std::sort(moved_enabled_order.begin(), moved_enabled_order.end(), [&](const std::string &a, const std::string &b) {
                return std::find(enabled.begin(), enabled.end(), a) < std::find(enabled.begin(), enabled.end(), b);
            });
            if (!moved_enabled_order.empty()) {
                const fs::path choice = conversions / "active.txt";
                if (fs::exists(choice, ec)) {
                    log("a conversion is already chosen; the enabled hack(s) are installed as conversions to pick in the Mods menu");
                    report.also_enabled = moved_enabled_order;
                }
                else {
                    report.chosen = moved_enabled_order.front();
                    report.also_enabled.assign(moved_enabled_order.begin() + 1, moved_enabled_order.end());
                    log("the next start plays " + report.chosen + " (it was enabled); while a conversion plays, your other "
                        "add-ons stay off for now");
                    if (!report.also_enabled.empty()) {
                        std::string rest;
                        for (const std::string &id : report.also_enabled) {
                            rest += " " + id;
                        }
                        log("also enabled before (only one hack runs at a time now; pick it in the Mods menu):" + rest);
                    }
                    if (!options.dry_run) {
                        fs::create_directories(conversions, ec);
                        write_text(choice, report.chosen + "\n");
                    }
                }
            }

            // 2. Record what is installed.
            const std::set<std::string> bundled_names(options.bundled_names.begin(), options.bundled_names.end());
            const std::set<std::string> bundled_packs = recomp::mods::bundled_packs::parse_record(
                read_text(mods / "models" / recomp::mods::bundled_packs::RecordFile));
            auto record = [&](const std::string &id, Kind kind, const std::string &version, const std::string &name,
                const std::string &rel, const std::string &source) { try {
                if (!valid_id(id)) {
                    log("not recorded (stays a manual install): " + rel + " (id \"" + id + "\" isn't a package id)");
                    return;
                }
                if (const std::string why = conflict(registry, id, kind, { rel }); !why.empty()) {
                    log("not recorded (stays a manual install): " + rel + " (" + why + ")");
                    return;
                }
                if (find(registry, id)) {
                    log("not recorded (stays a manual install): " + rel + " (the id \"" + id + "\" is already recorded)");
                    return;
                }
                std::optional<uint64_t> h = hash_path(data_dir / u8path(rel));
                if (!h && options.dry_run && kind == Kind::Conversion &&
                    std::find(report.moved.begin(), report.moved.end(), id) != report.moved.end()) {
                    h = 0;   // a hack that would have moved here
                }
                if (!h) {
                    log("not recorded: " + rel + " (can't be read)");
                    return;
                }
                Entry e;
                e.id = id;
                e.kind = kind;
                e.version = version_or_zero(version);
                e.display_name = name.empty() ? id : name;
                e.source = source;
                e.installed = now;
                e.files.push_back({ rel, *h });
                upsert(registry, e);
                report.recorded.push_back(id);
                log("recorded " + std::string(kind_name(kind)) + " " + id + " " + e.version + " (" + rel + ")");
            }
            catch (const std::exception &e) {
                log("not recorded: " + rel + " (" + e.what() + ")");
            } };
            auto owned = [&](const std::string &rel) { return !owner_of(registry, rel).empty(); };

            // Conversions (the moved hacks included).
            std::vector<fs::path> conv_dirs;
            for (fs::directory_iterator it(conversions, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code e;
                if (it->is_directory(e) && !internal_name(u8str(it->path().filename()))) {
                    conv_dirs.push_back(it->path());
                }
            }
            ec.clear();
            for (const std::string &id : report.moved) {
                if (options.dry_run) {
                    conv_dirs.push_back(conversions / u8path(id));
                }
            }
            std::sort(conv_dirs.begin(), conv_dirs.end());
            conv_dirs.erase(std::unique(conv_dirs.begin(), conv_dirs.end()), conv_dirs.end());
            for (const fs::path &dir : conv_dirs) {
                const std::string id = u8str(dir.filename());
                const std::string rel = "conversions/" + id;
                if (owned(rel)) {
                    continue;
                }
                const json aki = json::parse(read_text(dir / "aki.json"), nullptr, false);
                const bool moved = std::find(report.moved.begin(), report.moved.end(), id) != report.moved.end();
                if (!moved && str(aki, "format") != "aki-conversion") {
                    continue;   // not a conversion folder
                }
                const std::string version = str(aki, "version");
                const std::string name = str(aki, "name", id);
                record(id, Kind::Conversion, version, name, rel, "migrated");
            }

            // Add-ons, texture packs.
            std::vector<fs::path> mod_items;
            for (fs::directory_iterator it(mods, ec), end; !ec && it != end; it.increment(ec)) {
                if (!internal_name(u8str(it->path().filename()))) {
                    mod_items.push_back(it->path());
                }
            }
            ec.clear();
            std::sort(mod_items.begin(), mod_items.end());
            for (const fs::path &item : mod_items) {
                const std::string file = u8str(item.filename());
                const std::string rel = "mods/" + file;
                if (owned(rel) || bundled_names.count(file)) {
                    continue;   // bundled: the game's own copy, rewritten on update
                }
                std::error_code e;
                const std::string ext = lower(u8str(item.extension()));
                if (options.dry_run && ext == ".nrm") {
                    const ZipInfo z = inspect_zip(item);
                    if (std::find(report.moved.begin(), report.moved.end(), mod_fields(z.mod_json).id) != report.moved.end()) {
                        continue;   // the dry run's moved hack: it wouldn't be in mods any more
                    }
                }
                if (fs::is_regular_file(item, e) && ext == ".nrm") {
                    const ZipInfo z = inspect_zip(item);
                    if (!z.ok) {
                        continue;
                    }
                    const ModFields f = mod_fields(z.mod_json);
                    record(f.id, Kind::Addon, f.version, f.name, rel, "migrated");
                }
                else if (fs::is_regular_file(item, e) && ext == ".rtz") {
                    const ZipInfo z = inspect_zip(item);
                    const ModFields f = mod_fields(z.mod_json);
                    const std::string id = f.id.empty() ? u8str(item.stem()) : f.id;
                    record(id, Kind::TexturePack, f.version, f.name, rel, "migrated");
                }
                else if (fs::is_directory(item, e) && fs::is_regular_file(item / "rt64.json", e)) {
                    const ModFields f = mod_fields(read_text(item / "mod.json"));
                    record(f.id.empty() ? file : f.id, Kind::TexturePack, f.version, f.name, rel, "migrated");
                }
            }

            // Model packs.
            std::vector<fs::path> packs;
            for (fs::directory_iterator it(mods / "models", ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code e;
                if (it->is_directory(e) && lower(u8str(it->path().extension())) == ".akimodel" && !internal_name(u8str(it->path().filename()))) {
                    packs.push_back(it->path());
                }
            }
            ec.clear();
            std::sort(packs.begin(), packs.end());
            for (const fs::path &pack : packs) {
                const std::string file = u8str(pack.filename());
                const std::string rel = "mods/models/" + file;
                if (owned(rel)) {
                    continue;
                }
                const json pj = json::parse(read_text(pack / "pack.json"), nullptr, false);
                const std::string stem = u8str(pack.stem());
                if (bundled_packs.count(file)) {
                    continue;   // the game's own starter pack (bundled_packs keeps it up to date)
                }
                const std::string id = str(pj, "id", stem);
                record(id, Kind::ModelPack, str(pj, "version"), str(pj, "name", id), rel, "migrated");
            }

            // 3. Save, log, mark.
            if (options.dry_run) {
                log("(dry run: nothing was changed)");
                return report;
            }
            const bool saved = save_registry(data_dir, registry);
            if (!saved) {
                log("couldn't write packages.json; migration will run again at the next start");
            }
            std::string text = read_text(data_dir / MigrationLog);   // appended: a rerun keeps what moved before
            text += "[" + now + "] package migration\n";
            for (const std::string &line : report.lines) {
                text += line + "\n";
            }
            write_text(data_dir / MigrationLog, text);
            if (saved) {
                write_text(data_dir / MigrationMarker, "1\n");
            }
            return report;
        }
    }

    bool migration_done(const fs::path &data_dir) {
        std::error_code ec;
        return fs::exists(data_dir / MigrationMarker, ec);
    }

    MigrateReport migrate(const fs::path &data_dir, const MigrateOptions &options) {
        try {
            return migrate_impl(data_dir, options);
        }
        catch (const std::exception &e) {
            MigrateReport r;
            r.lines.push_back(std::string("migration stopped: ") + e.what());
            return r;
        }
    }
}
