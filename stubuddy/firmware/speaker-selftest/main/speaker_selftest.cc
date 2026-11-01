#include "board_config.h"
#include "ptt_button.h"
#include "status_led.h"

#include <driver/i2s_pdm.h>
#include <driver/i2s_std.h>
#include <esp_check.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_psram.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#ifndef CONFIG_IDF_TARGET_ESP32S3
#error "This diagnostic is for the ESP32-S3-WROOM-1-N16R8 only."
#endif

namespace {

constexpr char kTag[] = "STUBUDDY_SPEAKER";
using stubuddy::LedState;
stubuddy::StatusLed status_led;
constexpr uint32_t kSampleRate = 16000;
constexpr size_t kFramesPerBlock = 128;
constexpr size_t kChannels = 2;
constexpr uint32_t kDmaDescriptors = 4;
constexpr uint32_t kWriteTimeoutMs = 1000;
constexpr uint32_t kToneMs = 180;
constexpr uint32_t kFadeMs = 10;
constexpr int kRepetitions = 3;
constexpr size_t kRecordingSamples = kSampleRate * 5;
constexpr uint32_t kPttMaxSeconds = 60;
constexpr float kPi = 3.14159265358979323846f;
// Fixed 3% digital full-scale peak, independent of any saved user volume.
constexpr float kPeak16Bit = 983.0f;
using AudioBlock = std::array<int32_t, kFramesPerBlock * kChannels>;

static_assert(kSampleRate % 1000 == 0);

// GPIO10 belongs to the board for its entire lifetime. Repeated gpio_config()
// calls reserve this same output again in IDF 6.1 and produce a conflict warning.
// Only the main diagnostic task calls this function; later rounds only set level.
esp_err_t InitializeAmplifierControl() {
    static bool configured = false;
    ESP_RETURN_ON_ERROR(gpio_set_level(stubuddy::kAmplifierCtrl, stubuddy::kAmplifierOff), kTag,
                        "Cannot clear amplifier CTRL");
    if (!configured) {
        gpio_config_t gpio = {};
        gpio.pin_bit_mask = 1ULL << stubuddy::kAmplifierCtrl;
        gpio.mode = GPIO_MODE_OUTPUT;
        gpio.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio.pull_down_en = GPIO_PULLDOWN_DISABLE;
        gpio.intr_type = GPIO_INTR_DISABLE;
        ESP_RETURN_ON_ERROR(gpio_config(&gpio), kTag, "Cannot configure amplifier CTRL");
        configured = true;
        ESP_LOGI(kTag, "CTRL GPIO10 configured once; subsequent playback only changes level");
    }
    return ESP_OK;
}

struct RecordingLevels {
    int32_t mean;
    float gain;
};

class SpeakerOutput {
public:
    ~SpeakerOutput() {
        const esp_err_t result = Close();
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "Cleanup failed: %s", esp_err_to_name(result));
        }
    }

    esp_err_t Initialize() {
        ESP_RETURN_ON_ERROR(InitializeAmplifierControl(), kTag, "Amplifier CTRL init failed");
        ctrl_ready_ = true;

        // Speaker TX and PDM RX use independent I2S controllers.
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
        ESP_LOGI(kTag, "I2S1 ready: 16000 Hz, 32-bit L/R slots; LEFT=silence, RIGHT=audio");
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

    static RecordingLevels AnalyzeRecording(const int16_t* samples, size_t count) {
        int64_t sum = 0;
        for (size_t i = 0; i < count; ++i) {
            sum += samples[i];
        }
        const int32_t mean = static_cast<int32_t>(sum / static_cast<int64_t>(count));
        int32_t peak = 0;
        size_t clipped = 0;
        double energy = 0;
        for (size_t i = 0; i < count; ++i) {
            const int32_t centered = static_cast<int32_t>(samples[i]) - mean;
            peak = std::max(peak, std::abs(centered));
            energy += static_cast<double>(centered) * centered;
            clipped += samples[i] == INT16_MIN || samples[i] == INT16_MAX;
        }
        // Remove DC and limit playback peak to the validated tone's 3% level.
        // Bound gain to avoid turning a disconnected/quiet input into loud noise.
        const float gain = peak > 0 ? std::min(8.0f, kPeak16Bit / peak) : 0.0f;
        ESP_LOGI(kTag, "CAPTURE: samples=%u, DC=%ld, peak=%ld, RMS=%.1f, clipped=%u, gain=%.2f",
                 static_cast<unsigned>(count), static_cast<long>(mean), static_cast<long>(peak),
                 std::sqrt(energy / count), static_cast<unsigned>(clipped),
                 static_cast<double>(gain));
        if (peak < 16) {
            ESP_LOGW(kTag, "Very low input: check PDM slot, microphone power and wiring");
        }
        return {mean, gain};
    }

    esp_err_t PlayRecording(const int16_t* samples, size_t count, const RecordingLevels& levels) {
        AudioBlock block = {};
        constexpr size_t fade_frames = kFadeMs * (kSampleRate / 1000);
        for (size_t offset = 0; offset < count;) {
            const size_t frames = std::min(count - offset, kFramesPerBlock);
            for (size_t frame = 0; frame < frames; ++frame) {
                const size_t index = offset + frame;
                const size_t edge = std::min(index, count - 1 - index);
                const float envelope = std::min(1.0f, static_cast<float>(edge) / fade_frames);
                const int32_t centered = static_cast<int32_t>(samples[index]) - levels.mean;
                const auto sample = static_cast<int32_t>(centered * levels.gain * envelope);
                block[frame * kChannels] = 0;
                block[frame * kChannels + 1] = sample * 65536;
            }
            ESP_RETURN_ON_ERROR(Write(block, frames), kTag, "Recorded audio write failed");
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

class MicrophoneInput {
public:
    ~MicrophoneInput() {
        if (Close() != ESP_OK) {
            ESP_LOGE(kTag, "PDM cleanup failed");
        }
    }

    esp_err_t Initialize() {
        i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        channel.dma_desc_num = 6;
        channel.dma_frame_num = 256;
        ESP_RETURN_ON_ERROR(i2s_new_channel(&channel, nullptr, &rx_), kTag,
                            "PDM allocation failed");
        i2s_pdm_rx_config_t config = {};
        config.clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(kSampleRate);
        config.slot_cfg =
            I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
#ifdef CONFIG_STUBUDDY_MIC_RIGHT_SLOT
        config.slot_cfg.slot_mask = I2S_PDM_SLOT_RIGHT;
#else
        config.slot_cfg.slot_mask = I2S_PDM_SLOT_LEFT;
#endif
        config.gpio_cfg.clk = stubuddy::kMicrophoneClock;
        config.gpio_cfg.din = stubuddy::kMicrophoneData;
        ESP_RETURN_ON_ERROR(i2s_channel_init_pdm_rx_mode(rx_, &config), kTag, "PDM init failed");
        ESP_RETURN_ON_ERROR(i2s_channel_enable(rx_), kTag, "PDM enable failed");
        running_ = true;
        ESP_LOGI(kTag, "PDM ready: I2S0 CLK=3 DATA=42, 16000 Hz PCM, slot=%s",
                 config.slot_cfg.slot_mask == I2S_PDM_SLOT_LEFT ? "LEFT" : "RIGHT");
        return ESP_OK;
    }

    esp_err_t Read(int16_t* samples, size_t count) {
        size_t offset = 0;
        while (offset < count) {
            const size_t requested = std::min<size_t>(count - offset, 256) * sizeof(int16_t);
            size_t received = 0;
            ESP_RETURN_ON_ERROR(i2s_channel_read(rx_, samples + offset, requested, &received, 1000),
                                kTag, "PDM read timeout/error");
            ESP_RETURN_ON_FALSE(received > 0 && received <= requested && received % 2 == 0,
                                ESP_ERR_INVALID_SIZE, kTag, "Invalid PDM read size");
            offset += received / sizeof(int16_t);
        }
        return ESP_OK;
    }

    esp_err_t Close() {
        if (running_) {
            const esp_err_t result = i2s_channel_disable(rx_);
            if (result != ESP_OK) {
                return result;
            }
            running_ = false;
        }
        if (rx_ != nullptr) {
            const esp_err_t result = i2s_del_channel(rx_);
            if (result != ESP_OK) {
                return result;
            }
            rx_ = nullptr;
        }
        return ESP_OK;
    }

private:
    i2s_chan_handle_t rx_ = nullptr;
    bool running_ = false;
};

struct RecordingDeleter {
    void operator()(int16_t* data) const { heap_caps_free(data); }
};

[[maybe_unused]] esp_err_t RecordAndReplay() {
    std::unique_ptr<int16_t, RecordingDeleter> recording(static_cast<int16_t*>(heap_caps_malloc(
        kRecordingSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    ESP_RETURN_ON_FALSE(recording != nullptr, ESP_ERR_NO_MEM, kTag,
                        "Cannot allocate 160000-byte PSRAM buffer");
    MicrophoneInput microphone;
    ESP_RETURN_ON_ERROR(microphone.Initialize(), kTag, "Microphone startup failed");
    // Discard 200 ms of samples to settle the microphone/PDM converter.
    std::array<int16_t, 160> discard = {};
    for (int i = 0; i < 20; ++i) {
        ESP_RETURN_ON_ERROR(microphone.Read(discard.data(), discard.size()), kTag,
                            "PDM warmup failed");
    }
    status_led.SetState(LedState::Recording);
    ESP_LOGI(kTag, "REC START: speak now, 5 seconds; amplifier OFF");
    ESP_RETURN_ON_ERROR(microphone.Read(recording.get(), kRecordingSamples), kTag,
                        "Recording failed");
    ESP_RETURN_ON_ERROR(microphone.Close(), kTag, "Cannot stop microphone");
    status_led.SetState(LedState::Idle);
    ESP_LOGI(kTag, "REC STOP: 80000 samples / 160000 bytes; microphone OFF");

    const RecordingLevels levels =
        SpeakerOutput::AnalyzeRecording(recording.get(), kRecordingSamples);
    SpeakerOutput speaker;
    ESP_RETURN_ON_ERROR(speaker.Initialize(), kTag, "Replay output init failed");
    ESP_RETURN_ON_ERROR(speaker.EnableAmplifier(), kTag, "Replay amplifier startup failed");
    status_led.SetState(LedState::Playback);
    ESP_LOGI(kTag, "REPLAY START: playing the 5-second recording once");
    ESP_RETURN_ON_ERROR(speaker.PlayRecording(recording.get(), kRecordingSamples, levels), kTag,
                        "Replay failed");
    ESP_RETURN_ON_ERROR(speaker.Finish(), kTag, "Replay shutdown failed");
    status_led.SetState(LedState::Idle);
    ESP_LOGI(kTag, "REPLAY STOP: amplifier OFF; recording buffer released on return");
    return ESP_OK;
}

unsigned Milliseconds() { return static_cast<unsigned>(esp_timer_get_time() / 1000); }

bool RawButtonPressed() { return gpio_get_level(stubuddy::kMainButton) == 0; }

void RefreshLocalLed() { status_led.SetState(stubuddy::LocalWaitingLedState(RawButtonPressed())); }

[[maybe_unused]] esp_err_t ButtonRecordAndReplay() {
    constexpr size_t max_samples = kSampleRate * kPttMaxSeconds;
    constexpr size_t recording_bytes = max_samples * sizeof(int16_t);
    constexpr size_t min_samples = kSampleRate * 300 / 1000;
    constexpr size_t poll_samples = kSampleRate / 100;
    std::unique_ptr<int16_t, RecordingDeleter> recording(static_cast<int16_t*>(
        heap_caps_malloc(recording_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    ESP_RETURN_ON_FALSE(recording != nullptr, ESP_ERR_NO_MEM, kTag,
                        "Cannot allocate %u-byte PTT recording buffer",
                        static_cast<unsigned>(recording_bytes));
    ESP_LOGI(kTag, "PTT CONFIG: max=%u seconds, buffer=%u bytes; timeout=auto-replay",
             static_cast<unsigned>(kPttMaxSeconds), static_cast<unsigned>(recording_bytes));

    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << stubuddy::kMainButton;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&config), kTag, "GPIO15 button init failed");
    stubuddy::PttButton button;
    std::array<int16_t, poll_samples> discard = {};
    unsigned round = 0;

    while (true) {
        RefreshLocalLed();
        MicrophoneInput microphone;
        ESP_RETURN_ON_ERROR(microphone.Initialize(), kTag, "PTT microphone startup failed");
        // Warm up before arming the key, so the first word isn't discarded.
        // Idle samples are drained but never retained in the recording buffer.
        for (int i = 0; i < 20; ++i) {
            ESP_RETURN_ON_ERROR(microphone.Read(discard.data(), discard.size()), kTag,
                                "PTT microphone warmup failed");
            RefreshLocalLed();
        }
        ESP_LOGI(kTag, "WAIT RELEASE: release GPIO15 before the next recording");
        button.RequireRelease(RawButtonPressed(), Milliseconds());
        while (button.pressed()) {
            ESP_RETURN_ON_ERROR(microphone.Read(discard.data(), discard.size()), kTag,
                                "PTT release gate read failed");
            button.Update(RawButtonPressed(), Milliseconds());
            RefreshLocalLed();
        }
        status_led.SetState(LedState::Idle);
        ESP_LOGI(kTag, "IDLE: hold GPIO15 to record; release to replay (0.3-%u seconds)",
                 static_cast<unsigned>(kPttMaxSeconds));
        while (!button.pressed()) {
            ESP_RETURN_ON_ERROR(microphone.Read(discard.data(), discard.size()), kTag,
                                "PTT idle read failed");
            button.Update(RawButtonPressed(), Milliseconds());
            RefreshLocalLed();
        }

        size_t count = 0;
        status_led.SetState(LedState::Recording);
        ESP_LOGI(kTag, "REC START: round=%u; amplifier OFF", ++round);
        while (count < max_samples && button.pressed()) {
            const size_t frames = std::min(poll_samples, max_samples - count);
            ESP_RETURN_ON_ERROR(microphone.Read(recording.get() + count, frames), kTag,
                                "PTT recording failed");
            count += frames;
            button.Update(RawButtonPressed(), Milliseconds());
        }
        ESP_RETURN_ON_ERROR(microphone.Close(), kTag, "Cannot stop PTT microphone");
        // Recording has stopped. Preparation stays dark even if the timed-out
        // press is still held; the lamp turns on only when PCM playback begins.
        status_led.SetState(LedState::Idle);
        const bool limit_reached = count == max_samples;
        ESP_LOGI(kTag, "REC STOP: samples=%u, duration=%u ms, reason=%s; microphone OFF",
                 static_cast<unsigned>(count), static_cast<unsigned>(count * 1000 / kSampleRate),
                 limit_reached ? "LIMIT" : "RELEASE");
        if (limit_reached) {
            ESP_LOGI(kTag, "LIMIT: %u seconds reached; microphone OFF; LED OFF; automatic replay",
                     static_cast<unsigned>(kPttMaxSeconds));
        }
        if (count < min_samples) {
            ESP_LOGI(kTag, "DROP: recording shorter than 300 ms; no replay");
        } else {
            const RecordingLevels levels = SpeakerOutput::AnalyzeRecording(recording.get(), count);
            SpeakerOutput speaker;
            ESP_RETURN_ON_ERROR(speaker.Initialize(), kTag, "PTT replay output init failed");
            ESP_RETURN_ON_ERROR(speaker.EnableAmplifier(), kTag, "PTT amplifier startup failed");
            status_led.SetState(LedState::Playback);
            ESP_LOGI(kTag, "REPLAY START: round=%u; button presses ignored until next IDLE", round);
            ESP_RETURN_ON_ERROR(speaker.PlayRecording(recording.get(), count, levels), kTag,
                                "PTT replay failed");
            ESP_RETURN_ON_ERROR(speaker.Finish(), kTag, "PTT replay shutdown failed");
            RefreshLocalLed();
            ESP_LOGI(kTag, "REPLAY STOP: amplifier OFF");
        }
        // Reuse a single bounded PSRAM allocation across rounds and erase used audio.
        std::memset(recording.get(), 0, count * sizeof(int16_t));
        ESP_LOGI(kTag, "ROUND DONE: %u; internal_free=%u, PSRAM_free=%u", round,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        // A fresh release gate drops presses made during playback, including a
        // button held down across playback completion. It never starts a new recording.
    }
}

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

    ESP_LOGI(kTag, "START in 2 seconds; fixed 3%% prompt peak; no networking");
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_RETURN_ON_ERROR(speaker.EnableAmplifier(), kTag, "Amplifier startup failed");
#ifdef CONFIG_STUBUDDY_AUDIO_LOOPBACK
#ifdef CONFIG_STUBUDDY_BUTTON_PTT
    ESP_LOGI(kTag, "PROMPT: wait for IDLE, then hold GPIO15 to record");
#else
    ESP_LOGI(kTag, "PROMPT: recording will begin after the tone and a short silence");
#endif
    status_led.SetState(LedState::Playback);
    ESP_RETURN_ON_ERROR(speaker.WriteTone(660.0f), kTag, "Prompt failed");
    ESP_RETURN_ON_ERROR(speaker.Finish(), kTag, "Prompt shutdown failed");
    status_led.SetState(LedState::Idle);
    vTaskDelay(pdMS_TO_TICKS(300));
#ifdef CONFIG_STUBUDDY_BUTTON_PTT
    return ButtonRecordAndReplay();
#else
    return RecordAndReplay();
#endif
#else
    for (int repetition = 1; repetition <= kRepetitions; ++repetition) {
        status_led.SetState(LedState::Playback);
        ESP_LOGI(kTag, "PLAY %d/%d: 440 Hz -> 660 Hz", repetition, kRepetitions);
        ESP_RETURN_ON_ERROR(speaker.WriteTone(440.0f), kTag, "First tone failed");
        ESP_RETURN_ON_ERROR(speaker.WriteSilence(80), kTag, "Tone gap failed");
        ESP_RETURN_ON_ERROR(speaker.WriteTone(660.0f), kTag, "Second tone failed");
        if (repetition < kRepetitions) {
            status_led.SetState(LedState::Idle);
            ESP_RETURN_ON_ERROR(speaker.WriteSilence(2000), kTag, "Repeat gap failed");
        }
    }
    return speaker.Finish();
#endif
}

}  // namespace

extern "C" void app_main() {
    const esp_err_t led_result = status_led.Initialize();
    if (led_result != ESP_OK) {
        ESP_LOGE(kTag, "LED init failed: %s; audio test not started", esp_err_to_name(led_result));
        return;
    }
#if defined(CONFIG_STUBUDDY_AUDIO_LOOPBACK) && defined(CONFIG_STUBUDDY_BUTTON_PTT)
    ESP_LOGI(kTag, "stubuddy-s3-n16r8 audio self-test v0.4.3 / BUTTON-PTT / speaker RIGHT");
#elif defined(CONFIG_STUBUDDY_AUDIO_LOOPBACK)
    ESP_LOGI(kTag, "stubuddy-s3-n16r8 audio self-test v0.4.3 / RECORD-REPLAY / speaker RIGHT");
#else
    ESP_LOGI(kTag, "stubuddy-s3-n16r8 audio self-test v0.4.3 / THREE-TONE / speaker RIGHT");
#endif
#ifdef CONFIG_STUBUDDY_LED_ERROR_DEMO
    status_led.SetState(LedState::Error);
    ESP_LOGW(kTag, "LED ERROR DEMO: audio skipped; disable demo in menuconfig to resume");
    return;
#endif
    const esp_err_t result = RunSelfTest();
    if (result == ESP_OK) {
        status_led.SetState(LedState::Idle);
        ESP_LOGI(kTag, "DONE: amplifier off; waiting for reset (audibility is a manual check)");
    } else {
        status_led.SetState(LedState::Error);
        ESP_LOGE(kTag, "FAIL: %s; test stopped, no automatic retry", esp_err_to_name(result));
    }
    // Returning deletes the main task. Idle tasks continue until reset/power cycle.
}
