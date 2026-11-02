#pragma once

#include "captive_dns.h"
#include "status_led.h"
#include "wifi_cue.h"
#include "wifi_history.h"
#include "wifi_rules.h"

#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <lwip/sockets.h>
#include <atomic>

namespace stubuddy {

// Static lifetime. Only TaskMain mutates the connection state. SDK callbacks
// signal bits; HTTP callbacks validate/copy bounded commands into a queue.
class WifiSetup {
public:
    esp_err_t Start(StatusLed& led, bool force_setup);
    bool online() const {
        return online_.load() && connected_epoch_.load() == link_epoch_.load() && !fatal_.load();
    }
    bool failed() const { return fatal_.load(); }
    WifiCue TakeCue() { return cues_.Take(); }
    bool has_cue() const { return cues_.pending(); }
    unsigned link_epoch() const { return link_epoch_.load(); }

private:
    static constexpr unsigned kMaxNetworks = 20;
    enum class Phase { Setup, Connecting, Connected, Fault };
    using Credentials = WifiCredentials;
    struct Network {
        char ssid[33] = {};
        int rssi = 0;
        bool secured = false;
    };
    struct Snapshot {
        Phase phase = Phase::Setup;
        bool scanning = false;
        bool busy = false;
        bool portal_open = true;
        bool failed = false;
        bool manual_setup = true;
        unsigned count = 0;
        unsigned attempt = 0;
        char ap_ssid[33] = {};
        char target[33] = {};
        char ip[16] = {};
        char message[160] = {};
        Network networks[kMaxNetworks] = {};
        unsigned saved_count = 0;
        char saved_networks[WifiHistory::kCapacity][33] = {};
    };
    enum class CommandType { Connect, ConnectSaved, Scan };
    struct Command {
        CommandType type;
        Credentials credentials;
    };

    static void TaskEntry(void* context);
    static void OnEvent(void* context, esp_event_base_t base, int32_t id, void* data);
    static esp_err_t Page(httpd_req_t* request);
    static esp_err_t Status(httpd_req_t* request);
    static esp_err_t Scan(httpd_req_t* request);
    static esp_err_t Connect(httpd_req_t* request);
    static esp_err_t Reconnect(httpd_req_t* request);
    static esp_err_t Submit(httpd_req_t* request, bool saved);
    static esp_err_t PortalProbe(httpd_req_t* request);
    static esp_err_t Redirect(httpd_req_t* request, httpd_err_code_t error);

    void TaskMain();
    esp_err_t OpenPortal();
    esp_err_t StartPortalHttp();
    esp_err_t ClosePortal();
    void BeginAttempt(const Credentials& credentials, bool user_supplied);
    void FailAttempt(const char* message, bool cancel_driver);
    void GotAddress();
    void FinishScan();
    void Fault(esp_err_t error, const char* where);
    void RetryError(const char* message);
    void Publish(const char* message);
    bool ReadSnapshot(Snapshot& snapshot);
    bool CheckToken(httpd_req_t* request);
    esp_err_t LoadStorage();
    esp_err_t SaveCredentials();

    StatusLed* led_ = nullptr;
    esp_netif_t* station_ = nullptr;
    esp_netif_t* access_point_ = nullptr;
    httpd_handle_t http_ = nullptr;
    struct ifreq ap_interface_ = {};
    EventGroupHandle_t events_ = nullptr;
    QueueHandle_t commands_ = nullptr;
    SemaphoreHandle_t snapshot_mutex_ = nullptr;
    esp_event_handler_instance_t wifi_handler_ = nullptr;
    esp_event_handler_instance_t ip_handler_ = nullptr;
    std::atomic<bool> online_{false};
    std::atomic<bool> fatal_{false};
    std::atomic<unsigned> link_epoch_{0};
    std::atomic<unsigned> connected_epoch_{0};
    std::atomic<bool> submit_pending_{false};
    std::atomic<bool> scan_pending_{false};
    std::atomic<unsigned> disconnect_reason_{0};
    WifiCueMailbox cues_;
    Snapshot status_;
    Snapshot published_;
    WifiHistory history_;
    WifiConnectionPolicy connection_policy_;
    CaptiveDns dns_;
    Credentials pending_;
    wifi_ap_record_t scan_records_[kMaxNetworks] = {};
    char csrf_[33] = {};
    bool history_persisted_ = false;
    bool setup_confirmed_ = false;
    bool force_setup_ = false;
    bool connecting_ = false;
    bool user_attempt_ = false;
    bool disconnecting_ = false;
    bool scanning_ = false;
    int64_t connection_deadline_ = 0;
    int64_t retry_at_ = 0;
    int64_t disconnect_deadline_ = 0;
    int64_t portal_close_at_ = 0;
    int64_t scan_deadline_ = 0;
    int64_t error_led_until_ = 0;
};

}  // namespace stubuddy
