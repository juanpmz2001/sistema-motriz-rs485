#include "host_test.h"

#include <stdint.h>
#include <string.h>

#include "robot_driver_factory.h"

static const char *const ENDPOINT_NAMES[ROBOT_PROFILE_MAX_ENDPOINTS] = {
    "endpoint_1",
    "endpoint_2",
    "endpoint_3",
    "endpoint_4",
    "endpoint_5",
    "endpoint_6",
    "endpoint_7",
    "endpoint_8",
};

static robot_factory_result_t configured_validation_result;
static size_t configured_storage[ROBOT_PROFILE_MAX_DEVICES + 1U];
static size_t validate_calls;
static size_t storage_calls;
static size_t runtime_calls;

static void reset_factory_behavior(void)
{
    configured_validation_result = ROBOT_FACTORY_OK;
    validate_calls = 0U;
    storage_calls = 0U;
    runtime_calls = 0U;
    for (size_t index = 0U; index < HOST_TEST_ARRAY_COUNT(configured_storage);
         index++) {
        configured_storage[index] = 16U;
    }
}

static robot_factory_result_t fake_validate(
    const robot_driver_factory_t *factory,
    const robot_profile_t *profile,
    const robot_bus_profile_t *bus,
    const robot_device_profile_t *device)
{
    validate_calls++;
    if (!factory || !profile || !bus || !device) {
        return ROBOT_FACTORY_INVALID_CONFIGURATION;
    }
    return configured_validation_result;
}

static size_t fake_storage_required(const robot_driver_factory_t *factory,
                                    const robot_device_profile_t *device)
{
    storage_calls++;
    if (!factory || !device ||
        device->id >= HOST_TEST_ARRAY_COUNT(configured_storage)) {
        return 0U;
    }
    return configured_storage[device->id];
}

static robot_factory_result_t fake_construct(
    const robot_driver_factory_t *factory,
    void *runtime_context,
    const robot_profile_t *profile,
    const robot_bus_profile_t *bus,
    const robot_device_profile_t *device)
{
    (void)factory;
    (void)runtime_context;
    (void)profile;
    (void)bus;
    (void)device;
    runtime_calls++;
    return ROBOT_FACTORY_OK;
}

static robot_factory_result_t fake_create_endpoint(
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
    (void)endpoint;
    runtime_calls++;
    return ROBOT_FACTORY_OK;
}

static robot_factory_result_t fake_start(
    const robot_driver_factory_t *factory,
    void *runtime_context,
    const robot_device_profile_t *device)
{
    (void)factory;
    (void)runtime_context;
    (void)device;
    runtime_calls++;
    return ROBOT_FACTORY_OK;
}

static robot_factory_result_t fake_stop(
    const robot_driver_factory_t *factory,
    void *runtime_context,
    const robot_device_profile_t *device)
{
    (void)factory;
    (void)runtime_context;
    (void)device;
    runtime_calls++;
    return ROBOT_FACTORY_OK;
}

static void fake_destroy(const robot_driver_factory_t *factory,
                         void *runtime_context,
                         const robot_device_profile_t *device)
{
    (void)factory;
    (void)runtime_context;
    (void)device;
    runtime_calls++;
}

static const robot_driver_factory_ops_t COMPLETE_OPS = {
    .validate = fake_validate,
    .storage_required = fake_storage_required,
    .construct = fake_construct,
    .create_endpoint = fake_create_endpoint,
    .start = fake_start,
    .stop = fake_stop,
    .destroy = fake_destroy,
};

static robot_driver_factory_t make_factory(void)
{
    return (robot_driver_factory_t) {
        .driver_id = ROBOT_DRIVER_SVD48,
        .bus_type = ROBOT_BUS_UART_RS485,
        .capabilities = ROBOT_CAPABILITY_VELOCITY_RPM |
                        ROBOT_CAPABILITY_STOPPABLE,
        .max_channels = 2U,
        .ops = &COMPLETE_OPS,
    };
}

static robot_executable_factory_registry_t make_registry(
    const robot_driver_factory_t *factory)
{
    return (robot_executable_factory_registry_t) {
        .items = factory,
        .count = factory ? 1U : 0U,
        .runtime_storage_capacity = 1024U,
        .endpoint_capacity = ROBOT_PROFILE_MAX_ENDPOINTS,
        .legacy_binding_capacity = ROBOT_PROFILE_MAX_ENDPOINTS,
    };
}

static robot_profile_t make_profile(size_t device_count,
                                    size_t endpoint_count)
{
    robot_profile_t profile = {
        .schema_version = ROBOT_PROFILE_SCHEMA_VERSION,
        .name = "factory_test_profile",
        .board = robot_board_esp32s3_current(),
        .bus_count = 1U,
        .buses = {{
            .id = 1U,
            .type = ROBOT_BUS_UART_RS485,
            .peripheral = 2U,
            .pins = {17, 16},
            .rate = 115200U,
            .response_timeout_ms = 100U,
            .telemetry_period_ms = 30U,
            .stale_timeout_ms = 1000U,
            .retries = 2U,
        }},
        .device_count = device_count,
        .endpoint_count = endpoint_count,
        .application = {
            .kind = ROBOT_PROFILE_NO_GEOMETRY,
        },
    };

    for (size_t index = 0U; index < device_count; index++) {
        profile.devices[index] = (robot_device_profile_t) {
            .id = (uint16_t)(index + 1U),
            .driver_id = ROBOT_DRIVER_SVD48,
            .bus_id = 1U,
            .address = (uint8_t)(index + 1U),
            .channel_count = 2U,
            .criticality = ROBOT_ENDPOINT_REQUIRED,
        };
    }
    for (size_t index = 0U; index < endpoint_count; index++) {
        profile.endpoints[index] = (robot_endpoint_profile_t) {
            .id = (robot_endpoint_id_t)(index + 1U),
            .name = ENDPOINT_NAMES[index],
            .device_id = (uint16_t)(index / 2U + 1U),
            .channel = (uint8_t)(index % 2U),
            .capabilities = ROBOT_CAPABILITY_VELOCITY_RPM |
                            ROBOT_CAPABILITY_STOPPABLE,
            .criticality = ROBOT_ENDPOINT_REQUIRED,
            .min_rpm = -15,
            .max_rpm = 15,
        };
    }
    return profile;
}

static bool expect_schema_rejection(const robot_profile_t *profile,
                                    robot_profile_error_t expected_error)
{
    reset_factory_behavior();
    robot_driver_factory_t factory = make_factory();
    robot_executable_factory_registry_t registry = make_registry(&factory);
    robot_composition_diagnostics_t diagnostics;

    HOST_TEST_CHECK(!robot_composition_preflight(profile,
                                                 &registry,
                                                 &diagnostics));
    HOST_TEST_CHECK(!diagnostics.schema_valid);
    HOST_TEST_CHECK(!diagnostics.composition_supported);
    HOST_TEST_CHECK(diagnostics.schema_error == expected_error);
    HOST_TEST_CHECK(diagnostics.code ==
                    ROBOT_COMPOSITION_DIAGNOSTIC_SCHEMA_INVALID);
    HOST_TEST_CHECK(diagnostics.stage == ROBOT_COMPOSITION_STAGE_SCHEMA);
    HOST_TEST_CHECK(validate_calls == 0U);
    HOST_TEST_CHECK(storage_calls == 0U);
    HOST_TEST_CHECK(runtime_calls == 0U);
    return true;
}

static bool duplicate_and_missing_references_fail_before_factory_calls(void)
{
    robot_profile_t profile = make_profile(1U, 2U);
    profile.endpoints[1].id = profile.endpoints[0].id;
    HOST_TEST_CHECK(expect_schema_rejection(&profile,
                                            ROBOT_PROFILE_DUPLICATE_ID));

    profile = make_profile(1U, 2U);
    profile.endpoints[1].name = profile.endpoints[0].name;
    HOST_TEST_CHECK(expect_schema_rejection(&profile,
                                            ROBOT_PROFILE_DUPLICATE_NAME));

    profile = make_profile(1U, 2U);
    profile.endpoints[1].channel = profile.endpoints[0].channel;
    HOST_TEST_CHECK(expect_schema_rejection(&profile,
                                            ROBOT_PROFILE_DUPLICATE_CHANNEL));

    profile = make_profile(2U, 2U);
    profile.devices[1].id = profile.devices[0].id;
    HOST_TEST_CHECK(expect_schema_rejection(&profile,
                                            ROBOT_PROFILE_DUPLICATE_ID));

    profile = make_profile(2U, 2U);
    profile.devices[1].address = profile.devices[0].address;
    HOST_TEST_CHECK(expect_schema_rejection(&profile,
                                            ROBOT_PROFILE_DUPLICATE_ADDRESS));

    profile = make_profile(1U, 1U);
    profile.endpoints[0].device_id = 99U;
    HOST_TEST_CHECK(expect_schema_rejection(&profile,
                                            ROBOT_PROFILE_BAD_REFERENCE));
    return true;
}

static bool successful_preflight_is_pure(void)
{
    reset_factory_behavior();
    configured_storage[1] = 64U;
    robot_profile_t profile = make_profile(1U, 1U);
    robot_driver_factory_t factory = make_factory();
    robot_executable_factory_registry_t registry = make_registry(&factory);
    registry.runtime_storage_capacity = 64U;
    registry.endpoint_capacity = 1U;
    registry.legacy_binding_capacity = 1U;

    robot_composition_diagnostics_t diagnostics;
    HOST_TEST_CHECK(robot_composition_preflight(&profile,
                                                &registry,
                                                &diagnostics));
    HOST_TEST_CHECK(diagnostics.schema_valid);
    HOST_TEST_CHECK(diagnostics.composition_supported);
    HOST_TEST_CHECK(!diagnostics.runtime_ready);
    HOST_TEST_CHECK(diagnostics.schema_error == ROBOT_PROFILE_VALID);
    HOST_TEST_CHECK(diagnostics.code == ROBOT_COMPOSITION_DIAGNOSTIC_OK);
    HOST_TEST_CHECK(diagnostics.stage == ROBOT_COMPOSITION_STAGE_NONE);
    HOST_TEST_CHECK(diagnostics.driver_id == 0);
    HOST_TEST_CHECK(diagnostics.bus_id == 0U);
    HOST_TEST_CHECK(diagnostics.device_id == 0U);
    HOST_TEST_CHECK(diagnostics.required_storage == 64U);
    HOST_TEST_CHECK(diagnostics.available_storage == 64U);
    HOST_TEST_CHECK(validate_calls == 1U);
    HOST_TEST_CHECK(storage_calls == 1U);
    HOST_TEST_CHECK(runtime_calls == 0U);
    HOST_TEST_CHECK(robot_executable_factory_find(&registry,
                                                  ROBOT_DRIVER_SVD48) ==
                    &factory);
    HOST_TEST_CHECK(robot_executable_factory_find(&registry,
                                                  ROBOT_DRIVER_PWM_SERVO) ==
                    NULL);
    HOST_TEST_CHECK(robot_executable_factory_find(NULL,
                                                  ROBOT_DRIVER_SVD48) == NULL);
    return true;
}

static bool schema_and_output_arguments_are_rejected(void)
{
    reset_factory_behavior();
    robot_profile_t profile = make_profile(1U, 1U);
    robot_driver_factory_t factory = make_factory();
    robot_executable_factory_registry_t registry = make_registry(&factory);
    robot_composition_diagnostics_t diagnostics;

    HOST_TEST_CHECK(!robot_composition_preflight(&profile, &registry, NULL));
    HOST_TEST_CHECK(validate_calls == 0U);
    profile.schema_version++;
    HOST_TEST_CHECK(!robot_composition_preflight(&profile,
                                                 &registry,
                                                 &diagnostics));
    HOST_TEST_CHECK(!diagnostics.schema_valid);
    HOST_TEST_CHECK(!diagnostics.composition_supported);
    HOST_TEST_CHECK(diagnostics.schema_error == ROBOT_PROFILE_BAD_SCHEMA);
    HOST_TEST_CHECK(diagnostics.code ==
                    ROBOT_COMPOSITION_DIAGNOSTIC_SCHEMA_INVALID);
    HOST_TEST_CHECK(diagnostics.stage == ROBOT_COMPOSITION_STAGE_SCHEMA);
    HOST_TEST_CHECK(validate_calls == 0U);
    HOST_TEST_CHECK(storage_calls == 0U);
    HOST_TEST_CHECK(runtime_calls == 0U);
    return true;
}

static bool missing_factory_has_identity_diagnostics(void)
{
    reset_factory_behavior();
    robot_profile_t profile = make_profile(1U, 1U);
    robot_executable_factory_registry_t registry = make_registry(NULL);
    robot_composition_diagnostics_t diagnostics;

    HOST_TEST_CHECK(!robot_composition_preflight(&profile,
                                                 &registry,
                                                 &diagnostics));
    HOST_TEST_CHECK(diagnostics.schema_valid);
    HOST_TEST_CHECK(!diagnostics.composition_supported);
    HOST_TEST_CHECK(diagnostics.code ==
                    ROBOT_COMPOSITION_DIAGNOSTIC_FACTORY_MISSING);
    HOST_TEST_CHECK(diagnostics.stage ==
                    ROBOT_COMPOSITION_STAGE_FACTORY_LOOKUP);
    HOST_TEST_CHECK(diagnostics.driver_id == ROBOT_DRIVER_SVD48);
    HOST_TEST_CHECK(diagnostics.bus_id == 1U);
    HOST_TEST_CHECK(diagnostics.device_id == 1U);
    HOST_TEST_CHECK(validate_calls == 0U);
    HOST_TEST_CHECK(storage_calls == 0U);
    HOST_TEST_CHECK(runtime_calls == 0U);
    return true;
}

static bool incompatible_bus_is_rejected_before_factory_validate(void)
{
    reset_factory_behavior();
    robot_profile_t profile = make_profile(1U, 1U);
    robot_driver_factory_t factory = make_factory();
    factory.bus_type = ROBOT_BUS_CAN_TWAI;
    robot_executable_factory_registry_t registry = make_registry(&factory);
    robot_composition_diagnostics_t diagnostics;

    HOST_TEST_CHECK(!robot_composition_preflight(&profile,
                                                 &registry,
                                                 &diagnostics));
    HOST_TEST_CHECK(diagnostics.code ==
                    ROBOT_COMPOSITION_DIAGNOSTIC_BUS_INCOMPATIBLE);
    HOST_TEST_CHECK(diagnostics.stage ==
                    ROBOT_COMPOSITION_STAGE_FACTORY_LOOKUP);
    HOST_TEST_CHECK(diagnostics.driver_id == ROBOT_DRIVER_SVD48);
    HOST_TEST_CHECK(diagnostics.bus_id == 1U);
    HOST_TEST_CHECK(diagnostics.device_id == 1U);
    HOST_TEST_CHECK(validate_calls == 0U);
    HOST_TEST_CHECK(storage_calls == 0U);
    HOST_TEST_CHECK(runtime_calls == 0U);
    return true;
}

static bool invalid_device_and_factory_contract_are_rejected(void)
{
    reset_factory_behavior();
    robot_profile_t profile = make_profile(1U, 1U);
    robot_driver_factory_t factory = make_factory();
    robot_executable_factory_registry_t registry = make_registry(&factory);
    robot_composition_diagnostics_t diagnostics;

    configured_validation_result = ROBOT_FACTORY_INVALID_CONFIGURATION;
    HOST_TEST_CHECK(!robot_composition_preflight(&profile,
                                                 &registry,
                                                 &diagnostics));
    HOST_TEST_CHECK(diagnostics.code ==
                    ROBOT_COMPOSITION_DIAGNOSTIC_DEVICE_INVALID);
    HOST_TEST_CHECK(diagnostics.stage ==
                    ROBOT_COMPOSITION_STAGE_FACTORY_VALIDATE);
    HOST_TEST_CHECK(diagnostics.factory_result ==
                    ROBOT_FACTORY_INVALID_CONFIGURATION);
    HOST_TEST_CHECK(validate_calls == 1U);
    HOST_TEST_CHECK(storage_calls == 0U);

    reset_factory_behavior();
    robot_driver_factory_ops_t incomplete_ops = COMPLETE_OPS;
    incomplete_ops.storage_required = NULL;
    factory.ops = &incomplete_ops;
    HOST_TEST_CHECK(!robot_composition_preflight(&profile,
                                                 &registry,
                                                 &diagnostics));
    HOST_TEST_CHECK(diagnostics.code ==
                    ROBOT_COMPOSITION_DIAGNOSTIC_DEVICE_INVALID);
    HOST_TEST_CHECK(diagnostics.stage ==
                    ROBOT_COMPOSITION_STAGE_FACTORY_VALIDATE);
    HOST_TEST_CHECK(validate_calls == 0U);
    HOST_TEST_CHECK(storage_calls == 0U);

    reset_factory_behavior();
    factory = make_factory();
    factory.max_channels = 1U;
    registry = make_registry(&factory);
    HOST_TEST_CHECK(!robot_composition_preflight(&profile,
                                                 &registry,
                                                 &diagnostics));
    HOST_TEST_CHECK(diagnostics.code ==
                    ROBOT_COMPOSITION_DIAGNOSTIC_DEVICE_INVALID);
    HOST_TEST_CHECK(diagnostics.device_id == 1U);
    HOST_TEST_CHECK(validate_calls == 0U);
    HOST_TEST_CHECK(storage_calls == 0U);
    HOST_TEST_CHECK(runtime_calls == 0U);
    return true;
}

static bool unschedulable_poll_period_is_rejected_in_preflight(void)
{
    reset_factory_behavior();
    robot_profile_t profile = make_profile(1U, 1U);
    profile.buses[0].telemetry_period_ms = (uint32_t)INT32_MAX;
    robot_driver_factory_t factory = make_factory();
    robot_executable_factory_registry_t registry = make_registry(&factory);
    robot_composition_diagnostics_t diagnostics;

    HOST_TEST_CHECK(robot_profile_validate(&profile) == ROBOT_PROFILE_VALID);
    HOST_TEST_CHECK(!robot_composition_preflight(&profile,
                                                 &registry,
                                                 &diagnostics));
    HOST_TEST_CHECK(diagnostics.schema_valid);
    HOST_TEST_CHECK(!diagnostics.composition_supported);
    HOST_TEST_CHECK(diagnostics.code ==
                    ROBOT_COMPOSITION_DIAGNOSTIC_DEVICE_INVALID);
    HOST_TEST_CHECK(diagnostics.stage ==
                    ROBOT_COMPOSITION_STAGE_FACTORY_VALIDATE);
    HOST_TEST_CHECK(diagnostics.driver_id == ROBOT_DRIVER_SVD48);
    HOST_TEST_CHECK(diagnostics.bus_id == 1U);
    HOST_TEST_CHECK(diagnostics.device_id == 1U);
    HOST_TEST_CHECK(diagnostics.factory_result ==
                    ROBOT_FACTORY_INVALID_CONFIGURATION);
    HOST_TEST_CHECK(validate_calls == 0U);
    HOST_TEST_CHECK(storage_calls == 0U);
    HOST_TEST_CHECK(runtime_calls == 0U);
    return true;
}

static bool unsupported_endpoint_capability_is_rejected(void)
{
    reset_factory_behavior();
    robot_profile_t profile = make_profile(1U, 1U);
    robot_driver_factory_t factory = make_factory();
    factory.capabilities = ROBOT_CAPABILITY_VELOCITY_RPM;
    robot_executable_factory_registry_t registry = make_registry(&factory);
    robot_composition_diagnostics_t diagnostics;

    HOST_TEST_CHECK(robot_profile_validate(&profile) == ROBOT_PROFILE_VALID);
    HOST_TEST_CHECK(!robot_composition_preflight(&profile,
                                                 &registry,
                                                 &diagnostics));
    HOST_TEST_CHECK(diagnostics.code ==
                    ROBOT_COMPOSITION_DIAGNOSTIC_ENDPOINT_FAILED);
    HOST_TEST_CHECK(diagnostics.stage ==
                    ROBOT_COMPOSITION_STAGE_ENDPOINT_CONSTRUCT);
    HOST_TEST_CHECK(diagnostics.endpoint_id == 1);
    HOST_TEST_CHECK(diagnostics.device_id == 1U);
    HOST_TEST_CHECK(diagnostics.factory_result ==
                    ROBOT_FACTORY_ENDPOINT_FAILED);
    HOST_TEST_CHECK(validate_calls == 1U);
    HOST_TEST_CHECK(storage_calls == 1U);
    HOST_TEST_CHECK(runtime_calls == 0U);
    return true;
}

static bool endpoints_must_be_constructable_before_runtime_setup(void)
{
    reset_factory_behavior();
    robot_profile_t profile = make_profile(1U, 0U);
    robot_driver_factory_t factory = make_factory();
    robot_executable_factory_registry_t registry = make_registry(&factory);
    robot_composition_diagnostics_t diagnostics;

    HOST_TEST_CHECK(robot_profile_validate(&profile) == ROBOT_PROFILE_VALID);
    HOST_TEST_CHECK(!robot_composition_preflight(&profile,
                                                 &registry,
                                                 &diagnostics));
    HOST_TEST_CHECK(diagnostics.code ==
                    ROBOT_COMPOSITION_DIAGNOSTIC_ENDPOINT_FAILED);
    HOST_TEST_CHECK(diagnostics.stage ==
                    ROBOT_COMPOSITION_STAGE_ENDPOINT_CONSTRUCT);
    HOST_TEST_CHECK(diagnostics.factory_result ==
                    ROBOT_FACTORY_ENDPOINT_FAILED);
    HOST_TEST_CHECK(validate_calls == 0U);
    HOST_TEST_CHECK(storage_calls == 0U);
    HOST_TEST_CHECK(runtime_calls == 0U);

    reset_factory_behavior();
    profile = make_profile(1U, 1U);
    profile.endpoints[0].capabilities = 0U;
    HOST_TEST_CHECK(robot_profile_validate(&profile) == ROBOT_PROFILE_VALID);
    HOST_TEST_CHECK(!robot_composition_preflight(&profile,
                                                 &registry,
                                                 &diagnostics));
    HOST_TEST_CHECK(diagnostics.code ==
                    ROBOT_COMPOSITION_DIAGNOSTIC_ENDPOINT_FAILED);
    HOST_TEST_CHECK(diagnostics.endpoint_id == 1);
    HOST_TEST_CHECK(validate_calls == 1U);
    HOST_TEST_CHECK(storage_calls == 1U);
    HOST_TEST_CHECK(runtime_calls == 0U);

    reset_factory_behavior();
    profile = make_profile(1U, 1U);
    profile.endpoints[0].capabilities = ROBOT_CAPABILITY_VELOCITY_RPM;
    HOST_TEST_CHECK(robot_profile_validate(&profile) == ROBOT_PROFILE_VALID);
    HOST_TEST_CHECK(!robot_composition_preflight(&profile,
                                                 &registry,
                                                 &diagnostics));
    HOST_TEST_CHECK(diagnostics.code ==
                    ROBOT_COMPOSITION_DIAGNOSTIC_ENDPOINT_FAILED);
    HOST_TEST_CHECK(diagnostics.endpoint_id == 1);
    HOST_TEST_CHECK(validate_calls == 1U);
    HOST_TEST_CHECK(storage_calls == 1U);
    HOST_TEST_CHECK(runtime_calls == 0U);

    /* The factory gate applies to every physical-motion capability, not only
     * legacy velocity.  Use the schema-valid PWM-servo descriptor so this
     * reaches composition preflight rather than failing earlier as a profile
     * capability mismatch. */
    reset_factory_behavior();
    profile = make_profile(1U, 1U);
    profile.buses[0].type = ROBOT_BUS_PWM;
    profile.buses[0].rate = 50U;
    profile.devices[0].driver_id = ROBOT_DRIVER_PWM_SERVO;
    profile.devices[0].channel_count = 1U;
    profile.endpoints[0].capabilities = ROBOT_CAPABILITY_POSITION;
    profile.endpoints[0].min_position_degrees = -90.0f;
    profile.endpoints[0].max_position_degrees = 90.0f;
    factory = make_factory();
    factory.driver_id = ROBOT_DRIVER_PWM_SERVO;
    factory.bus_type = ROBOT_BUS_PWM;
    factory.capabilities = ROBOT_CAPABILITY_POSITION;
    factory.max_channels = 1U;
    registry = make_registry(&factory);
    HOST_TEST_CHECK(robot_profile_validate(&profile) == ROBOT_PROFILE_VALID);
    HOST_TEST_CHECK(!robot_composition_preflight(&profile,
                                                 &registry,
                                                 &diagnostics));
    HOST_TEST_CHECK(diagnostics.code ==
                    ROBOT_COMPOSITION_DIAGNOSTIC_ENDPOINT_FAILED);
    HOST_TEST_CHECK(diagnostics.stage ==
                    ROBOT_COMPOSITION_STAGE_ENDPOINT_CONSTRUCT);
    HOST_TEST_CHECK(diagnostics.endpoint_id == 1U);
    HOST_TEST_CHECK(validate_calls == 1U);
    HOST_TEST_CHECK(storage_calls == 1U);
    HOST_TEST_CHECK(runtime_calls == 0U);

    reset_factory_behavior();
    profile = make_profile(1U, 1U);
    factory = make_factory();
    registry = make_registry(&factory);
    profile.endpoints[0].capabilities = ROBOT_CAPABILITY_STOPPABLE;
    profile.endpoints[0].min_rpm = 1;
    profile.endpoints[0].max_rpm = -1;
    HOST_TEST_CHECK(robot_profile_validate(&profile) == ROBOT_PROFILE_VALID);
    HOST_TEST_CHECK(!robot_composition_preflight(&profile,
                                                 &registry,
                                                 &diagnostics));
    HOST_TEST_CHECK(diagnostics.code ==
                    ROBOT_COMPOSITION_DIAGNOSTIC_ENDPOINT_FAILED);
    HOST_TEST_CHECK(diagnostics.endpoint_id == 1);
    HOST_TEST_CHECK(validate_calls == 1U);
    HOST_TEST_CHECK(storage_calls == 1U);
    HOST_TEST_CHECK(runtime_calls == 0U);
    return true;
}

static bool endpoint_failure_reports_the_resolved_device_and_bus(void)
{
    reset_factory_behavior();
    robot_profile_t profile = make_profile(2U, 2U);
    profile.bus_count = 2U;
    profile.buses[1] = profile.buses[0];
    profile.buses[1].id = 2U;
    profile.buses[1].peripheral = 1U;
    profile.buses[1].pins[0] = 18;
    profile.buses[1].pins[1] = 21;
    profile.devices[1].bus_id = 2U;
    profile.endpoints[0].capabilities = 0U;
    robot_driver_factory_t factory = make_factory();
    robot_executable_factory_registry_t registry = make_registry(&factory);
    robot_composition_diagnostics_t diagnostics;

    HOST_TEST_CHECK(robot_profile_validate(&profile) == ROBOT_PROFILE_VALID);
    HOST_TEST_CHECK(!robot_composition_preflight(&profile,
                                                 &registry,
                                                 &diagnostics));
    HOST_TEST_CHECK(diagnostics.code ==
                    ROBOT_COMPOSITION_DIAGNOSTIC_ENDPOINT_FAILED);
    HOST_TEST_CHECK(diagnostics.stage ==
                    ROBOT_COMPOSITION_STAGE_ENDPOINT_CONSTRUCT);
    HOST_TEST_CHECK(diagnostics.driver_id == ROBOT_DRIVER_SVD48);
    HOST_TEST_CHECK(diagnostics.bus_id == 1U);
    HOST_TEST_CHECK(diagnostics.device_id == 1U);
    HOST_TEST_CHECK(diagnostics.endpoint_id == 1);
    HOST_TEST_CHECK(validate_calls == 2U);
    HOST_TEST_CHECK(storage_calls == 2U);
    HOST_TEST_CHECK(runtime_calls == 0U);
    return true;
}

static bool endpoint_capacity_is_enforced_before_factory_calls(void)
{
    reset_factory_behavior();
    robot_profile_t profile = make_profile(1U, 1U);
    robot_driver_factory_t factory = make_factory();
    robot_executable_factory_registry_t registry = make_registry(&factory);
    registry.endpoint_capacity = 0U;
    robot_composition_diagnostics_t diagnostics;

    HOST_TEST_CHECK(!robot_composition_preflight(&profile,
                                                 &registry,
                                                 &diagnostics));
    HOST_TEST_CHECK(diagnostics.code ==
                    ROBOT_COMPOSITION_DIAGNOSTIC_STATIC_CAPACITY_EXCEEDED);
    HOST_TEST_CHECK(diagnostics.stage == ROBOT_COMPOSITION_STAGE_SCHEMA);
    HOST_TEST_CHECK(diagnostics.factory_result == ROBOT_FACTORY_NO_STORAGE);
    HOST_TEST_CHECK(diagnostics.required_storage == 1U);
    HOST_TEST_CHECK(diagnostics.available_storage == 0U);
    HOST_TEST_CHECK(validate_calls == 0U);
    HOST_TEST_CHECK(storage_calls == 0U);
    HOST_TEST_CHECK(runtime_calls == 0U);
    return true;
}

static bool runtime_storage_capacity_and_overflow_are_enforced(void)
{
    reset_factory_behavior();
    robot_profile_t profile = make_profile(1U, 1U);
    robot_driver_factory_t factory = make_factory();
    robot_executable_factory_registry_t registry = make_registry(&factory);
    robot_composition_diagnostics_t diagnostics;

    configured_storage[1] = 65U;
    registry.runtime_storage_capacity = 64U;
    HOST_TEST_CHECK(!robot_composition_preflight(&profile,
                                                 &registry,
                                                 &diagnostics));
    HOST_TEST_CHECK(diagnostics.code ==
                    ROBOT_COMPOSITION_DIAGNOSTIC_STATIC_CAPACITY_EXCEEDED);
    HOST_TEST_CHECK(diagnostics.stage ==
                    ROBOT_COMPOSITION_STAGE_FACTORY_VALIDATE);
    HOST_TEST_CHECK(diagnostics.factory_result == ROBOT_FACTORY_NO_STORAGE);
    HOST_TEST_CHECK(diagnostics.required_storage == 65U);
    HOST_TEST_CHECK(diagnostics.available_storage == 64U);
    HOST_TEST_CHECK(validate_calls == 1U);
    HOST_TEST_CHECK(storage_calls == 1U);
    HOST_TEST_CHECK(runtime_calls == 0U);

    reset_factory_behavior();
    profile = make_profile(2U, 2U);
    configured_storage[1] = SIZE_MAX - 4U;
    configured_storage[2] = 8U;
    registry.runtime_storage_capacity = SIZE_MAX;
    HOST_TEST_CHECK(!robot_composition_preflight(&profile,
                                                 &registry,
                                                 &diagnostics));
    HOST_TEST_CHECK(diagnostics.code ==
                    ROBOT_COMPOSITION_DIAGNOSTIC_STATIC_CAPACITY_EXCEEDED);
    HOST_TEST_CHECK(diagnostics.stage ==
                    ROBOT_COMPOSITION_STAGE_FACTORY_VALIDATE);
    HOST_TEST_CHECK(diagnostics.factory_result == ROBOT_FACTORY_NO_STORAGE);
    HOST_TEST_CHECK(diagnostics.required_storage == SIZE_MAX);
    HOST_TEST_CHECK(diagnostics.available_storage == SIZE_MAX);
    HOST_TEST_CHECK(validate_calls == 2U);
    HOST_TEST_CHECK(storage_calls == 2U);
    HOST_TEST_CHECK(runtime_calls == 0U);
    return true;
}

static bool zero_runtime_storage_requirement_is_rejected(void)
{
    reset_factory_behavior();
    configured_storage[1] = 0U;
    robot_profile_t profile = make_profile(1U, 1U);
    robot_driver_factory_t factory = make_factory();
    robot_executable_factory_registry_t registry = make_registry(&factory);
    robot_composition_diagnostics_t diagnostics;

    HOST_TEST_CHECK(!robot_composition_preflight(&profile,
                                                 &registry,
                                                 &diagnostics));
    HOST_TEST_CHECK(diagnostics.code ==
                    ROBOT_COMPOSITION_DIAGNOSTIC_STATIC_CAPACITY_EXCEEDED);
    HOST_TEST_CHECK(diagnostics.factory_result == ROBOT_FACTORY_NO_STORAGE);
    HOST_TEST_CHECK(diagnostics.required_storage == 0U);
    HOST_TEST_CHECK(diagnostics.available_storage ==
                    registry.runtime_storage_capacity);
    HOST_TEST_CHECK(validate_calls == 1U);
    HOST_TEST_CHECK(storage_calls == 1U);
    HOST_TEST_CHECK(runtime_calls == 0U);
    return true;
}

static bool legacy_binding_limit_has_a_specific_diagnostic(void)
{
    reset_factory_behavior();
    robot_profile_t profile = make_profile(3U, 5U);
    robot_driver_factory_t factory = make_factory();
    robot_executable_factory_registry_t registry = make_registry(&factory);
    registry.legacy_binding_capacity = 4U;
    robot_composition_diagnostics_t diagnostics;

    HOST_TEST_CHECK(robot_profile_validate(&profile) == ROBOT_PROFILE_VALID);
    HOST_TEST_CHECK(!robot_composition_preflight(&profile,
                                                 &registry,
                                                 &diagnostics));
    HOST_TEST_CHECK(diagnostics.schema_valid);
    HOST_TEST_CHECK(!diagnostics.composition_supported);
    HOST_TEST_CHECK(diagnostics.code ==
                    ROBOT_COMPOSITION_DIAGNOSTIC_LEGACY_BINDING_LIMIT);
    HOST_TEST_CHECK(diagnostics.stage ==
                    ROBOT_COMPOSITION_STAGE_ENDPOINT_CONSTRUCT);
    HOST_TEST_CHECK(diagnostics.factory_result ==
                    ROBOT_FACTORY_ENDPOINT_FAILED);
    HOST_TEST_CHECK(diagnostics.required_storage == 5U);
    HOST_TEST_CHECK(diagnostics.available_storage == 4U);
    HOST_TEST_CHECK(validate_calls == 0U);
    HOST_TEST_CHECK(storage_calls == 0U);
    HOST_TEST_CHECK(runtime_calls == 0U);
    return true;
}

static bool diagnostic_names_cover_public_values(void)
{
    static const struct {
        robot_composition_diagnostic_code_t value;
        const char *name;
    } codes[] = {
        {ROBOT_COMPOSITION_DIAGNOSTIC_OK, "OK"},
        {ROBOT_COMPOSITION_DIAGNOSTIC_SCHEMA_INVALID, "SCHEMA_INVALID"},
        {ROBOT_COMPOSITION_DIAGNOSTIC_FACTORY_MISSING, "FACTORY_MISSING"},
        {ROBOT_COMPOSITION_DIAGNOSTIC_BUS_INCOMPATIBLE, "BUS_INCOMPATIBLE"},
        {ROBOT_COMPOSITION_DIAGNOSTIC_DEVICE_INVALID, "DEVICE_INVALID"},
        {ROBOT_COMPOSITION_DIAGNOSTIC_CONSTRUCTION_FAILED,
         "CONSTRUCTION_FAILED"},
        {ROBOT_COMPOSITION_DIAGNOSTIC_ENDPOINT_FAILED, "ENDPOINT_FAILED"},
        {ROBOT_COMPOSITION_DIAGNOSTIC_START_FAILED, "START_FAILED"},
        {ROBOT_COMPOSITION_DIAGNOSTIC_STATIC_CAPACITY_EXCEEDED,
         "STATIC_CAPACITY_EXCEEDED"},
        {ROBOT_COMPOSITION_DIAGNOSTIC_LEGACY_BINDING_LIMIT,
         "LEGACY_BINDING_LIMIT"},
    };
    static const struct {
        robot_composition_stage_t value;
        const char *name;
    } stages[] = {
        {ROBOT_COMPOSITION_STAGE_NONE, "NONE"},
        {ROBOT_COMPOSITION_STAGE_SCHEMA, "SCHEMA"},
        {ROBOT_COMPOSITION_STAGE_FACTORY_LOOKUP, "FACTORY_LOOKUP"},
        {ROBOT_COMPOSITION_STAGE_FACTORY_VALIDATE, "FACTORY_VALIDATE"},
        {ROBOT_COMPOSITION_STAGE_BUS_CONSTRUCT, "BUS_CONSTRUCT"},
        {ROBOT_COMPOSITION_STAGE_DEVICE_CONSTRUCT, "DEVICE_CONSTRUCT"},
        {ROBOT_COMPOSITION_STAGE_ENDPOINT_CONSTRUCT, "ENDPOINT_CONSTRUCT"},
        {ROBOT_COMPOSITION_STAGE_SERVICE_START, "SERVICE_START"},
    };

    for (size_t index = 0U; index < HOST_TEST_ARRAY_COUNT(codes); index++) {
        HOST_TEST_CHECK(strcmp(robot_composition_diagnostic_code_name(
                                   codes[index].value),
                               codes[index].name) == 0);
    }
    HOST_TEST_CHECK(strcmp(robot_composition_diagnostic_code_name(
                               (robot_composition_diagnostic_code_t)999),
                           "UNKNOWN") == 0);

    for (size_t index = 0U; index < HOST_TEST_ARRAY_COUNT(stages); index++) {
        HOST_TEST_CHECK(strcmp(robot_composition_stage_name(stages[index].value),
                               stages[index].name) == 0);
    }
    HOST_TEST_CHECK(strcmp(robot_composition_stage_name(
                               (robot_composition_stage_t)999),
                           "UNKNOWN") == 0);
    return true;
}

#ifdef BOTFARMS_VERIFY_RAFA_COMPOSITION
static bool selected_rafa_profile_passes_executable_preflight(void)
{
    reset_factory_behavior();
    const robot_profile_t *profile = robot_profile_selected();
    HOST_TEST_CHECK(profile != NULL);
    HOST_TEST_CHECK(strcmp(profile->name, "rafa") == 0);
    const robot_driver_factory_t factory = make_factory();
    const robot_executable_factory_registry_t registry = make_registry(&factory);
    robot_composition_diagnostics_t diagnostics;
    HOST_TEST_CHECK(robot_composition_preflight(profile, &registry, &diagnostics));
    HOST_TEST_CHECK(diagnostics.schema_valid);
    HOST_TEST_CHECK(diagnostics.composition_supported);
    HOST_TEST_CHECK(diagnostics.code == ROBOT_COMPOSITION_DIAGNOSTIC_OK);
    HOST_TEST_CHECK(diagnostics.stage == ROBOT_COMPOSITION_STAGE_NONE);
    HOST_TEST_CHECK(diagnostics.required_storage == 16U);
    HOST_TEST_CHECK(validate_calls == 1U);
    HOST_TEST_CHECK(storage_calls == 1U);
    HOST_TEST_CHECK(runtime_calls == 0U);
    return true;
}
#endif

int main(void)
{
    const host_test_case_t tests[] = {
        HOST_TEST_CASE(duplicate_and_missing_references_fail_before_factory_calls),
        HOST_TEST_CASE(successful_preflight_is_pure),
        HOST_TEST_CASE(schema_and_output_arguments_are_rejected),
        HOST_TEST_CASE(missing_factory_has_identity_diagnostics),
        HOST_TEST_CASE(incompatible_bus_is_rejected_before_factory_validate),
        HOST_TEST_CASE(invalid_device_and_factory_contract_are_rejected),
        HOST_TEST_CASE(unschedulable_poll_period_is_rejected_in_preflight),
        HOST_TEST_CASE(unsupported_endpoint_capability_is_rejected),
        HOST_TEST_CASE(endpoints_must_be_constructable_before_runtime_setup),
        HOST_TEST_CASE(endpoint_failure_reports_the_resolved_device_and_bus),
        HOST_TEST_CASE(endpoint_capacity_is_enforced_before_factory_calls),
        HOST_TEST_CASE(runtime_storage_capacity_and_overflow_are_enforced),
        HOST_TEST_CASE(zero_runtime_storage_requirement_is_rejected),
        HOST_TEST_CASE(legacy_binding_limit_has_a_specific_diagnostic),
        HOST_TEST_CASE(diagnostic_names_cover_public_values),
#ifdef BOTFARMS_VERIFY_RAFA_COMPOSITION
        HOST_TEST_CASE(selected_rafa_profile_passes_executable_preflight),
#endif
    };
    return host_test_exit_code(host_test_run_cases(tests,
                                                   HOST_TEST_ARRAY_COUNT(tests),
                                                   stderr));
}
