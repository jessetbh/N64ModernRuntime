//
// N64ModernRuntime (librecomp)
//

// [wcw] When the stall watchdog dumps (aki-roadmap#187). Pure logic, unit tested; the dump itself is stall_watchdog.cpp.

#ifndef __LIBRECOMP_STALL_RULES_HPP__
#define __LIBRECOMP_STALL_RULES_HPP__

#include <cstdint>

namespace recomp::stall {
    // Dumps per stall, at this many times the first threshold: a thread that's moved on between them isn't the cause.
    constexpr double DumpMultiples[] = { 1.0, 3.0, 12.0 };
    constexpr int MaxDumpsPerRun = 12; // a game with long legitimate waits can't fill the log

    enum class Event {
        None,
        Dump,    // stalled_for() has reached the next dump time
        Resumed, // frames came back after at least one dump: that stall was a wait, not a freeze
    };

    class Detector {
    public:
        explicit Detector(double first_dump_s) : first_dump_s(first_dump_s) {}

        // frames: a counter that rises while the game makes frames. Nothing counts until it has moved once (boot).
        Event update(double now_s, uint64_t frames);

        double stalled_for() const { return stalled_s; }
        int dumps_this_stall() const { return stall_dumps; }

    private:
        double first_dump_s;
        double last_change_s = 0;
        double stalled_s = 0;
        uint64_t last_frames = 0;
        bool started = false;
        int stall_dumps = 0;
        int run_dumps = 0;
    };

    // WCW_STALL_DUMP's value: seconds to the first dump, 0 for off (unset, unparsable, zero or negative).
    double parse_threshold(const char *env);

    // Runtime half (stall_watchdog.cpp): starts the watchdog thread if WCW_STALL_DUMP is set.
    void start_watchdog(uint8_t *rdram);
}

#endif
