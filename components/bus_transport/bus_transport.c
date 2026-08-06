#include "bus_transport.h"

#include <string.h>

static void record_result(bus_transport_controller_t *controller,
                          bus_transport_result_t result,
                          size_t request_length,
                          size_t response_length)
{
    if (!controller->backend.stats_acquire(controller->backend.context)) {
        return;
    }
    controller->stats.transactions++;
    controller->stats.tx_bytes += request_length;
    controller->stats.rx_bytes += response_length;
    switch (result) {
    case BUS_TRANSPORT_OK:
        controller->stats.successes++;
        break;
    case BUS_TRANSPORT_TIMEOUT:
        controller->stats.timeouts++;
        break;
    case BUS_TRANSPORT_BUSY:
        controller->stats.busy++;
        break;
    case BUS_TRANSPORT_IO_ERROR:
        controller->stats.io_errors++;
        break;
    case BUS_TRANSPORT_INCOMPLETE:
        controller->stats.incomplete_frames++;
        break;
    case BUS_TRANSPORT_CANCELLED:
        controller->stats.cancellations++;
        break;
    default:
        break;
    }
    controller->backend.stats_release(controller->backend.context);
}

static bus_transport_result_t controller_transact(bus_transport_t *port,
                                                  const uint8_t *request,
                                                  size_t request_length,
                                                  uint8_t *response,
                                                  size_t response_capacity,
                                                  size_t *response_length,
                                                  uint32_t timeout_ms)
{
    bus_transport_controller_t *controller = port ? port->context : NULL;
    if (response_length) {
        *response_length = 0;
    }
    if (!controller || !controller->initialized || !request || request_length == 0 ||
        !response || response_capacity == 0 || !response_length || timeout_ms == 0) {
        return BUS_TRANSPORT_INVALID_ARGUMENT;
    }

    uint32_t lock_timeout_ms = controller->lock_timeout_ms;
    if (lock_timeout_ms == 0 || lock_timeout_ms > timeout_ms) {
        lock_timeout_ms = timeout_ms;
    }
    if (!controller->backend.acquire(controller->backend.context, lock_timeout_ms)) {
        record_result(controller, BUS_TRANSPORT_BUSY, 0, 0);
        return BUS_TRANSPORT_BUSY;
    }

    bus_transport_result_t result = controller->backend.exchange(
        controller->backend.context,
        request,
        request_length,
        response,
        response_capacity,
        response_length,
        timeout_ms);
    controller->backend.release(controller->backend.context);
    record_result(controller, result, request_length, *response_length);
    return result;
}

static bool controller_get_stats(const bus_transport_t *port,
                                 bus_transport_stats_t *stats)
{
    bus_transport_controller_t *controller = port ? port->context : NULL;
    if (!controller || !controller->initialized || !stats) {
        return false;
    }
    if (!controller->backend.stats_acquire(controller->backend.context)) {
        return false;
    }
    *stats = controller->stats;
    controller->backend.stats_release(controller->backend.context);
    return true;
}

bool bus_transport_controller_init(bus_transport_controller_t *controller,
                                   const bus_transport_backend_t *backend,
                                   uint32_t lock_timeout_ms)
{
    static const bus_transport_ops_t ops = {
        .transact = controller_transact,
        .get_stats = controller_get_stats,
    };
    if (!controller || !backend || !backend->acquire || !backend->release ||
        !backend->stats_acquire || !backend->stats_release ||
        !backend->exchange) {
        return false;
    }
    memset(controller, 0, sizeof(*controller));
    controller->backend = *backend;
    controller->lock_timeout_ms = lock_timeout_ms;
    controller->port.ops = &ops;
    controller->port.context = controller;
    controller->initialized = true;
    return true;
}

void bus_transport_controller_reset(bus_transport_controller_t *controller)
{
    if (controller) {
        memset(controller, 0, sizeof(*controller));
    }
}

bus_transport_t *bus_transport_controller_port(bus_transport_controller_t *controller)
{
    return controller && controller->initialized ? &controller->port : NULL;
}

bus_transport_result_t bus_transport_transact(bus_transport_t *transport,
                                              const uint8_t *request,
                                              size_t request_length,
                                              uint8_t *response,
                                              size_t response_capacity,
                                              size_t *response_length,
                                              uint32_t timeout_ms)
{
    if (!transport || !transport->ops || !transport->ops->transact) {
        if (response_length) {
            *response_length = 0;
        }
        return BUS_TRANSPORT_INVALID_ARGUMENT;
    }
    return transport->ops->transact(transport,
                                    request,
                                    request_length,
                                    response,
                                    response_capacity,
                                    response_length,
                                    timeout_ms);
}

bool bus_transport_get_stats(const bus_transport_t *transport,
                             bus_transport_stats_t *stats)
{
    return transport && transport->ops && transport->ops->get_stats
               ? transport->ops->get_stats(transport, stats)
               : false;
}
