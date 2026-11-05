#pragma once
#include <esp_err.h>
#include "countdown_state.h"
#include "device_controls.h"

namespace stubuddy {
class Countdown {
public:
    esp_err_t Load();
    bool Due() const;
    const CountdownState& state() const { return state_; }
    VolumeResult Apply(const VolumeRequest& request, bool cache_ready);
    // Consume before starting physical playback, preventing reboot replay.
    esp_err_t Consume();

private:
    esp_err_t Save(const CountdownState& next);
    CountdownState state_;
};
}  // namespace stubuddy
