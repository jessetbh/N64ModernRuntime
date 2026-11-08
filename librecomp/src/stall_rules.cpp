#include <cstdlib>
#include <iterator>

#include "librecomp/stall_rules.hpp"

namespace recomp::stall {
    Event Detector::update(double now_s, uint64_t frames) {
        if (!started || frames != last_frames) {
            const bool was_dumping = stall_dumps > 0;
            // A counter still at its start value hasn't begun: boot waits aren't stalls.
            started = started || (frames != last_frames);
            last_frames = frames;
            last_change_s = now_s;
            stall_dumps = 0;
            if (was_dumping) {
                return Event::Resumed; // stalled_for() still holds the stall's length
            }
            stalled_s = 0;
            return Event::None;
        }

        stalled_s = now_s - last_change_s;
        if ((stall_dumps >= int(std::size(DumpMultiples))) || (run_dumps >= MaxDumpsPerRun)) {
            return Event::None;
        }
        if (stalled_s >= first_dump_s * DumpMultiples[stall_dumps]) {
            stall_dumps++;
            run_dumps++;
            return Event::Dump;
        }
        return Event::None;
    }

    double parse_threshold(const char *env) {
        if (env == nullptr) {
            return 0;
        }
        const double seconds = atof(env);
        return seconds > 0 ? seconds : 0;
    }
}
