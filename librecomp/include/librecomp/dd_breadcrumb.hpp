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

        // One tag as rt64's ddTag: key and value lower case, other characters as '_' (key: a-z 0-9 _ - . /, value
        // also ':'), a key not starting with a letter gets "t_", at most 200 characters; empty for an empty value.
        std::string tag(const std::string& key, const std::string& value);

        // Tags from WCW_DD_TAGS ("a:b,c:d") as rt64's ddParseTags: entries without ':' dropped, each through tag(),
        // exact duplicates once.
        std::vector<std::string> parse_tags(const char* value);

        // The game name as rt64's ddGameFromExe: the exe's file name, lower case, without .exe and "recompiled"
        // (NoMercyRecompiled.exe: nomercy; WCWRecompiled.exe: wt).
        std::string game_from_exe(const std::string& exe_name);

        // WCW_DD_GAME when set and not empty, else game_from_exe (rt64's rule).
        std::string fatal_game(const char* env_game, const std::string& exe_name);

        // A DogStatsD event: _e{title length,text length}:title|text|k:key|s:aki-game|t:alert|#tags. A newline in the
        // title becomes a space, in the text \n; carriage returns go.
        std::string event_line(const std::string& title, const std::string& text, const std::vector<std::string>& tags,
            const std::string& alert_type, const std::string& aggregation_key);

        // The tags of a fatal event: game:<game>, kind:fatal, then WCW_DD_TAGS (not game or kind).
        std::vector<std::string> fatal_tags(const std::string& game, const char* extra_tags);

        // The event title as rt64 writes its play-session events: "aki-game <game>: <what>" ("aki-game: <what>" without
        // a game), so the breadcrumb sorts with the session in the event stream.
        std::string fatal_title(const std::string& game, const std::string& what);

        // Sends one error event "aki-game <game>: <title>" with text to the local Agent, if WCW_DD_METRICS is on. Never
        // throws (it runs on the way out of the process, from extern "C" code), never blocks for long (one UDP
        // datagram), and reads nothing the shutdown may have reset (the game name comes from the exe, not the
        // running game).
        void fatal(const std::string& title, const std::string& text) noexcept;
    }
}

#endif
