#include "i2c_bitbang_transport.h"

#include <string.h>

#include "esp_rom_sys.h"
#include "esp_timer.h"

static bool deadline_expired(int64_t deadline_us)
{
    return deadline_us != 0 && esp_timer_get_time() >= deadline_us;
}

static esp_err_t set_line(const i2c_bitbang_transport_t *transport,
                          gpio_num_t pin,
                          uint32_t level)
{
    if (!transport || gpio_set_level(pin, level) != ESP_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void half_delay(const i2c_bitbang_transport_t *transport)
{
    esp_rom_delay_us(transport->config.half_period_us);
}

static esp_err_t release_scl(const i2c_bitbang_transport_t *transport,
                             int64_t deadline_us)
{
    esp_err_t error = set_line(transport, transport->config.scl_pin, 1U);
    if (error != ESP_OK) {
        return error;
    }
    const int64_t edge_deadline = esp_timer_get_time() +
                                  (int64_t)transport->config.edge_timeout_us;
    while (gpio_get_level(transport->config.scl_pin) == 0) {
        if (esp_timer_get_time() >= edge_deadline ||
            deadline_expired(deadline_us)) {
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(1U);
    }
    return ESP_OK;
}

static void release_lines(const i2c_bitbang_transport_t *transport)
{
    if (!transport) {
        return;
    }
    (void)set_line(transport, transport->config.sda_pin, 1U);
    (void)set_line(transport, transport->config.scl_pin, 1U);
}

static esp_err_t stop_condition(const i2c_bitbang_transport_t *transport,
                                int64_t deadline_us)
{
    esp_err_t error = set_line(transport, transport->config.sda_pin, 0U);
    if (error != ESP_OK) {
        return error;
    }
    half_delay(transport);
    error = release_scl(transport, deadline_us);
    if (error != ESP_OK) {
        return error;
    }
    half_delay(transport);
    error = set_line(transport, transport->config.sda_pin, 1U);
    if (error != ESP_OK) {
        return error;
    }
    half_delay(transport);
    return gpio_get_level(transport->config.scl_pin) != 0 &&
                   gpio_get_level(transport->config.sda_pin) != 0
               ? ESP_OK
               : ESP_FAIL;
}

static esp_err_t recover_bus(const i2c_bitbang_transport_t *transport,
                             int64_t deadline_us)
{
    release_lines(transport);
    esp_err_t error = release_scl(transport, deadline_us);
    if (error != ESP_OK) {
        return error;
    }
    half_delay(transport);
    if (gpio_get_level(transport->config.sda_pin) == 0) {
        for (uint8_t pulse = 0U; pulse < 9U; ++pulse) {
            if (deadline_expired(deadline_us)) {
                return ESP_ERR_TIMEOUT;
            }
            error = set_line(transport, transport->config.scl_pin, 0U);
            if (error != ESP_OK) {
                return error;
            }
            half_delay(transport);
            error = release_scl(transport, deadline_us);
            if (error != ESP_OK) {
                return error;
            }
            half_delay(transport);
        }
        error = stop_condition(transport, deadline_us);
        if (error != ESP_OK) {
            return error;
        }
    }
    return gpio_get_level(transport->config.sda_pin) != 0 &&
                   gpio_get_level(transport->config.scl_pin) != 0
               ? ESP_OK
               : ESP_FAIL;
}

static esp_err_t start_condition(const i2c_bitbang_transport_t *transport,
                                 int64_t deadline_us)
{
    if (deadline_expired(deadline_us)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t error = set_line(transport, transport->config.sda_pin, 1U);
    if (error != ESP_OK) {
        return error;
    }
    half_delay(transport);
    error = release_scl(transport, deadline_us);
    if (error != ESP_OK || gpio_get_level(transport->config.sda_pin) == 0) {
        return error == ESP_OK ? ESP_FAIL : error;
    }
    half_delay(transport);
    error = set_line(transport, transport->config.sda_pin, 0U);
    if (error != ESP_OK) {
        return error;
    }
    half_delay(transport);
    error = set_line(transport, transport->config.scl_pin, 0U);
    if (error != ESP_OK) {
        return error;
    }
    half_delay(transport);
    return ESP_OK;
}

static esp_err_t write_byte(const i2c_bitbang_transport_t *transport,
                            uint8_t value,
                            bool *acknowledged,
                            int64_t deadline_us)
{
    if (!acknowledged) {
        return ESP_ERR_INVALID_ARG;
    }
    *acknowledged = false;
    for (uint8_t mask = 0x80U; mask != 0U; mask >>= 1U) {
        if (deadline_expired(deadline_us)) {
            return ESP_ERR_TIMEOUT;
        }
        esp_err_t error = set_line(transport,
                                   transport->config.sda_pin,
                                   (value & mask) != 0U ? 1U : 0U);
        if (error != ESP_OK) {
            return error;
        }
        half_delay(transport);
        error = release_scl(transport, deadline_us);
        if (error != ESP_OK) {
            return error;
        }
        half_delay(transport);
        error = set_line(transport, transport->config.scl_pin, 0U);
        if (error != ESP_OK) {
            return error;
        }
        half_delay(transport);
    }
    esp_err_t error = set_line(transport, transport->config.sda_pin, 1U);
    if (error != ESP_OK) {
        return error;
    }
    half_delay(transport);
    error = release_scl(transport, deadline_us);
    if (error != ESP_OK) {
        return error;
    }
    half_delay(transport);
    *acknowledged = gpio_get_level(transport->config.sda_pin) == 0;
    error = set_line(transport, transport->config.scl_pin, 0U);
    if (error != ESP_OK) {
        return error;
    }
    half_delay(transport);
    return ESP_OK;
}

static esp_err_t read_byte(const i2c_bitbang_transport_t *transport,
                           uint8_t *value,
                           bool acknowledge_more,
                           int64_t deadline_us)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }
    *value = 0U;
    esp_err_t error = set_line(transport, transport->config.sda_pin, 1U);
    if (error != ESP_OK) {
        return error;
    }
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
        if (deadline_expired(deadline_us)) {
            return ESP_ERR_TIMEOUT;
        }
        half_delay(transport);
        error = release_scl(transport, deadline_us);
        if (error != ESP_OK) {
            return error;
        }
        half_delay(transport);
        *value = (uint8_t)((*value << 1U) |
                           (uint8_t)gpio_get_level(transport->config.sda_pin));
        error = set_line(transport, transport->config.scl_pin, 0U);
        if (error != ESP_OK) {
            return error;
        }
        half_delay(transport);
    }
    error = set_line(transport,
                     transport->config.sda_pin,
                     acknowledge_more ? 0U : 1U);
    if (error != ESP_OK) {
        return error;
    }
    half_delay(transport);
    error = release_scl(transport, deadline_us);
    if (error != ESP_OK) {
        return error;
    }
    half_delay(transport);
    error = set_line(transport, transport->config.scl_pin, 0U);
    if (error != ESP_OK) {
        return error;
    }
    error = set_line(transport, transport->config.sda_pin, 1U);
    if (error != ESP_OK) {
        return error;
    }
    half_delay(transport);
    return ESP_OK;
}

esp_err_t i2c_bitbang_transport_init(
    i2c_bitbang_transport_t *transport,
    const i2c_bitbang_transport_config_t *config)
{
    if (!transport || !config || config->sda_pin < 0 || config->scl_pin < 0 ||
        config->sda_pin == config->scl_pin || config->half_period_us == 0U ||
        config->edge_timeout_us == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(transport, 0, sizeof(*transport));
    transport->config = *config;
    gpio_config_t gpio = {
        .pin_bit_mask = (UINT64_C(1) << config->sda_pin) |
                        (UINT64_C(1) << config->scl_pin),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = config->enable_internal_pullups ? GPIO_PULLUP_ENABLE
                                                       : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t error = gpio_config(&gpio);
    if (error != ESP_OK) {
        return error;
    }
    transport->lock = xSemaphoreCreateMutexStatic(&transport->lock_storage);
    if (!transport->lock) {
        return ESP_ERR_NO_MEM;
    }
    release_lines(transport);
    transport->initialized = true;
    return ESP_OK;
}

void i2c_bitbang_transport_deinit(i2c_bitbang_transport_t *transport)
{
    if (!transport) {
        return;
    }
    if (transport->initialized) {
        release_lines(transport);
        (void)gpio_reset_pin(transport->config.sda_pin);
        (void)gpio_reset_pin(transport->config.scl_pin);
    }
    if (transport->lock) {
        vSemaphoreDelete(transport->lock);
    }
    memset(transport, 0, sizeof(*transport));
}

esp_err_t i2c_bitbang_transport_read_registers(
    i2c_bitbang_transport_t *transport,
    uint8_t address,
    uint8_t first_register,
    uint8_t *data,
    size_t length,
    uint32_t timeout_ms)
{
    if (!transport || !transport->initialized || !data || length == 0U ||
        address == 0U || address > 0x7FU || timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t lock_timeout = transport->config.lock_timeout_ms == 0U ||
                                      transport->config.lock_timeout_ms > timeout_ms
                                  ? timeout_ms
                                  : transport->config.lock_timeout_ms;
    if (xSemaphoreTake(transport->lock, pdMS_TO_TICKS(lock_timeout)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const int64_t deadline_us = esp_timer_get_time() +
                                (int64_t)timeout_ms * 1000LL;
    esp_err_t error = recover_bus(transport, deadline_us);
    bool acknowledged = false;
    if (error == ESP_OK) {
        error = start_condition(transport, deadline_us);
    }
    if (error == ESP_OK) {
        error = write_byte(transport, (uint8_t)(address << 1U), &acknowledged,
                           deadline_us);
        if (error == ESP_OK && !acknowledged) {
            error = ESP_ERR_NOT_FOUND;
        }
    }
    if (error == ESP_OK) {
        error = write_byte(transport, first_register, &acknowledged, deadline_us);
        if (error == ESP_OK && !acknowledged) {
            error = ESP_ERR_NOT_FOUND;
        }
    }
    if (error == ESP_OK) {
        error = start_condition(transport, deadline_us);
    }
    if (error == ESP_OK) {
        error = write_byte(transport,
                           (uint8_t)((address << 1U) | 1U),
                           &acknowledged,
                           deadline_us);
        if (error == ESP_OK && !acknowledged) {
            error = ESP_ERR_NOT_FOUND;
        }
    }
    for (size_t index = 0U; error == ESP_OK && index < length; ++index) {
        error = read_byte(transport, &data[index], index + 1U < length,
                          deadline_us);
    }
    const esp_err_t stop_error = stop_condition(transport, deadline_us);
    if (error == ESP_OK) {
        error = stop_error;
    }
    release_lines(transport);
    xSemaphoreGive(transport->lock);
    return error;
}
