#include "control_lan_sequence_model.h"

#include <limits.h>

bool control_lan_sequence_is_increasing(uint64_t previous, uint64_t incoming)
{
    return incoming > previous;
}

uint32_t control_lan_sequence_gap_count(uint64_t previous, uint64_t incoming)
{
    if (!control_lan_sequence_is_increasing(previous, incoming) ||
        incoming - previous <= UINT64_C(1)) {
        return 0U;
    }
    const uint64_t gap = incoming - previous - UINT64_C(1);
    return gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
}
