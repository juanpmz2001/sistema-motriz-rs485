#include "svd48_protocol.h"

uint16_t svd48_crc16_uumotor(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t pos = 0; pos < length; pos++) {
        crc ^= (uint16_t)data[pos];
        for (int i = 0; i < 8; i++) {
            if (crc & 0x0001) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

bool svd48_frame_has_valid_crc(const uint8_t *frame, size_t length)
{
    if (!frame || length < 4) {
        return false;
    }
    uint16_t expected = svd48_crc16_uumotor(frame, length - 2);
    return frame[length - 2] == (uint8_t)(expected >> 8) &&
           frame[length - 1] == (uint8_t)(expected & 0xFF);
}

static void append_crc(uint8_t *frame, size_t payload_length)
{
    uint16_t crc = svd48_crc16_uumotor(frame, payload_length);
    frame[payload_length] = (uint8_t)(crc >> 8);
    frame[payload_length + 1] = (uint8_t)(crc & 0xFF);
}

bool svd48_write_multiple_range_is_valid(uint16_t start_reg, uint16_t quantity)
{
    if (quantity == 0 || quantity > SVD48_WRITE_MULTIPLE_MAX_REGISTERS) {
        return false;
    }

    return (uint32_t)start_reg + (uint32_t)quantity - 1U <= UINT16_MAX;
}

bool svd48_register_is_runtime_actuation(uint16_t reg)
{
    switch (reg) {
    case 0x5300U:
    case 0x5301U:
    case 0x5304U:
    case 0x5305U:
    case 0x5308U:
    case 0x5309U:
        return true;
    default:
        return false;
    }
}

bool svd48_register_range_has_runtime_actuation(uint16_t start_reg, uint16_t quantity)
{
    if (!svd48_write_multiple_range_is_valid(start_reg, quantity)) {
        return false;
    }

    uint32_t end_reg = (uint32_t)start_reg + (uint32_t)quantity - 1U;
    for (uint32_t reg = start_reg; reg <= end_reg; ++reg) {
        if (svd48_register_is_runtime_actuation((uint16_t)reg)) {
            return true;
        }
    }
    return false;
}

size_t svd48_build_read_request(uint8_t slave_id, uint16_t reg, uint16_t quantity, uint8_t frame[8])
{
    if (!frame || slave_id < SVD48_MODBUS_MIN_SLAVE_ID ||
        slave_id > SVD48_MODBUS_MAX_SLAVE_ID || quantity == 0U ||
        quantity > SVD48_READ_MAX_REGISTERS ||
        (uint32_t)reg + (uint32_t)quantity - 1U > UINT16_MAX) {
        return 0;
    }
    frame[0] = slave_id;
    frame[1] = SVD48_FUNC_READ_HOLDING;
    frame[2] = (uint8_t)(reg >> 8);
    frame[3] = (uint8_t)(reg & 0xFF);
    frame[4] = (uint8_t)(quantity >> 8);
    frame[5] = (uint8_t)(quantity & 0xFF);
    append_crc(frame, 6);
    return 8;
}

size_t svd48_build_write_single_request(uint8_t slave_id, uint16_t reg, uint16_t value, uint8_t frame[8])
{
    if (!frame || slave_id < SVD48_MODBUS_MIN_SLAVE_ID ||
        slave_id > SVD48_MODBUS_MAX_SLAVE_ID) {
        return 0;
    }
    frame[0] = slave_id;
    frame[1] = SVD48_FUNC_WRITE_SINGLE;
    frame[2] = (uint8_t)(reg >> 8);
    frame[3] = (uint8_t)(reg & 0xFF);
    frame[4] = (uint8_t)(value >> 8);
    frame[5] = (uint8_t)(value & 0xFF);
    append_crc(frame, 6);
    return 8;
}

size_t svd48_build_write_multiple_request(uint8_t slave_id,
                                          uint16_t start_reg,
                                          const uint16_t *values,
                                          uint16_t quantity,
                                          uint8_t *frame,
                                          size_t frame_size)
{
    if (!values || !frame || slave_id < SVD48_MODBUS_MIN_SLAVE_ID ||
        slave_id > SVD48_MODBUS_MAX_SLAVE_ID ||
        !svd48_write_multiple_range_is_valid(start_reg, quantity)) {
        return 0;
    }

    size_t required = 9U + (size_t)quantity * 2U;
    if (frame_size < required) {
        return 0;
    }

    frame[0] = slave_id;
    frame[1] = SVD48_FUNC_WRITE_MULTI;
    frame[2] = (uint8_t)(start_reg >> 8);
    frame[3] = (uint8_t)(start_reg & 0xFF);
    frame[4] = (uint8_t)(quantity >> 8);
    frame[5] = (uint8_t)(quantity & 0xFF);
    frame[6] = (uint8_t)(quantity * 2U);
    for (uint16_t i = 0; i < quantity; i++) {
        frame[7 + i * 2] = (uint8_t)(values[i] >> 8);
        frame[8 + i * 2] = (uint8_t)(values[i] & 0xFF);
    }
    append_crc(frame, required - 2);
    return required;
}

bool svd48_parse_write_multiple_response(const uint8_t *frame,
                                         size_t length,
                                         uint8_t expected_slave_id,
                                         uint16_t expected_start_reg,
                                         uint16_t expected_quantity,
                                         svd48_write_multiple_response_t *response)
{
    if (!frame || !response || length != SVD48_WRITE_MULTIPLE_RESPONSE_SIZE ||
        !svd48_write_multiple_range_is_valid(expected_start_reg, expected_quantity) ||
        frame[0] != expected_slave_id || frame[1] != SVD48_FUNC_WRITE_MULTI ||
        !svd48_frame_has_valid_crc(frame, length)) {
        return false;
    }

    uint16_t start_register = ((uint16_t)frame[2] << 8) | frame[3];
    uint16_t quantity = ((uint16_t)frame[4] << 8) | frame[5];
    if (start_register != expected_start_reg || quantity != expected_quantity) {
        return false;
    }

    response->start_register = start_register;
    response->quantity = quantity;
    return true;
}

bool svd48_parse_exception_response(const uint8_t *frame,
                                    size_t length,
                                    uint8_t expected_slave_id,
                                    uint8_t request_function,
                                    svd48_exception_response_t *exception)
{
    if (!frame || !exception || length != 5 || frame[0] != expected_slave_id ||
        frame[1] != (uint8_t)(request_function | 0x80U) || !svd48_frame_has_valid_crc(frame, length)) {
        return false;
    }

    exception->function = frame[1];
    exception->code = frame[2];
    return true;
}
