#pragma once

#include "audio_codec.h"

// Board-specific NS4168 output adapter. The existing mono AudioCodec contract
// feeds a physical stereo bus: left slot zero, right slot signed 32-bit PCM.
class StubuddyAudioCodec final : public AudioCodec {
public:
    StubuddyAudioCodec();
    ~StubuddyAudioCodec() override;
    void Start() override;
    void EnableOutput(bool enable) override;
    void EnableInput(bool enable) override;
    bool HasOutputError() const override { return failed_; }

protected:
    int Write(const int16_t* data, int samples) override;
    int Read(int16_t*, int) override { return 0; }

private:
    esp_err_t Initialize();
    esp_err_t WriteSilence(unsigned duration_ms);
    esp_err_t WriteSlots(const int32_t* data, unsigned frames);
    esp_err_t Shutdown();
    void Fail(esp_err_t error, const char* operation);
    bool running_ = false;
    bool failed_ = false;
};
