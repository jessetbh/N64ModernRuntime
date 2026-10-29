// [wcw] The file work around the package rules (see packages.hpp): hashing installed paths and the registry file.

#include "librecomp/packages.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "librecomp/bundled_packs.hpp"

namespace recomp::packages {
    namespace fs = std::filesystem;

    namespace {
        // pack_hash, fed a piece at a time: the same bytes in the same order give the same hash.
        struct StreamHash {
            uint64_t h = 14695981039346656037ULL;
            void byte(unsigned char b) {
                h ^= b;
                h *= 1099511628211ULL;
            }
        };

        FILE *open_file(const fs::path &path, const char *mode) {
#ifdef _WIN32
            const std::wstring m(mode, mode + std::char_traits<char>::length(mode));
            FILE *f = nullptr;
            return (_wfopen_s(&f, path.c_str(), m.c_str()) == 0) ? f : nullptr;
#else
            return std::fopen(path.c_str(), mode);
#endif
        }

        // Feeds one file's contents. Text files drop the CR of each CRLF, also when the pair spans two pieces.
        bool feed_file(StreamHash &hash, const fs::path &path, bool text, size_t chunk_size) {
            FILE *f = open_file(path, "rb");
            if (!f) {
                return false;
            }
            std::vector<unsigned char> buf(std::max<size_t>(chunk_size, 1));
            bool pending_cr = false;
            bool ok = true;
            while (true) {
                const size_t n = std::fread(buf.data(), 1, buf.size(), f);
                for (size_t i = 0; i < n; i++) {
                    const unsigned char c = buf[i];
                    if (pending_cr) {
                        pending_cr = false;
                        if (c != '\n') {
                            hash.byte('\r');
                        }
                    }
                    if (text && c == '\r') {
                        pending_cr = true;
                        continue;
                    }
                    hash.byte(c);
                }
                if (n < buf.size()) {
                    ok = !std::ferror(f);
                    break;
                }
            }
            if (pending_cr) {
                hash.byte('\r');
            }
            std::fclose(f);
            return ok;
        }

        bool read_text(const fs::path &path, std::string &out) {
            FILE *f = open_file(path, "rb");
            if (!f) {
                return false;
            }
            out.clear();
            char buf[65536];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
                out.append(buf, n);
            }
            const bool ok = !std::ferror(f);
            std::fclose(f);
            return ok;
        }
    }

    std::optional<uint64_t> hash_path(const fs::path &path, size_t chunk_size) {
        std::error_code ec;
        const fs::file_status st = fs::status(path, ec);
        if (ec || !fs::exists(st)) {
            return std::nullopt;
        }
        // (relative '/' path, file) pairs, sorted by path like pack_hash sorts them.
        std::vector<std::pair<std::string, fs::path>> files;
        if (fs::is_regular_file(st)) {
            const std::u8string name = path.filename().generic_u8string();
            files.emplace_back(std::string(name.begin(), name.end()), path);
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
                const std::u8string rel = it->path().lexically_relative(path).generic_u8string();
                files.emplace_back(std::string(rel.begin(), rel.end()), it->path());
            }
            if (ec) {
                return std::nullopt;
            }
        }
        else {
            return std::nullopt;
        }
        std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        StreamHash hash;
        for (const auto &[rel, file] : files) {
            for (unsigned char c : rel) {
                hash.byte(c);
            }
            hash.byte(0);
            if (!feed_file(hash, file, recomp::mods::bundled_packs::is_text_file(rel), chunk_size)) {
                return std::nullopt;
            }
            hash.byte(0);
        }
        return hash.h;
    }

    RegistryStatus load_registry(const fs::path &data_dir, Registry &out) {
        out = Registry{};
        const fs::path file = data_dir / RegistryFile;
        std::error_code ec;
        const bool present = fs::exists(file, ec);
        if (ec) {
            return RegistryStatus::IoError;
        }
        if (!present) {
            return RegistryStatus::Ok;
        }
        std::string text;
        if (!read_text(file, text)) {
            return RegistryStatus::IoError;
        }
        return parse_registry(text, out);
    }

    bool save_registry(const fs::path &data_dir, const Registry &registry) {
        const fs::path file = data_dir / RegistryFile;
        fs::path temp = file;
        temp += ".tmp";
        const std::string text = format_registry(registry);
        FILE *f = open_file(temp, "wb");
        if (!f) {
            return false;
        }
        bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size() && std::fflush(f) == 0;
#ifdef _WIN32
        ok = ok && _commit(_fileno(f)) == 0;   // on disk before the rename makes it the registry
#else
        ok = ok && fsync(fileno(f)) == 0;
#endif
        ok = (std::fclose(f) == 0) && ok;
        std::error_code ec;
        if (ok) {
            // fs::rename replaces an existing file (MoveFileExW with REPLACE_EXISTING on Windows). Another program
            // reading the registry at that moment makes it fail with a sharing violation: try again briefly.
            for (int attempt = 0; attempt < 10; attempt++) {
                ec.clear();
                fs::rename(temp, file, ec);
                if (!ec) {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        std::error_code cleanup;
        fs::remove(temp, cleanup);
        return false;
    }
}
