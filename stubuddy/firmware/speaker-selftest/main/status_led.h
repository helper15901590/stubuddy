#pragma once

#include "led_pattern.h"

#include <esp_err.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

namespace stubuddy {

// Must have static lifetime: the timer continues after a diagnostic error or
// one-shot test returns from app_main. It is not owned by an audio task.
class StatusLed {
public:
    StatusLed() = default;
    StatusLed(const StatusLed&) = delete;
    StatusLed& operator=(const StatusLed&) = delete;

    esp_err_t Initialize();
    void SetState(LedState state);
    void SetNetworkState(NetworkLedState state);

private:
    static void OnTick(void* context);
    void UpdateOutput();

    esp_timer_handle_t timer_ = nullptr;
    portMUX_TYPE mutex_ = portMUX_INITIALIZER_UNLOCKED;
    LedState state_ = LedState::Idle;
    LedState audio_state_ = LedState::Idle;
    NetworkLedState network_state_ = NetworkLedState::None;
    unsigned epoch_ms_ = 0;
    unsigned last_duty_ = kLedMaxDuty + 1;
    bool output_error_logged_ = false;
};

}  // namespace stubuddy
