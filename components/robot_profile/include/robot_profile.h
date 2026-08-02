#ifndef ROBOT_PROFILE_H
#define ROBOT_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "robot_capabilities.h"

#define ROBOT_PROFILE_SCHEMA_VERSION 2
#define ROBOT_PROFILE_MAX_BUSES 5
#define ROBOT_PROFILE_MAX_DEVICES 4
#define ROBOT_PROFILE_MAX_ENDPOINTS 8
#define ROBOT_PROFILE_NO_BUS UINT16_MAX
#define ROBOT_PROFILE_NO_GEOMETRY 0
#define ROBOT_PROFILE_DIFFERENTIAL_GEOMETRY 1

typedef enum {
    ROBOT_BUS_UART_RS485 = 0,
    ROBOT_BUS_CAN_TWAI,
    ROBOT_BUS_I2C,
    ROBOT_BUS_PWM,
    ROBOT_BUS_GPIO,
} robot_bus_type_t;

typedef enum {
    ROBOT_DRIVER_SVD48 = 1,
    ROBOT_DRIVER_PWM_SERVO,
    ROBOT_DRIVER_MAGNETIC_ENCODER,
    ROBOT_DRIVER_TEST_CAN,
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
} robot_endpoint_profile_t;

typedef struct {
    uint8_t kind;
    float wheelbase_m;
    float track_width_m;
    float wheel_radius_m;
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
} robot_profile_error_t;

robot_profile_error_t robot_profile_validate(const robot_profile_t *profile);
robot_profile_error_t robot_profile_validate_with_registry(
    const robot_profile_t *profile, const robot_driver_registry_t *registry);
const robot_profile_t *robot_profile_selected(void);
const char *robot_profile_selected_name(void);
const robot_board_profile_t *robot_board_esp32s3_current(void);
const robot_bus_profile_t *robot_profile_find_bus_type(const robot_profile_t *profile, robot_bus_type_t type);
const robot_device_profile_t *robot_profile_find_device_driver(const robot_profile_t *profile, robot_driver_id_t driver, size_t ordinal);

#endif
