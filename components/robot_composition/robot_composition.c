#include "robot_composition.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

#define COORDINATOR_LOCK_TIMEOUT_MS 500U
#define POLLING_STOP_TIMEOUT_MS 2000U
#define STEERING_TASK_STOP_TIMEOUT_MS 2000U
#define STEERING_TASK_STACK_SIZE 4096U
#define STEERING_TASK_PRIORITY 8U
#define STEERING_TASK_PERIOD_MS ROBOT_PROFILE_STEERING_SERVICE_PERIOD_MS
#define RS485_BUS_LOCK_TIMEOUT_MS 1000U
#define I2C_BUS_LOCK_TIMEOUT_MS 100U
#define I2C_EDGE_TIMEOUT_US 1000U

static const char *TAG = "robot_composition";

static uint32_t composition_clock_ms(void *context)
{
    (void)context;
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint64_t composition_clock_ms64(void *context)
{
    (void)context;
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static const robot_bus_profile_t *find_bus_profile(const robot_profile_t *profile,
                                                   uint16_t bus_id)
{
    if (!profile) {
        return NULL;
    }
    for (size_t index = 0; index < profile->bus_count; ++index) {
        if (profile->buses[index].id == bus_id) {
            return &profile->buses[index];
        }
    }
    return NULL;
}

static const robot_device_profile_t *find_device_profile(
    const robot_profile_t *profile,
    uint16_t device_id)
{
    if (!profile) {
        return NULL;
    }
    for (size_t index = 0; index < profile->device_count; ++index) {
        if (profile->devices[index].id == device_id) {
            return &profile->devices[index];
        }
    }
    return NULL;
}

static robot_composition_bus_slot_t *find_bus_slot(robot_composition_t *composition,
                                                   uint16_t bus_id)
{
    for (size_t index = 0; index < ROBOT_PROFILE_MAX_BUSES; ++index) {
        if (composition->buses[index].used &&
            composition->buses[index].profile_bus_id == bus_id) {
            return &composition->buses[index];
        }
    }
    return NULL;
}

static robot_composition_i2c_bus_slot_t *find_i2c_bus_slot(
    robot_composition_t *composition,
    uint16_t bus_id)
{
    if (!composition) {
        return NULL;
    }
    for (size_t index = 0U; index < ROBOT_PROFILE_MAX_BUSES; ++index) {
        if (composition->i2c_buses[index].used &&
            composition->i2c_buses[index].profile_bus_id == bus_id) {
            return &composition->i2c_buses[index];
        }
    }
    return NULL;
}

static robot_composition_device_slot_t *find_device_slot(
    robot_composition_t *composition,
    uint16_t device_id)
{
    for (size_t index = 0; index < ROBOT_PROFILE_MAX_DEVICES; ++index) {
        if (composition->devices[index].used &&
            composition->devices[index].profile_device_id == device_id) {
            return &composition->devices[index];
        }
    }
    return NULL;
}

static robot_composition_pwm_motor_slot_t *find_pwm_motor_slot(
    robot_composition_t *composition,
    uint16_t device_id)
{
    if (!composition) {
        return NULL;
    }
    for (size_t index = 0U; index < ROBOT_PROFILE_MAX_DEVICES; ++index) {
        if (composition->pwm_motors[index].used &&
            composition->pwm_motors[index].profile_device_id == device_id) {
            return &composition->pwm_motors[index];
        }
    }
    return NULL;
}

static robot_composition_as5600_slot_t *find_as5600_slot(
    robot_composition_t *composition,
    uint16_t device_id)
{
    if (!composition) {
        return NULL;
    }
    for (size_t index = 0U; index < ROBOT_PROFILE_MAX_DEVICES; ++index) {
        if (composition->as5600_devices[index].used &&
            composition->as5600_devices[index].profile_device_id == device_id) {
            return &composition->as5600_devices[index];
        }
    }
    return NULL;
}

static robot_composition_steering_slot_t *find_steering_slot(
    robot_composition_t *composition,
    uint16_t device_id)
{
    if (!composition) {
        return NULL;
    }
    for (size_t index = 0U; index < ROBOT_PROFILE_MAX_DEVICES; ++index) {
        if (composition->steering_controllers[index].used &&
            composition->steering_controllers[index].profile_device_id ==
                device_id) {
            return &composition->steering_controllers[index];
        }
    }
    return NULL;
}

static as5600_position_endpoint_adapter_t *find_as5600_observation_adapter(
    robot_composition_t *composition,
    robot_endpoint_id_t endpoint_id)
{
    if (!composition || endpoint_id == 0U) {
        return NULL;
    }
    for (size_t index = 0U;
         index < composition->as5600_observation_adapter_count;
         ++index) {
        as5600_position_endpoint_adapter_t *adapter =
            &composition->as5600_observation_adapters[index];
        if (adapter->endpoint.id == endpoint_id) {
            return adapter;
        }
    }
    return NULL;
}

static bool acquire_device_state(void *context)
{
    robot_composition_device_slot_t *slot = context;
    return slot && slot->state_lock &&
           xSemaphoreTake(slot->state_lock, portMAX_DELAY) == pdTRUE;
}

static void release_device_state(void *context)
{
    robot_composition_device_slot_t *slot = context;
    if (slot && slot->state_lock) {
        xSemaphoreGive(slot->state_lock);
    }
}

static bool acquire_as5600_state(void *context)
{
    robot_composition_as5600_slot_t *slot = context;
    return slot != NULL && slot->state_lock != NULL &&
           xSemaphoreTake(slot->state_lock, portMAX_DELAY) == pdTRUE;
}

static void release_as5600_state(void *context)
{
    robot_composition_as5600_slot_t *slot = context;
    if (slot != NULL && slot->state_lock != NULL) {
        xSemaphoreGive(slot->state_lock);
    }
}

static bool acquire_steering_state(void *context)
{
    robot_composition_steering_slot_t *slot = context;
    return slot != NULL && slot->state_lock != NULL &&
           xSemaphoreTake(slot->state_lock, portMAX_DELAY) == pdTRUE;
}

static void release_steering_state(void *context)
{
    robot_composition_steering_slot_t *slot = context;
    if (slot != NULL && slot->state_lock != NULL) {
        xSemaphoreGive(slot->state_lock);
    }
}

static robot_factory_result_t svd48_factory_validate(
    const robot_driver_factory_t *factory,
    const robot_profile_t *profile,
    const robot_bus_profile_t *bus,
    const robot_device_profile_t *device)
{
    if (!factory || !profile || !bus || !device ||
        factory->driver_id != ROBOT_DRIVER_SVD48 ||
        bus->type != ROBOT_BUS_UART_RS485 || device->driver_id != ROBOT_DRIVER_SVD48 ||
        device->channel_count != SVD48_DEVICE_CHANNEL_COUNT || device->address == 0U ||
        device->address > SVD48_MODBUS_MAX_SLAVE_ID ||
        bus->retries > SVD48_DEVICE_MAX_RETRIES ||
        bus->rate == 0U || bus->response_timeout_ms == 0U ||
        bus->telemetry_period_ms == 0U ||
        bus->telemetry_period_ms >= INT32_MAX ||
        bus->stale_timeout_ms == 0U) {
        return ROBOT_FACTORY_INVALID_CONFIGURATION;
    }
    /* The compatibility maintenance API still addresses devices by slave ID. */
    for (size_t index = 0; index < profile->device_count; ++index) {
        const robot_device_profile_t *other = &profile->devices[index];
        if (other->id != device->id && other->driver_id == ROBOT_DRIVER_SVD48 &&
            other->address == device->address) {
            return ROBOT_FACTORY_INVALID_CONFIGURATION;
        }
    }
    return ROBOT_FACTORY_OK;
}

static size_t svd48_factory_storage_required(
    const robot_driver_factory_t *factory,
    const robot_device_profile_t *device)
{
    return factory && device ? sizeof(robot_composition_device_slot_t) : 0U;
}

static robot_factory_result_t svd48_factory_construct(
    const robot_driver_factory_t *factory,
    void *runtime_context,
    const robot_profile_t *profile,
    const robot_bus_profile_t *bus,
    const robot_device_profile_t *device)
{
    (void)factory;
    (void)profile;
    robot_composition_t *composition = runtime_context;
    if (!composition || !bus || !device ||
        composition->device_count >= ROBOT_PROFILE_MAX_DEVICES ||
        find_device_slot(composition, device->id)) {
        return ROBOT_FACTORY_INVALID_CONFIGURATION;
    }
    robot_composition_bus_slot_t *bus_slot = find_bus_slot(composition, bus->id);
    if (!bus_slot) {
        return ROBOT_FACTORY_CONSTRUCTION_FAILED;
    }
    robot_composition_device_slot_t *slot = NULL;
    for (size_t index = 0; index < ROBOT_PROFILE_MAX_DEVICES; ++index) {
        if (!composition->devices[index].used) {
            slot = &composition->devices[index];
            break;
        }
    }
    if (!slot) {
        return ROBOT_FACTORY_NO_STORAGE;
    }
    memset(slot, 0, sizeof(*slot));
    slot->profile_device_id = device->id;
    slot->profile_bus_id = device->bus_id;
    slot->criticality = device->criticality;
    slot->state_lock = xSemaphoreCreateMutexStatic(&slot->state_lock_storage);
    if (!slot->state_lock) {
        return ROBOT_FACTORY_NO_STORAGE;
    }
    svd48_device_config_t config = {
        .device_id = device->id,
        .address = device->address,
        .transport = rs485_transport_port(&bus_slot->rs485),
        .response_timeout_ms = bus->response_timeout_ms,
        .retries = bus->retries,
        .stale_timeout_ms = bus->stale_timeout_ms,
        .state_lock = {
            .acquire = acquire_device_state,
            .release = release_device_state,
            .context = slot,
        },
        .clock_ms = composition_clock_ms,
        .clock_context = composition,
    };
    if (!svd48_device_init(&slot->svd48, &config)) {
        vSemaphoreDelete(slot->state_lock);
        memset(slot, 0, sizeof(*slot));
        return ROBOT_FACTORY_CONSTRUCTION_FAILED;
    }
    slot->used = true;
    composition->svd48_device_views[composition->device_count] = &slot->svd48;
    composition->device_count++;
    if (!svd48_poll_service_add_device(&composition->polling_service,
                                       &slot->svd48,
                                       bus->telemetry_period_ms)) {
        svd48_device_deinit(&slot->svd48);
        vSemaphoreDelete(slot->state_lock);
        memset(slot, 0, sizeof(*slot));
        composition->device_count--;
        composition->svd48_device_views[composition->device_count] = NULL;
        return ROBOT_FACTORY_CONSTRUCTION_FAILED;
    }
    return ROBOT_FACTORY_OK;
}

static robot_factory_result_t svd48_factory_create_endpoint(
    const robot_driver_factory_t *factory,
    void *runtime_context,
    const robot_profile_t *profile,
    const robot_device_profile_t *device,
    const robot_endpoint_profile_t *endpoint)
{
    (void)factory;
    (void)profile;
    robot_composition_t *composition = runtime_context;
    if (!composition || !device || !endpoint ||
        composition->adapter_count >= ROBOT_PROFILE_MAX_ENDPOINTS ||
        endpoint->channel >= SVD48_DEVICE_CHANNEL_COUNT) {
        return ROBOT_FACTORY_ENDPOINT_FAILED;
    }
    robot_composition_device_slot_t *slot = find_device_slot(composition,
                                                             device->id);
    if (!slot) {
        return ROBOT_FACTORY_ENDPOINT_FAILED;
    }
    svd48_channel_endpoint_adapter_t *adapter =
        &composition->adapters[composition->adapter_count];
    if (!svd48_channel_endpoint_adapter_init(
            adapter,
            svd48_device_channel(&slot->svd48,
                                 (svd48_channel_id_t)endpoint->channel),
            endpoint->id,
            endpoint->name,
            endpoint->criticality,
            endpoint->capabilities,
            endpoint->min_rpm,
            endpoint->max_rpm) ||
        robot_endpoint_registry_add(&composition->registry,
                                    &adapter->endpoint) != ROBOT_REGISTRY_OK) {
        svd48_channel_endpoint_adapter_deinit(adapter);
        return ROBOT_FACTORY_ENDPOINT_FAILED;
    }
    composition->adapter_count++;
    if (composition->legacy_binding_count < SVD48_MOTOR_COUNT) {
        size_t legacy_index = composition->legacy_binding_count++;
        composition->legacy_bindings[legacy_index].device = &slot->svd48;
        composition->legacy_bindings[legacy_index].channel =
            (svd48_channel_id_t)endpoint->channel;
        composition->legacy_endpoint_ids[legacy_index] = endpoint->id;
    }
    return ROBOT_FACTORY_OK;
}

static robot_factory_result_t svd48_factory_start(
    const robot_driver_factory_t *factory,
    void *runtime_context,
    const robot_device_profile_t *device)
{
    (void)factory;
    robot_composition_t *composition = runtime_context;
    robot_composition_device_slot_t *slot = composition
                                                ? find_device_slot(composition,
                                                                   device->id)
                                                : NULL;
    if (!slot) {
        return ROBOT_FACTORY_START_FAILED;
    }
    slot->started = true;
    return ROBOT_FACTORY_OK;
}

static robot_factory_result_t svd48_factory_stop(
    const robot_driver_factory_t *factory,
    void *runtime_context,
    const robot_device_profile_t *device)
{
    (void)factory;
    robot_composition_t *composition = runtime_context;
    robot_composition_device_slot_t *slot = composition
                                                ? find_device_slot(composition,
                                                                   device->id)
                                                : NULL;
    if (!slot) {
        return ROBOT_FACTORY_STOP_FAILED;
    }
    slot->started = false;
    return ROBOT_FACTORY_OK;
}

static void svd48_factory_destroy(const robot_driver_factory_t *factory,
                                  void *runtime_context,
                                  const robot_device_profile_t *device)
{
    (void)factory;
    robot_composition_t *composition = runtime_context;
    robot_composition_device_slot_t *slot = composition
                                                ? find_device_slot(composition,
                                                                   device->id)
                                                : NULL;
    if (!slot) {
        return;
    }
    svd48_device_deinit(&slot->svd48);
    if (slot->state_lock) {
        vSemaphoreDelete(slot->state_lock);
    }
    memset(slot, 0, sizeof(*slot));
}

static const robot_driver_factory_ops_t SVD48_FACTORY_OPS = {
    .validate = svd48_factory_validate,
    .storage_required = svd48_factory_storage_required,
    .construct = svd48_factory_construct,
    .create_endpoint = svd48_factory_create_endpoint,
    .start = svd48_factory_start,
    .stop = svd48_factory_stop,
    .destroy = svd48_factory_destroy,
};

static const robot_steering_axis_profile_t *find_steering_axis_for_pwm(
    const robot_profile_t *profile,
    uint16_t pwm_device_id)
{
    if (!profile) {
        return NULL;
    }
    for (size_t index = 0U; index < profile->steering_axis_count; ++index) {
        if (profile->steering_axes[index].pwm_device_id == pwm_device_id) {
            return &profile->steering_axes[index];
        }
    }
    return NULL;
}

static const robot_steering_axis_profile_t *find_steering_axis_for_sensor(
    const robot_profile_t *profile,
    uint16_t sensor_device_id)
{
    if (!profile) {
        return NULL;
    }
    for (size_t index = 0U; index < profile->steering_axis_count; ++index) {
        if (profile->steering_axes[index].sensor_device_id == sensor_device_id) {
            return &profile->steering_axes[index];
        }
    }
    return NULL;
}

static robot_factory_result_t pwm_motor_factory_validate(
    const robot_driver_factory_t *factory,
    const robot_profile_t *profile,
    const robot_bus_profile_t *bus,
    const robot_device_profile_t *device)
{
    const robot_steering_axis_profile_t *axis =
        find_steering_axis_for_pwm(profile, device ? device->id : 0U);
    if (!factory || !profile || !bus || !device ||
        factory->driver_id != ROBOT_DRIVER_PWM_MOTOR_MODE ||
        bus->type != ROBOT_BUS_PWM ||
        device->driver_id != ROBOT_DRIVER_PWM_MOTOR_MODE ||
        device->channel_count != 1U || bus->rate == 0U ||
        bus->pins[0] < 0 || !axis) {
        return ROBOT_FACTORY_INVALID_CONFIGURATION;
    }
    return ROBOT_FACTORY_OK;
}

static size_t pwm_motor_factory_storage_required(
    const robot_driver_factory_t *factory,
    const robot_device_profile_t *device)
{
    return factory && device ? sizeof(robot_composition_pwm_motor_slot_t) : 0U;
}

static robot_factory_result_t pwm_motor_factory_construct(
    const robot_driver_factory_t *factory,
    void *runtime_context,
    const robot_profile_t *profile,
    const robot_bus_profile_t *bus,
    const robot_device_profile_t *device)
{
    (void)factory;
    robot_composition_t *composition = runtime_context;
    const robot_steering_axis_profile_t *axis =
        find_steering_axis_for_pwm(profile, device ? device->id : 0U);
    if (!composition || !bus || !device || !axis ||
        find_pwm_motor_slot(composition, device->id)) {
        return ROBOT_FACTORY_INVALID_CONFIGURATION;
    }
    robot_composition_pwm_motor_slot_t *slot = NULL;
    size_t slot_index = 0U;
    for (; slot_index < ROBOT_PROFILE_MAX_DEVICES; ++slot_index) {
        if (!composition->pwm_motors[slot_index].used) {
            slot = &composition->pwm_motors[slot_index];
            break;
        }
    }
    if (!slot || (int)LEDC_TIMER_1 + (int)slot_index >= (int)LEDC_TIMER_MAX ||
        (int)LEDC_CHANNEL_4 + (int)slot_index >= (int)LEDC_CHANNEL_MAX) {
        return ROBOT_FACTORY_NO_STORAGE;
    }
    memset(slot, 0, sizeof(*slot));
    motor_mode_pwm_config_t config = {
        .gpio_pin = bus->pins[0],
        .frequency_hz = bus->rate,
        .duty_resolution_bits = 14U,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer = (ledc_timer_t)((int)LEDC_TIMER_1 + (int)slot_index),
        .channel = (ledc_channel_t)((int)LEDC_CHANNEL_4 + (int)slot_index),
        .minimum_pulse_us = axis->pwm_minimum_us,
        .neutral_pulse_us = axis->pwm_neutral_us,
        .maximum_pulse_us = axis->pwm_maximum_us,
    };
    if (motor_mode_pwm_init(&slot->pwm, &config) != ESP_OK) {
        memset(slot, 0, sizeof(*slot));
        return ROBOT_FACTORY_CONSTRUCTION_FAILED;
    }
    slot->used = true;
    slot->profile_device_id = device->id;
    slot->profile_bus_id = bus->id;
    slot->criticality = device->criticality;
    ++composition->pwm_motor_count;
    return ROBOT_FACTORY_OK;
}

static robot_factory_result_t pwm_motor_factory_create_endpoint(
    const robot_driver_factory_t *factory,
    void *runtime_context,
    const robot_profile_t *profile,
    const robot_device_profile_t *device,
    const robot_endpoint_profile_t *endpoint)
{
    (void)factory;
    (void)runtime_context;
    (void)profile;
    (void)device;
    /* Motor-mode PWM is intentionally not an application endpoint.  The
     * steering controller is the sole owner of its physical output. */
    return endpoint ? ROBOT_FACTORY_ENDPOINT_FAILED : ROBOT_FACTORY_OK;
}

static robot_factory_result_t pwm_motor_factory_start(
    const robot_driver_factory_t *factory,
    void *runtime_context,
    const robot_device_profile_t *device)
{
    (void)factory;
    robot_composition_t *composition = runtime_context;
    robot_composition_pwm_motor_slot_t *slot =
        composition && device ? find_pwm_motor_slot(composition, device->id)
                              : NULL;
    if (!slot) {
        return ROBOT_FACTORY_START_FAILED;
    }
    slot->started = true;
    return ROBOT_FACTORY_OK;
}

static robot_factory_result_t pwm_motor_factory_stop(
    const robot_driver_factory_t *factory,
    void *runtime_context,
    const robot_device_profile_t *device)
{
    (void)factory;
    robot_composition_t *composition = runtime_context;
    robot_composition_pwm_motor_slot_t *slot =
        composition && device ? find_pwm_motor_slot(composition, device->id)
                              : NULL;
    if (!slot) {
        return ROBOT_FACTORY_STOP_FAILED;
    }
    slot->started = false;
    return motor_mode_pwm_stop(&slot->pwm) == ESP_OK ? ROBOT_FACTORY_OK
                                                      : ROBOT_FACTORY_STOP_FAILED;
}

static void pwm_motor_factory_destroy(
    const robot_driver_factory_t *factory,
    void *runtime_context,
    const robot_device_profile_t *device)
{
    (void)factory;
    robot_composition_t *composition = runtime_context;
    robot_composition_pwm_motor_slot_t *slot =
        composition && device ? find_pwm_motor_slot(composition, device->id)
                              : NULL;
    if (!slot) {
        return;
    }
    motor_mode_pwm_deinit(&slot->pwm);
    memset(slot, 0, sizeof(*slot));
    if (composition->pwm_motor_count > 0U) {
        --composition->pwm_motor_count;
    }
}

static const robot_driver_factory_ops_t PWM_MOTOR_FACTORY_OPS = {
    .validate = pwm_motor_factory_validate,
    .storage_required = pwm_motor_factory_storage_required,
    .construct = pwm_motor_factory_construct,
    .create_endpoint = pwm_motor_factory_create_endpoint,
    .start = pwm_motor_factory_start,
    .stop = pwm_motor_factory_stop,
    .destroy = pwm_motor_factory_destroy,
};

static as5600_device_result_t as5600_i2c_read(void *context,
                                               uint8_t address,
                                               uint8_t register_address,
                                               uint8_t *out_bytes,
                                               size_t byte_count,
                                               uint32_t timeout_ms)
{
    robot_composition_i2c_bus_slot_t *slot = context;
    if (!slot || !slot->used) {
        return AS5600_DEVICE_NOT_READY;
    }
    esp_err_t error = i2c_bitbang_transport_read_registers(
        &slot->i2c, address, register_address, out_bytes, byte_count, timeout_ms);
    if (error == ESP_OK) {
        return AS5600_DEVICE_OK;
    }
    if (error == ESP_ERR_TIMEOUT) {
        return AS5600_DEVICE_TIMEOUT;
    }
    if (error == ESP_ERR_INVALID_ARG || error == ESP_ERR_INVALID_STATE) {
        return AS5600_DEVICE_INVALID_ARGUMENT;
    }
    return AS5600_DEVICE_IO_ERROR;
}

static bool copy_profile_calibration(
    as5600_calibration_lut_t *destination,
    const robot_as5600_calibration_profile_t *source)
{
    if (!destination || !source || source->format_version != 1U ||
        !source->correction_centidegrees || source->correction_count !=
                                            AS5600_CALIBRATION_LUT_NODE_COUNT) {
        return false;
    }
    memset(destination, 0, sizeof(*destination));
    destination->metadata.format_version = AS5600_CALIBRATION_FORMAT_VERSION;
    destination->metadata.calibration_id = source->id;
    destination->metadata.hardware_identity = source->hardware_identity;
    destination->metadata.provenance = source->provenance_sha256;
    memcpy(destination->correction_centidegrees,
           source->correction_centidegrees,
           sizeof(destination->correction_centidegrees));
    return as5600_calibration_lut_validate(destination);
}

static robot_factory_result_t as5600_factory_validate(
    const robot_driver_factory_t *factory,
    const robot_profile_t *profile,
    const robot_bus_profile_t *bus,
    const robot_device_profile_t *device)
{
    const robot_steering_axis_profile_t *axis =
        find_steering_axis_for_sensor(profile, device ? device->id : 0U);
    if (!factory || !profile || !bus || !device || !axis ||
        factory->driver_id != ROBOT_DRIVER_AS5600 ||
        device->driver_id != ROBOT_DRIVER_AS5600 ||
        bus->type != ROBOT_BUS_I2C || device->channel_count != 1U ||
        device->address == 0U || device->address > 0x7FU || bus->rate == 0U ||
        bus->response_timeout_ms == 0U || bus->stale_timeout_ms == 0U ||
        bus->telemetry_period_ms == 0U ||
        !axis->calibration) {
        return ROBOT_FACTORY_INVALID_CONFIGURATION;
    }
    return ROBOT_FACTORY_OK;
}

static size_t as5600_factory_storage_required(
    const robot_driver_factory_t *factory,
    const robot_device_profile_t *device)
{
    return factory && device ? sizeof(robot_composition_as5600_slot_t) : 0U;
}

static robot_factory_result_t as5600_factory_construct(
    const robot_driver_factory_t *factory,
    void *runtime_context,
    const robot_profile_t *profile,
    const robot_bus_profile_t *bus,
    const robot_device_profile_t *device)
{
    (void)factory;
    robot_composition_t *composition = runtime_context;
    const robot_steering_axis_profile_t *axis =
        find_steering_axis_for_sensor(profile, device ? device->id : 0U);
    if (!composition || !bus || !device || !axis ||
        find_as5600_slot(composition, device->id)) {
        return ROBOT_FACTORY_INVALID_CONFIGURATION;
    }
    robot_composition_i2c_bus_slot_t *bus_slot =
        find_i2c_bus_slot(composition, bus->id);
    if (!bus_slot) {
        return ROBOT_FACTORY_CONSTRUCTION_FAILED;
    }
    robot_composition_as5600_slot_t *slot = NULL;
    for (size_t index = 0U; index < ROBOT_PROFILE_MAX_DEVICES; ++index) {
        if (!composition->as5600_devices[index].used) {
            slot = &composition->as5600_devices[index];
            break;
        }
    }
    if (!slot) {
        return ROBOT_FACTORY_NO_STORAGE;
    }
    memset(slot, 0, sizeof(*slot));
    if (!copy_profile_calibration(&slot->calibration, axis->calibration)) {
        return ROBOT_FACTORY_INVALID_CONFIGURATION;
    }
    slot->calibration_configured = true;
    slot->state_lock = xSemaphoreCreateMutexStatic(&slot->state_lock_storage);
    if (!slot->state_lock) {
        memset(slot, 0, sizeof(*slot));
        return ROBOT_FACTORY_NO_STORAGE;
    }
    const as5600_device_config_t config = {
        .device_id = device->id,
        .i2c_address = device->address,
        .register_read = {.read = as5600_i2c_read, .context = bus_slot},
        .clock_ms = composition_clock_ms,
        .clock_context = composition,
        .response_timeout_ms = bus->response_timeout_ms,
        .stale_timeout_ms = bus->stale_timeout_ms,
        .read_diagnostics = true,
        .calibration = &slot->calibration,
        .state_lock = {
            .acquire = acquire_as5600_state,
            .release = release_as5600_state,
            .context = slot,
        },
    };
    if (!as5600_device_init(&slot->as5600, &config)) {
        vSemaphoreDelete(slot->state_lock);
        memset(slot, 0, sizeof(*slot));
        return ROBOT_FACTORY_CONSTRUCTION_FAILED;
    }
    slot->used = true;
    slot->profile_device_id = device->id;
    slot->profile_bus_id = bus->id;
    slot->criticality = device->criticality;
    slot->telemetry_period_ms = bus->telemetry_period_ms;
    ++composition->as5600_device_count;
    return ROBOT_FACTORY_OK;
}

static robot_factory_result_t as5600_factory_create_endpoint(
    const robot_driver_factory_t *factory,
    void *runtime_context,
    const robot_profile_t *profile,
    const robot_device_profile_t *device,
    const robot_endpoint_profile_t *endpoint)
{
    (void)factory;
    (void)profile;
    robot_composition_t *composition = runtime_context;
    robot_composition_as5600_slot_t *slot =
        composition && device ? find_as5600_slot(composition, device->id) : NULL;
    if (!composition || !slot || !endpoint ||
        endpoint->capabilities != ROBOT_CAPABILITY_POSITION_OBSERVATION ||
        composition->as5600_observation_adapter_count >=
            ROBOT_PROFILE_MAX_ENDPOINTS) {
        return ROBOT_FACTORY_ENDPOINT_FAILED;
    }
    as5600_position_endpoint_adapter_t *adapter =
        &composition->as5600_observation_adapters[
            composition->as5600_observation_adapter_count];
    if (!as5600_position_endpoint_adapter_init(
            adapter,
            &slot->as5600,
            endpoint->id,
            endpoint->name,
            endpoint->criticality,
            slot->calibration_configured ? &slot->calibration : NULL) ||
        robot_endpoint_registry_add(&composition->registry, &adapter->endpoint) !=
            ROBOT_REGISTRY_OK) {
        as5600_position_endpoint_adapter_deinit(adapter);
        return ROBOT_FACTORY_ENDPOINT_FAILED;
    }
    ++composition->as5600_observation_adapter_count;
    return ROBOT_FACTORY_OK;
}

static robot_factory_result_t as5600_factory_start(
    const robot_driver_factory_t *factory,
    void *runtime_context,
    const robot_device_profile_t *device)
{
    (void)factory;
    robot_composition_t *composition = runtime_context;
    robot_composition_as5600_slot_t *slot =
        composition && device ? find_as5600_slot(composition, device->id) : NULL;
    if (!slot) {
        return ROBOT_FACTORY_START_FAILED;
    }
    slot->started = true;
    slot->next_poll_ms = composition_clock_ms(composition);
    return ROBOT_FACTORY_OK;
}

static robot_factory_result_t as5600_factory_stop(
    const robot_driver_factory_t *factory,
    void *runtime_context,
    const robot_device_profile_t *device)
{
    (void)factory;
    robot_composition_t *composition = runtime_context;
    robot_composition_as5600_slot_t *slot =
        composition && device ? find_as5600_slot(composition, device->id) : NULL;
    if (!slot) {
        return ROBOT_FACTORY_STOP_FAILED;
    }
    slot->started = false;
    return ROBOT_FACTORY_OK;
}

static void as5600_factory_destroy(const robot_driver_factory_t *factory,
                                   void *runtime_context,
                                   const robot_device_profile_t *device)
{
    (void)factory;
    robot_composition_t *composition = runtime_context;
    robot_composition_as5600_slot_t *slot =
        composition && device ? find_as5600_slot(composition, device->id) : NULL;
    if (!slot) {
        return;
    }
    as5600_device_deinit(&slot->as5600);
    if (slot->state_lock) {
        vSemaphoreDelete(slot->state_lock);
    }
    memset(slot, 0, sizeof(*slot));
    if (composition->as5600_device_count > 0U) {
        --composition->as5600_device_count;
    }
}

static const robot_driver_factory_ops_t AS5600_FACTORY_OPS = {
    .validate = as5600_factory_validate,
    .storage_required = as5600_factory_storage_required,
    .construct = as5600_factory_construct,
    .create_endpoint = as5600_factory_create_endpoint,
    .start = as5600_factory_start,
    .stop = as5600_factory_stop,
    .destroy = as5600_factory_destroy,
};

static bool steering_pwm_output(void *context, uint16_t pulse_us)
{
    robot_composition_pwm_motor_slot_t *slot = context;
    return slot != NULL && slot->used && slot->pwm.initialized &&
           motor_mode_pwm_set_pulse_us(&slot->pwm, pulse_us) == ESP_OK;
}

static robot_factory_result_t steering_factory_validate(
    const robot_driver_factory_t *factory,
    const robot_profile_t *profile,
    const robot_bus_profile_t *bus,
    const robot_device_profile_t *device)
{
    (void)bus;
    const robot_steering_axis_profile_t *axis =
        robot_profile_find_steering_axis(profile, device ? device->id : 0U);
    if (!factory || !profile || !device ||
        factory->driver_id != ROBOT_DRIVER_STEERING_POSITION_CONTROLLER ||
        device->driver_id != ROBOT_DRIVER_STEERING_POSITION_CONTROLLER ||
        device->bus_id != ROBOT_PROFILE_NO_BUS || device->channel_count != 1U ||
        !axis) {
        return ROBOT_FACTORY_INVALID_CONFIGURATION;
    }
    return ROBOT_FACTORY_OK;
}

static size_t steering_factory_storage_required(
    const robot_driver_factory_t *factory,
    const robot_device_profile_t *device)
{
    return factory && device ? sizeof(robot_composition_steering_slot_t) : 0U;
}

static robot_factory_result_t steering_factory_construct(
    const robot_driver_factory_t *factory,
    void *runtime_context,
    const robot_profile_t *profile,
    const robot_bus_profile_t *bus,
    const robot_device_profile_t *device)
{
    (void)factory;
    (void)profile;
    (void)bus;
    robot_composition_t *composition = runtime_context;
    if (!composition || !device || find_steering_slot(composition, device->id)) {
        return ROBOT_FACTORY_INVALID_CONFIGURATION;
    }
    robot_composition_steering_slot_t *slot = NULL;
    for (size_t index = 0U; index < ROBOT_PROFILE_MAX_DEVICES; ++index) {
        if (!composition->steering_controllers[index].used) {
            slot = &composition->steering_controllers[index];
            break;
        }
    }
    if (!slot) {
        return ROBOT_FACTORY_NO_STORAGE;
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->profile_device_id = device->id;
    slot->criticality = device->criticality;
    ++composition->steering_controller_count;
    return ROBOT_FACTORY_OK;
}

static robot_factory_result_t steering_factory_create_endpoint(
    const robot_driver_factory_t *factory,
    void *runtime_context,
    const robot_profile_t *profile,
    const robot_device_profile_t *device,
    const robot_endpoint_profile_t *endpoint)
{
    (void)factory;
    robot_composition_t *composition = runtime_context;
    const robot_steering_axis_profile_t *axis =
        robot_profile_find_steering_axis(profile, device ? device->id : 0U);
    robot_composition_steering_slot_t *slot =
        composition && device ? find_steering_slot(composition, device->id) : NULL;
    if (!composition || !device || !endpoint || !axis || !slot ||
        endpoint->id != axis->actuator_endpoint_id ||
        endpoint->capabilities != (ROBOT_CAPABILITY_POSITION |
                                   ROBOT_CAPABILITY_STOPPABLE |
                                   ROBOT_CAPABILITY_POSITION_REFERENCE) ||
        slot->adapter.initialized) {
        return ROBOT_FACTORY_ENDPOINT_FAILED;
    }
    robot_composition_pwm_motor_slot_t *pwm =
        find_pwm_motor_slot(composition, axis->pwm_device_id);
    robot_composition_as5600_slot_t *sensor =
        find_as5600_slot(composition, axis->sensor_device_id);
    if (!pwm || !sensor || !sensor->calibration_configured) {
        return ROBOT_FACTORY_ENDPOINT_FAILED;
    }
    slot->state_lock = xSemaphoreCreateMutexStatic(&slot->state_lock_storage);
    if (!slot->state_lock) {
        return ROBOT_FACTORY_NO_STORAGE;
    }
    const steering_position_controller_config_t controller_config = {
        .minimum_position_deg = axis->min_position_degrees,
        .maximum_position_deg = axis->max_position_degrees,
        .neutral_pulse_us = axis->pwm_neutral_us,
        .positive_far_pulse_us = axis->positive_far_us,
        .positive_near_pulse_us = axis->positive_near_us,
        .negative_far_pulse_us = axis->negative_far_us,
        .negative_near_pulse_us = axis->negative_near_us,
        .arrival_min_error_deg = axis->arrival_min_error_degrees,
        .arrival_max_error_deg = axis->arrival_max_error_degrees,
        .full_speed_error_deg = axis->full_speed_error_degrees,
        .reacquire_error_deg = axis->reacquire_error_degrees,
        .stable_sample_count = axis->stable_samples,
        .reacquire_sample_count = axis->reacquire_samples,
        .reversal_settle_ms = axis->reversal_neutral_ms,
        .sensor_stale_timeout_ms = axis->sensor_neutral_after_ms,
        .sensor_fault_timeout_ms = axis->sensor_fault_after_ms,
        .max_command_ttl_ms = axis->command_ttl_ms,
        .move_timeout_ms = axis->move_timeout_ms,
        .allow_degraded_sensor_health =
            axis->allow_magnet_too_weak_for_development,
    };
    const steering_position_endpoint_adapter_config_t config = {
        .as5600 = &sensor->as5600,
        .approved_calibration = &sensor->calibration,
        .endpoint_id = endpoint->id,
        .endpoint_name = endpoint->name,
        .criticality = endpoint->criticality,
        .controller_config = controller_config,
        .clock_ms = composition_clock_ms64,
        .clock_context = composition,
        .pwm_output = steering_pwm_output,
        .pwm_output_context = pwm,
        .position_command_ttl_ms = axis->command_ttl_ms,
        .max_raw_circular_step_counts = axis->max_raw_step_counts,
        .allow_magnet_too_weak_for_development =
            axis->allow_magnet_too_weak_for_development,
        .lock = {
            .acquire = acquire_steering_state,
            .release = release_steering_state,
            .context = slot,
        },
    };
    if (!steering_position_endpoint_adapter_init(&slot->adapter, &config) ||
        robot_endpoint_registry_add(&composition->registry,
                                    &slot->adapter.endpoint) !=
            ROBOT_REGISTRY_OK) {
        steering_position_endpoint_adapter_deinit(&slot->adapter);
        vSemaphoreDelete(slot->state_lock);
        slot->state_lock = NULL;
        return ROBOT_FACTORY_ENDPOINT_FAILED;
    }
    return ROBOT_FACTORY_OK;
}

static robot_factory_result_t steering_factory_start(
    const robot_driver_factory_t *factory,
    void *runtime_context,
    const robot_device_profile_t *device)
{
    (void)factory;
    robot_composition_t *composition = runtime_context;
    robot_composition_steering_slot_t *slot =
        composition && device ? find_steering_slot(composition, device->id)
                              : NULL;
    if (!slot || !slot->adapter.initialized) {
        return ROBOT_FACTORY_START_FAILED;
    }
    slot->started = true;
    return ROBOT_FACTORY_OK;
}

static robot_factory_result_t steering_factory_stop(
    const robot_driver_factory_t *factory,
    void *runtime_context,
    const robot_device_profile_t *device)
{
    (void)factory;
    robot_composition_t *composition = runtime_context;
    robot_composition_steering_slot_t *slot =
        composition && device ? find_steering_slot(composition, device->id)
                              : NULL;
    if (!slot || !slot->adapter.initialized) {
        return ROBOT_FACTORY_STOP_FAILED;
    }
    slot->started = false;
    return steering_position_endpoint_adapter_stop(&slot->adapter) == ROBOT_CAP_OK
               ? ROBOT_FACTORY_OK
               : ROBOT_FACTORY_STOP_FAILED;
}

static void steering_factory_destroy(const robot_driver_factory_t *factory,
                                     void *runtime_context,
                                     const robot_device_profile_t *device)
{
    (void)factory;
    robot_composition_t *composition = runtime_context;
    robot_composition_steering_slot_t *slot =
        composition && device ? find_steering_slot(composition, device->id)
                              : NULL;
    if (!slot) {
        return;
    }
    steering_position_endpoint_adapter_deinit(&slot->adapter);
    if (slot->state_lock) {
        vSemaphoreDelete(slot->state_lock);
    }
    memset(slot, 0, sizeof(*slot));
    if (composition->steering_controller_count > 0U) {
        --composition->steering_controller_count;
    }
}

static const robot_driver_factory_ops_t STEERING_FACTORY_OPS = {
    .validate = steering_factory_validate,
    .storage_required = steering_factory_storage_required,
    .construct = steering_factory_construct,
    .create_endpoint = steering_factory_create_endpoint,
    .start = steering_factory_start,
    .stop = steering_factory_stop,
    .destroy = steering_factory_destroy,
};

static const robot_driver_factory_t EXECUTABLE_FACTORIES[] = {
    {
        .driver_id = ROBOT_DRIVER_SVD48,
        .bus_type = ROBOT_BUS_UART_RS485,
        .capabilities = ROBOT_CAPABILITY_VELOCITY_RPM |
                        ROBOT_CAPABILITY_STOPPABLE,
        .max_channels = SVD48_DEVICE_CHANNEL_COUNT,
        .ops = &SVD48_FACTORY_OPS,
    },
    {
        .driver_id = ROBOT_DRIVER_PWM_MOTOR_MODE,
        .bus_type = ROBOT_BUS_PWM,
        .capabilities = 0U,
        .max_channels = 1U,
        .ops = &PWM_MOTOR_FACTORY_OPS,
    },
    {
        .driver_id = ROBOT_DRIVER_AS5600,
        .bus_type = ROBOT_BUS_I2C,
        .capabilities = ROBOT_CAPABILITY_POSITION_OBSERVATION,
        .max_channels = 1U,
        .ops = &AS5600_FACTORY_OPS,
    },
    {
        .driver_id = ROBOT_DRIVER_STEERING_POSITION_CONTROLLER,
        .bus_type = ROBOT_BUS_NONE,
        .capabilities = ROBOT_CAPABILITY_POSITION |
                        ROBOT_CAPABILITY_STOPPABLE |
                        ROBOT_CAPABILITY_POSITION_REFERENCE,
        .max_channels = 1U,
        .ops = &STEERING_FACTORY_OPS,
    },
};

static const robot_executable_factory_registry_t EXECUTABLE_REGISTRY = {
    .items = EXECUTABLE_FACTORIES,
    .count = sizeof(EXECUTABLE_FACTORIES) / sizeof(EXECUTABLE_FACTORIES[0]),
    .runtime_storage_capacity =
        sizeof(((robot_composition_t *)0)->devices) +
        sizeof(((robot_composition_t *)0)->pwm_motors) +
        sizeof(((robot_composition_t *)0)->as5600_devices) +
        sizeof(((robot_composition_t *)0)->steering_controllers),
    .endpoint_capacity = ROBOT_PROFILE_MAX_ENDPOINTS,
    .legacy_binding_capacity = SVD48_MOTOR_COUNT,
};

const robot_executable_factory_registry_t *robot_composition_factory_registry(void)
{
    return &EXECUTABLE_REGISTRY;
}

static bool acquire_coordinator(void *context)
{
    robot_composition_t *composition = context;
    return composition && composition->coordinator_lock &&
           xSemaphoreTake(composition->coordinator_lock,
                          pdMS_TO_TICKS(COORDINATOR_LOCK_TIMEOUT_MS)) == pdTRUE;
}

static void release_coordinator(void *context)
{
    robot_composition_t *composition = context;
    if (composition && composition->coordinator_lock) {
        xSemaphoreGive(composition->coordinator_lock);
    }
}

static robot_endpoint_id_t endpoint_for_legacy_motor(
    const robot_composition_t *composition,
    uint8_t motor)
{
    return composition && motor < composition->legacy_binding_count
               ? composition->legacy_endpoint_ids[motor]
               : 0U;
}

static bool legacy_motor_for_endpoint(
    const robot_composition_t *composition,
    robot_endpoint_id_t endpoint_id,
    uint8_t *motor)
{
    if (!composition || !motor || endpoint_id == 0U) {
        return false;
    }
    for (size_t index = 0; index < composition->legacy_binding_count; ++index) {
        if (composition->legacy_endpoint_ids[index] == endpoint_id) {
            *motor = (uint8_t)index;
            return true;
        }
    }
    return false;
}

static actuation_application_result_t map_actuation_result(
    actuation_result_t result)
{
    switch (result) {
    case ACTUATION_RESULT_SUCCESS:
        return ACTUATION_APPLICATION_OK;
    case ACTUATION_RESULT_PARTIAL:
        return ACTUATION_APPLICATION_PARTIAL;
    case ACTUATION_RESULT_LOCK_TIMEOUT:
        return ACTUATION_APPLICATION_TIMEOUT;
    default:
        return ACTUATION_APPLICATION_FAILED;
    }
}

static actuation_application_result_t application_set_speed(
    actuation_application_port_t *port,
    uint8_t motor,
    int16_t rpm)
{
    robot_composition_t *composition = port ? port->context : NULL;
    robot_endpoint_id_t endpoint_id = endpoint_for_legacy_motor(composition, motor);
    if (!composition || !composition->constructed || endpoint_id == 0U) {
        return ACTUATION_APPLICATION_INVALID_ARGUMENT;
    }
    actuation_report_t report;
    actuation_result_t result = actuation_coordinator_set_velocity_rpm(
        &composition->coordinator, endpoint_id, rpm, &report);
    if (result == ACTUATION_RESULT_SUCCESS && composition->legacy_robot) {
        (void)robot_control_record_coordinated_motor_speed(
            composition->legacy_robot, motor, rpm);
    }
    return map_actuation_result(result);
}

static actuation_application_result_t application_stop_motor(
    actuation_application_port_t *port,
    uint8_t motor)
{
    robot_composition_t *composition = port ? port->context : NULL;
    robot_endpoint_id_t endpoint_id = endpoint_for_legacy_motor(composition, motor);
    if (!composition || !composition->constructed || endpoint_id == 0U) {
        return ACTUATION_APPLICATION_INVALID_ARGUMENT;
    }
    actuation_report_t report;
    actuation_result_t result = actuation_coordinator_stop_endpoint(
        &composition->coordinator, endpoint_id, &report);
    if (result == ACTUATION_RESULT_SUCCESS && composition->legacy_robot) {
        (void)robot_control_record_coordinated_motor_speed(
            composition->legacy_robot, motor, 0);
    }
    return map_actuation_result(result);
}

static actuation_application_result_t application_stop_all(
    actuation_application_port_t *port)
{
    robot_composition_t *composition = port ? port->context : NULL;
    if (!composition || !composition->constructed) {
        return ACTUATION_APPLICATION_FAILED;
    }
    actuation_report_t report;
    actuation_result_t result = actuation_coordinator_stop_all(
        &composition->coordinator, &report);
    if (result == ACTUATION_RESULT_SUCCESS && composition->legacy_robot) {
        (void)robot_control_record_coordinated_stop(composition->legacy_robot);
    }
    return map_actuation_result(result);
}

static size_t application_motor_count(const actuation_application_port_t *port)
{
    const robot_composition_t *composition = port ? port->context : NULL;
    return composition ? composition->legacy_binding_count : 0U;
}

static bool application_motor_limits(const actuation_application_port_t *port,
                                     uint8_t motor,
                                     int16_t *min_rpm,
                                     int16_t *max_rpm)
{
    const robot_composition_t *composition = port ? port->context : NULL;
    robot_endpoint_id_t endpoint_id = endpoint_for_legacy_motor(composition, motor);
    robot_endpoint_t *endpoint = composition && endpoint_id
                                     ? robot_endpoint_registry_find(
                                           &composition->registry, endpoint_id)
                                     : NULL;
    if (!endpoint || !endpoint->velocity_rpm || !min_rpm || !max_rpm) {
        return false;
    }
    *min_rpm = endpoint->velocity_rpm->min_rpm;
    *max_rpm = endpoint->velocity_rpm->max_rpm;
    return true;
}

static size_t application_endpoint_count(
    const actuation_application_port_t *port)
{
    const robot_composition_t *composition = port ? port->context : NULL;
    return composition && composition->constructed
               ? composition->registry.count
               : 0U;
}

static bool application_endpoint_at(
    const actuation_application_port_t *port,
    size_t index,
    actuation_application_endpoint_info_t *info)
{
    const robot_composition_t *composition = port ? port->context : NULL;
    const robot_endpoint_t *endpoint = composition && composition->constructed
                                           ? robot_endpoint_registry_at(
                                                 &composition->registry, index)
                                           : NULL;
    if (!endpoint || !info) {
        return false;
    }
    memset(info, 0, sizeof(*info));
    info->id = endpoint->id;
    info->name = endpoint->name;
    info->capabilities = robot_endpoint_capabilities(endpoint);
    info->criticality = endpoint->criticality;
    info->available = endpoint->available;
    info->velocity_observation_supported =
        endpoint->velocity_observation != NULL;
    info->position_observation_supported =
        endpoint->position_observation != NULL;
    if (endpoint->velocity_rpm) {
        info->min_rpm = endpoint->velocity_rpm->min_rpm;
        info->max_rpm = endpoint->velocity_rpm->max_rpm;
    }
    if (endpoint->position) {
        info->min_position_degrees = endpoint->position->min_degrees;
        info->max_position_degrees = endpoint->position->max_degrees;
    }
    return true;
}

static actuation_application_result_t application_set_endpoint_speed(
    actuation_application_port_t *port,
    robot_endpoint_id_t endpoint_id,
    int16_t rpm)
{
    robot_composition_t *composition = port ? port->context : NULL;
    if (!composition || !composition->constructed || endpoint_id == 0U ||
        !robot_endpoint_registry_find(&composition->registry, endpoint_id)) {
        return ACTUATION_APPLICATION_INVALID_ARGUMENT;
    }
    actuation_report_t report;
    actuation_result_t result = actuation_coordinator_set_velocity_rpm(
        &composition->coordinator, endpoint_id, rpm, &report);
    uint8_t motor = 0U;
    if (result == ACTUATION_RESULT_SUCCESS && composition->legacy_robot &&
        legacy_motor_for_endpoint(composition, endpoint_id, &motor)) {
        (void)robot_control_record_coordinated_motor_speed(
            composition->legacy_robot, motor, rpm);
    }
    return map_actuation_result(result);
}

static actuation_application_result_t application_apply_endpoint_speeds(
    actuation_application_port_t *port,
    const actuation_application_velocity_request_t *requests,
    size_t request_count)
{
    robot_composition_t *composition = port ? port->context : NULL;
    if (!composition || !composition->constructed || !requests ||
        request_count == 0U ||
        request_count > ACTUATION_COORDINATOR_MAX_SETPOINTS) {
        return ACTUATION_APPLICATION_INVALID_ARGUMENT;
    }

    actuation_velocity_request_t coordinator_requests[
        ACTUATION_COORDINATOR_MAX_SETPOINTS];
    for (size_t index = 0U; index < request_count; ++index) {
        if (requests[index].endpoint_id == 0U ||
            !robot_endpoint_registry_find(&composition->registry,
                                          requests[index].endpoint_id)) {
            return ACTUATION_APPLICATION_INVALID_ARGUMENT;
        }
        coordinator_requests[index].endpoint_id = requests[index].endpoint_id;
        coordinator_requests[index].rpm = requests[index].rpm;
    }

    actuation_report_t report;
    actuation_result_t result = actuation_coordinator_apply_velocity_rpm(
        &composition->coordinator,
        coordinator_requests,
        request_count,
        &report);
    if (result == ACTUATION_RESULT_SUCCESS && composition->legacy_robot) {
        for (size_t index = 0U; index < request_count; ++index) {
            uint8_t motor = 0U;
            if (legacy_motor_for_endpoint(composition,
                                          requests[index].endpoint_id,
                                          &motor)) {
                (void)robot_control_record_coordinated_motor_speed(
                    composition->legacy_robot,
                    motor,
                    requests[index].rpm);
            }
        }
    }
    return map_actuation_result(result);
}

static actuation_application_result_t application_stop_endpoint(
    actuation_application_port_t *port,
    robot_endpoint_id_t endpoint_id)
{
    robot_composition_t *composition = port ? port->context : NULL;
    if (!composition || !composition->constructed || endpoint_id == 0U ||
        !robot_endpoint_registry_find(&composition->registry, endpoint_id)) {
        return ACTUATION_APPLICATION_INVALID_ARGUMENT;
    }
    actuation_report_t report;
    actuation_result_t result = actuation_coordinator_stop_endpoint(
        &composition->coordinator, endpoint_id, &report);
    uint8_t motor = 0U;
    if (result == ACTUATION_RESULT_SUCCESS && composition->legacy_robot &&
        legacy_motor_for_endpoint(composition, endpoint_id, &motor)) {
        (void)robot_control_record_coordinated_motor_speed(
            composition->legacy_robot, motor, 0);
    }
    return map_actuation_result(result);
}

static bool application_get_endpoint_velocity_observation(
    actuation_application_port_t *port,
    robot_endpoint_id_t endpoint_id,
    robot_velocity_observation_t *observation)
{
    robot_composition_t *composition = port ? port->context : NULL;
    robot_endpoint_t *endpoint = composition && composition->constructed
                                     ? robot_endpoint_registry_find(
                                           &composition->registry, endpoint_id)
                                     : NULL;
    return endpoint && observation &&
           robot_endpoint_read_velocity_observation(endpoint, observation) ==
               ROBOT_CAP_OK;
}

static actuation_application_result_t application_set_endpoint_position(
    actuation_application_port_t *port,
    robot_endpoint_id_t endpoint_id,
    float degrees)
{
    robot_composition_t *composition = port ? port->context : NULL;
    if (!composition || !composition->constructed || endpoint_id == 0U ||
        !robot_endpoint_registry_find(&composition->registry, endpoint_id)) {
        return ACTUATION_APPLICATION_INVALID_ARGUMENT;
    }
    actuation_report_t report;
    return map_actuation_result(actuation_coordinator_set_position_degrees(
        &composition->coordinator, endpoint_id, degrees, &report));
}

static actuation_application_result_t application_set_endpoint_position_reference(
    actuation_application_port_t *port,
    robot_endpoint_id_t endpoint_id,
    float degrees)
{
    robot_composition_t *composition = port ? port->context : NULL;
    if (!composition || !composition->constructed || endpoint_id == 0U ||
        !robot_endpoint_registry_find(&composition->registry, endpoint_id)) {
        return ACTUATION_APPLICATION_INVALID_ARGUMENT;
    }
    actuation_report_t report;
    return map_actuation_result(
        actuation_coordinator_set_position_reference_degrees(
            &composition->coordinator, endpoint_id, degrees, &report));
}

static bool application_get_endpoint_position_observation(
    actuation_application_port_t *port,
    robot_endpoint_id_t endpoint_id,
    robot_position_observation_t *observation)
{
    robot_composition_t *composition = port ? port->context : NULL;
    robot_endpoint_t *endpoint = composition && composition->constructed
                                     ? robot_endpoint_registry_find(
                                           &composition->registry, endpoint_id)
                                     : NULL;
    return endpoint && observation &&
           robot_endpoint_read_position_observation(endpoint, observation) ==
               ROBOT_CAP_OK;
}

/* This lower-level read-only path is intentionally not an endpoint capability:
 * L2/L3 may inspect AS5600 phase and magnetic diagnostics, while L4/L5 must
 * remain limited to typed generic position observations. */
static bool composition_read_as5600_diagnostics(
    as5600_diagnostics_port_t *port,
    uint16_t profile_device_id,
    as5600_device_diagnostics_t *diagnostics)
{
    robot_composition_t *composition = port ? port->context : NULL;
    robot_composition_as5600_slot_t *slot =
        composition && composition->constructed && profile_device_id != 0U
            ? find_as5600_slot(composition, profile_device_id)
            : NULL;
    return slot != NULL &&
           as5600_device_get_diagnostics(&slot->as5600, diagnostics);
}

static robot_endpoint_health_t workspace_health_from_svd48(
    svd48_channel_health_t health)
{
    switch (health) {
    case SVD48_CHANNEL_HEALTH_HEALTHY:
        return ROBOT_ENDPOINT_HEALTH_HEALTHY;
    case SVD48_CHANNEL_HEALTH_DEGRADED:
        return ROBOT_ENDPOINT_HEALTH_DEGRADED;
    case SVD48_CHANNEL_HEALTH_OFFLINE:
        return ROBOT_ENDPOINT_HEALTH_OFFLINE;
    case SVD48_CHANNEL_HEALTH_FAULT:
        return ROBOT_ENDPOINT_HEALTH_FAULT;
    case SVD48_CHANNEL_HEALTH_STALE:
        return ROBOT_ENDPOINT_HEALTH_STALE;
    case SVD48_CHANNEL_HEALTH_UNKNOWN:
    default:
        return ROBOT_ENDPOINT_HEALTH_UNKNOWN;
    }
}

static unsigned workspace_health_priority(robot_endpoint_health_t health)
{
    switch (health) {
    case ROBOT_ENDPOINT_HEALTH_FAULT:
        return 5U;
    case ROBOT_ENDPOINT_HEALTH_OFFLINE:
        return 4U;
    case ROBOT_ENDPOINT_HEALTH_STALE:
        return 3U;
    case ROBOT_ENDPOINT_HEALTH_DEGRADED:
        return 2U;
    case ROBOT_ENDPOINT_HEALTH_HEALTHY:
        return 1U;
    case ROBOT_ENDPOINT_HEALTH_UNKNOWN:
    default:
        return 0U;
    }
}

static const robot_endpoint_profile_t *find_svd48_endpoint_profile(
    const robot_profile_t *profile,
    uint16_t device_id,
    svd48_workspace_channel_id_t channel)
{
    if (!profile) {
        return NULL;
    }
    for (size_t index = 0U; index < profile->endpoint_count; ++index) {
        const robot_endpoint_profile_t *endpoint = &profile->endpoints[index];
        if (endpoint->device_id == device_id &&
            endpoint->channel == (uint8_t)channel) {
            return endpoint;
        }
    }
    return NULL;
}

static size_t workspace_controller_count(const svd48_workspace_port_t *port)
{
    const robot_composition_t *composition = port ? port->context : NULL;
    size_t count = 0U;
    if (!composition || !composition->constructed || !composition->profile) {
        return 0U;
    }
    for (size_t index = 0U; index < composition->profile->device_count; ++index) {
        if (composition->profile->devices[index].driver_id == ROBOT_DRIVER_SVD48) {
            ++count;
        }
    }
    return count;
}

static bool workspace_controller_at(
    svd48_workspace_port_t *port,
    size_t requested_index,
    svd48_workspace_controller_info_t *controller)
{
    robot_composition_t *composition = port ? port->context : NULL;
    if (!composition || !composition->constructed || !composition->profile ||
        !controller) {
        return false;
    }

    const robot_device_profile_t *device = NULL;
    size_t svd48_index = 0U;
    for (size_t index = 0U; index < composition->profile->device_count; ++index) {
        const robot_device_profile_t *candidate =
            &composition->profile->devices[index];
        if (candidate->driver_id != ROBOT_DRIVER_SVD48) {
            continue;
        }
        if (svd48_index++ == requested_index) {
            device = candidate;
            break;
        }
    }
    robot_composition_device_slot_t *slot =
        device ? find_device_slot(composition, device->id) : NULL;
    if (!device || !slot) {
        return false;
    }

    memset(controller, 0, sizeof(*controller));
    controller->device_id = device->id;
    controller->bus_id = device->bus_id;
    controller->address = device->address;
    controller->driver = "SVD48";
    controller->available = composition->started && slot->started;
    controller->channel_count = SVD48_WORKSPACE_CHANNEL_COUNT;
    controller->health = ROBOT_ENDPOINT_HEALTH_UNKNOWN;

    for (size_t index = 0U; index < SVD48_WORKSPACE_CHANNEL_COUNT; ++index) {
        svd48_workspace_channel_info_t *channel = &controller->channels[index];
        channel->channel = (svd48_workspace_channel_id_t)index;
        const robot_endpoint_profile_t *endpoint = find_svd48_endpoint_profile(
            composition->profile, device->id, channel->channel);
        if (endpoint) {
            actuation_application_endpoint_info_t application_endpoint;
            channel->endpoint_bound = true;
            channel->endpoint_id = endpoint->id;
            channel->endpoint_name = endpoint->name;
            channel->capabilities = endpoint->capabilities;
            channel->criticality = endpoint->criticality;
            channel->min_rpm = endpoint->min_rpm;
            channel->max_rpm = endpoint->max_rpm;
            channel->available =
                actuation_application_find_endpoint(&composition->application_port,
                                                    endpoint->id,
                                                    &application_endpoint) &&
                application_endpoint.available;
        }
        svd48_channel_t *physical_channel = svd48_device_channel(
            &slot->svd48, (svd48_channel_id_t)index);
        channel->health = workspace_health_from_svd48(
            svd48_channel_get_health(physical_channel));
        if (workspace_health_priority(channel->health) >
            workspace_health_priority(controller->health)) {
            controller->health = channel->health;
        }
    }
    return true;
}

static bool workspace_channel_telemetry(
    svd48_workspace_port_t *port,
    uint16_t device_id,
    svd48_workspace_channel_id_t channel,
    svd48_workspace_channel_telemetry_t *telemetry)
{
    robot_composition_t *composition = port ? port->context : NULL;
    if (!composition || !composition->constructed || !composition->profile ||
        !telemetry || channel >= SVD48_WORKSPACE_CHANNEL_COUNT) {
        return false;
    }
    const robot_device_profile_t *device = find_device_profile(
        composition->profile, device_id);
    robot_composition_device_slot_t *slot =
        device && device->driver_id == ROBOT_DRIVER_SVD48
            ? find_device_slot(composition, device_id)
            : NULL;
    svd48_channel_t *physical_channel = slot
                                            ? svd48_device_channel(
                                                  &slot->svd48,
                                                  (svd48_channel_id_t)channel)
                                            : NULL;
    svd48_channel_snapshot_t snapshot;
    if (!physical_channel ||
        !svd48_channel_get_snapshot(physical_channel, &snapshot)) {
        return false;
    }

    memset(telemetry, 0, sizeof(*telemetry));
    telemetry->device_id = device_id;
    telemetry->channel = channel;
    const robot_endpoint_profile_t *endpoint = find_svd48_endpoint_profile(
        composition->profile, device_id, channel);
    telemetry->endpoint_bound = endpoint != NULL;
    telemetry->endpoint_id = endpoint ? endpoint->id : 0U;
    telemetry->online = snapshot.online;
    telemetry->stale = snapshot.stale;
    telemetry->health = workspace_health_from_svd48(
        svd48_channel_health_from_snapshot(&snapshot));
    telemetry->valid_observations = snapshot.valid_observations;
    telemetry->failed_observations = snapshot.failed_observations;
    telemetry->stale_observations = snapshot.stale_observations;
    telemetry->status = snapshot.status;
    telemetry->observed_speed_rpm = snapshot.observed_speed_rpm;
    telemetry->current_deciamp = snapshot.current_deciamp;
    telemetry->motor_temp_decic = snapshot.motor_temp_decic;
    telemetry->bus_voltage_deciv = snapshot.bus_voltage_deciv;
    telemetry->mos_temp_decic = snapshot.mos_temp_decic;
    telemetry->position_counts = snapshot.position_counts;
    telemetry->error_code = snapshot.error_code;
    telemetry->communication_error = (uint16_t)snapshot.last_error;
    telemetry->last_exception_function = snapshot.last_exception_function;
    telemetry->last_exception_code = snapshot.last_exception_code;
    telemetry->last_exception_ms = snapshot.last_exception_ms;
    return true;
}

static svd48_workspace_hall_calibration_status_t
workspace_hall_calibration_status_from_device(
    svd48_hall_calibration_status_t status)
{
    switch (status) {
    case SVD48_HALL_CALIBRATION_STATUS_SUCCESS:
        return SVD48_WORKSPACE_HALL_CALIBRATION_STATUS_SUCCESS;
    case SVD48_HALL_CALIBRATION_STATUS_CALIBRATING:
        return SVD48_WORKSPACE_HALL_CALIBRATION_STATUS_CALIBRATING;
    case SVD48_HALL_CALIBRATION_STATUS_FAILED:
        return SVD48_WORKSPACE_HALL_CALIBRATION_STATUS_FAILED;
    case SVD48_HALL_CALIBRATION_STATUS_UNKNOWN:
    default:
        return SVD48_WORKSPACE_HALL_CALIBRATION_STATUS_UNKNOWN;
    }
}

static bool workspace_hall_calibrate(
    svd48_workspace_port_t *port,
    uint16_t device_id,
    svd48_workspace_channel_id_t channel,
    svd48_workspace_hall_calibration_result_t *result)
{
    robot_composition_t *composition = port ? port->context : NULL;
    if (!composition || !composition->constructed || !composition->started ||
        !composition->profile || !result ||
        channel >= SVD48_WORKSPACE_CHANNEL_COUNT) {
        return false;
    }
    const robot_device_profile_t *device = find_device_profile(
        composition->profile, device_id);
    robot_composition_device_slot_t *slot =
        device && device->driver_id == ROBOT_DRIVER_SVD48
            ? find_device_slot(composition, device_id)
            : NULL;
    if (!device || !slot || !slot->started) {
        return false;
    }
    svd48_channel_t *physical_channel = svd48_device_channel(
        &slot->svd48, (svd48_channel_id_t)channel);
    if (!physical_channel) {
        return false;
    }

    memset(result, 0, sizeof(*result));
    result->device_id = device_id;
    result->channel = channel;
    result->address = device->address;

    svd48_hall_calibration_result_t device_result = {0};
    svd48_device_result_t operation_result =
        svd48_channel_start_hall_calibration(physical_channel, &device_result);
    result->trigger_register = device_result.trigger_register;
    result->status_register = device_result.status_register;
    result->write_acknowledged = device_result.write_acknowledged;
    result->status_available = device_result.status_available;
    result->status_value = device_result.status_value;
    result->status = workspace_hall_calibration_status_from_device(
        device_result.status);
    result->write_result = (uint16_t)(device_result.write_acknowledged
                                          ? SVD48_DEVICE_OK
                                          : operation_result);
    result->status_read_result = (uint16_t)device_result.status_read_result;
    return true;
}

static const svd48_workspace_ops_t SVD48_WORKSPACE_OPS = {
    .controller_count = workspace_controller_count,
    .controller_at = workspace_controller_at,
    .channel_telemetry = workspace_channel_telemetry,
    .hall_calibrate = workspace_hall_calibrate,
};

static const actuation_application_ops_t APPLICATION_OPS = {
    .set_legacy_motor_speed_rpm = application_set_speed,
    .stop_legacy_motor = application_stop_motor,
    .stop_all = application_stop_all,
    .legacy_motor_count = application_motor_count,
    .legacy_motor_limits_rpm = application_motor_limits,
    .endpoint_count = application_endpoint_count,
    .endpoint_at = application_endpoint_at,
    .set_endpoint_speed_rpm = application_set_endpoint_speed,
    .apply_endpoint_speeds_rpm = application_apply_endpoint_speeds,
    .stop_endpoint = application_stop_endpoint,
    .get_endpoint_velocity_observation =
        application_get_endpoint_velocity_observation,
    .set_endpoint_position_degrees = application_set_endpoint_position,
    .set_endpoint_position_reference_degrees =
        application_set_endpoint_position_reference,
    .get_endpoint_position_observation =
        application_get_endpoint_position_observation,
};

static const as5600_diagnostics_port_ops_t AS5600_DIAGNOSTICS_PORT_OPS = {
    .read = composition_read_as5600_diagnostics,
};

static robot_capability_error_t project_steering_cyclic_phase(
    void *context,
    float corrected_cyclic_degrees,
    uint32_t sample_timestamp_ms,
    float *logical_degrees)
{
    return steering_position_endpoint_adapter_project_cyclic_phase(
        context,
        corrected_cyclic_degrees,
        sample_timestamp_ms,
        logical_degrees);
}

/* Bind reference frames only after every profile endpoint has been built.
 * This preserves profile declaration order independence and keeps the AS5600
 * adapter free of PWM/controller dependencies. */
static bool bind_steering_observation_frames(robot_composition_t *composition)
{
    if (!composition || !composition->profile) {
        return false;
    }
    for (size_t index = 0U;
         index < composition->profile->steering_axis_count;
         ++index) {
        const robot_steering_axis_profile_t *axis =
            &composition->profile->steering_axes[index];
        robot_composition_steering_slot_t *controller =
            find_steering_slot(composition, axis->controller_device_id);
        as5600_position_endpoint_adapter_t *observation =
            find_as5600_observation_adapter(composition,
                                            axis->observation_endpoint_id);
        const as5600_position_coordinate_frame_t frame = {
            .project_cyclic_phase = project_steering_cyclic_phase,
            .context = controller ? &controller->adapter : NULL,
        };
        if (!controller || !controller->adapter.initialized || !observation ||
            !as5600_position_endpoint_adapter_set_coordinate_frame(
                observation, &frame)) {
            composition->diagnostics.endpoint_id =
                axis->observation_endpoint_id;
            composition->diagnostics.device_id = axis->sensor_device_id;
            composition->diagnostics.driver_id = ROBOT_DRIVER_AS5600;
            return false;
        }
    }
    return true;
}

static bool bus_is_referenced(const robot_profile_t *profile, uint16_t bus_id)
{
    for (size_t index = 0; index < profile->device_count; ++index) {
        if (profile->devices[index].bus_id == bus_id) {
            return true;
        }
    }
    return false;
}

static esp_err_t construct_buses(robot_composition_t *composition)
{
    for (size_t index = 0; index < composition->profile->bus_count; ++index) {
        const robot_bus_profile_t *bus = &composition->profile->buses[index];
        if (!bus_is_referenced(composition->profile, bus->id)) {
            continue;
        }
        if (bus->type == ROBOT_BUS_UART_RS485) {
            if (composition->bus_count >= ROBOT_PROFILE_MAX_BUSES) {
                composition->diagnostics.code =
                    ROBOT_COMPOSITION_DIAGNOSTIC_BUS_INCOMPATIBLE;
                composition->diagnostics.stage =
                    ROBOT_COMPOSITION_STAGE_BUS_CONSTRUCT;
                composition->diagnostics.bus_id = bus->id;
                return ESP_ERR_NOT_SUPPORTED;
            }
            robot_composition_bus_slot_t *slot =
                &composition->buses[composition->bus_count];
            rs485_transport_config_t config = {
                .uart_port = (uart_port_t)bus->peripheral,
                .tx_pin = bus->pins[0],
                .rx_pin = bus->pins[1],
                .rts_pin = UART_PIN_NO_CHANGE,
                .use_rs485_half_duplex = false,
                .baud_rate = bus->rate,
                .lock_timeout_ms = RS485_BUS_LOCK_TIMEOUT_MS,
            };
            esp_err_t error = rs485_transport_init(&slot->rs485, &config);
            if (error != ESP_OK) {
                composition->diagnostics.code =
                    ROBOT_COMPOSITION_DIAGNOSTIC_CONSTRUCTION_FAILED;
                composition->diagnostics.stage =
                    ROBOT_COMPOSITION_STAGE_BUS_CONSTRUCT;
                composition->diagnostics.bus_id = bus->id;
                return error;
            }
            slot->used = true;
            slot->profile_bus_id = bus->id;
            composition->bus_count++;
            continue;
        }
        if (bus->type == ROBOT_BUS_I2C) {
            if (composition->i2c_bus_count >= ROBOT_PROFILE_MAX_BUSES ||
                bus->rate == 0U || bus->rate > 500000U) {
                composition->diagnostics.code =
                    ROBOT_COMPOSITION_DIAGNOSTIC_BUS_INCOMPATIBLE;
                composition->diagnostics.stage =
                    ROBOT_COMPOSITION_STAGE_BUS_CONSTRUCT;
                composition->diagnostics.bus_id = bus->id;
                return ESP_ERR_NOT_SUPPORTED;
            }
            robot_composition_i2c_bus_slot_t *slot =
                &composition->i2c_buses[composition->i2c_bus_count];
            const uint32_t half_period_us = 500000U / bus->rate;
            i2c_bitbang_transport_config_t config = {
                .sda_pin = (gpio_num_t)bus->pins[0],
                .scl_pin = (gpio_num_t)bus->pins[1],
                .half_period_us = half_period_us == 0U ? 1U : half_period_us,
                .edge_timeout_us = I2C_EDGE_TIMEOUT_US,
                .lock_timeout_ms = I2C_BUS_LOCK_TIMEOUT_MS,
                /* The empirical fixture was characterized with pull-ups. The
                 * internal ones are a weak backup, not a replacement for a
                 * reviewed physical bus/pull-up design. */
                .enable_internal_pullups = true,
            };
            esp_err_t error = i2c_bitbang_transport_init(&slot->i2c, &config);
            if (error != ESP_OK) {
                composition->diagnostics.code =
                    ROBOT_COMPOSITION_DIAGNOSTIC_CONSTRUCTION_FAILED;
                composition->diagnostics.stage =
                    ROBOT_COMPOSITION_STAGE_BUS_CONSTRUCT;
                composition->diagnostics.bus_id = bus->id;
                return error;
            }
            slot->used = true;
            slot->profile_bus_id = bus->id;
            composition->i2c_bus_count++;
            continue;
        }
        if (bus->type == ROBOT_BUS_PWM) {
            /* PWM has no shared transport object; the executable PWM factory
             * owns its dedicated LEDC timer/channel after profile validation. */
            continue;
        }
        {
            composition->diagnostics.code =
                ROBOT_COMPOSITION_DIAGNOSTIC_BUS_INCOMPATIBLE;
            composition->diagnostics.stage = ROBOT_COMPOSITION_STAGE_BUS_CONSTRUCT;
            composition->diagnostics.bus_id = bus->id;
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    return ESP_OK;
}

static bool polling_task_is_quiesced(const svd48_poll_task_t *task)
{
    return task && !task->service && !task->task && !task->running &&
           !task->stopped && !task->stop_requested;
}

static bool composition_time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

/* Schedule from the previous deadline instead of completion time. A slow
 * one-shot diagnostic read must skip a missed 40 ms slot rather than drift
 * every subsequent primary sample later and later. */
static uint32_t composition_next_periodic_deadline(uint32_t previous_deadline_ms,
                                                   uint32_t now_ms,
                                                   uint32_t period_ms)
{
    if (period_ms == 0U) {
        return now_ms;
    }
    uint32_t next_deadline_ms = previous_deadline_ms + period_ms;
    while (composition_time_reached(now_ms, next_deadline_ms)) {
        next_deadline_ms += period_ms;
    }
    return next_deadline_ms;
}

static void steering_service_task(void *argument)
{
    robot_composition_t *composition = argument;
    if (!composition) {
        vTaskDelete(NULL);
        return;
    }
    composition->steering_task.running = true;
    while (!composition->steering_task.stop_requested) {
        const uint32_t now_ms = composition_clock_ms(composition);
        for (size_t index = 0U; index < ROBOT_PROFILE_MAX_DEVICES; ++index) {
            robot_composition_as5600_slot_t *sensor =
                &composition->as5600_devices[index];
            if (!sensor->used || !sensor->started ||
                !composition_time_reached(now_ms, sensor->next_poll_ms)) {
                continue;
            }
            (void)as5600_device_poll(&sensor->as5600);
            sensor->next_poll_ms = composition_next_periodic_deadline(
                sensor->next_poll_ms,
                composition_clock_ms(composition),
                sensor->telemetry_period_ms);
        }
        for (size_t index = 0U; index < ROBOT_PROFILE_MAX_DEVICES; ++index) {
            robot_composition_steering_slot_t *controller =
                &composition->steering_controllers[index];
            if (controller->used && controller->started) {
                (void)steering_position_endpoint_adapter_tick(
                    &controller->adapter, NULL);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(STEERING_TASK_PERIOD_MS));
    }
    composition->steering_task.running = false;
    composition->steering_task.task = NULL;
    xSemaphoreGive(composition->steering_task.stopped);
    vTaskDelete(NULL);
}

static bool steering_task_is_quiesced(
    const robot_composition_steering_task_t *task)
{
    return task && !task->task && !task->stopped && !task->running &&
           !task->stop_requested;
}

static esp_err_t steering_task_start(robot_composition_t *composition)
{
    if (!composition || composition->as5600_device_count == 0U) {
        return ESP_OK;
    }
    robot_composition_steering_task_t *task = &composition->steering_task;
    if (!steering_task_is_quiesced(task)) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(task, 0, sizeof(*task));
    task->stopped = xSemaphoreCreateBinaryStatic(&task->stopped_storage);
    if (!task->stopped) {
        memset(task, 0, sizeof(*task));
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(steering_service_task,
                    "steering_ctl",
                    STEERING_TASK_STACK_SIZE,
                    composition,
                    STEERING_TASK_PRIORITY,
                    &task->task) != pdPASS) {
        vSemaphoreDelete(task->stopped);
        memset(task, 0, sizeof(*task));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t steering_task_stop(robot_composition_t *composition,
                                    uint32_t timeout_ms)
{
    if (!composition) {
        return ESP_ERR_INVALID_ARG;
    }
    robot_composition_steering_task_t *task = &composition->steering_task;
    if (!task->stopped) {
        if (task->task || task->running) {
            return ESP_ERR_INVALID_STATE;
        }
        memset(task, 0, sizeof(*task));
        return ESP_OK;
    }
    task->stop_requested = true;
    if (xSemaphoreTake(task->stopped, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    vSemaphoreDelete(task->stopped);
    memset(task, 0, sizeof(*task));
    return ESP_OK;
}

static bool cleanup_runtime(robot_composition_t *composition)
{
    if (!composition) {
        return false;
    }
    if (composition->started || composition->polling_task.service ||
        composition->polling_task.task ||
        composition->polling_task.running || composition->polling_task.stopped ||
        composition->polling_task.stop_requested ||
        composition->steering_task.task || composition->steering_task.running ||
        composition->steering_task.stopped ||
        composition->steering_task.stop_requested) {
        (void)robot_composition_stop(composition);
    }
    /* Preserve dependencies until a timed-out poll task is fully collected. */
    if (!polling_task_is_quiesced(&composition->polling_task) ||
        !steering_task_is_quiesced(&composition->steering_task)) {
        ESP_LOGE(TAG,
                 "Polling/control task did not stop; preserving runtime dependencies");
        return false;
    }
    if (composition->legacy_svd48) {
        svd48_deinit(composition->legacy_svd48);
        composition->legacy_svd48 = NULL;
    }
    for (size_t index = composition->adapter_count; index > 0U; --index) {
        svd48_channel_endpoint_adapter_deinit(
            &composition->adapters[index - 1U]);
    }
    composition->adapter_count = 0U;
    for (size_t index = composition->as5600_observation_adapter_count;
         index > 0U;
         --index) {
        as5600_position_endpoint_adapter_deinit(
            &composition->as5600_observation_adapters[index - 1U]);
    }
    composition->as5600_observation_adapter_count = 0U;
    if (composition->profile) {
        for (size_t index = composition->profile->device_count; index > 0U; --index) {
            const robot_device_profile_t *device =
                &composition->profile->devices[index - 1U];
            const robot_driver_factory_t *factory = robot_executable_factory_find(
                &EXECUTABLE_REGISTRY, device->driver_id);
            if (factory && factory->ops && factory->ops->destroy) {
                factory->ops->destroy(factory, composition, device);
            }
        }
    }
    composition->device_count = 0U;
    svd48_poll_service_reset(&composition->polling_service);
    for (size_t index = composition->bus_count; index > 0U; --index) {
        rs485_transport_deinit(&composition->buses[index - 1U].rs485);
        memset(&composition->buses[index - 1U],
               0,
               sizeof(composition->buses[index - 1U]));
    }
    composition->bus_count = 0U;
    for (size_t index = composition->i2c_bus_count; index > 0U; --index) {
        i2c_bitbang_transport_deinit(
            &composition->i2c_buses[index - 1U].i2c);
        memset(&composition->i2c_buses[index - 1U],
               0,
               sizeof(composition->i2c_buses[index - 1U]));
    }
    composition->i2c_bus_count = 0U;
    if (composition->coordinator_lock) {
        vSemaphoreDelete(composition->coordinator_lock);
        composition->coordinator_lock = NULL;
    }
    composition->constructed = false;
    composition->started = false;
    composition->diagnostics.runtime_ready = false;
    return true;
}

static esp_err_t fail_after_cleanup(robot_composition_t *composition,
                                    esp_err_t error)
{
    robot_composition_diagnostics_t diagnostics = composition->diagnostics;
    const robot_profile_t *profile = composition->profile;
    if (!cleanup_runtime(composition)) {
        return ESP_ERR_TIMEOUT;
    }
    memset(composition, 0, sizeof(*composition));
    composition->profile = profile;
    composition->diagnostics = diagnostics;
    composition->application_port.ops = &APPLICATION_OPS;
    composition->application_port.context = composition;
    composition->svd48_workspace_port.ops = &SVD48_WORKSPACE_OPS;
    composition->svd48_workspace_port.context = composition;
    composition->as5600_diagnostics_port.ops = &AS5600_DIAGNOSTICS_PORT_OPS;
    composition->as5600_diagnostics_port.context = composition;
    return error;
}

esp_err_t robot_composition_init(robot_composition_t *composition,
                                 const robot_profile_t *profile)
{
    if (!composition || !profile) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(composition, 0, sizeof(*composition));
    composition->profile = profile;
    composition->application_port.ops = &APPLICATION_OPS;
    composition->application_port.context = composition;
    composition->svd48_workspace_port.ops = &SVD48_WORKSPACE_OPS;
    composition->svd48_workspace_port.context = composition;
    composition->as5600_diagnostics_port.ops = &AS5600_DIAGNOSTICS_PORT_OPS;
    composition->as5600_diagnostics_port.context = composition;
    if (!robot_composition_preflight(profile,
                                     &EXECUTABLE_REGISTRY,
                                     &composition->diagnostics)) {
        return composition->diagnostics.schema_valid ? ESP_ERR_NOT_SUPPORTED
                                                     : ESP_ERR_INVALID_ARG;
    }
    if (!svd48_poll_service_init(&composition->polling_service,
                                 composition_clock_ms,
                                 composition)) {
        composition->diagnostics.code =
            ROBOT_COMPOSITION_DIAGNOSTIC_CONSTRUCTION_FAILED;
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = construct_buses(composition);
    if (error != ESP_OK) {
        return fail_after_cleanup(composition, error);
    }
    robot_endpoint_registry_init(&composition->registry);
    for (size_t index = 0; index < profile->device_count; ++index) {
        const robot_device_profile_t *device = &profile->devices[index];
        const robot_bus_profile_t *bus = find_bus_profile(profile, device->bus_id);
        const robot_driver_factory_t *factory = robot_executable_factory_find(
            &EXECUTABLE_REGISTRY, device->driver_id);
        composition->diagnostics.stage = ROBOT_COMPOSITION_STAGE_DEVICE_CONSTRUCT;
        composition->diagnostics.driver_id = device->driver_id;
        composition->diagnostics.device_id = device->id;
        composition->diagnostics.bus_id = device->bus_id;
        robot_factory_result_t result = factory->ops->construct(
            factory, composition, profile, bus, device);
        if (result != ROBOT_FACTORY_OK) {
            composition->diagnostics.code =
                ROBOT_COMPOSITION_DIAGNOSTIC_CONSTRUCTION_FAILED;
            composition->diagnostics.factory_result = result;
            return fail_after_cleanup(composition, ESP_FAIL);
        }
    }
    for (size_t index = 0; index < profile->endpoint_count; ++index) {
        const robot_endpoint_profile_t *endpoint = &profile->endpoints[index];
        const robot_device_profile_t *device = find_device_profile(
            profile, endpoint->device_id);
        const robot_driver_factory_t *factory = device
                                                    ? robot_executable_factory_find(
                                                          &EXECUTABLE_REGISTRY,
                                                          device->driver_id)
                                                    : NULL;
        composition->diagnostics.stage = ROBOT_COMPOSITION_STAGE_ENDPOINT_CONSTRUCT;
        composition->diagnostics.endpoint_id = endpoint->id;
        composition->diagnostics.device_id = endpoint->device_id;
        robot_factory_result_t result = factory
                                            ? factory->ops->create_endpoint(
                                                  factory,
                                                  composition,
                                                  profile,
                                                  device,
                                                  endpoint)
                                            : ROBOT_FACTORY_ENDPOINT_FAILED;
        if (result != ROBOT_FACTORY_OK) {
            composition->diagnostics.code =
                ROBOT_COMPOSITION_DIAGNOSTIC_ENDPOINT_FAILED;
            composition->diagnostics.factory_result = result;
            return fail_after_cleanup(composition, ESP_ERR_INVALID_ARG);
        }
    }
    if (!bind_steering_observation_frames(composition)) {
        composition->diagnostics.code =
            ROBOT_COMPOSITION_DIAGNOSTIC_ENDPOINT_FAILED;
        composition->diagnostics.factory_result = ROBOT_FACTORY_ENDPOINT_FAILED;
        return fail_after_cleanup(composition, ESP_ERR_INVALID_ARG);
    }
    if (composition->legacy_binding_count > 0U) {
        composition->legacy_svd48 = svd48_attach_devices(
            composition->svd48_device_views,
            composition->device_count,
            composition->legacy_bindings,
            composition->legacy_binding_count);
        if (!composition->legacy_svd48) {
            composition->diagnostics.code =
                ROBOT_COMPOSITION_DIAGNOSTIC_CONSTRUCTION_FAILED;
            return fail_after_cleanup(composition, ESP_ERR_NO_MEM);
        }
    }
    composition->coordinator_lock = xSemaphoreCreateMutexStatic(
        &composition->coordinator_lock_storage);
    const actuation_lock_port_t lock = {
        .acquire = acquire_coordinator,
        .release = release_coordinator,
        .context = composition,
    };
    if (!composition->coordinator_lock ||
        !actuation_coordinator_init(&composition->coordinator,
                                    &composition->registry,
                                    &lock)) {
        composition->diagnostics.code =
            ROBOT_COMPOSITION_DIAGNOSTIC_CONSTRUCTION_FAILED;
        return fail_after_cleanup(composition, ESP_ERR_NO_MEM);
    }
    composition->constructed = true;
    composition->diagnostics.code = ROBOT_COMPOSITION_DIAGNOSTIC_OK;
    composition->diagnostics.stage = ROBOT_COMPOSITION_STAGE_NONE;
    composition->diagnostics.factory_result = ROBOT_FACTORY_OK;
    composition->diagnostics.driver_id = 0;
    composition->diagnostics.bus_id = 0U;
    composition->diagnostics.device_id = 0U;
    composition->diagnostics.endpoint_id = 0U;
    ESP_LOGI(TAG,
             "profile %s composed buses=%u devices=%u endpoints=%u",
             profile->name,
             (unsigned)(composition->bus_count + composition->i2c_bus_count),
             (unsigned)profile->device_count,
             (unsigned)composition->registry.count);
    return ESP_OK;
}

esp_err_t robot_composition_start(robot_composition_t *composition)
{
    if (!composition || !composition->constructed || !composition->profile) {
        return ESP_ERR_INVALID_ARG;
    }
    if (composition->started) {
        return ESP_OK;
    }
    if (!polling_task_is_quiesced(&composition->polling_task) ||
        !steering_task_is_quiesced(&composition->steering_task)) {
        composition->diagnostics.code = ROBOT_COMPOSITION_DIAGNOSTIC_START_FAILED;
        composition->diagnostics.stage = ROBOT_COMPOSITION_STAGE_SERVICE_START;
        return ESP_ERR_INVALID_STATE;
    }
    composition->diagnostics.stage = ROBOT_COMPOSITION_STAGE_SERVICE_START;
    for (size_t index = 0; index < composition->profile->device_count; ++index) {
        const robot_device_profile_t *device = &composition->profile->devices[index];
        const robot_driver_factory_t *factory = robot_executable_factory_find(
            &EXECUTABLE_REGISTRY, device->driver_id);
        robot_factory_result_t result = factory->ops->start(factory,
                                                            composition,
                                                            device);
        if (result != ROBOT_FACTORY_OK) {
            composition->diagnostics.code =
                ROBOT_COMPOSITION_DIAGNOSTIC_START_FAILED;
            composition->diagnostics.factory_result = result;
            (void)robot_composition_stop(composition);
            return ESP_FAIL;
        }
    }
    esp_err_t error = ESP_OK;
    if (composition->polling_service.count > 0U) {
        error = svd48_poll_task_start(&composition->polling_task,
                                      &composition->polling_service);
    }
    if (error == ESP_OK) {
        error = steering_task_start(composition);
    }
    if (error != ESP_OK) {
        composition->diagnostics.code = ROBOT_COMPOSITION_DIAGNOSTIC_START_FAILED;
        composition->diagnostics.factory_result = ROBOT_FACTORY_START_FAILED;
        (void)robot_composition_stop(composition);
        return error;
    }
    composition->started = true;
    composition->diagnostics.runtime_ready = true;
    composition->diagnostics.code = ROBOT_COMPOSITION_DIAGNOSTIC_OK;
    composition->diagnostics.stage = ROBOT_COMPOSITION_STAGE_NONE;
    composition->diagnostics.driver_id = 0;
    composition->diagnostics.bus_id = 0U;
    composition->diagnostics.device_id = 0U;
    composition->diagnostics.endpoint_id = 0U;
    return ESP_OK;
}

esp_err_t robot_composition_stop(robot_composition_t *composition)
{
    if (!composition) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t first_error = ESP_OK;
    /* Ask every live endpoint to neutral before collecting either service
     * task. If a task later fails to quiesce, its dependencies and outputs are
     * preserved, but no old controller target is intentionally left active. */
    if (composition->coordinator.registry &&
        composition->coordinator.registry->count > 0U) {
        actuation_report_t report;
        actuation_result_t result = actuation_coordinator_stop_all(
            &composition->coordinator, &report);
        if (result != ACTUATION_RESULT_SUCCESS && first_error == ESP_OK) {
            first_error = ESP_FAIL;
        }
    }
    if (composition->steering_task.task || composition->steering_task.running ||
        composition->steering_task.stopped ||
        composition->steering_task.stop_requested) {
        esp_err_t error = steering_task_stop(composition,
                                             STEERING_TASK_STOP_TIMEOUT_MS);
        if (error != ESP_OK) {
            composition->diagnostics.code =
                ROBOT_COMPOSITION_DIAGNOSTIC_START_FAILED;
            composition->diagnostics.stage =
                ROBOT_COMPOSITION_STAGE_SERVICE_START;
            /* Never stop/destroy factories while this task may still access
             * them. cleanup_runtime will preserve the same dependencies until
             * an operator-visible retry can collect the task. */
            return error;
        }
    }
    if (composition->polling_task.service || composition->polling_task.task ||
        composition->polling_task.running ||
        composition->polling_task.stopped ||
        composition->polling_task.stop_requested) {
        esp_err_t error = svd48_poll_task_stop(&composition->polling_task,
                                              POLLING_STOP_TIMEOUT_MS);
        if (error != ESP_OK) {
            composition->diagnostics.code =
                ROBOT_COMPOSITION_DIAGNOSTIC_START_FAILED;
            composition->diagnostics.stage =
                ROBOT_COMPOSITION_STAGE_SERVICE_START;
            return error;
        }
    }
    if (composition->profile) {
        for (size_t index = composition->profile->device_count; index > 0U; --index) {
            const robot_device_profile_t *device =
                &composition->profile->devices[index - 1U];
            const robot_driver_factory_t *factory = robot_executable_factory_find(
                &EXECUTABLE_REGISTRY, device->driver_id);
            if (!factory || factory->ops->stop(factory, composition, device) !=
                                ROBOT_FACTORY_OK) {
                if (first_error == ESP_OK) {
                    first_error = ESP_FAIL;
                }
            }
        }
    }
    composition->started = false;
    composition->diagnostics.runtime_ready = false;
    return first_error;
}

void robot_composition_deinit(robot_composition_t *composition)
{
    if (!composition) {
        return;
    }
    if (!cleanup_runtime(composition)) {
        return;
    }
    memset(composition, 0, sizeof(*composition));
}

void robot_composition_attach_legacy_robot(robot_composition_t *composition,
                                           robot_control_handle_t legacy_robot)
{
    if (composition) {
        composition->legacy_robot = legacy_robot;
    }
}

svd48_handle_t robot_composition_legacy_svd48(robot_composition_t *composition)
{
    return composition && composition->constructed
               ? composition->legacy_svd48
               : NULL;
}

svd48_workspace_port_t *robot_composition_svd48_workspace_port(
    robot_composition_t *composition)
{
    return composition && composition->constructed
               ? &composition->svd48_workspace_port
               : NULL;
}

as5600_diagnostics_port_t *robot_composition_as5600_diagnostics_port(
    robot_composition_t *composition)
{
    return composition && composition->constructed
               ? &composition->as5600_diagnostics_port
               : NULL;
}

const robot_composition_diagnostics_t *robot_composition_get_diagnostics(
    const robot_composition_t *composition)
{
    return composition ? &composition->diagnostics : NULL;
}
