#include <limits.h>
#include <stdint.h>

#include "host_test.h"
#include "ppm_decoder_model.h"

static ppm_decoder_model_config_t tested_config(void)
{
    const ppm_decoder_model_config_t config = {
        .channel_count = 10,
        .min_frame_channels = 4,
        .sync_threshold_us = 3000,
        .min_pulse_us = 750,
        .max_pulse_us = 2250,
    };
    return config;
}

static bool test_rejects_invalid_configuration(void)
{
    ppm_decoder_model_t model;
    ppm_decoder_model_config_t config = tested_config();
    HOST_TEST_CHECK(!ppm_decoder_model_init(NULL, &config));
    HOST_TEST_CHECK(!ppm_decoder_model_init(&model, NULL));

    config.channel_count = 0;
    HOST_TEST_CHECK(!ppm_decoder_model_init(&model, &config));
    config = tested_config();
    config.min_frame_channels = 11;
    HOST_TEST_CHECK(!ppm_decoder_model_init(&model, &config));
    config = tested_config();
    config.sync_threshold_us = config.max_pulse_us;
    HOST_TEST_CHECK(!ppm_decoder_model_init(&model, &config));
    return true;
}

static bool test_publishes_complete_frame_atomically(void)
{
    ppm_decoder_model_t model;
    const ppm_decoder_model_config_t config = tested_config();
    HOST_TEST_CHECK(ppm_decoder_model_init(&model, &config));

    uint32_t now_us = 1000;
    HOST_TEST_CHECK(!ppm_decoder_model_feed_rising_edge(&model, now_us));
    now_us += 4000;
    HOST_TEST_CHECK(!ppm_decoder_model_feed_rising_edge(&model, now_us));

    const uint16_t expected[10] = {
        1000, 1100, 1200, 1300, 1400, 1500, 1600, 1700, 1800, 1900,
    };
    for (uint8_t i = 0; i < 10; i++) {
        now_us += expected[i];
        HOST_TEST_CHECK(!ppm_decoder_model_feed_rising_edge(&model, now_us));
    }

    ppm_decoder_model_status_t before_sync;
    HOST_TEST_CHECK(ppm_decoder_model_snapshot(&model, now_us, &before_sync));
    HOST_TEST_CHECK(before_sync.valid_frames == 0);
    HOST_TEST_CHECK(before_sync.channel_count == 0);

    now_us += 4000;
    HOST_TEST_CHECK(ppm_decoder_model_feed_rising_edge(&model, now_us));

    ppm_decoder_model_status_t status;
    HOST_TEST_CHECK(ppm_decoder_model_snapshot(&model, now_us + 250000, &status));
    HOST_TEST_CHECK(status.last_frame_age_us == 250000);
    HOST_TEST_CHECK(status.sync_gaps == 2);
    HOST_TEST_CHECK(status.valid_frames == 1);
    HOST_TEST_CHECK(status.channel_count == 10);
    for (uint8_t i = 0; i < 10; i++) {
        HOST_TEST_CHECK(status.channels[i] == expected[i]);
    }
    return true;
}

static bool test_counts_invalid_incomplete_and_overflow_input(void)
{
    ppm_decoder_model_t model;
    const ppm_decoder_model_config_t config = tested_config();
    HOST_TEST_CHECK(ppm_decoder_model_init(&model, &config));

    uint32_t now_us = 500;
    (void)ppm_decoder_model_feed_rising_edge(&model, now_us);
    now_us += 4000;
    (void)ppm_decoder_model_feed_rising_edge(&model, now_us);
    now_us += 500;
    (void)ppm_decoder_model_feed_rising_edge(&model, now_us);
    for (uint8_t i = 0; i < 3; i++) {
        now_us += 1500;
        (void)ppm_decoder_model_feed_rising_edge(&model, now_us);
    }
    now_us += 4000;
    HOST_TEST_CHECK(!ppm_decoder_model_feed_rising_edge(&model, now_us));

    for (uint8_t i = 0; i < 12; i++) {
        now_us += 1500;
        (void)ppm_decoder_model_feed_rising_edge(&model, now_us);
    }
    now_us += 4000;
    HOST_TEST_CHECK(ppm_decoder_model_feed_rising_edge(&model, now_us));

    ppm_decoder_model_status_t status;
    HOST_TEST_CHECK(ppm_decoder_model_snapshot(&model, now_us, &status));
    HOST_TEST_CHECK(status.invalid_pulses == 1);
    HOST_TEST_CHECK(status.incomplete_frames == 1);
    HOST_TEST_CHECK(status.overflow_pulses == 2);
    HOST_TEST_CHECK(status.valid_frames == 1);
    HOST_TEST_CHECK(status.channel_count == 10);
    return true;
}

static bool test_timestamp_wraparound(void)
{
    ppm_decoder_model_t model;
    const ppm_decoder_model_config_t config = tested_config();
    HOST_TEST_CHECK(ppm_decoder_model_init(&model, &config));

    uint32_t now_us = UINT32_MAX - 1000U;
    (void)ppm_decoder_model_feed_rising_edge(&model, now_us);
    now_us += 4000U;
    (void)ppm_decoder_model_feed_rising_edge(&model, now_us);
    for (uint8_t i = 0; i < 4; i++) {
        now_us += 1500U;
        (void)ppm_decoder_model_feed_rising_edge(&model, now_us);
    }
    now_us += 4000U;
    HOST_TEST_CHECK(ppm_decoder_model_feed_rising_edge(&model, now_us));

    ppm_decoder_model_status_t status;
    HOST_TEST_CHECK(ppm_decoder_model_snapshot(&model, now_us + 1234U, &status));
    HOST_TEST_CHECK(status.last_frame_age_us == 1234U);
    HOST_TEST_CHECK(status.valid_frames == 1);
    return true;
}

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(test_rejects_invalid_configuration),
        HOST_TEST_CASE(test_publishes_complete_frame_atomically),
        HOST_TEST_CASE(test_counts_invalid_incomplete_and_overflow_input),
        HOST_TEST_CASE(test_timestamp_wraparound),
    };
    const host_test_summary_t summary =
        host_test_run_cases(cases, HOST_TEST_ARRAY_COUNT(cases), stdout);
    return host_test_exit_code(summary);
}
