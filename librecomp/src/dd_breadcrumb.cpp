// [wcw] A last event to the local Datadog Agent when the game dies (aki-roadmap#125). See dd_breadcrumb.hpp.

#include "librecomp/dd_breadcrumb.hpp"

#include <cstring>

namespace recomp {
    namespace dd {
        bool switch_on(const char* value) {
            return (value != nullptr) && (value[0] != '\0') && (std::strcmp(value, "0") != 0);
        }

        std::vector<std::string> parse_tags(const char* value) {
            std::vector<std::string> tags;
            if (value == nullptr) {
                return tags;
            }

            const std::string list(value);
            size_t start = 0;
            while (start <= list.size()) {
                size_t end = list.find(',', start);
                if (end == std::string::npos) {
                    end = list.size();
                }

                std::string t = list.substr(start, end - start);
                const size_t first = t.find_first_not_of(" \t");
                const size_t last = t.find_last_not_of(" \t");
                t = (first == std::string::npos) ? std::string() : t.substr(first, last - first + 1);
                if (!t.empty()) {
                    tags.push_back(t);
                }

                start = end + 1;
            }

            return tags;
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
            if (!game.empty()) {
                tags.push_back("game:" + game);
            }

            tags.push_back("kind:fatal");
            for (const std::string& t : parse_tags(extra_tags)) {
                const std::string key = t.substr(0, t.find(':'));
                bool have = false;
                for (const std::string& u : tags) {
                    have = have || (u.substr(0, u.find(':')) == key);
                }

                if (!have) {
                    tags.push_back(t);
                }
            }

            return tags;
        }
    }
}
