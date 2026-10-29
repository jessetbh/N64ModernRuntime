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

        bool is_hex(const std::string &s, size_t n) {
            return s.size() == n && s.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
        }

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return s;
        }

        bool is_blank(const std::string &s) {
            return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
        }

        // Windows device names, matched on the stem before the first '.' ("nul.txt" and "aux.tar.gz" are devices too).
        bool is_device_name(const std::string &part) {
            const std::string stem = lower(part.substr(0, part.find('.')));
            if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" || stem == "conin$" || stem == "conout$" ||
                stem == "clock$") {
                return true;
            }
            return stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) &&
                stem[3] >= '1' && stem[3] <= '9';
        }

        // Path prefix test on '/' boundaries, ignoring ASCII case: "mods/a" owns "mods/a" and "mods/A/b", not "mods/ab".
        bool covers(const std::string &owner_path, const std::string &path) {
            const std::string owner = lower(normal_path(owner_path));
            const std::string p = lower(normal_path(path));
            if (p.size() < owner.size() || p.compare(0, owner.size(), owner) != 0) {
                return false;
            }
            return p.size() == owner.size() || p[owner.size()] == '/';
        }

        // Decimal digit strings of any length, as numbers.
        int compare_numbers(const std::string &a, const std::string &b) {
            const size_t za = std::min(a.find_first_not_of('0'), a.size());
            const size_t zb = std::min(b.find_first_not_of('0'), b.size());
            const size_t la = a.size() - za, lb = b.size() - zb;
            if (la != lb) {
                return (la < lb) ? -1 : 1;
            }
            const int c = a.compare(za, la, b, zb, lb);
            return (c < 0) ? -1 : (c > 0) ? 1 : 0;
        }

        bool all_digits(const std::string &s) {
            return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
        }

        std::vector<std::string> split(const std::string &s, char sep) {
            std::vector<std::string> out;
            size_t start = 0;
            while (true) {
                const size_t end = s.find(sep, start);
                out.push_back(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
                if (end == std::string::npos) {
                    return out;
                }
                start = end + 1;
            }
        }
    }

    const char *kind_name(Kind kind) {
        switch (kind) {
            case Kind::Addon: return "addon";
            case Kind::TexturePack: return "texture-pack";
            case Kind::ModelPack: return "model-pack";
            case Kind::Conversion: return "conversion";
            case Kind::Unknown: return "unknown";
        }
        return "unknown";
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
        if (id.empty() || id.size() > MaxIdLength || id.back() == '.' || is_device_name(id)) {
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
        const size_t dash = version.find('-');
        const std::vector<std::string> core = split(version.substr(0, dash), '.');
        if (core.size() != 3 || !std::all_of(core.begin(), core.end(), all_digits)) {
            return false;
        }
        if (dash == std::string::npos) {
            return true;
        }
        const std::string pre = version.substr(dash + 1);
        if (pre.empty()) {
            return false;
        }
        for (const std::string &id : split(pre, '.')) {
            if (id.empty() || !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c) || c == '-'; })) {
                return false;
            }
        }
        return true;
    }

    int compare_versions(const std::string &a, const std::string &b) {
        const size_t da = a.find('-'), db = b.find('-');
        const std::vector<std::string> ca = split(a.substr(0, da), '.'), cb = split(b.substr(0, db), '.');
        for (size_t i = 0; i < std::max(ca.size(), cb.size()); i++) {
            const std::string x = (i < ca.size() && all_digits(ca[i])) ? ca[i] : "0";
            const std::string y = (i < cb.size() && all_digits(cb[i])) ? cb[i] : "0";
            if (const int c = compare_numbers(x, y)) {
                return c;
            }
        }
        const bool pre_a = da != std::string::npos, pre_b = db != std::string::npos;
        if (!pre_a || !pre_b) {
            return (pre_a == pre_b) ? 0 : (pre_a ? -1 : 1);   // a release sorts after its prereleases
        }
        const std::vector<std::string> pa = split(a.substr(da + 1), '.'), pb = split(b.substr(db + 1), '.');
        for (size_t i = 0; i < std::min(pa.size(), pb.size()); i++) {
            const bool na = all_digits(pa[i]), nb = all_digits(pb[i]);
            int c;
            if (na && nb) {
                c = compare_numbers(pa[i], pb[i]);
            }
            else if (na != nb) {
                c = na ? -1 : 1;   // a number sorts before text
            }
            else {
                c = pa[i].compare(pb[i]);
                c = (c < 0) ? -1 : (c > 0) ? 1 : 0;
            }
            if (c) {
                return c;
            }
        }
        return (pa.size() == pb.size()) ? 0 : (pa.size() < pb.size() ? -1 : 1);
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
        if (path.empty() || path.front() == '/') {
            return false;
        }
        for (unsigned char c : path) {
            if (c < 0x20 || c == 0x7F || std::string("\\:*?\"<>|").find(char(c)) != std::string::npos) {
                return false;
            }
        }
        const std::string body = (path.back() == '/') ? path.substr(0, path.size() - 1) : path;
        if (body.empty()) {
            return false;
        }
        for (const std::string &part : split(body, '/')) {
            if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ' || is_device_name(part)) {
                return false;
            }
        }
        return true;
    }

    std::string normal_path(const std::string &path) {
        std::string p = path;
        while (!p.empty() && p.back() == '/') {
            p.pop_back();
        }
        return p;
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
            const int64_t f = fmt->get<int64_t>();
            out.format = (f >= 0 && f <= 1000000) ? int(f) : -1;
            if (f != ManifestFormat) {
                error("format", "format " + std::to_string(f) + " isn't supported by this version (1 is)");
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
            error("id", "\"" + out.id + "\" must be lower-case letters, digits, '_', '.' or '-', start with a letter or digit, "
                "not end in '.', not be a Windows device name, at most 48 characters");
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
        if (root.contains("display_name") && root["display_name"].is_string() && is_blank(out.display_name)) {
            error("display_name", "is empty");
        }
        if (!root.contains("authors")) {
            error("authors", "missing");
        }
        else {
            list(root, "authors", out.authors, "authors");
            if (out.authors.empty() || std::any_of(out.authors.begin(), out.authors.end(), is_blank)) {
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
                    if (!is_hex(h, 40)) {
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
                const std::string sl = lower(normal_path(s)), x = suffix;
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
                        if (!ends_with(p, ".akimodel")) {
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
                case Kind::Unknown:
                    break;
            }
            // Every payload part installs on its own: no path twice, none inside another.
            const std::vector<std::string> paths = payload_paths(out);
            for (size_t i = 0; i < paths.size(); i++) {
                for (size_t j = i + 1; j < paths.size(); j++) {
                    if (covers(paths[i], paths[j]) || covers(paths[j], paths[i])) {
                        error("payload", "\"" + paths[i] + "\" and \"" + paths[j] + "\" overlap");
                    }
                }
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

    bool may_save(RegistryStatus status) {
        return status == RegistryStatus::Ok || status == RegistryStatus::Damaged;
    }

    RegistryStatus parse_registry(const std::string &json_text, Registry &out) {
        out = Registry{};
        const json root = json::parse(json_text, nullptr, false);
        if (root.is_discarded() || !root.is_object() || !root.contains("format") || !root["format"].is_number_integer()) {
            return RegistryStatus::Corrupt;
        }
        const int64_t format = root["format"].get<int64_t>();
        if (format > RegistryFormat) {
            return RegistryStatus::Newer;
        }
        const auto list = root.find("packages");
        if (format != RegistryFormat || list == root.end() || !list->is_array()) {
            return RegistryStatus::Corrupt;
        }
        static const char *Known[] = { "id", "kind", "version", "display_name", "source", "installed", "parent", "files" };
        bool clean = true;
        for (const json &p : *list) {
            if (!p.is_object() || !p.contains("id") || !p["id"].is_string() || !p.contains("kind") || !p["kind"].is_string()) {
                clean = false;
                continue;
            }
            Entry e;
            e.id = p["id"].get<std::string>();
            e.kind_text = p["kind"].get<std::string>();
            e.kind = parse_kind(e.kind_text).value_or(Kind::Unknown);
            if (!valid_id(e.id) || e.kind_text.empty() || find(out, e.id)) {
                clean = false;
                continue;
            }
            auto str = [&](const char *key) {
                const auto it = p.find(key);
                return (it != p.end() && it->is_string()) ? it->get<std::string>() : std::string();
            };
            e.version = str("version");
            e.display_name = str("display_name");
            e.source = str("source");
            e.installed = str("installed");
            e.parent = str("parent");
            if (!e.parent.empty() && !valid_id(e.parent)) {
                e.parent.clear();
                clean = false;
            }
            bool files_ok = true;
            if (const auto files = p.find("files"); files != p.end()) {
                if (!files->is_array()) {
                    files_ok = false;
                }
                else {
                    for (const json &f : *files) {
                        const std::string path = (f.is_object() && f.contains("path") && f["path"].is_string()) ? f["path"].get<std::string>() : std::string();
                        const std::string hash = (f.is_object() && f.contains("hash") && f["hash"].is_string()) ? f["hash"].get<std::string>() : std::string();
                        if (!safe_relative_path(path) || !is_hex(hash, 16)) {
                            files_ok = false;
                            break;
                        }
                        e.files.push_back({ normal_path(path), std::stoull(hash, nullptr, 16) });
                    }
                }
            }
            // A path already claimed by an entry read before this one: keep the first owner.
            for (const InstalledFile &f : e.files) {
                for (const Entry &other : out.packages) {
                    for (const InstalledFile &g : other.files) {
                        if (covers(g.path, f.path) || covers(f.path, g.path)) {
                            files_ok = false;
                        }
                    }
                }
            }
            if (!files_ok) {
                clean = false;
                continue;
            }
            json extra = json::object();
            for (const auto &[key, value] : p.items()) {
                if (std::find(std::begin(Known), std::end(Known), key) == std::end(Known)) {
                    extra[key] = value;
                }
            }
            if (!extra.empty()) {
                e.extra = extra.dump();
            }
            upsert(out, std::move(e));
        }
        return clean ? RegistryStatus::Ok : RegistryStatus::Damaged;
    }

    std::string format_registry(const Registry &registry) {
        json list = json::array();
        for (const Entry &e : registry.packages) {
            json files = json::array();
            for (const InstalledFile &f : e.files) {
                files.push_back({ { "path", normal_path(f.path) }, { "hash", recomp::mods::bundled_packs::hash_hex(f.hash) } });
            }
            const std::string kind = (e.kind != Kind::Unknown) ? std::string(kind_name(e.kind)) :
                (e.kind_text.empty() ? std::string("unknown") : e.kind_text);
            json p = { { "id", e.id }, { "kind", kind }, { "version", e.version }, { "display_name", e.display_name },
                       { "source", e.source }, { "installed", e.installed } };
            if (!e.parent.empty()) {
                p["parent"] = e.parent;
            }
            p["files"] = files;
            if (!e.extra.empty()) {
                const json extra = json::parse(e.extra, nullptr, false);
                if (extra.is_object()) {
                    for (const auto &[key, value] : extra.items()) {
                        if (!p.contains(key)) {
                            p[key] = value;
                        }
                    }
                }
            }
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
        for (InstalledFile &f : entry.files) {
            f.path = normal_path(f.path);
        }
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

    std::string conflict(const Registry &registry, const std::string &id, Kind kind, const std::vector<std::string> &paths,
        const std::vector<std::string> &replacing) {
        auto skipped = [&](const std::string &other) {
            return other == id || std::find(replacing.begin(), replacing.end(), other) != replacing.end();
        };
        if (const Entry *e = find(registry, id); e && e->kind != kind) {
            return "the id \"" + id + "\" is already used by the " + (e->kind == Kind::Unknown ? e->kind_text : kind_name(e->kind)) +
                " \"" + (e->display_name.empty() ? e->id : e->display_name) + "\"";
        }
        for (const std::string &p : paths) {
            for (const Entry &e : registry.packages) {
                if (skipped(e.id)) {
                    continue;   // its own files (an upgrade), or a package this one replaces
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

    uint64_t hash_bytes(const std::string &file_name, const std::string &data) {
        return recomp::mods::bundled_packs::pack_hash({ { file_name, data } });
    }

    // ---- Installing (aki-roadmap#154 step 2): the pure parts ----

    std::vector<Placement> placements(const Manifest &m) {
        auto base = [](const std::string &p) {
            const std::string n = normal_path(p);
            const size_t slash = n.find_last_of('/');
            return (slash == std::string::npos) ? n : n.substr(slash + 1);
        };
        std::vector<Placement> out;
        switch (m.kind) {
            case Kind::Addon:
                out.push_back({ normal_path(m.nrm), "mods/" + base(m.nrm), false });
                break;
            case Kind::TexturePack:
                if (!m.rtz.empty()) {
                    out.push_back({ normal_path(m.rtz), "mods/" + base(m.rtz), false });
                }
                else {
                    out.push_back({ normal_path(m.folder), "mods/" + m.id, true });
                }
                break;
            case Kind::ModelPack:
                for (const std::string &p : m.packs) {
                    out.push_back({ normal_path(p), "mods/models/" + base(p), true });
                }
                for (const std::string &p : m.hd) {
                    out.push_back({ normal_path(p), "mods/" + base(p), false });
                }
                break;
            case Kind::Conversion:
                out.push_back({ normal_path(m.folder), "conversions/" + m.id, true });
                for (const std::string &p : m.addons) {
                    out.push_back({ normal_path(p), "mods/" + base(p), false });
                }
                break;
            case Kind::Unknown:
                break;
        }
        return out;
    }

    Plan plan_install(const Registry &registry, const Manifest &m, const InstallOptions &options) {
        Plan plan;
        plan.places = placements(m);
        auto refuse = [&](const std::string &why) {
            plan.action = Action::Refuse;
            plan.reason = why;
            return plan;
        };
        if (m.game_id != options.game_id) {
            const std::string key = key_for_game_id(m.game_id);
            return refuse("it's for another game (" + (key.empty() ? m.game_id : key) + ")");
        }
        if (!options.recomp_version.empty() && !m.requires_recomp.empty() &&
            compare_versions(options.recomp_version, m.requires_recomp) < 0) {
            return refuse("it needs game build " + m.requires_recomp + " or newer (this is " + options.recomp_version + ")");
        }
        if (!m.base_rom_sha1.empty()) {
            const std::string rom = m.base_rom_name.empty() ? std::string("a different ROM") : m.base_rom_name;
            if (options.rom_sha1.empty()) {
                if (m.kind == Kind::Conversion) {
                    return refuse("it needs " + rom + ", and your ROM can't be checked (import it in the game first)");
                }
            }
            else if (std::find(m.base_rom_sha1.begin(), m.base_rom_sha1.end(), lower(options.rom_sha1)) == m.base_rom_sha1.end()) {
                if (m.kind == Kind::Conversion) {
                    return refuse("it needs " + rom + "; your ROM is a different one");
                }
                plan.notes.push_back({ false, "base_rom", "made for " + rom + "; it may not work with your ROM" });
            }
        }
        for (size_t i = 0; i < plan.places.size(); i++) {
            for (size_t j = i + 1; j < plan.places.size(); j++) {
                if (covers(plan.places[i].to, plan.places[j].to) || covers(plan.places[j].to, plan.places[i].to)) {
                    return refuse(plan.places[i].from + " and " + plan.places[j].from + " would both install to " + plan.places[j].to);
                }
            }
        }
        if (m.permission.redistribute == "no") {
            plan.notes.push_back({ false, "permission", "for your own use: the authors don't allow sharing it" });
        }
        for (const std::string &dep : m.requires_packages) {
            if (!find(registry, dep)) {
                plan.notes.push_back({ false, "requires.packages", "it needs \"" + dep + "\", which isn't installed" });
            }
        }
        for (const std::string &r : m.replaces) {
            if (find(registry, r)) {
                plan.replaced.push_back(r);
            }
        }
        std::vector<std::string> tos;
        for (const Placement &p : plan.places) {
            tos.push_back(p.to);
        }
        if (const std::string why = conflict(registry, m.id, m.kind, tos, plan.replaced); !why.empty()) {
            return refuse(why);
        }
        const Entry *old = find(registry, m.id);
        if (!old) {
            plan.action = Action::Install;
            return plan;
        }
        plan.old_version = old->version;
        const int c = compare_versions(m.version, old->version);
        if (c > 0) {
            plan.action = Action::Upgrade;
        }
        else if (c == 0) {
            plan.action = options.reinstall ? Action::Reinstall : Action::AlreadyInstalled;
            if (!options.reinstall) {
                plan.reason = "version " + m.version + " is already installed";
            }
        }
        else if (options.allow_downgrade) {
            plan.action = Action::Downgrade;
        }
        else {
            return refuse("version " + old->version + " is installed, which is newer than " + m.version);
        }
        return plan;
    }

    const char *outcome_name(Outcome outcome) {
        switch (outcome) {
            case Outcome::Installed: return "installed";
            case Outcome::Upgraded: return "upgraded";
            case Outcome::Reinstalled: return "reinstalled";
            case Outcome::Downgraded: return "downgraded";
            case Outcome::AlreadyInstalled: return "already-installed";
            case Outcome::Refused: return "refused";
            case Outcome::Failed: return "failed";
        }
        return "failed";
    }
}
