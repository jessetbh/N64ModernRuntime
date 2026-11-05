//
// librecomp
//

// [wcw] Unit tests for librecomp/stall_rules (aki-roadmap#187): when the stall watchdog dumps.

#include "librecomp/stall_rules.hpp"

#include "wcw_test.h"

using namespace recomp::stall;

TEST_CASE("stall: boot waits before the first frame don't count") {
    Detector d{ 5.0 };
    CHECK(d.update(0.0, 0) == Event::None);
    CHECK(d.update(100.0, 0) == Event::None);
    CHECK(d.stalled_for() == 0.0);
}

TEST_CASE("stall: dumps at 1x, 3x and 12x the threshold, then stays quiet") {
    Detector d{ 5.0 };
    d.update(0.0, 1);
    d.update(1.0, 2);
    CHECK(d.update(5.9, 2) == Event::None);
    CHECK(d.update(6.0, 2) == Event::Dump);
    CHECK(d.update(6.5, 2) == Event::None);
    CHECK(d.update(16.0, 2) == Event::Dump);
    CHECK(d.update(61.0, 2) == Event::Dump);
    CHECK(d.dumps_this_stall() == 3);
    CHECK(d.update(1000.0, 2) == Event::None);
    CHECK_NEAR(d.stalled_for(), 999.0, 0.001);
}

TEST_CASE("stall: frames coming back after a dump report the stall's length, and the next stall dumps again") {
    Detector d{ 5.0 };
    d.update(0.0, 1);
    CHECK(d.update(5.0, 1) == Event::Dump);
    CHECK(d.update(7.0, 2) == Event::Resumed);
    CHECK_NEAR(d.stalled_for(), 5.0, 0.001);
    CHECK(d.dumps_this_stall() == 0);
    CHECK(d.update(8.0, 3) == Event::None);
    CHECK(d.stalled_for() == 0.0);
    CHECK(d.update(13.0, 3) == Event::Dump);
}

TEST_CASE("stall: a short wait with no dump isn't reported as resumed") {
    Detector d{ 5.0 };
    d.update(0.0, 1);
    CHECK(d.update(4.0, 1) == Event::None);
    CHECK(d.update(4.5, 2) == Event::None);
}

TEST_CASE("stall: a run's dumps are capped") {
    Detector d{ 1.0 };
    d.update(0.0, 1);
    double now = 0;
    uint64_t frames = 1;
    int dumps = 0;
    for (int stall = 0; stall < 20; stall++) {
        now += 1.0;
        if (d.update(now, frames) == Event::Dump) dumps++;
        frames++;
        now += 0.01;
        d.update(now, frames);
    }
    CHECK(dumps == MaxDumpsPerRun);
}

TEST_CASE("stall: threshold parsing") {
    CHECK(parse_threshold(nullptr) == 0.0);
    CHECK(parse_threshold("") == 0.0);
    CHECK(parse_threshold("junk") == 0.0);
    CHECK(parse_threshold("0") == 0.0);
    CHECK(parse_threshold("-3") == 0.0);
    CHECK_NEAR(parse_threshold("5"), 5.0, 0.001);
    CHECK_NEAR(parse_threshold("0.25"), 0.25, 0.001);
}
