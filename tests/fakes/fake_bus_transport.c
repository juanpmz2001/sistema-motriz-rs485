#include "fake_bus_transport.h"

#include <string.h>

static void record_mismatch(fake_bus_transport_t *fake,
                            fake_bus_transport_mismatch_t mismatch,
                            size_t exchange_index)
{
    if (fake->mismatch == FAKE_BUS_TRANSPORT_MISMATCH_NONE) {
        fake->mismatch = mismatch;
        fake->mismatch_exchange = exchange_index;
    }
}

static bool acquire_bus(void *context, uint32_t timeout_ms)
{
    fake_bus_transport_t *fake = context;
    if (!fake || !fake->initialized) {
        return false;
    }

    pthread_mutex_lock(&fake->state_lock);
    fake->acquire_attempts++;
    fake->last_lock_timeout_ms = timeout_ms;
    bool fail = fake->fail_next_acquire;
    fake->fail_next_acquire = false;
    pthread_cond_broadcast(&fake->state_changed);
    pthread_mutex_unlock(&fake->state_lock);
    if (fail || pthread_mutex_lock(&fake->bus_lock) != 0) {
        return false;
    }

    pthread_mutex_lock(&fake->state_lock);
    fake->acquire_successes++;
    pthread_cond_broadcast(&fake->state_changed);
    pthread_mutex_unlock(&fake->state_lock);
    return true;
}

static void release_bus(void *context)
{
    fake_bus_transport_t *fake = context;
    if (!fake || !fake->initialized) {
        return;
    }

    pthread_mutex_lock(&fake->state_lock);
    fake->release_calls++;
    pthread_cond_broadcast(&fake->state_changed);
    pthread_mutex_unlock(&fake->state_lock);
    pthread_mutex_unlock(&fake->bus_lock);
}

static bool acquire_stats(void *context)
{
    fake_bus_transport_t *fake = context;
    if (!fake || !fake->initialized) {
        return false;
    }

    pthread_mutex_lock(&fake->state_lock);
    fake->stats_acquire_attempts++;
    pthread_cond_broadcast(&fake->state_changed);
    pthread_mutex_unlock(&fake->state_lock);

    if (pthread_mutex_lock(&fake->stats_lock) != 0) {
        return false;
    }

    pthread_mutex_lock(&fake->state_lock);
    if (fake->pause_next_stats_acquire) {
        fake->pause_next_stats_acquire = false;
        fake->stats_acquire_paused = true;
        fake->resume_stats_acquire = false;
        pthread_cond_broadcast(&fake->state_changed);
        while (!fake->resume_stats_acquire) {
            pthread_cond_wait(&fake->state_changed, &fake->state_lock);
        }
        fake->stats_acquire_paused = false;
    }
    pthread_mutex_unlock(&fake->state_lock);
    return true;
}

static void release_stats(void *context)
{
    fake_bus_transport_t *fake = context;
    if (!fake || !fake->initialized) {
        return;
    }

    pthread_mutex_unlock(&fake->stats_lock);
    pthread_mutex_lock(&fake->state_lock);
    fake->stats_release_calls++;
    pthread_cond_broadcast(&fake->state_changed);
    pthread_mutex_unlock(&fake->state_lock);
}

static bus_transport_result_t exchange(void *context,
                                       const uint8_t *request,
                                       size_t request_length,
                                       uint8_t *response,
                                       size_t response_capacity,
                                       size_t *response_length,
                                       uint32_t timeout_ms)
{
    fake_bus_transport_t *fake = context;
    if (!fake || !fake->initialized || !request || request_length == 0U ||
        !response || response_capacity == 0U || !response_length) {
        return BUS_TRANSPORT_INVALID_ARGUMENT;
    }
    *response_length = 0U;

    pthread_mutex_lock(&fake->state_lock);
    size_t call_index = fake->exchange_calls;
    fake->exchange_calls++;
    fake->last_exchange_timeout_ms = timeout_ms;
    fake->active_exchanges++;
    if (fake->active_exchanges > fake->maximum_active_exchanges) {
        fake->maximum_active_exchanges = fake->active_exchanges;
    }

    bool cancelled = fake->cancelled;
    bool have_exchange = false;
    fake_bus_transport_exchange_t scripted = {0};
    if (!cancelled && fake->next_exchange < fake->exchange_count) {
        scripted = fake->exchanges[fake->next_exchange++];
        have_exchange = true;
        if (scripted.check_request &&
            scripted.expected_request_length != request_length) {
            record_mismatch(fake,
                            FAKE_BUS_TRANSPORT_MISMATCH_REQUEST_LENGTH,
                            call_index);
        } else if (scripted.check_request &&
                   memcmp(scripted.expected_request,
                          request,
                          request_length) != 0) {
            record_mismatch(fake,
                            FAKE_BUS_TRANSPORT_MISMATCH_REQUEST_BYTES,
                            call_index);
        }
    } else if (!cancelled) {
        record_mismatch(fake,
                        FAKE_BUS_TRANSPORT_MISMATCH_UNEXPECTED_CALL,
                        call_index);
    }

    if (fake->pause_next_exchange) {
        fake->pause_next_exchange = false;
        fake->exchange_paused = true;
        fake->resume_exchange = false;
        pthread_cond_broadcast(&fake->state_changed);
        while (!fake->resume_exchange) {
            pthread_cond_wait(&fake->state_changed, &fake->state_lock);
        }
        fake->exchange_paused = false;
        cancelled = fake->cancelled;
    }

    bus_transport_result_t result = BUS_TRANSPORT_IO_ERROR;
    if (cancelled) {
        result = BUS_TRANSPORT_CANCELLED;
    } else if (have_exchange) {
        result = scripted.result;
        if (scripted.response_length > response_capacity) {
            memcpy(response, scripted.response, response_capacity);
            *response_length = response_capacity;
            record_mismatch(fake,
                            FAKE_BUS_TRANSPORT_MISMATCH_RESPONSE_CAPACITY,
                            call_index);
            result = BUS_TRANSPORT_INCOMPLETE;
        } else if (scripted.response_length > 0U) {
            memcpy(response, scripted.response, scripted.response_length);
            *response_length = scripted.response_length;
        }
    }

    fake->active_exchanges--;
    pthread_cond_broadcast(&fake->state_changed);
    pthread_mutex_unlock(&fake->state_lock);
    return result;
}

bus_transport_backend_t fake_bus_transport_backend(fake_bus_transport_t *fake)
{
    return (bus_transport_backend_t) {
        .acquire = acquire_bus,
        .release = release_bus,
        .stats_acquire = acquire_stats,
        .stats_release = release_stats,
        .exchange = exchange,
        .context = fake,
    };
}

bool fake_bus_transport_init(fake_bus_transport_t *fake,
                             uint32_t lock_timeout_ms)
{
    if (!fake) {
        return false;
    }
    memset(fake, 0, sizeof(*fake));

    if (pthread_mutex_init(&fake->bus_lock, NULL) != 0) {
        return false;
    }
    if (pthread_mutex_init(&fake->stats_lock, NULL) != 0) {
        pthread_mutex_destroy(&fake->bus_lock);
        return false;
    }
    if (pthread_mutex_init(&fake->state_lock, NULL) != 0) {
        pthread_mutex_destroy(&fake->stats_lock);
        pthread_mutex_destroy(&fake->bus_lock);
        return false;
    }
    if (pthread_cond_init(&fake->state_changed, NULL) != 0) {
        pthread_mutex_destroy(&fake->state_lock);
        pthread_mutex_destroy(&fake->stats_lock);
        pthread_mutex_destroy(&fake->bus_lock);
        return false;
    }

    fake->initialized = true;
    bus_transport_backend_t backend = fake_bus_transport_backend(fake);
    if (!bus_transport_controller_init(&fake->controller,
                                       &backend,
                                       lock_timeout_ms)) {
        fake_bus_transport_deinit(fake);
        return false;
    }
    return true;
}

void fake_bus_transport_deinit(fake_bus_transport_t *fake)
{
    if (!fake || !fake->initialized) {
        return;
    }
    bus_transport_controller_reset(&fake->controller);
    fake->initialized = false;
    pthread_cond_destroy(&fake->state_changed);
    pthread_mutex_destroy(&fake->state_lock);
    pthread_mutex_destroy(&fake->stats_lock);
    pthread_mutex_destroy(&fake->bus_lock);
}

bus_transport_t *fake_bus_transport_port(fake_bus_transport_t *fake)
{
    return fake && fake->initialized
               ? bus_transport_controller_port(&fake->controller)
               : NULL;
}

bool fake_bus_transport_expect(fake_bus_transport_t *fake,
                               const uint8_t *expected_request,
                               size_t expected_request_length,
                               bus_transport_result_t result,
                               const uint8_t *response,
                               size_t response_length)
{
    if (!fake || !fake->initialized ||
        expected_request_length > FAKE_BUS_TRANSPORT_MAX_FRAME_SIZE ||
        response_length > FAKE_BUS_TRANSPORT_MAX_FRAME_SIZE ||
        (!expected_request && expected_request_length > 0U) ||
        (!response && response_length > 0U) ||
        result > BUS_TRANSPORT_CANCELLED) {
        return false;
    }

    pthread_mutex_lock(&fake->state_lock);
    if (fake->exchange_count >= FAKE_BUS_TRANSPORT_MAX_EXCHANGES) {
        pthread_mutex_unlock(&fake->state_lock);
        return false;
    }
    fake_bus_transport_exchange_t *scripted =
        &fake->exchanges[fake->exchange_count++];
    memset(scripted, 0, sizeof(*scripted));
    scripted->check_request = expected_request != NULL;
    scripted->expected_request_length = expected_request_length;
    scripted->result = result;
    scripted->response_length = response_length;
    if (expected_request_length > 0U) {
        memcpy(scripted->expected_request,
               expected_request,
               expected_request_length);
    }
    if (response_length > 0U) {
        memcpy(scripted->response, response, response_length);
    }
    pthread_mutex_unlock(&fake->state_lock);
    return true;
}

bool fake_bus_transport_expect_echo(fake_bus_transport_t *fake,
                                    const uint8_t *expected_request,
                                    size_t expected_request_length,
                                    bus_transport_result_t result)
{
    if (!expected_request || expected_request_length == 0U) {
        return false;
    }
    return fake_bus_transport_expect(fake,
                                     expected_request,
                                     expected_request_length,
                                     result,
                                     expected_request,
                                     expected_request_length);
}

void fake_bus_transport_fail_next_acquire(fake_bus_transport_t *fake)
{
    if (!fake || !fake->initialized) {
        return;
    }
    pthread_mutex_lock(&fake->state_lock);
    fake->fail_next_acquire = true;
    pthread_mutex_unlock(&fake->state_lock);
}

void fake_bus_transport_cancel(fake_bus_transport_t *fake)
{
    if (!fake || !fake->initialized) {
        return;
    }
    pthread_mutex_lock(&fake->state_lock);
    fake->cancelled = true;
    pthread_cond_broadcast(&fake->state_changed);
    pthread_mutex_unlock(&fake->state_lock);
}

void fake_bus_transport_clear_cancel(fake_bus_transport_t *fake)
{
    if (!fake || !fake->initialized) {
        return;
    }
    pthread_mutex_lock(&fake->state_lock);
    fake->cancelled = false;
    pthread_mutex_unlock(&fake->state_lock);
}

static size_t read_size(fake_bus_transport_t *fake, const size_t *value)
{
    if (!fake || !fake->initialized) {
        return 0U;
    }
    pthread_mutex_lock(&fake->state_lock);
    size_t result = *value;
    pthread_mutex_unlock(&fake->state_lock);
    return result;
}

size_t fake_bus_transport_call_count(fake_bus_transport_t *fake)
{
    return read_size(fake, fake ? &fake->exchange_calls : NULL);
}

size_t fake_bus_transport_acquire_attempt_count(fake_bus_transport_t *fake)
{
    return read_size(fake, fake ? &fake->acquire_attempts : NULL);
}

size_t fake_bus_transport_acquire_success_count(fake_bus_transport_t *fake)
{
    return read_size(fake, fake ? &fake->acquire_successes : NULL);
}

size_t fake_bus_transport_release_count(fake_bus_transport_t *fake)
{
    return read_size(fake, fake ? &fake->release_calls : NULL);
}

size_t fake_bus_transport_maximum_active_exchanges(fake_bus_transport_t *fake)
{
    return read_size(fake, fake ? &fake->maximum_active_exchanges : NULL);
}

uint32_t fake_bus_transport_last_lock_timeout_ms(fake_bus_transport_t *fake)
{
    if (!fake || !fake->initialized) {
        return 0U;
    }
    pthread_mutex_lock(&fake->state_lock);
    uint32_t result = fake->last_lock_timeout_ms;
    pthread_mutex_unlock(&fake->state_lock);
    return result;
}

uint32_t fake_bus_transport_last_exchange_timeout_ms(fake_bus_transport_t *fake)
{
    if (!fake || !fake->initialized) {
        return 0U;
    }
    pthread_mutex_lock(&fake->state_lock);
    uint32_t result = fake->last_exchange_timeout_ms;
    pthread_mutex_unlock(&fake->state_lock);
    return result;
}

fake_bus_transport_mismatch_t fake_bus_transport_mismatch(
    fake_bus_transport_t *fake)
{
    if (!fake || !fake->initialized) {
        return FAKE_BUS_TRANSPORT_MISMATCH_UNEXPECTED_CALL;
    }
    pthread_mutex_lock(&fake->state_lock);
    fake_bus_transport_mismatch_t result = fake->mismatch;
    pthread_mutex_unlock(&fake->state_lock);
    return result;
}

size_t fake_bus_transport_mismatch_exchange(fake_bus_transport_t *fake)
{
    return read_size(fake, fake ? &fake->mismatch_exchange : NULL);
}

bool fake_bus_transport_all_expectations_met(fake_bus_transport_t *fake)
{
    if (!fake || !fake->initialized) {
        return false;
    }
    pthread_mutex_lock(&fake->state_lock);
    bool result = fake->mismatch == FAKE_BUS_TRANSPORT_MISMATCH_NONE &&
                  fake->next_exchange == fake->exchange_count;
    pthread_mutex_unlock(&fake->state_lock);
    return result;
}

void fake_bus_transport_pause_next_exchange(fake_bus_transport_t *fake)
{
    if (!fake || !fake->initialized) {
        return;
    }
    pthread_mutex_lock(&fake->state_lock);
    fake->pause_next_exchange = true;
    fake->resume_exchange = false;
    pthread_mutex_unlock(&fake->state_lock);
}

void fake_bus_transport_wait_until_exchange_paused(fake_bus_transport_t *fake)
{
    if (!fake || !fake->initialized) {
        return;
    }
    pthread_mutex_lock(&fake->state_lock);
    while (!fake->exchange_paused) {
        pthread_cond_wait(&fake->state_changed, &fake->state_lock);
    }
    pthread_mutex_unlock(&fake->state_lock);
}

void fake_bus_transport_resume_exchange(fake_bus_transport_t *fake)
{
    if (!fake || !fake->initialized) {
        return;
    }
    pthread_mutex_lock(&fake->state_lock);
    fake->resume_exchange = true;
    pthread_cond_broadcast(&fake->state_changed);
    pthread_mutex_unlock(&fake->state_lock);
}

void fake_bus_transport_wait_for_acquire_attempts(fake_bus_transport_t *fake,
                                                  size_t expected_count)
{
    if (!fake || !fake->initialized) {
        return;
    }
    pthread_mutex_lock(&fake->state_lock);
    while (fake->acquire_attempts < expected_count) {
        pthread_cond_wait(&fake->state_changed, &fake->state_lock);
    }
    pthread_mutex_unlock(&fake->state_lock);
}

void fake_bus_transport_pause_next_stats_acquire(fake_bus_transport_t *fake)
{
    if (!fake || !fake->initialized) {
        return;
    }
    pthread_mutex_lock(&fake->state_lock);
    fake->pause_next_stats_acquire = true;
    fake->resume_stats_acquire = false;
    pthread_mutex_unlock(&fake->state_lock);
}

void fake_bus_transport_wait_until_stats_acquire_paused(
    fake_bus_transport_t *fake)
{
    if (!fake || !fake->initialized) {
        return;
    }
    pthread_mutex_lock(&fake->state_lock);
    while (!fake->stats_acquire_paused) {
        pthread_cond_wait(&fake->state_changed, &fake->state_lock);
    }
    pthread_mutex_unlock(&fake->state_lock);
}

void fake_bus_transport_resume_stats_acquire(fake_bus_transport_t *fake)
{
    if (!fake || !fake->initialized) {
        return;
    }
    pthread_mutex_lock(&fake->state_lock);
    fake->resume_stats_acquire = true;
    pthread_cond_broadcast(&fake->state_changed);
    pthread_mutex_unlock(&fake->state_lock);
}

void fake_bus_transport_wait_for_stats_acquire_attempts(
    fake_bus_transport_t *fake,
    size_t expected_count)
{
    if (!fake || !fake->initialized) {
        return;
    }
    pthread_mutex_lock(&fake->state_lock);
    while (fake->stats_acquire_attempts < expected_count) {
        pthread_cond_wait(&fake->state_changed, &fake->state_lock);
    }
    pthread_mutex_unlock(&fake->state_lock);
}
