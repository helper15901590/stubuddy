#include "audio_codec.h"
#include "board.h"
#include "config.h"
#include "led_pattern.h"
#include "ptt_button.h"
#include "ptt_playback.h"
#include "recording_pcm.h"
#include "startup_audio.h"
#ifdef CONFIG_STUBUDDY_OPUS_LOOPBACK
#include "opus_loopback.h"
#endif

#ifdef CONFIG_STUBUDDY_STATUS_LED
#include "status_led.h"
#endif
#ifdef CONFIG_STUBUDDY_SOFTAP_SETUP
#include "network_audio.h"
#include "wifi_audio_rules.h"
#include "wifi_setup.h"
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
#ifdef CONFIG_STUBUDDY_SOFTAP_SETUP
stubuddy::WifiSetup wifi_setup;
#endif
#ifdef CONFIG_STUBUDDY_OPUS_LOOPBACK
constexpr char kCheckpoint[] = "3.5";
stubuddy::OpusLoopback opus_loopback;
#elif defined(CONFIG_STUBUDDY_SOFTAP_SETUP)
constexpr char kCheckpoint[] = "3.4";
#elif defined(CONFIG_STUBUDDY_STATUS_LED)
constexpr char kCheckpoint[] = "3.3";
#else
constexpr char kCheckpoint[] = "3.2";
#endif
#ifdef CONFIG_STUBUDDY_STATUS_LED
constexpr char kLedMode[] = "stateful";
stubuddy::StatusLed status_led;
#else
constexpr char kLedMode[] = "OFF";
// Preserve the validated 3.2 variant, without linking the LEDC driver.
struct NoStatusLed {
    esp_err_t Initialize() { return ESP_OK; }
    esp_err_t ShowCaptureStopped(unsigned) { return ESP_OK; }
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
    static bool configured = false;
    if (configured)
        return ESP_OK;
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << stubuddy::kMainButton;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    const esp_err_t result = gpio_config(&config);
    configured = result == ESP_OK;
    return result;
}

#ifdef CONFIG_STUBUDDY_SOFTAP_SETUP
esp_err_t CaptureBootSetupRequest(bool& force_setup) {
    ESP_RETURN_ON_ERROR(InitializeButton(), kTag, "Boot key setup failed");
    const bool initially_pressed = RawButtonPressed();
    if (initially_pressed)
        vTaskDelay(pdMS_TO_TICKS(stubuddy::PttButton::kDebounceMs));
    force_setup = stubuddy::BootSetupRequested(initially_pressed, RawButtonPressed());
    ESP_LOGI(kTag, "BOOT BUTTON: setup=%s; sampled before startup prompt; history retained",
             force_setup ? "FORCED" : "NORMAL");
    return ESP_OK;
}

esp_err_t AwaitNetworkReady(AudioCodec& codec) {
    static bool first_ready = true;
    bool waited = false;
    bool handled_cue = false;
    while (true) {
        ESP_RETURN_ON_FALSE(!wifi_setup.failed(), ESP_FAIL, kTag, "WiFi controller fault");
        if (!wifi_setup.online() && !waited) {
            waited = true;
            ESP_LOGI(kTag, "NETWORK WAIT: new recordings paused; microphone OFF");
        }
        // One audio owner plays notifications. Retain failures through a rapid
        // retry; ordinary link updates still coalesce during long recordings.
        const auto cue = wifi_setup.TakeCue();
        if (cue != stubuddy::WifiCue::None) {
            handled_cue = true;
            ESP_RETURN_ON_ERROR(stubuddy::PlayWifiCue(codec, status_led, cue), kTag,
                                "WiFi prompt failed");
        }
        if (wifi_setup.online() && !wifi_setup.has_cue()) {
            if (first_ready || waited || handled_cue)
                ESP_LOGI(kTag, "NETWORK READY: local PTT available after a fresh release gate");
            first_ready = false;
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
#endif

unsigned NetworkEpoch() {
#ifdef CONFIG_STUBUDDY_SOFTAP_SETUP
    return wifi_setup.link_epoch();
#else
    return 0;
#endif
}

bool IdleSessionCurrent(unsigned epoch) {
#ifdef CONFIG_STUBUDDY_SOFTAP_SETUP
    const bool online = wifi_setup.online();
    const unsigned current_epoch = wifi_setup.link_epoch();
    return stubuddy::NetworkRecordingAllowed(online, epoch, current_epoch, wifi_setup.has_cue());
#else
    (void)epoch;
    return true;
#endif
}

enum class ReplayOutcome { Finished, Interrupted, CaptureStarted };

esp_err_t Replay(AudioCodec& codec, const int16_t* recording, size_t count,
                 std::vector<int16_t>& block, unsigned round, stubuddy::PttButton& button,
                 ReplayOutcome& outcome) {
    outcome = ReplayOutcome::Finished;
#ifdef CONFIG_STUBUDDY_SOFTAP_SETUP
    stubuddy::PlaybackInterrupt interrupt(button.pressed());
#else
    (void)button;
#endif
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
#ifdef CONFIG_STUBUDDY_SOFTAP_SETUP
    ESP_LOGI(kTag, "REPLAY START: round=%u; fresh press interrupts and starts held-key capture",
             round);
#else
    ESP_LOGI(kTag, "REPLAY START: round=%u; button presses ignored until next IDLE", round);
#endif
    for (size_t offset = 0; offset < count;) {
#ifdef CONFIG_STUBUDDY_SOFTAP_SETUP
        button.Update(RawButtonPressed(), Milliseconds());
        if (interrupt.Update(button.pressed())) {
            const unsigned handoff_start = Milliseconds();
            codec.AbortOutput();
#ifdef CONFIG_STUBUDDY_OPUS_LOOPBACK
            opus_loopback.Discard();
            ESP_LOGI(kTag, "OPUS DISCARD: old packet stream invalidated; no old audio resumes");
#endif
            status_led.SetPhase(stubuddy::PttLedPhase::ReleaseGate);
            ESP_RETURN_ON_FALSE(!codec.HasOutputError(), ESP_FAIL, kTag, "Replay abort failed");
            outcome = ReplayOutcome::Interrupted;
            ESP_LOGI(kTag, "REPLAY INTERRUPTED: round=%u; old output/DMA discarded", round);
            // A fresh user press outranks queued status sounds. Require a
            // continuous online link, but leave those sounds queued until idle.
            const unsigned new_epoch = NetworkEpoch();
            const bool online = wifi_setup.online();
            const unsigned current_epoch = NetworkEpoch();
            if (stubuddy::NetworkRecordingAllowed(online, new_epoch, current_epoch, false)) {
                codec.EnableInput(true);
                ESP_RETURN_ON_FALSE(codec.input_enabled() && !codec.HasInputError(), ESP_FAIL, kTag,
                                    "Cannot capture after replay interruption");
                status_led.SetPhase(stubuddy::PttLedPhase::Recording);
                outcome = ReplayOutcome::CaptureStarted;
                ESP_LOGI(kTag,
                         "INPUT HANDOFF: %u ms after abort; keep holding; release ends capture",
                         Milliseconds() - handoff_start);
            } else {
                ESP_LOGI(kTag, "INPUT HANDOFF DEFERRED: network not ready; fresh release required");
            }
            return ESP_OK;
        }
#endif
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
#ifdef CONFIG_STUBUDDY_OPUS_LOOPBACK
    ESP_RETURN_ON_ERROR(opus_loopback.Initialize(), kTag, "Opus loopback initialization failed");
#endif
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
    bool continue_capture = false;
    size_t dirty_samples = 0;
    while (true) {
        if (!continue_capture) {
            // Keep release gates dark. Reading the raw held key here must never
            // relight the LED or arm another recording after playback completion.
            status_led.SetPhase(stubuddy::PttLedPhase::ReleaseGate);
#ifdef CONFIG_STUBUDDY_SOFTAP_SETUP
            ESP_RETURN_ON_ERROR(AwaitNetworkReady(codec), kTag, "Network wait failed");
#endif
            const unsigned epoch = NetworkEpoch();
            block.resize(kPollSamples);
            codec.EnableInput(true);
            ESP_RETURN_ON_FALSE(codec.input_enabled() && !codec.HasInputError(), ESP_FAIL, kTag,
                                "Cannot start PDM input");
            // Warm up before arming the key, avoiding loss of the child's first word.
            // Idle audio is drained and discarded, never stored in the PSRAM recording.
            for (int i = 0; i < 20 && IdleSessionCurrent(epoch); ++i)
                ESP_RETURN_ON_FALSE(codec.InputData(block), ESP_FAIL, kTag, "PDM warmup failed");
            ESP_LOGI(kTag, "WAIT RELEASE: release GPIO15 before the next recording; LED OFF");
            button.RequireRelease(RawButtonPressed(), Milliseconds());
            while (button.pressed() && IdleSessionCurrent(epoch)) {
                ESP_RETURN_ON_FALSE(codec.InputData(block), ESP_FAIL, kTag,
                                    "Release gate read failed");
                button.Update(RawButtonPressed(), Milliseconds());
            }
            if (IdleSessionCurrent(epoch)) {
                status_led.SetPhase(stubuddy::PttLedPhase::ArmedIdle);
                ESP_LOGI(
                    kTag,
                    "IDLE: hold GPIO15 to record; release to replay (0.3-60 seconds); LED OFF");
            }
            while (!button.pressed() && IdleSessionCurrent(epoch)) {
                ESP_RETURN_ON_FALSE(codec.InputData(block), ESP_FAIL, kTag, "Idle read failed");
                button.Update(RawButtonPressed(), Milliseconds());
                if (IdleSessionCurrent(epoch))
                    status_led.SetPhase(stubuddy::PttLedPhase::ArmedIdle, button.pressed());
            }

            if (!IdleSessionCurrent(epoch)) {
                codec.EnableInput(false);
                status_led.SetPhase(stubuddy::PttLedPhase::ReleaseGate);
                ESP_RETURN_ON_FALSE(!codec.HasInputError(), ESP_FAIL, kTag, "Cannot pause input");
                ESP_LOGI(kTag,
                         "NETWORK PAUSE: idle session revoked; release gate reset; microphone OFF");
                continue;
            }
        } else {
            // Replay already opened RX on the new press. Do not discard 200ms
            // or wait for release here: that would lose the child's first word.
            continue_capture = false;
            block.resize(kPollSamples);
        }

        size_t count = 0;
#ifdef CONFIG_STUBUDDY_OPUS_LOOPBACK
        ESP_RETURN_ON_ERROR(opus_loopback.Begin(round + 1), kTag, "Opus round reset failed");
#endif
        // Admission is complete. From here through replay, link changes never
        // cut the current round short; only the next idle session is gated.
        status_led.SetPhase(stubuddy::PttLedPhase::Recording);
        ESP_LOGI(kTag, "REC START: round=%u; amplifier OFF; LED=%s", ++round, kLedMode);
        while (count < kMaxSamples && button.pressed()) {
            block.resize(std::min(kPollSamples, kMaxSamples - count));
            ESP_RETURN_ON_FALSE(codec.InputData(block), ESP_FAIL, kTag, "Recording read failed");
            std::memcpy(recording.get() + count, block.data(), block.size() * sizeof(int16_t));
            count += block.size();
#ifdef CONFIG_STUBUDDY_OPUS_LOOPBACK
            ESP_RETURN_ON_ERROR(opus_loopback.Append(block.data(), block.size()), kTag,
                                "Opus capture encode failed");
#endif
            button.Update(RawButtonPressed(), Milliseconds());
        }
        // Close RX before analyzing or enabling TX, including the 60-second case.
        codec.EnableInput(false);
        ESP_RETURN_ON_FALSE(!codec.HasInputError(), ESP_FAIL, kTag, "Microphone shutdown failed");
        const bool limit = count == kMaxSamples;
        dirty_samples = std::max(dirty_samples, count);
        ESP_RETURN_ON_ERROR(status_led.ShowCaptureStopped(limit ? 150 : 0), kTag,
                            "Capture-stop LED output acknowledgement failed");
        ESP_LOGI(kTag, "REC STOP: samples=%u, duration=%u ms, reason=%s; microphone OFF",
                 unsigned(count), unsigned(count * 1000 / stubuddy::kSampleRate),
                 limit ? "LIMIT" : "RELEASE");
        if (limit)
            ESP_LOGI(kTag, "LIMIT: 60 seconds reached; LED OFF applied + 150ms; automatic replay");
        ReplayOutcome outcome = ReplayOutcome::Finished;
        if (count < kMinSamples)
            ESP_LOGI(kTag, "DROP: recording shorter than 300 ms; no replay");
        else {
#ifdef CONFIG_STUBUDDY_OPUS_LOOPBACK
            ESP_RETURN_ON_ERROR(opus_loopback.FinishAndDecode(recording.get(), count), kTag,
                                "Opus decode failed; raw PCM is never replayed as a fallback");
#endif
            ESP_RETURN_ON_ERROR(
                Replay(codec, recording.get(), count, block, round, button, outcome), kTag,
                "Replay failed");
        }
        if (outcome == ReplayOutcome::CaptureStarted) {
            // Reuse the same bounded buffer immediately. The next capture only
            // plays its own count; clear the old tail when the chain finishes.
            continue_capture = true;
            ESP_LOGI(kTag,
                     "ROUND INTERRUPTED: %u; microphone already ON; old replay will not resume",
                     round);
            continue;
        }
        std::memset(recording.get(), 0, dirty_samples * sizeof(int16_t));
        dirty_samples = 0;
#ifdef CONFIG_STUBUDDY_OPUS_LOOPBACK
        opus_loopback.Erase();
#endif
        std::fill(block.begin(), block.end(), 0);
        ESP_LOGI(kTag, "ROUND DONE: %u; internal_free=%u, PSRAM_free=%u; recording erased", round,
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        // Normal completion still requires a fresh release. Only a verified
        // playback interruption carries the new press directly into capture.
    }
}
}  // namespace

extern "C" void app_main() {
    auto& board = Board::GetInstance();
#ifdef CONFIG_STUBUDDY_SOFTAP_SETUP
    bool force_setup = false;
    if (CaptureBootSetupRequest(force_setup) != ESP_OK) {
        ESP_LOGE(kTag, "FAILED: boot key setup; WiFi/PTT not started");
        return;
    }
#endif
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
#ifdef CONFIG_STUBUDDY_SOFTAP_SETUP
        if (wifi_setup.Start(status_led, force_setup) != ESP_OK) {
            status_led.SetState(stubuddy::LedState::Error);
            ESP_LOGE(kTag, "FAILED: WiFi startup; original NVS retained");
            return;
        }
#endif
        const esp_err_t result = RunPtt(*codec);
#ifdef CONFIG_STUBUDDY_OPUS_LOOPBACK
        // RunPtt has stopped both I2S directions. Clear any partial encoded
        // recording on the failure path as well as on normal round completion.
        opus_loopback.Erase();
#endif
        status_led.SetState(stubuddy::LedState::Error);
        ESP_LOGE(kTag, "FAILED: %s; PTT stopped, reset required", esp_err_to_name(result));
    }
}
