#include "countdown.h"
#include <esp_log.h>
#include <nvs.h>
#include <ctime>

namespace stubuddy {
esp_err_t Countdown::Load() {
    nvs_handle_t handle;
    auto result = nvs_open("stubuddy_audio", NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND)
        return ESP_OK;
    if (result != ESP_OK)
        return result;
    size_t size = sizeof(state_);
    result = nvs_get_blob(handle, "countdown_v1", &state_, &size);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        state_ = {};
        return ESP_OK;
    }
    if (result != ESP_OK)
        return result;
    if (size != sizeof(state_) || !state_.Valid())
        return ESP_ERR_INVALID_STATE;
    ESP_LOGI("STUBUDDY_TIMER", "LOADED: active=%d alarm_volume=%d", bool(state_.deadline),
             int(state_.alarm_volume));
    return ESP_OK;
}
esp_err_t Countdown::Save(const CountdownState& next) {
    nvs_handle_t handle;
    auto result = nvs_open("stubuddy_audio", NVS_READWRITE, &handle);
    if (result != ESP_OK)
        return result;
    result = nvs_set_blob(handle, "countdown_v1", &next, sizeof(next));
    if (result == ESP_OK)
        result = nvs_commit(handle);
    nvs_close(handle);
    if (result == ESP_OK)
        state_ = next;
    return result;
}
bool Countdown::Due() const { return state_.Due(std::time(nullptr)); }
esp_err_t Countdown::Consume() {
    auto next = state_;
    next.Clear();
    const auto result = Save(next);
    // A persistence failure must never leave a tight loop of spoken reminders.
    if (result != ESP_OK)
        state_.Clear();
    return result;
}
VolumeResult Countdown::Apply(const VolumeRequest& request, bool cache_ready) {
    auto next = state_;
    esp_err_t result = ESP_OK;
    switch (request.action) {
        case TimerAction::Start:
            if (!cache_ready || !next.Start(request.seconds, std::time(nullptr),
                                            request.cache_id.data(), request.text.data()))
                result = ESP_ERR_INVALID_STATE;
            else
                result = Save(next);
            break;
        case TimerAction::Cancel:
            if (next.deadline) {
                next.Clear();
                result = Save(next);
            }
            break;
        case TimerAction::Volume:
            if (request.volume < 0 || request.volume > 100)
                result = ESP_ERR_INVALID_ARG;
            else if (next.alarm_volume != request.volume) {
                next.alarm_volume = request.volume;
                result = Save(next);
            }
            break;
        case TimerAction::Get:
            break;
        default:
            result = ESP_ERR_INVALID_ARG;
    }
    VolumeResult reply{request, result == ESP_OK};
    reply.active = state_.deadline != 0;
    reply.remaining = state_.Remaining(std::time(nullptr));
    reply.alarm_volume = state_.alarm_volume;
    ESP_LOGI("STUBUDDY_TIMER", "TOOL: action=%d ok=%d active=%d remaining=%u alarm_volume=%d",
             int(request.action), reply.ok, reply.active, reply.remaining, reply.alarm_volume);
    return reply;
}
}  // namespace stubuddy
