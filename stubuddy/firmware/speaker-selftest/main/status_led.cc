#include "status_led.h"

#include "board_config.h"

#include <driver/ledc.h>
#include <esp_check.h>
#include <esp_log.h>

namespace stubuddy {
namespace {

constexpr char kTag[] = "STUBUDDY_LED";
constexpr ledc_mode_t kSpeed = LEDC_LOW_SPEED_MODE;
constexpr ledc_channel_t kChannel = LEDC_CHANNEL_0;

unsigned NowMs() { return static_cast<unsigned>(esp_timer_get_time() / 1000); }

}  // namespace

esp_err_t StatusLed::Initialize() {
    ESP_RETURN_ON_FALSE(timer_ == nullptr, ESP_ERR_INVALID_STATE, kTag, "Already initialized");
    ledc_timer_config_t timer = {};
    timer.speed_mode = kSpeed;
    timer.duty_resolution = LEDC_TIMER_10_BIT;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = 20000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), kTag, "PWM timer init failed");

    ledc_channel_config_t channel = {};
    channel.gpio_num = kStatusLed;
    channel.speed_mode = kSpeed;
    channel.channel = kChannel;
    channel.timer_sel = LEDC_TIMER_0;
    channel.duty = 0;
#ifdef CONFIG_STUBUDDY_LED_ACTIVE_LOW
    channel.flags.output_invert = true;
#endif
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), kTag, "PWM channel init failed");

    epoch_ms_ = NowMs();
    esp_timer_create_args_t args = {};
    args.callback = OnTick;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "stubuddy_led";
    args.skip_unhandled_events = true;
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &timer_), kTag, "LED timer create failed");
    const esp_err_t result = esp_timer_start_periodic(timer_, 20000);
    if (result != ESP_OK) {
        esp_timer_delete(timer_);
        timer_ = nullptr;
        ESP_LOGE(kTag, "LED timer start failed: %s", esp_err_to_name(result));
        return result;
    }
    ESP_LOGI(kTag, "GPIO18 ready: active-%s, PWM=20kHz, update=20ms",
             channel.flags.output_invert ? "LOW" : "HIGH");
    ESP_LOGI(kTag, "%s", LedStateName(state_));
    return ESP_OK;
}

void StatusLed::SetState(LedState state) {
    bool changed = false;
    portENTER_CRITICAL(&mutex_);
    if (state_ != state) {
        state_ = state;
        epoch_ms_ = NowMs();
        changed = true;
    }
    portEXIT_CRITICAL(&mutex_);
    if (changed) {
        ESP_LOGI(kTag, "%s", LedStateName(state));
    }
}

void StatusLed::OnTick(void* context) { static_cast<StatusLed*>(context)->UpdateOutput(); }

void StatusLed::UpdateOutput() {
    // Only snapshot state while locked. PWM work and logging happen outside
    // this tiny critical section, and never take an audio lock or allocate.
    portENTER_CRITICAL(&mutex_);
    const LedState state = state_;
    const unsigned elapsed = NowMs() - epoch_ms_;
    portEXIT_CRITICAL(&mutex_);
    const unsigned duty = LedBrightness(state, elapsed);
    if (duty == last_duty_) {
        return;
    }
    // This callback is the sole PWM writer after initialization. The two basic
    // APIs need no fade-service allocation or fade semaphore in the timer task.
    esp_err_t result = ledc_set_duty(kSpeed, kChannel, duty);
    if (result == ESP_OK) {
        result = ledc_update_duty(kSpeed, kChannel);
    }
    if (result == ESP_OK) {
        last_duty_ = duty;
    } else if (!output_error_logged_) {
        output_error_logged_ = true;
        ESP_LOGE(kTag, "LED update failed: %s", esp_err_to_name(result));
    }
}

}  // namespace stubuddy
