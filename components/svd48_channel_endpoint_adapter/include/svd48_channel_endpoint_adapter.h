#ifndef SVD48_CHANNEL_ENDPOINT_ADAPTER_H
#define SVD48_CHANNEL_ENDPOINT_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

#include "robot_capabilities.h"
#include "svd48_device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t device_id;
    uint8_t device_address;
    svd48_channel_id_t channel;
    svd48_channel_snapshot_t observation;
    svd48_channel_health_t health;
} svd48_endpoint_diagnostics_t;

typedef struct {
    svd48_channel_t *channel;
    uint32_t configured_capabilities;
    robot_velocity_rpm_port_t velocity;
    robot_stoppable_port_t stoppable;
    robot_endpoint_t endpoint;
} svd48_channel_endpoint_adapter_t;

bool svd48_channel_endpoint_adapter_init(
    svd48_channel_endpoint_adapter_t *adapter,
    svd48_channel_t *channel,
    robot_endpoint_id_t endpoint_id,
    const char *name,
    robot_endpoint_criticality_t criticality,
    uint32_t capabilities,
    int16_t min_rpm,
    int16_t max_rpm);
void svd48_channel_endpoint_adapter_deinit(
    svd48_channel_endpoint_adapter_t *adapter);
bool svd48_channel_endpoint_adapter_get_diagnostics(
    svd48_channel_endpoint_adapter_t *adapter,
    svd48_endpoint_diagnostics_t *diagnostics);

#ifdef __cplusplus
}
#endif

#endif
