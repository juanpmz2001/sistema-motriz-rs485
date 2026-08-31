#ifndef CONTROL_LAN_SEQUENCE_MODEL_H
#define CONTROL_LAN_SEQUENCE_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pure sequence arithmetic used by control_lan after stream identity has been
 * checked. It has no timeout or safety policy: a gap is diagnostics evidence,
 * while command_authority still owns the existing TTL deadline. */
bool control_lan_sequence_is_increasing(uint64_t previous, uint64_t incoming);
uint32_t control_lan_sequence_gap_count(uint64_t previous, uint64_t incoming);

#ifdef __cplusplus
}
#endif

#endif /* CONTROL_LAN_SEQUENCE_MODEL_H */
