#ifndef SVD48_PROTOCOL_H
#define SVD48_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SVD48_FUNC_READ_HOLDING 0x03
#define SVD48_FUNC_WRITE_SINGLE 0x06
#define SVD48_FUNC_WRITE_MULTI 0x10

#define SVD48_MODBUS_MIN_SLAVE_ID 1U
#define SVD48_MODBUS_MAX_SLAVE_ID 247U
#define SVD48_READ_MAX_REGISTERS 125U
#define SVD48_WRITE_MULTIPLE_MAX_REGISTERS 123U
#define SVD48_WRITE_MULTIPLE_RESPONSE_SIZE 8U
#define SVD48_WRITE_MULTIPLE_REQUEST_MAX_SIZE \
    (9U + SVD48_WRITE_MULTIPLE_MAX_REGISTERS * 2U)

typedef struct {
    uint8_t function;
    uint8_t code;
} svd48_exception_response_t;

typedef struct {
    uint16_t start_register;
    uint16_t quantity;
} svd48_write_multiple_response_t;

uint16_t svd48_crc16_uumotor(const uint8_t *data, size_t length);
bool svd48_frame_has_valid_crc(const uint8_t *frame, size_t length);
size_t svd48_build_read_request(uint8_t slave_id, uint16_t reg, uint16_t quantity, uint8_t frame[8]);
size_t svd48_build_write_single_request(uint8_t slave_id, uint16_t reg, uint16_t value, uint8_t frame[8]);
size_t svd48_build_write_multiple_request(uint8_t slave_id,
                                          uint16_t start_reg,
                                          const uint16_t *values,
                                          uint16_t quantity,
                                          uint8_t *frame,
                                          size_t frame_size);
bool svd48_write_multiple_range_is_valid(uint16_t start_reg, uint16_t quantity);
bool svd48_register_is_runtime_actuation(uint16_t reg);
bool svd48_register_range_has_runtime_actuation(uint16_t start_reg, uint16_t quantity);
bool svd48_parse_write_multiple_response(const uint8_t *frame,
                                         size_t length,
                                         uint8_t expected_slave_id,
                                         uint16_t expected_start_reg,
                                         uint16_t expected_quantity,
                                         svd48_write_multiple_response_t *response);
bool svd48_parse_exception_response(const uint8_t *frame,
                                    size_t length,
                                    uint8_t expected_slave_id,
                                    uint8_t request_function,
                                    svd48_exception_response_t *exception);

#ifdef __cplusplus
}
#endif

#endif // SVD48_PROTOCOL_H
