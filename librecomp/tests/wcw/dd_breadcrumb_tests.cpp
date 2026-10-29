//
// librecomp
//

// [wcw] Unit tests for librecomp/dd_breadcrumb (the last event to the local Datadog Agent when the game dies,
// aki-roadmap#125). Built and run as part of every librecomp build (LIBRECOMP_WCW_TESTS); a failure fails the build.

#include "librecomp/dd_breadcrumb.hpp"

#include <string>
#include <vector>

#include "wcw_test.h"

using namespace recomp::dd;

TEST_CASE("dd breadcrumb: WCW_DD_METRICS switches it on like rt64's metrics (set, not empty, not 0)") {
    CHECK(!switch_on(nullptr));
    CHECK(!switch_on(""));
    CHECK(!switch_on("0"));
    CHECK(switch_on("1"));
    CHECK(switch_on("yes"));
}

TEST_CASE("dd breadcrumb: the event line matches rt64's DogStatsD format (lengths, source, alert, tags)") {
    const std::string line = event_line("switch-case out of bounds", "in func_80001234 at 0x80001240\nexit 1",
        { "game:nomercy", "kind:fatal" }, "error", "fatal");
    // The text's newline is escaped: 38 characters.
    CHECK(line == "_e{25,38}:switch-case out of bounds|in func_80001234 at 0x80001240\\nexit 1|k:fatal|s:aki-game|t:error"
                  "|#game:nomercy,kind:fatal");
    CHECK(event_line("a\nb", "x\r", {}, "", "") == "_e{3,1}:a b|x|s:aki-game");
}

TEST_CASE("dd breadcrumb: tags: the game, kind:fatal, then WCW_DD_TAGS without repeating a key") {
    const std::vector<std::string> tags = fatal_tags("nomercy", " conversion:nmp , game:other,,owner:jesse ");
    CHECK(tags.size() == 4);
    CHECK(tags[0] == "game:nomercy");
    CHECK(tags[1] == "kind:fatal");
    CHECK(tags[2] == "conversion:nmp");
    CHECK(tags[3] == "owner:jesse");
    CHECK(fatal_tags("", nullptr).size() == 1);
    CHECK(parse_tags("a, b ,,c").size() == 3);
}
