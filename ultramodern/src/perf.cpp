#include <algorithm>
#include <atomic>

#include "ultramodern/perf.hpp"

// [wcw perf] See perf.hpp.

static std::atomic<ultramodern::perf::phase_sink_t> phase_sink{ nullptr };
static std::atomic<int64_t> game_idle_ns{ 0 };
static std::atomic<int64_t> last_swap_ns{ 0 };

void ultramodern::perf::set_phase_sink(phase_sink_t sink) {
    phase_sink.store(sink, std::memory_order_release);
}

bool ultramodern::perf::enabled() {
    return phase_sink.load(std::memory_order_relaxed) != nullptr;
}

void ultramodern::perf::phase(const char* name, int64_t start_ns, int64_t end_ns) {
    phase_sink_t sink = phase_sink.load(std::memory_order_acquire);
    if (sink != nullptr) {
        sink(name, start_ns, end_ns);
    }
}

void ultramodern::perf::add_game_idle(int64_t ns) {
    game_idle_ns.fetch_add(ns, std::memory_order_relaxed);
}

int64_t ultramodern::perf::busy_ns(int64_t frame_ns, int64_t idle_ns) {
    return std::clamp<int64_t>(frame_ns - idle_ns, 0, std::max<int64_t>(frame_ns, 0));
}

void ultramodern::perf::game_frame_boundary() {
    const int64_t now = now_ns();
    const int64_t idle = game_idle_ns.exchange(0, std::memory_order_relaxed);
    const int64_t last = last_swap_ns.exchange(now, std::memory_order_relaxed);
    if (last == 0) {
        return;
    }

    const int64_t frame = now - last;
    phase("game_frame", last, now);
    phase("game_busy", last, last + busy_ns(frame, idle));
}
