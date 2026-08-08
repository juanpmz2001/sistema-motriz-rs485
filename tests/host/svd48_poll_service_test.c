#include "host_test.h"

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "fake_bus_transport.h"
#include "svd48_poll_service.h"

typedef struct {
    uint32_t now_ms;
} test_clock_t;

typedef struct {
    fake_bus_transport_t bus;
    svd48_device_t device;
    test_clock_t *clock;
    uint8_t address;
} poll_device_fixture_t;

typedef struct {
    svd48_poll_service_t *service;
    svd48_device_result_t result;
} poll_service_worker_t;

static void *poll_service_worker_run(void *context)
{
    poll_service_worker_t *worker = context;
    worker->result = svd48_poll_service_run_once(worker->service);
    return NULL;
}

static void append_crc(uint8_t *frame, size_t payload_length)
{
    uint16_t crc = svd48_crc16_uumotor(frame, payload_length);
    frame[payload_length] = (uint8_t)(crc >> 8);
    frame[payload_length + 1U] = (uint8_t)(crc & 0xFFU);
}

static uint32_t test_clock_ms(void *context)
{
    const test_clock_t *clock = context;
    return clock ? clock->now_ms : 0U;
}

static bool state_lock_acquire(void *context)
{
    return context != NULL;
}

static void state_lock_release(void *context)
{
    (void)context;
}

static bool poll_device_fixture_init(poll_device_fixture_t *fixture,
                                     test_clock_t *clock,
                                     uint16_t device_id,
                                     uint8_t address,
                                     uint32_t stale_timeout_ms)
{
    if (!fixture || !clock) {
        return false;
    }
    memset(fixture, 0, sizeof(*fixture));
    if (!fake_bus_transport_init(&fixture->bus, 10U)) {
        return false;
    }
    fixture->clock = clock;
    fixture->address = address;
    const svd48_device_config_t config = {
        .device_id = device_id,
        .address = address,
        .transport = fake_bus_transport_port(&fixture->bus),
        .response_timeout_ms = 10U,
        .retries = 0U,
        .stale_timeout_ms = stale_timeout_ms,
        .state_lock = {
            .acquire = state_lock_acquire,
            .release = state_lock_release,
            .context = fixture,
        },
        .clock_ms = test_clock_ms,
        .clock_context = clock,
    };
    return svd48_device_init(&fixture->device, &config);
}

static bool expect_read_result(poll_device_fixture_t *fixture,
                               uint16_t reg,
                               uint16_t quantity,
                               bus_transport_result_t result)
{
    uint8_t request[8];
    size_t request_length = svd48_build_read_request(fixture->address,
                                                     reg,
                                                     quantity,
                                                     request);
    return request_length == sizeof(request) &&
           fake_bus_transport_expect(&fixture->bus,
                                     request,
                                     request_length,
                                     result,
                                     NULL,
                                     0U);
}

static bool expect_read_response(poll_device_fixture_t *fixture,
                                 uint16_t reg,
                                 const uint16_t *values,
                                 uint16_t quantity)
{
    if (!fixture || !values || quantity == 0U || quantity > 16U) {
        return false;
    }
    uint8_t request[8];
    uint8_t response[64] = {0};
    size_t request_length = svd48_build_read_request(fixture->address,
                                                     reg,
                                                     quantity,
                                                     request);
    response[0] = fixture->address;
    response[1] = SVD48_FUNC_READ_HOLDING;
    response[2] = (uint8_t)(quantity * 2U);
    for (uint16_t index = 0; index < quantity; ++index) {
        response[3U + index * 2U] = (uint8_t)(values[index] >> 8);
        response[4U + index * 2U] = (uint8_t)(values[index] & 0xFFU);
    }
    size_t response_length = 5U + (size_t)quantity * 2U;
    append_crc(response, response_length - 2U);
    return request_length == sizeof(request) &&
           fake_bus_transport_expect(&fixture->bus,
                                     request,
                                     request_length,
                                     BUS_TRANSPORT_OK,
                                     response,
                                     response_length);
}

static bool expect_complete_poll(poll_device_fixture_t *fixture,
                                 int16_t speed_rpm)
{
    const uint16_t position[] = {0U, 1U, 0U, 2U};
    const uint16_t speed[] = {(uint16_t)speed_rpm,
                              (uint16_t)(int16_t)-speed_rpm};
    const uint16_t current[] = {10U, 11U};
    const uint16_t status[] = {1U, 1U};
    const uint16_t motor_temp[] = {250U, 251U};
    const uint16_t voltage[] = {480U, 481U};
    const uint16_t mos_temp[] = {300U, 301U};
    const uint16_t error[] = {0U, 0U, 0U, 0U};
    return expect_read_response(fixture, 0x5418U, position, 4U) &&
           expect_read_response(fixture, 0x5410U, speed, 2U) &&
           expect_read_response(fixture, 0x5414U, current, 2U) &&
           expect_read_response(fixture, 0x5400U, status, 2U) &&
           expect_read_response(fixture, 0x5404U, motor_temp, 2U) &&
           expect_read_response(fixture, 0x540CU, voltage, 2U) &&
           expect_read_response(fixture, 0x5408U, mos_temp, 2U) &&
           expect_read_response(fixture, 0x5420U, error, 4U);
}

static bool expect_fast_poll(poll_device_fixture_t *fixture,
                             int16_t speed_rpm)
{
    const uint16_t position[] = {0U, 3U, 0U, 4U};
    const uint16_t speed[] = {(uint16_t)speed_rpm,
                              (uint16_t)(int16_t)-speed_rpm};
    const uint16_t current[] = {12U, 13U};
    return expect_read_response(fixture, 0x5418U, position, 4U) &&
           expect_read_response(fixture, 0x5410U, speed, 2U) &&
           expect_read_response(fixture, 0x5414U, current, 2U);
}

static bool expect_failed_complete_poll(poll_device_fixture_t *fixture,
                                        bus_transport_result_t result)
{
    return expect_read_result(fixture, 0x5418U, 4U, result) &&
           expect_read_result(fixture, 0x5410U, 2U, result) &&
           expect_read_result(fixture, 0x5414U, 2U, result) &&
           expect_read_result(fixture, 0x5400U, 2U, result) &&
           expect_read_result(fixture, 0x5404U, 2U, result) &&
           expect_read_result(fixture, 0x540CU, 2U, result) &&
           expect_read_result(fixture, 0x5408U, 2U, result) &&
           expect_read_result(fixture, 0x5420U, 4U, result);
}

static bool test_one_device_period_and_next_delay(void)
{
    test_clock_t clock = {.now_ms = 100U};
    poll_device_fixture_t fixture;
    svd48_poll_service_t service;
    HOST_TEST_CHECK(poll_device_fixture_init(&fixture, &clock, 1U, 1U, 1000U));
    HOST_TEST_CHECK(svd48_poll_service_init(&service, test_clock_ms, &clock));
    HOST_TEST_CHECK(svd48_poll_service_add_device(&service,
                                                 &fixture.device,
                                                 30U));
    HOST_TEST_CHECK(expect_complete_poll(&fixture, 10));
    HOST_TEST_CHECK(svd48_poll_service_run_once(&service) == SVD48_DEVICE_OK);
    HOST_TEST_CHECK(service.entries[0].scheduled);
    HOST_TEST_CHECK(service.entries[0].next_poll_ms == 130U);
    HOST_TEST_CHECK(svd48_poll_service_next_delay_ms(&service, 100U) == 30U);
    HOST_TEST_CHECK(fake_bus_transport_all_expectations_met(&fixture.bus));
    return true;
}

static bool test_two_devices_keep_independent_periods(void)
{
    test_clock_t clock = {.now_ms = 100U};
    poll_device_fixture_t first;
    poll_device_fixture_t second;
    svd48_poll_service_t service;
    HOST_TEST_CHECK(poll_device_fixture_init(&first, &clock, 1U, 1U, 1000U));
    HOST_TEST_CHECK(poll_device_fixture_init(&second, &clock, 2U, 2U, 1000U));
    HOST_TEST_CHECK(svd48_poll_service_init(&service, test_clock_ms, &clock));
    HOST_TEST_CHECK(svd48_poll_service_add_device(&service, &first.device, 30U));
    HOST_TEST_CHECK(svd48_poll_service_add_device(&service, &second.device, 50U));
    HOST_TEST_CHECK(expect_complete_poll(&first, 10));
    HOST_TEST_CHECK(expect_complete_poll(&second, 20));
    HOST_TEST_CHECK(svd48_poll_service_run_once(&service) == SVD48_DEVICE_OK);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&first.bus) == 8U);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&second.bus) == 8U);

    clock.now_ms = 130U;
    HOST_TEST_CHECK(expect_fast_poll(&first, 11));
    HOST_TEST_CHECK(svd48_poll_service_run_once(&service) == SVD48_DEVICE_OK);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&first.bus) == 11U);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&second.bus) == 8U);
    HOST_TEST_CHECK(svd48_poll_service_next_delay_ms(&service, 100U) == 20U);

    clock.now_ms = 150U;
    HOST_TEST_CHECK(expect_fast_poll(&second, 21));
    HOST_TEST_CHECK(svd48_poll_service_run_once(&service) == SVD48_DEVICE_OK);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&first.bus) == 11U);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&second.bus) == 11U);
    HOST_TEST_CHECK(svd48_poll_service_next_delay_ms(&service, 100U) == 10U);
    return true;
}

static bool test_duplicate_limit_and_period_bounds(void)
{
    test_clock_t clock = {0};
    poll_device_fixture_t fixtures[SVD48_POLL_SERVICE_MAX_DEVICES + 1U];
    svd48_poll_service_t service;
    HOST_TEST_CHECK(svd48_poll_service_init(&service, test_clock_ms, &clock));
    for (size_t index = 0U; index < HOST_TEST_ARRAY_COUNT(fixtures); ++index) {
        HOST_TEST_CHECK(poll_device_fixture_init(&fixtures[index],
                                                &clock,
                                                (uint16_t)(index + 1U),
                                                (uint8_t)(index + 1U),
                                                1000U));
    }
    HOST_TEST_CHECK(svd48_poll_service_add_device(&service,
                                                 &fixtures[0].device,
                                                 30U));
    HOST_TEST_CHECK(!svd48_poll_service_add_device(&service,
                                                  &fixtures[0].device,
                                                  30U));
    for (size_t index = 1U; index < SVD48_POLL_SERVICE_MAX_DEVICES; ++index) {
        HOST_TEST_CHECK(svd48_poll_service_add_device(&service,
                                                     &fixtures[index].device,
                                                     30U));
    }
    HOST_TEST_CHECK(!svd48_poll_service_add_device(
        &service, &fixtures[SVD48_POLL_SERVICE_MAX_DEVICES].device, 30U));

    svd48_poll_service_t periods;
    HOST_TEST_CHECK(svd48_poll_service_init(&periods, test_clock_ms, &clock));
    HOST_TEST_CHECK(!svd48_poll_service_add_device(&periods,
                                                  &fixtures[0].device,
                                                  (uint32_t)INT32_MAX));
    HOST_TEST_CHECK(svd48_poll_service_add_device(&periods,
                                                 &fixtures[0].device,
                                                 (uint32_t)INT32_MAX - 1U));
    return true;
}

static bool test_error_backoff_never_accelerates_slow_period(void)
{
    test_clock_t clock = {.now_ms = 100U};
    poll_device_fixture_t fixture;
    svd48_poll_service_t service;
    HOST_TEST_CHECK(poll_device_fixture_init(&fixture, &clock, 1U, 1U, 1000U));
    HOST_TEST_CHECK(svd48_poll_service_init(&service, test_clock_ms, &clock));
    HOST_TEST_CHECK(svd48_poll_service_add_device(&service,
                                                 &fixture.device,
                                                 1000U));
    HOST_TEST_CHECK(expect_failed_complete_poll(&fixture, BUS_TRANSPORT_TIMEOUT));
    HOST_TEST_CHECK(svd48_poll_service_run_once(&service) == SVD48_DEVICE_TIMEOUT);
    HOST_TEST_CHECK(service.entries[0].consecutive_failures == 1U);
    HOST_TEST_CHECK(svd48_poll_service_next_delay_ms(&service, 2000U) == 1000U);

    clock.now_ms = 1100U;
    HOST_TEST_CHECK(expect_fast_poll(&fixture, 10));
    HOST_TEST_CHECK(svd48_poll_service_run_once(&service) == SVD48_DEVICE_OK);
    HOST_TEST_CHECK(service.entries[0].consecutive_failures == 0U);
    HOST_TEST_CHECK(service.entries[0].next_poll_ms == 2100U);
    return true;
}

static bool test_partial_poll_uses_backoff_until_complete_recovery(void)
{
    test_clock_t clock = {.now_ms = 100U};
    poll_device_fixture_t fixture;
    svd48_poll_service_t service;
    HOST_TEST_CHECK(poll_device_fixture_init(&fixture, &clock, 1U, 1U, 1000U));
    HOST_TEST_CHECK(svd48_poll_service_init(&service, test_clock_ms, &clock));
    HOST_TEST_CHECK(svd48_poll_service_add_device(&service,
                                                 &fixture.device,
                                                 30U));
    HOST_TEST_CHECK(expect_complete_poll(&fixture, 50));
    HOST_TEST_CHECK(svd48_poll_service_run_once(&service) == SVD48_DEVICE_OK);

    clock.now_ms = 130U;
    const uint16_t position[] = {0U, 5U, 0U, 6U};
    const uint16_t current[] = {20U, 21U};
    HOST_TEST_CHECK(expect_read_response(&fixture, 0x5418U, position, 4U));
    HOST_TEST_CHECK(expect_read_result(&fixture, 0x5410U, 2U,
                                      BUS_TRANSPORT_TIMEOUT));
    HOST_TEST_CHECK(expect_read_response(&fixture, 0x5414U, current, 2U));
    HOST_TEST_CHECK(svd48_poll_service_run_once(&service) == SVD48_DEVICE_PARTIAL);
    HOST_TEST_CHECK(service.entries[0].last_result == SVD48_DEVICE_PARTIAL);
    HOST_TEST_CHECK(service.entries[0].consecutive_failures == 1U);
    HOST_TEST_CHECK(service.entries[0].next_poll_ms == 380U);

    clock.now_ms = 380U;
    HOST_TEST_CHECK(expect_fast_poll(&fixture, 51));
    HOST_TEST_CHECK(svd48_poll_service_run_once(&service) == SVD48_DEVICE_OK);
    HOST_TEST_CHECK(service.entries[0].consecutive_failures == 0U);
    HOST_TEST_CHECK(service.entries[0].next_poll_ms == 410U);
    return true;
}

static bool test_deadlines_start_when_each_poll_finishes(void)
{
    test_clock_t clock = {.now_ms = 100U};
    poll_device_fixture_t fixture;
    svd48_poll_service_t service;
    poll_service_worker_t worker = {
        .service = &service,
        .result = SVD48_DEVICE_INVALID_ARGUMENT,
    };
    pthread_t thread;

    HOST_TEST_CHECK(poll_device_fixture_init(&fixture, &clock, 1U, 1U, 1000U));
    HOST_TEST_CHECK(svd48_poll_service_init(&service, test_clock_ms, &clock));
    HOST_TEST_CHECK(svd48_poll_service_add_device(&service,
                                                 &fixture.device,
                                                 30U));
    HOST_TEST_CHECK(expect_failed_complete_poll(&fixture,
                                                BUS_TRANSPORT_TIMEOUT));
    fake_bus_transport_pause_next_exchange(&fixture.bus);
    HOST_TEST_CHECK(pthread_create(&thread,
                                  NULL,
                                  poll_service_worker_run,
                                  &worker) == 0);
    fake_bus_transport_wait_until_exchange_paused(&fixture.bus);
    clock.now_ms = 600U;
    fake_bus_transport_resume_exchange(&fixture.bus);
    HOST_TEST_CHECK(pthread_join(thread, NULL) == 0);

    HOST_TEST_CHECK(worker.result == SVD48_DEVICE_TIMEOUT);
    HOST_TEST_CHECK(service.entries[0].consecutive_failures == 1U);
    HOST_TEST_CHECK(service.entries[0].next_poll_ms == 850U);
    HOST_TEST_CHECK(svd48_poll_service_next_delay_ms(&service, 1000U) == 250U);

    clock.now_ms = 850U;
    HOST_TEST_CHECK(expect_fast_poll(&fixture, 10));
    fake_bus_transport_pause_next_exchange(&fixture.bus);
    worker.result = SVD48_DEVICE_INVALID_ARGUMENT;
    HOST_TEST_CHECK(pthread_create(&thread,
                                  NULL,
                                  poll_service_worker_run,
                                  &worker) == 0);
    fake_bus_transport_wait_until_exchange_paused(&fixture.bus);
    clock.now_ms = 1000U;
    fake_bus_transport_resume_exchange(&fixture.bus);
    HOST_TEST_CHECK(pthread_join(thread, NULL) == 0);

    HOST_TEST_CHECK(worker.result == SVD48_DEVICE_OK);
    HOST_TEST_CHECK(service.entries[0].consecutive_failures == 0U);
    HOST_TEST_CHECK(service.entries[0].next_poll_ms == 1030U);
    HOST_TEST_CHECK(svd48_poll_service_next_delay_ms(&service, 1000U) == 30U);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&fixture.bus) == 11U);
    HOST_TEST_CHECK(fake_bus_transport_all_expectations_met(&fixture.bus));
    return true;
}

static bool test_speed_freshness_expires_while_current_remains_online(void)
{
    test_clock_t clock = {.now_ms = 100U};
    poll_device_fixture_t fixture;
    svd48_poll_service_t service;
    HOST_TEST_CHECK(poll_device_fixture_init(&fixture, &clock, 1U, 1U, 1000U));
    HOST_TEST_CHECK(svd48_poll_service_init(&service, test_clock_ms, &clock));
    HOST_TEST_CHECK(svd48_poll_service_add_device(&service,
                                                 &fixture.device,
                                                 30U));
    HOST_TEST_CHECK(expect_complete_poll(&fixture, 50));
    HOST_TEST_CHECK(svd48_poll_service_run_once(&service) == SVD48_DEVICE_OK);

    clock.now_ms = 1201U;
    const uint16_t position[] = {0U, 7U, 0U, 8U};
    const uint16_t current[] = {30U, 31U};
    HOST_TEST_CHECK(expect_read_response(&fixture, 0x5418U, position, 4U));
    HOST_TEST_CHECK(expect_read_result(&fixture, 0x5410U, 2U,
                                      BUS_TRANSPORT_TIMEOUT));
    HOST_TEST_CHECK(expect_read_response(&fixture, 0x5414U, current, 2U));
    HOST_TEST_CHECK(svd48_poll_service_run_once(&service) == SVD48_DEVICE_PARTIAL);
    svd48_channel_t *channel = svd48_device_channel(&fixture.device,
                                                    SVD48_CHANNEL_M1);
    svd48_channel_snapshot_t snapshot;
    HOST_TEST_CHECK(svd48_channel_get_snapshot(channel, &snapshot));
    HOST_TEST_CHECK(snapshot.online);
    HOST_TEST_CHECK(snapshot.stale);
    HOST_TEST_CHECK((snapshot.stale_observations & SVD48_OBSERVATION_SPEED) != 0U);
    HOST_TEST_CHECK((snapshot.stale_observations & SVD48_OBSERVATION_CURRENT) == 0U);
    HOST_TEST_CHECK(svd48_channel_get_health(channel) == SVD48_CHANNEL_HEALTH_STALE);
    return true;
}

static bool test_wrapped_zero_deadline_is_not_an_initial_sentinel(void)
{
    test_clock_t clock = {.now_ms = UINT32_MAX - 9U};
    poll_device_fixture_t fixture;
    svd48_poll_service_t service;
    HOST_TEST_CHECK(poll_device_fixture_init(&fixture, &clock, 1U, 1U, 1000U));
    HOST_TEST_CHECK(svd48_poll_service_init(&service, test_clock_ms, &clock));
    HOST_TEST_CHECK(svd48_poll_service_add_device(&service,
                                                 &fixture.device,
                                                 10U));
    HOST_TEST_CHECK(expect_complete_poll(&fixture, 10));
    HOST_TEST_CHECK(svd48_poll_service_run_once(&service) == SVD48_DEVICE_OK);
    HOST_TEST_CHECK(service.entries[0].scheduled);
    HOST_TEST_CHECK(service.entries[0].next_poll_ms == 0U);
    HOST_TEST_CHECK(svd48_poll_service_next_delay_ms(&service, 50U) == 10U);

    clock.now_ms = UINT32_MAX;
    HOST_TEST_CHECK(svd48_poll_service_next_delay_ms(&service, 50U) == 1U);
    HOST_TEST_CHECK(svd48_poll_service_run_once(&service) == SVD48_DEVICE_OK);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&fixture.bus) == 8U);

    clock.now_ms = 0U;
    HOST_TEST_CHECK(expect_fast_poll(&fixture, 11));
    HOST_TEST_CHECK(svd48_poll_service_run_once(&service) == SVD48_DEVICE_OK);
    HOST_TEST_CHECK(service.entries[0].next_poll_ms == 10U);
    return true;
}

static bool test_unconfigured_device_is_not_polled_or_failed(void)
{
    test_clock_t clock = {.now_ms = 100U};
    poll_device_fixture_t configured;
    poll_device_fixture_t omitted;
    svd48_poll_service_t service;
    HOST_TEST_CHECK(poll_device_fixture_init(&configured, &clock, 1U, 1U, 1000U));
    HOST_TEST_CHECK(poll_device_fixture_init(&omitted, &clock, 2U, 2U, 1000U));
    HOST_TEST_CHECK(svd48_poll_service_init(&service, test_clock_ms, &clock));
    HOST_TEST_CHECK(svd48_poll_service_add_device(&service,
                                                 &configured.device,
                                                 30U));
    HOST_TEST_CHECK(expect_complete_poll(&configured, 10));
    HOST_TEST_CHECK(svd48_poll_service_run_once(&service) == SVD48_DEVICE_OK);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&omitted.bus) == 0U);
    svd48_channel_snapshot_t snapshot;
    HOST_TEST_CHECK(svd48_channel_get_snapshot(
                        svd48_device_channel(&omitted.device, SVD48_CHANNEL_M1),
                        &snapshot));
    HOST_TEST_CHECK(snapshot.valid_observations == 0U);
    HOST_TEST_CHECK(svd48_channel_get_health(
                        svd48_device_channel(&omitted.device, SVD48_CHANNEL_M1)) ==
                    SVD48_CHANNEL_HEALTH_OFFLINE);
    return true;
}

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(test_one_device_period_and_next_delay),
        HOST_TEST_CASE(test_two_devices_keep_independent_periods),
        HOST_TEST_CASE(test_duplicate_limit_and_period_bounds),
        HOST_TEST_CASE(test_error_backoff_never_accelerates_slow_period),
        HOST_TEST_CASE(test_partial_poll_uses_backoff_until_complete_recovery),
        HOST_TEST_CASE(test_deadlines_start_when_each_poll_finishes),
        HOST_TEST_CASE(test_speed_freshness_expires_while_current_remains_online),
        HOST_TEST_CASE(test_wrapped_zero_deadline_is_not_an_initial_sentinel),
        HOST_TEST_CASE(test_unconfigured_device_is_not_polled_or_failed),
    };
    host_test_summary_t summary =
        host_test_run_cases(cases, HOST_TEST_ARRAY_COUNT(cases), stdout);
    return host_test_exit_code(summary);
}
