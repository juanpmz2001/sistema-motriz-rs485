#include "host_test.h"

#include "control_lan_sequence_model.h"

static bool test_monotonic_gap_is_accepted_and_counted_without_a_stop(void)
{
    HOST_TEST_CHECK(control_lan_sequence_is_increasing(100U, 101U));
    HOST_TEST_CHECK(control_lan_sequence_gap_count(100U, 101U) == 0U);
    /* 103 is a valid next command. Its missing 102 is link-quality evidence,
     * not a synthetic STOP or a TTL extension. */
    HOST_TEST_CHECK(control_lan_sequence_is_increasing(101U, 103U));
    HOST_TEST_CHECK(control_lan_sequence_gap_count(101U, 103U) == 1U);
    return true;
}

static bool test_duplicate_and_out_of_order_sequences_are_rejected(void)
{
    HOST_TEST_CHECK(!control_lan_sequence_is_increasing(103U, 103U));
    HOST_TEST_CHECK(!control_lan_sequence_is_increasing(103U, 102U));
    HOST_TEST_CHECK(control_lan_sequence_gap_count(103U, 103U) == 0U);
    return true;
}

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(test_monotonic_gap_is_accepted_and_counted_without_a_stop),
        HOST_TEST_CASE(test_duplicate_and_out_of_order_sequences_are_rejected),
    };
    return host_test_exit_code(
        host_test_run_cases(cases, HOST_TEST_ARRAY_COUNT(cases), stdout));
}
