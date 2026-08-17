#include "robot_profile.h"

#include <math.h>
#include <string.h>

#define GPIO_BIT(pin) (UINT64_C(1) << (pin))
#define ESP32S3_VALID_GPIO_MASK ((UINT64_C(1) << 49) - 1)
#define ESP32S3_RESERVED_GPIO_MASK (GPIO_BIT(19) | GPIO_BIT(20))

static const robot_board_profile_t BOARD = {
    .id = "botfarms_esp32s3_rev1",
    .valid_gpio_mask = ESP32S3_VALID_GPIO_MASK,
    .reserved_gpio_mask = ESP32S3_RESERVED_GPIO_MASK,
    .input_gpio_mask = ESP32S3_VALID_GPIO_MASK,
    .output_gpio_mask = ESP32S3_VALID_GPIO_MASK & ~(GPIO_BIT(46)),
    .pwm_gpio_mask = ESP32S3_VALID_GPIO_MASK & ~(GPIO_BIT(46)),
    .uart_count = 3,
    .twai_count = 1,
    .i2c_count = 2,
};

static const robot_driver_descriptor_t DRIVERS[] = {
    {ROBOT_DRIVER_SVD48, ROBOT_BUS_UART_RS485,
     ROBOT_CAPABILITY_VELOCITY_RPM | ROBOT_CAPABILITY_STOPPABLE, 2},
    {ROBOT_DRIVER_PWM_SERVO, ROBOT_BUS_PWM, ROBOT_CAPABILITY_POSITION, 1},
    {ROBOT_DRIVER_MAGNETIC_ENCODER, ROBOT_BUS_I2C, ROBOT_CAPABILITY_POSITION_SENSOR, 1},
    {ROBOT_DRIVER_PWM_MOTOR_MODE, ROBOT_BUS_PWM, 0U, 1},
    {ROBOT_DRIVER_AS5600, ROBOT_BUS_I2C,
     ROBOT_CAPABILITY_POSITION_OBSERVATION, 1},
    {ROBOT_DRIVER_STEERING_POSITION_CONTROLLER, ROBOT_BUS_NONE,
     ROBOT_CAPABILITY_POSITION | ROBOT_CAPABILITY_STOPPABLE |
         ROBOT_CAPABILITY_POSITION_REFERENCE, 1},
};

static const robot_driver_registry_t BUILTIN_REGISTRY = {
    .items = DRIVERS, .count = sizeof(DRIVERS) / sizeof(DRIVERS[0])};

static const robot_profile_t CURRENT __attribute__((unused)) = {
    .schema_version = ROBOT_PROFILE_SCHEMA_VERSION,
    .name = "current_robot",
    .board = &BOARD,
    .bus_count = 2,
    .buses = {
        {1, ROBOT_BUS_UART_RS485, 2, {17, 16}, 115200, 100, 30, 1000, 2},
        {2, ROBOT_BUS_GPIO, 1, {14, -1}, 0, 0, 0, 0, 0},
    },
    .device_count = 2,
    .devices = {
        {1, ROBOT_DRIVER_SVD48, 1, 1, 2, ROBOT_ENDPOINT_REQUIRED},
        {2, ROBOT_DRIVER_SVD48, 1, 2, 2, ROBOT_ENDPOINT_REQUIRED},
    },
    .endpoint_count = 4,
    .endpoints = {
        {1, "traction_front_left", 1, 0, 3, ROBOT_ENDPOINT_REQUIRED, -15, 15},
        {2, "traction_front_right", 1, 1, 3, ROBOT_ENDPOINT_REQUIRED, -15, 15},
        {3, "traction_rear_left", 2, 0, 3, ROBOT_ENDPOINT_REQUIRED, -15, 15},
        {4, "traction_rear_right", 2, 1, 3, ROBOT_ENDPOINT_REQUIRED, -15, 15},
    },
    .application = {ROBOT_PROFILE_DIFFERENTIAL_GEOMETRY, 1.60f, 0.70f, 0.10f},
};

static const robot_profile_t SINGLE_MOTOR __attribute__((unused)) = {
    .schema_version = ROBOT_PROFILE_SCHEMA_VERSION,
    .name = "bench_single_svd48_motor",
    .board = &BOARD,
    .bus_count = 2,
    .buses = {{1, ROBOT_BUS_UART_RS485, 2, {17, 16}, 115200, 100, 30, 1000, 2},
              {2, ROBOT_BUS_GPIO, 1, {14, -1}, 0, 0, 0, 0, 0}},
    .device_count = 1,
    .devices = {{1, ROBOT_DRIVER_SVD48, 1, 1, 2, ROBOT_ENDPOINT_DEVELOPMENT}},
    .endpoint_count = 1,
    .endpoints = {{1, "bench_motor", 1, 0, 3, ROBOT_ENDPOINT_DEVELOPMENT, -15, 15}},
    .application = {ROBOT_PROFILE_NO_GEOMETRY, 0, 0, 0},
};

/* Rafa's channel-to-side mapping is intentionally unresolved until the first
 * installed hardware qualification.  M1/M2 are therefore the stable endpoint
 * names and no body-motion geometry is enabled. */
static const robot_profile_t RAFA __attribute__((unused)) = {
    .schema_version = ROBOT_PROFILE_SCHEMA_VERSION,
    .name = "rafa",
    .board = &BOARD,
    .bus_count = 2U,
    .buses = {
        {1U, ROBOT_BUS_UART_RS485, 2U, {17, 16}, 115200U, 100U, 30U, 1000U, 2U},
        {2U, ROBOT_BUS_GPIO, 1U, {14, -1}, 0U, 0U, 0U, 0U, 0U},
    },
    .device_count = 1U,
    .devices = {
        {1U, ROBOT_DRIVER_SVD48, 1U, 2U, 2U, ROBOT_ENDPOINT_REQUIRED},
    },
    .endpoint_count = 2U,
    .endpoints = {
        {1U, "rafa_traction_m1", 1U, 0U,
         ROBOT_CAPABILITY_VELOCITY_RPM | ROBOT_CAPABILITY_STOPPABLE,
         ROBOT_ENDPOINT_REQUIRED, -15, 15},
        {2U, "rafa_traction_m2", 1U, 1U,
         ROBOT_CAPABILITY_VELOCITY_RPM | ROBOT_CAPABILITY_STOPPABLE,
         ROBOT_ENDPOINT_REQUIRED, -15, 15},
    },
    .application = {ROBOT_PROFILE_NO_GEOMETRY, 0, 0, 0},
};

/* This table is a reviewed, static calibration candidate from the empirical
 * new-leg fixture.  It is intentionally scoped to the explicit bench profile
 * below; it must not be reused after an encoder, magnet, shaft, gap or geometry
 * change.  The raw capture is retained outside this repository.  Its historical
 * validation still had 7.925 degrees P95 and 15.924 degrees maximum residual, so
 * this table is neither an absolute-angle accuracy claim nor a basis for the
 * controller's 3-degree arrival policy. */
static const int16_t NEW_LEG_AS5600_CORRECTION_CENTIDEGREES[128] = {
     124,   58,   -9,  -77, -147, -218, -290, -364,
    -438, -511, -583, -654, -723, -789, -853, -914,
    -972,-1028,-1081,-1129,-1173,-1210,-1241,-1265,
   -1283,-1295,-1302,-1304,-1302,-1293,-1278,-1257,
   -1231,-1200,-1165,-1128,-1087,-1041, -990, -934,
    -875, -815, -754, -692, -630, -569, -510, -455,
    -405, -360, -320, -282, -248, -218, -191, -168,
    -148, -128, -109,  -89,  -69,  -49,  -30,  -12,
       7,   27,   50,   74,   99,  124,  149,  172,
     197,  223,  252,  283,  315,  346,  378,  409,
     443,  479,  518,  559,  602,  644,  686,  729,
     774,  820,  868,  915,  961, 1003, 1041, 1076,
    1108, 1135, 1157, 1174, 1184, 1188, 1186, 1178,
    1166, 1149, 1128, 1104, 1077, 1047, 1014,  979,
     941,  902,  860,  818,  774,  729,  684,  637,
     589,  538,  485,  430,  373,  313,  251,  188,
};

static const robot_as5600_calibration_profile_t NEW_LEG_AS5600_CALIBRATION = {
    .format_version = 1U,
    .id = "new_leg_2026_08_07_provisional",
    .hardware_identity = "new_leg_fixture_magnet_shaft_geometry_2026_08_07",
    .provenance_sha256 =
        "a6a163ee94145694af91419be1c5a2224ecfbe34bd2299ea6cf3ed19f1bd743f",
    .correction_centidegrees = NEW_LEG_AS5600_CORRECTION_CENTIDEGREES,
    .correction_count = sizeof(NEW_LEG_AS5600_CORRECTION_CENTIDEGREES) /
                        sizeof(NEW_LEG_AS5600_CORRECTION_CENTIDEGREES[0]),
};

/* A deliberately isolated development profile.  GPIO14 is PWM here, so the
 * PPM/RC GPIO14 bus found in traction profiles is absent by construction. */
static const robot_profile_t BENCH_STEERING_AS5600 __attribute__((unused)) = {
    .schema_version = ROBOT_PROFILE_SCHEMA_VERSION,
    .name = "bench_single_steering_as5600",
    .board = &BOARD,
    .bus_count = 2U,
    .buses = {
        {1U, ROBOT_BUS_PWM, 0U, {14, -1}, 50U, 0U, 0U, 0U, 0U},
        /* Every control-rate AS5600 sample is one contiguous STATUS+RAW read.
         * AGC/MAGNITUDE are an initial one-shot diagnostic read, so the first
         * poll can take two bounded transactions. The 25 ms deadline has
         * nominal recovery-path slack at 5 kHz; the profile validator also
         * budgets that first slot plus cadence before stale-neutral. This is a
         * source-level scheduling budget, not a physical stop-time claim. */
        {2U, ROBOT_BUS_I2C, 0U, {5, 7}, 5000U, 25U, 40U, 120U, 0U},
    },
    .device_count = 3U,
    .devices = {
        {1U, ROBOT_DRIVER_PWM_MOTOR_MODE, 1U, 0U, 1U,
         ROBOT_ENDPOINT_DEVELOPMENT},
        {2U, ROBOT_DRIVER_AS5600, 2U, 0x36U, 1U,
         ROBOT_ENDPOINT_DEVELOPMENT},
        {3U, ROBOT_DRIVER_STEERING_POSITION_CONTROLLER, ROBOT_PROFILE_NO_BUS,
         0U, 1U, ROBOT_ENDPOINT_DEVELOPMENT},
    },
    .endpoint_count = 2U,
    .endpoints = {
        {
            .id = 1U,
            .name = "bench_steering_position",
            .device_id = 3U,
            .channel = 0U,
            .capabilities = ROBOT_CAPABILITY_POSITION |
                            ROBOT_CAPABILITY_STOPPABLE |
                            ROBOT_CAPABILITY_POSITION_REFERENCE,
            .criticality = ROBOT_ENDPOINT_DEVELOPMENT,
            .min_position_degrees = -90.0f,
            .max_position_degrees = 90.0f,
        },
        {
            .id = 2U,
            .name = "bench_steering_position_feedback",
            .device_id = 2U,
            .channel = 0U,
            .capabilities = ROBOT_CAPABILITY_POSITION_OBSERVATION,
            .criticality = ROBOT_ENDPOINT_DEVELOPMENT,
        },
    },
    .steering_axis_count = 1U,
    .steering_axes = {{
        .controller_device_id = 3U,
        .pwm_device_id = 1U,
        .sensor_device_id = 2U,
        .actuator_endpoint_id = 1U,
        .observation_endpoint_id = 2U,
        .min_position_degrees = -90.0f,
        .max_position_degrees = 90.0f,
        .pwm_minimum_us = 500U,
        .pwm_neutral_us = 1500U,
        .pwm_maximum_us = 2500U,
        .positive_far_us = 1040U,
        .positive_near_us = 1390U,
        .negative_far_us = 2000U,
        .negative_near_us = 1680U,
        .arrival_min_error_degrees = 0.0f,
        .arrival_max_error_degrees = 3.0f,
        .full_speed_error_degrees = 6.0f,
        .reacquire_error_degrees = 4.5f,
        .stable_samples = 5U,
        .reacquire_samples = 5U,
        .max_raw_step_counts = 56U,
        .reversal_neutral_ms = 240U,
        .sensor_neutral_after_ms = 120U,
        .sensor_fault_after_ms = 400U,
        .command_ttl_ms = 650U,
        .move_timeout_ms = 45000U,
        .allow_magnet_too_weak_for_development = true,
        .calibration = &NEW_LEG_AS5600_CALIBRATION,
    }},
    .application = {ROBOT_PROFILE_NO_GEOMETRY, 0, 0, 0},
};

static const robot_bus_profile_t *find_bus(const robot_profile_t *profile, uint16_t id)
{
    for (size_t index = 0; index < profile->bus_count; ++index) {
        if (profile->buses[index].id == id) return &profile->buses[index];
    }
    return NULL;
}

static const robot_device_profile_t *find_device(const robot_profile_t *profile, uint16_t id)
{
    for (size_t index = 0; index < profile->device_count; ++index) {
        if (profile->devices[index].id == id) return &profile->devices[index];
    }
    return NULL;
}

static const robot_endpoint_profile_t *find_endpoint(const robot_profile_t *profile,
                                                     robot_endpoint_id_t id)
{
    for (size_t index = 0; index < profile->endpoint_count; ++index) {
        if (profile->endpoints[index].id == id) {
            return &profile->endpoints[index];
        }
    }
    return NULL;
}

static const robot_driver_descriptor_t *find_driver(const robot_driver_registry_t *registry,
                                                     robot_driver_id_t id)
{
    for (size_t index = 0; index < registry->count; ++index) {
        if (registry->items[index].driver_id == id) return &registry->items[index];
    }
    return NULL;
}

static bool sha256_text_is_valid(const char *value)
{
    if (!value || strlen(value) != 64U) {
        return false;
    }
    for (size_t index = 0U; index < 64U; ++index) {
        const char c = value[index];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

static float calibration_correction_degrees(
    const robot_as5600_calibration_profile_t *calibration,
    uint16_t raw_count)
{
    const uint16_t raw = (uint16_t)(raw_count & 0x0FFFU);
    const size_t index = (size_t)(raw >> 5U);
    const size_t next = (index + 1U) & 0x7FU;
    const float fraction = (float)(raw & 0x1FU) / 32.0f;
    const float first = (float)calibration->correction_centidegrees[index];
    const float second = (float)calibration->correction_centidegrees[next];
    return (first + (second - first) * fraction) * 0.01f;
}

static bool calibration_is_valid(const robot_as5600_calibration_profile_t *calibration)
{
    if (!calibration || calibration->format_version != 1U ||
        !calibration->id || !calibration->id[0] ||
        !calibration->hardware_identity || !calibration->hardware_identity[0] ||
        !sha256_text_is_valid(calibration->provenance_sha256) ||
        !calibration->correction_centidegrees ||
        calibration->correction_count != 128U) {
        return false;
    }
    const float first = calibration_correction_degrees(calibration, 0U);
    float previous = first;
    for (uint16_t raw = 1U; raw < 4096U; ++raw) {
        const float mapped = (float)raw * (360.0f / 4096.0f) +
                             calibration_correction_degrees(calibration, raw);
        if (!isfinite(mapped) || mapped <= previous) {
            return false;
        }
        previous = mapped;
    }
    /* The correction is cyclic.  A strictly increasing raw-to-corrected map
     * also needs a positive final increment across 4095 -> 0, otherwise a
     * schema-valid profile could reach construction only to be rejected by the
     * device-level LUT validator. */
    return previous < first + 360.0f;
}

static bool steering_axis_is_valid(const robot_profile_t *profile,
                                   const robot_steering_axis_profile_t *axis)
{
    const robot_device_profile_t *controller =
        find_device(profile, axis->controller_device_id);
    const robot_device_profile_t *pwm = find_device(profile, axis->pwm_device_id);
    const robot_device_profile_t *sensor =
        find_device(profile, axis->sensor_device_id);
    const robot_bus_profile_t *pwm_bus =
        pwm != NULL ? find_bus(profile, pwm->bus_id) : NULL;
    const robot_bus_profile_t *sensor_bus =
        sensor != NULL ? find_bus(profile, sensor->bus_id) : NULL;
    const robot_endpoint_profile_t *actuator =
        find_endpoint(profile, axis->actuator_endpoint_id);
    const robot_endpoint_profile_t *observation =
        find_endpoint(profile, axis->observation_endpoint_id);
    if (!controller || !pwm || !sensor || !actuator || !observation ||
        controller->driver_id != ROBOT_DRIVER_STEERING_POSITION_CONTROLLER ||
        controller->bus_id != ROBOT_PROFILE_NO_BUS ||
        pwm->driver_id != ROBOT_DRIVER_PWM_MOTOR_MODE ||
        sensor->driver_id != ROBOT_DRIVER_AS5600 ||
        actuator->device_id != controller->id ||
        observation->device_id != sensor->id ||
        (actuator->capabilities & (ROBOT_CAPABILITY_POSITION |
                                   ROBOT_CAPABILITY_STOPPABLE |
                                   ROBOT_CAPABILITY_POSITION_REFERENCE)) !=
            (ROBOT_CAPABILITY_POSITION | ROBOT_CAPABILITY_STOPPABLE |
             ROBOT_CAPABILITY_POSITION_REFERENCE) ||
        (observation->capabilities & ROBOT_CAPABILITY_POSITION_OBSERVATION) ==
            0U ||
        !isfinite(axis->min_position_degrees) ||
        !isfinite(axis->max_position_degrees) ||
        axis->min_position_degrees >= axis->max_position_degrees ||
        axis->min_position_degrees != actuator->min_position_degrees ||
        axis->max_position_degrees != actuator->max_position_degrees ||
        axis->pwm_minimum_us >= axis->pwm_neutral_us ||
        axis->pwm_neutral_us >= axis->pwm_maximum_us ||
        !pwm_bus || pwm_bus->type != ROBOT_BUS_PWM || pwm_bus->rate == 0U ||
        pwm_bus->rate > UINT32_C(1000000) ||
        axis->pwm_maximum_us >= UINT32_C(1000000) / pwm_bus->rate ||
        axis->positive_far_us < axis->pwm_minimum_us ||
        axis->positive_far_us > axis->pwm_maximum_us ||
        axis->positive_near_us < axis->pwm_minimum_us ||
        axis->positive_near_us > axis->pwm_maximum_us ||
        axis->negative_far_us < axis->pwm_minimum_us ||
        axis->negative_far_us > axis->pwm_maximum_us ||
        axis->negative_near_us < axis->pwm_minimum_us ||
        axis->negative_near_us > axis->pwm_maximum_us ||
        axis->positive_far_us >= axis->positive_near_us ||
        axis->positive_near_us >= axis->pwm_neutral_us ||
        axis->negative_far_us <= axis->negative_near_us ||
        axis->negative_near_us <= axis->pwm_neutral_us ||
        !isfinite(axis->arrival_min_error_degrees) ||
        !isfinite(axis->arrival_max_error_degrees) ||
        !isfinite(axis->full_speed_error_degrees) ||
        !isfinite(axis->reacquire_error_degrees) ||
        axis->arrival_min_error_degrees < 0.0f ||
        axis->arrival_max_error_degrees < axis->arrival_min_error_degrees ||
        axis->full_speed_error_degrees <= axis->arrival_max_error_degrees ||
        axis->reacquire_error_degrees <= axis->arrival_max_error_degrees ||
        axis->stable_samples == 0U || axis->reacquire_samples == 0U ||
        axis->max_raw_step_counts == 0U || axis->max_raw_step_counts > 2048U ||
        axis->reversal_neutral_ms == 0U ||
        axis->sensor_neutral_after_ms == 0U ||
        axis->sensor_fault_after_ms <= axis->sensor_neutral_after_ms ||
        /* The active bit-bang transaction sends address/register/read-address
         * and reads three bytes. Its nominal healthy path is 174 half-periods;
         * bus recovery adds 21. Include a further 1 ms CPU/GPIO margin before
         * accepting the bounded transaction deadline. STATUS+RAW are one
         * primary transaction and the initial AGC/MAGNITUDE diagnostic is one
         * more. The steering task schedules from the prior deadline (skipping
         * missed slots), so this is the bounded worst-age model before local
         * stale-neutral, not a physical response-time claim. */
        !sensor_bus || sensor_bus->response_timeout_ms == 0U ||
        sensor_bus->telemetry_period_ms == 0U || sensor_bus->rate == 0U ||
        ((UINT64_C(195) *
          ((UINT64_C(500000) + sensor_bus->rate - 1U) / sensor_bus->rate) +
          UINT64_C(1000)) >=
         (uint64_t)sensor_bus->response_timeout_ms * 1000U) ||
        ((uint64_t)sensor_bus->response_timeout_ms * 2U +
         sensor_bus->telemetry_period_ms +
         ROBOT_PROFILE_STEERING_SERVICE_PERIOD_MS >=
         axis->sensor_neutral_after_ms) ||
        axis->command_ttl_ms == 0U || axis->move_timeout_ms == 0U ||
        !calibration_is_valid(axis->calibration)) {
        return false;
    }
    return true;
}

robot_profile_error_t robot_profile_validate_with_registry(
    const robot_profile_t *profile, const robot_driver_registry_t *registry)
{
    if (!profile || !registry || profile->schema_version != ROBOT_PROFILE_SCHEMA_VERSION) return ROBOT_PROFILE_BAD_SCHEMA;
    if (!profile->board || profile->bus_count > ROBOT_PROFILE_MAX_BUSES ||
        profile->device_count > ROBOT_PROFILE_MAX_DEVICES ||
        profile->endpoint_count > ROBOT_PROFILE_MAX_ENDPOINTS ||
        profile->steering_axis_count > ROBOT_PROFILE_MAX_STEERING_AXES) return ROBOT_PROFILE_BAD_COUNT;
    uint64_t used_pins = 0;
    for (size_t i = 0; i < profile->bus_count; ++i) {
        const robot_bus_profile_t *bus = &profile->buses[i];
        if (bus->id == 0) return ROBOT_PROFILE_BAD_REFERENCE;
        if (bus->type == ROBOT_BUS_NONE) return ROBOT_PROFILE_BAD_BUS;
        for (size_t j = 0; j < i; ++j) if (profile->buses[j].id == bus->id) return ROBOT_PROFILE_DUPLICATE_ID;
        if ((bus->type == ROBOT_BUS_UART_RS485 && bus->peripheral >= profile->board->uart_count) ||
            (bus->type == ROBOT_BUS_CAN_TWAI && bus->peripheral >= profile->board->twai_count) ||
            (bus->type == ROBOT_BUS_I2C && bus->peripheral >= profile->board->i2c_count)) return ROBOT_PROFILE_BAD_BUS;
        for (size_t pin_index = 0; pin_index < 2; ++pin_index) {
            int pin = bus->pins[pin_index];
            if (pin < 0) continue;
            if (pin >= 64 || !(profile->board->valid_gpio_mask & GPIO_BIT(pin))) return ROBOT_PROFILE_BAD_PIN;
            if (profile->board->reserved_gpio_mask & GPIO_BIT(pin)) return ROBOT_PROFILE_RESERVED_PIN;
            if (used_pins & GPIO_BIT(pin)) return ROBOT_PROFILE_PIN_CONFLICT;
            used_pins |= GPIO_BIT(pin);
        }
    }
    for (size_t i = 0; i < profile->device_count; ++i) {
        const robot_device_profile_t *device = &profile->devices[i];
        if (device->id == 0) return ROBOT_PROFILE_BAD_REFERENCE;
        for (size_t j = 0; j < i; ++j) {
            if (profile->devices[j].id == device->id) return ROBOT_PROFILE_DUPLICATE_ID;
            if (profile->devices[j].bus_id == device->bus_id &&
                device->bus_id != ROBOT_PROFILE_NO_BUS &&
                profile->devices[j].address == device->address) {
                const robot_bus_profile_t *shared_bus =
                    find_bus(profile, device->bus_id);
                if (shared_bus && shared_bus->type != ROBOT_BUS_PWM &&
                    shared_bus->type != ROBOT_BUS_GPIO) {
                    return ROBOT_PROFILE_DUPLICATE_ADDRESS;
                }
            }
        }
        const robot_bus_profile_t *bus = device->bus_id == ROBOT_PROFILE_NO_BUS
                                             ? NULL
                                             : find_bus(profile, device->bus_id);
        const robot_driver_descriptor_t *driver = find_driver(registry, device->driver_id);
        if (!driver ||
            (driver->bus_type == ROBOT_BUS_NONE &&
             device->bus_id != ROBOT_PROFILE_NO_BUS) ||
            device->channel_count == 0 ||
            device->channel_count > driver->max_channels) return ROBOT_PROFILE_BAD_DRIVER;
        if (driver->bus_type != ROBOT_BUS_NONE && !bus) {
            return ROBOT_PROFILE_BAD_REFERENCE;
        }
        if (driver->bus_type != ROBOT_BUS_NONE && driver->bus_type != bus->type) {
            return ROBOT_PROFILE_BAD_DRIVER;
        }
    }
    for (size_t i = 0; i < profile->endpoint_count; ++i) {
        const robot_endpoint_profile_t *endpoint = &profile->endpoints[i];
        if (endpoint->id == 0 || !endpoint->name || !endpoint->name[0]) return ROBOT_PROFILE_BAD_REFERENCE;
        for (size_t j = 0; j < i; ++j) {
            if (profile->endpoints[j].id == endpoint->id) return ROBOT_PROFILE_DUPLICATE_ID;
            if (strcmp(profile->endpoints[j].name, endpoint->name) == 0) return ROBOT_PROFILE_DUPLICATE_NAME;
            if (profile->endpoints[j].device_id == endpoint->device_id &&
                profile->endpoints[j].channel == endpoint->channel) return ROBOT_PROFILE_DUPLICATE_CHANNEL;
        }
        const robot_device_profile_t *device = find_device(profile, endpoint->device_id);
        if (!device) return ROBOT_PROFILE_BAD_REFERENCE;
        const robot_driver_descriptor_t *driver = find_driver(registry, device->driver_id);
        if (endpoint->channel >= device->channel_count) return ROBOT_PROFILE_BAD_CHANNEL;
        if (!driver || (endpoint->capabilities & driver->capabilities) != endpoint->capabilities) return ROBOT_PROFILE_BAD_CAPABILITY;
        if ((endpoint->capabilities & ROBOT_CAPABILITY_VELOCITY_RPM) &&
            (endpoint->min_rpm > endpoint->max_rpm || endpoint->min_rpm > 0 || endpoint->max_rpm < 0)) return ROBOT_PROFILE_BAD_LIMIT;
        if ((endpoint->capabilities & ROBOT_CAPABILITY_POSITION) != 0U &&
            (!isfinite(endpoint->min_position_degrees) ||
             !isfinite(endpoint->max_position_degrees) ||
             endpoint->min_position_degrees >= endpoint->max_position_degrees)) {
            return ROBOT_PROFILE_BAD_LIMIT;
        }
    }
    for (size_t i = 0; i < profile->steering_axis_count; ++i) {
        if (!steering_axis_is_valid(profile, &profile->steering_axes[i])) {
            return ROBOT_PROFILE_BAD_STEERING_AXIS;
        }
        for (size_t j = 0; j < i; ++j) {
            const robot_steering_axis_profile_t *other =
                &profile->steering_axes[j];
            const robot_steering_axis_profile_t *axis =
                &profile->steering_axes[i];
            if (axis->controller_device_id == other->controller_device_id ||
                axis->pwm_device_id == other->pwm_device_id ||
                axis->sensor_device_id == other->sensor_device_id ||
                axis->actuator_endpoint_id == other->actuator_endpoint_id ||
                axis->observation_endpoint_id == other->observation_endpoint_id) {
                return ROBOT_PROFILE_BAD_STEERING_AXIS;
            }
        }
    }
    if (profile->application.kind == ROBOT_PROFILE_DIFFERENTIAL_GEOMETRY &&
        (!isfinite(profile->application.wheelbase_m) || profile->application.wheelbase_m <= 0 ||
         !isfinite(profile->application.track_width_m) || profile->application.track_width_m <= 0 ||
         !isfinite(profile->application.wheel_radius_m) || profile->application.wheel_radius_m <= 0)) return ROBOT_PROFILE_BAD_GEOMETRY;
    return ROBOT_PROFILE_VALID;
}

robot_profile_error_t robot_profile_validate(const robot_profile_t *profile)
{
    return robot_profile_validate_with_registry(profile, &BUILTIN_REGISTRY);
}

const robot_board_profile_t *robot_board_esp32s3_current(void) { return &BOARD; }

const robot_profile_t *robot_profile_selected(void)
{
#ifdef CONFIG_BOTFARMS_PROFILE_RAFA
    return &RAFA;
#elif defined(CONFIG_BOTFARMS_PROFILE_BENCH_SINGLE_STEERING_AS5600)
    return &BENCH_STEERING_AS5600;
#elif defined(CONFIG_BOTFARMS_PROFILE_BENCH_SINGLE_SVD48_MOTOR)
    return &SINGLE_MOTOR;
#else
    return &CURRENT;
#endif
}

const char *robot_profile_selected_name(void) { return robot_profile_selected()->name; }

const robot_bus_profile_t *robot_profile_find_bus_type(const robot_profile_t *profile, robot_bus_type_t type)
{
    if (!profile) return NULL;
    for (size_t index = 0; index < profile->bus_count; ++index)
        if (profile->buses[index].type == type) return &profile->buses[index];
    return NULL;
}

const robot_device_profile_t *robot_profile_find_device_driver(const robot_profile_t *profile, robot_driver_id_t driver, size_t ordinal)
{
    if (!profile) return NULL;
    for (size_t index = 0; index < profile->device_count; ++index) {
        if (profile->devices[index].driver_id == driver) {
            if (ordinal == 0) return &profile->devices[index];
            --ordinal;
        }
    }
    return NULL;
}

const robot_steering_axis_profile_t *robot_profile_find_steering_axis(
    const robot_profile_t *profile,
    uint16_t controller_device_id)
{
    if (!profile || controller_device_id == 0U) {
        return NULL;
    }
    for (size_t index = 0U; index < profile->steering_axis_count; ++index) {
        if (profile->steering_axes[index].controller_device_id ==
            controller_device_id) {
            return &profile->steering_axes[index];
        }
    }
    return NULL;
}
