//
// N64ModernRuntime (librecomp)
//

// [wcw] aki-roadmap#324: a conversion's GameShark codes run once per VI retrace, as Project64 applies cheats on every VI
// interrupt, not only once per game frame. Packs test exact values of counters that move every retrace (WWF Legends'
// neon menu stars: the retrace counter's low byte at 02, 06, ... 22, reset at 24); a game that makes a frame every
// other retrace showed the engine every second value, and half the sessions never matched (no animation, the base
// texture all session). Pure logic, unit tested; the hook itself is in ultramodern's osRecvMesg and conversion.cpp.

#ifndef __LIBRECOMP_RETRACE_CODES_RULES_HPP__
#define __LIBRECOMP_RETRACE_CODES_RULES_HPP__

#include <cstdint>

namespace recomp::retrace_codes {
    // A message a game thread received is the VI retrace event: the queue and message osViSetEvent registered. No
    // registration (queue 0) is never a retrace.
    bool is_retrace(uint32_t queue, uint32_t message, uint32_t vi_queue, uint32_t vi_message);

    // The frame hook (the cheats mod's call) runs the codes only while retraces aren't: so a game that never registers a
    // VI event (or stops) still gets them once a frame, and one that does gets them once a retrace, not twice.
    constexpr uint64_t RetraceQuietMs = 250;

    class FrameGate {
    public:
        void on_retrace(uint64_t now_ms) { last_retrace_ms = now_ms; retraces++; }
        bool frame_should_apply(uint64_t now_ms) const {
            return (last_retrace_ms == 0) || (now_ms < last_retrace_ms) || (now_ms - last_retrace_ms > RetraceQuietMs);
        }
        uint64_t retrace_count() const { return retraces; }

    private:
        uint64_t last_retrace_ms = 0;
        uint64_t retraces = 0;
    };
}

#endif
