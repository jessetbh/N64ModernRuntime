#include "librecomp/bundled_packs.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "json/json.hpp"

namespace recomp::mods::bundled_packs {
    bool is_text_file(const std::string &path) {
        // The extension as Python's os.path.splitext sees it: leading dots of the file name don't start one.
        const size_t slash = path.find_last_of('/');
        std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
        const size_t first = name.find_first_not_of('.');
        if (first == std::string::npos) {
            return false;
        }
        const size_t dot = name.find_last_of('.');
        if (dot == std::string::npos || dot < first) {
            return false;
        }
        std::string ext = name.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return ext == ".json" || ext == ".md" || ext == ".txt";
    }

    uint64_t pack_hash(std::vector<std::pair<std::string, std::string>> files) {
        std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        uint64_t h = 14695981039346656037ULL;
        auto feed = [&h](unsigned char byte) {
            h ^= byte;
            h *= 1099511628211ULL;
        };
        for (const auto &file : files) {
            for (unsigned char c : file.first) {
                feed(c);
            }
            feed(0);
            const bool text = is_text_file(file.first);
            const std::string &data = file.second;
            for (size_t i = 0; i < data.size(); i++) {
                if (text && data[i] == '\r' && (i + 1) < data.size() && data[i + 1] == '\n') {
                    continue;   // CRLF counts as LF
                }
                feed(static_cast<unsigned char>(data[i]));
            }
            feed(0);
        }
        return h;
    }

    std::string hash_hex(uint64_t hash) {
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
        return buf;
    }

    int compare_versions(const std::string &a, const std::string &b) {
        size_t i = 0, j = 0;
        while (i < a.size() || j < b.size()) {
            auto part = [](const std::string &s, size_t &pos) {
                unsigned long long v = 0;
                bool digits = true;
                const size_t start = pos;
                while (pos < s.size() && s[pos] != '.') {
                    if (s[pos] < '0' || s[pos] > '9') {
                        digits = false;
                    }
                    else if (digits && v < 1000000000ULL) {
                        v = v * 10 + unsigned(s[pos] - '0');
                    }
                    pos++;
                }
                if (pos < s.size()) {
                    pos++;   // the '.'
                }
                return (digits && pos > start) ? v : 0ULL;
            };
            const unsigned long long x = part(a, i);
            const unsigned long long y = part(b, j);
            if (x != y) {
                return (x < y) ? -1 : 1;
            }
        }
        return 0;
    }

    std::string pack_version(const std::string &pack_json_text) {
        const nlohmann::json j = nlohmann::json::parse(pack_json_text, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            return {};
        }
        const auto it = j.find("version");
        return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
    }

    Action decide(const Installed &installed, uint64_t bundled_hash, const std::string &bundled_version,
        const std::vector<uint64_t> &earlier_hashes) {
        if (!installed.present) {
            return installed.recorded ? Action::Skip : Action::Install;
        }
        if (installed.hash == bundled_hash) {
            return Action::Keep;
        }
        const bool earlier_bundle = std::find(earlier_hashes.begin(), earlier_hashes.end(), installed.hash) != earlier_hashes.end();
        if (earlier_bundle && compare_versions(installed.version, bundled_version) < 0) {
            return Action::Replace;
        }
        return Action::Keep;
    }

    std::set<std::string> parse_record(const std::string &text) {
        std::set<std::string> out;
        size_t start = 0;
        while (start <= text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) {
                end = text.size();
            }
            std::string line = text.substr(start, end - start);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
                line.pop_back();
            }
            if (!line.empty()) {
                out.insert(line);
            }
            start = end + 1;
        }
        return out;
    }

    std::string format_record(const std::set<std::string> &names) {
        std::string out;
        for (const std::string &n : names) {
            out += n;
            out += '\n';
        }
        return out;
    }

    std::map<std::string, std::vector<uint64_t>> parse_history(const std::string &json_text) {
        std::map<std::string, std::vector<uint64_t>> out;
        const nlohmann::json j = nlohmann::json::parse(json_text, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            return out;
        }
        const auto prev = j.find("previous");
        if (prev == j.end() || !prev->is_object()) {
            return out;
        }
        for (const auto &[id, list] : prev->items()) {
            if (!list.is_array()) {
                continue;
            }
            for (const auto &h : list) {
                if (!h.is_string()) {
                    continue;
                }
                const std::string s = h.get<std::string>();
                if (s.size() != 16 || s.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
                    continue;
                }
                out[id].push_back(std::stoull(s, nullptr, 16));
            }
        }
        return out;
    }
}
