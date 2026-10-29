// [wcw] Installing, upgrading and removing packages, and the drop folder (aki-roadmap#154 step 2): the file work
// around plan_install (see packages.hpp).

#include "librecomp/packages.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
#include <random>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "json/json.hpp"
#include "miniz.h"
#include "miniz_zip.h"

namespace recomp::packages {
    namespace fs = std::filesystem;
    using json = nlohmann::json;

    namespace {
        constexpr uint64_t MaxUnpackedBytes = uint64_t(16) << 30;   // 16 GiB: bigger than any real pack, stops a zip bomb
        constexpr uint64_t SpareBytes = uint64_t(512) << 20;        // leave this much free after unpacking
        constexpr const char *WorkFolder = ".aki-work";
        constexpr const char *JournalFile = "install-journal.json";

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return s;
        }

        fs::path u8path(const std::string &s) {
            return fs::path(std::u8string(s.begin(), s.end()));
        }

        std::string u8str(const fs::path &p) {
            const std::u8string s = p.generic_u8string();
            return std::string(s.begin(), s.end());
        }

        // Well-formed UTF-8 (no overlongs, no surrogates, nothing past U+10FFFF).
        bool valid_utf8(const std::string &s) {
            size_t i = 0;
            while (i < s.size()) {
                const unsigned char c = s[i];
                size_t n;
                uint32_t cp;
                if (c < 0x80) { i++; continue; }
                else if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
                else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
                else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
                else { return false; }
                if (i + n >= s.size()) { return false; }   // the sequence runs past the end
                for (size_t k = 1; k <= n; k++) {
                    const unsigned char cc = s[i + k];
                    if ((cc & 0xC0) != 0x80) { return false; }
                    cp = (cp << 6) | (cc & 0x3F);
                }
                if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10FFFF ||
                    (cp >= 0xD800 && cp <= 0xDFFF)) {
                    return false;
                }
                i += n + 1;
            }
            return true;
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

        std::string read_small_file(const fs::path &path) {
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

        bool write_small_file(const fs::path &path, const std::string &text) {
            FILE *f = open_file(path, L"wb", "wb");
            if (!f) {
                return false;
            }
            const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size() && std::fflush(f) == 0;
            return (std::fclose(f) == 0) && ok;
        }

        // Writes through a temporary file and a rename, so a reader never sees half of it.
        bool write_replacing(const fs::path &path, const std::string &text) {
            fs::path temp = path;
            temp += ".tmp";
            if (!write_small_file(temp, text)) {
                return false;
            }
            std::error_code ec;
            fs::rename(temp, path, ec);
            if (ec) {
                fs::remove(temp, ec);
                return false;
            }
            return true;
        }

        std::string dump(const json &j) {
            return j.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
        }

        fs::path work_folder(const fs::path &data_dir, const char *what) {
            static std::mt19937_64 rng{ std::random_device{}() ^ uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) };
            char name[64];
            std::snprintf(name, sizeof(name), "%s-%016llx", what, static_cast<unsigned long long>(rng()));
            return data_dir / WorkFolder / name;
        }

        bool ignorable(const std::string &top_level_name) {
            return top_level_name == "__MACOSX" || top_level_name == ".DS_Store";
        }

        size_t write_to_file(void *opaque, mz_uint64, const void *data, size_t n) {
            return std::fwrite(data, 1, n, static_cast<FILE *>(opaque));
        }

        // Extracts a zip into dst. Entry names must be UTF-8 (the zip's UTF-8 flag, or plain ASCII) and safe relative
        // paths; the total size is capped; entries stream to disk. Empty on success, else why not.
        std::string extract_zip(const fs::path &zip, const fs::path &dst) {
            FILE *zf = open_file(zip, L"rb", "rb");
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
            uint64_t total = 0;
            std::vector<mz_zip_archive_file_stat> stats;
            for (mz_uint i = 0; i < count && error.empty(); i++) {
                mz_zip_archive_file_stat st;
                if (!mz_zip_reader_file_stat(&archive, i, &st)) {
                    error = "the zip is damaged";
                    break;
                }
                const std::string raw = st.m_filename;
                const bool utf8_flag = (st.m_bit_flag & (1u << 11)) != 0;
                const bool ascii = std::all_of(raw.begin(), raw.end(), [](unsigned char c) { return c < 0x80; });
                if ((!utf8_flag && !ascii) || !valid_utf8(raw)) {
                    error = "its file names aren't UTF-8 (re-zip it with a current zip tool)";
                    break;
                }
                total += st.m_uncomp_size;
                stats.push_back(st);
            }
            if (error.empty() && total > MaxUnpackedBytes) {
                error = "it unpacks to more than 16 GiB";
            }
            if (error.empty()) {
                std::error_code ec;
                const fs::space_info space = fs::space(dst, ec);
                if (!ec && space.available < total + SpareBytes) {
                    error = "there isn't enough free disk space to unpack it";
                }
            }
            for (const mz_zip_archive_file_stat &st : stats) {
                if (!error.empty()) {
                    break;
                }
                std::string name = st.m_filename;
                std::replace(name.begin(), name.end(), '\\', '/');
                const std::string top = name.substr(0, name.find('/'));
                if (ignorable(top)) {
                    continue;
                }
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
                if (fs::exists(out, ec)) {
                    error = "the zip holds " + name + " twice (or two names that differ only in case)";
                    break;
                }
                fs::create_directories(out.parent_path(), ec);
                FILE *of = open_file(out, L"wb", "wb");
                if (!of) {
                    error = "can't write " + name;
                    break;
                }
                const bool ok = mz_zip_reader_extract_to_callback(&archive, st.m_file_index, write_to_file, of, 0);
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
                if (ignorable(u8str(it->path().filename()))) {
                    continue;
                }
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

        // A symlink, or on Windows any reparse point (a junction isn't reported as a symlink).
        bool is_link(const fs::path &p) {
            std::error_code ec;
            if (fs::is_symlink(fs::symlink_status(p, ec))) {
                return true;
            }
#ifdef _WIN32
            const DWORD attrs = GetFileAttributesW(p.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
                return true;
            }
#endif
            return false;
        }

        // Whether a package source holds a link anywhere (it isn't copied: it could point anywhere). The walk never
        // enters a link, so it can't be led outside the package.
        bool has_link(const fs::path &p) {
            if (is_link(p)) {
                return true;
            }
            std::error_code ec;
            if (!fs::is_directory(fs::symlink_status(p, ec))) {
                return false;
            }
            for (fs::recursive_directory_iterator it(p, fs::directory_options::none, ec), end; !ec && it != end; it.increment(ec)) {
                if (is_link(it->path())) {
                    return true;
                }
            }
            return !!ec;   // a folder we couldn't walk isn't known to be link-free
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

        // One swap of a destination: what is there moves aside, the staged copy moves in. All paths data-relative.
        struct Swap {
            std::string dst, staged;
            std::string aside;          // where the old content goes ("" = there is none)
            bool keep_aside = false;    // an edited / hand-placed copy: stays (under kept/) after success
            bool moved_aside = false;
            bool placed = false;
        };

        json journal_json(const std::string &id, const std::string &token, const std::vector<Swap> &swaps,
            const std::vector<std::string> &to_delete, bool committed) {
            json list = json::array();
            for (const Swap &s : swaps) {
                list.push_back({ { "dst", s.dst }, { "staged", s.staged }, { "aside", s.aside }, { "keep_aside", s.keep_aside } });
            }
            return { { "format", 1 }, { "id", id }, { "token", token }, { "committed", committed }, { "swaps", list }, { "delete", to_delete } };
        }

        // Undoes the swaps (newest first). Returns the paths it couldn't put back, for the message.
        std::vector<std::string> roll_back(const fs::path &data_dir, std::vector<Swap> &swaps) {
            std::vector<std::string> stuck;
            for (auto it = swaps.rbegin(); it != swaps.rend(); ++it) {
                std::error_code ec;
                const fs::path dst = data_dir / u8path(it->dst);
                if (it->placed) {
                    fs::remove_all(dst, ec);
                }
                if (it->moved_aside) {
                    ec.clear();
                    fs::rename(data_dir / u8path(it->aside), dst, ec);
                    if (ec) {
                        stuck.push_back(it->dst + " (it's at " + it->aside + ")");
                    }
                }
                fs::remove_all(data_dir / u8path(it->staged), ec);
            }
            return stuck;
        }

        InstallResult install_impl(const fs::path &package, const fs::path &data_dir, const InstallOptions &options_in) {
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
            if (const std::string recovered = recover_interrupted_install(data_dir); !recovered.empty()) {
                std::printf("[wcw] packages: %s\n", recovered.c_str());
                if (fs::exists(data_dir / WorkFolder / JournalFile, ec)) {
                    return finish(Outcome::Failed, "an earlier interrupted install couldn't be undone yet (a file is in use); "
                        "close other programs and start the game again");
                }
            }

            // 1. The package folder (a zip is unpacked into a working folder first).
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
                if (lower(u8str(package.extension())) != ".zip") {
                    return finish(Outcome::Refused, "not a package: expected a folder or a .zip with aki-package.json");
                }
                const fs::path unpacked = work_folder(data_dir, "unpack");
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
            const Plan plan = plan_install(registry, m, options);
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
                if (has_link(src)) {
                    return finish(Outcome::Refused, p.from + " holds a link to somewhere else; packages can't contain links");
                }
            }

            // 4. Decide every swap up front (where the current content goes), then stage the new parts.
            const Entry *old = find(registry, m.id);
            std::vector<const Entry *> replaced;
            for (const std::string &r : plan.replaced) {
                replaced.push_back(find(registry, r));
            }
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
            std::vector<std::string> reserved;   // kept names chosen in this install
            std::vector<Swap> swaps;
            for (const Placement &p : plan.places) {
                Swap s;
                s.dst = normal_path(p.to);
                s.staged = s.dst + ".installing";
                const fs::path dst = data_dir / u8path(s.dst);
                if (fs::exists(dst, ec)) {
                    const InstalledFile *rec = owner_record(p.to);
                    if (rec && file_state(*rec, hash_path(dst)) == FileState::Unchanged) {
                        s.aside = s.dst + ".replaced";
                        fs::remove_all(data_dir / u8path(s.aside), ec);   // a stale one must never be put back
                    }
                    else {
                        // Edited by the player, or put there by hand: kept, never overwritten.
                        s.aside = std::string(KeptFolder) + "/" + edited_name(s.dst, options.date, [&](const std::string &n) {
                            const std::string k = std::string(KeptFolder) + "/" + n;
                            std::error_code e;
                            return fs::exists(data_dir / u8path(k), e) || std::find(reserved.begin(), reserved.end(), k) != reserved.end();
                        });
                        reserved.push_back(s.aside);
                        s.keep_aside = true;
                    }
                }
                swaps.push_back(s);
            }
            for (size_t i = 0; i < swaps.size(); i++) {
                const fs::path staged = data_dir / u8path(swaps[i].staged);
                fs::remove_all(staged, ec);
                fs::create_directories(staged.parent_path(), ec);
                ec.clear();
                if (plan.places[i].folder) {
                    fs::copy(*root / u8path(plan.places[i].from), staged, fs::copy_options::recursive, ec);
                }
                else {
                    fs::copy_file(*root / u8path(plan.places[i].from), staged, ec);
                }
                if (ec) {
                    const std::string why = ec.message();
                    roll_back(data_dir, swaps);
                    return finish(Outcome::Failed, "can't copy " + plan.places[i].from + ": " + why);
                }
            }

            // 5. The journal, then the swaps. A crash from here on is finished or undone at the next start: the
            //    install's token goes into its registry entry, so a saved registry says "committed" by itself.
            fs::create_directories(data_dir / WorkFolder, ec);
            const fs::path journal = data_dir / WorkFolder / JournalFile;
            char token_buf[32];
            {
                static std::mt19937_64 rng{ std::random_device{}() ^ uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) };
                std::snprintf(token_buf, sizeof(token_buf), "%016llx", static_cast<unsigned long long>(rng()));
            }
            const std::string token = token_buf;
            if (!write_replacing(journal, dump(journal_json(m.id, token, swaps, {}, false)))) {
                roll_back(data_dir, swaps);
                return finish(Outcome::Failed, "can't write the install journal; nothing was changed");
            }
            auto fail = [&](const std::string &why) {
                const std::vector<std::string> stuck = roll_back(data_dir, swaps);
                std::error_code e;
                fs::remove(journal, e);
                fs::remove(data_dir / WorkFolder, e);
                std::string message = why;
                if (!stuck.empty()) {
                    message += "; couldn't put back:";
                    for (const std::string &s : stuck) {
                        message += " " + s;
                    }
                }
                return finish(Outcome::Failed, message);
            };
            for (Swap &s : swaps) {
                const fs::path dst = data_dir / u8path(s.dst);
                if (!s.aside.empty()) {
                    const fs::path aside = data_dir / u8path(s.aside);
                    fs::remove_all(aside, ec);   // a stale .replaced from before (kept names are always fresh)
                    fs::create_directories(aside.parent_path(), ec);
                    ec.clear();
                    fs::rename(dst, aside, ec);
                    if (ec) {
                        return fail("can't move " + s.dst + " aside: " + ec.message());
                    }
                    s.moved_aside = true;
                }
                ec.clear();
                fs::rename(data_dir / u8path(s.staged), dst, ec);
                if (ec) {
                    return fail("can't install " + s.dst + ": " + ec.message());
                }
                s.placed = true;
            }

            // 6. The new registry entry (hashes of what was installed).
            Entry entry;
            entry.id = m.id;
            entry.kind = m.kind;
            entry.version = m.version;
            entry.display_name = m.display_name;
            entry.source = options.source;
            entry.installed = options.now;
            entry.extra = json{ { "install_token", token } }.dump();
            for (const Swap &s : swaps) {
                const std::optional<uint64_t> h = hash_path(data_dir / u8path(s.dst));
                if (!h) {
                    return fail("can't read back " + s.dst);
                }
                entry.files.push_back({ s.dst, *h });
            }

            // 7. The old version's and the replaced packages' files the new one doesn't use: unedited ones go, edited
            //    ones stay where they are (now the player's own).
            std::vector<std::string> to_delete;
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
                    switch (file_state(f, hash_path(data_dir / u8path(f.path)))) {
                        case FileState::Unchanged: to_delete.push_back(normal_path(f.path)); break;
                        case FileState::Edited: result.kept.push_back(normal_path(f.path)); break;
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
            // The journal gets the delete list first: a crash right after the registry save is then finished with it.
            write_replacing(journal, dump(journal_json(m.id, token, swaps, to_delete, false)));
            if (!save_registry(data_dir, updated)) {
                return fail("can't write packages.json; nothing was changed");
            }

            // 8. Committed: the cleanup can't be undone any more, so the journal says to finish it after a crash.
            write_replacing(journal, dump(journal_json(m.id, token, swaps, to_delete, true)));
            for (const Swap &s : swaps) {
                if (s.keep_aside) {
                    result.kept.push_back(s.aside);
                }
                else if (!s.aside.empty()) {
                    remove_path(data_dir / u8path(s.aside));
                }
            }
            for (const std::string &p : to_delete) {
                remove_path(data_dir / u8path(p));
            }
            fs::remove(journal, ec);
            fs::remove(data_dir / WorkFolder, ec);

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
    }

    std::string recover_interrupted_install(const fs::path &data_dir) {
        const fs::path journal = data_dir / WorkFolder / JournalFile;
        std::error_code ec;
        if (!fs::exists(journal, ec)) {
            // Unpack folders a crash left behind (they can be large); no install is running while this is called.
            bool cleared = false;
            for (fs::directory_iterator it(data_dir / WorkFolder, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code e;
                if (u8str(it->path().filename()).rfind("unpack-", 0) == 0) {
                    fs::remove_all(it->path(), e);
                    cleared = true;
                }
            }
            ec.clear();
            fs::remove(data_dir / WorkFolder, ec);
            return cleared ? "removed what an interrupted unpack left behind" : std::string();
        }
        const json j = json::parse(read_small_file(journal), nullptr, false);
        if (j.is_discarded() || !j.is_object() || !j.contains("swaps") || !j["swaps"].is_array()) {
            fs::remove(journal, ec);
            return "an unreadable install journal was removed";
        }
        auto path_of = [&](const json &s, const char *key) {
            const std::string p = (s.contains(key) && s[key].is_string()) ? s[key].get<std::string>() : std::string();
            return (!p.empty() && safe_relative_path(p)) ? p : std::string();
        };
        bool committed = j.value("committed", false);
        if (!committed) {
            // The registry may have been saved just before the crash, ahead of the journal's "committed".
            Registry registry;
            const std::string id = j.value("id", std::string()), token = j.value("token", std::string());
            if (!token.empty() && may_save(load_registry(data_dir, registry))) {
                if (const Entry *e = find(registry, id); e && !e->extra.empty()) {
                    const json extra = json::parse(e->extra, nullptr, false);
                    committed = extra.is_object() && extra.value("install_token", std::string()) == token;
                }
            }
        }
        bool stuck = false;
        std::vector<json> swaps(j["swaps"].begin(), j["swaps"].end());
        if (committed) {
            // The registry has the new version: finish the cleanup.
            for (const json &s : swaps) {
                const std::string aside = path_of(s, "aside"), staged = path_of(s, "staged");
                if (!aside.empty() && !s.value("keep_aside", false)) {
                    fs::remove_all(data_dir / u8path(aside), ec);
                }
                if (!staged.empty()) {
                    fs::remove_all(data_dir / u8path(staged), ec);
                }
            }
            if (j.contains("delete") && j["delete"].is_array()) {
                for (const json &d : j["delete"]) {
                    if (d.is_string() && safe_relative_path(d.get<std::string>())) {
                        fs::remove_all(data_dir / u8path(d.get<std::string>()), ec);
                    }
                }
            }
        }
        else {
            // The registry still has the old version: put back what was there (newest swap first).
            for (auto it = swaps.rbegin(); it != swaps.rend(); ++it) {
                const std::string dst = path_of(*it, "dst"), staged = path_of(*it, "staged"), aside = path_of(*it, "aside");
                if (dst.empty()) {
                    continue;
                }
                const fs::path d = data_dir / u8path(dst);
                const bool staged_left = !staged.empty() && fs::exists(data_dir / u8path(staged), ec);
                if (!aside.empty() && fs::exists(data_dir / u8path(aside), ec)) {
                    fs::remove_all(d, ec);              // the new copy, if it got placed
                    ec.clear();
                    fs::rename(data_dir / u8path(aside), d, ec);
                    if (ec) {
                        stuck = true;
                    }
                }
                else if (aside.empty() && !staged_left) {
                    fs::remove_all(d, ec);              // nothing was there before: the new copy goes
                }
                if (staged_left) {
                    fs::remove_all(data_dir / u8path(staged), ec);
                }
            }
        }
        if (stuck) {
            // Keep the journal: the next start tries again (a file was locked by another program).
            return "couldn't fully undo an install a crash interrupted; will try again at the next start";
        }
        fs::remove(journal, ec);
        // Unpack folders a crash left behind (they can be large).
        for (fs::directory_iterator it(data_dir / WorkFolder, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e;
            if (u8str(it->path().filename()).rfind("unpack-", 0) == 0) {
                fs::remove_all(it->path(), e);
            }
        }
        ec.clear();
        fs::remove(data_dir / WorkFolder, ec);
        return committed ? "finished an install a crash interrupted" : "undid an install a crash interrupted";
    }

    InstallResult install(const fs::path &package, const fs::path &data_dir, const InstallOptions &options) {
        try {
            return install_impl(package, data_dir, options);
        }
        catch (const std::exception &e) {
            InstallResult r;
            r.outcome = Outcome::Failed;
            r.message = std::string("unexpected error: ") + e.what();
            return r;
        }
    }

    RemoveResult uninstall(const std::string &id, const fs::path &data_dir) {
        RemoveResult result;
        try {
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
        }
        catch (const std::exception &e) {
            result.removed = false;
            result.message = std::string("unexpected error: ") + e.what();
        }
        return result;
    }

    bool looks_like_package(const fs::path &path) {
        try {
            std::error_code ec;
            if (fs::is_directory(path, ec)) {
                return package_root(path).has_value();
            }
            if (!fs::is_regular_file(path, ec) || lower(u8str(path.extension())) != ".zip") {
                return false;
            }
            FILE *zf = open_file(path, L"rb", "rb");
            if (!zf) {
                return false;
            }
            mz_zip_archive archive{};
            bool found = false;
            if (mz_zip_reader_init_cfile(&archive, zf, 0, 0)) {
                std::vector<std::string> tops;
                bool root_manifest = false, nested_manifest = false;
                const mz_uint count = mz_zip_reader_get_num_files(&archive);
                for (mz_uint i = 0; i < count; i++) {
                    mz_zip_archive_file_stat st;
                    if (!mz_zip_reader_file_stat(&archive, i, &st)) {
                        continue;
                    }
                    std::string name = st.m_filename;
                    std::replace(name.begin(), name.end(), '\\', '/');
                    const std::string top = name.substr(0, name.find('/'));
                    if (ignorable(top)) {
                        continue;
                    }
                    if (std::find(tops.begin(), tops.end(), top) == tops.end()) {
                        tops.push_back(top);
                    }
                    root_manifest |= name == ManifestFile;
                    nested_manifest |= name == top + "/" + ManifestFile;
                }
                found = root_manifest || (nested_manifest && tops.size() == 1);
                mz_zip_reader_end(&archive);
            }
            std::fclose(zf);
            return found;
        }
        catch (const std::exception &) {
            return false;
        }
    }

    std::vector<InstallResult> process_import_folder(const fs::path &data_dir, const InstallOptions &options_in) {
        std::vector<InstallResult> results;
        try {
            if (const std::string recovered = recover_interrupted_install(data_dir); !recovered.empty()) {
                std::printf("[wcw] packages: %s\n", recovered.c_str());
            }
            const fs::path dir = data_dir / ImportFolder;
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) {
                return results;
            }
            InstallOptions options = options_in;
            options.source = "dropped";
            if (options.date.empty()) {
                options.date = today_local();
            }
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
                    const std::string target_name = edited_name(name, options.date, [&](const std::string &n) {
                        std::error_code e;
                        return fs::exists(rejected / u8path(n), e);
                    });
                    const fs::path target = fs::exists(rejected / item.filename(), ec) ? rejected / u8path(target_name) : rejected / item.filename();
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
            write_small_file(dir / "last-import.json", dump(json{ { "format", 1 }, { "results", report } }));
        }
        catch (const std::exception &e) {
            std::printf("[wcw] import: unexpected error: %s\n", e.what());
        }
        return results;
    }
}
