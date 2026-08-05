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
        for (size_t index = 0; index < SVD48_DEVICE_CHANNEL_COUNT; ++index) {
            device->snapshots[index].last_error = result;
            device->snapshots[index].online = false;
        }
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
        snapshot->online = false;
    }
    unlock_state(device);
}

static svd48_device_result_t transact(svd48_device_t *device,
                                      const uint8_t *request,
                                      size_t request_length,
                                      uint8_t *response,
                                      size_t response_capacity,
                                      size_t *response_length,
                                      uint8_t retries)
{
    if (!device || !device->initialized || !request || !response ||
        !response_length) {
        return SVD48_DEVICE_INVALID_ARGUMENT;
    }
    svd48_device_result_t last_result = SVD48_DEVICE_IO_ERROR;
    for (uint8_t attempt = 0; attempt <= retries; ++attempt) {
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
        if (*response_length > 0U && !valid_crc &&
            (transport_result == BUS_TRANSPORT_OK ||
             transport_result == BUS_TRANSPORT_INCOMPLETE)) {
            last_result = SVD48_DEVICE_CRC_ERROR;
        }
        svd48_exception_response_t exception;
        if (valid_crc && svd48_parse_exception_response(response,
                                                        *response_length,
                                                        device->config.address,
                                                        request[1],
                                                        &exception)) {
            last_result = SVD48_DEVICE_EXCEPTION;
            record_communication(device, last_result);
            record_exception(device, &exception);
            if (device->trace_enabled && device->trace) {
                device->trace(device->trace_context,
                              device->config.device_id,
                              device->config.address,
                              attempt + 1U,
                              request,
                              request_length,
                              response,
                              *response_length,
                              last_result);
            }
            return last_result;
        }
        if (device->trace_enabled && device->trace) {
            device->trace(device->trace_context,
                          device->config.device_id,
                          device->config.address,
                          attempt + 1U,
                          request,
                          request_length,
                          response,
                          *response_length,
                          last_result);
        }
        if (last_result == SVD48_DEVICE_OK && valid_crc) {
            record_communication(device, SVD48_DEVICE_OK);
            return SVD48_DEVICE_OK;
        }
    }
    record_communication(device, last_result);
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
    svd48_build_read_request(device->config.address, reg, quantity, request);
    svd48_device_result_t result = transact(device,
                                            request,
                                            sizeof(request),
                                            response,
                                            sizeof(response),
                                            &response_length,
                                            retries);
    if (result != SVD48_DEVICE_OK) {
        return result;
    }
    if (response_length != 5U + (size_t)quantity * 2U ||
        response[0] != device->config.address ||
        response[1] != SVD48_FUNC_READ_HOLDING ||
        response[2] != quantity * 2U) {
        record_communication(device, SVD48_DEVICE_BAD_RESPONSE);
        return SVD48_DEVICE_BAD_RESPONSE;
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
    svd48_device_result_t result = transact(device,
                                            request,
                                            sizeof(request),
                                            response,
                                            sizeof(response),
                                            &response_length,
                                            retries);
    if (result != SVD48_DEVICE_OK) {
        return result;
    }
    if (response_length != sizeof(request) ||
        memcmp(request, response, sizeof(request)) != 0) {
        record_communication(device, SVD48_DEVICE_BAD_RESPONSE);
        return SVD48_DEVICE_BAD_RESPONSE;
    }
    return SVD48_DEVICE_OK;
}

static void update_pair_i16(svd48_device_t *device,
                            pair_field_t field,
                            const uint16_t values[2])
{
    if (!lock_state(device)) {
        return;
    }
    uint32_t timestamp = now_ms(device);
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
            snapshot->observed_speed_decirpm = value;
            break;
        case PAIR_CURRENT:
            snapshot->current_deciamp = value;
            break;
        }
        snapshot->online = true;
        snapshot->stale = false;
        snapshot->last_error = SVD48_DEVICE_OK;
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
    for (size_t index = 0; index < SVD48_DEVICE_CHANNEL_COUNT; ++index) {
        uint32_t raw = ((uint32_t)values[index * 2U] << 16U) |
                       values[index * 2U + 1U];
        svd48_channel_snapshot_t *snapshot = &device->snapshots[index];
        if (error_code) {
            snapshot->error_code = raw;
        } else {
            snapshot->position_counts = (int32_t)raw;
        }
        snapshot->online = true;
        snapshot->stale = false;
        snapshot->last_error = SVD48_DEVICE_OK;
        snapshot->last_update_ms = timestamp;
    }
    unlock_state(device);
}

bool svd48_device_init(svd48_device_t *device,
                       const svd48_device_config_t *config)
{
    if (!device || !config || config->device_id == 0U || config->address == 0U ||
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

bool svd48_channel_get_snapshot(svd48_channel_t *channel,
                                svd48_channel_snapshot_t *snapshot)
{
    if (!channel_valid(channel) || !snapshot || !lock_state(channel->device)) {
        return false;
    }
    *snapshot = channel->device->snapshots[channel->id];
    uint32_t timestamp = now_ms(channel->device);
    uint32_t age = timestamp - snapshot->last_update_ms;
    snapshot->stale = snapshot->last_update_ms == 0U ||
                      age > channel->device->config.stale_timeout_ms;
    if (snapshot->stale) {
        snapshot->online = false;
    }
    unlock_state(channel->device);
    return true;
}

svd48_channel_health_t svd48_channel_get_health(svd48_channel_t *channel)
{
    svd48_channel_snapshot_t snapshot;
    if (!svd48_channel_get_snapshot(channel, &snapshot)) {
        return SVD48_CHANNEL_HEALTH_UNKNOWN;
    }
    if (snapshot.error_code != 0U) {
        return SVD48_CHANNEL_HEALTH_FAULT;
    }
    if (!snapshot.online || snapshot.stale) {
        return SVD48_CHANNEL_HEALTH_OFFLINE;
    }
    if (snapshot.last_error != SVD48_DEVICE_OK) {
        return SVD48_CHANNEL_HEALTH_DEGRADED;
    }
    return SVD48_CHANNEL_HEALTH_HEALTHY;
}

svd48_device_result_t svd48_device_poll(svd48_device_t *device)
{
    if (!device || !device->initialized) {
        return SVD48_DEVICE_INVALID_ARGUMENT;
    }
    uint16_t values2[2];
    uint16_t values4[4];
    bool slow_poll = (device->poll_count++ % SVD48_POLL_SLOW_DIVIDER) == 0U;
    svd48_device_result_t result = read_registers_with_retries(
        device, REG_M1_POSITION, 4, values4, 0);
    if (result != SVD48_DEVICE_OK) {
        return result;
    }
    update_pair_i32(device, values4, false);
    if (read_registers_with_retries(device,
                                    REG_M1_ACTUAL_SPEED,
                                    2,
                                    values2,
                                    0) == SVD48_DEVICE_OK) {
        update_pair_i16(device, PAIR_SPEED, values2);
    }
    if (read_registers_with_retries(device,
                                    REG_M1_ACTUAL_CURRENT,
                                    2,
                                    values2,
                                    0) == SVD48_DEVICE_OK) {
        update_pair_i16(device, PAIR_CURRENT, values2);
    }
    if (slow_poll) {
        if (read_registers_with_retries(device, REG_M1_STATUS, 2, values2, 0) ==
            SVD48_DEVICE_OK) {
            update_pair_i16(device, PAIR_STATUS, values2);
        }
        if (read_registers_with_retries(device,
                                        REG_M1_MOTOR_TEMP,
                                        2,
                                        values2,
                                        0) == SVD48_DEVICE_OK) {
            update_pair_i16(device, PAIR_MOTOR_TEMP, values2);
        }
        if (read_registers_with_retries(device,
                                        REG_M1_BUS_VOLTAGE,
                                        2,
                                        values2,
                                        0) == SVD48_DEVICE_OK) {
            update_pair_i16(device, PAIR_BUS_VOLTAGE, values2);
        }
        if (read_registers_with_retries(device,
                                        REG_M1_MOS_TEMP,
                                        2,
                                        values2,
                                        0) == SVD48_DEVICE_OK) {
            update_pair_i16(device, PAIR_MOS_TEMP, values2);
        }
        if (read_registers_with_retries(device,
                                        REG_M1_ERROR_CODE,
                                        4,
                                        values4,
                                        0) == SVD48_DEVICE_OK) {
            update_pair_i32(device, values4, true);
        }
    }
    return SVD48_DEVICE_OK;
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
                              device ? device->config.retries : 0U);
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
    svd48_device_result_t result = transact(device,
                                            request,
                                            request_length,
                                            response,
                                            sizeof(response),
                                            &response_length,
                                            0);
    if (result != SVD48_DEVICE_OK) {
        return result;
    }
    svd48_write_multiple_response_t acknowledgement;
    if (!svd48_parse_write_multiple_response(response,
                                             response_length,
                                             device->config.address,
                                             start_reg,
                                             quantity,
                                             &acknowledgement)) {
        record_communication(device, SVD48_DEVICE_BAD_RESPONSE);
        return SVD48_DEVICE_BAD_RESPONSE;
    }
    return SVD48_DEVICE_OK;
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
