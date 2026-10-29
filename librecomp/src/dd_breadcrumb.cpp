// [wcw] A last event to the local Datadog Agent when the game dies (aki-roadmap#125). See dd_breadcrumb.hpp.
// The tag and game-name rules are rt64's (common/rt64_wcw_logic.cpp: ddTag, ddParseTags, ddGameFromExe), so the
// breadcrumb carries the same tags as the play session it ends.

#include "librecomp/dd_breadcrumb.hpp"

#include <algorithm>
#include <cstring>

namespace recomp {
    namespace dd {
        namespace {
            std::string lower(std::string s) {
                for (char& c : s) {
                    c = ((c >= 'A') && (c <= 'Z')) ? char(c - 'A' + 'a') : c;
                }

                return s;
            }

            bool ends_with(const std::string& s, const std::string& suffix) {
                return (s.size() >= suffix.size()) && (s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0);
            }

            std::string trim(const std::string& s, const char* chars) {
                const size_t a = s.find_first_not_of(chars);
                if (a == std::string::npos) {
                    return std::string();
                }

                return s.substr(a, s.find_last_not_of(chars) - a + 1);
            }

            std::string sanitize(const std::string& s, const char* allowed_punct) {
                std::string out;
                for (char c : lower(s)) {
                    const bool ok = ((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')) || (c == '_') ||
                        ((c != '\0') && (std::strchr(allowed_punct, c) != nullptr));
                    const char r = ok ? c : '_';
                    if ((r == '_') && !out.empty() && (out.back() == '_')) {
                        continue;
                    }

                    out.push_back(r);
                }

                return out;
            }
        }

        bool switch_on(const char* value) {
            return (value != nullptr) && (value[0] != '\0') && (std::strcmp(value, "0") != 0);
        }

        std::string tag(const std::string& key, const std::string& value) {
            std::string v = trim(value, " \t\r\n");
            if (v.empty()) {
                return std::string();
            }

            std::string k = trim(sanitize(key, "-./"), "_");
            if (k.empty() || (k[0] < 'a') || (k[0] > 'z')) {
                k = "t_" + k;
            }

            v = trim(sanitize(v, "-:./"), "_");
            if (v.empty()) {
                return std::string();
            }

            std::string t = k + ":" + v;
            if (t.size() > 200) {
                t.resize(200);
            }

            return t;
        }

        std::vector<std::string> parse_tags(const char* value) {
            std::vector<std::string> tags;
            if (value == nullptr) {
                return tags;
            }

            const std::string all(value);
            size_t pos = 0;
            while (pos <= all.size()) {
                size_t end = all.find(',', pos);
                if (end == std::string::npos) {
                    end = all.size();
                }

                const std::string entry = all.substr(pos, end - pos);
                const size_t colon = entry.find(':');
                if (colon != std::string::npos) {
                    std::string t = tag(entry.substr(0, colon), entry.substr(colon + 1));
                    if (!t.empty() && (std::find(tags.begin(), tags.end(), t) == tags.end())) {
                        tags.push_back(std::move(t));
                    }
                }

                pos = end + 1;
            }

            return tags;
        }

        std::string game_from_exe(const std::string& exe_name) {
            const size_t slash = exe_name.find_last_of("/\\");
            std::string name = lower((slash == std::string::npos) ? exe_name : exe_name.substr(slash + 1));
            if (ends_with(name, ".exe")) {
                name.resize(name.size() - 4);
            }

            if (ends_with(name, "recompiled")) {
                name.resize(name.size() - 10);
            }

            return (name == "wcw") ? std::string("wt") : name;
        }

        std::string fatal_game(const char* env_game, const std::string& exe_name) {
            return ((env_game != nullptr) && (env_game[0] != '\0')) ? std::string(env_game) : game_from_exe(exe_name);
        }

        std::string event_line(const std::string& title, const std::string& text, const std::vector<std::string>& tags,
            const std::string& alert_type, const std::string& aggregation_key)
        {
            std::string ti, tx;
            for (char c : title) {
                if (c != '\r') {
                    ti.push_back((c == '\n') ? ' ' : c);
                }
            }

            for (char c : text) {
                if (c == '\n') {
                    tx += "\\n";
                }
                else if (c != '\r') {
                    tx.push_back(c);
                }
            }

            std::string line = "_e{" + std::to_string(ti.size()) + "," + std::to_string(tx.size()) + "}:" + ti + "|" + tx;
            if (!aggregation_key.empty()) {
                line += "|k:" + aggregation_key;
            }

            line += "|s:aki-game";
            if (!alert_type.empty()) {
                line += "|t:" + alert_type;
            }

            std::string joined;
            for (const std::string& t : tags) {
                if (!t.empty()) {
                    joined += (joined.empty() ? "" : ",") + t;
                }
            }

            if (!joined.empty()) {
                line += "|#" + joined;
            }

            return line;
        }

        std::string fatal_title(const std::string& game, const std::string& what) {
            return "aki-game" + (game.empty() ? std::string() : " " + game) + ": " + what;
        }

        std::vector<std::string> fatal_tags(const std::string& game, const char* extra_tags) {
            std::vector<std::string> tags;
            const std::string g = tag("game", game);
            if (!g.empty()) {
                tags.push_back(g);
            }

            tags.push_back("kind:fatal");
            // WCW_DD_TAGS can't override the breadcrumb's own keys (rt64 skips its built-in keys the same way).
            for (const std::string& t : parse_tags(extra_tags)) {
                const std::string key = t.substr(0, t.find(':'));
                if ((key != "game") && (key != "kind") && (std::find(tags.begin(), tags.end(), t) == tags.end())) {
                    tags.push_back(t);
                }
            }

            return tags;
        }
    }
}
