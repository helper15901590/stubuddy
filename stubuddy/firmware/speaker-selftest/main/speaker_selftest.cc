#include "board_config.h"

#include <driver/i2s_std.h>
#include <esp_check.h>
#include <esp_flash.h>
#include <esp_log.h>
#include <esp_psram.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#ifndef CONFIG_IDF_TARGET_ESP32S3
#error "This diagnostic is for the ESP32-S3-WROOM-1-N16R8 only."
#endif

namespace {

constexpr char kTag[] = "STUBUDDY_SPEAKER";
constexpr uint32_t kSampleRate = 16000;
constexpr size_t kFramesPerBlock = 128;
constexpr size_t kChannels = 2;
constexpr uint32_t kDmaDescriptors = 4;
constexpr uint32_t kWriteTimeoutMs = 1000;
constexpr uint32_t kToneMs = 180;
constexpr uint32_t kFadeMs = 10;
constexpr int kRepetitions = 3;
constexpr float kPi = 3.14159265358979323846f;
// Fixed 3% digital full-scale peak, independent of any saved user volume.
constexpr float kPeak16Bit = 983.0f;
using AudioBlock = std::array<int32_t, kFramesPerBlock * kChannels>;

static_assert(kSampleRate % 1000 == 0);

class SpeakerOutput {
public:
    ~SpeakerOutput() {
        const esp_err_t result = Close();
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "Cleanup failed: %s", esp_err_to_name(result));
        }
    }

    esp_err_t Initialize() {
        // Set the output latch before enabling GPIO output to avoid a high pulse.
        ESP_RETURN_ON_ERROR(gpio_set_level(stubuddy::kAmplifierCtrl, stubuddy::kAmplifierOff), kTag,
                            "Cannot clear amplifier CTRL");
        gpio_config_t gpio = {};
        gpio.pin_bit_mask = 1ULL << stubuddy::kAmplifierCtrl;
        gpio.mode = GPIO_MODE_OUTPUT;
        gpio.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio.pull_down_en = GPIO_PULLDOWN_DISABLE;
        gpio.intr_type = GPIO_INTR_DISABLE;
        ESP_RETURN_ON_ERROR(gpio_config(&gpio), kTag, "Cannot configure amplifier CTRL");
        ctrl_ready_ = true;

        // Reserve I2S0 for PDM capture in a future, separately validated step.
        i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
        channel.dma_desc_num = kDmaDescriptors;
        channel.dma_frame_num = kFramesPerBlock;
        channel.auto_clear_after_cb = true;
        ESP_RETURN_ON_ERROR(i2s_new_channel(&channel, &tx_, nullptr), kTag,
                            "Cannot allocate I2S1 TX");

        i2s_std_config_t config = {};
        config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
        config.slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
        config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
        config.gpio_cfg.bclk = stubuddy::kSpeakerBclk;
        config.gpio_cfg.ws = stubuddy::kSpeakerLrclk;
        config.gpio_cfg.dout = stubuddy::kSpeakerData;
        config.gpio_cfg.din = I2S_GPIO_UNUSED;
        ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx_, &config), kTag,
                            "Cannot configure I2S1 standard mode");

        AudioBlock silence = {};
        size_t loaded = 0;
        ESP_RETURN_ON_ERROR(i2s_channel_preload_data(tx_, silence.data(), sizeof(silence), &loaded),
                            kTag, "Cannot preload silence");
        ESP_RETURN_ON_FALSE(loaded == sizeof(silence), ESP_ERR_INVALID_SIZE, kTag,
                            "Incomplete silence preload");
        ESP_RETURN_ON_ERROR(i2s_channel_enable(tx_), kTag, "Cannot start I2S1 TX");
        running_ = true;
        ESP_LOGI(kTag, "I2S1 ready: 16000 Hz, 32-bit L/R slots; LEFT=silence, RIGHT=tone");
        return ESP_OK;
    }

    esp_err_t EnableAmplifier() {
        ESP_RETURN_ON_ERROR(WriteSilence(100), kTag, "Cannot prime silent output");
        ESP_RETURN_ON_ERROR(gpio_set_level(stubuddy::kAmplifierCtrl, stubuddy::kAmplifierRight),
                            kTag, "Cannot enable amplifier");
        ESP_LOGI(kTag, "CTRL=HIGH: amplifier enabled, RIGHT channel selected");
        return WriteSilence(20);
    }

    esp_err_t WriteSilence(uint32_t duration_ms) {
        const AudioBlock silence = {};
        uint32_t remaining = duration_ms * (kSampleRate / 1000);
        while (remaining > 0) {
            const size_t frames = std::min<size_t>(remaining, kFramesPerBlock);
            ESP_RETURN_ON_ERROR(Write(silence, frames), kTag, "Silence write failed");
            remaining -= frames;
        }
        return ESP_OK;
    }

    esp_err_t WriteTone(float frequency_hz) {
        constexpr uint32_t total_frames = kToneMs * (kSampleRate / 1000);
        constexpr uint32_t fade_frames = kFadeMs * (kSampleRate / 1000);
        AudioBlock block = {};
        for (uint32_t offset = 0; offset < total_frames;) {
            const size_t frames = std::min<size_t>(total_frames - offset, kFramesPerBlock);
            for (size_t frame = 0; frame < frames; ++frame) {
                const uint32_t index = offset + frame;
                const uint32_t edge = std::min(index, total_frames - 1 - index);
                const float envelope = std::min(1.0f, static_cast<float>(edge) / fade_frames);
                const float phase = 2.0f * kPi * frequency_hz * index / kSampleRate;
                const auto sample = static_cast<int32_t>(kPeak16Bit * envelope * std::sin(phase));
                block[frame * kChannels] = 0;
                // Multiplication is defined for negative PCM; signed left-shift is not.
                block[frame * kChannels + 1] = sample * 65536;
            }
            ESP_RETURN_ON_ERROR(Write(block, frames), kTag, "Tone write failed");
            offset += frames;
        }
        return ESP_OK;
    }

    esp_err_t Finish() {
        // Replace the complete DMA ring with silence before lowering CTRL.
        ESP_RETURN_ON_ERROR(WriteSilence(150), kTag, "Cannot flush trailing silence");
        constexpr uint32_t drain_ms =
            (kDmaDescriptors * kFramesPerBlock * 1000 + kSampleRate - 1) / kSampleRate + 20;
        vTaskDelay(pdMS_TO_TICKS(drain_ms));
        return Close();
    }

    esp_err_t Close() {
        esp_err_t first_error = ESP_OK;
        if (ctrl_ready_) {
            first_error = gpio_set_level(stubuddy::kAmplifierCtrl, stubuddy::kAmplifierOff);
        }
        if (running_) {
            // NS4168 requires at least 100 us with CTRL low to shut down.
            vTaskDelay(pdMS_TO_TICKS(10));
            const esp_err_t result = i2s_channel_disable(tx_);
            if (first_error == ESP_OK) {
                first_error = result;
            }
            if (result == ESP_OK) {
                running_ = false;
            }
        }
        if (tx_ != nullptr && !running_) {
            const esp_err_t result = i2s_del_channel(tx_);
            if (first_error == ESP_OK) {
                first_error = result;
            }
            if (result == ESP_OK) {
                tx_ = nullptr;
            }
        }
        return first_error;
    }

private:
    esp_err_t Write(const AudioBlock& block, size_t frames) {
        const size_t expected = frames * kChannels * sizeof(int32_t);
        size_t written = 0;
        ESP_RETURN_ON_ERROR(
            i2s_channel_write(tx_, block.data(), expected, &written, kWriteTimeoutMs), kTag,
            "I2S write failed");
        ESP_RETURN_ON_FALSE(written == expected, ESP_ERR_INVALID_SIZE, kTag,
                            "Short I2S write: %u/%u bytes", static_cast<unsigned>(written),
                            static_cast<unsigned>(expected));
        return ESP_OK;
    }

    i2s_chan_handle_t tx_ = nullptr;
    bool ctrl_ready_ = false;
    bool running_ = false;
};

esp_err_t RunSelfTest() {
    // Construct first so all subsequent error paths leave CTRL low.
    SpeakerOutput speaker;
    ESP_RETURN_ON_ERROR(speaker.Initialize(), kTag, "Output initialization failed");

    uint32_t flash_size = 0;
    ESP_RETURN_ON_ERROR(esp_flash_get_size(nullptr, &flash_size), kTag, "Cannot read flash size");
    const size_t psram_size = esp_psram_is_initialized() ? esp_psram_get_size() : 0;
    ESP_LOGI(kTag, "MEMORY: flash=%u MiB, PSRAM=%u MiB", static_cast<unsigned>(flash_size >> 20),
             static_cast<unsigned>(psram_size >> 20));
    ESP_RETURN_ON_FALSE(flash_size == (16U << 20) && psram_size == (8U << 20), ESP_ERR_INVALID_SIZE,
                        kTag, "Expected N16R8: 16 MiB flash and 8 MiB PSRAM");

    ESP_LOGI(kTag, "START in 2 seconds; fixed 3%% PCM peak; no networking or recording");
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_RETURN_ON_ERROR(speaker.EnableAmplifier(), kTag, "Amplifier startup failed");
    for (int repetition = 1; repetition <= kRepetitions; ++repetition) {
        ESP_LOGI(kTag, "PLAY %d/%d: 440 Hz -> 660 Hz", repetition, kRepetitions);
        ESP_RETURN_ON_ERROR(speaker.WriteTone(440.0f), kTag, "First tone failed");
        ESP_RETURN_ON_ERROR(speaker.WriteSilence(80), kTag, "Tone gap failed");
        ESP_RETURN_ON_ERROR(speaker.WriteTone(660.0f), kTag, "Second tone failed");
        if (repetition < kRepetitions) {
            ESP_RETURN_ON_ERROR(speaker.WriteSilence(2000), kTag, "Repeat gap failed");
        }
    }
    return speaker.Finish();
}

}  // namespace

extern "C" void app_main() {
    ESP_LOGI(kTag, "stubuddy-s3-n16r8 speaker self-test v0.1.0 / RIGHT channel");
    const esp_err_t result = RunSelfTest();
    if (result == ESP_OK) {
        ESP_LOGI(kTag, "DONE: amplifier off; waiting for reset (audibility is a manual check)");
    } else {
        ESP_LOGE(kTag, "FAIL: %s; test stopped, no automatic retry", esp_err_to_name(result));
    }
    // Returning deletes the main task. Idle tasks continue until reset/power cycle.
}
