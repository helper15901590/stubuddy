#pragma once

#include "audio_codec.h"
#ifdef CONFIG_STUBUDDY_AI_CONTROLS
#include "speech_volume.h"
#endif
#ifdef CONFIG_STUBUDDY_OPUS_LOOPBACK
#include <atomic>
#endif

// Board-specific PDM input and NS4168 output adapter. The mono AudioCodec contract
// feeds a physical stereo bus: left slot zero, right slot signed 32-bit PCM.
class StubuddyAudioCodec final : public AudioCodec {
public:
    StubuddyAudioCodec();
    ~StubuddyAudioCodec() override;
    void Start() override;
    void EnableOutput(bool enable) override;
    // Optional audio-owner poll while draining the tail. False cancels DMA.
    bool FinishOutput(bool (*proceed)(void*) = nullptr, void* context = nullptr);
    void AbortOutput() override;
    void EnableInput(bool enable) override;
    bool HasOutputError() const override { return failed_; }
#ifdef CONFIG_STUBUDDY_AI_CONTROLS
    int SpeechVolume() const { return volume_state_.volume; }
    esp_err_t RestoreSpeechVolume() { return SetSpeechVolume(volume_state_.audible); }
    esp_err_t SetSpeechVolume(int volume);
    void SelectSpeechOutput(bool speech);
#ifdef CONFIG_STUBUDDY_COUNTDOWN
    void SelectReminderOutput(int volume, bool voice);
    void ClearReminderOutput();
#endif
    void SetSpeechMeter(void (*callback)(void*, unsigned), void* context) {
        meter_callback_ = callback;
        meter_context_ = context;
    }
#endif
    bool HasInputError() const override {
#ifdef CONFIG_STUBUDDY_OPUS_LOOPBACK
        return failed_ || rx_overflow_.load(std::memory_order_relaxed);
#else
        return failed_;
#endif
    }

protected:
    int Write(const int16_t* data, int samples) override;
    int Read(int16_t* data, int samples) override;

private:
    esp_err_t Initialize();
    esp_err_t WriteSilence(unsigned duration_ms);
    esp_err_t WriteSlots(const int32_t* data, unsigned frames);
    esp_err_t Shutdown();
    esp_err_t ShutdownInput();
    void Fail(esp_err_t error, const char* operation);
    bool running_ = false;
    bool input_running_ = false;
    bool failed_ = false;
#ifdef CONFIG_STUBUDDY_AI_CONTROLS
    esp_err_t LoadSpeechVolume();
    static bool IRAM_ATTR OnOutputSent(i2s_chan_handle_t, i2s_event_data_t*, void*);
    stubuddy::SpeechVolumeState volume_state_;
    bool volume_loaded_ = false;
#ifdef CONFIG_STUBUDDY_COUNTDOWN
    int reminder_volume_ = -1;
#endif
    std::atomic<bool> speech_output_{false};
    void (*meter_callback_)(void*, unsigned) = nullptr;
    void* meter_context_ = nullptr;
#endif
#ifdef CONFIG_STUBUDDY_OPUS_LOOPBACK
    // An encoder stall must never silently turn dropped DMA data into a pass.
    std::atomic<bool> rx_overflow_{false};
    static bool IRAM_ATTR OnReceiveOverflow(i2s_chan_handle_t channel, i2s_event_data_t* event,
                                            void* context);
#endif
};
