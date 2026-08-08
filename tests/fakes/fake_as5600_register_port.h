#ifndef FAKE_AS5600_REGISTER_PORT_H
#define FAKE_AS5600_REGISTER_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "as5600_device.h"

#define FAKE_AS5600_REGISTER_PORT_MAX_EXPECTATIONS 32U
#define FAKE_AS5600_REGISTER_PORT_MAX_BYTES 8U

typedef struct {
    uint8_t device_address;
    uint8_t register_address;
    size_t byte_count;
    uint32_t timeout_ms;
    as5600_device_result_t result;
    uint8_t bytes[FAKE_AS5600_REGISTER_PORT_MAX_BYTES];
} fake_as5600_register_read_expectation_t;

typedef struct {
    fake_as5600_register_read_expectation_t
        expectations[FAKE_AS5600_REGISTER_PORT_MAX_EXPECTATIONS];
    size_t expectation_count;
    size_t next_expectation;
    bool mismatch;
} fake_as5600_register_port_t;

bool fake_as5600_register_port_init(fake_as5600_register_port_t *fake);
bool fake_as5600_register_port_expect_read(
    fake_as5600_register_port_t *fake,
    uint8_t device_address,
    uint8_t register_address,
    size_t byte_count,
    uint32_t timeout_ms,
    as5600_device_result_t result,
    const uint8_t *bytes);
as5600_register_read_port_t
fake_as5600_register_port_as_port(fake_as5600_register_port_t *fake);
bool fake_as5600_register_port_complete(const fake_as5600_register_port_t *fake);

#endif
