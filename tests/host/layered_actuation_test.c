#include "host_test.h"

#include <math.h>
#include "host_threads.h"
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

static bool fixture_init(fixture_t *fixture)
{
    if (!fixture) {
        return false;
    }
    memset(fixture, 0, sizeof(*fixture));
    if (pthread_mutex_init(&fixture->lock, NULL) != 0) {
        return false;
    }
    if (pthread_mutex_init(&fixture->gate_lock, NULL) != 0) {
        pthread_mutex_destroy(&fixture->lock);
        return false;
    }
    if (pthread_cond_init(&fixture->gate_changed, NULL) != 0) {
        pthread_mutex_destroy(&fixture->gate_lock);
        pthread_mutex_destroy(&fixture->lock);
        return false;
    }
    return true;
}

static void fixture_deinit(fixture_t *fixture)
{
    if (!fixture) {
        return;
    }
    pthread_cond_destroy(&fixture->gate_changed);
    pthread_mutex_destroy(&fixture->gate_lock);
    pthread_mutex_destroy(&fixture->lock);
}

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
    robot_capability_error_t result;
} fake_stoppable_t;

static robot_capability_error_t fake_stoppable_stop(
    robot_stoppable_port_t *port)
{
    fake_stoppable_t *fake = port ? port->context : NULL;
    if (!fake) {
        return ROBOT_CAP_INVALID_ARGUMENT;
    }
    ++fake->calls;
    return fake->result;
}

typedef struct {
    size_t set_calls;
    float last_degrees;
    robot_capability_error_t result;
} fake_position_t;

static robot_capability_error_t fake_position_set(
    robot_position_port_t *port,
    float degrees)
{
    fake_position_t *fake = port ? port->context : NULL;
    if (!fake) {
        return ROBOT_CAP_INVALID_ARGUMENT;
    }
    ++fake->set_calls;
    fake->last_degrees = degrees;
    return fake->result;
}

typedef struct {
    size_t set_calls;
    float last_degrees;
    robot_capability_error_t result;
} fake_position_reference_t;

static robot_capability_error_t fake_position_reference_set(
    robot_position_reference_port_t *port,
    float degrees)
{
    fake_position_reference_t *fake = port ? port->context : NULL;
    if (!fake) {
        return ROBOT_CAP_INVALID_ARGUMENT;
    }
    ++fake->set_calls;
    fake->last_degrees = degrees;
    return fake->result;
}

typedef struct {
    size_t read_calls;
    robot_capability_error_t result;
    robot_position_observation_t observation;
} fake_position_observation_t;

static robot_capability_error_t fake_position_observation_read(
    robot_position_observation_port_t *port,
    robot_position_observation_t *observation)
{
    fake_position_observation_t *fake = port ? port->context : NULL;
    if (!fake || !observation) {
        return ROBOT_CAP_INVALID_ARGUMENT;
    }
    ++fake->read_calls;
    *observation = fake->observation;
    return fake->result;
}

typedef struct {
    float degrees;
} fake_position_sensor_t;

static robot_capability_error_t fake_position_sensor_read(
    robot_position_sensor_port_t *port,
    float *degrees)
{
    fake_position_sensor_t *fake = port ? port->context : NULL;
    if (!fake || !degrees) {
        return ROBOT_CAP_INVALID_ARGUMENT;
    }
    *degrees = fake->degrees;
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

static bool position_capability_and_observation(void)
{
    static const robot_position_ops_t position_ops = {
        .set_position_degrees = fake_position_set,
    };
    static const robot_position_reference_ops_t position_reference_ops = {
        .set_reference_degrees = fake_position_reference_set,
    };
    static const robot_stoppable_ops_t stop_ops = {
        .stop = fake_stoppable_stop,
    };
    static const robot_position_sensor_ops_t sensor_ops = {
        .read_position_degrees = fake_position_sensor_read,
    };
    static const robot_position_observation_ops_t observation_ops = {
        .read = fake_position_observation_read,
    };
    fake_position_t position = {0};
    fake_position_reference_t position_reference = {0};
    fake_stoppable_t stoppable = {0};
    fake_position_sensor_t sensor = {.degrees = 12.0f};
    fake_position_observation_t observation = {
        .observation = {
            .valid = true,
            .calibrated = true,
            .referenced = true,
            .degrees = 12.5f,
            .timestamp_ms = 456U,
            .source = ROBOT_POSITION_OBSERVATION_SOURCE_INDEPENDENT_SENSOR,
            .online = true,
            .stale = false,
            .health = ROBOT_ENDPOINT_HEALTH_HEALTHY,
            .status = ROBOT_CAP_OK,
        },
    };
    robot_position_port_t position_port = {
        .ops = &position_ops,
        .context = &position,
        .min_degrees = -90.0f,
        .max_degrees = 90.0f,
    };
    robot_position_reference_port_t position_reference_port = {
        .ops = &position_reference_ops,
        .context = &position_reference,
        .min_degrees = -90.0f,
        .max_degrees = 90.0f,
    };
    robot_stoppable_port_t stop_port = {
        .ops = &stop_ops,
        .context = &stoppable,
    };
    robot_position_sensor_port_t sensor_port = {
        .ops = &sensor_ops,
        .context = &sensor,
    };
    robot_position_observation_port_t observation_port = {
        .ops = &observation_ops,
        .context = &observation,
    };
    robot_endpoint_t endpoint = {
        .id = 7U,
        .name = "steering",
        .criticality = ROBOT_ENDPOINT_REQUIRED,
        .available = true,
        .position = &position_port,
        .position_reference = &position_reference_port,
        .stoppable = &stop_port,
        .position_sensor = &sensor_port,
        .position_observation = &observation_port,
    };

    HOST_TEST_CHECK(robot_endpoint_capabilities(&endpoint) ==
                    (ROBOT_CAPABILITY_STOPPABLE | ROBOT_CAPABILITY_POSITION |
                     ROBOT_CAPABILITY_POSITION_REFERENCE |
                     ROBOT_CAPABILITY_POSITION_SENSOR |
                     ROBOT_CAPABILITY_POSITION_OBSERVATION));
    HOST_TEST_CHECK(robot_endpoint_has_capability(
        &endpoint, ROBOT_CAPABILITY_POSITION));
    HOST_TEST_CHECK(robot_endpoint_has_capability(
        &endpoint, ROBOT_CAPABILITY_POSITION_REFERENCE));
    HOST_TEST_CHECK(robot_endpoint_has_position_observation(&endpoint));
    HOST_TEST_CHECK(robot_position_set_degrees(&endpoint, -90.0f) == ROBOT_CAP_OK);
    HOST_TEST_CHECK(position.set_calls == 1U);
    HOST_TEST_CHECK(position.last_degrees == -90.0f);
    HOST_TEST_CHECK(robot_position_set_degrees(&endpoint, 90.1f) ==
                    ROBOT_CAP_OUT_OF_RANGE);
    HOST_TEST_CHECK(robot_position_set_degrees(&endpoint, NAN) ==
                    ROBOT_CAP_OUT_OF_RANGE);
    HOST_TEST_CHECK(position.set_calls == 1U);
    position_port.min_degrees = NAN;
    HOST_TEST_CHECK(robot_position_set_degrees(&endpoint, 0.0f) ==
                    ROBOT_CAP_INVALID_ARGUMENT);
    position_port.min_degrees = -90.0f;

    HOST_TEST_CHECK(robot_position_set_reference_degrees(
                        &endpoint, 0.0f) == ROBOT_CAP_OK);
    HOST_TEST_CHECK(position_reference.set_calls == 1U);
    HOST_TEST_CHECK(position_reference.last_degrees == 0.0f);
    HOST_TEST_CHECK(robot_position_set_reference_degrees(
                        &endpoint, 90.1f) == ROBOT_CAP_OUT_OF_RANGE);
    HOST_TEST_CHECK(position_reference.set_calls == 1U);

    robot_position_observation_t result;
    HOST_TEST_CHECK(robot_endpoint_read_position_observation(&endpoint, &result) ==
                    ROBOT_CAP_OK);
    HOST_TEST_CHECK(observation.read_calls == 1U);
    HOST_TEST_CHECK(result.valid);
    HOST_TEST_CHECK(result.calibrated);
    HOST_TEST_CHECK(result.referenced);
    HOST_TEST_CHECK(result.degrees == 12.5f);
    HOST_TEST_CHECK(result.timestamp_ms == 456U);
    HOST_TEST_CHECK(result.source_endpoint_id == endpoint.id);
    HOST_TEST_CHECK(result.source ==
                    ROBOT_POSITION_OBSERVATION_SOURCE_INDEPENDENT_SENSOR);
    HOST_TEST_CHECK(result.online);
    HOST_TEST_CHECK(!result.stale);
    HOST_TEST_CHECK(result.health == ROBOT_ENDPOINT_HEALTH_HEALTHY);
    HOST_TEST_CHECK(result.status == ROBOT_CAP_OK);

    observation.observation.valid = false;
    observation.observation.calibrated = false;
    observation.observation.referenced = false;
    observation.observation.health = ROBOT_ENDPOINT_HEALTH_DEGRADED;
    HOST_TEST_CHECK(robot_endpoint_read_position_observation(&endpoint, &result) ==
                    ROBOT_CAP_OK);
    HOST_TEST_CHECK(!result.valid);
    HOST_TEST_CHECK(!result.calibrated);
    HOST_TEST_CHECK(result.health == ROBOT_ENDPOINT_HEALTH_DEGRADED);

    endpoint.available = false;
    HOST_TEST_CHECK(robot_position_set_degrees(&endpoint, 0.0f) ==
                    ROBOT_CAP_UNAVAILABLE);
    HOST_TEST_CHECK(robot_endpoint_read_position_observation(&endpoint, &result) ==
                    ROBOT_CAP_UNAVAILABLE);
    endpoint.available = true;
    endpoint.position = NULL;
    HOST_TEST_CHECK(robot_position_set_degrees(&endpoint, 0.0f) ==
                    ROBOT_CAP_UNSUPPORTED);
    endpoint.position_reference = NULL;
    HOST_TEST_CHECK(robot_position_set_reference_degrees(&endpoint, 0.0f) ==
                    ROBOT_CAP_UNSUPPORTED);
    endpoint.position_observation = NULL;
    HOST_TEST_CHECK(!robot_endpoint_has_position_observation(&endpoint));
    HOST_TEST_CHECK(robot_endpoint_read_position_observation(&endpoint, &result) ==
                    ROBOT_CAP_UNSUPPORTED);
    return true;
}

static bool capabilities_and_registry(void)
{
    fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture));
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
    fixture_deinit(&fixture);
    return true;
}

typedef struct {
    actuation_application_endpoint_info_t endpoints[2];
    robot_endpoint_id_t last_endpoint_id;
    int16_t last_rpm;
    float last_degrees;
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

static actuation_application_result_t fake_application_set_position(
    actuation_application_port_t *port,
    robot_endpoint_id_t endpoint_id,
    float degrees)
{
    fake_application_t *application = port ? port->context : NULL;
    if (!application || endpoint_id == 0U) {
        return ACTUATION_APPLICATION_INVALID_ARGUMENT;
    }
    application->last_endpoint_id = endpoint_id;
    application->last_degrees = degrees;
    return ACTUATION_APPLICATION_OK;
}

static actuation_application_result_t fake_application_set_position_reference(
    actuation_application_port_t *port,
    robot_endpoint_id_t endpoint_id,
    float degrees)
{
    fake_application_t *application = port ? port->context : NULL;
    if (!application || endpoint_id == 0U) {
        return ACTUATION_APPLICATION_INVALID_ARGUMENT;
    }
    application->last_endpoint_id = endpoint_id;
    application->last_degrees = degrees;
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

static bool fake_application_position_observation(
    actuation_application_port_t *port,
    robot_endpoint_id_t endpoint_id,
    robot_position_observation_t *observation)
{
    fake_application_t *application = port ? port->context : NULL;
    if (!application || endpoint_id == 0U || !observation) {
        return false;
    }
    application->last_endpoint_id = endpoint_id;
    *observation = (robot_position_observation_t){
        .valid = true,
        .calibrated = true,
        .referenced = true,
        .degrees = 17.5f,
        .timestamp_ms = 456U,
        .source_endpoint_id = endpoint_id,
        .source = ROBOT_POSITION_OBSERVATION_SOURCE_INDEPENDENT_SENSOR,
        .online = true,
        .stale = false,
        .health = ROBOT_ENDPOINT_HEALTH_HEALTHY,
        .status = ROBOT_CAP_OK,
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
                                ROBOT_CAPABILITY_STOPPABLE |
                                ROBOT_CAPABILITY_POSITION |
                                ROBOT_CAPABILITY_POSITION_REFERENCE |
                                ROBOT_CAPABILITY_POSITION_OBSERVATION,
                .criticality = ROBOT_ENDPOINT_REQUIRED,
                .available = true,
                .min_rpm = -15,
                .max_rpm = 15,
                .position_observation_supported = true,
                .min_position_degrees = -90.0f,
                .max_position_degrees = 90.0f,
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
        .set_endpoint_position_degrees = fake_application_set_position,
        .set_endpoint_position_reference_degrees =
            fake_application_set_position_reference,
        .get_endpoint_position_observation = fake_application_position_observation,
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
    HOST_TEST_CHECK((endpoint.capabilities & ROBOT_CAPABILITY_POSITION) != 0U);
    HOST_TEST_CHECK((endpoint.capabilities &
                     ROBOT_CAPABILITY_POSITION_REFERENCE) != 0U);
    HOST_TEST_CHECK(endpoint.position_observation_supported);
    HOST_TEST_CHECK(endpoint.min_position_degrees == -90.0f);
    HOST_TEST_CHECK(endpoint.max_position_degrees == 90.0f);
    HOST_TEST_CHECK(actuation_application_find_endpoint(&port, 12U, &endpoint));
    HOST_TEST_CHECK(strcmp(endpoint.name, "right") == 0);
    HOST_TEST_CHECK(endpoint.criticality == ROBOT_ENDPOINT_DEVELOPMENT);
    HOST_TEST_CHECK(!endpoint.available);
    HOST_TEST_CHECK(!actuation_application_find_endpoint(&port, 99U, &endpoint));

    HOST_TEST_CHECK(actuation_application_set_endpoint_speed_rpm(
                        &port, 11U, -7) == ACTUATION_APPLICATION_OK);
    HOST_TEST_CHECK(application.last_endpoint_id == 11U);
    HOST_TEST_CHECK(application.last_rpm == -7);
    HOST_TEST_CHECK(actuation_application_set_endpoint_position_degrees(
                        &port, 11U, 17.5f) == ACTUATION_APPLICATION_OK);
    HOST_TEST_CHECK(application.last_endpoint_id == 11U);
    HOST_TEST_CHECK(application.last_degrees == 17.5f);
    HOST_TEST_CHECK(actuation_application_set_endpoint_position_reference_degrees(
                        &port, 11U, 0.0f) == ACTUATION_APPLICATION_OK);
    HOST_TEST_CHECK(application.last_endpoint_id == 11U);
    HOST_TEST_CHECK(application.last_degrees == 0.0f);
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

    robot_position_observation_t position_observation;
    HOST_TEST_CHECK(actuation_application_get_endpoint_position_observation(
        &port, 11U, &position_observation));
    HOST_TEST_CHECK(position_observation.valid);
    HOST_TEST_CHECK(position_observation.calibrated);
    HOST_TEST_CHECK(position_observation.referenced);
    HOST_TEST_CHECK(position_observation.degrees == 17.5f);
    HOST_TEST_CHECK(position_observation.timestamp_ms == 456U);
    HOST_TEST_CHECK(position_observation.source_endpoint_id == 11U);
    HOST_TEST_CHECK(position_observation.source ==
                    ROBOT_POSITION_OBSERVATION_SOURCE_INDEPENDENT_SENSOR);
    HOST_TEST_CHECK(position_observation.online);
    HOST_TEST_CHECK(!position_observation.stale);
    HOST_TEST_CHECK(position_observation.health == ROBOT_ENDPOINT_HEALTH_HEALTHY);
    HOST_TEST_CHECK(position_observation.status == ROBOT_CAP_OK);

    actuation_application_port_t unavailable = {0};
    HOST_TEST_CHECK(actuation_application_endpoint_count(&unavailable) == 0U);
    HOST_TEST_CHECK(actuation_application_set_endpoint_speed_rpm(
                        &unavailable, 11U, 0) ==
                    ACTUATION_APPLICATION_INVALID_ARGUMENT);
    HOST_TEST_CHECK(actuation_application_set_endpoint_position_degrees(
                        &unavailable, 11U, 0.0f) ==
                    ACTUATION_APPLICATION_INVALID_ARGUMENT);
    HOST_TEST_CHECK(actuation_application_set_endpoint_position_reference_degrees(
                        &unavailable, 11U, 0.0f) ==
                    ACTUATION_APPLICATION_INVALID_ARGUMENT);
    HOST_TEST_CHECK(!actuation_application_get_endpoint_velocity_observation(
        &unavailable, 11U, &observation));
    HOST_TEST_CHECK(!actuation_application_get_endpoint_position_observation(
        &unavailable, 11U, &position_observation));
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
                       ROBOT_ENDPOINT_DEVELOPMENT, 0, 0, -90.0f, 90.0f}},
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
    fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture));
    fixture.delay_stop = true;
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
    fixture_deinit(&fixture);
    return true;
}

static bool coordinator_position_requests_roll_back_on_required_failure(void)
{
    static const robot_position_ops_t position_ops = {
        .set_position_degrees = fake_position_set,
    };
    static const robot_stoppable_ops_t stop_ops = {
        .stop = fake_stoppable_stop,
    };
    fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture));
    fake_position_t first_position = {0};
    fake_position_t second_position = {.result = ROBOT_CAP_IO_ERROR};
    fake_stoppable_t first_stop = {0};
    fake_stoppable_t second_stop = {0};
    robot_position_port_t first_position_port = {
        .ops = &position_ops,
        .context = &first_position,
        .min_degrees = -90.0f,
        .max_degrees = 90.0f,
    };
    robot_position_port_t second_position_port = {
        .ops = &position_ops,
        .context = &second_position,
        .min_degrees = -90.0f,
        .max_degrees = 90.0f,
    };
    robot_stoppable_port_t first_stop_port = {
        .ops = &stop_ops,
        .context = &first_stop,
    };
    robot_stoppable_port_t second_stop_port = {
        .ops = &stop_ops,
        .context = &second_stop,
    };
    robot_endpoint_t endpoints[] = {
        {
            .id = 31U,
            .name = "front_steering",
            .criticality = ROBOT_ENDPOINT_REQUIRED,
            .available = true,
            .position = &first_position_port,
            .stoppable = &first_stop_port,
        },
        {
            .id = 32U,
            .name = "rear_steering",
            .criticality = ROBOT_ENDPOINT_REQUIRED,
            .available = true,
            .position = &second_position_port,
            .stoppable = &second_stop_port,
        },
    };
    robot_endpoint_registry_t registry;
    robot_endpoint_registry_init(&registry);
    HOST_TEST_CHECK(robot_endpoint_registry_add(&registry, &endpoints[0]) ==
                    ROBOT_REGISTRY_OK);
    HOST_TEST_CHECK(robot_endpoint_registry_add(&registry, &endpoints[1]) ==
                    ROBOT_REGISTRY_OK);
    actuation_lock_port_t lock = {lock_acquire, lock_release, &fixture};
    actuation_coordinator_t coordinator;
    HOST_TEST_CHECK(actuation_coordinator_init(&coordinator, &registry, &lock));

    actuation_report_t report;
    HOST_TEST_CHECK(actuation_coordinator_set_position_degrees(
                        &coordinator, 31U, 15.0f, &report) ==
                    ACTUATION_RESULT_SUCCESS);
    HOST_TEST_CHECK(report.requested == 1U);
    HOST_TEST_CHECK(report.applied == 1U);
    HOST_TEST_CHECK(report.endpoints[0].error == ROBOT_CAP_OK);
    HOST_TEST_CHECK(first_position.set_calls == 1U);
    HOST_TEST_CHECK(first_position.last_degrees == 15.0f);

    const actuation_position_request_t requests[] = {
        {.endpoint_id = 31U, .degrees = -10.0f},
        {.endpoint_id = 32U, .degrees = 10.0f},
    };
    HOST_TEST_CHECK(actuation_coordinator_apply_position_degrees(
                        &coordinator,
                        requests,
                        HOST_TEST_ARRAY_COUNT(requests),
                        &report) == ACTUATION_RESULT_PARTIAL);
    HOST_TEST_CHECK(report.requested == 2U);
    HOST_TEST_CHECK(report.applied == 1U);
    HOST_TEST_CHECK(report.endpoints[0].endpoint_id == 31U);
    HOST_TEST_CHECK(report.endpoints[0].applied);
    HOST_TEST_CHECK(report.endpoints[0].rollback_stop_attempted);
    HOST_TEST_CHECK(report.endpoints[0].rollback_stop_error == ROBOT_CAP_OK);
    HOST_TEST_CHECK(report.endpoints[1].endpoint_id == 32U);
    HOST_TEST_CHECK(report.endpoints[1].error == ROBOT_CAP_IO_ERROR);
    HOST_TEST_CHECK(first_position.set_calls == 2U);
    HOST_TEST_CHECK(second_position.set_calls == 1U);
    HOST_TEST_CHECK(first_stop.calls == 1U);
    HOST_TEST_CHECK(second_stop.calls == 0U);
    HOST_TEST_CHECK(fixture.lock_acquire_attempts == 2U);

    fixture_deinit(&fixture);
    return true;
}

static bool coordinator_reference_stops_before_mapping(void)
{
    static const robot_position_reference_ops_t reference_ops = {
        .set_reference_degrees = fake_position_reference_set,
    };
    static const robot_stoppable_ops_t stop_ops = {
        .stop = fake_stoppable_stop,
    };
    fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture));
    fake_position_reference_t reference = {0};
    fake_stoppable_t stop = {0};
    robot_position_reference_port_t reference_port = {
        .ops = &reference_ops,
        .context = &reference,
        .min_degrees = -90.0f,
        .max_degrees = 90.0f,
    };
    robot_stoppable_port_t stop_port = {
        .ops = &stop_ops,
        .context = &stop,
    };
    robot_endpoint_t endpoint = {
        .id = 44U,
        .name = "steering_reference",
        .criticality = ROBOT_ENDPOINT_DEVELOPMENT,
        .available = true,
        .position_reference = &reference_port,
        .stoppable = &stop_port,
    };
    robot_endpoint_registry_t registry;
    robot_endpoint_registry_init(&registry);
    HOST_TEST_CHECK(robot_endpoint_registry_add(&registry, &endpoint) ==
                    ROBOT_REGISTRY_OK);
    actuation_lock_port_t lock = {lock_acquire, lock_release, &fixture};
    actuation_coordinator_t coordinator;
    HOST_TEST_CHECK(actuation_coordinator_init(&coordinator, &registry, &lock));

    actuation_report_t report;
    HOST_TEST_CHECK(actuation_coordinator_set_position_reference_degrees(
                        &coordinator, endpoint.id, 0.0f, &report) ==
                    ACTUATION_RESULT_SUCCESS);
    HOST_TEST_CHECK(report.requested == 1U);
    HOST_TEST_CHECK(report.applied == 1U);
    HOST_TEST_CHECK(report.endpoints[0].rollback_stop_attempted);
    HOST_TEST_CHECK(report.endpoints[0].rollback_stop_error == ROBOT_CAP_OK);
    HOST_TEST_CHECK(stop.calls == 1U);
    HOST_TEST_CHECK(reference.set_calls == 1U);
    HOST_TEST_CHECK(reference.last_degrees == 0.0f);

    stop.result = ROBOT_CAP_IO_ERROR;
    HOST_TEST_CHECK(actuation_coordinator_set_position_reference_degrees(
                        &coordinator, endpoint.id, 10.0f, &report) ==
                    ACTUATION_RESULT_FAILURE);
    HOST_TEST_CHECK(stop.calls == 2U);
    HOST_TEST_CHECK(reference.set_calls == 1U);
    HOST_TEST_CHECK(report.endpoints[0].error == ROBOT_CAP_IO_ERROR);

    fixture_deinit(&fixture);
    return true;
}

static bool zero_stoppable_is_failure(void)
{
    fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture));
    robot_endpoint_registry_t registry;
    robot_endpoint_registry_init(&registry);
    actuation_lock_port_t lock = {lock_acquire, lock_release, &fixture};
    actuation_coordinator_t coordinator;
    HOST_TEST_CHECK(actuation_coordinator_init(&coordinator, &registry, &lock));
    actuation_report_t report;
    HOST_TEST_CHECK(actuation_coordinator_stop_all(&coordinator, &report) ==
                    ACTUATION_RESULT_FAILURE);
    fixture_deinit(&fixture);
    return true;
}

int main(void)
{
    const host_test_case_t tests[] = {
        HOST_TEST_CASE(unavailable_endpoint_still_attempts_stop),
        HOST_TEST_CASE(position_capability_and_observation),
        HOST_TEST_CASE(capabilities_and_registry),
        HOST_TEST_CASE(application_endpoint_boundary),
        HOST_TEST_CASE(profiles),
        HOST_TEST_CASE(coordinator_serialization),
        HOST_TEST_CASE(coordinator_position_requests_roll_back_on_required_failure),
        HOST_TEST_CASE(coordinator_reference_stops_before_mapping),
        HOST_TEST_CASE(zero_stoppable_is_failure),
    };
    return host_test_exit_code(host_test_run_cases(tests, HOST_TEST_ARRAY_COUNT(tests), stderr));
}
