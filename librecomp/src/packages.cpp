// [wcw] The file work around the package rules (see packages.hpp): hashing installed paths and the registry file.

#include "librecomp/packages.hpp"

#include <fstream>
#include <iterator>
#include <system_error>

#include "librecomp/bundled_packs.hpp"

namespace recomp::packages {
    namespace fs = std::filesystem;

    static bool read_file(const fs::path &path, std::string &out) {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            return false;
        }
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return !f.bad();
    }

    std::optional<uint64_t> hash_path(const fs::path &path) {
        std::error_code ec;
        const fs::file_status st = fs::status(path, ec);
        if (ec || !fs::exists(st)) {
            return std::nullopt;
        }
        std::vector<std::pair<std::string, std::string>> files;
        if (fs::is_regular_file(st)) {
            std::string data;
            if (!read_file(path, data)) {
                return std::nullopt;
            }
            const std::u8string name = path.filename().generic_u8string();
            files.emplace_back(std::string(name.begin(), name.end()), std::move(data));
        }
        else if (fs::is_directory(st)) {
            for (fs::recursive_directory_iterator it(path, ec), end; it != end; it.increment(ec)) {
                if (ec) {
                    return std::nullopt;   // a folder we can't walk can't be judged unedited
                }
                std::error_code file_ec;
                if (!it->is_regular_file(file_ec)) {
                    continue;
                }
                std::string data;
                if (!read_file(it->path(), data)) {
                    return std::nullopt;
                }
                const std::u8string rel = it->path().lexically_relative(path).generic_u8string();
                files.emplace_back(std::string(rel.begin(), rel.end()), std::move(data));
            }
            if (ec) {
                return std::nullopt;
            }
        }
        else {
            return std::nullopt;
        }
        return recomp::mods::bundled_packs::pack_hash(std::move(files));
    }

    bool load_registry(const fs::path &data_dir, Registry &out) {
        out = Registry{};
        const fs::path file = data_dir / RegistryFile;
        std::error_code ec;
        if (!fs::exists(file, ec)) {
            return !ec;
        }
        std::string text;
        if (!read_file(file, text)) {
            return false;
        }
        return parse_registry(text, out);
    }

    bool save_registry(const fs::path &data_dir, const Registry &registry) {
        const fs::path file = data_dir / RegistryFile;
        fs::path temp = file;
        temp += ".tmp";
        {
            std::ofstream f(temp, std::ios::binary | std::ios::trunc);
            if (!f) {
                return false;
            }
            f << format_registry(registry);
            f.flush();
            if (!f) {
                f.close();
                std::error_code cleanup;
                fs::remove(temp, cleanup);
                return false;
            }
        }
        std::error_code ec;
        fs::rename(temp, file, ec);   // replaces an existing file atomically on NTFS and POSIX
        if (ec) {
            std::error_code cleanup;
            fs::remove(temp, cleanup);
            return false;
        }
        return true;
    }
}
