#include "audio_codec.h"
#include "board.h"
#include "config.h"
#include "led_pattern.h"
#include "ptt_button.h"
#include "recording_pcm.h"
#include "startup_audio.h"

#ifdef CONFIG_STUBUDDY_STATUS_LED
#include "status_led.h"
#endif

#include <esp_app_desc.h>
#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace {
constexpr char kTag[] = "STUBUDDY_PTT";
#ifdef CONFIG_STUBUDDY_STATUS_LED
constexpr char kCheckpoint[] = "3.3";
constexpr char kLedMode[] = "stateful";
stubuddy::StatusLed status_led;
#else
constexpr char kCheckpoint[] = "3.2";
constexpr char kLedMode[] = "OFF";
// Preserve the validated 3.2 variant, without linking the LEDC driver.
struct NoStatusLed {
    esp_err_t Initialize() { return ESP_OK; }
    void SetState(stubuddy::LedState) {}
    void SetPhase(stubuddy::PttLedPhase, bool = false) {}
} status_led;
#endif
constexpr size_t kMaxSamples = stubuddy::kSampleRate * 60;
constexpr size_t kMinSamples = stubuddy::kSampleRate * 300 / 1000;
constexpr size_t kPollSamples = stubuddy::kSampleRate / 100;  // 10 ms.
constexpr size_t kRecordingBytes = kMaxSamples * sizeof(int16_t);
static_assert(kMaxSamples == 960000 && kRecordingBytes == 1920000);

struct RecordingDeleter {
    void operator()(int16_t* data) const {
        if (data) {
            std::memset(data, 0, kRecordingBytes);
            heap_caps_free(data);
        }
    }
};

// Every failure leaves both audio directions stopped. Board construction owns
// GPIO10/18 configuration; do not reset those pins when re-opening I2S channels.
struct AudioStopper {
    AudioCodec& codec;
    ~AudioStopper() {
        codec.EnableInput(false);
        codec.EnableOutput(false);
    }
};

unsigned Milliseconds() { return static_cast<unsigned>(esp_timer_get_time() / 1000); }

bool RawButtonPressed() { return gpio_get_level(stubuddy::kMainButton) == 0; }

void IndicateStartupPlayback(bool playing) {
    status_led.SetState(playing ? stubuddy::LedState::Playback : stubuddy::LedState::Idle);
}

esp_err_t InitializeButton() {
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << stubuddy::kMainButton;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&config);
}

esp_err_t Replay(AudioCodec& codec, const int16_t* recording, size_t count,
                 std::vector<int16_t>& block, unsigned round) {
    const auto levels = stubuddy::AnalyzeRecording(recording, count);
    ESP_LOGI(kTag, "CAPTURE: samples=%u, DC=%ld, peak=%ld, RMS=%.1f, clipped=%u, gain=%.2f",
             unsigned(count), long(levels.mean), long(levels.peak), levels.rms,
             unsigned(levels.clipped), double(levels.gain));
    if (levels.peak < 16)
        ESP_LOGW(kTag, "Very low input: check microphone power and the existing PDM wiring");
    codec.EnableOutput(true);
    ESP_RETURN_ON_FALSE(codec.output_enabled() && !codec.HasOutputError(), ESP_FAIL, kTag,
                        "Cannot start replay output");
    status_led.SetPhase(stubuddy::PttLedPhase::Playback);
    ESP_LOGI(kTag, "REPLAY START: round=%u; button presses ignored until next IDLE", round);
    for (size_t offset = 0; offset < count;) {
        const size_t frames = std::min<size_t>(count - offset, stubuddy::kFramesPerBlock);
        block.resize(frames);
        for (size_t i = 0; i < frames; ++i)
            block[i] = stubuddy::ReplaySample(recording[offset + i], offset + i, count, levels,
                                              stubuddy::kSampleRate * 10 / 1000);
        codec.OutputData(block);
        ESP_RETURN_ON_FALSE(codec.output_enabled() && !codec.HasOutputError(), ESP_FAIL, kTag,
                            "Replay write failed");
        offset += frames;
    }
    codec.EnableOutput(false);
    // Playback is over. This is unconditional, including a key still held down.
    status_led.SetPhase(stubuddy::PttLedPhase::ReleaseGate);
    ESP_RETURN_ON_FALSE(!codec.HasOutputError(), ESP_FAIL, kTag, "Replay shutdown failed");
    ESP_LOGI(kTag, "REPLAY STOP: amplifier OFF; LED OFF");
    return ESP_OK;
}

esp_err_t RunPtt(AudioCodec& codec) {
    AudioStopper stop_on_exit{codec};
    ESP_RETURN_ON_FALSE(codec.input_sample_rate() == 16000 && codec.input_channels() == 1,
                        ESP_ERR_INVALID_STATE, kTag, "Unexpected capture format");
    ESP_RETURN_ON_ERROR(InitializeButton(), kTag, "GPIO15 input setup failed");
    std::unique_ptr<int16_t, RecordingDeleter> recording(static_cast<int16_t*>(
        heap_caps_malloc(kRecordingBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    ESP_RETURN_ON_FALSE(recording != nullptr, ESP_ERR_NO_MEM, kTag,
                        "Cannot allocate %u-byte PSRAM recording buffer",
                        unsigned(kRecordingBytes));
    ESP_LOGI(kTag, "PTT CONFIG: max=60 seconds, buffer=1920000 bytes; timeout=auto-replay; LED=%s",
             kLedMode);
    std::vector<int16_t> block(kPollSamples);
    stubuddy::PttButton button;
    unsigned round = 0;
    while (true) {
        // Keep release gates dark. Reading the raw held key here must never
        // relight the LED or arm another recording after playback completion.
        status_led.SetPhase(stubuddy::PttLedPhase::ReleaseGate);
        block.resize(kPollSamples);
        codec.EnableInput(true);
        ESP_RETURN_ON_FALSE(codec.input_enabled() && !codec.HasInputError(), ESP_FAIL, kTag,
                            "Cannot start PDM input");
        // Warm up before arming the key, avoiding loss of the child's first word.
        // Idle audio is drained and discarded, never stored in the PSRAM recording.
        for (int i = 0; i < 20; ++i)
            ESP_RETURN_ON_FALSE(codec.InputData(block), ESP_FAIL, kTag, "PDM warmup failed");
        ESP_LOGI(kTag, "WAIT RELEASE: release GPIO15 before the next recording; LED OFF");
        button.RequireRelease(RawButtonPressed(), Milliseconds());
        while (button.pressed()) {
            ESP_RETURN_ON_FALSE(codec.InputData(block), ESP_FAIL, kTag, "Release gate read failed");
            button.Update(RawButtonPressed(), Milliseconds());
        }
        status_led.SetPhase(stubuddy::PttLedPhase::ArmedIdle);
        ESP_LOGI(kTag, "IDLE: hold GPIO15 to record; release to replay (0.3-60 seconds); LED OFF");
        while (!button.pressed()) {
            ESP_RETURN_ON_FALSE(codec.InputData(block), ESP_FAIL, kTag, "Idle read failed");
            button.Update(RawButtonPressed(), Milliseconds());
            status_led.SetPhase(stubuddy::PttLedPhase::ArmedIdle, button.pressed());
        }

        size_t count = 0;
        status_led.SetPhase(stubuddy::PttLedPhase::Recording);
        ESP_LOGI(kTag, "REC START: round=%u; amplifier OFF; LED=%s", ++round, kLedMode);
        while (count < kMaxSamples && button.pressed()) {
            block.resize(std::min(kPollSamples, kMaxSamples - count));
            ESP_RETURN_ON_FALSE(codec.InputData(block), ESP_FAIL, kTag, "Recording read failed");
            std::memcpy(recording.get() + count, block.data(), block.size() * sizeof(int16_t));
            count += block.size();
            button.Update(RawButtonPressed(), Milliseconds());
        }
        // Close RX before analyzing or enabling TX, including the 60-second case.
        codec.EnableInput(false);
        status_led.SetPhase(stubuddy::PttLedPhase::PreparingReplay);
        ESP_RETURN_ON_FALSE(!codec.HasInputError(), ESP_FAIL, kTag, "Microphone shutdown failed");
        const bool limit = count == kMaxSamples;
        ESP_LOGI(kTag, "REC STOP: samples=%u, duration=%u ms, reason=%s; microphone OFF",
                 unsigned(count), unsigned(count * 1000 / stubuddy::kSampleRate),
                 limit ? "LIMIT" : "RELEASE");
        if (limit)
            ESP_LOGI(kTag,
                     "LIMIT: 60 seconds reached; automatic replay without waiting for release");
        if (count < kMinSamples)
            ESP_LOGI(kTag, "DROP: recording shorter than 300 ms; no replay");
        else
            ESP_RETURN_ON_ERROR(Replay(codec, recording.get(), count, block, round), kTag,
                                "Replay failed");
        std::memset(recording.get(), 0, count * sizeof(int16_t));
        std::fill(block.begin(), block.end(), 0);
        ESP_LOGI(kTag, "ROUND DONE: %u; internal_free=%u, PSRAM_free=%u; recording erased", round,
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        // Never queue playback-time presses. Even a continuously held key must
        // pass the next release gate before it can initiate a new recording.
    }
}
}  // namespace

extern "C" void app_main() {
    auto& board = Board::GetInstance();
    ESP_LOGI(kTag, "CHECKPOINT %s; firmware=%s; board=%s; variant=%s", kCheckpoint,
             esp_app_get_description()->version, board.GetBoardType().c_str(), BOARD_NAME);
    if (status_led.Initialize() != ESP_OK) {
        ESP_LOGE(kTag, "FAILED: LED initialization; PTT not armed");
        return;
    }
    if (!stubuddy::PlayStartupPrompt(board, IndicateStartupPlayback)) {
        status_led.SetState(stubuddy::LedState::Error);
        ESP_LOGE(kTag, "FAILED: startup check; PTT not armed");
        return;
    }
    auto* codec = board.GetAudioCodec();
    if (codec) {
        const esp_err_t result = RunPtt(*codec);
        status_led.SetState(stubuddy::LedState::Error);
        ESP_LOGE(kTag, "FAILED: %s; PTT stopped, reset required", esp_err_to_name(result));
    }
}
