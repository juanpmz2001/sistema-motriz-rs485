#include "fake_as5600_register_port.h"

#include <string.h>

static as5600_device_result_t fake_as5600_register_read(
    void *context,
    uint8_t device_address,
    uint8_t register_address,
    uint8_t *out_bytes,
    size_t byte_count,
    uint32_t timeout_ms)
{
    fake_as5600_register_port_t *fake = context;
    if (fake == NULL || out_bytes == NULL ||
        fake->next_expectation >= fake->expectation_count) {
        if (fake != NULL) {
            fake->mismatch = true;
        }
        return AS5600_DEVICE_IO_ERROR;
    }

    const fake_as5600_register_read_expectation_t *expected =
        &fake->expectations[fake->next_expectation++];
    if (expected->device_address != device_address ||
        expected->register_address != register_address ||
        expected->byte_count != byte_count || expected->timeout_ms != timeout_ms) {
        fake->mismatch = true;
        return AS5600_DEVICE_IO_ERROR;
    }
    if (expected->result == AS5600_DEVICE_OK) {
        memcpy(out_bytes, expected->bytes, byte_count);
    }
    return expected->result;
}

bool fake_as5600_register_port_init(fake_as5600_register_port_t *fake)
{
    if (fake == NULL) {
        return false;
    }
    memset(fake, 0, sizeof(*fake));
    return true;
}

bool fake_as5600_register_port_expect_read(
    fake_as5600_register_port_t *fake,
    uint8_t device_address,
    uint8_t register_address,
    size_t byte_count,
    uint32_t timeout_ms,
    as5600_device_result_t result,
    const uint8_t *bytes)
{
    if (fake == NULL || byte_count == 0U ||
        byte_count > FAKE_AS5600_REGISTER_PORT_MAX_BYTES ||
        fake->expectation_count >= FAKE_AS5600_REGISTER_PORT_MAX_EXPECTATIONS ||
        (result == AS5600_DEVICE_OK && bytes == NULL)) {
        return false;
    }

    fake_as5600_register_read_expectation_t *expected =
        &fake->expectations[fake->expectation_count++];
    expected->device_address = device_address;
    expected->register_address = register_address;
    expected->byte_count = byte_count;
    expected->timeout_ms = timeout_ms;
    expected->result = result;
    if (bytes != NULL) {
        memcpy(expected->bytes, bytes, byte_count);
    }
    return true;
}

as5600_register_read_port_t
fake_as5600_register_port_as_port(fake_as5600_register_port_t *fake)
{
    return (as5600_register_read_port_t){
        .read = fake_as5600_register_read,
        .context = fake,
    };
}

bool fake_as5600_register_port_complete(const fake_as5600_register_port_t *fake)
{
    return fake != NULL && !fake->mismatch &&
           fake->next_expectation == fake->expectation_count;
}
