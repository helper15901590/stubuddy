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
#ifdef CONFIG_STUBUDDY_COUNTDOWN
#include <mbedtls/base64.h>
#endif

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
#ifdef CONFIG_STUBUDDY_AI_CONTROLS
constexpr unsigned kSendTimeoutMs = UplinkBudget::kSendTimeoutMs;
#else
constexpr unsigned kSendTimeoutMs = 500;
#endif
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
#ifdef CONFIG_STUBUDDY_COUNTDOWN
#ifdef CONFIG_STUBUDDY_SCHEDULED
    using PreparedCache = ReminderBank;
#else
    using PreparedCache = ReminderCache;
#endif
    auto* reminder_storage = static_cast<uint8_t*>(
        heap_caps_malloc(PreparedCache::kCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(reminder_storage, ESP_ERR_NO_MEM, kTag, "Cannot allocate reminder cache");
    reminder_ = std::make_unique<PreparedCache>(reminder_storage);
#endif
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
           wifi_epoch_.load() == wifi_->link_epoch() && SnapshotAgeMs(last_rx_, Now) < 20000;
}

void WsEcho::Fault(const char* reason) {
    ready_.store(false);
    if (!round_failed_.exchange(true))
        ESP_LOGW(kTag, "RX/TRANSPORT REJECT: turn=%u reason=%s", active_turn_.load(), reason);
    close_requested_.store(true);
    if (wifi_ && wifi_->online() && !fault_latched_.exchange(true))
        fault_notice_.store(true);
}

unsigned WsEcho::ProgressAgeMs() const { return SnapshotAgeMs(progress_at_, Now); }

void WsEcho::Timeout(const char* reason, unsigned elapsed_ms, unsigned limit_ms) {
    ESP_LOGW(kTag, "ROUND TIMEOUT: turn=%u reason=%s elapsed_ms=%u limit_ms=%u",
             active_turn_.load(), reason, elapsed_ms, limit_ms);
    Fault(reason);
}

bool WsEcho::Enqueue(Command& command) {
    const bool audio = command.kind == Kind::Audio;
    const bool admitted = !audio || uplink_budget_.AcquireAudio();
    command.queued_at = Now();
    if (admitted && xQueueSend(outgoing_, &command, 0) == pdTRUE) {
        const unsigned depth = uxQueueMessagesWaiting(outgoing_);
        unsigned high = queue_high_water_.load();
        while (depth > high && !queue_high_water_.compare_exchange_weak(high, depth)) {
        }
        if (command.kind == Kind::Stop)
            ESP_LOGI(kTag, "UPLINK QUEUED: turn=%u frames=%u pending=%u high_water=%u capacity=%u",
                     command.turn, command.metadata.frames, depth, queue_high_water_.load(),
                     kQueueDepth);
        return true;
    }
    if (audio && admitted)
        uplink_budget_.ReleaseAudio();
    ESP_LOGW(kTag,
             "SEND QUEUE FULL: kind=%u turn=%u seq=%u pending=%u high_water=%u capacity=%u; no "
             "silent frame drop",
             unsigned(command.kind), command.turn, command.metadata.frames,
             unsigned(uxQueueMessagesWaiting(outgoing_)), queue_high_water_.load(), kQueueDepth);
    Fault("uplink_queue_exhausted");
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
#ifdef CONFIG_STUBUDDY_AI_CONTROLS
    awaiting_tools_ = true;
#endif
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
#ifdef CONFIG_STUBUDDY_AI_CONTROLS
    tools_.Reset();
    awaiting_tools_ = false;
#endif
    accepting_ = false;
    done_.store(false);
    received_->Reset();
    xSemaphoreGive(rx_mutex_);
    if (old && ready()) {
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

#ifdef CONFIG_STUBUDDY_AI_CONTROLS
bool WsEcho::TakeVolumeRequest(VolumeRequest& request) {
    xSemaphoreTake(rx_mutex_, portMAX_DELAY);
    const bool take = awaiting_tools_ && ready() && !round_failed() && tools_.Take(request);
    xSemaphoreGive(rx_mutex_);
    return take;
}

void WsEcho::QueueVolumeResult(const VolumeResult& result) {
    auto& command = tool_reply_;
    command.kind = Kind::ToolResult;
    command.turn = result.request.turn;
    command.epoch = epoch();
    command.tool = result;
    std::memcpy(command.session.data(), session_id_.data(), 16);
    command.session[16] = 0;  // Hello validates the exact 16-byte session identifier.
    Enqueue(command);
}

void WsEcho::CompleteVolumeRequest(const VolumeResult& result) {
    xSemaphoreTake(rx_mutex_, portMAX_DELAY);
    if (result.request.turn == active_turn_.load() && ready() && tools_.Complete(result))
        QueueVolumeResult(result);
    xSemaphoreGive(rx_mutex_);
}
#endif

#ifdef CONFIG_STUBUDDY_COUNTDOWN
#ifdef CONFIG_STUBUDDY_SCHEDULED
void WsEcho::KeepReminders(const std::array<ReminderEntry, 4>& entries) {
    xSemaphoreTake(rx_mutex_, portMAX_DELAY);
    reminder_->Keep(entries);
    xSemaphoreGive(rx_mutex_);
}
#endif
bool WsEcho::ReminderReady(const char* id) {
    xSemaphoreTake(rx_mutex_, portMAX_DELAY);
    const bool ready = reminder_->Ready(id);
    xSemaphoreGive(rx_mutex_);
    return ready;
}
bool WsEcho::ReadReminder(const char* id, size_t& cursor, uint8_t* data, size_t& size,
                          echo::Metadata& metadata) {
    xSemaphoreTake(rx_mutex_, portMAX_DELAY);
    const bool ok = reminder_->Read(id, cursor, data, size, metadata);
    xSemaphoreGive(rx_mutex_);
    return ok;
}
void WsEcho::SyncCountdown(const CountdownState& state) {
    Command command;
    command.kind = Kind::TimerSync;
    command.epoch = epoch();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "countdown_sync");
    cJSON_AddStringToObject(root, "id", state.id);
    cJSON_AddStringToObject(root, "text", state.text);
    char* text = cJSON_PrintUnformatted(root);
    if (text && std::strlen(text) < sizeof(command.payload)) {
        command.size = std::strlen(text);
        std::memcpy(command.payload, text, command.size + 1);
        Enqueue(command);
    }
    cJSON_free(text);
    cJSON_Delete(root);
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
#ifdef CONFIG_STUBUDDY_AI_CONTROLS
            uint32_t controls = 0;
            if (!UInt(root, "device_controls", controls) || controls != 1) {
                cJSON_Delete(root);
                Fault("server_missing_device_controls_update_server_first");
                return;
            }
            uint32_t restore = 0;
            if (!UInt(root, "volume_restore", restore) || restore != 1) {
                cJSON_Delete(root);
                Fault("server_missing_volume_restore_update_server_first");
                return;
            }
            ESP_LOGI(kTag, "DEVICE CONTROLS READY: volume_restore=1; restricted tools v1");
#ifdef CONFIG_STUBUDDY_COUNTDOWN
            uint32_t countdown = 0;
            if (!UInt(root, "countdown", countdown) || countdown != 1) {
                cJSON_Delete(root);
                Fault("server_missing_countdown_update_server_first");
                return;
            }
#ifdef CONFIG_STUBUDDY_SCHEDULED
            uint32_t scheduled = 0;
            if (!UInt(root, "scheduled", scheduled) || scheduled != 1) {
                cJSON_Delete(root);
                Fault("server_missing_scheduled_update_server_first");
                return;
            }
#endif
#endif
#endif
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
#ifdef CONFIG_STUBUDDY_COUNTDOWN
    } else if (Text(root, "type", "countdown_cache") || Text(root, "type", "countdown_packet")) {
        const auto* id = cJSON_GetObjectItemCaseSensitive(root, "id");
        bool ok = ready() && Text(root, "session_id", session_id_.data()) && cJSON_IsString(id) &&
                  std::strlen(id->valuestring) == 16 &&
                  std::strspn(id->valuestring, "0123456789abcdef") == 16;
        if (ok) {
            if (Text(root, "type", "countdown_packet")) {
                uint32_t index = 0;
                const auto* encoded = cJSON_GetObjectItemCaseSensitive(root, "data");
                std::array<uint8_t, 227> packet{};
                size_t length = 0;
                ok = UInt(root, "sequence", index) && cJSON_IsString(encoded) &&
                     std::strlen(encoded->valuestring) <= 304 &&
                     mbedtls_base64_decode(
                         packet.data(), packet.size(), &length,
                         reinterpret_cast<const unsigned char*>(encoded->valuestring),
                         std::strlen(encoded->valuestring)) == 0 &&
                     reminder_->Push(id->valuestring, index, packet.data(), length);
            } else if (Text(root, "state", "start")) {
                ok = reminder_->Begin(id->valuestring);
            } else {
                uint32_t samples = 0, delay = 0, frames = 0, crc = 0;
                ok = Text(root, "state", "end") && UInt(root, "samples", samples) &&
                     UInt(root, "delay", delay) && UInt(root, "frames", frames) &&
                     UInt(root, "crc32", crc) &&
                     reminder_->Finish(id->valuestring, samples, delay, frames, crc);
                if (ok)
                    ESP_LOGI(kTag, "COUNTDOWN CACHE READY: samples=%u frames=%u", samples, frames);
            }
        }
        if (!ok)
            Fault("invalid_countdown_cache");
        last_rx_.store(Now());
        progress_at_.store(Now());
#endif
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
#ifdef CONFIG_STUBUDDY_AI_CONTROLS
        if (Text(root, "type", "device_tool")) {
            const auto* id = cJSON_GetObjectItemCaseSensitive(root, "call_id");
            const bool set = Text(root, "name", "self.audio_speaker.set_volume");
            const bool restore = Text(root, "name", "self.audio_speaker.restore_volume");
            bool timer = false;
#ifdef CONFIG_STUBUDDY_COUNTDOWN
            TimerAction action = TimerAction::None;
            if (Text(root, "name", "self.countdown.get"))
                action = TimerAction::Get;
            if (Text(root, "name", "self.countdown.start"))
                action = TimerAction::Start;
            if (Text(root, "name", "self.countdown.cancel"))
                action = TimerAction::Cancel;
            if (Text(root, "name", "self.countdown.volume"))
                action = TimerAction::Volume;
#ifdef CONFIG_STUBUDDY_SCHEDULED
            if (Text(root, "name", "self.reminders.get"))
                action = TimerAction::Get;
            if (Text(root, "name", "self.reminders.countdown"))
                action = TimerAction::Start;
            if (Text(root, "name", "self.reminders.start"))
                action = TimerAction::ScheduledStart;
            if (Text(root, "name", "self.reminders.cancel"))
                action = TimerAction::Cancel;
            if (Text(root, "name", "self.reminders.volume"))
                action = TimerAction::Volume;
#endif
            timer = action != TimerAction::None;
#endif
            uint32_t volume = 0;
            if (!awaiting_tools_ || !cJSON_IsString(id) || std::strlen(id->valuestring) != 16 ||
                std::strspn(id->valuestring, "0123456789abcdef") != 16 ||
                (!timer && !set && !restore && !Text(root, "name", "self.get_device_status")) ||
                (set && (!UInt(root, "volume", volume) || volume > 100))) {
                cJSON_Delete(root);
                Fault("invalid_device_tool");
                return;
            }
            VolumeRequest request;
            request.turn = turn;
            std::snprintf(request.id.data(), request.id.size(), "%s", id->valuestring);
            request.set = set;
            request.restore = restore;
            request.volume = set ? int(volume) : -1;
#ifdef CONFIG_STUBUDDY_COUNTDOWN
            request.action = action;
            if (action == TimerAction::Volume) {
                if (!UInt(root, "volume", volume) || volume > 100) {
                    cJSON_Delete(root);
                    Fault("invalid_alarm_volume");
                    return;
                }
                request.volume = int(volume);
            }
            if (action == TimerAction::Start) {
                const auto* id = cJSON_GetObjectItemCaseSensitive(root, "cache_id");
                const auto* text = cJSON_GetObjectItemCaseSensitive(root, "text");
                if (!UInt(root, "seconds", request.seconds) || request.seconds < 60 ||
                    request.seconds > 86400 || !cJSON_IsString(id) ||
                    std::strlen(id->valuestring) != 16 ||
                    std::strspn(id->valuestring, "0123456789abcdef") != 16 ||
                    !cJSON_IsString(text) || !text->valuestring[0] ||
                    std::strlen(text->valuestring) > 120) {
                    cJSON_Delete(root);
                    Fault("invalid_countdown_start");
                    return;
                }
                std::strcpy(request.cache_id.data(), id->valuestring);
                std::strcpy(request.text.data(), text->valuestring);
            }
#ifdef CONFIG_STUBUDDY_SCHEDULED
            if (action == TimerAction::Start || action == TimerAction::ScheduledStart ||
                action == TimerAction::Cancel) {
                const auto* identity = cJSON_GetObjectItemCaseSensitive(root, "cache_id");
                bool valid = cJSON_IsString(identity) && std::strlen(identity->valuestring) == 16 &&
                             std::strspn(identity->valuestring, "0123456789abcdef") == 16;
                if (valid)
                    std::strcpy(request.cache_id.data(), identity->valuestring);
                if (action != TimerAction::Cancel) {
                    const auto* item = cJSON_GetObjectItemCaseSensitive(root, "item");
                    const auto* speech = cJSON_GetObjectItemCaseSensitive(root, "text");
                    valid = valid && cJSON_IsString(item) && item->valuestring[0] &&
                            std::strlen(item->valuestring) <= 60 && cJSON_IsString(speech) &&
                            speech->valuestring[0] && std::strlen(speech->valuestring) <= 120;
                    if (valid) {
                        std::strcpy(request.item.data(), item->valuestring);
                        std::strcpy(request.text.data(), speech->valuestring);
                    }
                }
                if (action == TimerAction::ScheduledStart) {
                    const auto* deadline = cJSON_GetObjectItemCaseSensitive(root, "deadline");
                    valid = valid && UInt(root, "minute", request.minute) &&
                            request.minute < 1440 && UInt(root, "weekdays", request.weekdays) &&
                            request.weekdays <= 127 && cJSON_IsNumber(deadline) &&
                            std::isfinite(deadline->valuedouble) &&
                            deadline->valuedouble >= 1767225600 &&
                            deadline->valuedouble < 4102444800LL &&
                            std::floor(deadline->valuedouble) == deadline->valuedouble;
                    if (valid)
                        request.deadline = int64_t(deadline->valuedouble);
                }
                if (!valid) {
                    cJSON_Delete(root);
                    Fault("invalid_reminder_arguments");
                    return;
                }
            }
#endif
#endif
            VolumeResult cached;
            const auto arrival = tools_.Receive(request, cached);
            if (arrival == VolumeMailbox::Arrival::Invalid ||
                (accepting_ && arrival == VolumeMailbox::Arrival::New))
                Fault("device_tool_limit_or_id_reuse");
            else if (arrival == VolumeMailbox::Arrival::Cached)
                QueueVolumeResult(cached);
            progress_at_.store(Now());
            last_rx_.store(Now());
            cJSON_Delete(root);
            return;
        }
#endif
        if (Text(root, "type", "stage")) {
            if (!Text(root, "state", "asr") && !Text(root, "state", "llm") &&
                !Text(root, "state", "tts")
#ifdef CONFIG_STUBUDDY_AI_CONTROLS
                && !Text(root, "state", "tools")
#endif
            ) {
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
    const unsigned started = Now();
    const int sent =
        esp_websocket_client_send_text(client_, text, size, pdMS_TO_TICKS(kSendTimeoutMs));
    const unsigned elapsed = Now() - started;
    if (elapsed >= 100)
        ESP_LOGW(kTag, "WS SEND SLOW: text turn=%u elapsed_ms=%u pending=%u high_water=%u",
                 active_turn_.load(), elapsed, unsigned(uxQueueMessagesWaiting(outgoing_)),
                 queue_high_water_.load());
    if (sent == int(size))
        return true;
    ESP_LOGW(kTag,
             "WS SEND FAILED: text turn=%u requested=%u sent=%d timeout_ms=%u; no frame retry",
             active_turn_.load(), unsigned(size), sent, kSendTimeoutMs);
    Fault("text_socket_write_failed");
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
#ifdef CONFIG_STUBUDDY_AI_CONTROLS
#ifdef CONFIG_STUBUDDY_COUNTDOWN
#ifdef CONFIG_STUBUDDY_SCHEDULED
            SendText(
                R"({"type":"hello","version":1,"transport":"websocket","ai_protocol":1,"reply_stream":1,"max_reply_packet_bytes":1275,"device_controls":1,"volume_restore":1,"countdown":1,"scheduled":1,"audio_params":{"format":"opus","sample_rate":16000,"channels":1,"frame_duration":60}})");
#else
            SendText(
                R"({"type":"hello","version":1,"transport":"websocket","ai_protocol":1,"reply_stream":1,"max_reply_packet_bytes":1275,"device_controls":1,"volume_restore":1,"countdown":1,"audio_params":{"format":"opus","sample_rate":16000,"channels":1,"frame_duration":60}})");
#endif
#else
            SendText(
                R"({"type":"hello","version":1,"transport":"websocket","ai_protocol":1,"reply_stream":1,"max_reply_packet_bytes":1275,"device_controls":1,"volume_restore":1,"audio_params":{"format":"opus","sample_rate":16000,"channels":1,"frame_duration":60}})");
#endif
#else
            SendText(
                R"({"type":"hello","version":1,"transport":"websocket","ai_protocol":1,"reply_stream":1,"max_reply_packet_bytes":1275,"audio_params":{"format":"opus","sample_rate":16000,"channels":1,"frame_duration":60}})");
#endif
#else
            SendText(
                R"({"type":"hello","version":1,"transport":"websocket","echo_protocol":1,"audio_params":{"format":"opus","sample_rate":16000,"channels":1,"frame_duration":60}})");
#endif
        }
        if (connected_.load()) {
            if (ready_.load() && !ready())
                Fault("link_or_rx_liveness_expired");
            else if (!ready_.load()) {
                const unsigned age = SnapshotAgeMs(last_rx_, Now);
                if (age > 10000)
                    Timeout("hello_wait", age, 10000);
            }
        }
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
        if (sending_.kind == Kind::Audio)
            uplink_budget_.ReleaseAudio();
        if (!ready() || sending_.epoch != epoch() ||
            (sending_.kind != Kind::Abort && sending_.kind != Kind::Played &&
             sending_.kind != Kind::TimerSync && sending_.turn != active_turn_.load()))
            continue;
        const unsigned waited = Now() - sending_.queued_at;
        if (waited >= 500)
            ESP_LOGW(kTag, "UPLINK BACKLOG: kind=%u turn=%u seq=%u wait_ms=%u pending=%u",
                     unsigned(sending_.kind), sending_.turn, sending_.metadata.frames, waited,
                     unsigned(uxQueueMessagesWaiting(outgoing_)));
        char text[384];
        if (sending_.kind == Kind::TimerSync) {
            SendText(reinterpret_cast<const char*>(sending_.payload));
        } else if (sending_.kind == Kind::Audio) {
            const size_t size =
                echo::PackAudio(wire_.data(), wire_.size(), sending_.turn, sending_.metadata.frames,
                                sending_.payload, sending_.size);
            const unsigned started = Now();
            const int sent = size ? esp_websocket_client_send_bin(
                                        client_, reinterpret_cast<const char*>(wire_.data()), size,
                                        pdMS_TO_TICKS(kSendTimeoutMs))
                                  : -1;
            const unsigned elapsed = Now() - started;
            if (elapsed >= 100)
                ESP_LOGW(
                    kTag,
                    "WS SEND SLOW: audio turn=%u seq=%u elapsed_ms=%u pending=%u high_water=%u",
                    sending_.turn, sending_.metadata.frames, elapsed,
                    unsigned(uxQueueMessagesWaiting(outgoing_)), queue_high_water_.load());
            if (!size || sent != int(size)) {
                ESP_LOGW(kTag,
                         "WS SEND FAILED: audio turn=%u seq=%u requested=%u sent=%d timeout_ms=%u; "
                         "no frame retry",
                         sending_.turn, sending_.metadata.frames, unsigned(size), sent,
                         kSendTimeoutMs);
                Fault("audio_socket_write_failed");
            }
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
        }
#ifdef CONFIG_STUBUDDY_AI_CONTROLS
        else if (sending_.kind == Kind::ToolResult) {
#ifdef CONFIG_STUBUDDY_COUNTDOWN
            if (sending_.tool.request.action != TimerAction::None) {
#ifdef CONFIG_STUBUDDY_SCHEDULED
                const auto& result = sending_.tool;
                cJSON* root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "type", "tool_result");
                cJSON_AddNumberToObject(root, "turn", sending_.turn);
                cJSON_AddStringToObject(root, "call_id", result.request.id.data());
                cJSON_AddStringToObject(root, "session_id", sending_.session.data());
                cJSON_AddBoolToObject(root, "ok", result.ok);
                cJSON_AddNumberToObject(root, "error", result.error);
                cJSON_AddNumberToObject(root, "now", double(result.now));
                cJSON_AddNumberToObject(root, "alarm_volume", result.alarm_volume);
                auto* records = cJSON_AddArrayToObject(root, "records");
                for (unsigned i = 0; i < result.records.size(); ++i) {
                    const auto& entry = result.records[i];
                    if (!entry.deadline)
                        continue;
                    auto* row = cJSON_CreateObject();
                    cJSON_AddStringToObject(row, "id", entry.id);
                    cJSON_AddStringToObject(row, "item", entry.item);
                    cJSON_AddStringToObject(row, "text", entry.text);
                    cJSON_AddStringToObject(row, "kind", i == 0 ? "countdown" : "scheduled");
                    cJSON_AddNumberToObject(row, "deadline", double(entry.deadline));
                    cJSON_AddNumberToObject(row, "minute", entry.minute);
                    cJSON_AddNumberToObject(row, "weekdays", entry.weekdays);
                    cJSON_AddItemToArray(records, row);
                }
                char* message = cJSON_PrintUnformatted(root);
                if (message && std::strlen(message) <= 4000)
                    SendText(message);
                else
                    Fault("reminder_status_size");
                cJSON_free(message);
                cJSON_Delete(root);
#else
                std::snprintf(
                    text, sizeof(text),
                    "{\"type\":\"tool_result\",\"turn\":%u,\"call_id\":\"%s\",\"session_id\":\"%"
                    "s\",\"ok\":%s,\"active\":%s,\"remaining\":%u,\"alarm_volume\":%d}",
                    sending_.turn, sending_.tool.request.id.data(), sending_.session.data(),
                    sending_.tool.ok ? "true" : "false", sending_.tool.active ? "true" : "false",
                    sending_.tool.remaining, sending_.tool.alarm_volume);
                SendText(text);
#endif
                continue;
            }
#endif
            std::snprintf(text, sizeof(text),
                          "{\"type\":\"tool_result\",\"turn\":%u,\"call_id\":\"%s\",\"session_id\":"
                          "\"%s\",\"ok\":%s,\"volume\":%d}",
                          sending_.turn, sending_.tool.request.id.data(), sending_.session.data(),
                          sending_.tool.ok ? "true" : "false", sending_.tool.volume);
            SendText(text);
        }
#endif
        else if (sending_.kind == Kind::Played) {
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
