#ifndef RS485_TRANSPORT_H
#define RS485_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "bus_transport.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uart_port_t uart_port;
    int tx_pin;
    int rx_pin;
    int rts_pin;
    bool use_rs485_half_duplex;
    uint32_t baud_rate;
    uint32_t lock_timeout_ms;
    size_t rx_buffer_size;
    size_t tx_buffer_size;
} rs485_transport_config_t;

typedef struct {
    rs485_transport_config_t config;
    bus_transport_controller_t controller;
    StaticSemaphore_t lock_storage;
    SemaphoreHandle_t lock;
    StaticSemaphore_t stats_lock_storage;
    SemaphoreHandle_t stats_lock;
    bool uart_installed;
    bool initialized;
    volatile bool cancelled;
} rs485_transport_t;

esp_err_t rs485_transport_init(rs485_transport_t *transport,
                               const rs485_transport_config_t *config);
void rs485_transport_deinit(rs485_transport_t *transport);
bus_transport_t *rs485_transport_port(rs485_transport_t *transport);
void rs485_transport_cancel(rs485_transport_t *transport);
bool rs485_transport_get_stats(const rs485_transport_t *transport,
                               bus_transport_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
