#ifndef __RECOMP_DD_BREADCRUMB_HPP__
#define __RECOMP_DD_BREADCRUMB_HPP__

// [wcw] A last event to the local Datadog Agent when the game dies (aki-roadmap#125): a switch-case out of bounds, a
// break, a fatal error box. Only with WCW_DD_METRICS=1, the same switch as rt64's aki.game.* metrics, and only to
// 127.0.0.1 (WCW_DD_PORT, default 8125), so nothing leaves the PC except through the owner's own Agent. The event uses
// rt64's DogStatsD format and source (aki-game), so it lands in the same event stream as the play sessions. The
// formatting is pure and unit tested in tests/wcw/dd_breadcrumb_tests.cpp.

#include <string>
#include <vector>

namespace recomp {
    namespace dd {
        // Whether an environment value turns a switch on: set, not empty and not "0" (rt64's envSwitchOn).
        bool switch_on(const char* value);

        // Tags from WCW_DD_TAGS ("a:b,c:d"): trimmed, empty ones dropped.
        std::vector<std::string> parse_tags(const char* value);

        // A DogStatsD event: _e{title length,text length}:title|text|k:key|s:aki-game|t:alert|#tags. A newline in the
        // title becomes a space, in the text \n; carriage returns go.
        std::string event_line(const std::string& title, const std::string& text, const std::vector<std::string>& tags,
            const std::string& alert_type, const std::string& aggregation_key);

        // The tags of a fatal event: game:<WCW_DD_GAME, else the running game's id>, kind:fatal, then WCW_DD_TAGS
        // (a key already given is not repeated).
        std::vector<std::string> fatal_tags(const std::string& game, const char* extra_tags);

        // The event title as rt64 writes its play-session events: "aki-game <game>: <what>" ("aki-game: <what>" without
        // a game), so the breadcrumb sorts with the session in the event stream.
        std::string fatal_title(const std::string& game, const std::string& what);

        // Sends one error event "<title>" with text to the local Agent, if WCW_DD_METRICS is on; never throws, never
        // blocks for long (one UDP datagram). Safe to call on the way out of the process.
        void fatal(const std::string& title, const std::string& text);
    }
}

#endif
