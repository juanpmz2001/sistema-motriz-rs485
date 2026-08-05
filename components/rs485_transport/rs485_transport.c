#include "rs485_transport.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#define RS485_DEFAULT_BAUD_RATE 115200U
#define RS485_DEFAULT_BUFFER_SIZE 512U
#define RS485_DEFAULT_LOCK_TIMEOUT_MS 1000U
#define RS485_INTERBYTE_IDLE_MS 3U

static const char *TAG = "rs485_transport";

static uint32_t elapsed_ms(int64_t start_us)
{
    return (uint32_t)((esp_timer_get_time() - start_us) / 1000);
}

static bool acquire_bus(void *context, uint32_t timeout_ms)
{
    rs485_transport_t *transport = context;
    if (!transport || !transport->initialized || transport->cancelled) {
        return false;
    }
    return xSemaphoreTake(transport->lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void release_bus(void *context)
{
    rs485_transport_t *transport = context;
    if (transport && transport->lock) {
        xSemaphoreGive(transport->lock);
    }
}

static bus_transport_result_t exchange_uart(void *context,
                                            const uint8_t *request,
                                            size_t request_length,
                                            uint8_t *response,
                                            size_t response_capacity,
                                            size_t *response_length,
                                            uint32_t timeout_ms)
{
    rs485_transport_t *transport = context;
    if (!transport || !transport->initialized || !request || request_length == 0 ||
        !response || response_capacity == 0 || !response_length) {
        return BUS_TRANSPORT_INVALID_ARGUMENT;
    }
    *response_length = 0;
    if (transport->cancelled) {
        return BUS_TRANSPORT_CANCELLED;
    }

    uart_flush_input(transport->config.uart_port);
    int written = uart_write_bytes(transport->config.uart_port,
                                   request,
                                   request_length);
    if (written != (int)request_length) {
        return BUS_TRANSPORT_IO_ERROR;
    }
    esp_err_t tx_error = uart_wait_tx_done(transport->config.uart_port,
                                           pdMS_TO_TICKS(timeout_ms));
    if (tx_error != ESP_OK) {
        return tx_error == ESP_ERR_TIMEOUT ? BUS_TRANSPORT_TIMEOUT
                                           : BUS_TRANSPORT_IO_ERROR;
    }

    int64_t start_us = esp_timer_get_time();
    int64_t last_byte_us = 0;
    while (elapsed_ms(start_us) < timeout_ms && *response_length < response_capacity) {
        if (transport->cancelled) {
            return BUS_TRANSPORT_CANCELLED;
        }
        uint32_t remaining_ms = timeout_ms - elapsed_ms(start_us);
        uint32_t read_wait_ms = remaining_ms > 10U ? 10U : remaining_ms;
        if (*response_length > 0 && read_wait_ms > RS485_INTERBYTE_IDLE_MS) {
            read_wait_ms = RS485_INTERBYTE_IDLE_MS;
        }
        if (read_wait_ms == 0) {
            read_wait_ms = 1;
        }
        int read = uart_read_bytes(transport->config.uart_port,
                                   response + *response_length,
                                   response_capacity - *response_length,
                                   pdMS_TO_TICKS(read_wait_ms));
        if (read < 0) {
            return BUS_TRANSPORT_IO_ERROR;
        }
        if (read > 0) {
            *response_length += (size_t)read;
            last_byte_us = esp_timer_get_time();
            continue;
        }
        if (*response_length > 0 && last_byte_us != 0 &&
            (uint32_t)((esp_timer_get_time() - last_byte_us) / 1000) >=
                RS485_INTERBYTE_IDLE_MS) {
            return BUS_TRANSPORT_OK;
        }
    }

    if (*response_length == response_capacity) {
        return BUS_TRANSPORT_OK;
    }
    return *response_length == 0 ? BUS_TRANSPORT_TIMEOUT
                                 : BUS_TRANSPORT_INCOMPLETE;
}

esp_err_t rs485_transport_init(rs485_transport_t *transport,
                               const rs485_transport_config_t *config)
{
    if (!transport || !config || config->uart_port < UART_NUM_0 ||
        config->uart_port >= UART_NUM_MAX || config->tx_pin < 0 ||
        config->rx_pin < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(transport, 0, sizeof(*transport));
    transport->config = *config;
    if (transport->config.baud_rate == 0) {
        transport->config.baud_rate = RS485_DEFAULT_BAUD_RATE;
    }
    if (transport->config.rx_buffer_size == 0) {
        transport->config.rx_buffer_size = RS485_DEFAULT_BUFFER_SIZE;
    }
    if (transport->config.tx_buffer_size == 0) {
        transport->config.tx_buffer_size = RS485_DEFAULT_BUFFER_SIZE;
    }
    if (transport->config.lock_timeout_ms == 0) {
        transport->config.lock_timeout_ms = RS485_DEFAULT_LOCK_TIMEOUT_MS;
    }

    transport->lock = xSemaphoreCreateMutexStatic(&transport->lock_storage);
    if (!transport->lock) {
        return ESP_ERR_NO_MEM;
    }
    transport->initialized = true;

    uart_config_t uart_config = {
        .baud_rate = (int)transport->config.baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t error = uart_driver_install(transport->config.uart_port,
                                          transport->config.rx_buffer_size,
                                          transport->config.tx_buffer_size,
                                          0,
                                          NULL,
                                          0);
    if (error != ESP_OK) {
        rs485_transport_deinit(transport);
        return error;
    }
    transport->uart_installed = true;
    error = uart_param_config(transport->config.uart_port, &uart_config);
    if (error == ESP_OK) {
        error = uart_set_pin(transport->config.uart_port,
                             transport->config.tx_pin,
                             transport->config.rx_pin,
                             transport->config.rts_pin,
                             UART_PIN_NO_CHANGE);
    }
    if (error == ESP_OK && transport->config.use_rs485_half_duplex) {
        error = uart_set_mode(transport->config.uart_port,
                              UART_MODE_RS485_HALF_DUPLEX);
    }
    if (error != ESP_OK) {
        rs485_transport_deinit(transport);
        return error;
    }

    bus_transport_backend_t backend = {
        .acquire = acquire_bus,
        .release = release_bus,
        .exchange = exchange_uart,
        .context = transport,
    };
    if (!bus_transport_controller_init(&transport->controller,
                                       &backend,
                                       transport->config.lock_timeout_ms)) {
        rs485_transport_deinit(transport);
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG,
             "UART%d RS485 transport ready tx=%d rx=%d baud=%lu",
             transport->config.uart_port,
             transport->config.tx_pin,
             transport->config.rx_pin,
             (unsigned long)transport->config.baud_rate);
    return ESP_OK;
}

void rs485_transport_deinit(rs485_transport_t *transport)
{
    if (!transport) {
        return;
    }
    transport->cancelled = true;
    if (transport->uart_installed) {
        uart_driver_delete(transport->config.uart_port);
    }
    if (transport->lock) {
        vSemaphoreDelete(transport->lock);
    }
    memset(transport, 0, sizeof(*transport));
}

bus_transport_t *rs485_transport_port(rs485_transport_t *transport)
{
    return transport && transport->initialized
               ? bus_transport_controller_port(&transport->controller)
               : NULL;
}

void rs485_transport_cancel(rs485_transport_t *transport)
{
    if (transport) {
        transport->cancelled = true;
    }
}

bool rs485_transport_get_stats(const rs485_transport_t *transport,
                               bus_transport_stats_t *stats)
{
    return transport && transport->initialized
               ? bus_transport_get_stats(&transport->controller.port, stats)
               : false;
}
