#include "as5600_position_endpoint_adapter.h"

#include <string.h>

static robot_capability_error_t map_result(as5600_device_result_t result)
{
    switch (result) {
    case AS5600_DEVICE_OK:
        return ROBOT_CAP_OK;
    case AS5600_DEVICE_INVALID_ARGUMENT:
        return ROBOT_CAP_INVALID_ARGUMENT;
    case AS5600_DEVICE_NOT_READY:
        return ROBOT_CAP_UNAVAILABLE;
    case AS5600_DEVICE_BUS_BUSY:
    case AS5600_DEVICE_TIMEOUT:
    case AS5600_DEVICE_IO_ERROR:
    case AS5600_DEVICE_BAD_RESPONSE:
    case AS5600_DEVICE_PARTIAL:
    default:
        return ROBOT_CAP_IO_ERROR;
    }
}

static robot_endpoint_health_t map_health(as5600_device_health_t health)
{
    switch (health) {
    case AS5600_DEVICE_HEALTH_HEALTHY:
        return ROBOT_ENDPOINT_HEALTH_HEALTHY;
    case AS5600_DEVICE_HEALTH_DEGRADED:
        return ROBOT_ENDPOINT_HEALTH_DEGRADED;
    case AS5600_DEVICE_HEALTH_OFFLINE:
        return ROBOT_ENDPOINT_HEALTH_OFFLINE;
    case AS5600_DEVICE_HEALTH_STALE:
        return ROBOT_ENDPOINT_HEALTH_STALE;
    case AS5600_DEVICE_HEALTH_UNKNOWN:
    default:
        return ROBOT_ENDPOINT_HEALTH_UNKNOWN;
    }
}

static bool calibration_is_approved(
    const as5600_position_endpoint_adapter_t *adapter)
{
    return adapter != NULL && adapter->calibration_approved &&
           adapter->device != NULL &&
           adapter->device->config.calibration == adapter->approved_calibration;
}

static robot_capability_error_t read_position_observation(
    robot_position_observation_port_t *port,
    robot_position_observation_t *observation)
{
    as5600_position_endpoint_adapter_t *adapter =
        port != NULL ? port->context : NULL;
    if (adapter == NULL || adapter->device == NULL || observation == NULL) {
        return ROBOT_CAP_INVALID_ARGUMENT;
    }

    as5600_device_snapshot_t raw_snapshot;
    if (!as5600_device_get_snapshot(adapter->device, &raw_snapshot)) {
        return ROBOT_CAP_IO_ERROR;
    }

    const bool calibrated = calibration_is_approved(adapter);
    robot_endpoint_health_t health = map_health(raw_snapshot.health);
    /*
     * Preserve the AS5600's magnetic-field warning even if a future device
     * implementation changes its own summary policy. An unapproved mapping is
     * also not a healthy physical-position endpoint.
     */
    if (health == ROBOT_ENDPOINT_HEALTH_HEALTHY &&
        (raw_snapshot.magnet_too_weak || raw_snapshot.magnet_too_strong ||
         !calibrated)) {
        health = ROBOT_ENDPOINT_HEALTH_DEGRADED;
    }

    const bool sensor_usable = raw_snapshot.raw_angle_valid &&
                               raw_snapshot.magnet_detected &&
                               raw_snapshot.online && !raw_snapshot.stale &&
                               calibrated;
    const float corrected_cyclic_degrees =
        as5600_calibration_corrected_degrees(
            adapter->device->config.calibration, raw_snapshot.raw_angle);
    float logical_degrees = 0.0f;
    robot_capability_error_t frame_result = ROBOT_CAP_UNAVAILABLE;
    bool referenced = false;
    if (sensor_usable &&
        adapter->coordinate_frame.project_cyclic_phase != NULL) {
        frame_result = adapter->coordinate_frame.project_cyclic_phase(
            adapter->coordinate_frame.context,
            corrected_cyclic_degrees,
            raw_snapshot.sample_timestamp_ms,
            &logical_degrees);
        referenced = frame_result == ROBOT_CAP_OK;
    }

    /* A LUT makes a cyclic sensor phase linear, not a steering-coordinate
     * measurement. The opaque frame becomes valid only after explicit
     * reference and after the controller has accepted this exact sample. */
    const robot_capability_error_t sensor_result =
        map_result(raw_snapshot.last_poll_result);
    *observation = (robot_position_observation_t){
        .valid = sensor_usable && referenced,
        .calibrated = calibrated,
        .referenced = referenced,
        .degrees = referenced ? logical_degrees : 0.0f,
        .timestamp_ms = raw_snapshot.sample_timestamp_ms,
        .source_endpoint_id = adapter->endpoint.id,
        .source = ROBOT_POSITION_OBSERVATION_SOURCE_INDEPENDENT_SENSOR,
        .online = raw_snapshot.online,
        .stale = raw_snapshot.stale,
        .health = health,
        /* Preserve a failed latest bus transaction even while a retained
         * sample is still fresh enough for the local stale policy. */
        .status = sensor_result != ROBOT_CAP_OK ? sensor_result
                  : !sensor_usable    ? sensor_result
                                      : frame_result,
    };
    return ROBOT_CAP_OK;
}

bool as5600_position_endpoint_adapter_init(
    as5600_position_endpoint_adapter_t *adapter,
    as5600_device_t *device,
    robot_endpoint_id_t endpoint_id,
    const char *name,
    robot_endpoint_criticality_t criticality,
    const as5600_calibration_lut_t *approved_calibration)
{
    static const robot_position_observation_ops_t observation_ops = {
        .read = read_position_observation,
    };
    if (adapter == NULL || device == NULL || !device->initialized ||
        endpoint_id == 0U || name == NULL || name[0] == '\0') {
        return false;
    }
    if (approved_calibration != NULL &&
        (!as5600_calibration_lut_validate(approved_calibration) ||
         device->config.calibration != approved_calibration)) {
        return false;
    }

    memset(adapter, 0, sizeof(*adapter));
    adapter->device = device;
    adapter->approved_calibration = approved_calibration;
    adapter->calibration_approved = approved_calibration != NULL;
    adapter->position_observation.ops = &observation_ops;
    adapter->position_observation.context = adapter;
    adapter->endpoint.id = endpoint_id;
    adapter->endpoint.name = name;
    adapter->endpoint.criticality = criticality;
    adapter->endpoint.position_observation = &adapter->position_observation;
    /* Communication health remains observable instead of hiding the endpoint. */
    adapter->endpoint.available = true;
    return true;
}

bool as5600_position_endpoint_adapter_set_coordinate_frame(
    as5600_position_endpoint_adapter_t *adapter,
    const as5600_position_coordinate_frame_t *coordinate_frame)
{
    if (adapter == NULL || !adapter->device || !adapter->device->initialized) {
        return false;
    }
    if (coordinate_frame != NULL &&
        coordinate_frame->project_cyclic_phase == NULL) {
        return false;
    }
    adapter->coordinate_frame = coordinate_frame != NULL
                                    ? *coordinate_frame
                                    : (as5600_position_coordinate_frame_t){0};
    return true;
}

void as5600_position_endpoint_adapter_deinit(
    as5600_position_endpoint_adapter_t *adapter)
{
    if (adapter != NULL) {
        memset(adapter, 0, sizeof(*adapter));
    }
}
