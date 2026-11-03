#pragma once

#include "opus_framing.h"

#include <esp_err.h>
#include <array>
#include <memory>

namespace stubuddy {
// Single audio-task owner. All storage is allocated once before arming PTT.
// Encoding is incremental; a bounded RAM packet store feeds the real decoder.
class OpusLoopback {
public:
    OpusLoopback() = default;
    ~OpusLoopback();
    OpusLoopback(const OpusLoopback&) = delete;
    OpusLoopback& operator=(const OpusLoopback&) = delete;
    esp_err_t Initialize();
    esp_err_t Begin(unsigned round);
    esp_err_t Append(const int16_t* pcm, size_t samples);
    esp_err_t FinishAndDecode(int16_t* pcm, size_t samples);
    void Discard();
    void Erase();
#ifdef CONFIG_STUBUDDY_WS_ECHO
    using PacketSink = void (*)(void*, const uint8_t*, size_t);
    void SetPacketSink(PacketSink sink, void* context) {
        sink_ = sink;
        sink_context_ = context;
    }
    esp_err_t FinishCapture(size_t samples);
    esp_err_t DecodeEcho(int16_t* pcm, size_t samples, bool (*proceed)(void*), void* context);
    OpusPacketStore& echo_store() { return *packets_; }
    size_t packet_limit() const { return packet_capacity_; }
    size_t delay() const { return delay_; }
    size_t frames() const { return encoded_frames_; }
#endif

private:
    esp_err_t Check(int result, const char* operation);
    esp_err_t EncodeFrame();
    esp_err_t DecodePacket(OpusPacketView packet);
    esp_err_t MeasureDelay();
    void* encoder_ = nullptr;
    void* decoder_ = nullptr;
    uint8_t* storage_ = nullptr;
    uint8_t* packet_ = nullptr;
    std::unique_ptr<OpusPacketStore> packets_;
    std::array<int16_t, kOpusFrameSamples> input_{};
    std::array<int16_t, kOpusFrameSamples> decoded_{};
    size_t packet_capacity_ = 0;
    size_t delay_ = 0;
    size_t pending_ = 0;
    size_t samples_ = 0;
    unsigned round_ = 0;
    unsigned errors_ = 0;
    uint64_t encode_us_ = 0;
    unsigned encode_max_us_ = 0;
    unsigned decode_max_us_ = 0;
    bool capturing_ = false;
#ifdef CONFIG_STUBUDDY_WS_ECHO
    PacketSink sink_ = nullptr;
    void* sink_context_ = nullptr;
    size_t encoded_frames_ = 0;
#endif
};
}  // namespace stubuddy
