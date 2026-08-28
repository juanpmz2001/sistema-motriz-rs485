#include "host_test.h"

#include <errno.h>
#include <limits.h>
#include "host_threads.h"
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "fake_bus_transport.h"
#include "svd48_device.h"

typedef struct {
    fake_bus_transport_t bus;
    svd48_device_t device;
    pthread_mutex_t state_lock;
    uint32_t now_ms;
} device_fixture_t;

typedef struct {
    svd48_device_t *device;
    svd48_device_result_t result;
    pthread_mutex_t lock;
    pthread_cond_t completed;
    bool done;
} poll_worker_t;

typedef struct {
    pthread_mutex_t lock;
    uint8_t count;
    uint8_t first_request[8];
    size_t first_request_length;
    uint8_t first_response[64];
    size_t first_response_length;
} diagnostic_trace_sink_t;

static void diagnostic_trace_capture(void *context,
                                     uint16_t device_id,
                                     uint8_t address,
                                     uint8_t attempt,
                                     const uint8_t *request,
                                     size_t request_length,
                                     const uint8_t *response,
                                     size_t response_length,
                                     svd48_device_result_t result)
{
    diagnostic_trace_sink_t *sink = context;
    if (!sink || !request || !response) return;
    pthread_mutex_lock(&sink->lock);
    if (sink->count++ == 0U) {
        sink->first_request_length = request_length;
        sink->first_response_length = response_length;
        memcpy(sink->first_request, request, request_length);
        memcpy(sink->first_response, response, response_length);
    }
    pthread_mutex_unlock(&sink->lock);
    (void)device_id; (void)address; (void)attempt; (void)result;
}

static void append_crc(uint8_t *frame, size_t payload_length)
{
    uint16_t crc = svd48_crc16_uumotor(frame, payload_length);
    frame[payload_length] = (uint8_t)(crc >> 8);
    frame[payload_length + 1U] = (uint8_t)(crc & 0xFFU);
}

static bool queue_read_result(fake_bus_transport_t *bus,
                              uint8_t address,
                              uint16_t reg,
                              uint16_t quantity,
                              bus_transport_result_t result)
{
    uint8_t request[8];
    size_t length = svd48_build_read_request(address, reg, quantity, request);
    return length == sizeof(request) &&
           fake_bus_transport_expect(bus, request, length, result, NULL, 0U);
}

static size_t build_read_response(uint8_t address,
                                  const uint16_t *values,
                                  uint16_t quantity,
                                  uint8_t response[64])
{
    if (!values || !response || quantity == 0U || quantity > 16U) {
        return 0U;
    }
    response[0] = address;
    response[1] = SVD48_FUNC_READ_HOLDING;
    response[2] = (uint8_t)(quantity * 2U);
    for (uint16_t index = 0; index < quantity; ++index) {
        response[3U + index * 2U] = (uint8_t)(values[index] >> 8);
        response[4U + index * 2U] = (uint8_t)(values[index] & 0xFFU);
    }
    size_t response_length = 5U + (size_t)quantity * 2U;
    append_crc(response, response_length - 2U);
    return response_length;
}

static bool queue_raw_read_response(fake_bus_transport_t *bus,
                                    uint8_t address,
                                    uint16_t reg,
                                    uint16_t quantity,
                                    const uint8_t *response,
                                    size_t response_length)
{
    uint8_t request[8];
    size_t request_length = svd48_build_read_request(address, reg, quantity, request);
    return request_length == sizeof(request) &&
           fake_bus_transport_expect(bus,
                                     request,
                                     request_length,
                                     BUS_TRANSPORT_OK,
                                     response,
                                     response_length);
}

static bool queue_read_response(fake_bus_transport_t *bus,
                                uint8_t address,
                                uint16_t reg,
                                const uint16_t *values,
                                uint16_t quantity)
{
    if (!values || quantity == 0U || quantity > 16U) {
        return false;
    }
    uint8_t response[64] = {0};
    size_t response_length = build_read_response(address, values, quantity, response);
    return response_length > 0U &&
           queue_raw_read_response(bus,
                                   address,
                                   reg,
                                   quantity,
                                   response,
                                   response_length);
}

static bool queue_write_single_result(fake_bus_transport_t *bus,
                                      uint8_t address,
                                      uint16_t reg,
                                      uint16_t value,
                                      bus_transport_result_t result,
                                      bool echo)
{
    uint8_t request[8];
    size_t length = svd48_build_write_single_request(address, reg, value, request);
    return echo ? fake_bus_transport_expect_echo(bus, request, length, result)
                : fake_bus_transport_expect(bus, request, length, result, NULL, 0U);
}

static bool queue_write_multiple_result(fake_bus_transport_t *bus,
                                        uint8_t address,
                                        uint16_t reg,
                                        const uint16_t *values,
                                        uint16_t quantity,
                                        bus_transport_result_t result,
                                        bool acknowledge)
{
    uint8_t request[SVD48_WRITE_MULTIPLE_REQUEST_MAX_SIZE];
    uint8_t response[SVD48_WRITE_MULTIPLE_RESPONSE_SIZE] = {0};
    size_t length = svd48_build_write_multiple_request(address,
                                                       reg,
                                                       values,
                                                       quantity,
                                                       request,
                                                       sizeof(request));
    if (acknowledge) {
        response[0] = address;
        response[1] = SVD48_FUNC_WRITE_MULTI;
        response[2] = (uint8_t)(reg >> 8);
        response[3] = (uint8_t)(reg & 0xFFU);
        response[4] = (uint8_t)(quantity >> 8);
        response[5] = (uint8_t)(quantity & 0xFFU);
        append_crc(response, sizeof(response) - 2U);
    }
    return fake_bus_transport_expect(bus,
                                     request,
                                     length,
                                     result,
                                     acknowledge ? response : NULL,
                                     acknowledge ? sizeof(response) : 0U);
}

static bool state_lock_acquire(void *context)
{
    device_fixture_t *fixture = context;
    return fixture && pthread_mutex_lock(&fixture->state_lock) == 0;
}

static void state_lock_release(void *context)
{
    device_fixture_t *fixture = context;
    if (fixture) {
        pthread_mutex_unlock(&fixture->state_lock);
    }
}

static uint32_t fixture_clock_ms(void *context)
{
    const device_fixture_t *fixture = context;
    return fixture ? fixture->now_ms : 0U;
}

static bool fixture_init(device_fixture_t *fixture,
                         uint8_t retries,
                         uint32_t stale_timeout_ms)
{
    if (!fixture) {
        return false;
    }
    memset(fixture, 0, sizeof(*fixture));
    if (!fake_bus_transport_init(&fixture->bus, 10U)) {
        return false;
    }
    if (pthread_mutex_init(&fixture->state_lock, NULL) != 0) {
        fake_bus_transport_deinit(&fixture->bus);
        return false;
    }
    svd48_device_config_t config = {
        .device_id = 42U,
        .address = 1U,
        .transport = fake_bus_transport_port(&fixture->bus),
        .response_timeout_ms = 10U,
        .retries = retries,
        .stale_timeout_ms = stale_timeout_ms,
        .state_lock = {
            .acquire = state_lock_acquire,
            .release = state_lock_release,
            .context = fixture,
        },
        .clock_ms = fixture_clock_ms,
        .clock_context = fixture,
    };
    if (!svd48_device_init(&fixture->device, &config)) {
        pthread_mutex_destroy(&fixture->state_lock);
        fake_bus_transport_deinit(&fixture->bus);
        return false;
    }
    return true;
}

static bool queue_complete_poll_with_errors(fake_bus_transport_t *bus,
                                            int16_t m1_speed_rpm,
                                            int16_t m2_speed_rpm,
                                            int16_t m1_current_deciamp,
                                            int16_t m2_current_deciamp,
                                            uint32_t m1_error_code,
                                            uint32_t m2_error_code)
{
    const uint16_t position[] = {0U, 123U, UINT16_MAX, UINT16_MAX - 1U};
    const uint16_t speed[] = {(uint16_t)m1_speed_rpm, (uint16_t)m2_speed_rpm};
    const uint16_t current[] = {(uint16_t)m1_current_deciamp,
                                (uint16_t)m2_current_deciamp};
    const uint16_t status[] = {1U, 0U};
    const uint16_t motor_temp[] = {250U, 260U};
    const uint16_t bus_voltage[] = {480U, 481U};
    const uint16_t mos_temp[] = {300U, 310U};
    const uint16_t error_code[] = {
        (uint16_t)(m1_error_code >> 16U),
        (uint16_t)m1_error_code,
        (uint16_t)(m2_error_code >> 16U),
        (uint16_t)m2_error_code,
    };
    return queue_read_response(bus, 1U, 0x5418U, position, 4U) &&
           queue_read_response(bus, 1U, 0x5410U, speed, 2U) &&
           queue_read_response(bus, 1U, 0x5414U, current, 2U) &&
           queue_read_response(bus, 1U, 0x5400U, status, 2U) &&
           queue_read_response(bus, 1U, 0x5404U, motor_temp, 2U) &&
           queue_read_response(bus, 1U, 0x540CU, bus_voltage, 2U) &&
           queue_read_response(bus, 1U, 0x5408U, mos_temp, 2U) &&
           queue_read_response(bus, 1U, 0x5420U, error_code, 4U);
}

static bool queue_complete_poll(fake_bus_transport_t *bus,
                                int16_t m1_speed_rpm,
                                int16_t m2_speed_rpm,
                                int16_t m1_current_deciamp,
                                int16_t m2_current_deciamp)
{
    return queue_complete_poll_with_errors(bus,
                                           m1_speed_rpm,
                                           m2_speed_rpm,
                                           m1_current_deciamp,
                                           m2_current_deciamp,
                                           0U,
                                           0U);
}

static bool queue_fast_poll(fake_bus_transport_t *bus,
                            int16_t m1_speed_rpm,
                            int16_t m2_speed_rpm,
                            int16_t m1_current_deciamp,
                            int16_t m2_current_deciamp)
{
    const uint16_t position[] = {0U, 124U, UINT16_MAX, UINT16_MAX - 2U};
    const uint16_t speed[] = {(uint16_t)m1_speed_rpm, (uint16_t)m2_speed_rpm};
    const uint16_t current[] = {(uint16_t)m1_current_deciamp,
                                (uint16_t)m2_current_deciamp};
    return queue_read_response(bus, 1U, 0x5418U, position, 4U) &&
           queue_read_response(bus, 1U, 0x5410U, speed, 2U) &&
           queue_read_response(bus, 1U, 0x5414U, current, 2U);
}

static bool poll_worker_init(poll_worker_t *worker, svd48_device_t *device)
{
    if (!worker || !device) {
        return false;
    }
    memset(worker, 0, sizeof(*worker));
    worker->device = device;
    if (pthread_mutex_init(&worker->lock, NULL) != 0) {
        return false;
    }
    if (pthread_cond_init(&worker->completed, NULL) != 0) {
        pthread_mutex_destroy(&worker->lock);
        return false;
    }
    return true;
}

static void poll_worker_deinit(poll_worker_t *worker)
{
    if (worker) {
        pthread_cond_destroy(&worker->completed);
        pthread_mutex_destroy(&worker->lock);
    }
}

static void *poll_worker_run(void *context)
{
    poll_worker_t *worker = context;
    worker->result = svd48_device_poll(worker->device);
    pthread_mutex_lock(&worker->lock);
    worker->done = true;
    pthread_cond_broadcast(&worker->completed);
    pthread_mutex_unlock(&worker->lock);
    return NULL;
}

static bool poll_worker_wait(poll_worker_t *worker, long timeout_ms)
{
    struct timespec deadline;
    if (!worker || timeout_ms <= 0 ||
        timespec_get(&deadline, TIME_UTC) != TIME_UTC) {
        return false;
    }
    deadline.tv_sec += timeout_ms / 1000L;
    deadline.tv_nsec += (timeout_ms % 1000L) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&worker->lock);
    int result = 0;
    while (!worker->done && result == 0) {
        result = pthread_cond_timedwait(&worker->completed,
                                        &worker->lock,
                                        &deadline);
    }
    bool done = worker->done;
    pthread_mutex_unlock(&worker->lock);
    return done && (result == 0 || result == ETIMEDOUT);
}

static bool test_identity_channels_and_initial_health(void)
{
    device_fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture, 2U, 1000U));
    HOST_TEST_CHECK(svd48_device_id(&fixture.device) == 42U);
    HOST_TEST_CHECK(svd48_device_address(&fixture.device) == 1U);
    HOST_TEST_CHECK(svd48_device_transport(&fixture.device) ==
                    fake_bus_transport_port(&fixture.bus));
    HOST_TEST_CHECK(svd48_device_channel(&fixture.device, SVD48_CHANNEL_M1) != NULL);
    HOST_TEST_CHECK(svd48_device_channel(&fixture.device, SVD48_CHANNEL_M2) != NULL);
    HOST_TEST_CHECK(svd48_channel_control_register(SVD48_CHANNEL_M1) == 0x5300U);
    HOST_TEST_CHECK(svd48_channel_control_register(SVD48_CHANNEL_M2) == 0x5301U);
    HOST_TEST_CHECK(svd48_channel_velocity_register(SVD48_CHANNEL_M1) == 0x5304U);
    HOST_TEST_CHECK(svd48_channel_velocity_register(SVD48_CHANNEL_M2) == 0x5305U);
    HOST_TEST_CHECK(svd48_channel_hall_calibration_trigger_register(
                        SVD48_CHANNEL_M1) == 0x5600U);
    HOST_TEST_CHECK(svd48_channel_hall_calibration_trigger_register(
                        SVD48_CHANNEL_M2) == 0x5601U);
    HOST_TEST_CHECK(svd48_channel_hall_calibration_status_register(
                        SVD48_CHANNEL_M1) == 0x5684U);
    HOST_TEST_CHECK(svd48_channel_hall_calibration_status_register(
                        SVD48_CHANNEL_M2) == 0x5685U);

    svd48_channel_snapshot_t snapshot;
    svd48_channel_t *m1 = svd48_device_channel(&fixture.device, SVD48_CHANNEL_M1);
    HOST_TEST_CHECK(svd48_channel_get_snapshot(m1, &snapshot));
    HOST_TEST_CHECK(!snapshot.online);
    HOST_TEST_CHECK(snapshot.stale);
    HOST_TEST_CHECK(snapshot.valid_observations == 0U);
    HOST_TEST_CHECK(snapshot.stale_observations == SVD48_OBSERVATION_ALL);
    HOST_TEST_CHECK(svd48_channel_get_health(m1) == SVD48_CHANNEL_HEALTH_OFFLINE);

    svd48_device_t invalid_device;
    svd48_device_config_t invalid_config = fixture.device.config;
    invalid_config.address = 248U;
    HOST_TEST_CHECK(!svd48_device_init(&invalid_device, &invalid_config));
    invalid_config.address = 1U;
    invalid_config.retries = SVD48_DEVICE_MAX_RETRIES + 1U;
    HOST_TEST_CHECK(!svd48_device_init(&invalid_device, &invalid_config));
    return true;
}

static bool test_m1_m2_target_rpm_framing(void)
{
    device_fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture, 0U, 1000U));
    HOST_TEST_CHECK(queue_write_single_result(&fixture.bus,
                                             1U,
                                             0x5304U,
                                             (uint16_t)(int16_t)-123,
                                             BUS_TRANSPORT_OK,
                                             true));
    HOST_TEST_CHECK(queue_write_single_result(&fixture.bus,
                                             1U,
                                             0x5305U,
                                             321U,
                                             BUS_TRANSPORT_OK,
                                             true));
    HOST_TEST_CHECK(svd48_channel_set_target_rpm(
                        svd48_device_channel(&fixture.device, SVD48_CHANNEL_M1), -123) ==
                    SVD48_DEVICE_OK);
    HOST_TEST_CHECK(svd48_channel_set_target_rpm(
                        svd48_device_channel(&fixture.device, SVD48_CHANNEL_M2), 321) ==
                    SVD48_DEVICE_OK);
    HOST_TEST_CHECK(fake_bus_transport_mismatch(&fixture.bus) ==
                    FAKE_BUS_TRANSPORT_MISMATCH_NONE);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&fixture.bus) == 2U);
    return true;
}

static bool test_enable_stop_clear_and_current_framing(void)
{
    device_fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture, 0U, 1000U));
    HOST_TEST_CHECK(queue_write_single_result(&fixture.bus, 1U, 0x5301U, 1U,
                                             BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(queue_write_single_result(&fixture.bus, 1U, 0x5304U, 0U,
                                             BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(queue_write_single_result(&fixture.bus, 1U, 0x5300U, 0U,
                                             BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(queue_write_single_result(&fixture.bus, 1U, 0x5301U, 2U,
                                             BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(queue_write_single_result(&fixture.bus, 1U, 0x5308U,
                                             (uint16_t)(int16_t)-9,
                                             BUS_TRANSPORT_OK, true));

    HOST_TEST_CHECK(svd48_channel_enable(
                        svd48_device_channel(&fixture.device, SVD48_CHANNEL_M2)) ==
                    SVD48_DEVICE_OK);
    HOST_TEST_CHECK(svd48_channel_stop(
                        svd48_device_channel(&fixture.device, SVD48_CHANNEL_M1)) ==
                    SVD48_DEVICE_OK);
    HOST_TEST_CHECK(svd48_channel_clear_fault(
                        svd48_device_channel(&fixture.device, SVD48_CHANNEL_M2)) ==
                    SVD48_DEVICE_OK);
    HOST_TEST_CHECK(svd48_channel_set_current_deciamp(
                        svd48_device_channel(&fixture.device, SVD48_CHANNEL_M1), -9) ==
                    SVD48_DEVICE_OK);
    HOST_TEST_CHECK(fake_bus_transport_mismatch(&fixture.bus) ==
                    FAKE_BUS_TRANSPORT_MISMATCH_NONE);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&fixture.bus) == 5U);
    return true;
}

static bool test_hall_calibration_is_one_shot_with_independent_status(void)
{
    device_fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture, 0U, 1000U));
    const uint16_t m1_calibrating[] = {1U};
    const uint16_t m2_success[] = {0U};
    HOST_TEST_CHECK(queue_write_single_result(&fixture.bus,
                                             1U,
                                             0x5600U,
                                             1U,
                                             BUS_TRANSPORT_OK,
                                             true));
    HOST_TEST_CHECK(queue_read_response(&fixture.bus,
                                       1U,
                                       0x5684U,
                                       m1_calibrating,
                                       1U));
    HOST_TEST_CHECK(queue_write_single_result(&fixture.bus,
                                             1U,
                                             0x5601U,
                                             1U,
                                             BUS_TRANSPORT_OK,
                                             true));
    HOST_TEST_CHECK(queue_read_response(&fixture.bus,
                                       1U,
                                       0x5685U,
                                       m2_success,
                                       1U));

    svd48_hall_calibration_result_t result;
    HOST_TEST_CHECK(svd48_channel_start_hall_calibration(
                        svd48_device_channel(&fixture.device, SVD48_CHANNEL_M1),
                        &result) == SVD48_DEVICE_OK);
    HOST_TEST_CHECK(result.trigger_register == 0x5600U);
    HOST_TEST_CHECK(result.status_register == 0x5684U);
    HOST_TEST_CHECK(result.write_acknowledged);
    HOST_TEST_CHECK(result.status_available);
    HOST_TEST_CHECK(result.status_value == 1U);
    HOST_TEST_CHECK(result.status == SVD48_HALL_CALIBRATION_STATUS_CALIBRATING);
    HOST_TEST_CHECK(result.status_read_result == SVD48_DEVICE_OK);
    HOST_TEST_CHECK(result.trace_count == 2U);
    HOST_TEST_CHECK(result.trace[0].attempt == 1U);
    HOST_TEST_CHECK(result.trace[0].result == SVD48_DEVICE_OK);
    HOST_TEST_CHECK(result.trace[0].request_length == 8U);
    HOST_TEST_CHECK(result.trace[0].response_length == 8U);
    HOST_TEST_CHECK(result.trace[0].request[0] == 1U);
    HOST_TEST_CHECK(result.trace[0].request[1] == SVD48_FUNC_WRITE_SINGLE);
    HOST_TEST_CHECK(result.trace[0].request[2] == 0x56U);
    HOST_TEST_CHECK(result.trace[0].request[3] == 0x00U);
    HOST_TEST_CHECK(memcmp(result.trace[0].request,
                           result.trace[0].response,
                           result.trace[0].request_length) == 0);
    HOST_TEST_CHECK(result.trace[1].request_length == 8U);
    HOST_TEST_CHECK(result.trace[1].response_length == 7U);
    HOST_TEST_CHECK(result.trace[1].request[1] == SVD48_FUNC_READ_HOLDING);
    HOST_TEST_CHECK(result.trace[1].request[2] == 0x56U);
    HOST_TEST_CHECK(result.trace[1].request[3] == 0x84U);

    HOST_TEST_CHECK(svd48_channel_start_hall_calibration(
                        svd48_device_channel(&fixture.device, SVD48_CHANNEL_M2),
                        &result) == SVD48_DEVICE_OK);
    HOST_TEST_CHECK(result.trigger_register == 0x5601U);
    HOST_TEST_CHECK(result.status_register == 0x5685U);
    HOST_TEST_CHECK(result.status == SVD48_HALL_CALIBRATION_STATUS_SUCCESS);
    HOST_TEST_CHECK(strcmp(svd48_hall_calibration_status_name(result.status),
                           "SUCCESS") == 0);
    HOST_TEST_CHECK(fake_bus_transport_mismatch(&fixture.bus) ==
                    FAKE_BUS_TRANSPORT_MISMATCH_NONE);

    device_fixture_t unverified;
    HOST_TEST_CHECK(fixture_init(&unverified, 0U, 1000U));
    HOST_TEST_CHECK(queue_write_single_result(&unverified.bus,
                                             1U,
                                             0x5600U,
                                             1U,
                                             BUS_TRANSPORT_OK,
                                             true));
    HOST_TEST_CHECK(queue_read_result(&unverified.bus,
                                     1U,
                                     0x5684U,
                                     1U,
                                     BUS_TRANSPORT_TIMEOUT));
    HOST_TEST_CHECK(svd48_channel_start_hall_calibration(
                        svd48_device_channel(&unverified.device,
                                             SVD48_CHANNEL_M1),
                        &result) == SVD48_DEVICE_PARTIAL);
    HOST_TEST_CHECK(result.write_acknowledged);
    HOST_TEST_CHECK(!result.status_available);
    HOST_TEST_CHECK(result.status_read_result == SVD48_DEVICE_TIMEOUT);
    HOST_TEST_CHECK(result.trace_count == 2U);
    HOST_TEST_CHECK(result.trace[1].result == SVD48_DEVICE_TIMEOUT);
    HOST_TEST_CHECK(result.trace[1].response_length == 0U);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&unverified.bus) == 2U);
    return true;
}

static bool test_diagnostic_trace_captures_real_crc_frames_with_bounded_sink(void)
{
    device_fixture_t fixture;
    diagnostic_trace_sink_t sink = {0};
    HOST_TEST_CHECK(fixture_init(&fixture, 0U, 1000U));
    HOST_TEST_CHECK(pthread_mutex_init(&sink.lock, NULL) == 0);
    HOST_TEST_CHECK(queue_write_single_result(&fixture.bus, 1U, 0x5304U, 7U,
                                             BUS_TRANSPORT_OK, true));
    svd48_device_set_diagnostic_trace(&fixture.device,
                                      diagnostic_trace_capture, &sink);
    HOST_TEST_CHECK(svd48_channel_set_target_rpm(
                        svd48_device_channel(&fixture.device, SVD48_CHANNEL_M1), 7) ==
                    SVD48_DEVICE_OK);
    svd48_device_set_diagnostic_trace(&fixture.device, NULL, NULL);
    HOST_TEST_CHECK(sink.count == 1U);
    HOST_TEST_CHECK(sink.first_request_length == 8U);
    HOST_TEST_CHECK(sink.first_response_length == 8U);
    HOST_TEST_CHECK(svd48_frame_has_valid_crc(sink.first_request,
                                               sink.first_request_length));
    HOST_TEST_CHECK(svd48_frame_has_valid_crc(sink.first_response,
                                               sink.first_response_length));
    HOST_TEST_CHECK(memcmp(sink.first_request, sink.first_response, 8U) == 0);
    pthread_mutex_destroy(&sink.lock);
    return true;
}

static bool test_retry_is_bounded_and_only_for_retryable_results(void)
{
    device_fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture, 2U, 1000U));
    for (size_t index = 0; index < 3U; ++index) {
        HOST_TEST_CHECK(queue_write_single_result(&fixture.bus, 1U, 0x5304U, 7U,
                                                 BUS_TRANSPORT_TIMEOUT, false));
    }
    HOST_TEST_CHECK(svd48_channel_set_target_rpm(
                        svd48_device_channel(&fixture.device, SVD48_CHANNEL_M1), 7) ==
                    SVD48_DEVICE_TIMEOUT);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&fixture.bus) == 3U);

    device_fixture_t cancelled;
    HOST_TEST_CHECK(fixture_init(&cancelled, 2U, 1000U));
    HOST_TEST_CHECK(queue_write_single_result(&cancelled.bus, 1U, 0x5304U, 7U,
                                             BUS_TRANSPORT_CANCELLED, false));
    HOST_TEST_CHECK(queue_write_single_result(&cancelled.bus, 1U, 0x5304U, 7U,
                                             BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(svd48_channel_set_target_rpm(
                        svd48_device_channel(&cancelled.device, SVD48_CHANNEL_M1), 7) ==
                    SVD48_DEVICE_CANCELLED);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&cancelled.bus) == 1U);
    return true;
}

static bool test_timeout_retry_can_recover(void)
{
    device_fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture, 2U, 1000U));
    HOST_TEST_CHECK(queue_write_single_result(&fixture.bus, 1U, 0x5304U, 11U,
                                             BUS_TRANSPORT_TIMEOUT, false));
    HOST_TEST_CHECK(queue_write_single_result(&fixture.bus, 1U, 0x5304U, 11U,
                                             BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(svd48_channel_set_target_rpm(
                        svd48_device_channel(&fixture.device, SVD48_CHANNEL_M1), 11) ==
                    SVD48_DEVICE_OK);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&fixture.bus) == 2U);
    svd48_device_communication_t communication;
    HOST_TEST_CHECK(svd48_device_get_communication(&fixture.device,
                                                  &communication));
    HOST_TEST_CHECK(communication.transactions == 2U);
    HOST_TEST_CHECK(communication.successful_transactions == 1U);
    HOST_TEST_CHECK(communication.failed_transactions == 1U);
    HOST_TEST_CHECK(communication.consecutive_failures == 0U);
    HOST_TEST_CHECK(communication.last_error == SVD48_DEVICE_OK);
    return true;
}

static bool test_generic_writes_are_not_retried(void)
{
    device_fixture_t single;
    HOST_TEST_CHECK(fixture_init(&single, 2U, 1000U));
    HOST_TEST_CHECK(queue_write_single_result(&single.bus, 1U, 0x5018U, 10U,
                                             BUS_TRANSPORT_TIMEOUT, false));
    HOST_TEST_CHECK(queue_write_single_result(&single.bus, 1U, 0x5018U, 10U,
                                             BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(svd48_device_write_register(&single.device, 0x5018U, 10U) ==
                    SVD48_DEVICE_TIMEOUT);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&single.bus) == 1U);

    device_fixture_t multiple;
    const uint16_t values[] = {10U, 11U};
    HOST_TEST_CHECK(fixture_init(&multiple, 2U, 1000U));
    HOST_TEST_CHECK(queue_write_multiple_result(&multiple.bus, 1U, 0x5018U, values, 2U,
                                               BUS_TRANSPORT_TIMEOUT, false));
    HOST_TEST_CHECK(queue_write_multiple_result(&multiple.bus, 1U, 0x5018U, values, 2U,
                                               BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(svd48_device_write_registers(&multiple.device, 0x5018U, values, 2U) ==
                    SVD48_DEVICE_TIMEOUT);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&multiple.bus) == 1U);
    return true;
}

static bool test_overflowing_read_range_is_rejected_without_io(void)
{
    device_fixture_t fixture;
    uint16_t output[2] = {0U, 0U};
    HOST_TEST_CHECK(fixture_init(&fixture, 2U, 1000U));
    HOST_TEST_CHECK(svd48_device_read_registers(&fixture.device,
                                               UINT16_MAX,
                                               2U,
                                               output) ==
                    SVD48_DEVICE_INVALID_ARGUMENT);
    HOST_TEST_CHECK(fake_bus_transport_acquire_attempt_count(&fixture.bus) == 0U);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&fixture.bus) == 0U);
    svd48_device_communication_t communication;
    HOST_TEST_CHECK(svd48_device_get_communication(&fixture.device,
                                                  &communication));
    HOST_TEST_CHECK(communication.transactions == 0U);
    return true;
}

static bool test_address_probe_is_read_only_and_does_not_change_device_health(void)
{
    device_fixture_t fixture;
    uint16_t output[2] = {0U, 0U};
    const uint16_t bus_voltage[] = {480U, 481U};
    svd48_device_communication_t before;
    svd48_device_communication_t after;
    svd48_channel_snapshot_t channel_before;
    svd48_channel_snapshot_t channel_after;

    HOST_TEST_CHECK(fixture_init(&fixture, 2U, 1000U));
    HOST_TEST_CHECK(svd48_device_get_communication(&fixture.device, &before));
    HOST_TEST_CHECK(svd48_channel_get_snapshot(
        svd48_device_channel(&fixture.device, SVD48_CHANNEL_M1),
        &channel_before));

    HOST_TEST_CHECK(queue_read_response(&fixture.bus,
                                       7U,
                                       0x540CU,
                                       bus_voltage,
                                       2U));
    HOST_TEST_CHECK(svd48_device_probe_address(&fixture.device,
                                              7U,
                                              0x540CU,
                                              2U,
                                              output) == SVD48_DEVICE_OK);
    HOST_TEST_CHECK(output[0] == 480U);
    HOST_TEST_CHECK(output[1] == 481U);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&fixture.bus) == 1U);

    HOST_TEST_CHECK(queue_read_result(&fixture.bus,
                                     23U,
                                     0x540CU,
                                     2U,
                                     BUS_TRANSPORT_TIMEOUT));
    HOST_TEST_CHECK(svd48_device_probe_address(&fixture.device,
                                              23U,
                                              0x540CU,
                                              2U,
                                              output) == SVD48_DEVICE_TIMEOUT);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&fixture.bus) == 2U);

    HOST_TEST_CHECK(svd48_device_get_communication(&fixture.device, &after));
    HOST_TEST_CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    HOST_TEST_CHECK(svd48_channel_get_snapshot(
        svd48_device_channel(&fixture.device, SVD48_CHANNEL_M1),
        &channel_after));
    HOST_TEST_CHECK(memcmp(&channel_before,
                           &channel_after,
                           sizeof(channel_before)) == 0);

    HOST_TEST_CHECK(svd48_device_probe_address(&fixture.device,
                                              0U,
                                              0x540CU,
                                              2U,
                                              output) ==
                    SVD48_DEVICE_INVALID_ARGUMENT);
    HOST_TEST_CHECK(svd48_device_probe_address(&fixture.device,
                                              248U,
                                              0x540CU,
                                              2U,
                                              output) ==
                    SVD48_DEVICE_INVALID_ARGUMENT);
    HOST_TEST_CHECK(svd48_device_probe_address(&fixture.device,
                                              7U,
                                              UINT16_MAX,
                                              2U,
                                              output) ==
                    SVD48_DEVICE_INVALID_ARGUMENT);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&fixture.bus) == 2U);
    HOST_TEST_CHECK(fake_bus_transport_mismatch(&fixture.bus) ==
                    FAKE_BUS_TRANSPORT_MISMATCH_NONE);
    return true;
}

static bool test_direct_actuation_writes_are_denied(void)
{
    device_fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture, 0U, 1000U));
    const uint16_t blocked[] = {0x5300U, 0x5301U, 0x5304U,
                                0x5305U, 0x5308U, 0x5309U};
    for (size_t index = 0; index < HOST_TEST_ARRAY_COUNT(blocked); ++index) {
        HOST_TEST_CHECK(svd48_device_write_register(&fixture.device, blocked[index], 0U) ==
                        SVD48_DEVICE_UNSUPPORTED);
    }
    const uint16_t blocked_range[] = {1U, 2U, 3U};
    HOST_TEST_CHECK(svd48_device_write_registers(&fixture.device,
                                                0x5302U,
                                                blocked_range,
                                                3U) == SVD48_DEVICE_UNSUPPORTED);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&fixture.bus) == 0U);

    HOST_TEST_CHECK(queue_write_single_result(&fixture.bus, 1U, 0x5018U, 10U,
                                             BUS_TRANSPORT_OK, true));
    HOST_TEST_CHECK(svd48_device_write_register(&fixture.device, 0x5018U, 10U) ==
                    SVD48_DEVICE_OK);
    const uint16_t maintenance[] = {10U, 11U};
    HOST_TEST_CHECK(queue_write_multiple_result(&fixture.bus,
                                               1U,
                                               0x5018U,
                                               maintenance,
                                               2U,
                                               BUS_TRANSPORT_OK,
                                               true));
    HOST_TEST_CHECK(svd48_device_write_registers(&fixture.device,
                                                0x5018U,
                                                maintenance,
                                                2U) == SVD48_DEVICE_OK);
    HOST_TEST_CHECK(fake_bus_transport_mismatch(&fixture.bus) ==
                    FAKE_BUS_TRANSPORT_MISMATCH_NONE);
    return true;
}

static bool test_modbus_response_validation(void)
{
    uint16_t output = 0U;
    const uint16_t value[] = {0x1234U};
    uint8_t response[64] = {0};
    size_t response_length = build_read_response(1U, value, 1U, response);

    device_fixture_t crc;
    HOST_TEST_CHECK(fixture_init(&crc, 0U, 1000U));
    response[response_length - 1U] ^= 1U;
    HOST_TEST_CHECK(queue_raw_read_response(&crc.bus,
                                           1U,
                                           0x5018U,
                                           1U,
                                           response,
                                           response_length));
    HOST_TEST_CHECK(svd48_device_read_registers(&crc.device, 0x5018U, 1U, &output) ==
                    SVD48_DEVICE_CRC_ERROR);

    device_fixture_t length;
    HOST_TEST_CHECK(fixture_init(&length, 0U, 1000U));
    response[0] = 1U;
    response[1] = SVD48_FUNC_READ_HOLDING;
    response[2] = 0U;
    response_length = 5U;
    append_crc(response, 3U);
    HOST_TEST_CHECK(queue_raw_read_response(&length.bus,
                                           1U,
                                           0x5018U,
                                           1U,
                                           response,
                                           response_length));
    HOST_TEST_CHECK(svd48_device_read_registers(&length.device, 0x5018U, 1U, &output) ==
                    SVD48_DEVICE_BAD_RESPONSE);
    svd48_device_communication_t communication;
    HOST_TEST_CHECK(svd48_device_get_communication(&length.device,
                                                  &communication));
    HOST_TEST_CHECK(communication.transactions == 1U);
    HOST_TEST_CHECK(communication.successful_transactions == 0U);
    HOST_TEST_CHECK(communication.failed_transactions == 1U);

    device_fixture_t address;
    HOST_TEST_CHECK(fixture_init(&address, 0U, 1000U));
    response_length = build_read_response(2U, value, 1U, response);
    HOST_TEST_CHECK(queue_raw_read_response(&address.bus,
                                           1U,
                                           0x5018U,
                                           1U,
                                           response,
                                           response_length));
    HOST_TEST_CHECK(svd48_device_read_registers(&address.device, 0x5018U, 1U, &output) ==
                    SVD48_DEVICE_BAD_RESPONSE);

    device_fixture_t function;
    HOST_TEST_CHECK(fixture_init(&function, 0U, 1000U));
    response_length = build_read_response(1U, value, 1U, response);
    response[1] = SVD48_FUNC_WRITE_SINGLE;
    append_crc(response, response_length - 2U);
    HOST_TEST_CHECK(queue_raw_read_response(&function.bus,
                                           1U,
                                           0x5018U,
                                           1U,
                                           response,
                                           response_length));
    HOST_TEST_CHECK(svd48_device_read_registers(&function.device, 0x5018U, 1U, &output) ==
                    SVD48_DEVICE_BAD_RESPONSE);

    device_fixture_t exception;
    HOST_TEST_CHECK(fixture_init(&exception, 2U, 1000U));
    response[0] = 1U;
    response[1] = (uint8_t)(SVD48_FUNC_READ_HOLDING | 0x80U);
    response[2] = 2U;
    response_length = 5U;
    append_crc(response, 3U);
    HOST_TEST_CHECK(queue_raw_read_response(&exception.bus,
                                           1U,
                                           0x5018U,
                                           1U,
                                           response,
                                           response_length));
    HOST_TEST_CHECK(svd48_device_read_registers(&exception.device,
                                               0x5018U,
                                               1U,
                                               &output) == SVD48_DEVICE_EXCEPTION);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&exception.bus) == 1U);
    return true;
}

static bool test_successful_poll_has_rpm_and_fresh_observations(void)
{
    device_fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture, 0U, 1000U));
    fixture.now_ms = 100U;
    HOST_TEST_CHECK(queue_complete_poll(&fixture.bus, 120, -45, 10, -11));
    HOST_TEST_CHECK(svd48_device_poll(&fixture.device) == SVD48_DEVICE_OK);

    svd48_channel_snapshot_t m1_snapshot;
    svd48_channel_snapshot_t m2_snapshot;
    svd48_channel_t *m1 = svd48_device_channel(&fixture.device, SVD48_CHANNEL_M1);
    svd48_channel_t *m2 = svd48_device_channel(&fixture.device, SVD48_CHANNEL_M2);
    HOST_TEST_CHECK(svd48_channel_get_snapshot(m1, &m1_snapshot));
    HOST_TEST_CHECK(svd48_channel_get_snapshot(m2, &m2_snapshot));
    HOST_TEST_CHECK(m1_snapshot.observed_speed_rpm == 120);
    HOST_TEST_CHECK(m2_snapshot.observed_speed_rpm == -45);
    HOST_TEST_CHECK(m1_snapshot.current_deciamp == 10);
    HOST_TEST_CHECK(m2_snapshot.current_deciamp == -11);
    HOST_TEST_CHECK(m1_snapshot.valid_observations == SVD48_OBSERVATION_ALL);
    HOST_TEST_CHECK(m1_snapshot.failed_observations == 0U);
    HOST_TEST_CHECK(m1_snapshot.stale_observations == 0U);
    HOST_TEST_CHECK(m1_snapshot.last_poll_result == SVD48_DEVICE_OK);
    HOST_TEST_CHECK(m1_snapshot.online && !m1_snapshot.stale);
    HOST_TEST_CHECK(svd48_channel_get_health(m1) == SVD48_CHANNEL_HEALTH_HEALTHY);
    HOST_TEST_CHECK(fake_bus_transport_mismatch(&fixture.bus) ==
                    FAKE_BUS_TRANSPORT_MISMATCH_NONE);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&fixture.bus) == 8U);
    return true;
}

static bool test_partial_poll_preserves_independent_speed_freshness(void)
{
    device_fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture, 0U, 1000U));
    fixture.now_ms = 100U;
    HOST_TEST_CHECK(queue_complete_poll(&fixture.bus, 50, -50, 10, 11));
    HOST_TEST_CHECK(svd48_device_poll(&fixture.device) == SVD48_DEVICE_OK);

    const uint16_t position[] = {0U, 200U, 0U, 201U};
    const uint16_t current[] = {20U, 21U};
    fixture.now_ms = 200U;
    HOST_TEST_CHECK(queue_read_response(&fixture.bus, 1U, 0x5418U, position, 4U));
    HOST_TEST_CHECK(queue_read_result(&fixture.bus, 1U, 0x5410U, 2U,
                                     BUS_TRANSPORT_TIMEOUT));
    HOST_TEST_CHECK(queue_read_response(&fixture.bus, 1U, 0x5414U, current, 2U));
    HOST_TEST_CHECK(svd48_device_poll(&fixture.device) == SVD48_DEVICE_PARTIAL);

    svd48_channel_t *m1 = svd48_device_channel(&fixture.device, SVD48_CHANNEL_M1);
    svd48_channel_snapshot_t snapshot;
    HOST_TEST_CHECK(svd48_channel_get_snapshot(m1, &snapshot));
    HOST_TEST_CHECK(snapshot.observed_speed_rpm == 50);
    HOST_TEST_CHECK(snapshot.current_deciamp == 20);
    HOST_TEST_CHECK((snapshot.failed_observations & SVD48_OBSERVATION_SPEED) != 0U);
    HOST_TEST_CHECK((snapshot.failed_observations & SVD48_OBSERVATION_CURRENT) == 0U);
    HOST_TEST_CHECK(snapshot.last_poll_result == SVD48_DEVICE_PARTIAL);
    HOST_TEST_CHECK(snapshot.online && !snapshot.stale);
    HOST_TEST_CHECK(svd48_channel_get_health(m1) == SVD48_CHANNEL_HEALTH_DEGRADED);

    fixture.now_ms = 1201U;
    HOST_TEST_CHECK(queue_read_response(&fixture.bus, 1U, 0x5418U, position, 4U));
    HOST_TEST_CHECK(queue_read_result(&fixture.bus, 1U, 0x5410U, 2U,
                                     BUS_TRANSPORT_TIMEOUT));
    HOST_TEST_CHECK(queue_read_response(&fixture.bus, 1U, 0x5414U, current, 2U));
    HOST_TEST_CHECK(svd48_device_poll(&fixture.device) == SVD48_DEVICE_PARTIAL);
    HOST_TEST_CHECK(svd48_channel_get_snapshot(m1, &snapshot));
    HOST_TEST_CHECK(snapshot.online);
    HOST_TEST_CHECK(snapshot.stale);
    HOST_TEST_CHECK((snapshot.stale_observations & SVD48_OBSERVATION_SPEED) != 0U);
    HOST_TEST_CHECK((snapshot.stale_observations & SVD48_OBSERVATION_CURRENT) == 0U);
    HOST_TEST_CHECK(svd48_channel_get_health(m1) == SVD48_CHANNEL_HEALTH_STALE);
    return true;
}

static bool test_total_poll_failure_degrades_then_goes_offline(void)
{
    device_fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture, 0U, 1000U));
    fixture.now_ms = 100U;
    HOST_TEST_CHECK(queue_complete_poll(&fixture.bus, 50, -50, 10, 11));
    HOST_TEST_CHECK(svd48_device_poll(&fixture.device) == SVD48_DEVICE_OK);

    fixture.now_ms = 200U;
    HOST_TEST_CHECK(queue_read_result(&fixture.bus,
                                     1U,
                                     0x5418U,
                                     4U,
                                     BUS_TRANSPORT_TIMEOUT));
    HOST_TEST_CHECK(queue_read_result(&fixture.bus,
                                     1U,
                                     0x5410U,
                                     2U,
                                     BUS_TRANSPORT_TIMEOUT));
    HOST_TEST_CHECK(queue_read_result(&fixture.bus,
                                     1U,
                                     0x5414U,
                                     2U,
                                     BUS_TRANSPORT_TIMEOUT));
    HOST_TEST_CHECK(svd48_device_poll(&fixture.device) == SVD48_DEVICE_TIMEOUT);

    svd48_channel_t *m1 = svd48_device_channel(&fixture.device,
                                               SVD48_CHANNEL_M1);
    svd48_channel_snapshot_t snapshot;
    HOST_TEST_CHECK(svd48_channel_get_snapshot(m1, &snapshot));
    HOST_TEST_CHECK(snapshot.online && !snapshot.stale);
    HOST_TEST_CHECK(snapshot.last_poll_result == SVD48_DEVICE_TIMEOUT);
    HOST_TEST_CHECK((snapshot.failed_observations &
                     (SVD48_OBSERVATION_POSITION |
                      SVD48_OBSERVATION_SPEED |
                      SVD48_OBSERVATION_CURRENT)) ==
                    (SVD48_OBSERVATION_POSITION |
                     SVD48_OBSERVATION_SPEED |
                     SVD48_OBSERVATION_CURRENT));
    HOST_TEST_CHECK(svd48_channel_get_health(m1) ==
                    SVD48_CHANNEL_HEALTH_DEGRADED);

    fixture.now_ms = 1101U;
    HOST_TEST_CHECK(svd48_channel_get_snapshot(m1, &snapshot));
    HOST_TEST_CHECK(!snapshot.online && snapshot.stale);
    HOST_TEST_CHECK(svd48_channel_get_health(m1) ==
                    SVD48_CHANNEL_HEALTH_OFFLINE);
    return true;
}

static bool test_slow_diagnostics_do_not_make_live_velocity_communication_stale(void)
{
    svd48_channel_snapshot_t snapshot = {
        .online = true,
        .valid_observations = SVD48_OBSERVATION_ALL,
        .stale_observations = SVD48_OBSERVATION_STATUS |
                              SVD48_OBSERVATION_MOTOR_TEMP |
                              SVD48_OBSERVATION_MOS_TEMP |
                              SVD48_OBSERVATION_BUS_VOLTAGE |
                              SVD48_OBSERVATION_ERROR_CODE,
        .stale = true,
    };
    HOST_TEST_CHECK(svd48_channel_health_from_snapshot(&snapshot) ==
                    SVD48_CHANNEL_HEALTH_HEALTHY);
    snapshot.stale_observations |= SVD48_OBSERVATION_SPEED;
    HOST_TEST_CHECK(svd48_channel_health_from_snapshot(&snapshot) ==
                    SVD48_CHANNEL_HEALTH_STALE);
    snapshot.stale_observations = 0U;
    snapshot.failed_observations = SVD48_OBSERVATION_CURRENT;
    HOST_TEST_CHECK(svd48_channel_health_from_snapshot(&snapshot) ==
                    SVD48_CHANNEL_HEALTH_DEGRADED);
    return true;
}

static bool test_fresh_fault_clears_after_zero_error_poll(void)
{
    device_fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture, 0U, 1000U));
    fixture.now_ms = 100U;
    HOST_TEST_CHECK(queue_complete_poll_with_errors(&fixture.bus,
                                                   50,
                                                   -50,
                                                   10,
                                                   11,
                                                   0x00010002U,
                                                   0U));
    HOST_TEST_CHECK(svd48_device_poll(&fixture.device) == SVD48_DEVICE_OK);

    svd48_channel_t *m1 = svd48_device_channel(&fixture.device,
                                               SVD48_CHANNEL_M1);
    svd48_channel_t *m2 = svd48_device_channel(&fixture.device,
                                               SVD48_CHANNEL_M2);
    HOST_TEST_CHECK(svd48_channel_get_health(m1) == SVD48_CHANNEL_HEALTH_FAULT);
    HOST_TEST_CHECK(svd48_channel_get_health(m2) == SVD48_CHANNEL_HEALTH_HEALTHY);

    fixture.device.poll_count = 0U;
    fixture.now_ms = 130U;
    HOST_TEST_CHECK(queue_complete_poll(&fixture.bus, 51, -51, 12, 13));
    HOST_TEST_CHECK(svd48_device_poll(&fixture.device) == SVD48_DEVICE_OK);

    svd48_channel_snapshot_t snapshot;
    HOST_TEST_CHECK(svd48_channel_get_snapshot(m1, &snapshot));
    HOST_TEST_CHECK(snapshot.error_code == 0U);
    HOST_TEST_CHECK((snapshot.failed_observations &
                     SVD48_OBSERVATION_ERROR_CODE) == 0U);
    HOST_TEST_CHECK(svd48_channel_get_health(m1) ==
                    SVD48_CHANNEL_HEALTH_HEALTHY);
    return true;
}

static bool test_successful_read_clears_failed_observation(void)
{
    device_fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture, 0U, 1000U));
    fixture.now_ms = 100U;
    HOST_TEST_CHECK(queue_complete_poll(&fixture.bus, 50, -50, 10, 11));
    HOST_TEST_CHECK(svd48_device_poll(&fixture.device) == SVD48_DEVICE_OK);

    const uint16_t position[] = {0U, 200U, 0U, 201U};
    const uint16_t current[] = {20U, 21U};
    fixture.now_ms = 200U;
    HOST_TEST_CHECK(queue_read_response(&fixture.bus,
                                       1U,
                                       0x5418U,
                                       position,
                                       4U));
    HOST_TEST_CHECK(queue_read_result(&fixture.bus,
                                     1U,
                                     0x5410U,
                                     2U,
                                     BUS_TRANSPORT_TIMEOUT));
    HOST_TEST_CHECK(queue_read_response(&fixture.bus,
                                       1U,
                                       0x5414U,
                                       current,
                                       2U));
    HOST_TEST_CHECK(svd48_device_poll(&fixture.device) == SVD48_DEVICE_PARTIAL);

    svd48_channel_t *m1 = svd48_device_channel(&fixture.device,
                                               SVD48_CHANNEL_M1);
    svd48_channel_snapshot_t snapshot;
    HOST_TEST_CHECK(svd48_channel_get_snapshot(m1, &snapshot));
    HOST_TEST_CHECK((snapshot.failed_observations &
                     SVD48_OBSERVATION_SPEED) != 0U);
    HOST_TEST_CHECK(svd48_channel_get_health(m1) ==
                    SVD48_CHANNEL_HEALTH_DEGRADED);

    fixture.now_ms = 230U;
    HOST_TEST_CHECK(queue_fast_poll(&fixture.bus, 55, -55, 22, 23));
    HOST_TEST_CHECK(svd48_device_poll(&fixture.device) == SVD48_DEVICE_OK);
    HOST_TEST_CHECK(svd48_channel_get_snapshot(m1, &snapshot));
    HOST_TEST_CHECK(snapshot.observed_speed_rpm == 55);
    HOST_TEST_CHECK(snapshot.failed_observations == 0U);
    HOST_TEST_CHECK(snapshot.last_poll_result == SVD48_DEVICE_OK);
    HOST_TEST_CHECK(svd48_channel_get_health(m1) ==
                    SVD48_CHANNEL_HEALTH_HEALTHY);
    return true;
}

static bool test_fast_poll_after_initial_success(void)
{
    device_fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture, 0U, 1000U));
    fixture.now_ms = 100U;
    HOST_TEST_CHECK(queue_complete_poll(&fixture.bus, 10, 20, 1, 2));
    HOST_TEST_CHECK(svd48_device_poll(&fixture.device) == SVD48_DEVICE_OK);
    fixture.now_ms = 130U;
    HOST_TEST_CHECK(queue_fast_poll(&fixture.bus, 11, 21, 3, 4));
    HOST_TEST_CHECK(svd48_device_poll(&fixture.device) == SVD48_DEVICE_OK);
    svd48_channel_snapshot_t snapshot;
    HOST_TEST_CHECK(svd48_channel_get_snapshot(
                        svd48_device_channel(&fixture.device, SVD48_CHANNEL_M2),
                        &snapshot));
    HOST_TEST_CHECK(snapshot.observed_speed_rpm == 21);
    HOST_TEST_CHECK(snapshot.current_deciamp == 4);
    return true;
}

static bool test_concurrent_poll_is_rejected_and_does_not_advance_cycle(void)
{
    device_fixture_t fixture;
    poll_worker_t first;
    poll_worker_t second;
    pthread_t first_thread;
    pthread_t second_thread;
    HOST_TEST_CHECK(fixture_init(&fixture, 0U, 1000U));
    fixture.now_ms = 100U;
    HOST_TEST_CHECK(queue_complete_poll(&fixture.bus, 10, 20, 1, 2));
    HOST_TEST_CHECK(poll_worker_init(&first, &fixture.device));
    HOST_TEST_CHECK(poll_worker_init(&second, &fixture.device));
    fake_bus_transport_pause_next_exchange(&fixture.bus);
    HOST_TEST_CHECK(pthread_create(&first_thread, NULL, poll_worker_run, &first) == 0);
    fake_bus_transport_wait_until_exchange_paused(&fixture.bus);

    int second_create = pthread_create(&second_thread,
                                       NULL,
                                       poll_worker_run,
                                       &second);
    bool second_completed_while_first_was_paused = false;
    if (second_create == 0) {
        second_completed_while_first_was_paused = poll_worker_wait(&second, 250L);
    }
    fake_bus_transport_resume_exchange(&fixture.bus);
    int first_join = pthread_join(first_thread, NULL);
    int second_join = second_create == 0 ? pthread_join(second_thread, NULL) : -1;

    HOST_TEST_CHECK(second_create == 0);
    HOST_TEST_CHECK(first_join == 0);
    HOST_TEST_CHECK(second_join == 0);
    HOST_TEST_CHECK(second_completed_while_first_was_paused);
    HOST_TEST_CHECK(first.result == SVD48_DEVICE_OK);
    HOST_TEST_CHECK(second.result == SVD48_DEVICE_BUS_BUSY);
    HOST_TEST_CHECK(fixture.device.poll_count == 1U);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&fixture.bus) == 8U);
    HOST_TEST_CHECK(fake_bus_transport_mismatch(&fixture.bus) ==
                    FAKE_BUS_TRANSPORT_MISMATCH_NONE);

    HOST_TEST_CHECK(queue_fast_poll(&fixture.bus, 11, 21, 3, 4));
    HOST_TEST_CHECK(svd48_device_poll(&fixture.device) == SVD48_DEVICE_OK);
    HOST_TEST_CHECK(fixture.device.poll_count == 2U);
    poll_worker_deinit(&second);
    poll_worker_deinit(&first);
    return true;
}

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(test_identity_channels_and_initial_health),
        HOST_TEST_CASE(test_m1_m2_target_rpm_framing),
        HOST_TEST_CASE(test_enable_stop_clear_and_current_framing),
        HOST_TEST_CASE(test_hall_calibration_is_one_shot_with_independent_status),
        HOST_TEST_CASE(test_diagnostic_trace_captures_real_crc_frames_with_bounded_sink),
        HOST_TEST_CASE(test_retry_is_bounded_and_only_for_retryable_results),
        HOST_TEST_CASE(test_timeout_retry_can_recover),
        HOST_TEST_CASE(test_generic_writes_are_not_retried),
        HOST_TEST_CASE(test_overflowing_read_range_is_rejected_without_io),
        HOST_TEST_CASE(test_address_probe_is_read_only_and_does_not_change_device_health),
        HOST_TEST_CASE(test_direct_actuation_writes_are_denied),
        HOST_TEST_CASE(test_modbus_response_validation),
        HOST_TEST_CASE(test_successful_poll_has_rpm_and_fresh_observations),
        HOST_TEST_CASE(test_partial_poll_preserves_independent_speed_freshness),
        HOST_TEST_CASE(test_total_poll_failure_degrades_then_goes_offline),
        HOST_TEST_CASE(test_slow_diagnostics_do_not_make_live_velocity_communication_stale),
        HOST_TEST_CASE(test_fresh_fault_clears_after_zero_error_poll),
        HOST_TEST_CASE(test_successful_read_clears_failed_observation),
        HOST_TEST_CASE(test_fast_poll_after_initial_success),
        HOST_TEST_CASE(test_concurrent_poll_is_rejected_and_does_not_advance_cycle),
    };
    host_test_summary_t summary =
        host_test_run_cases(cases, HOST_TEST_ARRAY_COUNT(cases), stdout);
    return host_test_exit_code(summary);
}
