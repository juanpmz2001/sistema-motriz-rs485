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

static const robot_driver_descriptor_t *find_driver(const robot_driver_registry_t *registry,
                                                     robot_driver_id_t id)
{
    for (size_t index = 0; index < registry->count; ++index) {
        if (registry->items[index].driver_id == id) return &registry->items[index];
    }
    return NULL;
}

robot_profile_error_t robot_profile_validate_with_registry(
    const robot_profile_t *profile, const robot_driver_registry_t *registry)
{
    if (!profile || !registry || profile->schema_version != ROBOT_PROFILE_SCHEMA_VERSION) return ROBOT_PROFILE_BAD_SCHEMA;
    if (!profile->board || profile->bus_count > ROBOT_PROFILE_MAX_BUSES ||
        profile->device_count > ROBOT_PROFILE_MAX_DEVICES ||
        profile->endpoint_count > ROBOT_PROFILE_MAX_ENDPOINTS) return ROBOT_PROFILE_BAD_COUNT;
    uint64_t used_pins = 0;
    for (size_t i = 0; i < profile->bus_count; ++i) {
        const robot_bus_profile_t *bus = &profile->buses[i];
        if (bus->id == 0) return ROBOT_PROFILE_BAD_REFERENCE;
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
                profile->devices[j].address == device->address) return ROBOT_PROFILE_DUPLICATE_ADDRESS;
        }
        const robot_bus_profile_t *bus = find_bus(profile, device->bus_id);
        const robot_driver_descriptor_t *driver = find_driver(registry, device->driver_id);
        if (!bus) return ROBOT_PROFILE_BAD_REFERENCE;
        if (!driver || driver->bus_type != bus->type || device->channel_count == 0 ||
            device->channel_count > driver->max_channels) return ROBOT_PROFILE_BAD_DRIVER;
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
#ifdef CONFIG_BOTFARMS_PROFILE_BENCH_SINGLE_SVD48_MOTOR
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
