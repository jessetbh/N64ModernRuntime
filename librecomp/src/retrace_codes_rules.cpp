// [wcw] aki-roadmap#324: see retrace_codes_rules.hpp.
#include "librecomp/retrace_codes_rules.hpp"
#include "ultramodern/ultramodern.hpp"

namespace recomp::retrace_codes {
    bool is_retrace(uint32_t queue, uint32_t message, uint32_t vi_queue, uint32_t vi_message) {
        return ultramodern::is_vi_retrace_message(queue, message, vi_queue, vi_message);   // the one osRecvMesg uses
    }
}
