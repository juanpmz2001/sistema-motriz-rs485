#ifndef I2C_BITBANG_TRANSPORT_H
#define I2C_BITBANG_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Deliberately small, register-read-only I2C transport for a bench sensor.
 * It is not an actuator interface and it does not know any sensor register
 * map.  The AS5600 driver owns address/register semantics.
 */
typedef struct {
    gpio_num_t sda_pin;
    gpio_num_t scl_pin;
    uint32_t half_period_us;
    uint32_t edge_timeout_us;
    uint32_t lock_timeout_ms;
    bool enable_internal_pullups;
} i2c_bitbang_transport_config_t;

typedef struct {
    i2c_bitbang_transport_config_t config;
    StaticSemaphore_t lock_storage;
    SemaphoreHandle_t lock;
    bool initialized;
} i2c_bitbang_transport_t;

esp_err_t i2c_bitbang_transport_init(
    i2c_bitbang_transport_t *transport,
    const i2c_bitbang_transport_config_t *config);
void i2c_bitbang_transport_deinit(i2c_bitbang_transport_t *transport);

/* Read `length` bytes beginning at an 8-bit register address from a 7-bit
 * I2C address.  Each transaction attempts recovery first and leaves both
 * lines released on every return path. */
esp_err_t i2c_bitbang_transport_read_registers(
    i2c_bitbang_transport_t *transport,
    uint8_t address,
    uint8_t first_register,
    uint8_t *data,
    size_t length,
    uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
