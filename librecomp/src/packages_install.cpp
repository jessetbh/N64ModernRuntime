// [wcw] Installing, upgrading and removing packages, and the drop folder (aki-roadmap#154 step 2): the file work
// around plan_install (see packages.hpp).

#include "librecomp/packages.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>
#include <system_error>

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

        std::string today_local() {
            const std::time_t t = std::time(nullptr);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char buf[16];
            std::strftime(buf, sizeof(buf), "%Y%m%d", &tm);
            return buf;
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

        std::string read_small_file(const fs::path &path) {
            FILE *f = nullptr;
#ifdef _WIN32
            if (_wfopen_s(&f, path.c_str(), L"rb") != 0) {
                f = nullptr;
            }
#else
            f = std::fopen(path.c_str(), "rb");
#endif
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

        bool write_small_file(const fs::path &path, const std::string &text) {
            FILE *f = nullptr;
#ifdef _WIN32
            if (_wfopen_s(&f, path.c_str(), L"wb") != 0) {
                f = nullptr;
            }
#else
            f = std::fopen(path.c_str(), "wb");
#endif
            if (!f) {
                return false;
            }
            const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
            return (std::fclose(f) == 0) && ok;
        }

        // A fresh working folder under <data>/.aki-work, removed by its owner.
        fs::path work_folder(const fs::path &data_dir, const char *what) {
            static std::mt19937_64 rng{ std::random_device{}() ^ uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) };
            char name[64];
            std::snprintf(name, sizeof(name), ".%s-%016llx", what, static_cast<unsigned long long>(rng()));
            return data_dir / ".aki-work" / name;
        }

        size_t write_to_file(void *opaque, mz_uint64, const void *data, size_t n) {
            return std::fwrite(data, 1, n, static_cast<FILE *>(opaque));
        }

        // Extracts a zip into dst. Every entry name must be a safe relative path (no zip-slip); entries are streamed
        // to disk, never held whole in memory. Empty on success, else why not.
        std::string extract_zip(const fs::path &zip, const fs::path &dst) {
            FILE *zf = nullptr;
#ifdef _WIN32
            if (_wfopen_s(&zf, zip.c_str(), L"rb") != 0) {
                zf = nullptr;
            }
#else
            zf = std::fopen(zip.c_str(), "rb");
#endif
            if (!zf) {
                return "it can't be opened";
            }
            mz_zip_archive archive{};
            if (!mz_zip_reader_init_cfile(&archive, zf, 0, 0)) {
                std::fclose(zf);
                return "it isn't a valid zip";
            }
            std::string error;
            const mz_uint count = mz_zip_reader_get_num_files(&archive);
            for (mz_uint i = 0; i < count && error.empty(); i++) {
                mz_zip_archive_file_stat st;
                if (!mz_zip_reader_file_stat(&archive, i, &st)) {
                    error = "the zip is damaged";
                    break;
                }
                std::string name = st.m_filename;
                std::replace(name.begin(), name.end(), '\\', '/');
                if (!safe_relative_path(name)) {
                    error = "the zip holds an unsafe path (" + name + ")";
                    break;
                }
                const fs::path out = dst / u8path(normal_path(name));
                std::error_code ec;
                if (st.m_is_directory) {
                    fs::create_directories(out, ec);
                    if (ec) {
                        error = "can't create " + name;
                    }
                    continue;
                }
                fs::create_directories(out.parent_path(), ec);
                FILE *of = nullptr;
#ifdef _WIN32
                if (_wfopen_s(&of, out.c_str(), L"wb") != 0) {
                    of = nullptr;
                }
#else
                of = std::fopen(out.c_str(), "wb");
#endif
                if (!of) {
                    error = "can't write " + name;
                    break;
                }
                const bool ok = mz_zip_reader_extract_to_callback(&archive, i, write_to_file, of, 0);
                if (std::fclose(of) != 0 || !ok) {
                    error = "can't extract " + name;
                }
            }
            mz_zip_reader_end(&archive);
            std::fclose(zf);
            return error;
        }

        // The package root: the folder holding aki-package.json (the folder itself, or its only subfolder).
        std::optional<fs::path> package_root(const fs::path &folder) {
            std::error_code ec;
            if (fs::is_regular_file(folder / ManifestFile, ec)) {
                return folder;
            }
            std::optional<fs::path> only;
            int entries = 0;
            for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
                entries++;
                if (it->is_directory(ec)) {
                    only = it->path();
                }
            }
            if (entries == 1 && only && fs::is_regular_file(*only / ManifestFile, ec)) {
                return only;
            }
            return std::nullopt;
        }

        const InstalledFile *record_for(const Entry *entry, const std::string &path) {
            if (!entry) {
                return nullptr;
            }
            for (const InstalledFile &f : entry->files) {
                if (lower(normal_path(f.path)) == lower(normal_path(path))) {
                    return &f;
                }
            }
            return nullptr;
        }

        bool remove_path(const fs::path &p) {
            std::error_code ec;
            fs::remove_all(p, ec);
            return !ec;
        }

        // One swap of a destination path: what was there moves aside, the staged copy moves in.
        struct Swap {
            fs::path dst, staged;
            fs::path aside;         // where the old content went ("" = there was none)
            bool keep_aside = false;// an edited / hand-placed copy: stays as .edited-<date> after success
            bool done = false;
        };

        void roll_back(std::vector<Swap> &swaps) {
            for (auto it = swaps.rbegin(); it != swaps.rend(); ++it) {
                std::error_code ec;
                if (it->done) {
                    fs::remove_all(it->dst, ec);
                }
                if (!it->aside.empty()) {
                    fs::rename(it->aside, it->dst, ec);
                }
                fs::remove_all(it->staged, ec);
            }
        }
    }

    InstallResult install(const fs::path &package, const fs::path &data_dir, const InstallOptions &options_in) {
        InstallResult result;
        InstallOptions options = options_in;
        if (options.date.empty()) {
            options.date = today_local();
        }
        if (options.now.empty()) {
            options.now = now_utc();
        }
        auto finish = [&](Outcome outcome, const std::string &message) {
            result.outcome = outcome;
            result.message = message;
            return result;
        };
        std::error_code ec;

        // 1. The package folder (a zip is unpacked into a working folder first).
        fs::path unpacked;
        struct Cleanup {
            fs::path p;
            ~Cleanup() {
                if (!p.empty()) {
                    std::error_code e;
                    fs::remove_all(p, e);
                    fs::remove(p.parent_path(), e);   // .aki-work, when nothing else is in it
                }
            }
        } cleanup;
        fs::path folder = package;
        if (fs::is_regular_file(package, ec)) {
            std::string ext = lower(u8str(package.extension()));
            if (ext != ".zip") {
                return finish(Outcome::Refused, "not a package: expected a folder or a .zip with aki-package.json");
            }
            unpacked = work_folder(data_dir, "unpack");
            cleanup.p = unpacked;
            fs::create_directories(unpacked, ec);
            if (ec) {
                return finish(Outcome::Failed, "can't unpack it: " + ec.message());
            }
            if (const std::string why = extract_zip(package, unpacked); !why.empty()) {
                return finish(Outcome::Refused, "can't unpack it: " + why);
            }
            folder = unpacked;
        }
        else if (!fs::is_directory(package, ec)) {
            return finish(Outcome::Refused, "not found");
        }
        const std::optional<fs::path> root = package_root(folder);
        if (!root) {
            return finish(Outcome::Refused, "not a package: no aki-package.json at its root");
        }

        // 2. The manifest.
        Manifest m;
        if (!parse_manifest(read_small_file(*root / ManifestFile), m, result.problems)) {
            return finish(Outcome::Refused, "its aki-package.json has errors");
        }
        result.id = m.id;
        result.display_name = m.display_name;
        result.version = m.version;
        result.kind = m.kind;

        // 3. The registry and the plan.
        Registry registry;
        const RegistryStatus status = load_registry(data_dir, registry);
        if (!may_save(status)) {
            return finish(Outcome::Refused, status == RegistryStatus::Newer ?
                "packages.json was written by a newer version; update the game first" :
                "packages.json can't be read; nothing was changed");
        }
        Plan plan = plan_install(registry, m, options);
        result.old_version = plan.old_version;
        result.problems.insert(result.problems.end(), plan.notes.begin(), plan.notes.end());
        if (plan.action == Action::Refuse) {
            return finish(Outcome::Refused, plan.reason);
        }
        if (plan.action == Action::AlreadyInstalled) {
            return finish(Outcome::AlreadyInstalled, plan.reason);
        }
        for (const Placement &p : plan.places) {
            const fs::path src = *root / u8path(p.from);
            if (p.folder ? !fs::is_directory(src, ec) : !fs::is_regular_file(src, ec)) {
                return finish(Outcome::Refused, "it names " + p.from + ", which isn't in the package");
            }
        }

        // 4. Stage every part next to its destination.
        const Entry *old = find(registry, m.id);
        std::vector<const Entry *> replaced;
        for (const std::string &r : plan.replaced) {
            replaced.push_back(find(registry, r));
        }
        std::vector<Swap> swaps;
        for (const Placement &p : plan.places) {
            Swap s;
            s.dst = data_dir / u8path(p.to);
            s.staged = s.dst;
            s.staged += ".installing";
            fs::remove_all(s.staged, ec);
            fs::create_directories(s.dst.parent_path(), ec);
            ec.clear();
            if (p.folder) {
                fs::copy(*root / u8path(p.from), s.staged, fs::copy_options::recursive, ec);
            }
            else {
                fs::copy_file(*root / u8path(p.from), s.staged, ec);
            }
            if (ec) {
                const std::string why = ec.message();
                swaps.push_back(s);
                roll_back(swaps);
                return finish(Outcome::Failed, "can't copy " + p.from + ": " + why);
            }
            swaps.push_back(s);
        }

        // 5. Swap in: what is at each destination moves aside first.
        auto owner_record = [&](const std::string &to) -> const InstalledFile * {
            if (const InstalledFile *f = record_for(old, to)) {
                return f;
            }
            for (const Entry *e : replaced) {
                if (const InstalledFile *f = record_for(e, to)) {
                    return f;
                }
            }
            return nullptr;
        };
        for (size_t i = 0; i < swaps.size(); i++) {
            Swap &s = swaps[i];
            const std::string to = plan.places[i].to;
            if (fs::exists(s.dst, ec)) {
                const InstalledFile *rec = owner_record(to);
                const bool unchanged = rec && file_state(*rec, hash_path(s.dst)) == FileState::Unchanged;
                if (unchanged) {
                    s.aside = s.dst;
                    s.aside += ".replaced";
                    fs::remove_all(s.aside, ec);
                }
                else {
                    // Edited by the player, or put there by hand: kept, never overwritten.
                    const std::string kept = edited_name(to, options.date, [&](const std::string &n) {
                        std::error_code e;
                        return fs::exists(data_dir / u8path(n), e);
                    });
                    s.aside = data_dir / u8path(kept);
                    s.keep_aside = true;
                }
                ec.clear();
                fs::rename(s.dst, s.aside, ec);
                if (ec) {
                    const std::string why = ec.message();
                    s.aside.clear();
                    roll_back(swaps);
                    return finish(Outcome::Failed, "can't move " + to + " aside: " + why);
                }
            }
            ec.clear();
            fs::rename(s.staged, s.dst, ec);
            if (ec) {
                const std::string why = ec.message();
                roll_back(swaps);
                return finish(Outcome::Failed, "can't install " + to + ": " + why);
            }
            s.done = true;
        }

        // 6. The new registry entry (hashes of what was installed).
        Entry entry;
        entry.id = m.id;
        entry.kind = m.kind;
        entry.version = m.version;
        entry.display_name = m.display_name;
        entry.source = options.source;
        entry.installed = options.now;
        for (size_t i = 0; i < swaps.size(); i++) {
            const std::optional<uint64_t> h = hash_path(swaps[i].dst);
            if (!h) {
                roll_back(swaps);
                return finish(Outcome::Failed, "can't read back " + plan.places[i].to);
            }
            entry.files.push_back({ normal_path(plan.places[i].to), *h });
        }

        // 7. The old version's and the replaced packages' files the new one doesn't use: unedited ones go, edited
        //    ones stay where they are (now the player's own).
        std::vector<fs::path> to_delete;
        auto retire = [&](const Entry *e) {
            if (!e) {
                return;
            }
            for (const InstalledFile &f : e->files) {
                const bool reused = std::any_of(entry.files.begin(), entry.files.end(),
                    [&](const InstalledFile &n) { return lower(n.path) == lower(normal_path(f.path)); });
                if (reused) {
                    continue;
                }
                const fs::path p = data_dir / u8path(f.path);
                switch (file_state(f, hash_path(p))) {
                    case FileState::Unchanged: to_delete.push_back(p); break;
                    case FileState::Edited: result.kept.push_back(f.path); break;
                    case FileState::Missing: break;
                }
            }
        };
        retire(old);
        for (const Entry *e : replaced) {
            retire(e);
        }

        Registry updated = registry;
        for (const std::string &r : plan.replaced) {
            remove(updated, r);
        }
        upsert(updated, entry);
        if (!save_registry(data_dir, updated)) {
            roll_back(swaps);
            return finish(Outcome::Failed, "can't write packages.json; nothing was changed");
        }
        for (Swap &s : swaps) {
            if (!s.aside.empty()) {
                if (s.keep_aside) {
                    result.kept.push_back(u8str(s.aside.lexically_relative(data_dir)));
                }
                else {
                    remove_path(s.aside);
                }
            }
        }
        for (const fs::path &p : to_delete) {
            remove_path(p);
        }

        const std::string name = m.display_name + " " + m.version;
        std::string kept_note;
        if (!result.kept.empty()) {
            kept_note = " (" + std::to_string(result.kept.size()) + " of your edited files kept)";
        }
        switch (plan.action) {
            case Action::Upgrade: return finish(Outcome::Upgraded, "updated " + m.display_name + " " + plan.old_version + " -> " + m.version + kept_note);
            case Action::Reinstall: return finish(Outcome::Reinstalled, "reinstalled " + name + kept_note);
            case Action::Downgrade: return finish(Outcome::Downgraded, "went back to " + name + " (from " + plan.old_version + ")" + kept_note);
            default: return finish(Outcome::Installed, std::string("installed ") + name + " (" + kind_name(m.kind) + ")" + kept_note);
        }
    }

    RemoveResult uninstall(const std::string &id, const fs::path &data_dir) {
        RemoveResult result;
        Registry registry;
        const RegistryStatus status = load_registry(data_dir, registry);
        if (!may_save(status)) {
            result.message = "packages.json can't be read; nothing was removed";
            return result;
        }
        const std::optional<Entry> entry = remove(registry, id);
        if (!entry) {
            result.message = "\"" + id + "\" isn't an installed package";
            return result;
        }
        std::vector<fs::path> to_delete;
        for (const InstalledFile &f : entry->files) {
            const fs::path p = data_dir / u8path(f.path);
            switch (file_state(f, hash_path(p))) {
                case FileState::Unchanged: to_delete.push_back(p); break;
                case FileState::Edited: result.kept.push_back(f.path); break;
                case FileState::Missing: break;
            }
        }
        if (!save_registry(data_dir, registry)) {
            result.message = "can't write packages.json; nothing was removed";
            return result;
        }
        for (const fs::path &p : to_delete) {
            remove_path(p);
        }
        result.removed = true;
        result.message = "removed " + (entry->display_name.empty() ? entry->id : entry->display_name) +
            (result.kept.empty() ? std::string() : " (" + std::to_string(result.kept.size()) + " edited files kept)");
        return result;
    }

    std::vector<InstallResult> process_import_folder(const fs::path &data_dir, const InstallOptions &options_in) {
        std::vector<InstallResult> results;
        const fs::path dir = data_dir / ImportFolder;
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            return results;
        }
        InstallOptions options = options_in;
        options.source = "dropped";
        std::vector<fs::path> items;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = u8str(it->path().filename());
            if (name.empty() || name[0] == '.' || name == "not-installed" || name == "last-import.json") {
                continue;
            }
            items.push_back(it->path());
        }
        if (items.empty()) {
            return results;
        }
        std::sort(items.begin(), items.end());
        const fs::path rejected = dir / "not-installed";
        json report = json::array();
        for (const fs::path &item : items) {
            InstallResult r = install(item, data_dir, options);
            const std::string name = u8str(item.filename());
            const bool done = r.outcome != Outcome::Refused && r.outcome != Outcome::Failed;
            if (done) {
                remove_path(item);
            }
            else {
                fs::create_directories(rejected, ec);
                const std::string moved = edited_name(name, options.date.empty() ? today_local() : options.date,
                    [&](const std::string &n) { std::error_code e; return fs::exists(rejected / u8path(n), e); });
                const fs::path target = fs::exists(rejected / item.filename(), ec) ? rejected / u8path(moved) : rejected / item.filename();
                fs::rename(item, target, ec);
                std::string why = r.message + "\n";
                for (const Problem &p : r.problems) {
                    why += std::string(p.error ? "error " : "warning ") + (p.field.empty() ? "" : p.field + ": ") + p.message + "\n";
                }
                write_small_file(target.parent_path() / u8path(u8str(target.filename()) + ".txt"), why);
            }
            json problems = json::array();
            for (const Problem &p : r.problems) {
                problems.push_back({ { "error", p.error }, { "field", p.field }, { "message", p.message } });
            }
            report.push_back({ { "file", name }, { "outcome", outcome_name(r.outcome) }, { "id", r.id },
                { "name", r.display_name }, { "version", r.version }, { "message", r.message }, { "problems", problems } });
            std::printf("[wcw] import %s: %s (%s)\n", name.c_str(), outcome_name(r.outcome), r.message.c_str());
            results.push_back(std::move(r));
        }
        write_small_file(dir / "last-import.json", json{ { "format", 1 }, { "results", report } }.dump(2) + "\n");
        return results;
    }
}
