#pragma once
#include <esp_err.h>
#include <ctime>
#include "device_controls.h"
#include "scheduled_state.h"
namespace stubuddy {
class ScheduledReminders {
public:
    using Clock = int64_t (*)();
    explicit ScheduledReminders(Clock clock = [] { return int64_t(std::time(nullptr)); })
        : clock_(clock) {}
    esp_err_t Load();
    const ScheduledState& state() const { return state_; }
    VolumeResult Apply(const VolumeRequest& request, bool cache_ready);
    bool Due() const;
    // Claim all entries sharing the earliest deadline before physical output.
    // Repeating entries advance; once/countdown entries disappear persistently.
    esp_err_t Claim(std::array<ReminderEntry, 4>& group, unsigned& count);

private:
    esp_err_t Save(const ScheduledState& state);
    ScheduledState state_;
    Clock clock_;
};
}  // namespace stubuddy
