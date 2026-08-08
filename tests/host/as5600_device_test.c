#include "host_test.h"

#include <string.h>

#include "as5600_device.h"
#include "fake_as5600_register_port.h"

#define FIXTURE_RESPONSE_TIMEOUT_MS 17U
#define FIXTURE_STALE_TIMEOUT_MS 100U

typedef struct {
    fake_as5600_register_port_t register_port;
    as5600_device_t device;
    uint32_t now_ms;
    bool lock_available;
    bool lock_held;
    size_t lock_acquires;
    size_t lock_releases;
} as5600_fixture_t;

static uint32_t fixture_clock_ms(void *context)
{
    const as5600_fixture_t *fixture = context;
    return fixture != NULL ? fixture->now_ms : 0U;
}

static bool fixture_lock_acquire(void *context)
{
    as5600_fixture_t *fixture = context;
    if (fixture == NULL) {
        return false;
    }
    fixture->lock_acquires++;
    if (!fixture->lock_available || fixture->lock_held) {
        return false;
    }
    fixture->lock_held = true;
    return true;
}

static void fixture_lock_release(void *context)
{
    as5600_fixture_t *fixture = context;
    if (fixture != NULL) {
        fixture->lock_releases++;
        fixture->lock_held = false;
    }
}

static bool fixture_init(as5600_fixture_t *fixture,
                         bool read_diagnostics,
                         uint32_t stale_timeout_ms,
                         const as5600_calibration_lut_t *calibration)
{
    if (fixture == NULL) {
        return false;
    }
    memset(fixture, 0, sizeof(*fixture));
    if (!fake_as5600_register_port_init(&fixture->register_port)) {
        return false;
    }
    fixture->now_ms = 0U;
    fixture->lock_available = true;
    const as5600_device_config_t config = {
        .device_id = 7U,
        .i2c_address = AS5600_DEFAULT_I2C_ADDRESS,
        .register_read = fake_as5600_register_port_as_port(&fixture->register_port),
        .clock_ms = fixture_clock_ms,
        .clock_context = fixture,
        .response_timeout_ms = FIXTURE_RESPONSE_TIMEOUT_MS,
        .stale_timeout_ms = stale_timeout_ms,
        .read_diagnostics = read_diagnostics,
        .calibration = calibration,
        .state_lock = {
            .acquire = fixture_lock_acquire,
            .release = fixture_lock_release,
            .context = fixture,
        },
    };
    return as5600_device_init(&fixture->device, &config);
}

static bool queue_sample(as5600_fixture_t *fixture,
                         uint8_t status,
                         uint16_t raw_angle,
                         bool diagnostics,
                         uint8_t automatic_gain_control,
                         uint16_t magnitude)
{
    if (fixture == NULL) {
        return false;
    }
    const uint8_t primary_bytes[] = {
        status,
        (uint8_t)((raw_angle >> 8U) & 0x0FU),
        (uint8_t)(raw_angle & 0xFFU),
    };
    if (!fake_as5600_register_port_expect_read(&fixture->register_port,
                                                AS5600_DEFAULT_I2C_ADDRESS,
                                                0x0BU,
                                                sizeof(primary_bytes),
                                                FIXTURE_RESPONSE_TIMEOUT_MS,
                                                AS5600_DEVICE_OK,
                                                primary_bytes)) {
        return false;
    }
    if (!diagnostics) {
        return true;
    }

    const uint8_t diagnostic_bytes[] = {
        automatic_gain_control,
        (uint8_t)(magnitude >> 8U),
        (uint8_t)(magnitude & 0xFFU),
    };
    return fake_as5600_register_port_expect_read(&fixture->register_port,
                                                  AS5600_DEFAULT_I2C_ADDRESS,
                                                  0x1AU,
                                                  sizeof(diagnostic_bytes),
                                                  FIXTURE_RESPONSE_TIMEOUT_MS,
                                                  AS5600_DEVICE_OK,
                                                  diagnostic_bytes);
}

static bool queue_status_error(as5600_fixture_t *fixture,
                               as5600_device_result_t result)
{
    return fixture != NULL &&
           fake_as5600_register_port_expect_read(&fixture->register_port,
                                                 AS5600_DEFAULT_I2C_ADDRESS,
                                                 0x0BU,
                                                 3U,
                                                 FIXTURE_RESPONSE_TIMEOUT_MS,
                                                 result,
                                                 NULL);
}

static bool nearly_equal(float actual, float expected, float tolerance)
{
    return actual >= expected - tolerance && actual <= expected + tolerance;
}

static as5600_calibration_lut_t identity_lut(void)
{
    return (as5600_calibration_lut_t){
        .metadata = {
            .format_version = AS5600_CALIBRATION_FORMAT_VERSION,
            .calibration_id = "test-identity-v1",
            .hardware_identity = "host-fake-fixture",
            .provenance = "host-test synthetic calibration",
        },
        .correction_centidegrees = {0},
    };
}

static bool test_raw_conversion_diagnostics_and_healthy_position(void)
{
    as5600_fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture,
                                 true,
                                 FIXTURE_STALE_TIMEOUT_MS,
                                 NULL));
    fixture.now_ms = 100U;
    HOST_TEST_CHECK(queue_sample(&fixture,
                                 AS5600_STATUS_MAGNET_DETECTED,
                                 0x0ABCU,
                                 true,
                                 0x7AU,
                                 0x0A34U));
    HOST_TEST_CHECK(as5600_device_poll(&fixture.device) == AS5600_DEVICE_OK);
    HOST_TEST_CHECK(fake_as5600_register_port_complete(&fixture.register_port));

    as5600_device_snapshot_t raw;
    as5600_position_snapshot_t position;
    HOST_TEST_CHECK(as5600_device_get_snapshot(&fixture.device, &raw));
    HOST_TEST_CHECK(as5600_device_get_position_snapshot(&fixture.device, &position));
    HOST_TEST_CHECK(raw.raw_angle_valid);
    HOST_TEST_CHECK(raw.raw_angle == 0x0ABCU);
    HOST_TEST_CHECK(raw.magnet_detected);
    HOST_TEST_CHECK(raw.diagnostics_requested);
    HOST_TEST_CHECK(raw.diagnostics_valid);
    HOST_TEST_CHECK(raw.automatic_gain_control == 0x7AU);
    HOST_TEST_CHECK(raw.magnitude == 0x0A34U);
    HOST_TEST_CHECK(raw.sample_timestamp_ms == 100U);
    HOST_TEST_CHECK(raw.online);
    HOST_TEST_CHECK(!raw.stale);
    HOST_TEST_CHECK(raw.health == AS5600_DEVICE_HEALTH_HEALTHY);
    HOST_TEST_CHECK(position.valid);
    HOST_TEST_CHECK(!position.calibration_applied);
    HOST_TEST_CHECK(nearly_equal(position.degrees,
                                  as5600_raw_angle_degrees(0x0ABCU),
                                  0.0001f));
    return true;
}

static bool test_cached_diagnostics_never_poll_or_rejuvenate_state(void)
{
    as5600_calibration_lut_t lut = identity_lut();
    as5600_fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture,
                                 true,
                                 FIXTURE_STALE_TIMEOUT_MS,
                                 &lut));
    fixture.now_ms = 100U;
    HOST_TEST_CHECK(queue_sample(&fixture,
                                 AS5600_STATUS_MAGNET_DETECTED,
                                 0x0A55U,
                                 true,
                                 0x63U,
                                 0x0BEEU));
    HOST_TEST_CHECK(as5600_device_poll(&fixture.device) == AS5600_DEVICE_OK);
    HOST_TEST_CHECK(fake_as5600_register_port_complete(&fixture.register_port));

    const size_t reads_before = fixture.register_port.next_expectation;
    const size_t lock_acquires_before = fixture.lock_acquires;
    const size_t lock_releases_before = fixture.lock_releases;
    as5600_device_diagnostics_t diagnostics;
    HOST_TEST_CHECK(as5600_device_get_diagnostics(&fixture.device,
                                                   &diagnostics));
    HOST_TEST_CHECK(diagnostics.device_id == 7U);
    HOST_TEST_CHECK(diagnostics.i2c_address == AS5600_DEFAULT_I2C_ADDRESS);
    HOST_TEST_CHECK(diagnostics.snapshot.raw_angle_valid);
    HOST_TEST_CHECK(diagnostics.snapshot.raw_angle == 0x0A55U);
    HOST_TEST_CHECK(diagnostics.snapshot.magnet_detected);
    HOST_TEST_CHECK(diagnostics.snapshot.diagnostics_valid);
    HOST_TEST_CHECK(diagnostics.snapshot.automatic_gain_control == 0x63U);
    HOST_TEST_CHECK(diagnostics.snapshot.magnitude == 0x0BEEU);
    HOST_TEST_CHECK(diagnostics.communication.polls == 1U);
    HOST_TEST_CHECK(diagnostics.communication.successful_samples == 1U);
    HOST_TEST_CHECK(diagnostics.communication.failed_polls == 0U);
    HOST_TEST_CHECK(diagnostics.communication.consecutive_failures == 0U);
    HOST_TEST_CHECK(diagnostics.communication.last_success_ms == 100U);
    HOST_TEST_CHECK(diagnostics.communication.last_failure_ms == 0U);
    HOST_TEST_CHECK(diagnostics.communication.last_error == AS5600_DEVICE_OK);
    HOST_TEST_CHECK(diagnostics.calibration_configured);
    HOST_TEST_CHECK(diagnostics.calibration_metadata.format_version ==
                    AS5600_CALIBRATION_FORMAT_VERSION);
    HOST_TEST_CHECK(strcmp(diagnostics.calibration_metadata.calibration_id,
                           "test-identity-v1") == 0);
    HOST_TEST_CHECK(fixture.register_port.next_expectation == reads_before);
    HOST_TEST_CHECK(fixture.lock_acquires == lock_acquires_before + 1U);
    HOST_TEST_CHECK(fixture.lock_releases == lock_releases_before + 1U);
    HOST_TEST_CHECK(!fixture.lock_held);

    /* A read-only diagnostics query reports copied freshness but must not make
     * the old sample current or issue another transport request. */
    fixture.now_ms = 201U;
    HOST_TEST_CHECK(as5600_device_get_diagnostics(&fixture.device,
                                                   &diagnostics));
    HOST_TEST_CHECK(diagnostics.snapshot.stale);
    HOST_TEST_CHECK(!diagnostics.snapshot.online);
    HOST_TEST_CHECK(diagnostics.snapshot.health == AS5600_DEVICE_HEALTH_STALE);
    HOST_TEST_CHECK(fixture.register_port.next_expectation == reads_before);
    HOST_TEST_CHECK(fixture.lock_acquires == lock_acquires_before + 2U);
    HOST_TEST_CHECK(fixture.lock_releases == lock_releases_before + 2U);
    fixture.lock_available = false;
    HOST_TEST_CHECK(!as5600_device_get_diagnostics(&fixture.device,
                                                    &diagnostics));
    HOST_TEST_CHECK(fixture.register_port.next_expectation == reads_before);
    HOST_TEST_CHECK(fixture.lock_acquires == lock_acquires_before + 3U);
    HOST_TEST_CHECK(fixture.lock_releases == lock_releases_before + 2U);
    HOST_TEST_CHECK(!as5600_device_get_diagnostics(NULL, &diagnostics));
    HOST_TEST_CHECK(!as5600_device_get_diagnostics(&fixture.device, NULL));
    return true;
}

static bool test_diagnostics_are_one_shot_and_do_not_refresh_raw_time(void)
{
    as5600_fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture,
                                 true,
                                 FIXTURE_STALE_TIMEOUT_MS,
                                 NULL));

    fixture.now_ms = 100U;
    HOST_TEST_CHECK(queue_sample(&fixture,
                                 AS5600_STATUS_MAGNET_DETECTED,
                                 0x0100U,
                                 true,
                                 0x42U,
                                 0x0BEEU));
    HOST_TEST_CHECK(as5600_device_poll(&fixture.device) == AS5600_DEVICE_OK);

    as5600_device_snapshot_t snapshot;
    HOST_TEST_CHECK(as5600_device_get_snapshot(&fixture.device, &snapshot));
    HOST_TEST_CHECK(snapshot.diagnostics_attempted);
    HOST_TEST_CHECK(snapshot.diagnostics_valid);
    HOST_TEST_CHECK(snapshot.diagnostics_timestamp_ms == 100U);
    HOST_TEST_CHECK(snapshot.diagnostics_last_result == AS5600_DEVICE_OK);
    HOST_TEST_CHECK(snapshot.magnitude == 0x0BEEU);

    /* The next control-rate poll is a single contiguous primary transaction:
     * diagnostics cannot consume every 40 ms slot or rejuvenate raw feedback. */
    fixture.now_ms = 140U;
    HOST_TEST_CHECK(queue_sample(&fixture,
                                 AS5600_STATUS_MAGNET_DETECTED,
                                 0x0101U,
                                 false,
                                 0U,
                                 0U));
    HOST_TEST_CHECK(as5600_device_poll(&fixture.device) == AS5600_DEVICE_OK);
    HOST_TEST_CHECK(fake_as5600_register_port_complete(&fixture.register_port));
    HOST_TEST_CHECK(as5600_device_get_snapshot(&fixture.device, &snapshot));
    HOST_TEST_CHECK(snapshot.raw_angle == 0x0101U);
    HOST_TEST_CHECK(snapshot.sample_timestamp_ms == 140U);
    HOST_TEST_CHECK(snapshot.diagnostics_timestamp_ms == 100U);
    HOST_TEST_CHECK(snapshot.diagnostics_valid);
    return true;
}

static bool test_magnet_status_degrades_without_erasing_position(void)
{
    as5600_fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture,
                                 false,
                                 FIXTURE_STALE_TIMEOUT_MS,
                                 NULL));

    fixture.now_ms = 10U;
    HOST_TEST_CHECK(queue_sample(&fixture,
                                 AS5600_STATUS_MAGNET_DETECTED |
                                     AS5600_STATUS_MAGNET_TOO_WEAK,
                                 0x0200U,
                                 false,
                                 0U,
                                 0U));
    HOST_TEST_CHECK(as5600_device_poll(&fixture.device) == AS5600_DEVICE_OK);
    as5600_device_snapshot_t raw;
    as5600_position_snapshot_t position;
    HOST_TEST_CHECK(as5600_device_get_snapshot(&fixture.device, &raw));
    HOST_TEST_CHECK(as5600_device_get_position_snapshot(&fixture.device, &position));
    HOST_TEST_CHECK(raw.magnet_too_weak);
    HOST_TEST_CHECK(raw.health == AS5600_DEVICE_HEALTH_DEGRADED);
    HOST_TEST_CHECK(position.valid);
    HOST_TEST_CHECK(position.health == AS5600_DEVICE_HEALTH_DEGRADED);

    fixture.now_ms = 11U;
    HOST_TEST_CHECK(queue_sample(&fixture,
                                 AS5600_STATUS_MAGNET_DETECTED |
                                     AS5600_STATUS_MAGNET_TOO_STRONG,
                                 0x0201U,
                                 false,
                                 0U,
                                 0U));
    HOST_TEST_CHECK(as5600_device_poll(&fixture.device) == AS5600_DEVICE_OK);
    HOST_TEST_CHECK(as5600_device_get_snapshot(&fixture.device, &raw));
    HOST_TEST_CHECK(raw.magnet_too_strong);
    HOST_TEST_CHECK(raw.health == AS5600_DEVICE_HEALTH_DEGRADED);

    fixture.now_ms = 12U;
    HOST_TEST_CHECK(queue_sample(&fixture, 0U, 0x0202U, false, 0U, 0U));
    HOST_TEST_CHECK(as5600_device_poll(&fixture.device) == AS5600_DEVICE_OK);
    HOST_TEST_CHECK(as5600_device_get_position_snapshot(&fixture.device, &position));
    HOST_TEST_CHECK(!position.valid);
    HOST_TEST_CHECK(position.health == AS5600_DEVICE_HEALTH_DEGRADED);
    HOST_TEST_CHECK(fake_as5600_register_port_complete(&fixture.register_port));
    return true;
}

static bool test_calibration_interpolation_and_cyclic_wrap(void)
{
    as5600_calibration_lut_t interpolation_lut = identity_lut();
    interpolation_lut.correction_centidegrees[1] = 32;
    interpolation_lut.correction_centidegrees[127] = 32;
    HOST_TEST_CHECK(as5600_calibration_lut_validate(&interpolation_lut));
    HOST_TEST_CHECK(nearly_equal(
        as5600_calibration_correction_degrees(&interpolation_lut, 16U),
        0.16f,
        0.0001f));
    HOST_TEST_CHECK(nearly_equal(
        as5600_calibration_correction_degrees(&interpolation_lut, 4095U),
        0.01f,
        0.0001f));

    as5600_calibration_lut_t wrap_lut = identity_lut();
    for (size_t index = 0U; index < AS5600_CALIBRATION_LUT_NODE_COUNT; ++index) {
        wrap_lut.correction_centidegrees[index] = 100;
    }
    HOST_TEST_CHECK(as5600_calibration_lut_validate(&wrap_lut));
    HOST_TEST_CHECK(nearly_equal(
        as5600_calibration_corrected_degrees(&wrap_lut, 0U), 1.0f, 0.0001f));
    HOST_TEST_CHECK(as5600_calibration_corrected_degrees(&wrap_lut, 4095U) <
                    1.0f);
    return true;
}

static bool test_calibration_rejects_nonmonotonic_or_undocumented_lut(void)
{
    as5600_calibration_lut_t lut = identity_lut();
    lut.correction_centidegrees[1] = -1000;
    HOST_TEST_CHECK(!as5600_calibration_lut_validate(&lut));

    lut = identity_lut();
    lut.metadata.provenance = "";
    HOST_TEST_CHECK(!as5600_calibration_lut_validate(&lut));

    as5600_fixture_t fixture;
    HOST_TEST_CHECK(!fixture_init(&fixture,
                                  false,
                                  FIXTURE_STALE_TIMEOUT_MS,
                                  &lut));
    return true;
}

static bool test_raw_angle_wrap_is_single_turn(void)
{
    as5600_fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture,
                                 false,
                                 FIXTURE_STALE_TIMEOUT_MS,
                                 NULL));
    fixture.now_ms = 1U;
    HOST_TEST_CHECK(queue_sample(&fixture,
                                 AS5600_STATUS_MAGNET_DETECTED,
                                 0x0FFFU,
                                 false,
                                 0U,
                                 0U));
    HOST_TEST_CHECK(as5600_device_poll(&fixture.device) == AS5600_DEVICE_OK);
    as5600_position_snapshot_t position;
    HOST_TEST_CHECK(as5600_device_get_position_snapshot(&fixture.device, &position));
    HOST_TEST_CHECK(position.raw_angle == 0x0FFFU);
    HOST_TEST_CHECK(position.degrees > 359.9f);

    fixture.now_ms = 2U;
    HOST_TEST_CHECK(queue_sample(&fixture,
                                 AS5600_STATUS_MAGNET_DETECTED,
                                 0U,
                                 false,
                                 0U,
                                 0U));
    HOST_TEST_CHECK(as5600_device_poll(&fixture.device) == AS5600_DEVICE_OK);
    HOST_TEST_CHECK(as5600_device_get_position_snapshot(&fixture.device, &position));
    HOST_TEST_CHECK(position.raw_angle == 0U);
    HOST_TEST_CHECK(nearly_equal(position.degrees, 0.0f, 0.0001f));
    HOST_TEST_CHECK(fake_as5600_register_port_complete(&fixture.register_port));
    return true;
}

static bool test_clock_staleness_and_transport_offline(void)
{
    as5600_fixture_t stale_fixture;
    HOST_TEST_CHECK(fixture_init(&stale_fixture,
                                 false,
                                 FIXTURE_STALE_TIMEOUT_MS,
                                 NULL));
    stale_fixture.now_ms = 100U;
    HOST_TEST_CHECK(queue_sample(&stale_fixture,
                                 AS5600_STATUS_MAGNET_DETECTED,
                                 0x0100U,
                                 false,
                                 0U,
                                 0U));
    HOST_TEST_CHECK(as5600_device_poll(&stale_fixture.device) == AS5600_DEVICE_OK);
    stale_fixture.now_ms = 201U;
    as5600_device_snapshot_t raw;
    as5600_position_snapshot_t position;
    HOST_TEST_CHECK(as5600_device_get_snapshot(&stale_fixture.device, &raw));
    HOST_TEST_CHECK(raw.stale);
    HOST_TEST_CHECK(!raw.online);
    HOST_TEST_CHECK(raw.health == AS5600_DEVICE_HEALTH_STALE);
    HOST_TEST_CHECK(
        as5600_device_get_position_snapshot(&stale_fixture.device, &position));
    HOST_TEST_CHECK(!position.valid);
    HOST_TEST_CHECK(position.health == AS5600_DEVICE_HEALTH_STALE);

    as5600_fixture_t offline_fixture;
    HOST_TEST_CHECK(fixture_init(&offline_fixture,
                                 false,
                                 FIXTURE_STALE_TIMEOUT_MS,
                                 NULL));
    offline_fixture.now_ms = 100U;
    HOST_TEST_CHECK(queue_sample(&offline_fixture,
                                 AS5600_STATUS_MAGNET_DETECTED,
                                 0x0100U,
                                 false,
                                 0U,
                                 0U));
    HOST_TEST_CHECK(as5600_device_poll(&offline_fixture.device) == AS5600_DEVICE_OK);
    offline_fixture.now_ms = 150U;
    HOST_TEST_CHECK(queue_status_error(&offline_fixture, AS5600_DEVICE_TIMEOUT));
    HOST_TEST_CHECK(as5600_device_poll(&offline_fixture.device) ==
                    AS5600_DEVICE_TIMEOUT);
    HOST_TEST_CHECK(as5600_device_get_snapshot(&offline_fixture.device, &raw));
    HOST_TEST_CHECK(raw.raw_angle_valid);
    HOST_TEST_CHECK(raw.online);
    HOST_TEST_CHECK(!raw.stale);
    HOST_TEST_CHECK(raw.last_error == AS5600_DEVICE_TIMEOUT);
    HOST_TEST_CHECK(raw.health == AS5600_DEVICE_HEALTH_DEGRADED);
    offline_fixture.now_ms = 201U;
    HOST_TEST_CHECK(as5600_device_get_snapshot(&offline_fixture.device, &raw));
    HOST_TEST_CHECK(raw.stale);
    HOST_TEST_CHECK(!raw.online);
    HOST_TEST_CHECK(raw.health == AS5600_DEVICE_HEALTH_OFFLINE);
    HOST_TEST_CHECK(fake_as5600_register_port_complete(&stale_fixture.register_port));
    HOST_TEST_CHECK(fake_as5600_register_port_complete(&offline_fixture.register_port));
    return true;
}

static bool test_optional_diagnostics_failure_is_degraded_not_position_loss(void)
{
    as5600_fixture_t fixture;
    HOST_TEST_CHECK(fixture_init(&fixture,
                                 true,
                                 FIXTURE_STALE_TIMEOUT_MS,
                                 NULL));
    fixture.now_ms = 100U;
    const uint8_t primary[] = {
        AS5600_STATUS_MAGNET_DETECTED, 0x03U, 0x21U};
    HOST_TEST_CHECK(fake_as5600_register_port_expect_read(&fixture.register_port,
                                                           AS5600_DEFAULT_I2C_ADDRESS,
                                                           0x0BU,
                                                           sizeof(primary),
                                                           FIXTURE_RESPONSE_TIMEOUT_MS,
                                                           AS5600_DEVICE_OK,
                                                           primary));
    HOST_TEST_CHECK(fake_as5600_register_port_expect_read(&fixture.register_port,
                                                           AS5600_DEFAULT_I2C_ADDRESS,
                                                           0x1AU,
                                                           3U,
                                                           FIXTURE_RESPONSE_TIMEOUT_MS,
                                                           AS5600_DEVICE_TIMEOUT,
                                                           NULL));
    HOST_TEST_CHECK(as5600_device_poll(&fixture.device) == AS5600_DEVICE_PARTIAL);

    as5600_device_snapshot_t raw;
    as5600_position_snapshot_t position;
    HOST_TEST_CHECK(as5600_device_get_snapshot(&fixture.device, &raw));
    HOST_TEST_CHECK(as5600_device_get_position_snapshot(&fixture.device, &position));
    HOST_TEST_CHECK(raw.raw_angle_valid);
    HOST_TEST_CHECK(raw.diagnostics_attempted);
    HOST_TEST_CHECK(!raw.diagnostics_valid);
    HOST_TEST_CHECK(raw.diagnostics_last_result == AS5600_DEVICE_TIMEOUT);
    HOST_TEST_CHECK(raw.last_poll_result == AS5600_DEVICE_PARTIAL);
    HOST_TEST_CHECK(raw.health == AS5600_DEVICE_HEALTH_DEGRADED);
    HOST_TEST_CHECK(position.valid);
    HOST_TEST_CHECK(position.health == AS5600_DEVICE_HEALTH_DEGRADED);
    HOST_TEST_CHECK(fake_as5600_register_port_complete(&fixture.register_port));
    return true;
}

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(test_raw_conversion_diagnostics_and_healthy_position),
        HOST_TEST_CASE(test_cached_diagnostics_never_poll_or_rejuvenate_state),
        HOST_TEST_CASE(test_diagnostics_are_one_shot_and_do_not_refresh_raw_time),
        HOST_TEST_CASE(test_magnet_status_degrades_without_erasing_position),
        HOST_TEST_CASE(test_calibration_interpolation_and_cyclic_wrap),
        HOST_TEST_CASE(test_calibration_rejects_nonmonotonic_or_undocumented_lut),
        HOST_TEST_CASE(test_raw_angle_wrap_is_single_turn),
        HOST_TEST_CASE(test_clock_staleness_and_transport_offline),
        HOST_TEST_CASE(test_optional_diagnostics_failure_is_degraded_not_position_loss),
    };
    return host_test_exit_code(host_test_run_cases(cases,
                                                   HOST_TEST_ARRAY_COUNT(cases),
                                                   stdout));
}
