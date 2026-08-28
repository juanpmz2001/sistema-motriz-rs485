#include "host_test.h"

#include "host_threads.h"
#include <stdatomic.h>
#include <string.h>

#include "bus_transport.h"
#include "fake_bus_transport.h"

static bool stats_are_zero(const bus_transport_stats_t *stats)
{
    return stats->transactions == 0U && stats->successes == 0U &&
           stats->timeouts == 0U && stats->busy == 0U &&
           stats->io_errors == 0U && stats->incomplete_frames == 0U &&
           stats->cancellations == 0U && stats->tx_bytes == 0U &&
           stats->rx_bytes == 0U;
}

static bool successful_transaction(void)
{
    fake_bus_transport_t fake;
    HOST_TEST_CHECK(fake_bus_transport_init(&fake, 9U));

    const uint8_t request[] = {0x01, 0x03, 0x20, 0x00};
    const uint8_t expected_response[] = {0x01, 0x03, 0x02, 0x00, 0x2A};
    HOST_TEST_CHECK(fake_bus_transport_expect(&fake,
                                              request,
                                              sizeof(request),
                                              BUS_TRANSPORT_OK,
                                              expected_response,
                                              sizeof(expected_response)));

    uint8_t response[16] = {0};
    size_t response_length = sizeof(response);
    HOST_TEST_CHECK(bus_transport_transact(fake_bus_transport_port(&fake),
                                           request,
                                           sizeof(request),
                                           response,
                                           sizeof(response),
                                           &response_length,
                                           25U) == BUS_TRANSPORT_OK);
    HOST_TEST_CHECK(response_length == sizeof(expected_response));
    HOST_TEST_CHECK(memcmp(response,
                           expected_response,
                           sizeof(expected_response)) == 0);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&fake) == 1U);
    HOST_TEST_CHECK(fake_bus_transport_acquire_attempt_count(&fake) == 1U);
    HOST_TEST_CHECK(fake_bus_transport_acquire_success_count(&fake) == 1U);
    HOST_TEST_CHECK(fake_bus_transport_release_count(&fake) == 1U);
    HOST_TEST_CHECK(fake_bus_transport_last_lock_timeout_ms(&fake) == 9U);
    HOST_TEST_CHECK(fake_bus_transport_last_exchange_timeout_ms(&fake) == 25U);
    HOST_TEST_CHECK(fake_bus_transport_all_expectations_met(&fake));

    bus_transport_stats_t stats = {0};
    HOST_TEST_CHECK(bus_transport_get_stats(fake_bus_transport_port(&fake),
                                            &stats));
    HOST_TEST_CHECK(stats.transactions == 1U);
    HOST_TEST_CHECK(stats.successes == 1U);
    HOST_TEST_CHECK(stats.tx_bytes == sizeof(request));
    HOST_TEST_CHECK(stats.rx_bytes == sizeof(expected_response));

    fake_bus_transport_deinit(&fake);
    return true;
}

static bool invalid_arguments_do_not_reach_backend(void)
{
    fake_bus_transport_t fake;
    HOST_TEST_CHECK(fake_bus_transport_init(&fake, 5U));
    bus_transport_t *port = fake_bus_transport_port(&fake);
    const uint8_t request[] = {0x01};
    uint8_t response[2] = {0};
    size_t response_length = 99U;

    HOST_TEST_CHECK(bus_transport_transact(NULL,
                                           request,
                                           sizeof(request),
                                           response,
                                           sizeof(response),
                                           &response_length,
                                           1U) == BUS_TRANSPORT_INVALID_ARGUMENT);
    HOST_TEST_CHECK(response_length == 0U);

    response_length = 99U;
    HOST_TEST_CHECK(bus_transport_transact(port,
                                           NULL,
                                           sizeof(request),
                                           response,
                                           sizeof(response),
                                           &response_length,
                                           1U) == BUS_TRANSPORT_INVALID_ARGUMENT);
    HOST_TEST_CHECK(response_length == 0U);
    response_length = 99U;
    HOST_TEST_CHECK(bus_transport_transact(port,
                                           request,
                                           0U,
                                           response,
                                           sizeof(response),
                                           &response_length,
                                           1U) == BUS_TRANSPORT_INVALID_ARGUMENT);
    HOST_TEST_CHECK(response_length == 0U);
    response_length = 99U;
    HOST_TEST_CHECK(bus_transport_transact(port,
                                           request,
                                           sizeof(request),
                                           NULL,
                                           sizeof(response),
                                           &response_length,
                                           1U) == BUS_TRANSPORT_INVALID_ARGUMENT);
    HOST_TEST_CHECK(response_length == 0U);
    response_length = 99U;
    HOST_TEST_CHECK(bus_transport_transact(port,
                                           request,
                                           sizeof(request),
                                           response,
                                           0U,
                                           &response_length,
                                           1U) == BUS_TRANSPORT_INVALID_ARGUMENT);
    HOST_TEST_CHECK(response_length == 0U);
    HOST_TEST_CHECK(bus_transport_transact(port,
                                           request,
                                           sizeof(request),
                                           response,
                                           sizeof(response),
                                           NULL,
                                           1U) == BUS_TRANSPORT_INVALID_ARGUMENT);
    response_length = 99U;
    HOST_TEST_CHECK(bus_transport_transact(port,
                                           request,
                                           sizeof(request),
                                           response,
                                           sizeof(response),
                                           &response_length,
                                           0U) == BUS_TRANSPORT_INVALID_ARGUMENT);
    HOST_TEST_CHECK(response_length == 0U);

    HOST_TEST_CHECK(fake_bus_transport_call_count(&fake) == 0U);
    HOST_TEST_CHECK(fake_bus_transport_acquire_attempt_count(&fake) == 0U);
    bus_transport_stats_t stats = {0};
    HOST_TEST_CHECK(bus_transport_get_stats(port, &stats));
    HOST_TEST_CHECK(stats_are_zero(&stats));
    HOST_TEST_CHECK(!bus_transport_get_stats(port, NULL));
    HOST_TEST_CHECK(!bus_transport_get_stats(NULL, &stats));

    bus_transport_backend_t backend = fake_bus_transport_backend(&fake);
    bus_transport_controller_t controller;
    backend.stats_acquire = NULL;
    HOST_TEST_CHECK(!bus_transport_controller_init(&controller, &backend, 1U));
    backend = fake_bus_transport_backend(&fake);
    backend.stats_release = NULL;
    HOST_TEST_CHECK(!bus_transport_controller_init(&controller, &backend, 1U));

    fake_bus_transport_deinit(&fake);
    return true;
}

static bool normalized_results_have_coherent_statistics(void)
{
    fake_bus_transport_t fake;
    HOST_TEST_CHECK(fake_bus_transport_init(&fake, 10U));
    const uint8_t timeout_request[] = {0x10, 0x11};
    const uint8_t busy_request[] = {0x20, 0x21, 0x22};
    const uint8_t io_request[] = {0x30, 0x31, 0x32, 0x33};
    const uint8_t incomplete_request[] = {0x40, 0x41, 0x42};
    const uint8_t incomplete_response[] = {0x40, 0x41};
    const uint8_t cancelled_request[] = {0x50, 0x51, 0x52, 0x53, 0x54};

    HOST_TEST_CHECK(fake_bus_transport_expect(&fake,
                                              timeout_request,
                                              sizeof(timeout_request),
                                              BUS_TRANSPORT_TIMEOUT,
                                              NULL,
                                              0U));
    HOST_TEST_CHECK(fake_bus_transport_expect(&fake,
                                              io_request,
                                              sizeof(io_request),
                                              BUS_TRANSPORT_IO_ERROR,
                                              NULL,
                                              0U));
    HOST_TEST_CHECK(fake_bus_transport_expect(&fake,
                                              incomplete_request,
                                              sizeof(incomplete_request),
                                              BUS_TRANSPORT_INCOMPLETE,
                                              incomplete_response,
                                              sizeof(incomplete_response)));

    uint8_t response[16] = {0};
    size_t response_length = 0U;
    bus_transport_t *port = fake_bus_transport_port(&fake);
    HOST_TEST_CHECK(bus_transport_transact(port,
                                           timeout_request,
                                           sizeof(timeout_request),
                                           response,
                                           sizeof(response),
                                           &response_length,
                                           20U) == BUS_TRANSPORT_TIMEOUT);

    fake_bus_transport_fail_next_acquire(&fake);
    HOST_TEST_CHECK(bus_transport_transact(port,
                                           busy_request,
                                           sizeof(busy_request),
                                           response,
                                           sizeof(response),
                                           &response_length,
                                           20U) == BUS_TRANSPORT_BUSY);

    HOST_TEST_CHECK(bus_transport_transact(port,
                                           io_request,
                                           sizeof(io_request),
                                           response,
                                           sizeof(response),
                                           &response_length,
                                           20U) == BUS_TRANSPORT_IO_ERROR);
    HOST_TEST_CHECK(bus_transport_transact(port,
                                           incomplete_request,
                                           sizeof(incomplete_request),
                                           response,
                                           sizeof(response),
                                           &response_length,
                                           20U) == BUS_TRANSPORT_INCOMPLETE);
    HOST_TEST_CHECK(response_length == sizeof(incomplete_response));

    fake_bus_transport_cancel(&fake);
    HOST_TEST_CHECK(bus_transport_transact(port,
                                           cancelled_request,
                                           sizeof(cancelled_request),
                                           response,
                                           sizeof(response),
                                           &response_length,
                                           20U) == BUS_TRANSPORT_CANCELLED);
    fake_bus_transport_clear_cancel(&fake);

    bus_transport_stats_t stats = {0};
    HOST_TEST_CHECK(bus_transport_get_stats(port, &stats));
    HOST_TEST_CHECK(stats.transactions == 5U);
    HOST_TEST_CHECK(stats.successes == 0U);
    HOST_TEST_CHECK(stats.timeouts == 1U);
    HOST_TEST_CHECK(stats.busy == 1U);
    HOST_TEST_CHECK(stats.io_errors == 1U);
    HOST_TEST_CHECK(stats.incomplete_frames == 1U);
    HOST_TEST_CHECK(stats.cancellations == 1U);
    HOST_TEST_CHECK(stats.tx_bytes == sizeof(timeout_request) +
                                                sizeof(io_request) +
                                                sizeof(incomplete_request) +
                                                sizeof(cancelled_request));
    HOST_TEST_CHECK(stats.rx_bytes == sizeof(incomplete_response));
    HOST_TEST_CHECK(fake_bus_transport_acquire_attempt_count(&fake) == 5U);
    HOST_TEST_CHECK(fake_bus_transport_acquire_success_count(&fake) == 4U);
    HOST_TEST_CHECK(fake_bus_transport_release_count(&fake) == 4U);
    HOST_TEST_CHECK(fake_bus_transport_call_count(&fake) == 4U);
    HOST_TEST_CHECK(fake_bus_transport_all_expectations_met(&fake));

    fake_bus_transport_deinit(&fake);
    return true;
}

static bool protocol_payloads_are_opaque_to_transport(void)
{
    fake_bus_transport_t fake;
    HOST_TEST_CHECK(fake_bus_transport_init(&fake, 5U));
    const uint8_t requests[][2] = {
        {0x01, 0x03},
        {0x01, 0x04},
        {0x01, 0x06},
    };
    const uint8_t exception_response[] = {0x01, 0x83, 0x02, 0xF1, 0xC0};
    const uint8_t bad_crc_response[] = {0x01, 0x04, 0x02, 0x00, 0x01, 0x00, 0x00};
    const uint8_t wrong_response[] = {0x02, 0x10, 0x00};
    const uint8_t *responses[] = {
        exception_response,
        bad_crc_response,
        wrong_response,
    };
    const size_t response_lengths[] = {
        sizeof(exception_response),
        sizeof(bad_crc_response),
        sizeof(wrong_response),
    };

    for (size_t index = 0U; index < HOST_TEST_ARRAY_COUNT(requests); index++) {
        HOST_TEST_CHECK(fake_bus_transport_expect(&fake,
                                                  requests[index],
                                                  sizeof(requests[index]),
                                                  BUS_TRANSPORT_OK,
                                                  responses[index],
                                                  response_lengths[index]));
    }

    bus_transport_t *port = fake_bus_transport_port(&fake);
    size_t total_rx_bytes = 0U;
    for (size_t index = 0U; index < HOST_TEST_ARRAY_COUNT(requests); index++) {
        uint8_t response[16] = {0};
        size_t response_length = 0U;
        HOST_TEST_CHECK(bus_transport_transact(port,
                                               requests[index],
                                               sizeof(requests[index]),
                                               response,
                                               sizeof(response),
                                               &response_length,
                                               10U) == BUS_TRANSPORT_OK);
        HOST_TEST_CHECK(response_length == response_lengths[index]);
        HOST_TEST_CHECK(memcmp(response,
                               responses[index],
                               response_lengths[index]) == 0);
        total_rx_bytes += response_length;
    }

    bus_transport_stats_t stats = {0};
    HOST_TEST_CHECK(bus_transport_get_stats(port, &stats));
    HOST_TEST_CHECK(stats.transactions == 3U);
    HOST_TEST_CHECK(stats.successes == 3U);
    HOST_TEST_CHECK(stats.rx_bytes == total_rx_bytes);
    HOST_TEST_CHECK(fake_bus_transport_all_expectations_met(&fake));

    fake_bus_transport_deinit(&fake);
    return true;
}

static bool lock_is_released_after_exchange_error(void)
{
    fake_bus_transport_t fake;
    HOST_TEST_CHECK(fake_bus_transport_init(&fake, 10U));
    const uint8_t failed_request[] = {0x01, 0x03};
    const uint8_t successful_request[] = {0x01, 0x06};
    const uint8_t successful_response[] = {0x01, 0x06, 0x00};
    HOST_TEST_CHECK(fake_bus_transport_expect(&fake,
                                              failed_request,
                                              sizeof(failed_request),
                                              BUS_TRANSPORT_IO_ERROR,
                                              NULL,
                                              0U));
    HOST_TEST_CHECK(fake_bus_transport_expect(&fake,
                                              successful_request,
                                              sizeof(successful_request),
                                              BUS_TRANSPORT_OK,
                                              successful_response,
                                              sizeof(successful_response)));

    uint8_t response[8] = {0};
    size_t response_length = 0U;
    bus_transport_t *port = fake_bus_transport_port(&fake);
    HOST_TEST_CHECK(bus_transport_transact(port,
                                           failed_request,
                                           sizeof(failed_request),
                                           response,
                                           sizeof(response),
                                           &response_length,
                                           10U) == BUS_TRANSPORT_IO_ERROR);
    HOST_TEST_CHECK(fake_bus_transport_release_count(&fake) == 1U);
    HOST_TEST_CHECK(bus_transport_transact(port,
                                           successful_request,
                                           sizeof(successful_request),
                                           response,
                                           sizeof(response),
                                           &response_length,
                                           10U) == BUS_TRANSPORT_OK);
    HOST_TEST_CHECK(response_length == sizeof(successful_response));
    HOST_TEST_CHECK(fake_bus_transport_acquire_success_count(&fake) == 2U);
    HOST_TEST_CHECK(fake_bus_transport_release_count(&fake) == 2U);
    HOST_TEST_CHECK(fake_bus_transport_all_expectations_met(&fake));

    fake_bus_transport_deinit(&fake);
    return true;
}

typedef struct {
    bus_transport_t *port;
    const uint8_t *request;
    size_t request_length;
    uint8_t response[16];
    size_t response_length;
    bus_transport_result_t result;
    atomic_bool done;
} transaction_thread_t;

static void *run_transaction(void *context)
{
    transaction_thread_t *thread = context;
    thread->result = bus_transport_transact(thread->port,
                                            thread->request,
                                            thread->request_length,
                                            thread->response,
                                            sizeof(thread->response),
                                            &thread->response_length,
                                            50U);
    atomic_store(&thread->done, true);
    return NULL;
}

static bool concurrent_transactions_do_not_interleave(void)
{
    fake_bus_transport_t fake;
    HOST_TEST_CHECK(fake_bus_transport_init(&fake, 50U));
    const uint8_t first_request[] = {0x11};
    const uint8_t second_request[] = {0x22};
    const uint8_t first_response[] = {0xA1};
    const uint8_t second_response[] = {0xA2};
    HOST_TEST_CHECK(fake_bus_transport_expect(&fake,
                                              first_request,
                                              sizeof(first_request),
                                              BUS_TRANSPORT_OK,
                                              first_response,
                                              sizeof(first_response)));
    HOST_TEST_CHECK(fake_bus_transport_expect(&fake,
                                              second_request,
                                              sizeof(second_request),
                                              BUS_TRANSPORT_OK,
                                              second_response,
                                              sizeof(second_response)));

    transaction_thread_t first = {
        .port = fake_bus_transport_port(&fake),
        .request = first_request,
        .request_length = sizeof(first_request),
        .done = false,
    };
    transaction_thread_t second = {
        .port = fake_bus_transport_port(&fake),
        .request = second_request,
        .request_length = sizeof(second_request),
        .done = false,
    };
    pthread_t first_thread;
    pthread_t second_thread;

    fake_bus_transport_pause_next_exchange(&fake);
    HOST_TEST_CHECK(pthread_create(&first_thread,
                                   NULL,
                                   run_transaction,
                                   &first) == 0);
    fake_bus_transport_wait_until_exchange_paused(&fake);
    HOST_TEST_CHECK(pthread_create(&second_thread,
                                   NULL,
                                   run_transaction,
                                   &second) == 0);
    fake_bus_transport_wait_for_acquire_attempts(&fake, 2U);

    HOST_TEST_CHECK(fake_bus_transport_call_count(&fake) == 1U);
    HOST_TEST_CHECK(fake_bus_transport_maximum_active_exchanges(&fake) == 1U);
    HOST_TEST_CHECK(!atomic_load(&first.done));
    HOST_TEST_CHECK(!atomic_load(&second.done));

    fake_bus_transport_resume_exchange(&fake);
    HOST_TEST_CHECK(pthread_join(first_thread, NULL) == 0);
    HOST_TEST_CHECK(pthread_join(second_thread, NULL) == 0);
    HOST_TEST_CHECK(first.result == BUS_TRANSPORT_OK);
    HOST_TEST_CHECK(second.result == BUS_TRANSPORT_OK);
    HOST_TEST_CHECK(first.response_length == sizeof(first_response));
    HOST_TEST_CHECK(second.response_length == sizeof(second_response));
    HOST_TEST_CHECK(first.response[0] == first_response[0]);
    HOST_TEST_CHECK(second.response[0] == second_response[0]);
    HOST_TEST_CHECK(fake_bus_transport_maximum_active_exchanges(&fake) == 1U);
    HOST_TEST_CHECK(fake_bus_transport_acquire_success_count(&fake) == 2U);
    HOST_TEST_CHECK(fake_bus_transport_release_count(&fake) == 2U);
    HOST_TEST_CHECK(fake_bus_transport_all_expectations_met(&fake));

    fake_bus_transport_deinit(&fake);
    return true;
}

typedef struct {
    const bus_transport_t *port;
    bus_transport_stats_t stats;
    bool result;
    atomic_bool done;
} stats_thread_t;

static void *read_stats(void *context)
{
    stats_thread_t *thread = context;
    thread->result = bus_transport_get_stats(thread->port, &thread->stats);
    atomic_store(&thread->done, true);
    return NULL;
}

static bool concurrent_stats_snapshot_is_coherent(void)
{
    fake_bus_transport_t fake;
    HOST_TEST_CHECK(fake_bus_transport_init(&fake, 20U));
    const uint8_t request[] = {0x31, 0x32, 0x33};
    const uint8_t response[] = {0x41, 0x42, 0x43, 0x44};
    HOST_TEST_CHECK(fake_bus_transport_expect(&fake,
                                              request,
                                              sizeof(request),
                                              BUS_TRANSPORT_OK,
                                              response,
                                              sizeof(response)));

    transaction_thread_t transaction = {
        .port = fake_bus_transport_port(&fake),
        .request = request,
        .request_length = sizeof(request),
        .done = false,
    };
    stats_thread_t snapshot = {
        .port = fake_bus_transport_port(&fake),
        .done = false,
    };
    pthread_t transaction_thread;
    pthread_t stats_thread;

    /* The writer owns stats_lock before record_result mutates any counter. */
    fake_bus_transport_pause_next_stats_acquire(&fake);
    HOST_TEST_CHECK(pthread_create(&transaction_thread,
                                   NULL,
                                   run_transaction,
                                   &transaction) == 0);
    fake_bus_transport_wait_until_stats_acquire_paused(&fake);
    HOST_TEST_CHECK(pthread_create(&stats_thread,
                                   NULL,
                                   read_stats,
                                   &snapshot) == 0);
    fake_bus_transport_wait_for_stats_acquire_attempts(&fake, 2U);

    HOST_TEST_CHECK(!atomic_load(&transaction.done));
    HOST_TEST_CHECK(!atomic_load(&snapshot.done));

    fake_bus_transport_resume_stats_acquire(&fake);
    HOST_TEST_CHECK(pthread_join(transaction_thread, NULL) == 0);
    HOST_TEST_CHECK(pthread_join(stats_thread, NULL) == 0);
    HOST_TEST_CHECK(transaction.result == BUS_TRANSPORT_OK);
    HOST_TEST_CHECK(snapshot.result);
    HOST_TEST_CHECK(snapshot.stats.transactions == 1U);
    HOST_TEST_CHECK(snapshot.stats.successes == 1U);
    HOST_TEST_CHECK(snapshot.stats.timeouts == 0U);
    HOST_TEST_CHECK(snapshot.stats.busy == 0U);
    HOST_TEST_CHECK(snapshot.stats.io_errors == 0U);
    HOST_TEST_CHECK(snapshot.stats.incomplete_frames == 0U);
    HOST_TEST_CHECK(snapshot.stats.cancellations == 0U);
    HOST_TEST_CHECK(snapshot.stats.tx_bytes == sizeof(request));
    HOST_TEST_CHECK(snapshot.stats.rx_bytes == sizeof(response));
    HOST_TEST_CHECK(fake_bus_transport_all_expectations_met(&fake));

    fake_bus_transport_deinit(&fake);
    return true;
}

int main(void)
{
    const host_test_case_t tests[] = {
        HOST_TEST_CASE(successful_transaction),
        HOST_TEST_CASE(invalid_arguments_do_not_reach_backend),
        HOST_TEST_CASE(normalized_results_have_coherent_statistics),
        HOST_TEST_CASE(protocol_payloads_are_opaque_to_transport),
        HOST_TEST_CASE(lock_is_released_after_exchange_error),
        HOST_TEST_CASE(concurrent_transactions_do_not_interleave),
        HOST_TEST_CASE(concurrent_stats_snapshot_is_coherent),
    };
    return host_test_exit_code(host_test_run_cases(tests,
                                                   HOST_TEST_ARRAY_COUNT(tests),
                                                   stderr));
}
