#ifndef ROBOT_PROFILE_H
#define ROBOT_PROFILE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "robot_capabilities.h"
#define ROBOT_PROFILE_SCHEMA_VERSION 1
#define ROBOT_PROFILE_MAX_ENDPOINTS 8
#define ROBOT_PROFILE_MAX_DRIVES 4
typedef struct { int uart_port,tx_pin,rx_pin,rts_pin; bool half_duplex; uint32_t baud_rate,response_timeout_ms,telemetry_period_ms,stale_timeout_ms; uint8_t retries,drive_count,drive_ids[ROBOT_PROFILE_MAX_DRIVES]; } robot_rs485_profile_t;
typedef struct { robot_endpoint_id_t id; const char *name; uint8_t drive_index,channel,legacy_motor_index; uint32_t capabilities; robot_endpoint_criticality_t criticality; int16_t min_rpm,max_rpm; } robot_endpoint_profile_t;
typedef struct { uint32_t schema_version; const char *name; robot_rs485_profile_t rs485; int rc_uart_port,rc_rx_pin; int steering_servo_pins[4]; bool steering_servos_enabled; int servo_min_us,servo_center_us,servo_max_us; float servo_min_deg,servo_max_deg; float wheelbase_m,track_width_m,wheel_radius_m,max_wheel_rpm; size_t endpoint_count; robot_endpoint_profile_t endpoints[ROBOT_PROFILE_MAX_ENDPOINTS]; } robot_profile_t;
typedef enum { ROBOT_PROFILE_VALID=0, ROBOT_PROFILE_BAD_SCHEMA, ROBOT_PROFILE_BAD_COUNT, ROBOT_PROFILE_DUPLICATE_ID, ROBOT_PROFILE_BAD_PIN, ROBOT_PROFILE_PIN_CONFLICT, ROBOT_PROFILE_BAD_REFERENCE, ROBOT_PROFILE_BAD_CHANNEL, ROBOT_PROFILE_BAD_CAPABILITY, ROBOT_PROFILE_BAD_LIMIT, ROBOT_PROFILE_BAD_GEOMETRY } robot_profile_error_t;
robot_profile_error_t robot_profile_validate(const robot_profile_t*);
const robot_profile_t *robot_profile_current(void);
#endif
