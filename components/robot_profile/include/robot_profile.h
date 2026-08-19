#ifndef ROBOT_PROFILE_H
#define ROBOT_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "robot_capabilities.h"

#define ROBOT_PROFILE_SCHEMA_VERSION 4
#define ROBOT_PROFILE_CONTROL_TTL_MIN_MS 50U
#define ROBOT_PROFILE_CONTROL_TTL_MAX_MS 500U
#define ROBOT_PROFILE_MAX_BUSES 5
#define ROBOT_PROFILE_MAX_DEVICES 4
#define ROBOT_PROFILE_MAX_ENDPOINTS 8
#define ROBOT_PROFILE_MAX_STEERING_AXES 2
#define ROBOT_PROFILE_NO_BUS UINT16_MAX
#define ROBOT_PROFILE_NO_GEOMETRY 0
#define ROBOT_PROFILE_DIFFERENTIAL_GEOMETRY 1
#define ROBOT_PROFILE_MOTION_SIDE_NONE 0
#define ROBOT_PROFILE_MOTION_SIDE_LEFT 1
#define ROBOT_PROFILE_MOTION_SIDE_RIGHT 2
/* The selected steering composition runs its sensor/controller service at
 * this fixed cadence. Steering-profile validation includes it in the local
 * stale-feedback scheduling budget. */
#define ROBOT_PROFILE_STEERING_SERVICE_PERIOD_MS 10U

typedef enum {
    ROBOT_BUS_UART_RS485 = 0,
    ROBOT_BUS_CAN_TWAI,
    ROBOT_BUS_I2C,
    ROBOT_BUS_PWM,
    ROBOT_BUS_GPIO,
    ROBOT_BUS_NONE,
} robot_bus_type_t;

typedef enum {
    ROBOT_DRIVER_SVD48 = 1,
    ROBOT_DRIVER_PWM_SERVO,
    ROBOT_DRIVER_MAGNETIC_ENCODER,
    ROBOT_DRIVER_TEST_CAN,
    ROBOT_DRIVER_PWM_MOTOR_MODE,
    ROBOT_DRIVER_AS5600,
    ROBOT_DRIVER_STEERING_POSITION_CONTROLLER,
} robot_driver_id_t;

typedef struct {
    uint16_t id;
    robot_bus_type_t type;
    uint8_t peripheral;
    int pins[2];
    uint32_t rate;
    uint32_t response_timeout_ms;
    uint32_t telemetry_period_ms;
    uint32_t stale_timeout_ms;
    uint8_t retries;
} robot_bus_profile_t;

typedef struct {
    uint16_t id;
    robot_driver_id_t driver_id;
    uint16_t bus_id;
    uint8_t address;
    uint8_t channel_count;
    robot_endpoint_criticality_t criticality;
} robot_device_profile_t;

typedef struct {
    robot_endpoint_id_t id;
    const char *name;
    uint16_t device_id;
    uint8_t channel;
    uint32_t capabilities;
    robot_endpoint_criticality_t criticality;
    int16_t min_rpm;
    int16_t max_rpm;
    float min_position_degrees;
    float max_position_degrees;
    /* Application-level differential mapping. A zero side deliberately keeps
     * this endpoint out of robot motion while preserving its device capability. */
    uint8_t motion_side;
    int8_t motion_direction_sign;
    float motor_to_wheel_ratio;
} robot_endpoint_profile_t;

/* A linearity calibration is profile data, not a generic AS5600 property.  Its
 * raw capture remains in durable external evidence; only the reviewable static
 * correction and its provenance are carried by the immutable build profile. */
typedef struct {
    uint32_t format_version;
    const char *id;
    const char *hardware_identity;
    const char *provenance_sha256;
    const int16_t *correction_centidegrees;
    size_t correction_count;
} robot_as5600_calibration_profile_t;

/* One position axis composes independent PWM and sensor devices.  The controller
 * device owns closed-loop behavior; neither low-level device embeds the other. */
typedef struct {
    uint16_t controller_device_id;
    uint16_t pwm_device_id;
    uint16_t sensor_device_id;
    robot_endpoint_id_t actuator_endpoint_id;
    robot_endpoint_id_t observation_endpoint_id;
    float min_position_degrees;
    float max_position_degrees;
    uint16_t pwm_minimum_us;
    uint16_t pwm_neutral_us;
    uint16_t pwm_maximum_us;
    uint16_t positive_far_us;
    uint16_t positive_near_us;
    uint16_t negative_far_us;
    uint16_t negative_near_us;
    float arrival_min_error_degrees;
    float arrival_max_error_degrees;
    float full_speed_error_degrees;
    float reacquire_error_degrees;
    uint8_t stable_samples;
    uint8_t reacquire_samples;
    uint16_t max_raw_step_counts;
    uint32_t reversal_neutral_ms;
    uint32_t sensor_neutral_after_ms;
    uint32_t sensor_fault_after_ms;
    uint32_t command_ttl_ms;
    uint32_t move_timeout_ms;
    /* The empirical fixture currently reports STATUS.ML.  This is a narrowly
     * scoped development exception: MD must still be present, and MH, an
     * incomplete primary read, or any transport failure must inhibit control.
     * It is not a general permission to drive on degraded sensor health. */
    bool allow_magnet_too_weak_for_development;
    const robot_as5600_calibration_profile_t *calibration;
} robot_steering_axis_profile_t;

typedef struct {
    uint8_t kind;
    /* Differential v1 does not consume wheelbase. Zero means not applicable;
     * a positive value remains available to the transitional four-wheel
     * MOVE_VEL compatibility path. */
    float wheelbase_m;
    float track_width_m;
    float wheel_radius_m;
    float max_vx_mps;
    float max_vy_mps;
    float max_wz_radps;
    uint32_t control_ttl_ms;
} robot_application_profile_t;

typedef struct {
    const char *id;
    uint64_t valid_gpio_mask;
    uint64_t reserved_gpio_mask;
    uint64_t input_gpio_mask;
    uint64_t output_gpio_mask;
    uint64_t pwm_gpio_mask;
    uint8_t uart_count;
    uint8_t twai_count;
    uint8_t i2c_count;
} robot_board_profile_t;

typedef struct {
    uint32_t schema_version;
    const char *name;
    const robot_board_profile_t *board;
    size_t bus_count;
    robot_bus_profile_t buses[ROBOT_PROFILE_MAX_BUSES];
    size_t device_count;
    robot_device_profile_t devices[ROBOT_PROFILE_MAX_DEVICES];
    size_t endpoint_count;
    robot_endpoint_profile_t endpoints[ROBOT_PROFILE_MAX_ENDPOINTS];
    size_t steering_axis_count;
    robot_steering_axis_profile_t steering_axes[ROBOT_PROFILE_MAX_STEERING_AXES];
    robot_application_profile_t application;
} robot_profile_t;

typedef struct {
    robot_driver_id_t driver_id;
    robot_bus_type_t bus_type;
    uint32_t capabilities;
    uint8_t max_channels;
} robot_driver_descriptor_t;

typedef struct {
    const robot_driver_descriptor_t *items;
    size_t count;
} robot_driver_registry_t;

typedef enum {
    ROBOT_PROFILE_VALID = 0,
    ROBOT_PROFILE_BAD_SCHEMA,
    ROBOT_PROFILE_BAD_COUNT,
    ROBOT_PROFILE_DUPLICATE_ID,
    ROBOT_PROFILE_DUPLICATE_NAME,
    ROBOT_PROFILE_BAD_PIN,
    ROBOT_PROFILE_RESERVED_PIN,
    ROBOT_PROFILE_PIN_CONFLICT,
    ROBOT_PROFILE_BAD_BUS,
    ROBOT_PROFILE_BAD_DRIVER,
    ROBOT_PROFILE_BAD_REFERENCE,
    ROBOT_PROFILE_BAD_CHANNEL,
    ROBOT_PROFILE_DUPLICATE_CHANNEL,
    ROBOT_PROFILE_BAD_CAPABILITY,
    ROBOT_PROFILE_BAD_LIMIT,
    ROBOT_PROFILE_BAD_GEOMETRY,
    ROBOT_PROFILE_DUPLICATE_ADDRESS,
    ROBOT_PROFILE_BAD_STEERING_AXIS,
    ROBOT_PROFILE_BAD_CALIBRATION,
} robot_profile_error_t;

robot_profile_error_t robot_profile_validate(const robot_profile_t *profile);
robot_profile_error_t robot_profile_validate_with_registry(
    const robot_profile_t *profile, const robot_driver_registry_t *registry);
const robot_profile_t *robot_profile_selected(void);
const char *robot_profile_selected_name(void);
const robot_board_profile_t *robot_board_esp32s3_current(void);
const robot_bus_profile_t *robot_profile_find_bus_type(const robot_profile_t *profile, robot_bus_type_t type);
const robot_device_profile_t *robot_profile_find_device_driver(const robot_profile_t *profile, robot_driver_id_t driver, size_t ordinal);
const robot_steering_axis_profile_t *robot_profile_find_steering_axis(
    const robot_profile_t *profile,
    uint16_t controller_device_id);

#endif
