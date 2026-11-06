#include "audio_codec.h"
#include "board.h"
#include "config.h"
#include "led_pattern.h"
#include "ptt_button.h"
#include "ptt_playback.h"
#include "recording_pcm.h"
#include "startup_audio.h"
#ifdef CONFIG_STUBUDDY_COUNTDOWN
#include <cmath>
#include <ctime>
#include "audio_levels.h"
#include "countdown.h"
#include "reminder_cycle.h"
#ifdef CONFIG_STUBUDDY_SCHEDULED
#include "scheduled_reminders.h"
#endif
#endif
#ifdef CONFIG_STUBUDDY_AI_PTT
#include "stubuddy_audio_codec.h"
#endif
#ifdef CONFIG_STUBUDDY_AI_PTT
#include "ai_stream.h"
#endif
#ifdef CONFIG_STUBUDDY_OPUS_LOOPBACK
#include "opus_loopback.h"
#endif
#ifdef CONFIG_STUBUDDY_WS_ECHO
#include "ws_echo.h"
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
#ifdef CONFIG_STUBUDDY_WS_ECHO
#ifdef CONFIG_STUBUDDY_COUNTDOWN
#ifdef CONFIG_STUBUDDY_SCHEDULED
constexpr char kCheckpoint[] = "4.3.2 (one countdown, three weekly/once schedules)";
stubuddy::ScheduledReminders countdown;
#else
constexpr char kCheckpoint[] = "4.3.1 (single countdown; independent reminder volume)";
stubuddy::Countdown countdown;
#endif
bool countdown_boot_pending = false;
#elif defined(CONFIG_STUBUDDY_AI_CONTROLS)
constexpr char kCheckpoint[] = "4.2 (PTT volume tools and speech LED)";
#elif defined(CONFIG_STUBUDDY_AI_PTT)
constexpr char kCheckpoint[] = "4.1 (merged ASR/LLM/TTS)";
#elif defined(CONFIG_STUBUDDY_WSS_ECHO)
constexpr char kCheckpoint[] = "3.6.2";
#else
constexpr char kCheckpoint[] = "3.6.1";
#endif
stubuddy::OpusLoopback opus_loopback;
stubuddy::WsEcho ws_echo;
#elif defined(CONFIG_STUBUDDY_OPUS_LOOPBACK)
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

#ifdef CONFIG_STUBUDDY_COUNTDOWN
bool CountdownDue() {
    const auto now = std::time(nullptr);
    if (!stubuddy::CountdownState::TimeReady(now))
        return false;
    const bool boot_pending = countdown_boot_pending;
    countdown_boot_pending = false;
#ifdef CONFIG_STUBUDDY_SCHEDULED
    while (countdown.Due()) {
        int64_t first = INT64_MAX;
        for (const auto& entry : countdown.state().entries)
            if (entry.deadline && entry.deadline <= now)
                first = std::min(first, entry.deadline);
        bool cached = true;
        for (const auto& entry : countdown.state().entries)
            if (entry.deadline == first)
                cached &= ws_echo.ReminderReady(entry.id);
        if (!boot_pending && wifi_setup.online() && ws_echo.ready() && cached)
            return true;
        std::array<stubuddy::ReminderEntry, 4> skipped;
        unsigned count = 0;
        countdown.Claim(skipped, count);
        ESP_LOGI("STUBUDDY_TIMER",
                 "SKIPPED: count=%u reason=%s; repeating plans advance; no catch-up", count,
                 boot_pending ? "powered_off" : "offline_or_cache_unavailable");
    }
    return false;
#else
    const auto decision =
        countdown.state().Decision(now, wifi_setup.online() && ws_echo.ready(),
                                   ws_echo.ReminderReady(countdown.state().id), boot_pending);
    if (decision == stubuddy::CountdownDecision::Skip) {
        countdown.Consume();
        ESP_LOGI("STUBUDDY_TIMER", "SKIPPED: %s; deleted; no catch-up",
                 boot_pending ? "expired while powered off"
                              : "offline or voice unavailable at deadline");
    }
    return decision == stubuddy::CountdownDecision::Play;
#endif
}
#ifdef CONFIG_STUBUDDY_SCHEDULED
void SyncReminders() {
    static std::array<unsigned, 4> epochs{};
    static std::array<std::array<char, 17>, 4> ids{};
    const auto& entries = countdown.state().entries;
    ws_echo.KeepReminders(entries);
    if (!ws_echo.ready())
        return;
    for (unsigned i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        if (!entry.deadline || ws_echo.ReminderReady(entry.id) ||
            (epochs[i] == ws_echo.epoch() && !std::strcmp(ids[i].data(), entry.id)))
            continue;
        stubuddy::CountdownState clip;
        std::strcpy(clip.id, entry.id);
        std::strcpy(clip.text, entry.text);
        ws_echo.SyncCountdown(clip);
        std::strcpy(ids[i].data(), entry.id);
        epochs[i] = ws_echo.epoch();
    }
}
#endif
#endif

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
#ifdef CONFIG_STUBUDDY_AI_CONTROLS
    bool connected_cue_pending = false;
#endif
    while (true) {
#ifdef CONFIG_STUBUDDY_COUNTDOWN
        if (CountdownDue())
            return ESP_OK;
#endif
        ESP_RETURN_ON_FALSE(!wifi_setup.failed(), ESP_FAIL, kTag, "WiFi controller fault");
        if (!wifi_setup.online() && !waited) {
            waited = true;
            ESP_LOGI(kTag, "NETWORK WAIT: new recordings paused; microphone OFF");
        }
        // One audio owner plays notifications. Retain failures through a rapid
        // retry; ordinary link updates still coalesce during long recordings.
        auto cue = wifi_setup.TakeCue();
#ifdef CONFIG_STUBUDDY_AI_CONTROLS
        if (!wifi_setup.online())
            connected_cue_pending = false;
        if (cue == stubuddy::WifiCue::Connected) {
            connected_cue_pending = true;
            cue = stubuddy::WifiCue::None;
        }
#endif
        if (cue != stubuddy::WifiCue::None) {
            handled_cue = true;
            ESP_RETURN_ON_ERROR(stubuddy::PlayWifiCue(codec, status_led, cue), kTag,
                                "WiFi prompt failed");
        }
#ifdef CONFIG_STUBUDDY_WS_ECHO
        if (ws_echo.TakeFaultNotice()) {
            handled_cue = true;
            ESP_RETURN_ON_ERROR(stubuddy::PlayWifiCue(codec, status_led, stubuddy::WifiCue::Failed),
                                kTag, "WS failure cue failed");
        }
        const bool service_ready = ws_echo.ready();
        if (wifi_setup.online() && !service_ready)
            status_led.SetState(stubuddy::LedState::ModelWaiting);
#else
        const bool service_ready = true;
#endif
        if (wifi_setup.online() && !wifi_setup.has_cue() && service_ready) {
#ifdef CONFIG_STUBUDDY_AI_CONTROLS
            if (connected_cue_pending) {
                connected_cue_pending = false;
                handled_cue = true;
                ESP_RETURN_ON_ERROR(
                    stubuddy::PlayWifiCue(codec, status_led, stubuddy::WifiCue::Connected), kTag,
                    "AI ready prompt failed");
                if (!wifi_setup.online() || !ws_echo.ready())
                    continue;
            }
#endif
#ifdef CONFIG_STUBUDDY_WS_ECHO
            status_led.SetState(stubuddy::LedState::Idle);
#endif
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
#ifdef CONFIG_STUBUDDY_WS_ECHO
    return wifi_setup.link_epoch() + ws_echo.epoch();
#elif defined(CONFIG_STUBUDDY_SOFTAP_SETUP)
    return wifi_setup.link_epoch();
#else
    return 0;
#endif
}

bool IdleSessionCurrent(unsigned epoch) {
#ifdef CONFIG_STUBUDDY_SOFTAP_SETUP
    const bool online = wifi_setup.online()
#ifdef CONFIG_STUBUDDY_WS_ECHO
                        && ws_echo.ready()
#endif
        ;
    const unsigned current_epoch = NetworkEpoch();
    return stubuddy::NetworkRecordingAllowed(online, epoch, current_epoch, wifi_setup.has_cue());
#else
    (void)epoch;
    return true;
#endif
}

enum class ReplayOutcome { Finished, Interrupted, CaptureStarted, FreshPressReady, Reminder };

#ifdef CONFIG_STUBUDDY_WS_ECHO
// Called only by the audio owner, including while awaiting/decoding the echo.
esp_err_t InterruptWaitingEcho(AudioCodec& codec, ReplayOutcome& outcome) {
    ws_echo.Cancel();
    opus_loopback.Discard();
    outcome = ReplayOutcome::Interrupted;
    status_led.SetPhase(stubuddy::PttLedPhase::ReleaseGate);
    if (wifi_setup.online() && ws_echo.ready()) {
        const unsigned started = Milliseconds();
        codec.EnableInput(true);
        ESP_RETURN_ON_FALSE(codec.input_enabled() && !codec.HasInputError(), ESP_FAIL, kTag,
                            "Cannot capture after echo wait interruption");
        status_led.SetPhase(stubuddy::PttLedPhase::Recording);
        outcome = ReplayOutcome::CaptureStarted;
        ESP_LOGI(kTag, "INPUT HANDOFF: %u ms; old echo cancelled; keep holding to record",
                 Milliseconds() - started);
    }
    return ESP_OK;
}

#ifndef CONFIG_STUBUDDY_AI_PTT
esp_err_t ReceiveEcho(AudioCodec& codec, int16_t* recording, size_t samples, size_t frames,
                      stubuddy::PttButton& button, ReplayOutcome& outcome, bool& decoded,
                      size_t& playback_samples) {
    decoded = false;
    if (!ws_echo.Finish(samples, opus_loopback.delay(), frames))
        return ESP_OK;  // Recoverable network failure. Never use local PCM.
    status_led.SetState(stubuddy::LedState::ModelWaiting);
    stubuddy::PlaybackInterrupt interrupt(button.pressed());
#ifdef CONFIG_STUBUDDY_AI_PTT
    const unsigned waiting_started = Milliseconds();
#endif
    while (!ws_echo.done()) {
        button.Update(RawButtonPressed(), Milliseconds());
        if (interrupt.Update(button.pressed()))
            return InterruptWaitingEcho(codec, outcome);
        if (ws_echo.round_failed())
            return ESP_OK;
        const unsigned progress_age = ws_echo.ProgressAgeMs();
        if (
#ifdef CONFIG_STUBUDDY_AI_PTT
            progress_age >= 45000 || Milliseconds() - waiting_started >= 95000
#else
            progress_age >= stubuddy::echo::kProgressTimeoutMs
#endif
        ) {
            ws_echo.Timeout("echo_progress", progress_age, stubuddy::echo::kProgressTimeoutMs);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
#ifdef CONFIG_STUBUDDY_AI_PTT
    if (ws_echo.round_empty())
        return ESP_OK;
    playback_samples = ws_echo.reply_samples();
#else
    playback_samples = samples;
#endif
    struct DecodeContext {
        stubuddy::PttButton& button;
        stubuddy::PlaybackInterrupt& interrupt;
        bool cancelled = false;
    } context{button, interrupt};
    const auto proceed = [](void* raw) {
        auto& state = *static_cast<DecodeContext*>(raw);
        state.button.Update(RawButtonPressed(), Milliseconds());
        state.cancelled = state.interrupt.Update(state.button.pressed());
        return !state.cancelled;
    };
#ifdef CONFIG_STUBUDDY_AI_PTT
    const esp_err_t result = opus_loopback.DecodeReply(recording, playback_samples,
                                                       ws_echo.reply_delay(), proceed, &context);
#else
    const esp_err_t result = opus_loopback.DecodeEcho(recording, samples, proceed, &context);
#endif
    if (context.cancelled)
        return InterruptWaitingEcho(codec, outcome);
    ESP_RETURN_ON_ERROR(result, kTag, "Returned Opus decode failed");
    decoded = true;
    return ESP_OK;
}
#endif
#endif

#ifdef CONFIG_STUBUDDY_AI_PTT
esp_err_t StreamReply(AudioCodec& codec, int16_t* pcm, size_t samples, stubuddy::PttButton& button,
                      std::vector<int16_t>& block, ReplayOutcome& outcome, size_t& dirty_samples) {
    if (!ws_echo.Finish(samples, opus_loopback.delay(), opus_loopback.frames()))
        return ESP_OK;
#ifdef CONFIG_STUBUDDY_AI_CONTROLS
    auto& speech_codec = static_cast<StubuddyAudioCodec&>(codec);
    struct SpeechScope {
        StubuddyAudioCodec& codec;
        ~SpeechScope() { codec.SelectSpeechOutput(false); }
    } speech_scope{speech_codec};
    speech_codec.SelectSpeechOutput(true);
#endif
    status_led.SetState(stubuddy::LedState::ModelWaiting);
    ESP_RETURN_ON_ERROR(opus_loopback.StartStreamDecode(), kTag, "Stream decoder reset failed");
    stubuddy::PlaybackInterrupt interrupt(button.pressed());
    std::array<uint8_t, stubuddy::kAiReplyPacketBytes> packet{};
    struct DrainContext {
        stubuddy::PttButton& button;
        stubuddy::PlaybackInterrupt& interrupt;
        bool cancelled = false;
#ifdef CONFIG_STUBUDDY_COUNTDOWN
        bool reminder = false;
#endif
    } drain_context{button, interrupt};
    const auto keep_playing = [](void* raw) {
        auto& context = *static_cast<DrainContext*>(raw);
        context.button.Update(RawButtonPressed(), Milliseconds());
#ifdef CONFIG_STUBUDDY_COUNTDOWN
        context.reminder = CountdownDue();
        if (context.reminder)
            return false;
#endif
        context.cancelled = context.interrupt.Update(context.button.pressed());
        return !context.cancelled;
    };
    const auto drain_output = [&]() {
        static_cast<StubuddyAudioCodec&>(codec).FinishOutput(keep_playing, &drain_context);
    };
    size_t cursor = 0, index = 0, decoded = 0, played = 0;
    stubuddy::echo::Metadata verified;
    const unsigned started = Milliseconds();
    bool playing = false, ever_played = false;
    bool finished = false;
    stubuddy::RecordingLevels levels;
    while (true) {
        button.Update(RawButtonPressed(), Milliseconds());
#ifdef CONFIG_STUBUDDY_COUNTDOWN
        if (CountdownDue()) {
            codec.AbortOutput();
            ws_echo.Cancel();
            status_led.SetState(stubuddy::LedState::Idle);
            outcome = ReplayOutcome::Reminder;
            ESP_LOGI("STUBUDDY_TIMER", "PREEMPT: waiting/AI reply discarded; no resume queue");
            return ESP_OK;
        }
#endif
        if (interrupt.Update(button.pressed())) {
            codec.AbortOutput();
            ESP_RETURN_ON_FALSE(!codec.HasOutputError(), ESP_FAIL, kTag, "Stream abort failed");
            ESP_LOGI(kTag, "AI STREAM INTERRUPTED: old audio/DMA discarded");
            return InterruptWaitingEcho(codec, outcome);
        }
        // Preserve accepted disconnect behavior: a fully verified cached
        // response can finish offline. Incomplete streams fail immediately.
        if (ws_echo.stream_failed())
            break;
        if (ws_echo.round_empty()) {
            status_led.SetState(stubuddy::LedState::Idle);
            return ESP_OK;
        }
#ifdef CONFIG_STUBUDDY_AI_CONTROLS
        stubuddy::VolumeRequest request;
        if (ws_echo.TakeVolumeRequest(request)) {
#ifdef CONFIG_STUBUDDY_COUNTDOWN
            if (request.action != stubuddy::TimerAction::None) {
                const bool start = request.action == stubuddy::TimerAction::Start ||
                                   request.action == stubuddy::TimerAction::ScheduledStart;
                const auto result = countdown.Apply(
                    request, !start || ws_echo.ReminderReady(request.cache_id.data()));
#ifdef CONFIG_STUBUDDY_SCHEDULED
                ws_echo.KeepReminders(countdown.state().entries);
#endif
                ws_echo.CompleteVolumeRequest(result);
                continue;
            }
#endif
            const auto result = request.restore ? speech_codec.RestoreSpeechVolume()
                                : request.set   ? speech_codec.SetSpeechVolume(request.volume)
                                                : ESP_OK;
            ws_echo.CompleteVolumeRequest({request, result == ESP_OK, speech_codec.SpeechVolume()});
            ESP_LOGI(kTag, "VOLUME TOOL: turn=%u operation=%s ok=%s actual=%d", request.turn,
                     request.restore ? "restore"
                     : request.set   ? "set"
                                     : "get",
                     result == ESP_OK ? "yes" : "no", speech_codec.SpeechVolume());
        }
#endif
        const unsigned progress_age = ws_echo.ProgressAgeMs();
        const unsigned elapsed = stubuddy::ElapsedMs(Milliseconds(), started);
        const bool progress_expired = !ws_echo.done() && progress_age >= 45000;
        const bool first_audio_expired = !ever_played && elapsed >= 95000;
        if (progress_expired || first_audio_expired || elapsed >= 155000) {
            ws_echo.Timeout(progress_expired      ? "ai_progress"
                            : first_audio_expired ? "ai_first_audio"
                                                  : "ai_round",
                            progress_expired ? progress_age : elapsed,
                            progress_expired      ? 45000
                            : first_audio_expired ? 95000
                                                  : 155000);
            break;
        }
        // Decode just ahead of the speaker. Incoming Opus is bounded in PSRAM;
        // its receive task never waits for the speaker or does codec work.
        while (decoded - played < stubuddy::kAiPrebufferSamples) {
            size_t size = 0;
            if (!ws_echo.ReadStreamPacket(cursor, index, packet.data(), size, verified, finished))
                break;
            size_t copied = 0;
            const auto result = opus_loopback.DecodeStreamPacket(
                {packet.data(), size}, index, verified.delay, verified.samples, pcm, copied);
            if (result != ESP_OK) {
                codec.AbortOutput();
                ESP_LOGW(kTag, "AI DECODE FAILED: index=%u packet_bytes=%u error=%s",
                         unsigned(index), unsigned(size), esp_err_to_name(result));
                ws_echo.RejectReply("stream_decode_failed");
                status_led.SetState(stubuddy::LedState::Idle);
                return ESP_OK;  // Recoverable round failure, no permanent PTT stop.
            }
            ++index;
            decoded += copied;
            dirty_samples = std::max(dirty_samples, decoded);
        }
        if (ws_echo.stream_failed())
            break;
        if (finished && index == verified.frames && decoded == played) {
            if (decoded != verified.samples) {
                ESP_LOGW(kTag, "AI SAMPLE MISMATCH: decoded=%u expected=%u frames=%u",
                         unsigned(decoded), unsigned(verified.samples), unsigned(index));
                ws_echo.RejectReply("stream_sample_mismatch");
                break;
            }
            drain_output();  // Poll the key through final tail/DMA drain too.
#ifdef CONFIG_STUBUDDY_COUNTDOWN
            if (drain_context.reminder) {
                outcome = ReplayOutcome::Reminder;
                return ESP_OK;
            }
#endif
            status_led.SetPhase(stubuddy::PttLedPhase::ReleaseGate);
            ESP_RETURN_ON_FALSE(!codec.HasOutputError(), ESP_FAIL, kTag, "Stream shutdown failed");
            if (drain_context.cancelled)
                return InterruptWaitingEcho(codec, outcome);
            ws_echo.PlaybackDone();
            // Keep a released key armed across cleanup/RX reopening. A still-held
            // 60-second key must retain the original release gate.
            if (interrupt.released())
                outcome = ReplayOutcome::FreshPressReady;
            ESP_LOGI(kTag, "AI STREAM DONE: samples=%u frames=%u; amplifier OFF; LED OFF; errors=0",
                     unsigned(played), unsigned(index));
            return ESP_OK;
        }
        const size_t available = decoded - played;
        if (!playing && (available >= 4000 || (finished && available))) {
            if (!ever_played) {
                levels = stubuddy::AiStreamLevels(pcm, decoded);
            }
            codec.EnableOutput(true);
            ESP_RETURN_ON_FALSE(codec.output_enabled() && !codec.HasOutputError(), ESP_FAIL, kTag,
                                "Cannot start streaming output");
#ifdef CONFIG_STUBUDDY_AI_CONTROLS
            status_led.SetState(stubuddy::LedState::SpeechPlayback);
#else
            status_led.SetPhase(stubuddy::PttLedPhase::Playback);
#endif
            ESP_LOGI(kTag, "AI STREAM %s: first_voice_ms=%u buffered=%u; LED ON",
                     ever_played ? "RESUME" : "START", Milliseconds() - started,
                     unsigned(available));
            playing = ever_played = true;
        }
        if (playing && available) {
            const size_t count = std::min(available, stubuddy::kFramesPerBlock);
            block.resize(count);
            for (size_t i = 0; i < count; ++i)
                block[i] = stubuddy::ReplaySample(pcm[played + i], played + i,
                                                  finished ? verified.samples : kMaxSamples, levels,
                                                  stubuddy::kSampleRate / 100);
            codec.OutputData(block);
            ESP_RETURN_ON_FALSE(!codec.HasOutputError(), ESP_FAIL, kTag, "Streaming output failed");
            played += count;
        } else if (playing) {
            drain_output();
#ifdef CONFIG_STUBUDDY_COUNTDOWN
            if (drain_context.reminder) {
                outcome = ReplayOutcome::Reminder;
                return ESP_OK;
            }
#endif
            ESP_RETURN_ON_FALSE(!codec.HasOutputError(), ESP_FAIL, kTag, "Stream pause failed");
            if (drain_context.cancelled)
                return InterruptWaitingEcho(codec, outcome);
            status_led.SetState(stubuddy::LedState::ModelWaiting);
            playing = false;
            ESP_LOGW(kTag, "AI STREAM BUFFERING: waiting for verified audio; LED BREATH");
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    codec.AbortOutput();
    status_led.SetPhase(stubuddy::PttLedPhase::ReleaseGate);
    ESP_LOGW(kTag,
             "AI STREAM FAILED: partial audio discarded; server retains generated-answer history");
    return ESP_OK;
}
#endif

#ifdef CONFIG_STUBUDDY_COUNTDOWN
esp_err_t PlayReminder(AudioCodec& codec, int16_t* pcm, stubuddy::PttButton& button,
                       std::vector<int16_t>& block, ReplayOutcome& outcome) {
    std::array<stubuddy::CountdownState, 4> clips{};
    unsigned clip_count = 1;
#ifdef CONFIG_STUBUDDY_SCHEDULED
    std::array<stubuddy::ReminderEntry, 4> group;
    const auto consumed = countdown.Claim(group, clip_count);
    for (unsigned i = 0; i < clip_count; ++i) {
        std::strcpy(clips[i].id, group[i].id);
        clips[i].alarm_volume = countdown.state().alarm_volume;
    }
#else
    clips[0] = countdown.state();
    // Claim once before physical output. A power cut during the announcement
    // must not resurrect the reminder on the next boot.
    const auto consumed = countdown.Consume();
#endif
    const auto& saved = clips[0];
    codec.EnableInput(false);
    codec.AbortOutput();
    ws_echo.Cancel();
    opus_loopback.Discard();
    status_led.SetState(stubuddy::LedState::Idle);
    if (consumed != ESP_OK) {
        ESP_LOGE("STUBUDDY_TIMER", "DELETE FAILED: no playback/retry; inspect NVS");
        return ESP_OK;
    }
    auto& output = static_cast<StubuddyAudioCodec&>(codec);
    struct OutputScope {
        StubuddyAudioCodec& codec;
        ~OutputScope() {
            codec.AbortOutput();
            codec.ClearReminderOutput();
        }
    } scope{output};
    stubuddy::PlaybackInterrupt interrupt(button.pressed());
    struct Context {
        stubuddy::PttButton& button;
        stubuddy::PlaybackInterrupt& interrupt;
        bool stopped = false;
        bool limit_active = false, expired = false, superseded = false;
        stubuddy::ReminderCycle cycle{0};
    } context{button, interrupt};
    const auto proceed = [](void* raw) {
        auto& c = *static_cast<Context*>(raw);
        c.button.Update(RawButtonPressed(), Milliseconds());
        const bool playing =
            c.cycle.Poll(Milliseconds(), c.interrupt.Update(c.button.pressed()), c.limit_active);
        c.stopped = c.cycle.stopped();
        c.expired = c.limit_active && c.cycle.Expired(Milliseconds());
#ifdef CONFIG_STUBUDDY_SCHEDULED
        c.superseded = c.superseded || (c.limit_active && CountdownDue());
#endif
        return playing && !c.superseded;
    };
    ESP_LOGI("STUBUDDY_TIMER", "DUE: reminder wins; old recording/reply discarded; alarm_volume=%d",
             int(saved.alarm_volume));
    size_t decoded = 0;
    std::array<size_t, 4> offsets{}, lengths{};
    std::array<uint8_t, 227> packet{};
    for (unsigned clip = 0; clip < clip_count && proceed(&context); ++clip) {
        ESP_RETURN_ON_ERROR(opus_loopback.StartStreamDecode(), kTag,
                            "Reminder decoder reset failed");
        size_t cursor = 0, index = 0;
        offsets[clip] = decoded;
        stubuddy::echo::Metadata metadata{};
        while (proceed(&context)) {
            size_t size = 0, copied = 0;
            if (!ws_echo.ReadReminder(clips[clip].id, cursor, packet.data(), size, metadata))
                break;
            ESP_RETURN_ON_ERROR(
                opus_loopback.DecodeStreamPacket({packet.data(), size}, index++, metadata.delay,
                                                 metadata.samples, pcm + offsets[clip], copied),
                kTag, "Reminder decode failed");
            decoded += copied;
            lengths[clip] += copied;
        }
        if (!context.stopped &&
            (!lengths[clip] || lengths[clip] != metadata.samples || index != metadata.frames)) {
            ESP_LOGW("STUBUDDY_TIMER", "PLAY FAILED: incomplete cached voice; no retry");
            std::memset(pcm, 0, decoded * sizeof(int16_t));
            return ESP_OK;
        }
    }
    unsigned cycles = 0;
    context.cycle.Begin(Milliseconds());
    context.limit_active = true;
    while (proceed(&context)) {
        output.SelectReminderOutput(saved.alarm_volume, false);
        codec.EnableOutput(true);
        if (codec.HasOutputError() || !proceed(&context))
            break;
        status_led.SetState(stubuddy::LedState::Playback);
        ESP_LOGI("STUBUDDY_TIMER", "REMINDER CYCLE: index=%u items=%u limit_ms=180000 gap_ms=3000",
                 ++cycles, clip_count);
        // Each cycle reuses the prepared PCM: no extra TTS calls or allocation.
        for (unsigned offset = 0; offset < 2400 && proceed(&context);) {
            const unsigned size = std::min<unsigned>(160, 2400 - offset);
            block.resize(size);
            for (unsigned i = 0; i < size; ++i) {
                const unsigned n = offset + i;
                const float fade = std::min(1.0f, float(std::min(n, 2399 - n)) / 160);
                block[i] = int16_t(stubuddy::kOutputPeakPcm16 * fade *
                                   std::sin(2 * 3.14159265358979323846 * 880 * n / 16000));
            }
            codec.OutputData(block);
            offset += size;
        }
        if (codec.HasOutputError() || !proceed(&context))
            break;
        output.SelectReminderOutput(saved.alarm_volume, true);
        status_led.SetState(stubuddy::LedState::SpeechPlayback);
        for (unsigned clip = 0; clip < clip_count && proceed(&context); ++clip) {
            const auto* samples = pcm + offsets[clip];
            const auto length = lengths[clip];
            const auto levels = stubuddy::AnalyzeRecording(samples, length);
            for (size_t offset = 0; offset < length && proceed(&context);) {
                block.resize(std::min<size_t>(stubuddy::kFramesPerBlock, length - offset));
                for (size_t i = 0; i < block.size(); ++i)
                    block[i] = stubuddy::ReplaySample(samples[offset + i], offset + i, length,
                                                      levels, 160);
                codec.OutputData(block);
                if (codec.HasOutputError())
                    break;
                offset += block.size();
            }
            if (codec.HasOutputError())
                break;
        }
        if (!context.stopped && !codec.HasOutputError())
            output.FinishOutput(proceed, &context);
        status_led.SetState(stubuddy::LedState::Idle);
        if (codec.HasOutputError() || !proceed(&context))
            break;
        const unsigned finished = Milliseconds();
        ESP_LOGI("STUBUDDY_TIMER", "REMINDER GAP: LED OFF; fresh press stops/deletes");
        while (proceed(&context) && !context.cycle.NextReady(Milliseconds(), finished))
            vTaskDelay(pdMS_TO_TICKS(10));
    }
    codec.AbortOutput();
    status_led.SetState(stubuddy::LedState::Idle);
    std::memset(pcm, 0, decoded * sizeof(int16_t));
    outcome = context.stopped        ? ReplayOutcome::Interrupted
              : interrupt.released() ? ReplayOutcome::FreshPressReady
                                     : ReplayOutcome::Finished;
    ESP_LOGI("STUBUDDY_TIMER", "REMINDER END: reason=%s; deleted; LED OFF; no replay queue",
             context.stopped          ? "button"
             : codec.HasOutputError() ? "audio_failure"
             : context.superseded     ? "new_reminder"
             : context.expired        ? "timeout"
                                      : "complete");
    return ESP_OK;
}
#endif

[[maybe_unused]] esp_err_t Replay(AudioCodec& codec, const int16_t* recording, size_t count,
                                  std::vector<int16_t>& block, unsigned round,
                                  stubuddy::PttButton& button, ReplayOutcome& outcome) {
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
#ifdef CONFIG_STUBUDDY_WS_ECHO
            ws_echo.Cancel();
#endif
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
            const bool online = wifi_setup.online()
#ifdef CONFIG_STUBUDDY_WS_ECHO
                                && ws_echo.ready()
#endif
                ;
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
#ifdef CONFIG_STUBUDDY_AI_CONTROLS
    static_cast<StubuddyAudioCodec&>(codec).SetSpeechMeter(
        [](void* context, unsigned power) {
            static_cast<stubuddy::StatusLed*>(context)->PublishPlaybackPower(power);
        },
        &status_led);
#endif
    ESP_RETURN_ON_FALSE(codec.input_sample_rate() == 16000 && codec.input_channels() == 1,
                        ESP_ERR_INVALID_STATE, kTag, "Unexpected capture format");
    ESP_RETURN_ON_ERROR(InitializeButton(), kTag, "GPIO15 input setup failed");
#ifdef CONFIG_STUBUDDY_OPUS_LOOPBACK
    ESP_RETURN_ON_ERROR(opus_loopback.Initialize(), kTag, "Opus loopback initialization failed");
#endif
#ifdef CONFIG_STUBUDDY_COUNTDOWN
    ESP_RETURN_ON_ERROR(countdown.Load(), kTag, "Countdown NVS load failed");
    countdown_boot_pending = true;
#endif
#ifdef CONFIG_STUBUDDY_WS_ECHO
    ESP_RETURN_ON_ERROR(
        ws_echo.Start(wifi_setup, opus_loopback.echo_store(), opus_loopback.packet_limit()), kTag,
        "WS initialization failed");
    opus_loopback.SetPacketSink(
        [](void*, const uint8_t* data, size_t size) { ws_echo.PushAudio(data, size); }, nullptr);
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
    bool preserve_idle_key = false;
    unsigned preserved_epoch = 0;
    size_t dirty_samples = 0;
#if defined(CONFIG_STUBUDDY_COUNTDOWN) && !defined(CONFIG_STUBUDDY_SCHEDULED)
    unsigned countdown_sync_epoch = 0;
#endif
    while (true) {
#ifdef CONFIG_STUBUDDY_COUNTDOWN
        if (CountdownDue()) {
            ReplayOutcome reminder_outcome = ReplayOutcome::Finished;
            ESP_RETURN_ON_ERROR(
                PlayReminder(codec, recording.get(), button, block, reminder_outcome), kTag,
                "Reminder failed");
            preserve_idle_key = reminder_outcome == ReplayOutcome::FreshPressReady;
            preserved_epoch = NetworkEpoch();
            continue_capture = false;
            if (reminder_outcome == ReplayOutcome::Interrupted &&
                IdleSessionCurrent(NetworkEpoch())) {
                codec.EnableInput(true);
                continue_capture = true;
            }
        }
#ifdef CONFIG_STUBUDDY_SCHEDULED
        SyncReminders();
#else
        if (ws_echo.ready() && countdown.state().deadline &&
            !ws_echo.ReminderReady(countdown.state().id) &&
            countdown_sync_epoch != ws_echo.epoch()) {
            ws_echo.SyncCountdown(countdown.state());
            countdown_sync_epoch = ws_echo.epoch();
        }
#endif
#endif
        if (!continue_capture) {
            // Keep release gates dark. Reading the raw held key here must never
            // relight the LED or arm another recording after playback completion.
            status_led.SetPhase(stubuddy::PttLedPhase::ReleaseGate);
#ifdef CONFIG_STUBUDDY_SOFTAP_SETUP
            ESP_RETURN_ON_ERROR(AwaitNetworkReady(codec), kTag, "Network wait failed");
#endif
#ifdef CONFIG_STUBUDDY_COUNTDOWN
            if (CountdownDue())
                continue;
#ifdef CONFIG_STUBUDDY_SCHEDULED
            SyncReminders();
#else
            if (countdown.state().deadline && !ws_echo.ReminderReady(countdown.state().id) &&
                countdown_sync_epoch != ws_echo.epoch()) {
                ws_echo.SyncCountdown(countdown.state());
                countdown_sync_epoch = ws_echo.epoch();
            }
#endif
#endif
            const unsigned epoch = NetworkEpoch();
            const bool resume_idle =
                preserve_idle_key && epoch == preserved_epoch && IdleSessionCurrent(epoch);
            preserve_idle_key = false;
            block.resize(kPollSamples);
            codec.EnableInput(true);
            ESP_RETURN_ON_FALSE(codec.input_enabled() && !codec.HasInputError(), ESP_FAIL, kTag,
                                "Cannot start PDM input");
            // Warm up before arming the key, avoiding loss of the child's first word.
            // Idle audio is drained and discarded, never stored in the PSRAM recording.
            if (!resume_idle) {
                for (int i = 0; i < 20 && IdleSessionCurrent(epoch); ++i)
                    ESP_RETURN_ON_FALSE(codec.InputData(block), ESP_FAIL, kTag,
                                        "PDM warmup failed");
                ESP_LOGI(kTag, "WAIT RELEASE: release GPIO15 before the next recording; LED OFF");
                button.RequireRelease(RawButtonPressed(), Milliseconds());
                while (button.pressed() && IdleSessionCurrent(epoch)
#ifdef CONFIG_STUBUDDY_COUNTDOWN
                       && !CountdownDue()
#endif
                ) {
                    ESP_RETURN_ON_FALSE(codec.InputData(block), ESP_FAIL, kTag,
                                        "Release gate read failed");
                    button.Update(RawButtonPressed(), Milliseconds());
                }
            } else {
                button.Update(RawButtonPressed(), Milliseconds());
                ESP_LOGI(kTag,
                         "PTT REARM: released key preserved through playback end; no extra "
                         "release/warmup");
            }
            if (IdleSessionCurrent(epoch)) {
                status_led.SetPhase(stubuddy::PttLedPhase::ArmedIdle);
                ESP_LOGI(
                    kTag,
                    "IDLE: hold GPIO15 to record; release to replay (0.3-60 seconds); LED OFF");
            }
            while (!button.pressed() && IdleSessionCurrent(epoch)
#ifdef CONFIG_STUBUDDY_COUNTDOWN
                   && !CountdownDue()
#endif
            ) {
                ESP_RETURN_ON_FALSE(codec.InputData(block), ESP_FAIL, kTag, "Idle read failed");
                button.Update(RawButtonPressed(), Milliseconds());
                if (IdleSessionCurrent(epoch))
                    status_led.SetPhase(stubuddy::PttLedPhase::ArmedIdle, button.pressed());
            }

#ifdef CONFIG_STUBUDDY_COUNTDOWN
            if (CountdownDue()) {
                codec.EnableInput(false);
                continue;
            }
#endif

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

        const unsigned round_epoch = NetworkEpoch();
        size_t count = 0;
#ifdef CONFIG_STUBUDDY_OPUS_LOOPBACK
        ESP_RETURN_ON_ERROR(opus_loopback.Begin(round + 1), kTag, "Opus round reset failed");
#endif
#ifdef CONFIG_STUBUDDY_WS_ECHO
        if (!ws_echo.Begin(round + 1))
            ESP_LOGW(kTag, "WS ROUND FAILED AT START: finish capture, then wait for service");
#endif
        // Admission is complete. From here through replay, link changes never
        // cut the current round short; only the next idle session is gated.
        status_led.SetPhase(stubuddy::PttLedPhase::Recording);
        ESP_LOGI(kTag, "REC START: round=%u; amplifier OFF; LED=%s", ++round, kLedMode);
        while (count < kMaxSamples && button.pressed()
#ifdef CONFIG_STUBUDDY_COUNTDOWN
               && !CountdownDue()
#endif
        ) {
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
#ifdef CONFIG_STUBUDDY_COUNTDOWN
        if (CountdownDue()) {
            ws_echo.Cancel();
            outcome = ReplayOutcome::Reminder;
            ESP_LOGI("STUBUDDY_TIMER", "PREEMPT: recording discarded; child must record again");
        } else
#endif
            if (count < kMinSamples) {
            ESP_LOGI(kTag, "DROP: recording shorter than 300 ms; no replay");
#ifdef CONFIG_STUBUDDY_WS_ECHO
            ws_echo.Cancel();
#endif
        } else {
#ifdef CONFIG_STUBUDDY_WS_ECHO
            ESP_RETURN_ON_ERROR(opus_loopback.FinishCapture(count), kTag,
                                "Remote encode finish failed");
#ifdef CONFIG_STUBUDDY_AI_PTT
            ESP_RETURN_ON_ERROR(
                StreamReply(codec, recording.get(), count, button, block, outcome, dirty_samples),
                kTag, "AI stream failed");
#else
            bool decoded = false;
            size_t playback_samples = count;
            ESP_RETURN_ON_ERROR(ReceiveEcho(codec, recording.get(), count, opus_loopback.frames(),
                                            button, outcome, decoded, playback_samples),
                                kTag, "Echo receive failed");
            dirty_samples = std::max(dirty_samples, playback_samples);
            if (decoded) {
                ESP_RETURN_ON_ERROR(
                    Replay(codec, recording.get(), playback_samples, block, round, button, outcome),
                    kTag, "Remote replay failed");
#ifdef CONFIG_STUBUDDY_AI_PTT
                if (outcome == ReplayOutcome::Finished)
                    ws_echo.PlaybackDone();
#endif
            } else if (outcome == ReplayOutcome::Finished) {
#ifdef CONFIG_STUBUDDY_AI_PTT
                if (ws_echo.round_empty())
                    ESP_LOGI(kTag, "AI ROUND EMPTY: %u; LED OFF", round);
                else
#endif
                    ESP_LOGW(kTag,
                             "ROUND FAILED: %u; no server echo played; reconnect before next round",
                             round);
                status_led.SetPhase(stubuddy::PttLedPhase::ReleaseGate);
            }
#endif
#elif defined(CONFIG_STUBUDDY_OPUS_LOOPBACK)
            ESP_RETURN_ON_ERROR(opus_loopback.FinishAndDecode(recording.get(), count), kTag,
                                "Opus decode failed; raw PCM is never replayed as a fallback");
#endif
#ifndef CONFIG_STUBUDDY_WS_ECHO
            ESP_RETURN_ON_ERROR(
                Replay(codec, recording.get(), count, block, round, button, outcome), kTag,
                "Replay failed");
#endif
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
        preserve_idle_key = outcome == ReplayOutcome::FreshPressReady;
        preserved_epoch = round_epoch;
        dirty_samples = 0;
#ifdef CONFIG_STUBUDDY_WS_ECHO
        ws_echo.Cancel();
#endif
#ifdef CONFIG_STUBUDDY_OPUS_LOOPBACK
        opus_loopback.Erase();
#endif
        std::fill(block.begin(), block.end(), 0);
        ESP_LOGI(kTag, "ROUND DONE: %u; internal_free=%u, PSRAM_free=%u; recording erased", round,
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        // AI completion preserves an already-released key; held timeout keys
        // and sessions crossing a network recovery still require release.
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
#ifdef CONFIG_STUBUDDY_WS_ECHO
        ws_echo.Cancel();
#endif
#ifdef CONFIG_STUBUDDY_OPUS_LOOPBACK
        // RunPtt has stopped both I2S directions. Clear any partial encoded
        // recording on the failure path as well as on normal round completion.
        opus_loopback.Erase();
#endif
        status_led.SetState(stubuddy::LedState::Error);
        ESP_LOGE(kTag, "FAILED: %s; PTT stopped, reset required", esp_err_to_name(result));
    }
}
