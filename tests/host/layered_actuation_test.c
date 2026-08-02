#include "host_test.h"

#include <pthread.h>
#include <string.h>
#include <threads.h>

#include "actuation_coordinator.h"
#include "robot_control_endpoint_adapter.h"
#include "robot_profile.h"

typedef struct {
    pthread_mutex_t lock;
    int event_count;
    int events[32];
    int fail_speed;
    int fail_stop;
    bool delay_stop;
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
    if (fake->fixture->delay_stop) thrd_sleep(&(struct timespec){.tv_nsec = 20000000}, NULL);
    return fake->fixture->fail_stop;
}

static bool lock_acquire(void *context)
{
    return pthread_mutex_lock(context) == 0;
}

static void lock_release(void *context)
{
    pthread_mutex_unlock(context);
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
    fixture_t fixture = {.lock = PTHREAD_MUTEX_INITIALIZER, .delay_stop = true};
    fake_context_t contexts[2] = {{.fixture = &fixture}, {.fixture = &fixture}};
    robot_control_endpoint_adapter_t adapters[2];
    robot_endpoint_registry_t registry;
    robot_endpoint_registry_init(&registry);
    HOST_TEST_CHECK(add_adapter(&registry, &adapters[0], &contexts[0], 1, 0));
    HOST_TEST_CHECK(add_adapter(&registry, &adapters[1], &contexts[1], 2, 1));
    actuation_lock_port_t lock = {lock_acquire, lock_release, &fixture.lock};
    actuation_coordinator_t coordinator;
    HOST_TEST_CHECK(actuation_coordinator_init(&coordinator, &registry, &lock));

    pthread_t stop_thread;
    pthread_t set_thread;
    thread_argument_t stop = {&coordinator, true};
    thread_argument_t set = {&coordinator, false};
    pthread_create(&stop_thread, NULL, run_operation, &stop);
    thrd_sleep(&(struct timespec){.tv_nsec = 5000000}, NULL);
    pthread_create(&set_thread, NULL, run_operation, &set);
    pthread_join(stop_thread, NULL);
    pthread_join(set_thread, NULL);
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
    return true;
}

static bool zero_stoppable_is_failure(void)
{
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    robot_endpoint_registry_t registry;
    robot_endpoint_registry_init(&registry);
    actuation_lock_port_t lock = {lock_acquire, lock_release, &mutex};
    actuation_coordinator_t coordinator;
    HOST_TEST_CHECK(actuation_coordinator_init(&coordinator, &registry, &lock));
    actuation_report_t report;
    HOST_TEST_CHECK(actuation_coordinator_stop_all(&coordinator, &report) ==
                    ACTUATION_RESULT_FAILURE);
    return true;
}

int main(void)
{
    const host_test_case_t tests[] = {
        HOST_TEST_CASE(capabilities_and_registry),
        HOST_TEST_CASE(profiles),
        HOST_TEST_CASE(coordinator_serialization),
        HOST_TEST_CASE(zero_stoppable_is_failure),
    };
    return host_test_exit_code(host_test_run_cases(tests, HOST_TEST_ARRAY_COUNT(tests), stderr));
}
