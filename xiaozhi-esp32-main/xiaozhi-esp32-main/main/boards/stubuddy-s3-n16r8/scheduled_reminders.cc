#include "scheduled_reminders.h"
#include <esp_log.h>
#include <nvs.h>
#include <ctime>
namespace stubuddy {
esp_err_t ScheduledReminders::Save(const ScheduledState& state) {
    nvs_handle_t handle;
    auto error = nvs_open("stubuddy_audio", NVS_READWRITE, &handle);
    if (error != ESP_OK)
        return error;
    error = nvs_set_blob(handle, "reminders_v2", &state, sizeof(state));
    if (error == ESP_OK)
        error = nvs_commit(handle);
    nvs_close(handle);
    if (error == ESP_OK)
        state_ = state;
    return error;
}
esp_err_t ScheduledReminders::Load() {
    nvs_handle_t handle;
    auto error = nvs_open("stubuddy_audio", NVS_READWRITE, &handle);
    if (error != ESP_OK)
        return error;
    size_t size = sizeof(state_);
    error = nvs_get_blob(handle, "reminders_v2", &state_, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        CountdownState legacy;
        size_t old_size = sizeof(legacy);
        error = nvs_get_blob(handle, "countdown_v1", &legacy, &old_size);
        if (error == ESP_ERR_NVS_NOT_FOUND)
            error = ESP_OK;
        else if (error == ESP_OK && old_size == sizeof(legacy) && legacy.Valid()) {
            state_.alarm_volume = legacy.alarm_volume;
            if (legacy.deadline) {
                auto& entry = state_.entries[0];
                entry.deadline = legacy.deadline;
                std::strcpy(entry.id, legacy.id);
                std::strcpy(entry.text, legacy.text);
                // Older records contain speech only; expose that actual saved
                // text without inventing a subject from conversation history.
                size_t length = std::min<size_t>(std::strlen(legacy.text), 60);
                while (length && (static_cast<uint8_t>(legacy.text[length]) & 0xc0) == 0x80)
                    --length;
                std::memcpy(entry.item, legacy.text, length);
            }
        } else if (error == ESP_OK)
            error = ESP_ERR_INVALID_STATE;
        nvs_close(handle);
        if (error != ESP_OK)
            return error;
        // A durable v2 marker prevents stale v1 countdown resurrection.
        error = Save(state_);
        if (error != ESP_OK)
            return error;
        return Load();  // Also retire the legacy deadline after the v2 marker.
    }
    nvs_close(handle);
    if (error != ESP_OK || size != sizeof(state_) || !state_.Valid())
        return ESP_ERR_INVALID_STATE;
    // Keep a rollback from resurrecting the pre-migration timer. This is not a
    // per-tick write: only clear a real legacy deadline or changed volume.
    error = nvs_open("stubuddy_audio", NVS_READWRITE, &handle);
    if (error != ESP_OK)
        return error;
    CountdownState legacy;
    size_t old_size = sizeof(legacy);
    const auto old_error = nvs_get_blob(handle, "countdown_v1", &legacy, &old_size);
    if (old_error == ESP_OK && old_size == sizeof(legacy) && legacy.Valid() &&
        (legacy.deadline || legacy.alarm_volume != state_.alarm_volume)) {
        legacy.Clear();
        legacy.alarm_volume = state_.alarm_volume;
        error = nvs_set_blob(handle, "countdown_v1", &legacy, sizeof(legacy));
        if (error == ESP_OK)
            error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}
bool ScheduledReminders::Due() const {
    const auto now = clock_();
    if (!CountdownState::TimeReady(now))
        return false;
    for (const auto& entry : state_.entries)
        if (entry.deadline && entry.deadline <= now)
            return true;
    return false;
}
esp_err_t ScheduledReminders::Claim(std::array<ReminderEntry, 4>& group, unsigned& count) {
    count = 0;
    const auto now = clock_();
    int64_t first = INT64_MAX;
    for (const auto& entry : state_.entries)
        if (entry.deadline && entry.deadline <= now)
            first = std::min(first, entry.deadline);
    auto next = state_;
    for (auto& entry : next.entries)
        if (entry.deadline == first) {
            group[count++] = entry;
            entry.Advance(now);
        }
    if (!count)
        return ESP_ERR_INVALID_STATE;
    const auto error = Save(next);
    if (error != ESP_OK) {
        // Stop retry loops on NVS failure; no physical announcement on failure.
        state_ = next;
        count = 0;
    }
    return error;
}
VolumeResult ScheduledReminders::Apply(const VolumeRequest& request, bool cache_ready) {
    auto next = state_;
    VolumeResult result;
    result.request = request;
    unsigned code = 0;
    switch (request.action) {
        case TimerAction::Get:
            break;
        case TimerAction::Volume:
            if (request.volume < 0 || request.volume > 100)
                code = 3;
            else
                next.alarm_volume = request.volume;
            break;
        case TimerAction::Start:
        case TimerAction::ScheduledStart: {
            ReminderEntry entry;
            const auto now = clock_();
            entry.deadline =
                request.action == TimerAction::Start ? now + request.seconds : request.deadline;
            entry.minute = request.minute;
            entry.weekdays = request.weekdays;
            std::strcpy(entry.id, request.cache_id.data());
            std::strcpy(entry.text, request.text.data());
            std::strcpy(entry.item, request.item.data());
            code = !CountdownState::TimeReady(now) || !cache_ready
                       ? 3
                       : next.Add(entry, request.action == TimerAction::Start, now);
            break;
        }
        case TimerAction::Cancel: {
            bool found = false;
            for (auto& entry : next.entries)
                if (entry.deadline && !std::strcmp(entry.id, request.cache_id.data())) {
                    entry = {};
                    found = true;
                }
            if (!found)
                code = 4;
            break;
        }
        default:
            code = 3;
    }
    result.error = code;
    result.ok = !code && (request.action == TimerAction::Get || Save(next) == ESP_OK);
    if (!result.ok && !code)
        result.error = 5;
    result.records = state_.entries;
    result.now = clock_();
    result.alarm_volume = state_.alarm_volume;
    result.active = state_.entries[0].deadline != 0;
    result.remaining = result.active && result.now < state_.entries[0].deadline
                           ? unsigned(state_.entries[0].deadline - result.now)
                           : 0;
    ESP_LOGI("STUBUDDY_TIMER", "TOOL v2: action=%d ok=%d error=%u", int(request.action), result.ok,
             result.error);
    return result;
}
}  // namespace stubuddy
