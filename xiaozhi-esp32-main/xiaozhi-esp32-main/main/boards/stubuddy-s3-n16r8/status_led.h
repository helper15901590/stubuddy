#pragma once
#include "sdkconfig.h"

#include "led_pattern.h"
#ifdef CONFIG_STUBUDDY_AI_CONTROLS
#include "playback_led.h"
#endif

#include <esp_err.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

namespace stubuddy {

// Board-local validation driver. Static lifetime keeps error indication alive
// after app_main returns. The timer task is the sole PWM writer after init.
class StatusLed {
public:
    StatusLed() = default;
    StatusLed(const StatusLed&) = delete;
    StatusLed& operator=(const StatusLed&) = delete;

    esp_err_t Initialize();
    void SetState(LedState state);
    void SetNetworkState(NetworkLedState state);
#ifdef CONFIG_STUBUDDY_AI_CONTROLS
    void IRAM_ATTR PublishPlaybackPower(unsigned power);
#endif
    // Wait for the timer's actual PWM update before allowing replay to relight
    // the LED. minimum_off_ms is used only for the visible 60-second transition.
    esp_err_t ShowCaptureStopped(unsigned minimum_off_ms);
    void SetPhase(PttLedPhase phase, bool key_pressed = false) {
        SetState(PttLedState(phase, key_pressed));
    }

private:
    static void OnTick(void* context);
    void UpdateOutput();

    esp_timer_handle_t timer_ = nullptr;
    portMUX_TYPE mutex_ = portMUX_INITIALIZER_UNLOCKED;
    LedState state_ = LedState::Idle;
    LedState audio_state_ = LedState::Idle;
    NetworkLedState network_state_ = NetworkLedState::None;
    unsigned epoch_ms_ = 0;
    unsigned last_duty_ = 0;
    unsigned generation_ = 0;
    unsigned applied_generation_ = 0;
    bool output_error_logged_ = false;
#ifdef CONFIG_STUBUDDY_AI_CONTROLS
    unsigned meter_power_ = 0;
    unsigned envelope_generation_ = 0;
    PlaybackEnvelope envelope_;
#endif
};
}  // namespace stubuddy
