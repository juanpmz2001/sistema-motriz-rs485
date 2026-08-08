#include "robot_composition.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

#define COORDINATOR_LOCK_TIMEOUT_MS 500U
#define POLLING_STOP_TIMEOUT_MS 2000U
#define RS485_BUS_LOCK_TIMEOUT_MS 1000U

static const char *TAG = "robot_composition";

static uint32_t composition_clock_ms(void *context)
{
    (void)context;
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
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

static const robot_driver_factory_t EXECUTABLE_FACTORIES[] = {
    {
        .driver_id = ROBOT_DRIVER_SVD48,
        .bus_type = ROBOT_BUS_UART_RS485,
        .capabilities = ROBOT_CAPABILITY_VELOCITY_RPM |
                        ROBOT_CAPABILITY_STOPPABLE,
        .max_channels = SVD48_DEVICE_CHANNEL_COUNT,
        .ops = &SVD48_FACTORY_OPS,
    },
};

static const robot_executable_factory_registry_t EXECUTABLE_REGISTRY = {
    .items = EXECUTABLE_FACTORIES,
    .count = sizeof(EXECUTABLE_FACTORIES) / sizeof(EXECUTABLE_FACTORIES[0]),
    .runtime_storage_capacity =
        sizeof(((robot_composition_t *)0)->devices),
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

static const actuation_application_ops_t APPLICATION_OPS = {
    .set_legacy_motor_speed_rpm = application_set_speed,
    .stop_legacy_motor = application_stop_motor,
    .stop_all = application_stop_all,
    .legacy_motor_count = application_motor_count,
    .legacy_motor_limits_rpm = application_motor_limits,
};

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
        if (bus->type != ROBOT_BUS_UART_RS485 ||
            composition->bus_count >= ROBOT_PROFILE_MAX_BUSES) {
            composition->diagnostics.code =
                ROBOT_COMPOSITION_DIAGNOSTIC_BUS_INCOMPATIBLE;
            composition->diagnostics.stage = ROBOT_COMPOSITION_STAGE_BUS_CONSTRUCT;
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
            composition->diagnostics.stage = ROBOT_COMPOSITION_STAGE_BUS_CONSTRUCT;
            composition->diagnostics.bus_id = bus->id;
            return error;
        }
        slot->used = true;
        slot->profile_bus_id = bus->id;
        composition->bus_count++;
    }
    return ESP_OK;
}

static bool polling_task_is_quiesced(const svd48_poll_task_t *task)
{
    return task && !task->service && !task->task && !task->running &&
           !task->stopped && !task->stop_requested;
}

static bool cleanup_runtime(robot_composition_t *composition)
{
    if (!composition) {
        return false;
    }
    if (composition->started || composition->polling_task.service ||
        composition->polling_task.task ||
        composition->polling_task.running || composition->polling_task.stopped ||
        composition->polling_task.stop_requested) {
        (void)robot_composition_stop(composition);
    }
    /* Preserve dependencies until a timed-out poll task is fully collected. */
    if (!polling_task_is_quiesced(&composition->polling_task)) {
        ESP_LOGE(TAG,
                 "Polling task did not stop; preserving runtime dependencies");
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
    if (composition->legacy_binding_count == 0U ||
        composition->legacy_binding_count != profile->endpoint_count) {
        composition->diagnostics.code = ROBOT_COMPOSITION_DIAGNOSTIC_ENDPOINT_FAILED;
        composition->diagnostics.stage = ROBOT_COMPOSITION_STAGE_ENDPOINT_CONSTRUCT;
        return fail_after_cleanup(composition, ESP_ERR_NOT_SUPPORTED);
    }
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
             (unsigned)composition->bus_count,
             (unsigned)composition->device_count,
             (unsigned)composition->adapter_count);
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
    if (!polling_task_is_quiesced(&composition->polling_task)) {
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
    esp_err_t error = svd48_poll_task_start(&composition->polling_task,
                                            &composition->polling_service);
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
    if (composition->polling_task.service || composition->polling_task.task ||
        composition->polling_task.running ||
        composition->polling_task.stopped ||
        composition->polling_task.stop_requested) {
        esp_err_t error = svd48_poll_task_stop(&composition->polling_task,
                                              POLLING_STOP_TIMEOUT_MS);
        if (error != ESP_OK) {
            first_error = error;
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

const robot_composition_diagnostics_t *robot_composition_get_diagnostics(
    const robot_composition_t *composition)
{
    return composition ? &composition->diagnostics : NULL;
}
