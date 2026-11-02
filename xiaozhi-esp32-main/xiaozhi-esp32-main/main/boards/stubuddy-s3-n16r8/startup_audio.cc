#include "startup_audio.h"
#include "audio_codec.h"
#include "board.h"

#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_psram.h>
#include <freertos/task.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace stubuddy {
constexpr char kTag[] = "STUBUDDY_STARTUP";
constexpr unsigned kBlockSamples = 128;
constexpr unsigned kToneMs = 180;
constexpr unsigned kFadeMs = 10;
constexpr float kPeak = 983.0f;  // Same 3% PCM16 peak as the accepted hardware test.
constexpr float kPi = 3.14159265358979323846f;

// The board factory must be constructed before checks, so failure paths also
// leave the amplifier and LED off. Core code uses only Board/AudioCodec APIs.
bool PlayStartupPrompt(Board& board) {
    uint32_t flash_size = 0;
    const esp_err_t flash_result = esp_flash_get_size(nullptr, &flash_size);
    const size_t psram_size = esp_psram_is_initialized() ? esp_psram_get_size() : 0;
    ESP_LOGI(kTag, "MEMORY: flash=%u MiB; PSRAM=%u MiB", unsigned(flash_size >> 20),
             unsigned(psram_size >> 20));
    if (flash_result != ESP_OK || flash_size != (16U << 20) || psram_size != (8U << 20)) {
        ESP_LOGE(kTag, "Expected N16R8 memory; tone skipped");
        return false;
    }
    auto* codec = board.GetAudioCodec();
    if (!codec) {
        ESP_LOGE(kTag, "Board outputs are not ready; tone skipped");
        return false;
    }
    codec->Start();
    if (codec->HasOutputError())
        return false;
    const unsigned rate = codec->output_sample_rate();
    if (rate != 16000 || codec->output_channels() != 1) {
        codec->EnableOutput(false);
        ESP_LOGE(kTag, "Unexpected AudioCodec format");
        return false;
    }
    ESP_LOGI(kTag, "READY: one 660 Hz tone in 2 seconds; fixed 3%% peak");
    vTaskDelay(pdMS_TO_TICKS(2000));
    codec->EnableOutput(true);
    if (!codec->output_enabled()) {
        ESP_LOGE(kTag, "Cannot enable AudioCodec output");
        return false;
    }
    std::vector<int16_t> block(kBlockSamples);
    const unsigned total = rate * kToneMs / 1000;
    const unsigned fade = rate * kFadeMs / 1000;
    ESP_LOGI(kTag, "TONE START 1/1: 660 Hz; mono PCM -> AudioCodec -> RIGHT I2S slot");
    for (unsigned offset = 0; offset < total;) {
        const unsigned count = std::min(total - offset, kBlockSamples);
        block.resize(count);
        for (unsigned i = 0; i < count; ++i) {
            const unsigned index = offset + i;
            const unsigned edge = std::min(index, total - 1 - index);
            const float envelope = std::min(1.0f, float(edge) / fade);
            block[i] =
                static_cast<int16_t>(kPeak * envelope * std::sin(2 * kPi * 660 * index / rate));
        }
        codec->OutputData(block);
        if (!codec->output_enabled()) {
            codec->EnableOutput(false);
            ESP_LOGE(kTag, "AudioCodec write failed; test stopped");
            return false;
        }
        offset += count;
    }
    codec->EnableOutput(false);
    if (codec->HasOutputError())
        return false;
    ESP_LOGI(kTag, "TONE FINISHED: audibility and quiet shutdown require hardware verification");
    return true;
}
}  // namespace stubuddy
