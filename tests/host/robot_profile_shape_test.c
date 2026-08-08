#include "host_test.h"

#include <string.h>

#include "robot_profile.h"

static size_t count_bus_type(const robot_profile_t *profile,
                             robot_bus_type_t type)
{
    size_t count = 0U;
    for (size_t index = 0; index < profile->bus_count; ++index) {
        if (profile->buses[index].type == type) {
            count++;
        }
    }
    return count;
}

static bool common_profile_contract(void)
{
    const robot_profile_t *profile = robot_profile_selected();
    HOST_TEST_CHECK(profile != NULL);
    HOST_TEST_CHECK(robot_profile_validate(profile) == ROBOT_PROFILE_VALID);
    HOST_TEST_CHECK(count_bus_type(profile, ROBOT_BUS_UART_RS485) == 1U);
    HOST_TEST_CHECK(count_bus_type(profile, ROBOT_BUS_GPIO) == 1U);
    HOST_TEST_CHECK(profile->buses[0].type == ROBOT_BUS_UART_RS485);
    HOST_TEST_CHECK(profile->buses[0].id == 1U);
    HOST_TEST_CHECK(profile->buses[0].rate == 115200U);
    for (size_t index = 0; index < profile->endpoint_count; ++index) {
        const robot_endpoint_profile_t *endpoint = &profile->endpoints[index];
        HOST_TEST_CHECK(endpoint->id == index + 1U);
        HOST_TEST_CHECK(endpoint->capabilities ==
                        (ROBOT_CAPABILITY_VELOCITY_RPM |
                         ROBOT_CAPABILITY_STOPPABLE));
        HOST_TEST_CHECK(endpoint->min_rpm == -15);
        HOST_TEST_CHECK(endpoint->max_rpm == 15);
    }
    return true;
}

#ifdef BOTFARMS_EXPECT_BENCH_PROFILE
static bool selected_profile_shape(void)
{
    const robot_profile_t *profile = robot_profile_selected();
    HOST_TEST_CHECK(strcmp(profile->name, "bench_single_svd48_motor") == 0);
    HOST_TEST_CHECK(profile->device_count == 1U);
    HOST_TEST_CHECK(profile->devices[0].driver_id == ROBOT_DRIVER_SVD48);
    HOST_TEST_CHECK(profile->devices[0].bus_id == 1U);
    HOST_TEST_CHECK(profile->devices[0].address == 1U);
    HOST_TEST_CHECK(profile->devices[0].channel_count == 2U);
    HOST_TEST_CHECK(profile->endpoint_count == 1U);
    HOST_TEST_CHECK(profile->endpoints[0].device_id == 1U);
    HOST_TEST_CHECK(profile->endpoints[0].channel == 0U);
    HOST_TEST_CHECK(profile->application.kind == ROBOT_PROFILE_NO_GEOMETRY);
    /* The endpoint array is the explicit legacy index mapping. */
    HOST_TEST_CHECK(profile->endpoint_count > 0U);
    HOST_TEST_CHECK(profile->endpoint_count <= 1U);
    return true;
}
#else
static bool selected_profile_shape(void)
{
    const robot_profile_t *profile = robot_profile_selected();
    HOST_TEST_CHECK(strcmp(profile->name, "current_robot") == 0);
    HOST_TEST_CHECK(profile->device_count == 2U);
    for (size_t index = 0; index < profile->device_count; ++index) {
        HOST_TEST_CHECK(profile->devices[index].driver_id == ROBOT_DRIVER_SVD48);
        HOST_TEST_CHECK(profile->devices[index].bus_id == 1U);
        HOST_TEST_CHECK(profile->devices[index].address == index + 1U);
        HOST_TEST_CHECK(profile->devices[index].channel_count == 2U);
    }
    HOST_TEST_CHECK(profile->endpoint_count == 4U);
    for (size_t index = 0; index < profile->endpoint_count; ++index) {
        HOST_TEST_CHECK(profile->endpoints[index].device_id == index / 2U + 1U);
        HOST_TEST_CHECK(profile->endpoints[index].channel == index % 2U);
    }
    HOST_TEST_CHECK(profile->application.kind ==
                    ROBOT_PROFILE_DIFFERENTIAL_GEOMETRY);
    HOST_TEST_CHECK(profile->application.wheelbase_m > 0.0f);
    HOST_TEST_CHECK(profile->application.track_width_m > 0.0f);
    HOST_TEST_CHECK(profile->application.wheel_radius_m > 0.0f);
    return true;
}
#endif

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(common_profile_contract),
        HOST_TEST_CASE(selected_profile_shape),
    };
    return host_test_exit_code(
        host_test_run_cases(cases, HOST_TEST_ARRAY_COUNT(cases), stdout));
}
