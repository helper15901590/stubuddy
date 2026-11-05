#include "network_audio.h"
#include "audio_codec.h"
#include "audio_levels.h"
#include "status_led.h"
#include "wifi_setup.h"

#include <esp_check.h>
#include <esp_log.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace stubuddy {
namespace {
constexpr char kTag[] = "STUBUDDY_WIFI_AUDIO";
constexpr unsigned kRate = 16000;
constexpr unsigned kBlock = 128;
constexpr float kPi = 3.14159265358979323846f;

struct CueStopper {
    AudioCodec& codec;
    StatusLed& led;
    bool closed = false;
    ~CueStopper() {
        if (!closed)
            codec.EnableOutput(false);
        led.SetState(LedState::Idle);
    }
};

esp_err_t WriteTone(AudioCodec& codec, std::vector<int16_t>& block, unsigned frequency,
                    unsigned duration_ms) {
    const unsigned total = kRate * duration_ms / 1000;
    constexpr unsigned fade = kRate / 100;
    for (unsigned offset = 0; offset < total;) {
        const unsigned frames = std::min(total - offset, kBlock);
        block.resize(frames);
        for (unsigned i = 0; i < frames; ++i) {
            const unsigned index = offset + i;
            const unsigned edge = std::min(index, total - 1 - index);
            const float envelope = std::min(1.0f, float(edge) / fade);
            block[i] = frequency == 0
                           ? 0
                           : static_cast<int16_t>(kOutputPeakPcm16 * envelope *
                                                  std::sin(2 * kPi * frequency * index / kRate));
        }
        codec.OutputData(block);
        ESP_RETURN_ON_FALSE(codec.output_enabled() && !codec.HasOutputError(), ESP_FAIL, kTag,
                            "WiFi cue write failed");
        offset += frames;
    }
    return ESP_OK;
}
}  // namespace

esp_err_t PlayWifiCue(AudioCodec& codec, StatusLed& led, WifiCue cue) {
    if (cue == WifiCue::None)
        return ESP_OK;
    ESP_RETURN_ON_FALSE(!codec.input_enabled(), ESP_ERR_INVALID_STATE, kTag,
                        "WiFi cue requested while RX is active");
    const auto pattern = CuePattern(cue);
    CueStopper stop_on_exit{codec, led};
    codec.EnableOutput(true);
    ESP_RETURN_ON_FALSE(codec.output_enabled() && !codec.HasOutputError(), ESP_FAIL, kTag,
                        "Cannot enable WiFi cue output");
    // A failure cue may have waited behind a long record/replay round. Keep
    // its error indication even when the network's brief flash has expired.
    led.SetState(cue == WifiCue::Failed ? LedState::Error : LedState::Playback);
    ESP_LOGI(kTag, "CUE: %s; tones=%u; %u/%u/%u Hz; serialized with PTT",
             cue == WifiCue::Connected ? "CONNECTED"
             : cue == WifiCue::Setup   ? "SETUP"
                                       : "FAILED",
             pattern.count, pattern.frequencies[0], pattern.frequencies[1], pattern.frequencies[2]);
    std::vector<int16_t> block(kBlock);
    for (unsigned i = 0; i < pattern.count; ++i) {
        if (i)
            ESP_RETURN_ON_ERROR(WriteTone(codec, block, 0, 80), kTag, "Tone gap failed");
        ESP_RETURN_ON_ERROR(WriteTone(codec, block, pattern.frequencies[i], 180), kTag,
                            "Cue tone failed");
    }
    // Explicit close/check before returning; the guard also covers early errors.
    codec.EnableOutput(false);
    stop_on_exit.closed = true;
    return codec.HasOutputError() ? ESP_FAIL : ESP_OK;
}
}  // namespace stubuddy
