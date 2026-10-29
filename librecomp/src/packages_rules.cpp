#include "librecomp/packages.hpp"

#include <algorithm>
#include <cctype>

#include "librecomp/bundled_packs.hpp"
#include "json/json.hpp"

using json = nlohmann::json;

namespace recomp::packages {
    namespace {
        struct GameKey {
            const char *key;
            const char *game_id;
        };
        constexpr GameKey Games[] = {
            { "wt", "wcwnwoworldtour" }, { "revenge", "wcwnworevenge" }, { "wm2k", "wwfwm2k" },
            { "vpw64", "vpw64" }, { "nomercy", "wwfnomercy" }, { "vpw2", "vpw2" },
        };

        constexpr size_t MaxIdLength = 48;

        bool is_hex40(const std::string &s) {
            return s.size() == 40 && s.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
        }

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return s;
        }

        // Path prefix test on '/' boundaries: "mods/a" owns "mods/a" and "mods/a/b", not "mods/ab".
        bool covers(const std::string &owner, const std::string &path) {
            if (path.size() < owner.size() || path.compare(0, owner.size(), owner) != 0) {
                return false;
            }
            return path.size() == owner.size() || path[owner.size()] == '/';
        }
    }

    const char *kind_name(Kind kind) {
        switch (kind) {
            case Kind::Addon: return "addon";
            case Kind::TexturePack: return "texture-pack";
            case Kind::ModelPack: return "model-pack";
            case Kind::Conversion: return "conversion";
        }
        return "addon";
    }

    std::optional<Kind> parse_kind(const std::string &name) {
        for (Kind k : { Kind::Addon, Kind::TexturePack, Kind::ModelPack, Kind::Conversion }) {
            if (name == kind_name(k)) {
                return k;
            }
        }
        return std::nullopt;
    }

    bool valid_id(const std::string &id) {
        if (id.empty() || id.size() > MaxIdLength) {
            return false;
        }
        for (size_t i = 0; i < id.size(); i++) {
            const char c = id[i];
            const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
            if (!alnum && (i == 0 || (c != '_' && c != '.' && c != '-'))) {
                return false;
            }
        }
        return true;
    }

    bool valid_version(const std::string &version) {
        size_t pos = 0;
        for (int part = 0; part < 3; part++) {
            const size_t start = pos;
            while (pos < version.size() && version[pos] >= '0' && version[pos] <= '9') {
                pos++;
            }
            if (pos == start) {
                return false;
            }
            if (part < 2) {
                if (pos >= version.size() || version[pos] != '.') {
                    return false;
                }
                pos++;
            }
        }
        if (pos == version.size()) {
            return true;
        }
        if (version[pos] != '-' || pos + 1 == version.size()) {
            return false;
        }
        for (size_t i = pos + 1; i < version.size(); i++) {
            const char c = version[i];
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-') {
                return false;
            }
        }
        return true;
    }

    int compare_versions(const std::string &a, const std::string &b) {
        const size_t da = a.find('-'), db = b.find('-');
        const std::string ra = a.substr(0, da), rb = b.substr(0, db);
        const int c = recomp::mods::bundled_packs::compare_versions(ra, rb);
        if (c != 0) {
            return c;
        }
        const std::string pa = (da == std::string::npos) ? std::string() : a.substr(da + 1);
        const std::string pb = (db == std::string::npos) ? std::string() : b.substr(db + 1);
        if (pa == pb) {
            return 0;
        }
        if (pa.empty()) {
            return 1;   // a release sorts after its prereleases
        }
        if (pb.empty()) {
            return -1;
        }
        return (pa < pb) ? -1 : 1;
    }

    std::string game_id_for_key(const std::string &short_key) {
        for (const GameKey &g : Games) {
            if (short_key == g.key) {
                return g.game_id;
            }
        }
        return {};
    }

    std::string key_for_game_id(const std::string &game_id) {
        for (const GameKey &g : Games) {
            if (game_id == g.game_id) {
                return g.key;
            }
        }
        return {};
    }

    bool safe_relative_path(const std::string &path) {
        if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos ||
            path.find(':') != std::string::npos || path.find('\0') != std::string::npos) {
            return false;
        }
        size_t start = 0;
        while (start <= path.size()) {
            size_t end = path.find('/', start);
            if (end == std::string::npos) {
                end = path.size();
            }
            const std::string part = path.substr(start, end - start);
            if (part.empty() || part == "." || part == "..") {
                // A single trailing '/' on a folder is allowed ("conversion/").
                if (!(part.empty() && end == path.size() && start > 0)) {
                    return false;
                }
            }
            start = end + 1;
        }
        return true;
    }

    bool parse_manifest(const std::string &json_text, Manifest &out, std::vector<Problem> &problems) {
        out = Manifest{};
        const size_t before = problems.size();
        auto error = [&](const std::string &field, const std::string &message) { problems.push_back({ true, field, message }); };
        auto warn = [&](const std::string &field, const std::string &message) { problems.push_back({ false, field, message }); };

        const json root = json::parse(json_text, nullptr, false);
        if (root.is_discarded() || !root.is_object()) {
            error("", "aki-package.json isn't a JSON object");
            return false;
        }
        auto text = [&](const json &obj, const char *key, std::string &dst, bool required, const std::string &field) {
            const auto it = obj.find(key);
            if (it == obj.end()) {
                if (required) {
                    error(field, "missing");
                }
                return;
            }
            if (!it->is_string()) {
                error(field, "must be a string");
                return;
            }
            dst = it->get<std::string>();
        };
        auto list = [&](const json &obj, const char *key, std::vector<std::string> &dst, const std::string &field) {
            const auto it = obj.find(key);
            if (it == obj.end()) {
                return;
            }
            if (!it->is_array()) {
                error(field, "must be a list of strings");
                return;
            }
            for (const json &v : *it) {
                if (!v.is_string()) {
                    error(field, "must be a list of strings");
                    return;
                }
                dst.push_back(v.get<std::string>());
            }
        };

        // format
        const auto fmt = root.find("format");
        if (fmt == root.end() || !fmt->is_number_integer()) {
            error("format", "missing (must be 1)");
        }
        else {
            out.format = fmt->get<int>();
            if (out.format != ManifestFormat) {
                error("format", "format " + std::to_string(out.format) + " isn't supported by this version (1 is)");
            }
        }

        // kind
        std::string kind;
        text(root, "kind", kind, true, "kind");
        bool kind_ok = false;
        if (!kind.empty()) {
            if (auto k = parse_kind(kind)) {
                out.kind = *k;
                kind_ok = true;
            }
            else {
                error("kind", "\"" + kind + "\" isn't one of addon, texture-pack, model-pack, conversion");
            }
        }

        text(root, "id", out.id, true, "id");
        if (!out.id.empty() && !valid_id(out.id)) {
            error("id", "\"" + out.id + "\" must be lower-case letters, digits, '_', '.' or '-', start with a letter or digit, at most 48 characters");
        }
        text(root, "version", out.version, true, "version");
        if (!out.version.empty() && !valid_version(out.version)) {
            error("version", "\"" + out.version + "\" must be major.minor.patch (e.g. 1.2.0)");
        }
        text(root, "game_id", out.game_id, true, "game_id");
        if (!out.game_id.empty() && key_for_game_id(out.game_id).empty()) {
            const std::string mapped = game_id_for_key(out.game_id);
            error("game_id", "\"" + out.game_id + "\" isn't a game id" + (mapped.empty() ? std::string() : " (use \"" + mapped + "\")"));
        }
        text(root, "display_name", out.display_name, true, "display_name");
        if (root.contains("display_name") && out.display_name.empty()) {
            error("display_name", "is empty");
        }
        if (!root.contains("authors")) {
            error("authors", "missing");
        }
        else {
            list(root, "authors", out.authors, "authors");
            if (out.authors.empty() || std::any_of(out.authors.begin(), out.authors.end(), [](const std::string &a) { return a.empty(); })) {
                error("authors", "needs at least one non-empty name");
            }
        }
        text(root, "short_description", out.short_description, false, "short_description");
        text(root, "description", out.description, false, "description");
        text(root, "thumb", out.thumb, false, "thumb");
        if (!out.thumb.empty() && !safe_relative_path(out.thumb)) {
            error("thumb", "isn't a safe relative path");
        }
        text(root, "license", out.license, false, "license");
        text(root, "source", out.source, false, "source");

        if (const auto perm = root.find("permission"); perm != root.end()) {
            if (!perm->is_object()) {
                error("permission", "must be an object");
            }
            else {
                text(*perm, "redistribute", out.permission.redistribute, false, "permission.redistribute");
                text(*perm, "modify", out.permission.modify, false, "permission.modify");
                text(*perm, "statement", out.permission.statement, false, "permission.statement");
                text(*perm, "url", out.permission.url, false, "permission.url");
                for (auto [field, value] : { std::pair{ "permission.redistribute", &out.permission.redistribute },
                                             std::pair{ "permission.modify", &out.permission.modify } }) {
                    if (!value->empty() && *value != "yes" && *value != "no" && *value != "ask" && *value != "unknown") {
                        warn(field, "\"" + *value + "\" isn't yes, no, ask or unknown");
                    }
                }
            }
        }

        if (const auto rom = root.find("base_rom"); rom != root.end()) {
            if (!rom->is_object()) {
                error("base_rom", "must be an object");
            }
            else {
                list(*rom, "sha1", out.base_rom_sha1, "base_rom.sha1");
                for (std::string &h : out.base_rom_sha1) {
                    if (!is_hex40(h)) {
                        error("base_rom.sha1", "\"" + h + "\" isn't a 40-digit SHA-1");
                    }
                    h = lower(h);
                }
                text(*rom, "name", out.base_rom_name, false, "base_rom.name");
            }
        }
        if (kind_ok && out.kind == Kind::Conversion && out.base_rom_sha1.empty()) {
            error("base_rom.sha1", "a conversion must name the ROM(s) it is made for");
        }

        if (const auto req = root.find("requires"); req != root.end()) {
            if (!req->is_object()) {
                error("requires", "must be an object");
            }
            else {
                text(*req, "launcher", out.requires_launcher, false, "requires.launcher");
                text(*req, "minimum_recomp_version", out.requires_recomp, false, "requires.minimum_recomp_version");
                list(*req, "packages", out.requires_packages, "requires.packages");
                for (const std::string &p : out.requires_packages) {
                    if (!valid_id(p)) {
                        error("requires.packages", "\"" + p + "\" isn't a package id");
                    }
                }
            }
        }
        list(root, "replaces", out.replaces, "replaces");
        for (const std::string &r : out.replaces) {
            if (!valid_id(r)) {
                error("replaces", "\"" + r + "\" isn't a package id");
            }
            else if (r == out.id) {
                error("replaces", "a package can't replace itself");
            }
        }

        // payload, by kind
        const auto payload = root.find("payload");
        if (payload == root.end() || !payload->is_object()) {
            error("payload", payload == root.end() ? "missing" : "must be an object");
        }
        else if (kind_ok) {
            auto path_field = [&](const char *key, std::string &dst, bool required) {
                const std::string field = std::string("payload.") + key;
                text(*payload, key, dst, required, field);
                if (!dst.empty() && !safe_relative_path(dst)) {
                    error(field, "\"" + dst + "\" isn't a safe relative path");
                }
            };
            auto path_list = [&](const char *key, std::vector<std::string> &dst) {
                const std::string field = std::string("payload.") + key;
                list(*payload, key, dst, field);
                for (const std::string &p : dst) {
                    if (!safe_relative_path(p)) {
                        error(field, "\"" + p + "\" isn't a safe relative path");
                    }
                }
            };
            auto ends_with = [](const std::string &s, const char *suffix) {
                const std::string sl = lower(s), x = suffix;
                return sl.size() >= x.size() && sl.compare(sl.size() - x.size(), x.size(), x) == 0;
            };
            switch (out.kind) {
                case Kind::Addon:
                    path_field("nrm", out.nrm, true);
                    if (!out.nrm.empty() && !ends_with(out.nrm, ".nrm")) {
                        error("payload.nrm", "must be an .nrm file");
                    }
                    break;
                case Kind::TexturePack:
                    path_field("rtz", out.rtz, false);
                    path_field("folder", out.folder, false);
                    if (out.rtz.empty() == out.folder.empty()) {
                        error("payload", "a texture pack needs exactly one of rtz or folder");
                    }
                    if (!out.rtz.empty() && !ends_with(out.rtz, ".rtz")) {
                        error("payload.rtz", "must be an .rtz file");
                    }
                    break;
                case Kind::ModelPack:
                    path_list("packs", out.packs);
                    path_list("hd", out.hd);
                    if (out.packs.empty()) {
                        error("payload.packs", "a model pack needs at least one .akimodel folder");
                    }
                    for (const std::string &p : out.packs) {
                        std::string trimmed = p;
                        while (!trimmed.empty() && trimmed.back() == '/') {
                            trimmed.pop_back();
                        }
                        if (!ends_with(trimmed, ".akimodel")) {
                            error("payload.packs", "\"" + p + "\" must be an .akimodel folder");
                        }
                    }
                    for (const std::string &p : out.hd) {
                        if (!ends_with(p, ".rtz")) {
                            error("payload.hd", "\"" + p + "\" must be an .rtz file");
                        }
                    }
                    break;
                case Kind::Conversion:
                    path_field("folder", out.folder, true);
                    path_list("addons", out.addons);
                    for (const std::string &p : out.addons) {
                        if (!ends_with(p, ".rtz")) {
                            error("payload.addons", "\"" + p + "\" must be an .rtz texture pack");
                        }
                    }
                    break;
            }
        }

        return std::none_of(problems.begin() + before, problems.end(), [](const Problem &p) { return p.error; });
    }

    std::vector<std::string> payload_paths(const Manifest &m) {
        std::vector<std::string> out;
        auto add = [&](const std::string &p) {
            if (!p.empty()) {
                out.push_back(p);
            }
        };
        add(m.nrm);
        add(m.rtz);
        add(m.folder);
        for (const auto &p : m.packs) add(p);
        for (const auto &p : m.hd) add(p);
        for (const auto &p : m.addons) add(p);
        return out;
    }

    bool parse_registry(const std::string &json_text, Registry &out) {
        out = Registry{};
        const json root = json::parse(json_text, nullptr, false);
        if (root.is_discarded() || !root.is_object() || !root.contains("format") || !root["format"].is_number_integer() ||
            root["format"].get<int>() != RegistryFormat) {
            return false;
        }
        const auto list = root.find("packages");
        if (list == root.end() || !list->is_array()) {
            return false;
        }
        bool clean = true;
        for (const json &p : *list) {
            Entry e;
            const auto kind = (p.is_object() && p.contains("kind") && p["kind"].is_string()) ? parse_kind(p["kind"].get<std::string>()) : std::nullopt;
            if (!p.is_object() || !kind || !p.contains("id") || !p["id"].is_string() || !valid_id(p["id"].get<std::string>()) || find(out, p["id"].get<std::string>())) {
                clean = false;
                continue;
            }
            e.id = p["id"].get<std::string>();
            e.kind = *kind;
            e.version = p.value("version", std::string());
            e.display_name = p.value("display_name", std::string());
            e.source = p.value("source", std::string());
            e.installed = p.value("installed", std::string());
            e.parent = p.value("parent", std::string());
            bool files_ok = true;
            if (const auto files = p.find("files"); files != p.end() && files->is_array()) {
                for (const json &f : *files) {
                    const std::string path = (f.is_object() && f.contains("path") && f["path"].is_string()) ? f["path"].get<std::string>() : std::string();
                    const std::string hash = (f.is_object() && f.contains("hash") && f["hash"].is_string()) ? f["hash"].get<std::string>() : std::string();
                    if (!safe_relative_path(path) || hash.size() != 16 || hash.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
                        files_ok = false;
                        break;
                    }
                    e.files.push_back({ path, std::stoull(hash, nullptr, 16) });
                }
            }
            if (!files_ok) {
                clean = false;
                continue;
            }
            upsert(out, std::move(e));
        }
        return clean;
    }

    std::string format_registry(const Registry &registry) {
        json list = json::array();
        for (const Entry &e : registry.packages) {
            json files = json::array();
            for (const InstalledFile &f : e.files) {
                files.push_back({ { "path", f.path }, { "hash", recomp::mods::bundled_packs::hash_hex(f.hash) } });
            }
            json p = { { "id", e.id }, { "kind", kind_name(e.kind) }, { "version", e.version }, { "display_name", e.display_name },
                       { "source", e.source }, { "installed", e.installed } };
            if (!e.parent.empty()) {
                p["parent"] = e.parent;
            }
            p["files"] = files;
            list.push_back(p);
        }
        return json{ { "format", RegistryFormat }, { "packages", list } }.dump(2) + "\n";
    }

    const Entry *find(const Registry &registry, const std::string &id) {
        for (const Entry &e : registry.packages) {
            if (e.id == id) {
                return &e;
            }
        }
        return nullptr;
    }

    void upsert(Registry &registry, Entry entry) {
        auto &list = registry.packages;
        const auto it = std::lower_bound(list.begin(), list.end(), entry.id, [](const Entry &e, const std::string &id) { return e.id < id; });
        if (it != list.end() && it->id == entry.id) {
            *it = std::move(entry);
        }
        else {
            list.insert(it, std::move(entry));
        }
    }

    std::optional<Entry> remove(Registry &registry, const std::string &id) {
        auto &list = registry.packages;
        const auto it = std::find_if(list.begin(), list.end(), [&](const Entry &e) { return e.id == id; });
        if (it == list.end()) {
            return std::nullopt;
        }
        Entry e = std::move(*it);
        list.erase(it);
        return e;
    }

    std::string owner_of(const Registry &registry, const std::string &path) {
        for (const Entry &e : registry.packages) {
            for (const InstalledFile &f : e.files) {
                if (covers(f.path, path)) {
                    return e.id;
                }
            }
        }
        return {};
    }

    std::string conflict(const Registry &registry, const std::string &id, Kind kind, const std::vector<std::string> &paths) {
        if (const Entry *e = find(registry, id); e && e->kind != kind) {
            return "the id \"" + id + "\" is already used by the " + kind_name(e->kind) + " \"" +
                (e->display_name.empty() ? e->id : e->display_name) + "\"";
        }
        for (const std::string &p : paths) {
            for (const Entry &e : registry.packages) {
                if (e.id == id) {
                    continue;   // its own files (an upgrade)
                }
                for (const InstalledFile &f : e.files) {
                    if (covers(f.path, p) || covers(p, f.path)) {
                        return p + " belongs to \"" + (e.display_name.empty() ? e.id : e.display_name) + "\"";
                    }
                }
            }
        }
        return {};
    }

    FileState file_state(const InstalledFile &file, std::optional<uint64_t> current_hash) {
        if (!current_hash) {
            return FileState::Missing;
        }
        return (*current_hash == file.hash) ? FileState::Unchanged : FileState::Edited;
    }
}
