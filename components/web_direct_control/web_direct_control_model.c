#include "web_direct_control_model.h"

#include <math.h>
#include <string.h>

static bool finite_axis(float value)
{
    return isfinite(value) && value >= -1.0f && value <= 1.0f;
}

static float apply_deadzone(float value, float deadzone)
{
    return fabsf(value) < deadzone ? 0.0f : value;
}

bool web_direct_control_model_init(web_direct_control_model_t *model,
                                   const web_direct_control_model_config_t *config)
{
    if (!model || !config || config->ttl_ms == 0U || !isfinite(config->deadzone) ||
        config->deadzone < 0.0f || config->deadzone >= 1.0f) {
        return false;
    }
    memset(model, 0, sizeof(*model));
    model->config = *config;
    model->state = WEB_DIRECT_MODEL_DISARMED;
    return true;
}

bool web_direct_control_model_claim_session(web_direct_control_model_t *model,
                                            uint64_t session_id)
{
    if (!model || session_id == 0U) return false;
    if (model->session_claimed && model->session_id != session_id) return false;
    model->session_claimed = true;
    model->session_id = session_id;
    return true;
}

void web_direct_control_model_release_session(web_direct_control_model_t *model,
                                              uint64_t session_id)
{
    if (!model || !model->session_claimed || model->session_id != session_id) return;
    model->session_claimed = false;
    model->session_id = 0U;
    model->armed = false;
    model->lease_seen = false;
    if (model->state != WEB_DIRECT_MODEL_EXPIRED) {
        model->state = WEB_DIRECT_MODEL_DISARMED;
    }
}

static bool owns_session(const web_direct_control_model_t *model, uint64_t session_id)
{
    return model && session_id != 0U && model->session_claimed &&
           model->session_id == session_id;
}

web_direct_control_model_result_t web_direct_control_model_arm(
    web_direct_control_model_t *model, uint64_t session_id, uint64_t now_ms)
{
    if (!owns_session(model, session_id)) return WEB_DIRECT_MODEL_REJECTED_SESSION;
    model->armed = true;
    model->lease_seen = true;
    model->last_valid_ms = now_ms;
    model->state = WEB_DIRECT_MODEL_ARMED;
    return WEB_DIRECT_MODEL_ACCEPTED;
}

web_direct_control_model_result_t web_direct_control_model_disarm(
    web_direct_control_model_t *model, uint64_t session_id)
{
    if (!owns_session(model, session_id)) return WEB_DIRECT_MODEL_REJECTED_SESSION;
    model->armed = false;
    model->lease_seen = false;
    model->state = WEB_DIRECT_MODEL_DISARMED;
    return WEB_DIRECT_MODEL_ACCEPTED;
}

web_direct_control_model_result_t web_direct_control_model_command(
    web_direct_control_model_t *model,
    uint64_t session_id,
    uint64_t now_ms,
    float forward,
    float turn,
    bool deadman,
    web_direct_control_command_t *out_command)
{
    if (!out_command || !finite_axis(forward) || !finite_axis(turn)) {
        return WEB_DIRECT_MODEL_REJECTED_ARGUMENT;
    }
    memset(out_command, 0, sizeof(*out_command));
    if (!owns_session(model, session_id)) return WEB_DIRECT_MODEL_REJECTED_SESSION;
    if (!model->armed) return WEB_DIRECT_MODEL_REJECTED_NOT_ARMED;
    out_command->forward = apply_deadzone(forward, model->config.deadzone);
    out_command->turn = apply_deadzone(turn, model->config.deadzone);
    out_command->zero_intent = out_command->forward == 0.0f && out_command->turn == 0.0f;
    out_command->deadman = deadman && !out_command->zero_intent;
    out_command->accepted = true;
    out_command->renews_lease = true;
    model->last_valid_ms = now_ms;
    model->lease_seen = true;
    model->state = out_command->deadman ? WEB_DIRECT_MODEL_ACTIVE : WEB_DIRECT_MODEL_ARMED;
    return WEB_DIRECT_MODEL_ACCEPTED;
}

bool web_direct_control_model_expire(web_direct_control_model_t *model,
                                     uint64_t now_ms)
{
    if (!model || !model->armed || !model->lease_seen ||
        now_ms - model->last_valid_ms <= model->config.ttl_ms) {
        return false;
    }
    model->armed = false;
    model->lease_seen = false;
    model->state = WEB_DIRECT_MODEL_EXPIRED;
    return true;
}

void web_direct_control_model_fault(web_direct_control_model_t *model)
{
    if (!model) return;
    model->armed = false;
    model->lease_seen = false;
    model->state = WEB_DIRECT_MODEL_FAULT;
}

void web_direct_control_model_reject_command(web_direct_control_model_t *model)
{
    if (!model) return;
    model->lease_seen = false;
    if (model->armed) model->state = WEB_DIRECT_MODEL_ARMED;
}

const char *web_direct_control_model_state_name(web_direct_control_model_state_t state)
{
    switch (state) {
    case WEB_DIRECT_MODEL_DISARMED: return "DISARMED";
    case WEB_DIRECT_MODEL_ARMED: return "ARMED";
    case WEB_DIRECT_MODEL_ACTIVE: return "ACTIVE";
    case WEB_DIRECT_MODEL_EXPIRED: return "EXPIRED";
    case WEB_DIRECT_MODEL_FAULT: return "FAULT";
    default: return "UNKNOWN";
    }
}
