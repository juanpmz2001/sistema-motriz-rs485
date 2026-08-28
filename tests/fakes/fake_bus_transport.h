#ifndef BOTFARMS_FAKE_BUS_TRANSPORT_H
#define BOTFARMS_FAKE_BUS_TRANSPORT_H

#include "host_threads.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bus_transport.h"

#define FAKE_BUS_TRANSPORT_MAX_EXCHANGES 64U
#define FAKE_BUS_TRANSPORT_MAX_FRAME_SIZE 256U

typedef enum {
    FAKE_BUS_TRANSPORT_MISMATCH_NONE = 0,
    FAKE_BUS_TRANSPORT_MISMATCH_UNEXPECTED_CALL,
    FAKE_BUS_TRANSPORT_MISMATCH_REQUEST_LENGTH,
    FAKE_BUS_TRANSPORT_MISMATCH_REQUEST_BYTES,
    FAKE_BUS_TRANSPORT_MISMATCH_RESPONSE_CAPACITY,
} fake_bus_transport_mismatch_t;

typedef struct {
    bool check_request;
    uint8_t expected_request[FAKE_BUS_TRANSPORT_MAX_FRAME_SIZE];
    size_t expected_request_length;
    bus_transport_result_t result;
    uint8_t response[FAKE_BUS_TRANSPORT_MAX_FRAME_SIZE];
    size_t response_length;
} fake_bus_transport_exchange_t;

/*
 * Host-only scripted backend. The object owns pthread synchronization objects
 * and must not be copied after initialization.
 */
typedef struct {
    bus_transport_controller_t controller;

    pthread_mutex_t bus_lock;
    pthread_mutex_t stats_lock;
    pthread_mutex_t state_lock;
    pthread_cond_t state_changed;

    fake_bus_transport_exchange_t exchanges[FAKE_BUS_TRANSPORT_MAX_EXCHANGES];
    size_t exchange_count;
    size_t next_exchange;

    size_t acquire_attempts;
    size_t acquire_successes;
    size_t release_calls;
    size_t exchange_calls;
    size_t active_exchanges;
    size_t maximum_active_exchanges;
    size_t stats_acquire_attempts;
    size_t stats_release_calls;
    uint32_t last_lock_timeout_ms;
    uint32_t last_exchange_timeout_ms;

    fake_bus_transport_mismatch_t mismatch;
    size_t mismatch_exchange;
    bool fail_next_acquire;
    bool cancelled;
    bool initialized;

    bool pause_next_exchange;
    bool exchange_paused;
    bool resume_exchange;
    bool pause_next_stats_acquire;
    bool stats_acquire_paused;
    bool resume_stats_acquire;
} fake_bus_transport_t;

bool fake_bus_transport_init(fake_bus_transport_t *fake,
                             uint32_t lock_timeout_ms);
void fake_bus_transport_deinit(fake_bus_transport_t *fake);

bus_transport_t *fake_bus_transport_port(fake_bus_transport_t *fake);
bus_transport_backend_t fake_bus_transport_backend(fake_bus_transport_t *fake);

/* A NULL expected_request with length zero accepts any non-empty request. */
bool fake_bus_transport_expect(fake_bus_transport_t *fake,
                               const uint8_t *expected_request,
                               size_t expected_request_length,
                               bus_transport_result_t result,
                               const uint8_t *response,
                               size_t response_length);
bool fake_bus_transport_expect_echo(fake_bus_transport_t *fake,
                                    const uint8_t *expected_request,
                                    size_t expected_request_length,
                                    bus_transport_result_t result);

void fake_bus_transport_fail_next_acquire(fake_bus_transport_t *fake);
void fake_bus_transport_cancel(fake_bus_transport_t *fake);
void fake_bus_transport_clear_cancel(fake_bus_transport_t *fake);

size_t fake_bus_transport_call_count(fake_bus_transport_t *fake);
size_t fake_bus_transport_acquire_attempt_count(fake_bus_transport_t *fake);
size_t fake_bus_transport_acquire_success_count(fake_bus_transport_t *fake);
size_t fake_bus_transport_release_count(fake_bus_transport_t *fake);
size_t fake_bus_transport_maximum_active_exchanges(fake_bus_transport_t *fake);
uint32_t fake_bus_transport_last_lock_timeout_ms(fake_bus_transport_t *fake);
uint32_t fake_bus_transport_last_exchange_timeout_ms(fake_bus_transport_t *fake);
fake_bus_transport_mismatch_t fake_bus_transport_mismatch(
    fake_bus_transport_t *fake);
size_t fake_bus_transport_mismatch_exchange(fake_bus_transport_t *fake);
bool fake_bus_transport_all_expectations_met(fake_bus_transport_t *fake);

/* Deterministic concurrency gates. Call arm before starting the worker. */
void fake_bus_transport_pause_next_exchange(fake_bus_transport_t *fake);
void fake_bus_transport_wait_until_exchange_paused(fake_bus_transport_t *fake);
void fake_bus_transport_resume_exchange(fake_bus_transport_t *fake);
void fake_bus_transport_wait_for_acquire_attempts(fake_bus_transport_t *fake,
                                                  size_t expected_count);

void fake_bus_transport_pause_next_stats_acquire(fake_bus_transport_t *fake);
void fake_bus_transport_wait_until_stats_acquire_paused(
    fake_bus_transport_t *fake);
void fake_bus_transport_resume_stats_acquire(fake_bus_transport_t *fake);
void fake_bus_transport_wait_for_stats_acquire_attempts(
    fake_bus_transport_t *fake,
    size_t expected_count);

#endif
