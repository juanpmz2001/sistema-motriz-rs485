#include "fake_clock.h"
#include "fake_event_sink.h"
#include "host_test.h"

#include <stdint.h>
#include <stdio.h>

static bool always_passes(void)
{
    return true;
}

static bool always_fails(void)
{
    return false;
}

static bool test_runner_reports_results_deterministically(void)
{
    const host_test_case_t nested_cases[] = {
        HOST_TEST_CASE(always_passes),
        HOST_TEST_CASE(always_fails),
        { "missing_function", NULL },
    };

    host_test_summary_t first =
        host_test_run_cases(nested_cases, HOST_TEST_ARRAY_COUNT(nested_cases), NULL);
    host_test_summary_t second =
        host_test_run_cases(nested_cases, HOST_TEST_ARRAY_COUNT(nested_cases), NULL);

    HOST_TEST_CHECK(first.total == 3U);
    HOST_TEST_CHECK(first.passed == 1U);
    HOST_TEST_CHECK(first.failed == 2U);
    HOST_TEST_CHECK(first.total == second.total);
    HOST_TEST_CHECK(first.passed == second.passed);
    HOST_TEST_CHECK(first.failed == second.failed);
    HOST_TEST_CHECK(host_test_exit_code(first) != 0);

    host_test_summary_t empty = host_test_run_cases(NULL, 0U, NULL);
    HOST_TEST_CHECK(empty.total == 0U);
    HOST_TEST_CHECK(empty.passed == 0U);
    HOST_TEST_CHECK(empty.failed == 0U);
    HOST_TEST_CHECK(host_test_exit_code(empty) == 0);
    return true;
}

static bool test_fake_clock_is_explicit_and_overflow_safe(void)
{
    fake_clock_t clock;
    fake_clock_init(&clock, 125U);

    HOST_TEST_CHECK(fake_clock_now_ms(&clock) == 125U);
    HOST_TEST_CHECK(fake_clock_advance_ms(&clock, 75U));
    HOST_TEST_CHECK(fake_clock_now_ms(&clock) == 200U);

    fake_clock_set_ms(&clock, UINT64_MAX - 2U);
    HOST_TEST_CHECK(!fake_clock_advance_ms(&clock, 3U));
    HOST_TEST_CHECK(fake_clock_now_ms(&clock) == UINT64_MAX - 2U);

    fake_clock_reset(&clock);
    HOST_TEST_CHECK(fake_clock_now_ms(&clock) == 0U);
    HOST_TEST_CHECK(fake_clock_now_ms(NULL) == 0U);
    HOST_TEST_CHECK(!fake_clock_advance_ms(NULL, 1U));
    return true;
}

static bool test_fake_event_sink_preserves_order_and_resets(void)
{
    fake_event_sink_t sink;
    fake_event_sink_reset(&sink);

    HOST_TEST_CHECK(fake_event_sink_capture(&sink, 10U, 1, -25, 100U));
    HOST_TEST_CHECK(fake_event_sink_capture(&sink, 20U, 2, 50, 125U));
    HOST_TEST_CHECK(fake_event_sink_count(&sink) == 2U);
    HOST_TEST_CHECK(fake_event_sink_dropped_count(&sink) == 0U);

    const fake_event_record_t *first = fake_event_sink_get(&sink, 0U);
    const fake_event_record_t *second = fake_event_sink_get(&sink, 1U);
    HOST_TEST_CHECK(first != NULL);
    HOST_TEST_CHECK(second != NULL);
    HOST_TEST_CHECK(first->event_id == 10U);
    HOST_TEST_CHECK(first->subject == 1);
    HOST_TEST_CHECK(first->value == -25);
    HOST_TEST_CHECK(first->timestamp_ms == 100U);
    HOST_TEST_CHECK(second->event_id == 20U);
    HOST_TEST_CHECK(second->timestamp_ms == 125U);
    HOST_TEST_CHECK(fake_event_sink_get(&sink, 2U) == NULL);

    fake_event_sink_reset(&sink);
    HOST_TEST_CHECK(fake_event_sink_count(&sink) == 0U);
    HOST_TEST_CHECK(fake_event_sink_dropped_count(&sink) == 0U);
    HOST_TEST_CHECK(fake_event_sink_get(&sink, 0U) == NULL);
    return true;
}

static bool test_fake_event_sink_reports_capacity_exhaustion(void)
{
    fake_event_sink_t sink;
    fake_event_sink_reset(&sink);

    for (size_t index = 0U; index < FAKE_EVENT_SINK_CAPACITY; index++) {
        HOST_TEST_CHECK(fake_event_sink_capture(&sink,
                                                (uint32_t)index,
                                                0,
                                                (int64_t)index,
                                                (uint64_t)index));
    }

    HOST_TEST_CHECK(!fake_event_sink_capture(&sink, 999U, 0, 0, 999U));
    HOST_TEST_CHECK(fake_event_sink_count(&sink) == FAKE_EVENT_SINK_CAPACITY);
    HOST_TEST_CHECK(fake_event_sink_dropped_count(&sink) == 1U);
    HOST_TEST_CHECK(!fake_event_sink_capture(NULL, 1U, 0, 0, 0U));
    HOST_TEST_CHECK(fake_event_sink_count(NULL) == 0U);
    HOST_TEST_CHECK(fake_event_sink_dropped_count(NULL) == 0U);
    return true;
}

int main(void)
{
    const host_test_case_t cases[] = {
        HOST_TEST_CASE(test_runner_reports_results_deterministically),
        HOST_TEST_CASE(test_fake_clock_is_explicit_and_overflow_safe),
        HOST_TEST_CASE(test_fake_event_sink_preserves_order_and_resets),
        HOST_TEST_CASE(test_fake_event_sink_reports_capacity_exhaustion),
    };

    host_test_summary_t summary =
        host_test_run_cases(cases, HOST_TEST_ARRAY_COUNT(cases), stdout);
    return host_test_exit_code(summary);
}
