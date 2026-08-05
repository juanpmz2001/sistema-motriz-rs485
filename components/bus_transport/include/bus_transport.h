#ifndef BUS_TRANSPORT_H
#define BUS_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BUS_TRANSPORT_OK = 0,
    BUS_TRANSPORT_INVALID_ARGUMENT,
    BUS_TRANSPORT_TIMEOUT,
    BUS_TRANSPORT_BUSY,
    BUS_TRANSPORT_IO_ERROR,
    BUS_TRANSPORT_INCOMPLETE,
    BUS_TRANSPORT_CANCELLED,
} bus_transport_result_t;

typedef struct {
    uint32_t transactions;
    uint32_t successes;
    uint32_t timeouts;
    uint32_t busy;
    uint32_t io_errors;
    uint32_t incomplete_frames;
    uint32_t cancellations;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
} bus_transport_stats_t;

typedef struct bus_transport bus_transport_t;

typedef struct {
    bus_transport_result_t (*transact)(bus_transport_t *transport,
                                       const uint8_t *request,
                                       size_t request_length,
                                       uint8_t *response,
                                       size_t response_capacity,
                                       size_t *response_length,
                                       uint32_t timeout_ms);
    bool (*get_stats)(const bus_transport_t *transport, bus_transport_stats_t *stats);
} bus_transport_ops_t;

struct bus_transport {
    const bus_transport_ops_t *ops;
    void *context;
};

typedef struct {
    bool (*acquire)(void *context, uint32_t timeout_ms);
    void (*release)(void *context);
    bus_transport_result_t (*exchange)(void *context,
                                       const uint8_t *request,
                                       size_t request_length,
                                       uint8_t *response,
                                       size_t response_capacity,
                                       size_t *response_length,
                                       uint32_t timeout_ms);
    void *context;
} bus_transport_backend_t;

typedef struct {
    bus_transport_t port;
    bus_transport_backend_t backend;
    bus_transport_stats_t stats;
    uint32_t lock_timeout_ms;
    bool initialized;
} bus_transport_controller_t;

bool bus_transport_controller_init(bus_transport_controller_t *controller,
                                   const bus_transport_backend_t *backend,
                                   uint32_t lock_timeout_ms);
void bus_transport_controller_reset(bus_transport_controller_t *controller);
bus_transport_t *bus_transport_controller_port(bus_transport_controller_t *controller);

bus_transport_result_t bus_transport_transact(bus_transport_t *transport,
                                              const uint8_t *request,
                                              size_t request_length,
                                              uint8_t *response,
                                              size_t response_capacity,
                                              size_t *response_length,
                                              uint32_t timeout_ms);
bool bus_transport_get_stats(const bus_transport_t *transport,
                             bus_transport_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
