#ifndef __ULTRAMODERN_PERF_HPP__
#define __ULTRAMODERN_PERF_HPP__

#include <chrono>
#include <cstdint>

// [wcw perf] CPU frame-phase timings (WCW_PERF_LOG=1, aki-roadmap#82). ultramodern times the game-side phases and hands
// them to a sink the renderer installs (RecompFrontend sets rt64's, which writes wcw_cpu_phases.csv). With no sink set,
// each timing point costs one atomic load. Phases reported:
//   game_frame  time from one osViSwapBuffer to the next (one game frame)
//   game_busy   the part of that frame the game's threads ran (the frame minus the idle thread's waits)
//   rsp_audio   an audio RSP task (rsp_task: any other non-graphics task)
//   dl_submit   handing a display list to the renderer (rt64 parses it into a workload)
namespace ultramodern::perf {
    // phase is a static string; start and end are steady_clock times in nanoseconds since its epoch.
    using phase_sink_t = void (*)(const char* phase, int64_t start_ns, int64_t end_ns);

    void set_phase_sink(phase_sink_t sink);
    bool enabled();
    void phase(const char* name, int64_t start_ns, int64_t end_ns);

    inline int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // The idle thread's waits for external messages (pause_self): time the game had nothing to run.
    void add_game_idle(int64_t ns);

    // Called at each osViSwapBuffer: reports game_frame and game_busy for the frame that just ended.
    void game_frame_boundary();

    // The part of a frame the game ran, given the frame's length and the idle time inside it.
    int64_t busy_ns(int64_t frame_ns, int64_t idle_ns);
}

#endif
