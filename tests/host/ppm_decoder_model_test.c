#include <limits.h>
#include <stdint.h>

#include "host_test.h"
#include "ppm_decoder_model.h"

static ppm_decoder_model_config_t tested_config(void)
{
    return (ppm_decoder_model_config_t){
        .channel_count = 8U,
        .min_frame_channels = 8U,
        .sync_threshold_us = 3000U,
        .min_pulse_us = 750U,
        .max_pulse_us = 2250U,
    };
}

static bool feed_frame(ppm_decoder_model_t *model,
                       uint32_t *now_us,
                       const uint16_t *pulses,
                       uint8_t pulse_count)
{
    for (uint8_t index = 0U; index < pulse_count; ++index) {
        *now_us += pulses[index];
        (void)ppm_decoder_model_feed_rising_edge(model, *now_us);
    }
    *now_us += 4000U;
    return ppm_decoder_model_feed_rising_edge(model, *now_us);
}

static bool test_rejects_invalid_configuration(void)
{
    ppm_decoder_model_t model;
    ppm_decoder_model_config_t config = tested_config();
    HOST_TEST_CHECK(!ppm_decoder_model_init(NULL, &config));
    HOST_TEST_CHECK(!ppm_decoder_model_init(&model, NULL));
    config.channel_count = 0U;
    HOST_TEST_CHECK(!ppm_decoder_model_init(&model, &config));
    config = tested_config();
    config.min_frame_channels = 7U;
    HOST_TEST_CHECK(!ppm_decoder_model_init(&model, &config));
    config = tested_config();
    config.sync_threshold_us = config.max_pulse_us;
    HOST_TEST_CHECK(!ppm_decoder_model_init(&model, &config));
    return true;
}

static bool test_accepts_exact_eight_channel_frame_atomically(void)
{
    ppm_decoder_model_t model;
    const ppm_decoder_model_config_t config = tested_config();
    HOST_TEST_CHECK(ppm_decoder_model_init(&model, &config));
    uint32_t now_us = 1000U;
    (void)ppm_decoder_model_feed_rising_edge(&model, now_us);
    now_us += 4000U;
    (void)ppm_decoder_model_feed_rising_edge(&model, now_us);
    const uint16_t expected[8] = {1000U, 1100U, 1200U, 1300U,
                                  1400U, 1500U, 1600U, 1700U};
    HOST_TEST_CHECK(feed_frame(&model, &now_us, expected, 8U));
    ppm_decoder_model_status_t status;
    HOST_TEST_CHECK(ppm_decoder_model_snapshot(&model, now_us + 250000U, &status));
    HOST_TEST_CHECK(status.last_frame_age_us == 250000U);
    HOST_TEST_CHECK(status.valid_frames == 1U);
    HOST_TEST_CHECK(status.channel_count == 8U);
    for (uint8_t index = 0U; index < 8U; ++index) {
        HOST_TEST_CHECK(status.channels[index] == expected[index]);
    }
    return true;
}

static bool test_rejects_short_and_extra_frames_preserving_last_good(void)
{
    ppm_decoder_model_t model;
    const ppm_decoder_model_config_t config = tested_config();
    HOST_TEST_CHECK(ppm_decoder_model_init(&model, &config));
    uint32_t now_us = 500U;
    (void)ppm_decoder_model_feed_rising_edge(&model, now_us);
    now_us += 4000U;
    (void)ppm_decoder_model_feed_rising_edge(&model, now_us);
    const uint16_t good[8] = {1000U, 1100U, 1200U, 1300U,
                              1400U, 2000U, 1600U, 1700U};
    const uint16_t seven[7] = {1500U, 1500U, 1500U, 1500U,
                               1500U, 1500U, 1500U};
    const uint16_t nine[9] = {1500U, 1500U, 1500U, 1500U, 1500U,
                              1500U, 1500U, 1500U, 1500U};
    const uint16_t ten[10] = {1500U, 1500U, 1500U, 1500U, 1500U,
                               1500U, 1500U, 1500U, 1500U, 1500U};
    HOST_TEST_CHECK(feed_frame(&model, &now_us, good, 8U));
    HOST_TEST_CHECK(!feed_frame(&model, &now_us, seven, 7U));
    HOST_TEST_CHECK(!feed_frame(&model, &now_us, nine, 9U));
    HOST_TEST_CHECK(!feed_frame(&model, &now_us, ten, 10U));

    ppm_decoder_model_status_t status;
    HOST_TEST_CHECK(ppm_decoder_model_snapshot(&model, now_us, &status));
    HOST_TEST_CHECK(status.valid_frames == 1U);
    HOST_TEST_CHECK(status.rejected_frames == 3U);
    HOST_TEST_CHECK(status.incomplete_frames == 3U);
    HOST_TEST_CHECK(status.overflow_pulses == 3U);
    HOST_TEST_CHECK(status.channel_count == 8U);
    /* The rejected frame never substitutes the LKG CH5 (physical channel 5). */
    HOST_TEST_CHECK(status.channels[4] == 1400U);
    HOST_TEST_CHECK(status.channels[5] == 2000U);
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
    const uint16_t pulses[8] = {1500U, 1500U, 1500U, 1500U,
                                1500U, 1500U, 1500U, 1500U};
    HOST_TEST_CHECK(feed_frame(&model, &now_us, pulses, 8U));
    ppm_decoder_model_status_t status;
    HOST_TEST_CHECK(ppm_decoder_model_snapshot(&model, now_us + 1234U, &status));
    HOST_TEST_CHECK(status.last_frame_age_us == 1234U);
    return true;
}

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(test_rejects_invalid_configuration),
        HOST_TEST_CASE(test_accepts_exact_eight_channel_frame_atomically),
        HOST_TEST_CASE(test_rejects_short_and_extra_frames_preserving_last_good),
        HOST_TEST_CASE(test_timestamp_wraparound),
    };
    const host_test_summary_t summary =
        host_test_run_cases(cases, HOST_TEST_ARRAY_COUNT(cases), stdout);
    return host_test_exit_code(summary);
}
