#include "svd48_device.h"

#include <string.h>

#define SVD48_DEFAULT_RESPONSE_TIMEOUT_MS 100U
#define SVD48_DEFAULT_STALE_TIMEOUT_MS 1000U
#define SVD48_POLL_SLOW_DIVIDER 20U

#define REG_M1_STATUS 0x5400U
#define REG_M1_MOTOR_TEMP 0x5404U
#define REG_M1_MOS_TEMP 0x5408U
#define REG_M1_BUS_VOLTAGE 0x540CU
#define REG_M1_ACTUAL_SPEED 0x5410U
#define REG_M1_ACTUAL_CURRENT 0x5414U
#define REG_M1_POSITION 0x5418U
#define REG_M1_ERROR_CODE 0x5420U

#define REG_M1_CONTROL_CMD 0x5300U
#define REG_M2_CONTROL_CMD 0x5301U
#define REG_M1_GIVEN_SPEED 0x5304U
#define REG_M2_GIVEN_SPEED 0x5305U
#define REG_M1_GIVEN_CURRENT 0x5308U
#define REG_M2_GIVEN_CURRENT 0x5309U

/* Reviewed SVD48V V2.0 Hall calibration registers. The command trigger and
 * its result/status register are distinct one-shot semantics. */
#define REG_M1_HALL_CALIBRATION_TRIGGER 0x5600U
#define REG_M2_HALL_CALIBRATION_TRIGGER 0x5601U
#define REG_M1_HALL_CALIBRATION_STATUS 0x5684U
#define REG_M2_HALL_CALIBRATION_STATUS 0x5685U
#define SVD48_HALL_CALIBRATION_TRIGGER_VALUE 1U

#define SVD48_CMD_STOP 0U
#define SVD48_CMD_START 1U
#define SVD48_CMD_CLEAR_ALARM 2U

typedef enum {
    PAIR_STATUS,
    PAIR_MOTOR_TEMP,
    PAIR_MOS_TEMP,
    PAIR_BUS_VOLTAGE,
    PAIR_SPEED,
    PAIR_CURRENT,
} pair_field_t;

static uint32_t now_ms(const svd48_device_t *device)
{
    return device && device->config.clock_ms
               ? device->config.clock_ms(device->config.clock_context)
               : 0;
}

static bool lock_state(svd48_device_t *device)
{
    return !device->config.state_lock.acquire ||
           device->config.state_lock.acquire(device->config.state_lock.context);
}

static void unlock_state(svd48_device_t *device)
{
    if (device->config.state_lock.release) {
        device->config.state_lock.release(device->config.state_lock.context);
    }
}

static svd48_device_result_t map_transport_result(bus_transport_result_t result)
{
    switch (result) {
    case BUS_TRANSPORT_OK:
        return SVD48_DEVICE_OK;
    case BUS_TRANSPORT_INVALID_ARGUMENT:
        return SVD48_DEVICE_INVALID_ARGUMENT;
    case BUS_TRANSPORT_TIMEOUT:
        return SVD48_DEVICE_TIMEOUT;
    case BUS_TRANSPORT_BUSY:
        return SVD48_DEVICE_BUS_BUSY;
    case BUS_TRANSPORT_IO_ERROR:
        return SVD48_DEVICE_IO_ERROR;
    case BUS_TRANSPORT_INCOMPLETE:
        return SVD48_DEVICE_INCOMPLETE_FRAME;
    case BUS_TRANSPORT_CANCELLED:
        return SVD48_DEVICE_CANCELLED;
    default:
        return SVD48_DEVICE_IO_ERROR;
    }
}

static void record_communication(svd48_device_t *device,
                                 svd48_device_result_t result)
{
    if (!lock_state(device)) {
        return;
    }
    uint32_t timestamp = now_ms(device);
    device->communication.transactions++;
    device->communication.last_error = result;
    if (result == SVD48_DEVICE_OK) {
        device->communication.successful_transactions++;
        device->communication.consecutive_failures = 0;
        device->communication.last_success_ms = timestamp;
    } else {
        device->communication.failed_transactions++;
        device->communication.consecutive_failures++;
        device->communication.last_failure_ms = timestamp;
    }
    unlock_state(device);
}

static void record_exception(svd48_device_t *device,
                             const svd48_exception_response_t *exception)
{
    if (!exception || !lock_state(device)) {
        return;
    }
    uint32_t timestamp = now_ms(device);
    device->communication.last_error = SVD48_DEVICE_EXCEPTION;
    device->communication.last_exception_function = exception->function;
    device->communication.last_exception_code = exception->code;
    for (size_t index = 0; index < SVD48_DEVICE_CHANNEL_COUNT; ++index) {
        svd48_channel_snapshot_t *snapshot = &device->snapshots[index];
        snapshot->last_error = SVD48_DEVICE_EXCEPTION;
        snapshot->last_exception_function = exception->function;
        snapshot->last_exception_code = exception->code;
        snapshot->last_exception_ms = timestamp;
    }
    unlock_state(device);
}

typedef bool (*response_validator_fn)(const uint8_t *response,
                                      size_t response_length,
                                      const void *context);

typedef struct {
    uint8_t address;
    uint16_t quantity;
} read_response_context_t;

typedef struct {
    const uint8_t *request;
    size_t request_length;
} write_single_response_context_t;

typedef struct {
    uint8_t address;
    uint16_t start_reg;
    uint16_t quantity;
} write_multiple_response_context_t;

static bool validate_read_response(const uint8_t *response,
                                   size_t response_length,
                                   const void *context)
{
    const read_response_context_t *read_context = context;
    return response && read_context &&
           response_length == 5U + (size_t)read_context->quantity * 2U &&
           response[0] == read_context->address &&
           response[1] == SVD48_FUNC_READ_HOLDING &&
           response[2] == read_context->quantity * 2U;
}

static bool validate_write_single_response(const uint8_t *response,
                                            size_t response_length,
                                            const void *context)
{
    const write_single_response_context_t *write_context = context;
    return response && write_context && write_context->request &&
           response_length == write_context->request_length &&
           memcmp(response,
                  write_context->request,
                  write_context->request_length) == 0;
}

static bool validate_write_multiple_response(const uint8_t *response,
                                              size_t response_length,
                                              const void *context)
{
    const write_multiple_response_context_t *write_context = context;
    svd48_write_multiple_response_t acknowledgement;
    return write_context &&
           svd48_parse_write_multiple_response(response,
                                                response_length,
                                                write_context->address,
                                                write_context->start_reg,
                                                write_context->quantity,
                                                &acknowledgement);
}

static bool result_is_retryable(svd48_device_result_t result)
{
    switch (result) {
    case SVD48_DEVICE_TIMEOUT:
    case SVD48_DEVICE_BUS_BUSY:
    case SVD48_DEVICE_IO_ERROR:
    case SVD48_DEVICE_INCOMPLETE_FRAME:
    case SVD48_DEVICE_CRC_ERROR:
    case SVD48_DEVICE_BAD_RESPONSE:
        return true;
    default:
        return false;
    }
}

static svd48_device_result_t transact(svd48_device_t *device,
                                      const uint8_t *request,
                                      size_t request_length,
                                      uint8_t *response,
                                      size_t response_capacity,
                                      size_t *response_length,
                                      uint8_t retries,
                                      response_validator_fn validator,
                                      const void *validator_context)
{
    if (!device || !device->initialized || !request || !response ||
        !response_length || !validator || retries > SVD48_DEVICE_MAX_RETRIES) {
        return SVD48_DEVICE_INVALID_ARGUMENT;
    }
    svd48_device_result_t last_result = SVD48_DEVICE_IO_ERROR;
    for (uint16_t attempt = 0; attempt <= retries; ++attempt) {
        *response_length = 0;
        bus_transport_result_t transport_result = bus_transport_transact(
            device->config.transport,
            request,
            request_length,
            response,
            response_capacity,
            response_length,
            device->config.response_timeout_ms);
        last_result = map_transport_result(transport_result);

        bool valid_crc = *response_length >= 5U &&
                         svd48_frame_has_valid_crc(response, *response_length);
        if (!valid_crc && transport_result == BUS_TRANSPORT_OK) {
            last_result = SVD48_DEVICE_CRC_ERROR;
        } else if (*response_length > 0U && !valid_crc &&
                   transport_result == BUS_TRANSPORT_INCOMPLETE) {
            last_result = SVD48_DEVICE_CRC_ERROR;
        }
        svd48_exception_response_t exception = {0};
        bool is_exception = last_result == SVD48_DEVICE_OK && valid_crc &&
                            svd48_parse_exception_response(response,
                                                           *response_length,
                                                           device->config.address,
                                                           request[1],
                                                           &exception);
        if (is_exception) {
            last_result = SVD48_DEVICE_EXCEPTION;
        } else if (last_result == SVD48_DEVICE_OK && valid_crc &&
                   !validator(response, *response_length, validator_context)) {
            last_result = SVD48_DEVICE_BAD_RESPONSE;
        }
        record_communication(device, last_result);
        if (is_exception) {
            record_exception(device, &exception);
        }
        if (device->trace_enabled && device->trace) {
            device->trace(device->trace_context,
                          device->config.device_id,
                          device->config.address,
                          (uint8_t)(attempt + 1U),
                          request,
                          request_length,
                          response,
                          *response_length,
                          last_result);
        }
        if (last_result == SVD48_DEVICE_OK && valid_crc) {
            return SVD48_DEVICE_OK;
        }
        if (!result_is_retryable(last_result)) {
            return last_result;
        }
    }
    return last_result;
}

static svd48_device_result_t read_registers_with_retries(svd48_device_t *device,
                                                         uint16_t reg,
                                                         uint16_t quantity,
                                                         uint16_t *out_regs,
                                                         uint8_t retries)
{
    if (!device || !device->initialized || quantity == 0U || quantity > 16U ||
        !out_regs) {
        return SVD48_DEVICE_INVALID_ARGUMENT;
    }
    uint8_t request[8];
    uint8_t response[64];
    size_t response_length = 0;
    size_t request_length = svd48_build_read_request(device->config.address,
                                                     reg,
                                                     quantity,
                                                     request);
    if (request_length == 0U) {
        return SVD48_DEVICE_INVALID_ARGUMENT;
    }
    const read_response_context_t response_context = {
        .address = device->config.address,
        .quantity = quantity,
    };
    svd48_device_result_t result = transact(device,
                                            request,
                                            request_length,
                                            response,
                                            sizeof(response),
                                            &response_length,
                                            retries,
                                            validate_read_response,
                                            &response_context);
    if (result != SVD48_DEVICE_OK) {
        return result;
    }
    for (uint16_t index = 0; index < quantity; ++index) {
        out_regs[index] = ((uint16_t)response[3U + index * 2U] << 8U) |
                          response[4U + index * 2U];
    }
    return SVD48_DEVICE_OK;
}

static svd48_device_result_t write_register_raw(svd48_device_t *device,
                                                uint16_t reg,
                                                uint16_t value,
                                                uint8_t retries)
{
    if (!device || !device->initialized) {
        return SVD48_DEVICE_INVALID_ARGUMENT;
    }
    uint8_t request[8];
    uint8_t response[16];
    size_t response_length = 0;
    svd48_build_write_single_request(device->config.address, reg, value, request);
    const write_single_response_context_t response_context = {
        .request = request,
        .request_length = sizeof(request),
    };
    svd48_device_result_t result = transact(device,
                                            request,
                                            sizeof(request),
                                            response,
                                            sizeof(response),
                                            &response_length,
                                            retries,
                                            validate_write_single_response,
                                            &response_context);
    return result;
}

static void update_pair_i16(svd48_device_t *device,
                            pair_field_t field,
                            const uint16_t values[2])
{
    if (!lock_state(device)) {
        return;
    }
    uint32_t timestamp = now_ms(device);
    uint32_t observation = 0U;
    size_t observation_index = 0U;
    switch (field) {
    case PAIR_STATUS:
        observation = SVD48_OBSERVATION_STATUS;
        observation_index = SVD48_OBSERVATION_INDEX_STATUS;
        break;
    case PAIR_MOTOR_TEMP:
        observation = SVD48_OBSERVATION_MOTOR_TEMP;
        observation_index = SVD48_OBSERVATION_INDEX_MOTOR_TEMP;
        break;
    case PAIR_MOS_TEMP:
        observation = SVD48_OBSERVATION_MOS_TEMP;
        observation_index = SVD48_OBSERVATION_INDEX_MOS_TEMP;
        break;
    case PAIR_BUS_VOLTAGE:
        observation = SVD48_OBSERVATION_BUS_VOLTAGE;
        observation_index = SVD48_OBSERVATION_INDEX_BUS_VOLTAGE;
        break;
    case PAIR_SPEED:
        observation = SVD48_OBSERVATION_SPEED;
        observation_index = SVD48_OBSERVATION_INDEX_SPEED;
        break;
    case PAIR_CURRENT:
        observation = SVD48_OBSERVATION_CURRENT;
        observation_index = SVD48_OBSERVATION_INDEX_CURRENT;
        break;
    }
    for (size_t index = 0; index < SVD48_DEVICE_CHANNEL_COUNT; ++index) {
        svd48_channel_snapshot_t *snapshot = &device->snapshots[index];
        int16_t value = (int16_t)values[index];
        switch (field) {
        case PAIR_STATUS:
            snapshot->status = value;
            break;
        case PAIR_MOTOR_TEMP:
            snapshot->motor_temp_decic = value;
            break;
        case PAIR_MOS_TEMP:
            snapshot->mos_temp_decic = value;
            break;
        case PAIR_BUS_VOLTAGE:
            snapshot->bus_voltage_deciv = value;
            break;
        case PAIR_SPEED:
            snapshot->observed_speed_rpm = value;
            break;
        case PAIR_CURRENT:
            snapshot->current_deciamp = value;
            break;
        }
        snapshot->valid_observations |= observation;
        snapshot->failed_observations &= ~observation;
        snapshot->observation_update_ms[observation_index] = timestamp;
        snapshot->last_update_ms = timestamp;
    }
    unlock_state(device);
}

static void update_pair_i32(svd48_device_t *device,
                            const uint16_t values[4],
                            bool error_code)
{
    if (!lock_state(device)) {
        return;
    }
    uint32_t timestamp = now_ms(device);
    const uint32_t observation = error_code ? SVD48_OBSERVATION_ERROR_CODE
                                            : SVD48_OBSERVATION_POSITION;
    const size_t observation_index =
        error_code ? SVD48_OBSERVATION_INDEX_ERROR_CODE
                   : SVD48_OBSERVATION_INDEX_POSITION;
    for (size_t index = 0; index < SVD48_DEVICE_CHANNEL_COUNT; ++index) {
        uint32_t raw = ((uint32_t)values[index * 2U] << 16U) |
                       values[index * 2U + 1U];
        svd48_channel_snapshot_t *snapshot = &device->snapshots[index];
        if (error_code) {
            snapshot->error_code = raw;
        } else {
            snapshot->position_counts = (int32_t)raw;
        }
        snapshot->valid_observations |= observation;
        snapshot->failed_observations &= ~observation;
        snapshot->observation_update_ms[observation_index] = timestamp;
        snapshot->last_update_ms = timestamp;
    }
    unlock_state(device);
}

static void mark_observation_failure(svd48_device_t *device,
                                     uint32_t observation)
{
    if (!lock_state(device)) {
        return;
    }
    for (size_t index = 0; index < SVD48_DEVICE_CHANNEL_COUNT; ++index) {
        device->snapshots[index].failed_observations |= observation;
    }
    unlock_state(device);
}

static void finish_poll(svd48_device_t *device,
                        svd48_device_result_t poll_result,
                        svd48_device_result_t first_error)
{
    if (!lock_state(device)) {
        return;
    }
    const uint32_t timestamp = now_ms(device);
    for (size_t index = 0; index < SVD48_DEVICE_CHANNEL_COUNT; ++index) {
        svd48_channel_snapshot_t *snapshot = &device->snapshots[index];
        snapshot->last_poll_ms = timestamp;
        snapshot->last_poll_result = poll_result;
        snapshot->last_error = poll_result == SVD48_DEVICE_OK
                                   ? SVD48_DEVICE_OK
                                   : first_error;
    }
    device->poll_in_progress = false;
    unlock_state(device);
}

static bool begin_poll(svd48_device_t *device, bool *slow_poll)
{
    if (!device || !slow_poll || !lock_state(device)) {
        return false;
    }
    if (device->poll_in_progress) {
        unlock_state(device);
        return false;
    }
    device->poll_in_progress = true;
    *slow_poll = (device->poll_count++ % SVD48_POLL_SLOW_DIVIDER) == 0U;
    unlock_state(device);
    return true;
}

bool svd48_device_init(svd48_device_t *device,
                       const svd48_device_config_t *config)
{
    if (!device || !config || config->device_id == 0U || config->address == 0U ||
        config->address > SVD48_MODBUS_MAX_SLAVE_ID ||
        config->retries > SVD48_DEVICE_MAX_RETRIES ||
        !config->transport || !config->state_lock.acquire ||
        !config->state_lock.release || !config->clock_ms) {
        return false;
    }
    memset(device, 0, sizeof(*device));
    device->config = *config;
    if (device->config.response_timeout_ms == 0U) {
        device->config.response_timeout_ms = SVD48_DEFAULT_RESPONSE_TIMEOUT_MS;
    }
    if (device->config.stale_timeout_ms == 0U) {
        device->config.stale_timeout_ms = SVD48_DEFAULT_STALE_TIMEOUT_MS;
    }
    for (size_t index = 0; index < SVD48_DEVICE_CHANNEL_COUNT; ++index) {
        device->channels[index].device = device;
        device->channels[index].id = (svd48_channel_id_t)index;
        device->snapshots[index].stale = true;
        device->snapshots[index].last_error = SVD48_DEVICE_TIMEOUT;
        device->snapshots[index].last_poll_result = SVD48_DEVICE_TIMEOUT;
        device->snapshots[index].stale_observations = SVD48_OBSERVATION_ALL;
    }
    device->communication.last_error = SVD48_DEVICE_TIMEOUT;
    device->initialized = true;
    return true;
}

void svd48_device_deinit(svd48_device_t *device)
{
    if (device) {
        memset(device, 0, sizeof(*device));
    }
}

uint16_t svd48_device_id(const svd48_device_t *device)
{
    return device && device->initialized ? device->config.device_id : 0U;
}

uint8_t svd48_device_address(const svd48_device_t *device)
{
    return device && device->initialized ? device->config.address : 0U;
}

bus_transport_t *svd48_device_transport(const svd48_device_t *device)
{
    return device && device->initialized ? device->config.transport : NULL;
}

svd48_channel_t *svd48_device_channel(svd48_device_t *device,
                                      svd48_channel_id_t channel)
{
    return device && device->initialized && channel < SVD48_DEVICE_CHANNEL_COUNT
               ? &device->channels[channel]
               : NULL;
}

uint16_t svd48_channel_control_register(svd48_channel_id_t channel)
{
    return channel == SVD48_CHANNEL_M1 ? REG_M1_CONTROL_CMD
                                       : channel == SVD48_CHANNEL_M2
                                             ? REG_M2_CONTROL_CMD
                                             : 0U;
}

uint16_t svd48_channel_velocity_register(svd48_channel_id_t channel)
{
    return channel == SVD48_CHANNEL_M1 ? REG_M1_GIVEN_SPEED
                                       : channel == SVD48_CHANNEL_M2
                                             ? REG_M2_GIVEN_SPEED
                                             : 0U;
}

uint16_t svd48_channel_current_register(svd48_channel_id_t channel)
{
    return channel == SVD48_CHANNEL_M1 ? REG_M1_GIVEN_CURRENT
                                       : channel == SVD48_CHANNEL_M2
                                             ? REG_M2_GIVEN_CURRENT
                                             : 0U;
}

uint16_t svd48_channel_hall_calibration_trigger_register(
    svd48_channel_id_t channel)
{
    return channel == SVD48_CHANNEL_M1 ? REG_M1_HALL_CALIBRATION_TRIGGER
                                       : channel == SVD48_CHANNEL_M2
                                             ? REG_M2_HALL_CALIBRATION_TRIGGER
                                             : 0U;
}

uint16_t svd48_channel_hall_calibration_status_register(
    svd48_channel_id_t channel)
{
    return channel == SVD48_CHANNEL_M1 ? REG_M1_HALL_CALIBRATION_STATUS
                                       : channel == SVD48_CHANNEL_M2
                                             ? REG_M2_HALL_CALIBRATION_STATUS
                                             : 0U;
}

static bool channel_valid(const svd48_channel_t *channel)
{
    return channel && channel->device && channel->device->initialized &&
           channel->id < SVD48_DEVICE_CHANNEL_COUNT;
}

svd48_device_result_t svd48_channel_set_target_rpm(svd48_channel_t *channel,
                                                   int16_t target_rpm)
{
    return channel_valid(channel)
               ? write_register_raw(channel->device,
                                    svd48_channel_velocity_register(channel->id),
                                    (uint16_t)target_rpm,
                                    channel->device->config.retries)
               : SVD48_DEVICE_INVALID_ARGUMENT;
}

svd48_device_result_t svd48_channel_enable(svd48_channel_t *channel)
{
    return channel_valid(channel)
               ? write_register_raw(channel->device,
                                    svd48_channel_control_register(channel->id),
                                    SVD48_CMD_START,
                                    channel->device->config.retries)
               : SVD48_DEVICE_INVALID_ARGUMENT;
}

svd48_device_result_t svd48_channel_stop(svd48_channel_t *channel)
{
    if (!channel_valid(channel)) {
        return SVD48_DEVICE_INVALID_ARGUMENT;
    }
    svd48_device_result_t speed_result = svd48_channel_set_target_rpm(channel, 0);
    svd48_device_result_t stop_result = write_register_raw(
        channel->device,
        svd48_channel_control_register(channel->id),
        SVD48_CMD_STOP,
        channel->device->config.retries);
    return speed_result != SVD48_DEVICE_OK ? speed_result : stop_result;
}

svd48_device_result_t svd48_channel_clear_fault(svd48_channel_t *channel)
{
    return channel_valid(channel)
               ? write_register_raw(channel->device,
                                    svd48_channel_control_register(channel->id),
                                    SVD48_CMD_CLEAR_ALARM,
                                    channel->device->config.retries)
               : SVD48_DEVICE_INVALID_ARGUMENT;
}

svd48_device_result_t svd48_channel_set_current_deciamp(svd48_channel_t *channel,
                                                       int16_t deciamp)
{
    return channel_valid(channel)
               ? write_register_raw(channel->device,
                                    svd48_channel_current_register(channel->id),
                                    (uint16_t)deciamp,
                                    channel->device->config.retries)
               : SVD48_DEVICE_INVALID_ARGUMENT;
}

static svd48_hall_calibration_status_t hall_calibration_status_from_value(
    uint16_t value)
{
    switch (value) {
    case 0U:
        return SVD48_HALL_CALIBRATION_STATUS_SUCCESS;
    case 1U:
        return SVD48_HALL_CALIBRATION_STATUS_CALIBRATING;
    case 2U:
        return SVD48_HALL_CALIBRATION_STATUS_FAILED;
    default:
        return SVD48_HALL_CALIBRATION_STATUS_UNKNOWN;
    }
}

const char *svd48_hall_calibration_status_name(
    svd48_hall_calibration_status_t status)
{
    switch (status) {
    case SVD48_HALL_CALIBRATION_STATUS_SUCCESS:
        return "SUCCESS";
    case SVD48_HALL_CALIBRATION_STATUS_CALIBRATING:
        return "CALIBRATING";
    case SVD48_HALL_CALIBRATION_STATUS_FAILED:
        return "FAILED";
    case SVD48_HALL_CALIBRATION_STATUS_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

svd48_device_result_t svd48_channel_start_hall_calibration(
    svd48_channel_t *channel,
    svd48_hall_calibration_result_t *result)
{
    if (!channel_valid(channel) || !result) {
        return SVD48_DEVICE_INVALID_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    result->trigger_register =
        svd48_channel_hall_calibration_trigger_register(channel->id);
    result->status_register =
        svd48_channel_hall_calibration_status_register(channel->id);
    result->status = SVD48_HALL_CALIBRATION_STATUS_UNKNOWN;
    result->status_read_result = SVD48_DEVICE_INVALID_ARGUMENT;
    if (result->trigger_register == 0U || result->status_register == 0U) {
        return SVD48_DEVICE_INVALID_ARGUMENT;
    }

    /* Do not retry a one-shot calibration trigger after an unknown transport
     * outcome. The acknowledgement is the only proof that it was accepted. */
    svd48_device_result_t write_result = write_register_raw(
        channel->device,
        result->trigger_register,
        SVD48_HALL_CALIBRATION_TRIGGER_VALUE,
        0U);
    if (write_result != SVD48_DEVICE_OK) {
        return write_result;
    }
    result->write_acknowledged = true;

    uint16_t raw_status = 0U;
    result->status_read_result = read_registers_with_retries(
        channel->device,
        result->status_register,
        1U,
        &raw_status,
        channel->device->config.retries);
    if (result->status_read_result != SVD48_DEVICE_OK) {
        return SVD48_DEVICE_PARTIAL;
    }
    result->status_available = true;
    result->status_value = raw_status;
    result->status = hall_calibration_status_from_value(raw_status);
    return SVD48_DEVICE_OK;
}

bool svd48_channel_get_snapshot(svd48_channel_t *channel,
                                svd48_channel_snapshot_t *snapshot)
{
    if (!channel_valid(channel) || !snapshot || !lock_state(channel->device)) {
        return false;
    }
    *snapshot = channel->device->snapshots[channel->id];
    uint32_t timestamp = now_ms(channel->device);
    snapshot->stale_observations = 0U;
    for (size_t index = 0; index < SVD48_OBSERVATION_COUNT; ++index) {
        const uint32_t observation = 1U << index;
        const bool valid = (snapshot->valid_observations & observation) != 0U;
        const uint32_t age = timestamp - snapshot->observation_update_ms[index];
        if (!valid || age > channel->device->config.stale_timeout_ms) {
            snapshot->stale_observations |= observation;
        }
    }
    snapshot->stale = snapshot->stale_observations != 0U;
    snapshot->online = channel->device->communication.successful_transactions > 0U &&
                       timestamp -
                               channel->device->communication.last_success_ms <=
                           channel->device->config.stale_timeout_ms;
    unlock_state(channel->device);
    return true;
}

svd48_channel_health_t svd48_channel_health_from_snapshot(
    const svd48_channel_snapshot_t *snapshot)
{
    if (!snapshot) {
        return SVD48_CHANNEL_HEALTH_UNKNOWN;
    }
    if (!snapshot->online) {
        return SVD48_CHANNEL_HEALTH_OFFLINE;
    }
    if ((snapshot->valid_observations & SVD48_OBSERVATION_ERROR_CODE) != 0U &&
        (snapshot->stale_observations & SVD48_OBSERVATION_ERROR_CODE) == 0U &&
        snapshot->error_code != 0U) {
        return SVD48_CHANNEL_HEALTH_FAULT;
    }
    if ((snapshot->valid_observations &
         SVD48_OBSERVATION_VELOCITY_COMMUNICATION) !=
            SVD48_OBSERVATION_VELOCITY_COMMUNICATION ||
        (snapshot->stale_observations &
         SVD48_OBSERVATION_VELOCITY_COMMUNICATION) != 0U) {
        return SVD48_CHANNEL_HEALTH_STALE;
    }
    if ((snapshot->failed_observations &
         SVD48_OBSERVATION_VELOCITY_COMMUNICATION) != 0U) {
        return SVD48_CHANNEL_HEALTH_DEGRADED;
    }
    return SVD48_CHANNEL_HEALTH_HEALTHY;
}

svd48_channel_health_t svd48_channel_get_health(svd48_channel_t *channel)
{
    svd48_channel_snapshot_t snapshot;
    return svd48_channel_get_snapshot(channel, &snapshot)
               ? svd48_channel_health_from_snapshot(&snapshot)
               : SVD48_CHANNEL_HEALTH_UNKNOWN;
}

svd48_device_result_t svd48_device_poll(svd48_device_t *device)
{
    if (!device || !device->initialized) {
        return SVD48_DEVICE_INVALID_ARGUMENT;
    }
    uint16_t values2[2];
    uint16_t values4[4];
    size_t successful_reads = 0U;
    size_t failed_reads = 0U;
    svd48_device_result_t first_error = SVD48_DEVICE_OK;
    bool slow_poll = false;
    if (!begin_poll(device, &slow_poll)) {
        return SVD48_DEVICE_BUS_BUSY;
    }
    svd48_device_result_t result = read_registers_with_retries(
        device, REG_M1_POSITION, 4, values4, 0);
    if (result == SVD48_DEVICE_OK) {
        update_pair_i32(device, values4, false);
        successful_reads++;
    } else {
        mark_observation_failure(device, SVD48_OBSERVATION_POSITION);
        first_error = result;
        failed_reads++;
    }
    result = read_registers_with_retries(device,
                                         REG_M1_ACTUAL_SPEED,
                                         2,
                                         values2,
                                         0);
    if (result == SVD48_DEVICE_OK) {
        update_pair_i16(device, PAIR_SPEED, values2);
        successful_reads++;
    } else {
        mark_observation_failure(device, SVD48_OBSERVATION_SPEED);
        if (first_error == SVD48_DEVICE_OK) {
            first_error = result;
        }
        failed_reads++;
    }
    result = read_registers_with_retries(device,
                                         REG_M1_ACTUAL_CURRENT,
                                         2,
                                         values2,
                                         0);
    if (result == SVD48_DEVICE_OK) {
        update_pair_i16(device, PAIR_CURRENT, values2);
        successful_reads++;
    } else {
        mark_observation_failure(device, SVD48_OBSERVATION_CURRENT);
        if (first_error == SVD48_DEVICE_OK) {
            first_error = result;
        }
        failed_reads++;
    }
    if (slow_poll) {
        result = read_registers_with_retries(
            device, REG_M1_STATUS, 2, values2, 0);
        if (result == SVD48_DEVICE_OK) {
            update_pair_i16(device, PAIR_STATUS, values2);
            successful_reads++;
        } else {
            mark_observation_failure(device, SVD48_OBSERVATION_STATUS);
            if (first_error == SVD48_DEVICE_OK) {
                first_error = result;
            }
            failed_reads++;
        }
        result = read_registers_with_retries(device,
                                             REG_M1_MOTOR_TEMP,
                                             2,
                                             values2,
                                             0);
        if (result == SVD48_DEVICE_OK) {
            update_pair_i16(device, PAIR_MOTOR_TEMP, values2);
            successful_reads++;
        } else {
            mark_observation_failure(device, SVD48_OBSERVATION_MOTOR_TEMP);
            if (first_error == SVD48_DEVICE_OK) {
                first_error = result;
            }
            failed_reads++;
        }
        result = read_registers_with_retries(device,
                                             REG_M1_BUS_VOLTAGE,
                                             2,
                                             values2,
                                             0);
        if (result == SVD48_DEVICE_OK) {
            update_pair_i16(device, PAIR_BUS_VOLTAGE, values2);
            successful_reads++;
        } else {
            mark_observation_failure(device, SVD48_OBSERVATION_BUS_VOLTAGE);
            if (first_error == SVD48_DEVICE_OK) {
                first_error = result;
            }
            failed_reads++;
        }
        result = read_registers_with_retries(device,
                                             REG_M1_MOS_TEMP,
                                             2,
                                             values2,
                                             0);
        if (result == SVD48_DEVICE_OK) {
            update_pair_i16(device, PAIR_MOS_TEMP, values2);
            successful_reads++;
        } else {
            mark_observation_failure(device, SVD48_OBSERVATION_MOS_TEMP);
            if (first_error == SVD48_DEVICE_OK) {
                first_error = result;
            }
            failed_reads++;
        }
        result = read_registers_with_retries(device,
                                             REG_M1_ERROR_CODE,
                                             4,
                                             values4,
                                             0);
        if (result == SVD48_DEVICE_OK) {
            update_pair_i32(device, values4, true);
            successful_reads++;
        } else {
            mark_observation_failure(device, SVD48_OBSERVATION_ERROR_CODE);
            if (first_error == SVD48_DEVICE_OK) {
                first_error = result;
            }
            failed_reads++;
        }
    }
    svd48_device_result_t poll_result = SVD48_DEVICE_OK;
    if (failed_reads > 0U) {
        poll_result = successful_reads > 0U ? SVD48_DEVICE_PARTIAL : first_error;
    }
    finish_poll(device, poll_result, first_error);
    return poll_result;
}

svd48_device_result_t svd48_device_read_registers(svd48_device_t *device,
                                                  uint16_t reg,
                                                  uint16_t quantity,
                                                  uint16_t *out_regs)
{
    return read_registers_with_retries(device,
                                       reg,
                                       quantity,
                                       out_regs,
                                       device ? device->config.retries : 0U);
}

svd48_device_result_t svd48_device_probe_address(
    svd48_device_t *device,
    uint8_t address,
    uint16_t reg,
    uint16_t quantity,
    uint16_t *out_regs)
{
    if (!device || !device->initialized || address == 0U || address > 247U ||
        quantity == 0U || quantity > 16U || !out_regs ||
        (uint32_t)reg + (uint32_t)quantity > 0x10000UL) {
        return SVD48_DEVICE_INVALID_ARGUMENT;
    }

    uint8_t request[8] = {0};
    uint8_t response[64] = {0};
    size_t response_length = 0U;
    size_t request_length = svd48_build_read_request(address,
                                                     reg,
                                                     quantity,
                                                     request);
    if (request_length == 0U) {
        return SVD48_DEVICE_INVALID_ARGUMENT;
    }

    bus_transport_result_t transport_result = bus_transport_transact(
        device->config.transport,
        request,
        request_length,
        response,
        sizeof(response),
        &response_length,
        device->config.response_timeout_ms);
    svd48_device_result_t result = map_transport_result(transport_result);
    bool valid_crc = response_length >= 5U &&
                     svd48_frame_has_valid_crc(response, response_length);
    if (!valid_crc && transport_result == BUS_TRANSPORT_OK) {
        result = SVD48_DEVICE_CRC_ERROR;
    } else if (response_length > 0U && !valid_crc &&
               transport_result == BUS_TRANSPORT_INCOMPLETE) {
        result = SVD48_DEVICE_CRC_ERROR;
    }

    svd48_exception_response_t exception = {0};
    bool is_exception = result == SVD48_DEVICE_OK && valid_crc &&
                        svd48_parse_exception_response(response,
                                                       response_length,
                                                       address,
                                                       request[1],
                                                       &exception);
    const read_response_context_t response_context = {
        .address = address,
        .quantity = quantity,
    };
    if (is_exception) {
        result = SVD48_DEVICE_EXCEPTION;
    } else if (result == SVD48_DEVICE_OK && valid_crc &&
               !validate_read_response(response,
                                       response_length,
                                       &response_context)) {
        result = SVD48_DEVICE_BAD_RESPONSE;
    }

    if (device->trace_enabled && device->trace) {
        device->trace(device->trace_context,
                      device->config.device_id,
                      address,
                      1U,
                      request,
                      request_length,
                      response,
                      response_length,
                      result);
    }
    if (result != SVD48_DEVICE_OK) {
        return result;
    }

    for (uint16_t index = 0U; index < quantity; ++index) {
        out_regs[index] = ((uint16_t)response[3U + index * 2U] << 8U) |
                          response[4U + index * 2U];
    }
    return SVD48_DEVICE_OK;
}

svd48_device_result_t svd48_device_write_register(svd48_device_t *device,
                                                  uint16_t reg,
                                                  uint16_t value)
{
    if (svd48_register_is_runtime_actuation(reg)) {
        return SVD48_DEVICE_UNSUPPORTED;
    }
    return write_register_raw(device,
                              reg,
                              value,
                              0U);
}

svd48_device_result_t svd48_device_write_registers(svd48_device_t *device,
                                                   uint16_t start_reg,
                                                   const uint16_t *values,
                                                   uint16_t quantity)
{
    if (!device || !device->initialized || !values ||
        !svd48_write_multiple_range_is_valid(start_reg, quantity)) {
        return SVD48_DEVICE_INVALID_ARGUMENT;
    }
    if (svd48_register_range_has_runtime_actuation(start_reg, quantity)) {
        return SVD48_DEVICE_UNSUPPORTED;
    }
    uint8_t request[SVD48_WRITE_MULTIPLE_REQUEST_MAX_SIZE];
    uint8_t response[SVD48_WRITE_MULTIPLE_RESPONSE_SIZE];
    size_t request_length = svd48_build_write_multiple_request(
        device->config.address,
        start_reg,
        values,
        quantity,
        request,
        sizeof(request));
    if (request_length == 0U) {
        return SVD48_DEVICE_INVALID_ARGUMENT;
    }
    size_t response_length = 0;
    const write_multiple_response_context_t response_context = {
        .address = device->config.address,
        .start_reg = start_reg,
        .quantity = quantity,
    };
    svd48_device_result_t result = transact(device,
                                            request,
                                            request_length,
                                            response,
                                            sizeof(response),
                                            &response_length,
                                            0,
                                            validate_write_multiple_response,
                                            &response_context);
    return result;
}

bool svd48_device_get_communication(svd48_device_t *device,
                                    svd48_device_communication_t *communication)
{
    if (!device || !device->initialized || !communication || !lock_state(device)) {
        return false;
    }
    *communication = device->communication;
    unlock_state(device);
    return true;
}

void svd48_device_set_trace(svd48_device_t *device,
                            bool enabled,
                            svd48_device_trace_fn trace,
                            void *trace_context)
{
    if (!device || !device->initialized) {
        return;
    }
    device->trace_enabled = enabled;
    device->trace = trace;
    device->trace_context = trace_context;
}
