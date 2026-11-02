#include "board.h"
#include "config.h"
#include "stubuddy_audio_codec.h"

#include <esp_log.h>

namespace {
constexpr char kTag[] = "STUBUDDY_BOARD";

esp_err_t InitializeOutput(gpio_num_t pin, int initial_level) {
    esp_err_t result = gpio_set_level(pin, initial_level);
    if (result != ESP_OK)
        return result;
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << pin;
    config.mode = GPIO_MODE_OUTPUT;
    config.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&config);
}
}  // namespace

class StubuddyBoard final : public Board {
public:
    StubuddyBoard() {
        // Configure these pins exactly once. The codec owns subsequent level changes.
        ready_ = InitializeOutput(stubuddy::kAmplifierCtrl, stubuddy::kAmplifierOff) == ESP_OK;
        const bool led_ready = InitializeOutput(stubuddy::kStatusLed, stubuddy::kLedOff) == ESP_OK;
        ready_ = ready_ && led_ready;
        ESP_LOGI(kTag, "board=%s; variant=%s; amplifier OFF; LED OFF; ready=%s", BOARD_TYPE,
                 BOARD_NAME, ready_ ? "yes" : "no");
    }

    std::string GetBoardType() override { return BOARD_TYPE; }
    AudioCodec* GetAudioCodec() override {
        if (!ready_)
            return nullptr;
        static StubuddyAudioCodec codec;
        return &codec;
    }
    NetworkInterface* GetNetwork() override { return nullptr; }
    void StartNetwork() override {
        ESP_LOGW(kTag, "Network is unavailable in local audio validation");
    }
    const char* GetNetworkStateIcon() override { return ""; }
    void SetPowerSaveLevel(PowerSaveLevel) override {}
    std::string GetBoardJson() override {
        return R"({"type":")" BOARD_TYPE R"(","name":")" BOARD_NAME R"(","audio_only":true})";
    }
    std::string GetDeviceStatusJson() override { return R"({"startup_validation":true})"; }

private:
    bool ready_ = false;
};

DECLARE_BOARD(StubuddyBoard);
