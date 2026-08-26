#include "host_test.h"
#include "ppm_motion_model.h"

static ppm_motion_model_config_t standard_config(void)
{
    return (ppm_motion_model_config_t){
        .throttle_channel = 2U,
        .steering_channel = 4U,
        .enable_channel = 5U,
        .enable_active_max_us = 1500U,
        .neutral_deadband_us = 30U,
        .valid_min_us = 750U,
        .valid_max_us = 2250U,
        .throttle_min_us = 1000U,
        .throttle_center_us = 1500U,
        .throttle_max_us = 2000U,
        .steering_min_us = 1000U,
        .steering_center_us = 1500U,
        .steering_max_us = 2000U,
        .throttle_sign = 1,
        .steering_sign = -1,
        .speed_scale_channel = 6U,
        .speed_scale_min_us = 1000U,
        .speed_scale_max_us = 2000U,
        .speed_scale_min = 0.50f,
        .speed_scale_max = 1.00f,
    };
}

static ppm_motion_input_t frame(uint32_t sequence,
                                uint16_t ch2,
                                uint16_t ch4,
                                uint16_t ch5,
                                uint16_t ch6)
{
    ppm_motion_input_t input = {
        .signal_valid = true,
        .valid_frame_sequence = sequence,
        .channel_count = 6U,
    };
    input.channels[1] = ch2;
    input.channels[3] = ch4;
    input.channels[4] = ch5;
    input.channels[5] = ch6;
    return input;
}

static bool arm_to_rc_source(ppm_motion_model_t *model,
                             ppm_motion_output_t *output)
{
    ppm_motion_input_t input = frame(1U, 1500U, 1500U, 1400U, 1500U);
    HOST_TEST_CHECK(ppm_motion_model_step(model, &input, false, output));
    HOST_TEST_CHECK(output->action == PPM_MOTION_ACTION_STOP);
    input = frame(2U, 1500U, 1500U, 1400U, 1500U);
    HOST_TEST_CHECK(ppm_motion_model_step(model, &input, false, output));
    HOST_TEST_CHECK(output->action == PPM_MOTION_ACTION_ARM);
    HOST_TEST_CHECK(output->stream_id != 0U);
    HOST_TEST_CHECK(output->sequence == 1U);
    HOST_TEST_CHECK(ppm_motion_model_step(model, &input, true, output));
    HOST_TEST_CHECK(output->action == PPM_MOTION_ACTION_NONE);
    HOST_TEST_CHECK(model->phase == PPM_MOTION_PHASE_ARMED);
    return true;
}

static bool test_priority_requires_stop_then_fresh_neutral_arm(void)
{
    ppm_motion_model_t model;
    ppm_motion_output_t output;
    const ppm_motion_model_config_t config = standard_config();
    HOST_TEST_CHECK(ppm_motion_model_init(&model, &config));
    return arm_to_rc_source(&model, &output);
}

static bool test_ch2_forward_and_ch4_right_are_bounded_rc_commands(void)
{
    ppm_motion_model_t model;
    ppm_motion_output_t output;
    const ppm_motion_model_config_t config = standard_config();
    HOST_TEST_CHECK(ppm_motion_model_init(&model, &config));
    HOST_TEST_CHECK(arm_to_rc_source(&model, &output));

    ppm_motion_input_t input = frame(3U, 2000U, 2000U, 1400U, 2000U);
    HOST_TEST_CHECK(ppm_motion_model_step(&model, &input, true, &output));
    HOST_TEST_CHECK(output.action == PPM_MOTION_ACTION_COMMAND);
    HOST_TEST_CHECK(output.deadman);
    HOST_TEST_CHECK(output.normalized_vx == 1.0f);
    /* Positive differential wz is left; operator CH4 high means right. */
    HOST_TEST_CHECK(output.normalized_wz == -1.0f);
    HOST_TEST_CHECK(output.speed_scale == 1.0f);

    input = frame(4U, 1000U, 1000U, 1400U, 1000U);
    HOST_TEST_CHECK(ppm_motion_model_step(&model, &input, true, &output));
    HOST_TEST_CHECK(output.normalized_vx == -1.0f);
    HOST_TEST_CHECK(output.normalized_wz == 1.0f);
    HOST_TEST_CHECK(output.speed_scale == 0.50f);
    return true;
}

static bool test_axis_calibration_is_independent_from_validity_envelope(void)
{
    ppm_motion_model_t model;
    ppm_motion_output_t output;
    const ppm_motion_model_config_t config = standard_config();
    HOST_TEST_CHECK(ppm_motion_model_init(&model, &config));
    HOST_TEST_CHECK(arm_to_rc_source(&model, &output));

    /* The accepted 750--2250us frame envelope must not stretch the calibrated
     * 1000--1500--2000us motion axis. Values inside the envelope clamp. */
    ppm_motion_input_t input = frame(3U, 2250U, 750U, 1400U, 750U);
    HOST_TEST_CHECK(ppm_motion_model_step(&model, &input, true, &output));
    HOST_TEST_CHECK(output.action == PPM_MOTION_ACTION_COMMAND);
    HOST_TEST_CHECK(output.normalized_vx == 1.0f);
    HOST_TEST_CHECK(output.normalized_wz == 1.0f);
    HOST_TEST_CHECK(output.speed_scale == 0.50f);

    input = frame(4U, 1500U, 1500U, 1400U, 1500U);
    HOST_TEST_CHECK(ppm_motion_model_step(&model, &input, true, &output));
    HOST_TEST_CHECK(output.normalized_vx == 0.0f);
    HOST_TEST_CHECK(output.normalized_wz == 0.0f);
    HOST_TEST_CHECK(output.speed_scale == 0.75f);

    /* A used channel outside the plausibility envelope invalidates the PPM
     * source and produces its terminal STOP; it is never treated as a clamp. */
    input = frame(5U, 749U, 1500U, 1400U, 1500U);
    HOST_TEST_CHECK(ppm_motion_model_step(&model, &input, true, &output));
    HOST_TEST_CHECK(output.action == PPM_MOTION_ACTION_STOP);
    HOST_TEST_CHECK(model.phase == PPM_MOTION_PHASE_INACTIVE);
    return true;
}

static bool test_signal_loss_stops_and_external_stop_requires_new_neutral(void)
{
    ppm_motion_model_t model;
    ppm_motion_output_t output;
    const ppm_motion_model_config_t config = standard_config();
    HOST_TEST_CHECK(ppm_motion_model_init(&model, &config));
    HOST_TEST_CHECK(arm_to_rc_source(&model, &output));
    ppm_motion_input_t input = frame(3U, 2000U, 1500U, 1400U, 1500U);
    HOST_TEST_CHECK(ppm_motion_model_step(&model, &input, true, &output));
    HOST_TEST_CHECK(output.action == PPM_MOTION_ACTION_COMMAND);

    /* A global STOP observed by the application cannot be bypassed by held
     * throttle. A fresh neutral frame is required before a new RC arm. */
    HOST_TEST_CHECK(ppm_motion_model_step(&model, &input, false, &output));
    HOST_TEST_CHECK(model.phase == PPM_MOTION_PHASE_WAIT_NEUTRAL);
    input = frame(4U, 2000U, 1500U, 1400U, 1500U);
    HOST_TEST_CHECK(ppm_motion_model_step(&model, &input, false, &output));
    HOST_TEST_CHECK(output.action == PPM_MOTION_ACTION_NONE);
    input = frame(5U, 1500U, 1500U, 1400U, 1500U);
    HOST_TEST_CHECK(ppm_motion_model_step(&model, &input, false, &output));
    HOST_TEST_CHECK(output.action == PPM_MOTION_ACTION_ARM);

    input = frame(6U, 1500U, 1500U, 2000U, 1500U);
    HOST_TEST_CHECK(ppm_motion_model_step(&model, &input, false, &output));
    HOST_TEST_CHECK(output.action == PPM_MOTION_ACTION_STOP);
    HOST_TEST_CHECK(model.phase == PPM_MOTION_PHASE_INACTIVE);
    return true;
}

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(test_priority_requires_stop_then_fresh_neutral_arm),
        HOST_TEST_CASE(test_ch2_forward_and_ch4_right_are_bounded_rc_commands),
        HOST_TEST_CASE(test_axis_calibration_is_independent_from_validity_envelope),
        HOST_TEST_CASE(test_signal_loss_stops_and_external_stop_requires_new_neutral),
    };
    return host_test_exit_code(
        host_test_run_cases(cases, HOST_TEST_ARRAY_COUNT(cases), stdout));
}
