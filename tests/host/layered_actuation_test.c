#include "host_test.h"

#include <pthread.h>
#include <string.h>

#include "actuation_application_port.h"
#include "actuation_coordinator.h"
#include "robot_control_endpoint_adapter.h"
#include "robot_profile.h"

typedef struct {
    pthread_mutex_t lock;
    pthread_mutex_t gate_lock;
    pthread_cond_t gate_changed;
    int event_count;
    int events[32];
    int fail_speed;
    int fail_stop;
    bool delay_stop;
    bool stop_entered;
    bool release_stop;
    size_t lock_acquire_attempts;
} fixture_t;

typedef struct { fixture_t *fixture; uint8_t motor; } fake_context_t;

static int fake_speed(void *context, uint8_t motor, int16_t rpm)
{
    (void)rpm;
    fake_context_t *fake = context;
    fake->fixture->events[fake->fixture->event_count++] = 100 + motor;
    return fake->fixture->fail_speed;
}

static int fake_stop(void *context, uint8_t motor)
{
    fake_context_t *fake = context;
    fake->fixture->events[fake->fixture->event_count++] = 200 + motor;
    if (fake->fixture->delay_stop) {
        pthread_mutex_lock(&fake->fixture->gate_lock);
        fake->fixture->stop_entered = true;
        pthread_cond_broadcast(&fake->fixture->gate_changed);
        while (!fake->fixture->release_stop) {
            pthread_cond_wait(&fake->fixture->gate_changed,
                              &fake->fixture->gate_lock);
        }
        pthread_mutex_unlock(&fake->fixture->gate_lock);
    }
    return fake->fixture->fail_stop;
}

static bool lock_acquire(void *context)
{
    fixture_t *fixture = context;
    if (!fixture) {
        return false;
    }
    pthread_mutex_lock(&fixture->gate_lock);
    fixture->lock_acquire_attempts++;
    pthread_cond_broadcast(&fixture->gate_changed);
    pthread_mutex_unlock(&fixture->gate_lock);
    return pthread_mutex_lock(&fixture->lock) == 0;
}

static void lock_release(void *context)
{
    fixture_t *fixture = context;
    if (fixture) {
        pthread_mutex_unlock(&fixture->lock);
    }
}

static void wait_for_stop_and_lock_attempts(fixture_t *fixture,
                                            size_t lock_attempts)
{
    pthread_mutex_lock(&fixture->gate_lock);
    while (!fixture->stop_entered ||
           fixture->lock_acquire_attempts < lock_attempts) {
        pthread_cond_wait(&fixture->gate_changed, &fixture->gate_lock);
    }
    pthread_mutex_unlock(&fixture->gate_lock);
}

static void release_delayed_stop(fixture_t *fixture)
{
    pthread_mutex_lock(&fixture->gate_lock);
    fixture->release_stop = true;
    pthread_cond_broadcast(&fixture->gate_changed);
    pthread_mutex_unlock(&fixture->gate_lock);
}

static bool add_adapter(robot_endpoint_registry_t *registry,
                        robot_control_endpoint_adapter_t *adapter,
                        fake_context_t *context,
                        robot_endpoint_id_t id,
                        uint8_t motor)
{
    context->motor = motor;
    HOST_TEST_CHECK(robot_control_endpoint_adapter_init(
        adapter, context, motor, id, "traction", ROBOT_ENDPOINT_REQUIRED,
        -15, 15, fake_speed, fake_stop));
    HOST_TEST_CHECK(robot_endpoint_registry_add(registry, &adapter->endpoint) == ROBOT_REGISTRY_OK);
    return true;
}

typedef struct {
    size_t calls;
} fake_stoppable_t;

static robot_capability_error_t fake_stoppable_stop(
    robot_stoppable_port_t *port)
{
    fake_stoppable_t *fake = port ? port->context : NULL;
    if (!fake) {
        return ROBOT_CAP_INVALID_ARGUMENT;
    }
    ++fake->calls;
    return ROBOT_CAP_OK;
}

static bool unavailable_endpoint_still_attempts_stop(void)
{
    static const robot_stoppable_ops_t stop_ops = {
        .stop = fake_stoppable_stop,
    };
    fake_stoppable_t fake = {0};
    robot_stoppable_port_t stop_port = {
        .ops = &stop_ops,
        .context = &fake,
    };
    robot_endpoint_t endpoint = {
        .id = 1U,
        .name = "unavailable_stoppable",
        .available = false,
        .stoppable = &stop_port,
    };

    HOST_TEST_CHECK(robot_endpoint_stop(&endpoint) == ROBOT_CAP_OK);
    HOST_TEST_CHECK(fake.calls == 1U);
    return true;
}

static bool capabilities_and_registry(void)
{
    fixture_t fixture = {.lock = PTHREAD_MUTEX_INITIALIZER};
    fake_context_t context = {.fixture = &fixture};
    robot_control_endpoint_adapter_t adapter;
    robot_endpoint_registry_t registry;
    robot_endpoint_registry_init(&registry);
    HOST_TEST_CHECK(add_adapter(&registry, &adapter, &context, 1, 3));
    HOST_TEST_CHECK(robot_endpoint_capabilities(&adapter.endpoint) ==
                    (ROBOT_CAPABILITY_VELOCITY_RPM | ROBOT_CAPABILITY_STOPPABLE));
    HOST_TEST_CHECK(robot_velocity_set_rpm(&adapter.endpoint, 12) == ROBOT_CAP_OK);
    HOST_TEST_CHECK(robot_velocity_set_rpm(&adapter.endpoint, 16) == ROBOT_CAP_OUT_OF_RANGE);
    adapter.endpoint.available = false;
    HOST_TEST_CHECK(robot_velocity_set_rpm(&adapter.endpoint, 1) == ROBOT_CAP_UNAVAILABLE);
    adapter.endpoint.available = true;
    adapter.endpoint.velocity_rpm = NULL;
    HOST_TEST_CHECK(robot_velocity_set_rpm(&adapter.endpoint, 1) == ROBOT_CAP_UNSUPPORTED);
    HOST_TEST_CHECK(robot_endpoint_registry_find(&registry, 1) == &adapter.endpoint);
    HOST_TEST_CHECK(robot_endpoint_registry_find(&registry, 99) == NULL);
    HOST_TEST_CHECK(robot_endpoint_registry_add(&registry, &adapter.endpoint) ==
                    ROBOT_REGISTRY_DUPLICATE_ID);
    return true;
}

typedef struct {
    actuation_application_endpoint_info_t endpoints[2];
    robot_endpoint_id_t last_endpoint_id;
    int16_t last_rpm;
    bool stopped;
} fake_application_t;

static size_t fake_application_endpoint_count(
    const actuation_application_port_t *port)
{
    return port && port->context ? 2U : 0U;
}

static bool fake_application_endpoint_at(
    const actuation_application_port_t *port,
    size_t index,
    actuation_application_endpoint_info_t *endpoint)
{
    const fake_application_t *application = port ? port->context : NULL;
    if (!application || !endpoint || index >= 2U) {
        return false;
    }
    *endpoint = application->endpoints[index];
    return true;
}

static actuation_application_result_t fake_application_set_speed(
    actuation_application_port_t *port,
    robot_endpoint_id_t endpoint_id,
    int16_t rpm)
{
    fake_application_t *application = port ? port->context : NULL;
    if (!application || endpoint_id == 0U) {
        return ACTUATION_APPLICATION_INVALID_ARGUMENT;
    }
    application->last_endpoint_id = endpoint_id;
    application->last_rpm = rpm;
    return ACTUATION_APPLICATION_OK;
}

static actuation_application_result_t fake_application_stop(
    actuation_application_port_t *port,
    robot_endpoint_id_t endpoint_id)
{
    fake_application_t *application = port ? port->context : NULL;
    if (!application || endpoint_id == 0U) {
        return ACTUATION_APPLICATION_INVALID_ARGUMENT;
    }
    application->last_endpoint_id = endpoint_id;
    application->stopped = true;
    return ACTUATION_APPLICATION_OK;
}

static bool fake_application_observation(
    actuation_application_port_t *port,
    robot_endpoint_id_t endpoint_id,
    robot_velocity_observation_t *observation)
{
    fake_application_t *application = port ? port->context : NULL;
    if (!application || endpoint_id == 0U || !observation) {
        return false;
    }
    application->last_endpoint_id = endpoint_id;
    *observation = (robot_velocity_observation_t){
        .valid = true,
        .rpm = -7,
        .timestamp_ms = 123U,
        .source = ROBOT_VELOCITY_OBSERVATION_SOURCE_DEVICE_FEEDBACK,
        .online = true,
        .stale = false,
        .health = ROBOT_ENDPOINT_HEALTH_HEALTHY,
    };
    return true;
}

static bool application_endpoint_boundary(void)
{
    fake_application_t application = {
        .endpoints = {
            {
                .id = 11U,
                .name = "left",
                .capabilities = ROBOT_CAPABILITY_VELOCITY_RPM |
                                ROBOT_CAPABILITY_STOPPABLE,
                .criticality = ROBOT_ENDPOINT_REQUIRED,
                .available = true,
                .min_rpm = -15,
                .max_rpm = 15,
            },
            {
                .id = 12U,
                .name = "right",
                .capabilities = ROBOT_CAPABILITY_STOPPABLE,
                .criticality = ROBOT_ENDPOINT_DEVELOPMENT,
                .available = false,
            },
        },
    };
    static const actuation_application_ops_t ops = {
        .endpoint_count = fake_application_endpoint_count,
        .endpoint_at = fake_application_endpoint_at,
        .set_endpoint_speed_rpm = fake_application_set_speed,
        .stop_endpoint = fake_application_stop,
        .get_endpoint_velocity_observation = fake_application_observation,
    };
    actuation_application_port_t port = {
        .ops = &ops,
        .context = &application,
    };

    HOST_TEST_CHECK(actuation_application_endpoint_count(&port) == 2U);
    actuation_application_endpoint_info_t endpoint;
    HOST_TEST_CHECK(actuation_application_endpoint_at(&port, 0U, &endpoint));
    HOST_TEST_CHECK(endpoint.id == 11U);
    HOST_TEST_CHECK(endpoint.criticality == ROBOT_ENDPOINT_REQUIRED);
    HOST_TEST_CHECK((endpoint.capabilities & ROBOT_CAPABILITY_VELOCITY_RPM) !=
                    0U);
    HOST_TEST_CHECK(actuation_application_find_endpoint(&port, 12U, &endpoint));
    HOST_TEST_CHECK(strcmp(endpoint.name, "right") == 0);
    HOST_TEST_CHECK(endpoint.criticality == ROBOT_ENDPOINT_DEVELOPMENT);
    HOST_TEST_CHECK(!endpoint.available);
    HOST_TEST_CHECK(!actuation_application_find_endpoint(&port, 99U, &endpoint));

    HOST_TEST_CHECK(actuation_application_set_endpoint_speed_rpm(
                        &port, 11U, -7) == ACTUATION_APPLICATION_OK);
    HOST_TEST_CHECK(application.last_endpoint_id == 11U);
    HOST_TEST_CHECK(application.last_rpm == -7);
    /* Availability describes observed readiness; it must not suppress a
     * fail-safe stop attempt for an existing stoppable endpoint. */
    HOST_TEST_CHECK(!application.stopped);
    HOST_TEST_CHECK(actuation_application_stop_endpoint(
                        &port, 12U) == ACTUATION_APPLICATION_OK);
    HOST_TEST_CHECK(application.last_endpoint_id == 12U);
    HOST_TEST_CHECK(application.stopped);

    robot_velocity_observation_t observation;
    HOST_TEST_CHECK(actuation_application_get_endpoint_velocity_observation(
        &port, 11U, &observation));
    HOST_TEST_CHECK(observation.valid);
    HOST_TEST_CHECK(observation.rpm == -7);
    HOST_TEST_CHECK(observation.timestamp_ms == 123U);
    HOST_TEST_CHECK(observation.source ==
                    ROBOT_VELOCITY_OBSERVATION_SOURCE_DEVICE_FEEDBACK);
    HOST_TEST_CHECK(observation.online);
    HOST_TEST_CHECK(!observation.stale);
    HOST_TEST_CHECK(observation.health == ROBOT_ENDPOINT_HEALTH_HEALTHY);

    actuation_application_port_t unavailable = {0};
    HOST_TEST_CHECK(actuation_application_endpoint_count(&unavailable) == 0U);
    HOST_TEST_CHECK(actuation_application_set_endpoint_speed_rpm(
                        &unavailable, 11U, 0) ==
                    ACTUATION_APPLICATION_INVALID_ARGUMENT);
    HOST_TEST_CHECK(!actuation_application_get_endpoint_velocity_observation(
        &unavailable, 11U, &observation));
    return true;
}

static robot_profile_t position_profile(void)
{
    robot_profile_t profile = {
        .schema_version = ROBOT_PROFILE_SCHEMA_VERSION,
        .name = "single_servo",
        .board = robot_board_esp32s3_current(),
        .bus_count = 1,
        .buses = {{1, ROBOT_BUS_PWM, 0, {4, -1}, 50}},
        .device_count = 1,
        .devices = {{1, ROBOT_DRIVER_PWM_SERVO, 1, 0, 1, ROBOT_ENDPOINT_DEVELOPMENT}},
        .endpoint_count = 1,
        .endpoints = {{1, "servo", 1, 0, ROBOT_CAPABILITY_POSITION,
                       ROBOT_ENDPOINT_DEVELOPMENT, 0, 0}},
        .application = {ROBOT_PROFILE_NO_GEOMETRY, 0, 0, 0},
    };
    return profile;
}

static bool profiles(void)
{
    HOST_TEST_CHECK(robot_profile_validate(robot_profile_selected()) == ROBOT_PROFILE_VALID);
    robot_profile_t profile = position_profile();
    HOST_TEST_CHECK(robot_profile_validate(&profile) == ROBOT_PROFILE_VALID);
    profile.endpoints[0].channel = 1;
    HOST_TEST_CHECK(robot_profile_validate(&profile) == ROBOT_PROFILE_BAD_CHANNEL);
    profile = position_profile();
    profile.buses[0].pins[0] = 19;
    HOST_TEST_CHECK(robot_profile_validate(&profile) == ROBOT_PROFILE_RESERVED_PIN);
    profile = position_profile();
    profile.devices[0].bus_id = 99;
    HOST_TEST_CHECK(robot_profile_validate(&profile) == ROBOT_PROFILE_BAD_REFERENCE);
    profile = position_profile();
    profile.endpoints[0].capabilities = ROBOT_CAPABILITY_VELOCITY_RPM;
    HOST_TEST_CHECK(robot_profile_validate(&profile) == ROBOT_PROFILE_BAD_CAPABILITY);
    profile = position_profile();
    profile.buses[0].id = 0;
    HOST_TEST_CHECK(robot_profile_validate(&profile) == ROBOT_PROFILE_BAD_REFERENCE);
    profile = position_profile();
    profile.devices[0].id = 0;
    HOST_TEST_CHECK(robot_profile_validate(&profile) == ROBOT_PROFILE_BAD_REFERENCE);
    profile = position_profile();
    profile.endpoints[0].id = 0;
    HOST_TEST_CHECK(robot_profile_validate(&profile) == ROBOT_PROFILE_BAD_REFERENCE);

    const robot_driver_descriptor_t can_driver = {
        ROBOT_DRIVER_TEST_CAN, ROBOT_BUS_CAN_TWAI, ROBOT_CAPABILITY_VELOCITY_RPM, 2};
    const robot_driver_registry_t registry = {&can_driver, 1};
    profile = position_profile();
    profile.buses[0] = (robot_bus_profile_t){.id=1, .type=ROBOT_BUS_CAN_TWAI, .peripheral=0, .pins={4,5}, .rate=500000};
    profile.devices[0] = (robot_device_profile_t){1, ROBOT_DRIVER_TEST_CAN, 1, 7, 2,
                                                  ROBOT_ENDPOINT_DEVELOPMENT};
    profile.endpoints[0].capabilities = ROBOT_CAPABILITY_VELOCITY_RPM;
    profile.endpoints[0].min_rpm = -10;
    profile.endpoints[0].max_rpm = 10;
    HOST_TEST_CHECK(robot_profile_validate_with_registry(&profile, &registry) == ROBOT_PROFILE_VALID);
    return true;
}

typedef struct { actuation_coordinator_t *coordinator; bool stop; } thread_argument_t;

static void *run_operation(void *argument)
{
    thread_argument_t *thread = argument;
    actuation_report_t report;
    if (thread->stop) actuation_coordinator_stop_all(thread->coordinator, &report);
    else actuation_coordinator_set_velocity_rpm(thread->coordinator, 1, 5, &report);
    return NULL;
}

static bool coordinator_serialization(void)
{
    fixture_t fixture = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .gate_lock = PTHREAD_MUTEX_INITIALIZER,
        .gate_changed = PTHREAD_COND_INITIALIZER,
        .delay_stop = true,
    };
    fake_context_t contexts[2] = {{.fixture = &fixture}, {.fixture = &fixture}};
    robot_control_endpoint_adapter_t adapters[2];
    robot_endpoint_registry_t registry;
    robot_endpoint_registry_init(&registry);
    HOST_TEST_CHECK(add_adapter(&registry, &adapters[0], &contexts[0], 1, 0));
    HOST_TEST_CHECK(add_adapter(&registry, &adapters[1], &contexts[1], 2, 1));
    actuation_lock_port_t lock = {lock_acquire, lock_release, &fixture};
    actuation_coordinator_t coordinator;
    HOST_TEST_CHECK(actuation_coordinator_init(&coordinator, &registry, &lock));

    pthread_t stop_thread;
    pthread_t set_thread;
    thread_argument_t stop = {&coordinator, true};
    thread_argument_t set = {&coordinator, false};
    HOST_TEST_CHECK(pthread_create(&stop_thread, NULL, run_operation, &stop) == 0);
    wait_for_stop_and_lock_attempts(&fixture, 1U);
    HOST_TEST_CHECK(pthread_create(&set_thread, NULL, run_operation, &set) == 0);
    wait_for_stop_and_lock_attempts(&fixture, 2U);
    release_delayed_stop(&fixture);
    HOST_TEST_CHECK(pthread_join(stop_thread, NULL) == 0);
    HOST_TEST_CHECK(pthread_join(set_thread, NULL) == 0);
    HOST_TEST_CHECK(fixture.event_count == 3);
    HOST_TEST_CHECK(fixture.events[0] == 200);
    HOST_TEST_CHECK(fixture.events[1] == 201);
    HOST_TEST_CHECK(fixture.events[2] == 100);

    fixture.fail_speed = 1;
    actuation_report_t report;
    HOST_TEST_CHECK(actuation_coordinator_set_velocity_rpm(&coordinator, 1, 5, &report) ==
                    ACTUATION_RESULT_FAILURE);
    fixture.fail_speed = 0;
    HOST_TEST_CHECK(actuation_coordinator_set_velocity_rpm(&coordinator, 1, 5, &report) ==
                    ACTUATION_RESULT_SUCCESS);
    pthread_cond_destroy(&fixture.gate_changed);
    pthread_mutex_destroy(&fixture.gate_lock);
    pthread_mutex_destroy(&fixture.lock);
    return true;
}

static bool zero_stoppable_is_failure(void)
{
    fixture_t fixture = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .gate_lock = PTHREAD_MUTEX_INITIALIZER,
        .gate_changed = PTHREAD_COND_INITIALIZER,
    };
    robot_endpoint_registry_t registry;
    robot_endpoint_registry_init(&registry);
    actuation_lock_port_t lock = {lock_acquire, lock_release, &fixture};
    actuation_coordinator_t coordinator;
    HOST_TEST_CHECK(actuation_coordinator_init(&coordinator, &registry, &lock));
    actuation_report_t report;
    HOST_TEST_CHECK(actuation_coordinator_stop_all(&coordinator, &report) ==
                    ACTUATION_RESULT_FAILURE);
    pthread_cond_destroy(&fixture.gate_changed);
    pthread_mutex_destroy(&fixture.gate_lock);
    pthread_mutex_destroy(&fixture.lock);
    return true;
}

int main(void)
{
    const host_test_case_t tests[] = {
        HOST_TEST_CASE(unavailable_endpoint_still_attempts_stop),
        HOST_TEST_CASE(capabilities_and_registry),
        HOST_TEST_CASE(application_endpoint_boundary),
        HOST_TEST_CASE(profiles),
        HOST_TEST_CASE(coordinator_serialization),
        HOST_TEST_CASE(zero_stoppable_is_failure),
    };
    return host_test_exit_code(host_test_run_cases(tests, HOST_TEST_ARRAY_COUNT(tests), stderr));
}
