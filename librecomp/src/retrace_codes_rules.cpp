// [wcw] aki-roadmap#324: see retrace_codes_rules.hpp.
#include "librecomp/retrace_codes_rules.hpp"

namespace recomp::retrace_codes {
    bool is_retrace(uint32_t queue, uint32_t message, uint32_t vi_queue, uint32_t vi_message) {
        return (vi_queue != 0) && (queue == vi_queue) && (message == vi_message);
    }
}
