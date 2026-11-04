#include "ws_echo.h"
#ifdef CONFIG_STUBUDDY_AI_PTT
#include "ai_stream.h"
#endif

#ifdef CONFIG_STUBUDDY_WSS_ECHO
#ifdef CONFIG_STUBUDDY_AI_PTT
#if __has_include("stubuddy_ai_private.h")
#include "stubuddy_ai_private.h"
#else
#error "Run stubuddy/server/voice-ai/configure.py before building ai-ptt"
#endif
#elif __has_include("stubuddy_echo_cloud.h")
#include "stubuddy_echo_cloud.h"
#else
#error "Run python stubuddy/server/ws-echo/configure_cloud.py --server-ip PUBLIC_IP first"
#endif
#include <esp_crt_bundle.h>
#include <esp_netif_sntp.h>
#include <ctime>
#if defined(CONFIG_ESP_TLS_INSECURE) || defined(CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY) || \
    !defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) || !defined(CONFIG_MBEDTLS_HAVE_TIME_DATE)
#error "WSS requires CA bundle, certificate validity and peer verification"
#endif
#elif __has_include("stubuddy_echo_local.h")
#include "stubuddy_echo_local.h"
#else
#error \
    "Run python stubuddy/server/ws-echo/configure.py from the workspace root before building ws-echo"
#endif

#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace stubuddy {
#ifdef CONFIG_STUBUDDY_AI_PTT
namespace echo_settings = ai_private;
#elif defined(CONFIG_STUBUDDY_WSS_ECHO)
namespace echo_settings = echo_cloud;
#else
namespace echo_settings = echo_local;
#endif
namespace {
constexpr char kTag[] = "STUBUDDY_WS";
unsigned Now() { return static_cast<unsigned>(esp_timer_get_time() / 1000); }
bool UInt(const cJSON* root, const char* name, uint32_t& value) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) || item->valuedouble < 0 ||
        item->valuedouble > UINT32_MAX || std::floor(item->valuedouble) != item->valuedouble)
        return false;
    value = static_cast<uint32_t>(item->valuedouble);
    return true;
}
bool Text(const cJSON* root, const char* name, const char* expected) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(item) && std::strcmp(item->valuestring, expected) == 0;
}
}  // namespace

esp_err_t WsEcho::Start(WifiSetup& wifi, OpusPacketStore& received, size_t packet_limit) {
#ifdef CONFIG_STUBUDDY_WSS_ECHO
    constexpr char kScheme[] = "wss://";
    constexpr char kTransport[] = "WSS";
#else
    constexpr char kScheme[] = "ws://";
    constexpr char kTransport[] = "LAN";
#endif
    ESP_RETURN_ON_FALSE(!client_ && packet_limit <= kOpusMaxPacketBytes &&
                            std::strlen(echo_settings::kToken) == 64 &&
                            std::strncmp(echo_settings::kUrl, kScheme, sizeof(kScheme) - 1) == 0,
                        ESP_ERR_INVALID_STATE, kTag, "Invalid echo transport configuration");
    wifi_ = &wifi;
    received_ = &received;
    packet_limit_ = packet_limit;
    rx_mutex_ = xSemaphoreCreateMutex();
    queue_storage_ = static_cast<uint8_t*>(
        heap_caps_malloc(kQueueDepth * sizeof(Command), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(rx_mutex_ && queue_storage_, ESP_ERR_NO_MEM, kTag,
                        "Cannot allocate bounded echo queue");
    outgoing_ = xQueueCreateStatic(kQueueDepth, sizeof(Command), queue_storage_, &queue_state_);
    ESP_RETURN_ON_FALSE(outgoing_, ESP_ERR_NO_MEM, kTag, "Cannot create echo queue");
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), kTag, "Cannot identify device");
    char device[18];
    std::snprintf(device, sizeof(device), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
                  mac[3], mac[4], mac[5]);
    const std::string headers = std::string("Authorization: Bearer ") + echo_settings::kToken +
                                "\r\nDevice-Id: " + device + "\r\nProtocol-Version: 1\r\n";
    esp_websocket_client_config_t config = {};
    config.uri = echo_settings::kUrl;
    config.headers = headers.c_str();
    config.disable_auto_reconnect = false;
    config.enable_close_reconnect = true;
    config.reconnect_timeout_ms = 1000;
    config.network_timeout_ms = 2000;
#ifdef CONFIG_STUBUDDY_WSS_ECHO
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.skip_cert_common_name_check = false;
    config.network_timeout_ms = 10000;  // Allow public-network TCP/TLS handshakes.
#endif
    config.buffer_size = 1024;
    config.task_stack = 8192;
    config.task_prio = 4;
    config.ping_interval_sec = 5;
    config.pingpong_timeout_sec = 15;
    client_ = esp_websocket_client_init(&config);
    ESP_RETURN_ON_FALSE(client_, ESP_ERR_NO_MEM, kTag, "Cannot initialize WebSocket");
    ESP_RETURN_ON_ERROR(esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, Event, this),
                        kTag, "Cannot register WS events");
    ESP_RETURN_ON_FALSE(xTaskCreate(WorkerEntry, "stubuddy_ws", 8192, this, 3, nullptr) == pdPASS,
                        ESP_ERR_NO_MEM, kTag, "Cannot create echo worker");
#ifdef CONFIG_STUBUDDY_AI_PTT
    ESP_LOGI(kTag, "%s AI: %s; token not logged; queue=%u frames/%u PSRAM bytes", kTransport,
             echo_settings::kUrl, kQueueDepth, unsigned(kQueueDepth * sizeof(Command)));
#else
    ESP_LOGI(kTag, "%s ECHO: %s; token not logged; queue=%u frames/%u PSRAM bytes", kTransport,
             echo_settings::kUrl, kQueueDepth, unsigned(kQueueDepth * sizeof(Command)));
#endif
    return ESP_OK;
}

bool WsEcho::ready() const {
    return ready_.load() && connected_.load() && wifi_ && wifi_->online() &&
           wifi_epoch_.load() == wifi_->link_epoch() && Now() - last_rx_.load() < 20000;
}

void WsEcho::Fault(const char* reason) {
    ready_.store(false);
    if (!round_failed_.exchange(true))
        ESP_LOGW(kTag, "RX/TRANSPORT REJECT: turn=%u reason=%s", active_turn_.load(), reason);
    close_requested_.store(true);
    if (wifi_ && wifi_->online() && !fault_latched_.exchange(true))
        fault_notice_.store(true);
}

void WsEcho::Timeout() {
    ESP_LOGW(kTag, "ECHO TIMEOUT: no progress; round cancelled");
    Fault();
}

bool WsEcho::Enqueue(const Command& command) {
    if (xQueueSend(outgoing_, &command, 0) == pdTRUE)
        return true;
    ESP_LOGW(kTag, "SEND QUEUE FULL: round failed; capture may finish; no silent frame drop");
    Fault();
    return false;
}

bool WsEcho::Begin(unsigned turn) {
    Cancel();
#ifdef CONFIG_STUBUDDY_AI_PTT
    empty_.store(false);
#endif
    uploaded_ = {};
    uploaded_.turn = turn;
    if (!ready() || !turn) {
        round_failed_.store(true);
        return false;
    }
    xSemaphoreTake(rx_mutex_, portMAX_DELAY);
    expected_ = {};
    received_->Reset();
    accepting_ = false;
    done_.store(false);
    round_failed_.store(false);
    active_turn_.store(turn);
    xSemaphoreGive(rx_mutex_);
    Command command;
    command.kind = Kind::Start;
    command.turn = turn;
    command.epoch = epoch();
    return Enqueue(command);
}

void WsEcho::PushAudio(const uint8_t* payload, size_t size) {
    if (!payload || !size || size > packet_limit_ || uploaded_.frames >= kOpusMaxPackets) {
        Fault();
        return;
    }
    const unsigned sequence = uploaded_.frames++;
    uploaded_.bytes += size;
    uploaded_.crc32 = echo::Crc32(uploaded_.crc32, payload, size);
    if (round_failed_.load() || !ready()) {
        round_failed_.store(true);
        return;
    }
    Command command;
    command.kind = Kind::Audio;
    command.turn = uploaded_.turn;
    command.epoch = epoch();
    command.metadata.frames = sequence;
    command.size = size;
    std::memcpy(command.payload, payload, size);
    Enqueue(command);
}

bool WsEcho::Finish(size_t samples, size_t delay, size_t frames) {
    if (round_failed_.load() || !ready() || frames != uploaded_.frames) {
        round_failed_.store(true);
        return false;
    }
    uploaded_.samples = samples;
    uploaded_.delay = delay;
    if (!uploaded_.valid()) {
        Fault();
        return false;
    }
    xSemaphoreTake(rx_mutex_, portMAX_DELAY);
    expected_ = uploaded_;
    received_->Reset();
    received_frames_ = received_bytes_ = received_crc_ = 0;
    accepting_ = false;
    progress_at_.store(Now());
    xSemaphoreGive(rx_mutex_);
    Command command;
    command.kind = Kind::Stop;
    command.turn = uploaded_.turn;
    command.epoch = epoch();
    command.metadata = uploaded_;
    return Enqueue(command);
}

void WsEcho::Cancel() {
    if (!rx_mutex_)
        return;
    xSemaphoreTake(rx_mutex_, portMAX_DELAY);
    const unsigned old = active_turn_.exchange(0);
    accepting_ = false;
    done_.store(false);
    received_->Reset();
    xSemaphoreGive(rx_mutex_);
    if (old && connected_.load()) {
        Command command;
        command.kind = Kind::Abort;
        command.turn = old;
        command.epoch = epoch();
        Enqueue(command);
    }
}

#ifdef CONFIG_STUBUDDY_AI_PTT
bool WsEcho::ReadStreamPacket(size_t& cursor, size_t index, uint8_t* packet, size_t& size,
                              echo::Metadata& verified, bool& complete) {
    xSemaphoreTake(rx_mutex_, portMAX_DELAY);
    verified = expected_;
    complete = done();  // End flag and final trim belong to the same snapshot.
    OpusPacketView view;
    const bool available =
        !stream_failed() && index < verified.frames && received_->Next(cursor, view);
    size = available ? view.size : 0;
    if (available)
        std::memcpy(packet, view.data, view.size);
    xSemaphoreGive(rx_mutex_);
    return available;
}

void WsEcho::PlaybackDone() {
    Command command;
    command.kind = Kind::Played;
    command.turn = active_turn_.load();
    command.epoch = epoch();
    if (command.turn)
        Enqueue(command);
}
#endif

bool WsEcho::ReadMetadata(const cJSON* root, echo::Metadata& m) {
    return UInt(root, "turn", m.turn) && UInt(root, "samples", m.samples) &&
           UInt(root, "delay", m.delay) && UInt(root, "frames", m.frames) &&
           UInt(root, "bytes", m.bytes) && UInt(root, "crc32", m.crc32) && m.valid();
}

void WsEcho::OnMessage(unsigned opcode, const uint8_t* data, size_t size) {
    if (opcode == 2) {
        echo::AudioView audio;
        if (!echo::ParseAudio(data, size, audio)) {
            Fault();
            return;
        }
        if (audio.turn != active_turn_.load() || done_.load() || round_failed_.load())
            return;
        const size_t receive_limit =
#ifdef CONFIG_STUBUDDY_AI_PTT
            kAiReplyPacketBytes;
#else
            packet_limit_;
#endif
        const unsigned frame_limit =
#ifdef CONFIG_STUBUDDY_AI_PTT
            kOpusMaxPackets;
#else
            expected_.frames;
#endif
        if (!accepting_ || audio.sequence != received_frames_ || received_frames_ >= frame_limit ||
            audio.size > receive_limit || !received_->Push(audio.data, audio.size)) {
            ESP_LOGW(kTag, "AUDIO REJECT: seq=%u expected=%u size=%u limit=%u", audio.sequence,
                     received_frames_, unsigned(audio.size), unsigned(receive_limit));
            Fault("audio_sequence_size_or_storage");
            return;
        }
        ++received_frames_;
        received_bytes_ += audio.size;
        received_crc_ = echo::Crc32(received_crc_, audio.data, audio.size);
        progress_at_.store(Now());
        last_rx_.store(Now());
        return;
    }
    if (size > 1024 || !echo::JsonDepthAllowed(data, size)) {
        Fault();
        return;
    }
    const char* end = nullptr;
    const char* limit = reinterpret_cast<const char*>(data) + size;
    cJSON* root = cJSON_ParseWithLengthOpts(reinterpret_cast<const char*>(data), size, &end, false);
    while (end && end < limit && (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t'))
        ++end;
    if (!cJSON_IsObject(root) || end != limit) {
        cJSON_Delete(root);
        Fault();
        return;
    }
    if (Text(root, "type", "hello")) {
        if (ready_.load()) {
            cJSON_Delete(root);
            Fault();
            return;
        }
        uint32_t protocol = 0, rate = 0, channels = 0, duration = 0;
        const cJSON* audio = cJSON_GetObjectItemCaseSensitive(root, "audio_params");
        if (Text(root, "transport", "websocket") &&
#ifdef CONFIG_STUBUDDY_AI_PTT
            UInt(root, "ai_protocol", protocol) &&
#else
            UInt(root, "echo_protocol", protocol) &&
#endif
            protocol == 1 && Text(audio, "format", "opus") && UInt(audio, "sample_rate", rate) &&
            rate == 16000 && UInt(audio, "channels", channels) && channels == 1 &&
            UInt(audio, "frame_duration", duration) && duration == 60 && wifi_->online()) {
#ifdef CONFIG_STUBUDDY_AI_PTT
            uint32_t stream = 0, packet_limit = 0;
            if (!UInt(root, "reply_stream", stream) || stream != 1 ||
                !UInt(root, "max_reply_packet_bytes", packet_limit) ||
                packet_limit != kAiReplyPacketBytes) {
                cJSON_Delete(root);
                Fault("server_missing_stream_support_update_server_first");
                return;
            }
            const cJSON* session = cJSON_GetObjectItemCaseSensitive(root, "session_id");
            if (!cJSON_IsString(session) || std::strlen(session->valuestring) != 16) {
                cJSON_Delete(root);
                Fault();
                return;
            }
            std::snprintf(session_id_.data(), session_id_.size(), "%s", session->valuestring);
#endif
            wifi_epoch_.store(wifi_->link_epoch());
            last_rx_.store(Now());
            ready_.store(true);
            failures_.store(0);
            fault_latched_.store(false);
#ifdef CONFIG_STUBUDDY_AI_PTT
            ESP_LOGI(kTag,
                     "AI READY: authenticated ai/1; reply_stream=1 packet_limit=1275; epoch=%u",
                     epoch());
#else
            ESP_LOGI(kTag, "WS READY: authenticated echo/1; 16000Hz mono/60ms; epoch=%u", epoch());
#endif
        } else
            Fault();
    } else if (Text(root, "type", "pong")) {
        last_rx_.store(Now());
    } else {
        uint32_t turn = 0;
        if (!UInt(root, "turn", turn)) {
            cJSON_Delete(root);
            Fault();
            return;
        }
        if (turn != active_turn_.load() || done_.load() || round_failed_.load()) {
            cJSON_Delete(root);
            return;
        }
#ifdef CONFIG_STUBUDDY_AI_PTT
        if (!Text(root, "session_id", session_id_.data())) {
            cJSON_Delete(root);
            Fault();
            return;
        }
        if (Text(root, "type", "stage")) {
            if (!Text(root, "state", "asr") && !Text(root, "state", "llm") &&
                !Text(root, "state", "tts")) {
                cJSON_Delete(root);
                Fault();
                return;
            }
            progress_at_.store(Now());
            last_rx_.store(Now());
            ESP_LOGI(kTag, "AI WAIT: turn=%u; stage=%s", turn,
                     cJSON_GetObjectItemCaseSensitive(root, "state")->valuestring);
            cJSON_Delete(root);
            return;
        }
        if (Text(root, "type", "stt") || Text(root, "type", "reply_text")) {
            const cJSON* text = cJSON_GetObjectItemCaseSensitive(root, "text");
            if (!cJSON_IsString(text) || std::strlen(text->valuestring) > 750) {
                cJSON_Delete(root);
                Fault();
                return;
            }
            ESP_LOGI(kTag, "AI %s: turn=%u; %s", Text(root, "type", "stt") ? "STT" : "TEXT", turn,
                     text->valuestring);
            progress_at_.store(Now());
            last_rx_.store(Now());
            cJSON_Delete(root);
            return;
        }
        if (Text(root, "type", "turn_empty")) {
            empty_.store(true);
            done_.store(true, std::memory_order_release);
            ESP_LOGI(kTag, "AI EMPTY: turn=%u; no speech recognized", turn);
            cJSON_Delete(root);
            return;
        }
        if (Text(root, "type", "turn_error")) {
            round_failed_.store(true);
            fault_notice_.store(true);
            ESP_LOGW(kTag, "AI TURN FAILED: turn=%u; retry with a fresh press", turn);
            cJSON_Delete(root);
            return;
        }
        if (Text(root, "type", "reply_start") && !accepting_) {
            uint32_t stream = 0, delay = 0, limit = 0;
            if (!UInt(root, "stream", stream) || stream != 1 || !UInt(root, "delay", delay) ||
                delay >= kOpusFrameSamples || !UInt(root, "max_packet_bytes", limit) ||
                limit != kAiReplyPacketBytes) {
                cJSON_Delete(root);
                Fault("invalid_stream_start");
                return;
            }
            expected_ = {};
            expected_.turn = turn;
            expected_.delay = delay;
            received_->Reset();
            received_frames_ = received_bytes_ = received_crc_ = 0;
            accepting_ = true;
            progress_at_.store(Now());
            last_rx_.store(Now());
            cJSON_Delete(root);
            return;
        }
        echo::Metadata metadata;
        const bool final = Text(root, "type", "reply_end");
        if ((!final && !Text(root, "type", "reply_chunk")) || !accepting_ ||
            !UInt(root, "turn", metadata.turn) || !UInt(root, "samples", metadata.samples) ||
            !UInt(root, "delay", metadata.delay) || !UInt(root, "frames", metadata.frames) ||
            !UInt(root, "bytes", metadata.bytes) || !UInt(root, "crc32", metadata.crc32) ||
            metadata.delay != expected_.delay || !ValidAiCheckpoint(metadata, expected_, final) ||
            metadata.frames != received_frames_ || metadata.bytes != received_bytes_ ||
            metadata.crc32 != received_crc_) {
            cJSON_Delete(root);
            Fault("stream_checkpoint_sequence_samples_or_crc");
            return;
        }
        expected_ = metadata;
        progress_at_.store(Now());
        if (final) {
            accepting_ = false;
            ESP_LOGI(kTag, "AI VERIFIED: turn=%u; frames=%u; samples=%u; crc32=%08x", turn,
                     received_frames_, expected_.samples, received_crc_);
            done_.store(true, std::memory_order_release);
        }
#else
        echo::Metadata metadata;
        if (!ReadMetadata(root, metadata) || !(metadata == expected_)) {
            cJSON_Delete(root);
            Fault();
            return;
        }
        if (Text(root, "type", "echo_start") && !accepting_) {
            accepting_ = true;
            progress_at_.store(Now());
        } else if (Text(root, "type", "echo_end") && accepting_ &&
                   received_frames_ == expected_.frames && received_bytes_ == expected_.bytes &&
                   received_crc_ == expected_.crc32) {
            accepting_ = false;
            ESP_LOGI(kTag,
                     "ECHO VERIFIED: turn=%u; frames=%u; bytes=%u; samples=%u; crc32=%08x; "
                     "source=SERVER",
                     unsigned(turn), received_frames_, received_bytes_, unsigned(expected_.samples),
                     received_crc_);
            done_.store(true, std::memory_order_release);
        } else
            Fault();
#endif
        last_rx_.store(Now());
    }
    cJSON_Delete(root);
}

void WsEcho::Event(void* context, esp_event_base_t, int32_t id, void* raw) {
    auto& self = *static_cast<WsEcho*>(context);
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        self.epoch_.fetch_add(1);
        self.connected_.store(true);
        self.ready_.store(false);
        self.last_rx_.store(Now());
        self.hello_needed_.store(true);
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_CLOSED ||
               id == WEBSOCKET_EVENT_ERROR) {
        self.connected_.store(false);
        self.epoch_.fetch_add(1);
        self.Fault();
        if (id == WEBSOCKET_EVENT_DISCONNECTED) {
            const unsigned failures = std::min(self.failures_.fetch_add(1), 5U);
            const unsigned delay =
                std::min(30000U, (1000U << failures) + unsigned(esp_random() % 250));
            esp_websocket_client_set_reconnect_timeout(self.client_, delay);
            ESP_LOGW(kTag, "WS DISCONNECTED: retry=%u ms; unfinished round invalid", delay);
        }
        xSemaphoreTake(self.rx_mutex_, portMAX_DELAY);
        self.accepting_ = false;
        self.assembler_.Reset();
        xSemaphoreGive(self.rx_mutex_);
    } else if (id == WEBSOCKET_EVENT_DATA) {
        const auto* event = static_cast<esp_websocket_event_data_t*>(raw);
        if (event->op_code != 1 && event->op_code != 2 && event->op_code != 0)
            return;  // The transport owns close/ping/pong control frames.
        xSemaphoreTake(self.rx_mutex_, portMAX_DELAY);
        const auto result = self.assembler_.Append(
            event->op_code, event->fin, event->payload_len, event->payload_offset,
            reinterpret_cast<const uint8_t*>(event->data_ptr), event->data_len);
        if (result == echo::MessageBuffer::Result::Complete) {
            self.OnMessage(self.assembler_.opcode(), self.assembler_.data(),
                           self.assembler_.size());
            self.assembler_.Reset();
        } else if (result == echo::MessageBuffer::Result::Invalid)
            self.Fault();
        xSemaphoreGive(self.rx_mutex_);
    }
}

bool WsEcho::SendText(const char* text) {
    const size_t size = std::strlen(text);
    if (esp_websocket_client_send_text(client_, text, size, pdMS_TO_TICKS(500)) == int(size))
        return true;
    Fault();
    return false;
}

void WsEcho::WorkerEntry(void* context) { static_cast<WsEcho*>(context)->Worker(); }

void WsEcho::Worker() {
    while (!wifi_->online())
        vTaskDelay(pdMS_TO_TICKS(100));
#ifdef CONFIG_STUBUDDY_WSS_ECHO
    // Only this worker waits for time. PTT/LED and Wi-Fi recovery remain live.
    esp_sntp_config_t clock = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "time.cloudflare.com"));
    if (esp_netif_sntp_init(&clock) != ESP_OK) {
        ESP_LOGE(kTag, "TIME INIT FAILED: WSS remains unavailable");
        Fault();
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(kTag, "TIME WAIT: synchronize before verified TLS; recording paused");
    unsigned last_notice = Now();
    bool synchronized = false;
    while (true) {
        if (!synchronized)
            synchronized = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(1000)) == ESP_OK &&
                           std::time(nullptr) >= 1735689600;
        if (synchronized && wifi_->online())
            break;
        if (Now() - last_notice >= 30000) {
            ESP_LOGW(kTag, "TIME WAIT: check Internet/DNS/NTP; TLS verification stays required");
            Fault();
            last_notice = Now();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(kTag, "TIME READY: epoch=%lld; CA/peer/validity checks enabled",
             static_cast<long long>(std::time(nullptr)));
#endif
    if (esp_websocket_client_start(client_) != ESP_OK) {
        Fault();
        vTaskDelete(nullptr);
        return;
    }
    unsigned last_ping = 0;
    while (true) {
        if (hello_needed_.exchange(false)) {
            close_requested_.store(false);
#ifdef CONFIG_STUBUDDY_AI_PTT
            SendText(
                R"({"type":"hello","version":1,"transport":"websocket","ai_protocol":1,"reply_stream":1,"max_reply_packet_bytes":1275,"audio_params":{"format":"opus","sample_rate":16000,"channels":1,"frame_duration":60}})");
#else
            SendText(
                R"({"type":"hello","version":1,"transport":"websocket","echo_protocol":1,"audio_params":{"format":"opus","sample_rate":16000,"channels":1,"frame_duration":60}})");
#endif
        }
        if (connected_.load() &&
            ((ready_.load() && !ready()) || (!ready_.load() && Now() - last_rx_.load() > 10000)))
            Fault();
        if (close_requested_.exchange(false) && connected_.load()) {
            ready_.store(false);
            esp_websocket_client_close_with_code(client_, 1002, nullptr, 0, pdMS_TO_TICKS(500));
        }
        if (ready() && Now() - last_ping >= 5000) {
            SendText(R"({"type":"ping"})");
            last_ping = Now();
        }
        if (xQueueReceive(outgoing_, &sending_, pdMS_TO_TICKS(20)) != pdTRUE)
            continue;
        if (!ready() || sending_.epoch != epoch() ||
            (sending_.kind != Kind::Abort && sending_.kind != Kind::Played &&
             sending_.turn != active_turn_.load()))
            continue;
        char text[384];
        if (sending_.kind == Kind::Audio) {
            const size_t size =
                echo::PackAudio(wire_.data(), wire_.size(), sending_.turn, sending_.metadata.frames,
                                sending_.payload, sending_.size);
            if (!size ||
                esp_websocket_client_send_bin(client_, reinterpret_cast<const char*>(wire_.data()),
                                              size, pdMS_TO_TICKS(500)) != int(size))
                Fault();
        } else if (sending_.kind == Kind::Start) {
            std::snprintf(
                text, sizeof(text),
                "{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\",\"turn\":%u}",
                sending_.turn);
            SendText(text);
        } else if (sending_.kind == Kind::Stop) {
            const auto& m = sending_.metadata;
            std::snprintf(text, sizeof(text),
                          "{\"type\":\"listen\",\"state\":\"stop\",\"turn\":%u,\"samples\":%u,"
                          "\"delay\":%u,\"frames\":%u,\"bytes\":%u,\"crc32\":%u}",
                          unsigned(m.turn), unsigned(m.samples), unsigned(m.delay),
                          unsigned(m.frames), unsigned(m.bytes), unsigned(m.crc32));
            SendText(text);
        } else if (sending_.kind == Kind::Played) {
            std::snprintf(text, sizeof(text), "{\"type\":\"playback_done\",\"turn\":%u}",
                          sending_.turn);
            SendText(text);
        } else {
            std::snprintf(text, sizeof(text), "{\"type\":\"abort\",\"turn\":%u}", sending_.turn);
            SendText(text);
        }
    }
}
}  // namespace stubuddy
