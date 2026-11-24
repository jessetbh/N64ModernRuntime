//
// librecomp
//

// [wcw] Unit tests for librecomp/retrace_codes_rules (aki-roadmap#324): conversion codes once per VI retrace.

#include "librecomp/retrace_codes_rules.hpp"
#include "ultramodern/ultramodern.hpp"

#include "wcw_test.h"

using namespace recomp::retrace_codes;

TEST_CASE("retrace codes: only the registered queue and message are a retrace") {
    // No Mercy registers its scheduler's queue with message 0x29A
    CHECK(is_retrace(0x80057188 + 0x40, 0x29A, 0x80057188 + 0x40, 0x29A));
    CHECK(!is_retrace(0x80057188 + 0x40, 0x29D, 0x80057188 + 0x40, 0x29A));   // the same queue's other message (pre-NMI)
    CHECK(!is_retrace(0x80060000, 0x29A, 0x80057188 + 0x40, 0x29A));          // another queue
    CHECK(!is_retrace(0, 0, 0, 0));                                           // no osViSetEvent yet
    CHECK(!is_retrace(0x80057188, 0, 0, 0));
    // a re-registration (WCW at the end of its attract): the same queue with a new message
    CHECK(!is_retrace(0x800571C8, 0x29A, 0x800571C8, 0x2A0));
    CHECK(is_retrace(0x800571C8, 0x2A0, 0x800571C8, 0x2A0));
    // osRecvMesg calls the same check
    CHECK(ultramodern::is_vi_retrace_message(0x800571C8, 0x2A0, 0x800571C8, 0x2A0) == is_retrace(0x800571C8, 0x2A0, 0x800571C8, 0x2A0));
    CHECK(!ultramodern::is_vi_retrace_message(0, 0, 0, 0));
}

TEST_CASE("retrace codes: the frame hook runs them until retraces do") {
    FrameGate g;
    CHECK(g.retrace_count() == 0);
    CHECK(g.frame_should_apply(0));           // the first install: the first frames apply before any retrace has
    CHECK(g.frame_should_apply(1000));        // nothing from retraces yet: the frame hook keeps the codes running
    g.on_retrace(1000);
    CHECK(!g.frame_should_apply(1010));       // retraces run them: the frame hook doesn't again
    CHECK(!g.frame_should_apply(1000 + RetraceQuietMs));
    CHECK(g.frame_should_apply(1000 + RetraceQuietMs + 1));   // retraces stopped (a game that unregisters): frames again
    g.on_retrace(2000);
    CHECK(!g.frame_should_apply(2016));
    CHECK(g.frame_should_apply(1999));        // a clock that went back doesn't stop the codes
    CHECK(g.retrace_count() == 2);
}

TEST_CASE("retrace codes: a counter that moves 2 per frame is seen at every value once per retrace") {
    // The Legends case: the counter's low byte rises once a retrace; the game makes a frame every other retrace.
    int seen_frame[256] = {}, seen_retrace[256] = {};
    for (int retrace = 0; retrace < 512; retrace++) {
        const int counter = (retrace + 1) & 0xFF;     // an odd phase: the frame hook only ever sees odd values
        seen_retrace[counter] = 1;
        if ((retrace % 2) == 0) seen_frame[counter] = 1;
    }
    int frame_evens = 0, retrace_all = 0;
    for (int v = 0; v < 256; v++) {
        frame_evens += ((v % 2) == 0) && seen_frame[v];
        retrace_all += seen_retrace[v];
    }
    CHECK(frame_evens == 0);       // 02, 06, ... 22 and the reset at 24 never match once a frame
    CHECK(retrace_all == 256);     // once a retrace, every value comes by
}
