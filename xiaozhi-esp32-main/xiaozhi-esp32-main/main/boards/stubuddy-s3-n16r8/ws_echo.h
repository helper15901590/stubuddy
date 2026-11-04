#pragma once

#include "echo_protocol.h"
#include "wifi_setup.h"

#include <esp_websocket_client.h>
#include <cJSON.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <atomic>

namespace stubuddy {
// WebSocket events only validate/copy bounded data. One worker owns all sends;
// the PTT task never waits on a socket. No model or generic Application needed.
class WsEcho {
public:
    esp_err_t Start(WifiSetup& wifi, OpusPacketStore& received, size_t packet_limit);
    bool ready() const;
    unsigned epoch() const { return epoch_.load(); }
    bool TakeFaultNotice() { return fault_notice_.exchange(false); }
    bool Begin(unsigned turn);
    void PushAudio(const uint8_t* payload, size_t size);
    bool Finish(size_t samples, size_t delay, size_t frames);
    void Cancel();
    bool round_failed() const { return round_failed_.load() || (!done_.load() && !ready()); }
    bool done() const { return done_.load(std::memory_order_acquire); }
#ifdef CONFIG_STUBUDDY_AI_PTT
    size_t reply_samples() const { return expected_.samples; }
    size_t reply_delay() const { return expected_.delay; }
    bool round_empty() const { return empty_.load(); }
    bool stream_failed() const { return round_failed() && !(done() && !round_empty()); }
    void PlaybackDone();
    bool ReadStreamPacket(size_t& cursor, size_t index, uint8_t* packet, size_t& size,
                          echo::Metadata& verified, bool& complete);
#endif
    unsigned progress_at() const { return progress_at_.load(); }
    void Timeout();

private:
    enum class Kind { Start, Audio, Stop, Abort, Played };
    struct Command {
        Kind kind = Kind::Start;
        unsigned turn = 0, epoch = 0;
        echo::Metadata metadata;
        size_t size = 0;
        uint8_t payload[kOpusMaxPacketBytes] = {};
    };
    static constexpr unsigned kQueueDepth = 8;
    static void WorkerEntry(void* context);
    static void Event(void* context, esp_event_base_t base, int32_t id, void* data);
    void Worker();
    void OnMessage(unsigned opcode, const uint8_t* data, size_t size);
    void Fault(const char* reason = "transport_or_protocol");
    bool Enqueue(const Command& command);
    bool SendText(const char* text);
    bool ReadMetadata(const cJSON* root, echo::Metadata& metadata);
    WifiSetup* wifi_ = nullptr;
    OpusPacketStore* received_ = nullptr;
    size_t packet_limit_ = 0;
    esp_websocket_client_handle_t client_ = nullptr;
    QueueHandle_t outgoing_ = nullptr;
    StaticQueue_t queue_state_{};
    uint8_t* queue_storage_ = nullptr;
    SemaphoreHandle_t rx_mutex_ = nullptr;
    echo::MessageBuffer assembler_;
    echo::Metadata expected_, uploaded_;
    unsigned received_frames_ = 0, received_bytes_ = 0, received_crc_ = 0;
    bool accepting_ = false;
    Command sending_;  // Avoid a 4KiB command on the worker stack.
    std::array<uint8_t, echo::kMaxMessage> wire_{};
    std::atomic<bool> connected_{false}, hello_needed_{false}, ready_{false}, round_failed_{false},
        done_{false}, close_requested_{false}, fault_notice_{false}, fault_latched_{false};
    std::atomic<unsigned> epoch_{0}, active_turn_{0}, last_rx_{0}, progress_at_{0}, wifi_epoch_{0},
        failures_{0};
#ifdef CONFIG_STUBUDDY_AI_PTT
    std::atomic<bool> empty_{false};
    std::array<char, 33> session_id_{};
#endif
};
}  // namespace stubuddy
