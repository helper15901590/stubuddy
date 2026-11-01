#include "wifi_setup.h"
#include "wifi_rules.h"

#include <esp_check.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

extern const unsigned char setup_page_start[] asm("_binary_setup_html_start");

namespace stubuddy {
namespace {
constexpr char kTag[] = "STUBUDDY_WIFI";
constexpr EventBits_t kStarted = BIT0;
constexpr EventBits_t kDisconnected = BIT1;
constexpr EventBits_t kGotIp = BIT2;
constexpr EventBits_t kScanDone = BIT3;
constexpr uint32_t kStorageVersion = 0x53544231;
char kPortalUrl[] = "http://192.168.4.1/";
constexpr int64_t kAttemptMs = 20000;
int64_t Now() { return esp_timer_get_time() / 1000; }

void RandomHex(char* destination, unsigned bytes) {
    uint8_t random[16];
    esp_fill_random(random, bytes);
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < bytes; ++i) {
        destination[2 * i] = digits[random[i] >> 4];
        destination[2 * i + 1] = digits[random[i] & 15];
    }
    destination[2 * bytes] = 0;
}

esp_err_t JsonResponse(httpd_req_t* request, cJSON* object) {
    if (object == nullptr)
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    char* text = cJSON_PrintUnformatted(object);
    cJSON_Delete(object);
    if (!text)
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    const esp_err_t result = httpd_resp_sendstr(request, text);
    cJSON_free(text);
    return result;
}

esp_err_t ApiError(httpd_req_t* request, const char* status, const char* message) {
    httpd_resp_set_status(request, status);
    cJSON* root = cJSON_CreateObject();
    if (root)
        cJSON_AddStringToObject(root, "error", message);
    // Reject and close even when a malformed request body was not consumed.
    JsonResponse(request, root);
    return ESP_FAIL;
}
}  // namespace

esp_err_t WifiSetup::LoadStorage() {
    // Deliberately do not erase all NVS if initialization fails: it may contain
    // other device identity/settings in later firmware.
    ESP_RETURN_ON_ERROR(nvs_flash_init(), kTag, "NVS init failed; existing data retained");
    nvs_handle_t storage;
    ESP_RETURN_ON_ERROR(nvs_open("stubuddy_wifi", NVS_READWRITE, &storage), kTag,
                        "NVS open failed");
    uint8_t confirmed = 0;
    const esp_err_t confirmation = nvs_get_u8(storage, "setup_done", &confirmed);
    setup_confirmed_ = confirmation == ESP_OK && confirmed == 1;
    if (!setup_confirmed_)
        ESP_LOGI(kTag,
                 "SETUP CONFIRMATION: missing; saved networks remain available for selection");
    WifiHistory history;
    size_t length = sizeof(history);
    const esp_err_t load_history = nvs_get_blob(storage, "history", &history, &length);
    if (load_history == ESP_OK && length == sizeof(history) && history.Valid()) {
        history_ = history;
        history_persisted_ = true;
        nvs_close(storage);
        ESP_LOGI(kTag, "SAVED NETWORKS: %u (passwords not logged)", history_.count);
        return ESP_OK;
    }
    if (load_history != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGW(kTag, "WiFi history unavailable/invalid; checking the legacy record");
    struct Stored {
        uint32_t version;
        Credentials credentials;
    } record = {};
    length = sizeof(record);
    const esp_err_t load = nvs_get_blob(storage, "station", &record, &length);
    if (load == ESP_OK && length == sizeof(record) && record.version == kStorageVersion) {
        const unsigned ssid_bytes =
            strnlen(record.credentials.ssid, sizeof(record.credentials.ssid));
        const unsigned password_bytes =
            strnlen(record.credentials.password, sizeof(record.credentials.password));
        if (ValidWifiCredentials(record.credentials.ssid, ssid_bytes, record.credentials.password,
                                 password_bytes)) {
            history_.Remember(record.credentials);
            ESP_LOGI(kTag, "LEGACY WIFI: loaded for history migration after successful connect");
        }
    } else if (load != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(kTag, "Saved WiFi record unavailable/invalid; entering setup without erasing NVS");
    }
    nvs_close(storage);
    return ESP_OK;
}

esp_err_t WifiSetup::SaveCredentials() {
    WifiHistory updated = history_;
    const bool changed = updated.Remember(pending_);
    if (!changed && history_persisted_ && setup_confirmed_)
        return ESP_OK;
    nvs_handle_t storage;
    ESP_RETURN_ON_ERROR(nvs_open("stubuddy_wifi", NVS_READWRITE, &storage), kTag,
                        "NVS open failed");
    esp_err_t result = ESP_OK;
    if (changed || !history_persisted_)
        result = nvs_set_blob(storage, "history", &updated, sizeof(updated));
    if (result == ESP_OK && (changed || !history_persisted_)) {
        // Remove obsolete single-network and AP-password keys only after the
        // replacement record has been written. Other NVS namespaces are untouched.
        const esp_err_t legacy = nvs_erase_key(storage, "station");
        const esp_err_t password = nvs_erase_key(storage, "ap_password");
        if (legacy != ESP_OK && legacy != ESP_ERR_NVS_NOT_FOUND)
            result = legacy;
        if (password != ESP_OK && password != ESP_ERR_NVS_NOT_FOUND)
            result = password;
    }
    if (result == ESP_OK && !setup_confirmed_) {
        // A legacy history alone is not completed onboarding. Mark completion
        // only after an explicit setup request actually acquired an IP.
        result = user_attempt_ ? nvs_set_u8(storage, "setup_done", 1) : ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK)
        result = nvs_commit(storage);
    nvs_close(storage);
    if (result == ESP_OK) {
        history_ = updated;
        history_persisted_ = true;
        setup_confirmed_ = true;
    }
    return result;
}

esp_err_t WifiSetup::Start(StatusLed& led, bool force_setup) {
    led_ = &led;
    force_setup_ = force_setup;
    ESP_RETURN_ON_ERROR(LoadStorage(), kTag, "Cannot load WiFi settings");
    connection_policy_.Start(history_.count != 0, force_setup_, setup_confirmed_);
    if (history_.count && !setup_confirmed_)
        ESP_LOGI(kTag, "BOOT MODE: first setup confirmation required; automatic reconnect paused");
    status_.portal_open = connection_policy_.manual_setup();
    events_ = xEventGroupCreate();
    commands_ = xQueueCreate(4, sizeof(Command));
    snapshot_mutex_ = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(events_ && commands_ && snapshot_mutex_, ESP_ERR_NO_MEM, kTag,
                        "WiFi control allocation failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), kTag, "Network interface init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), kTag, "Event loop init failed");
    station_ = esp_netif_create_default_wifi_sta();
    access_point_ = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(station_ && access_point_, ESP_ERR_NO_MEM, kTag,
                        "WiFi interfaces unavailable");
    const esp_err_t stop = esp_netif_dhcps_stop(access_point_);
    ESP_RETURN_ON_FALSE(stop == ESP_OK || stop == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED, stop,
                        kTag, "Cannot prepare AP address");
    esp_netif_ip_info_t address = {};
    IP4_ADDR(&address.ip, 192, 168, 4, 1);
    address.gw = address.ip;
    IP4_ADDR(&address.netmask, 255, 255, 255, 0);
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(access_point_, &address), kTag,
                        "Cannot set AP address");
    esp_netif_dns_info_t dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = address.ip.addr;
    ESP_RETURN_ON_ERROR(esp_netif_set_dns_info(access_point_, ESP_NETIF_DNS_MAIN, &dns), kTag,
                        "Cannot set AP DNS");
    uint8_t offer_dns = 1;
    ESP_RETURN_ON_ERROR(
        esp_netif_dhcps_option(access_point_, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                               &offer_dns, sizeof(offer_dns)),
        kTag, "Cannot offer AP DNS");
    ESP_RETURN_ON_ERROR(
        esp_netif_dhcps_option(access_point_, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                               kPortalUrl, strlen(kPortalUrl)),
        kTag, "Cannot set captive portal DHCP option");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(access_point_), kTag, "Cannot start AP DHCP");
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), kTag, "MAC read failed");
    snprintf(status_.ap_ssid, sizeof(status_.ap_ssid), "stubuddy-%02X%02X", mac[4], mac[5]);
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(station_, status_.ap_ssid), kTag,
                        "Hostname setup failed");
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), kTag, "WiFi driver init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, OnEvent,
                                                            this, &wifi_handler_),
                        kTag, "WiFi handler init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, OnEvent,
                                                            this, &ip_handler_),
                        kTag, "IP handler init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), kTag,
                        "WiFi RAM storage setup failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), kTag, "WiFi mode setup failed");
    wifi_config_t ap = {};
    memcpy(ap.ap.ssid, status_.ap_ssid, strlen(status_.ap_ssid));
    ap.ap.ssid_len = strlen(status_.ap_ssid);
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.pmf_cfg.required = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), kTag, "AP setup failed");
    if (!status_.portal_open) {
        // Configure AP credentials above while the radio is stopped, but never
        // advertise an onboarding hotspot during a normal saved-network boot.
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), kTag, "STA-only startup failed");
        ESP_LOGI(kTag, "BOOT MODE: saved-network connect; setup AP stays off");
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "WiFi startup failed");
    // Radio is running when generating the short-lived portal request token.
    RandomHex(csrf_, 16);
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), kTag, "WiFi power mode setup failed");
    Publish("正在启动配网服务");
    ESP_RETURN_ON_FALSE(xTaskCreate(TaskEntry, "stubuddy_wifi", 8192, this, 2, nullptr) == pdPASS,
                        ESP_ERR_NO_MEM, kTag, "WiFi task creation failed");
    return ESP_OK;
}

void WifiSetup::OnEvent(void* context, esp_event_base_t base, int32_t id, void* data) {
    auto& self = *static_cast<WifiSetup*>(context);
    EventBits_t bits = 0;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
        bits = kGotIp;
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START)
            bits = kStarted;
        if (id == WIFI_EVENT_SCAN_DONE)
            bits = kScanDone;
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            self.disconnect_reason_.store(
                static_cast<wifi_event_sta_disconnected_t*>(data)->reason);
            bits = kDisconnected;
        }
    }
    if (bits)
        xEventGroupSetBits(self.events_, bits);
}

void WifiSetup::Publish(const char* message) {
    if (message)
        snprintf(status_.message, sizeof(status_.message), "%s", message);
    status_.busy = connecting_ || disconnecting_;
    status_.scanning = scanning_;
    status_.manual_setup = connection_policy_.manual_setup();
    status_.saved_count = history_.count;
    for (unsigned i = 0; i < history_.count; ++i)
        snprintf(status_.saved_networks[i], sizeof(status_.saved_networks[i]), "%s",
                 history_.items[i].ssid);
    xSemaphoreTake(snapshot_mutex_, portMAX_DELAY);
    published_ = status_;
    xSemaphoreGive(snapshot_mutex_);
}

bool WifiSetup::ReadSnapshot(Snapshot& snapshot) {
    if (xSemaphoreTake(snapshot_mutex_, pdMS_TO_TICKS(200)) != pdTRUE)
        return false;
    snapshot = published_;
    xSemaphoreGive(snapshot_mutex_);
    return true;
}

esp_err_t WifiSetup::OpenPortal() {
    connection_policy_.EnterManualSetup();
    if (!status_.portal_open) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), kTag, "AP restart failed");
        status_.portal_open = true;
    }
    if (!http_) {
        ESP_RETURN_ON_ERROR(esp_netif_get_netif_impl_name(access_point_, ap_interface_.ifr_name),
                            kTag, "Cannot find AP interface");
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.stack_size = 8192;
        config.max_open_sockets = 4;
        config.max_uri_handlers = 7;
        config.uri_match_fn = httpd_uri_match_wildcard;
        config.max_resp_headers = 8;
        config.max_req_hdr_len = 2048;
        config.max_uri_len = 128;
        config.lru_purge_enable = true;
        config.recv_wait_timeout = 3;
        config.send_wait_timeout = 3;
        config.if_name = &ap_interface_;
        ESP_RETURN_ON_ERROR(httpd_start(&http_, &config), kTag, "HTTP startup failed");
        const httpd_uri_t routes[] = {
            {.uri = "/", .method = HTTP_GET, .handler = Page, .user_ctx = this},
            {.uri = "/api/status", .method = HTTP_GET, .handler = Status, .user_ctx = this},
            {.uri = "/api/scan", .method = HTTP_GET, .handler = Scan, .user_ctx = this},
            {.uri = "/api/connect", .method = HTTP_POST, .handler = Connect, .user_ctx = this},
            {.uri = "/api/reconnect", .method = HTTP_POST, .handler = Reconnect, .user_ctx = this},
            // Keep these last: exact application routes take priority. Normal
            // OS connectivity probes must not take the warning-producing 404 path.
            {.uri = "/*", .method = HTTP_GET, .handler = PortalProbe, .user_ctx = this},
            {.uri = "/*", .method = HTTP_HEAD, .handler = PortalProbe, .user_ctx = this},
        };
        for (const auto& route : routes) {
            ESP_RETURN_ON_ERROR(httpd_register_uri_handler(http_, &route), kTag,
                                "HTTP route setup failed");
        }
        ESP_RETURN_ON_ERROR(httpd_register_err_handler(http_, HTTPD_404_NOT_FOUND, Redirect), kTag,
                            "Captive redirect setup failed");
        ESP_LOGI(kTag, "SETUP AP: %s", status_.ap_ssid);
        ESP_LOGI(kTag, "SETUP AP AUTH: OPEN (no hotspot password)");
        ESP_LOGI(kTag, "SETUP MODE: manual; automatic reconnect paused until explicit selection");
        ESP_LOGI(kTag, "SETUP URL: http://192.168.4.1 (2.4 GHz, SoftAP only)");
    }
    ESP_RETURN_ON_ERROR(dns_.Start(ap_interface_), kTag, "Captive DNS startup failed");
    return ESP_OK;
}

esp_err_t WifiSetup::ClosePortal() {
    dns_.Stop();
    if (http_) {
        ESP_RETURN_ON_ERROR(httpd_stop(http_), kTag, "HTTP shutdown failed");
        http_ = nullptr;
    }
    if (status_.portal_open) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), kTag, "AP shutdown failed");
        status_.portal_open = false;
        connection_policy_.SetupCompleted();
        Publish(nullptr);
        ESP_LOGI(kTag, "SETUP AP CLOSED; saved WiFi will reconnect after power cycle");
    }
    return ESP_OK;
}

void WifiSetup::BeginAttempt(const Credentials& credentials, bool user_supplied) {
    if (connecting_ || disconnecting_ || online_.load())
        return;
    if (!user_supplied && !connection_policy_.AutomaticReconnectAllowed()) {
        ESP_LOGI(kTag, "AUTO CONNECT SKIPPED: manual setup is waiting for parent selection");
        return;
    }
    if (scanning_) {
        esp_wifi_scan_stop();
        esp_wifi_clear_ap_list();
        scanning_ = false;
    }
    pending_ = credentials;
    error_led_until_ = 0;
    user_attempt_ = user_supplied;
    wifi_config_t station = {};
    memcpy(station.sta.ssid, credentials.ssid, strlen(credentials.ssid));
    memcpy(station.sta.password, credentials.password, strlen(credentials.password));
    station.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    station.sta.threshold.authmode = credentials.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    station.sta.pmf_cfg.capable = true;
    const esp_err_t configured = esp_wifi_set_config(WIFI_IF_STA, &station);
    if (configured != ESP_OK) {
        FailAttempt("网络配置失败，请重试", false);
        return;
    }
    connecting_ = true;
    retry_at_ = 0;
    connection_deadline_ = Now() + kAttemptMs;
    portal_close_at_ = 0;
    status_.phase = Phase::Connecting;
    status_.failed = false;
    ++status_.attempt;
    snprintf(status_.target, sizeof(status_.target), "%s", credentials.ssid);
    led_->SetNetworkState(NetworkLedState::Connecting);
    Publish("正在连接家庭 Wi-Fi，请稍候（最多20秒）");
    ESP_LOGI(kTag, "CONNECT attempt=%u; source=%s; credentials not logged", status_.attempt,
             user_supplied ? "portal" : "saved");
    const esp_err_t result = esp_wifi_connect();
    if (result != ESP_OK)
        FailAttempt("连接未能启动，请重试", true);
}

void WifiSetup::FailAttempt(const char* message, bool cancel_driver) {
    connecting_ = false;
    retry_at_ = 0;
    online_.store(false);
    portal_close_at_ = 0;
    if (cancel_driver) {
        disconnecting_ = esp_wifi_disconnect() == ESP_OK;
        disconnect_deadline_ = Now() + 1000;
    }
    const esp_err_t result = OpenPortal();
    if (result != ESP_OK) {
        Fault(result, "open portal after failure");
        return;
    }
    status_.phase = Phase::Setup;
    status_.failed = true;
    status_.ip[0] = 0;
    cue_.store(WifiCue::Failed);
    RetryError(message);
    ESP_LOGW(kTag, "CONNECT FAILED: %s; previous saved credentials retained", message);
}

void WifiSetup::GotAddress() {
    if (!connecting_ || disconnecting_)
        return;
    wifi_ap_record_t current = {};
    esp_netif_ip_info_t address = {};
    if (esp_wifi_sta_get_ap_info(&current) != ESP_OK ||
        esp_netif_get_ip_info(station_, &address) != ESP_OK || address.ip.addr == 0 ||
        strncmp(reinterpret_cast<const char*>(current.ssid), pending_.ssid, 32) != 0)
        return;
    const esp_err_t saved = SaveCredentials();
    if (saved != ESP_OK) {
        FailAttempt("Wi-Fi 已连接但保存失败，请重试", true);
        return;
    }
    connecting_ = false;
    status_.phase = Phase::Connected;
    status_.failed = false;
    snprintf(status_.ip, sizeof(status_.ip), IPSTR, IP2STR(&address.ip));
    error_led_until_ = 0;
    led_->SetNetworkState(NetworkLedState::None);
    portal_close_at_ = status_.portal_open ? Now() + 5000 : 0;
    Publish(status_.portal_open
                ? "连接成功！配置已保存，设备热点将在5秒后关闭。可以回到家庭 Wi-Fi。"
                : "已连接保存的 Wi-Fi");
    online_.store(true);
    cue_.store(WifiCue::Connected);
    ESP_LOGI(kTag, "CONNECTED: ip=%s; saved=%s; %s", status_.ip,
             user_attempt_ ? "yes" : "already saved",
             status_.portal_open ? "AP closes after 5 seconds" : "setup AP stays off");
    if (user_attempt_)
        ESP_LOGI(kTag,
                 "SETUP CONFIRMED: normal future boots may reconnect; saved history retained");
}

void WifiSetup::FinishScan() {
    if (!scanning_)
        return;
    scanning_ = false;
    uint16_t count = kMaxNetworks;
    const esp_err_t result = esp_wifi_scan_get_ap_records(&count, scan_records_);
    status_.count = 0;
    if (result == ESP_OK) {
        for (unsigned i = 0; i < count; ++i) {
            const auto& record = scan_records_[i];
            const unsigned bytes = strnlen(reinterpret_cast<const char*>(record.ssid), 32);
            if (!bytes || !ValidWifiText(reinterpret_cast<const char*>(record.ssid), bytes))
                continue;
            bool duplicate = false;
            for (unsigned j = 0; j < status_.count; ++j) {
                if (strlen(status_.networks[j].ssid) == bytes &&
                    memcmp(status_.networks[j].ssid, record.ssid, bytes) == 0)
                    duplicate = true;
            }
            if (duplicate)
                continue;
            auto& item = status_.networks[status_.count++];
            item = Network{};
            memcpy(item.ssid, record.ssid, bytes);
            item.rssi = record.rssi;
            item.secured = record.authmode != WIFI_AUTH_OPEN;
        }
    }
    if (result == ESP_OK)
        Publish(nullptr);
    else
        RetryError("扫描失败，可以重试或手动输入 Wi-Fi 名称");
}

void WifiSetup::RetryError(const char* message) {
    status_.failed = true;
    error_led_until_ = Now() + 2000;
    led_->SetNetworkState(NetworkLedState::RetryError);
    Publish(message);
}

void WifiSetup::Fault(esp_err_t error, const char* where) {
    online_.store(false);
    fatal_.store(true);
    status_.phase = Phase::Fault;
    status_.failed = true;
    led_->SetNetworkState(NetworkLedState::Fault);
    Publish("网络模块异常，请查看串口日志并重新上电");
    ESP_LOGE(kTag, "%s: %s", where, esp_err_to_name(error));
}

void WifiSetup::TaskEntry(void* context) {
    static_cast<WifiSetup*>(context)->TaskMain();
    vTaskDelete(nullptr);
}

void WifiSetup::TaskMain() {
    if (!(xEventGroupWaitBits(events_, kStarted, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000)) &
          kStarted)) {
        Fault(ESP_ERR_TIMEOUT, "station start event");
        return;
    }
    if (connection_policy_.AutomaticReconnectAllowed())
        BeginAttempt(history_.items[0], false);
    else {
        const esp_err_t opened = OpenPortal();
        if (opened != ESP_OK) {
            Fault(opened, "initial portal");
            return;
        }
        status_.phase = Phase::Setup;
        led_->SetNetworkState(NetworkLedState::Setup);
        cue_.store(WifiCue::Setup);
        Publish(force_setup_ ? "已进入重新配网，原配置会保留到新网络连接成功"
                             : "请选择家庭的2.4GHz Wi-Fi");
    }
    while (!fatal_.load()) {
        const EventBits_t bits =
            xEventGroupWaitBits(events_, kDisconnected | kGotIp | kScanDone, pdTRUE, pdFALSE, 0);
        if (bits & kDisconnected) {
            if (disconnecting_) {
                disconnecting_ = false;
                Publish(nullptr);
            } else if (online_.exchange(false)) {
                if (connection_policy_.manual_setup())
                    FailAttempt("连接已断开，请选择网络重新连接", false);
                else {
                    ESP_LOGW(kTag, "WiFi lost; reconnecting for up to 20 seconds");
                    BeginAttempt(history_.items[0], false);
                }
            } else if (connecting_) {
                const unsigned reason = disconnect_reason_.load();
                if (user_attempt_ || reason == WIFI_REASON_AUTH_FAIL ||
                    reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) {
                    FailAttempt(reason == WIFI_REASON_AUTH_FAIL ||
                                        reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
                                    ? "认证失败，请检查 Wi-Fi 密码与路由器设置"
                                    : "连接失败，请检查距离、密码和2.4GHz网络",
                                false);
                } else
                    retry_at_ = Now() + 1000;
            }
        }
        if (bits & kGotIp)
            GotAddress();
        if (bits & kScanDone)
            FinishScan();
        const int64_t now = Now();
        if (error_led_until_ && now >= error_led_until_ && !fatal_.load()) {
            error_led_until_ = 0;
            led_->SetNetworkState(connecting_ ? NetworkLedState::Connecting
                                              : NetworkLedState::Setup);
        }
        if (disconnecting_ && now >= disconnect_deadline_) {
            disconnecting_ = false;
            Publish(nullptr);
        }
        if (connecting_ && now >= connection_deadline_)
            FailAttempt("连接超时，请检查路由器后重试", true);
        else if (connecting_ && retry_at_ && now >= retry_at_) {
            retry_at_ = 0;
            if (esp_wifi_connect() != ESP_OK)
                FailAttempt("重连失败，请重新配网", true);
        }
        if (scanning_ && now >= scan_deadline_) {
            esp_wifi_scan_stop();
            esp_wifi_clear_ap_list();
            scanning_ = false;
            RetryError("扫描超时，可以重试或手动输入 Wi-Fi 名称");
        }
        if (portal_close_at_ && now >= portal_close_at_) {
            portal_close_at_ = 0;
            const esp_err_t closed = ClosePortal();
            if (closed != ESP_OK)
                Fault(closed, "close portal");
        }
        Command command = {};
        if (xQueueReceive(commands_, &command, pdMS_TO_TICKS(100)) != pdTRUE)
            continue;
        if (command.type != CommandType::Scan) {
            if (command.type == CommandType::Connect)
                BeginAttempt(command.credentials, true);
            else {
                const int found = history_.Find(command.credentials.ssid);
                if (found >= 0)
                    BeginAttempt(history_.items[found], true);
                else
                    RetryError("已保存的网络不存在，请输入名称与密码重新连接");
            }
            submit_pending_.store(false);
            // Only this bounded queue entry and the pending RAM copy contain the trial secret.
            command.credentials = Credentials{};
        } else {
            scan_pending_.store(false);
            if (!connecting_ && !disconnecting_ && !online_.load() && !scanning_) {
                wifi_scan_config_t scan = {};
                scan.show_hidden = false;
                const esp_err_t result = esp_wifi_scan_start(&scan, false);
                if (result == ESP_OK) {
                    scanning_ = true;
                    scan_deadline_ = Now() + 10000;
                    Publish(nullptr);
                } else
                    RetryError("扫描暂不可用，可以重试或手动输入 Wi-Fi 名称");
            }
        }
    }
}

bool WifiSetup::CheckToken(httpd_req_t* request) {
    if (httpd_req_get_hdr_value_len(request, "X-Setup-Token") != 32)
        return false;
    char supplied[33];
    if (httpd_req_get_hdr_value_str(request, "X-Setup-Token", supplied, sizeof(supplied)) != ESP_OK)
        return false;
    unsigned difference = 0;
    for (unsigned i = 0; i < 32; ++i)
        difference |= supplied[i] ^ csrf_[i];
    return difference == 0;
}

esp_err_t WifiSetup::PortalProbe(httpd_req_t* request) {
    return Redirect(request, HTTPD_404_NOT_FOUND);
}

esp_err_t WifiSetup::Redirect(httpd_req_t* request, httpd_err_code_t) {
    // Android/iOS/Windows probe different HTTP URLs. Return a real body as
    // well as an absolute redirect, so they can recognize the local portal.
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", kPortalUrl);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Connection", "close");
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    const char* body = request->method == HTTP_HEAD
                           ? ""
                           : "<!doctype html><meta charset=utf-8><a href='http://192.168.4.1/'>"
                             "连接思巴迪 Wi-Fi</a>";
    const esp_err_t sent = httpd_resp_sendstr(request, body);
    if (sent != ESP_OK)
        return sent;
    // Returning ESP_FAIL from a regular handler creates another warning.
    // Queue a normal close instead, so probes do not occupy scarce sockets.
    return httpd_sess_trigger_close(request->handle, httpd_req_to_sockfd(request));
}

esp_err_t WifiSetup::Page(httpd_req_t* request) {
    auto& self = *static_cast<WifiSetup*>(request->user_ctx);
    const char* page = reinterpret_cast<const char*>(setup_page_start);
    const char* token = strstr(page, "__CSRF_TOKEN__");
    if (!token)
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Page unavailable");
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(
        request, "Content-Security-Policy",
        "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src "
        "'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, page, token - page), kTag,
                        "Page send failed");
    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(request, self.csrf_), kTag, "Token send failed");
    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(request, token + strlen("__CSRF_TOKEN__")), kTag,
                        "Page send failed");
    return httpd_resp_send_chunk(request, nullptr, 0);
}

esp_err_t WifiSetup::Status(httpd_req_t* request) {
    auto& self = *static_cast<WifiSetup*>(request->user_ctx);
    Snapshot snapshot;
    if (!self.ReadSnapshot(snapshot))
        return ApiError(request, "503 Service Unavailable", "设备繁忙，请稍后重试");
    cJSON* root = cJSON_CreateObject();
    if (root) {
        const char* phase = snapshot.phase == Phase::Connected    ? "connected"
                            : snapshot.phase == Phase::Connecting ? "connecting"
                            : snapshot.phase == Phase::Fault      ? "error"
                                                                  : "setup";
        cJSON_AddStringToObject(root, "phase", phase);
        cJSON_AddStringToObject(root, "message", snapshot.message);
        cJSON_AddBoolToObject(root, "failed", snapshot.failed);
        cJSON_AddBoolToObject(root, "manual_setup", snapshot.manual_setup);
        cJSON_AddStringToObject(root, "ap", snapshot.ap_ssid);
        cJSON_AddStringToObject(root, "ssid", snapshot.target);
        cJSON_AddStringToObject(root, "ip", snapshot.ip);
        cJSON_AddNumberToObject(root, "attempt", snapshot.attempt);
        cJSON_AddBoolToObject(root, "busy", snapshot.busy || self.submit_pending_.load());
        cJSON_AddBoolToObject(root, "scanning", snapshot.scanning || self.scan_pending_.load());
        cJSON* saved = cJSON_AddArrayToObject(root, "saved_networks");
        if (saved)
            for (unsigned i = 0; i < snapshot.saved_count; ++i)
                cJSON_AddItemToArray(saved, cJSON_CreateString(snapshot.saved_networks[i]));
        cJSON* networks = cJSON_AddArrayToObject(root, "networks");
        if (networks)
            for (unsigned i = 0; i < snapshot.count; ++i) {
                cJSON* item = cJSON_CreateObject();
                if (!item)
                    break;
                cJSON_AddStringToObject(item, "ssid", snapshot.networks[i].ssid);
                cJSON_AddNumberToObject(item, "rssi", snapshot.networks[i].rssi);
                cJSON_AddBoolToObject(item, "secured", snapshot.networks[i].secured);
                cJSON_AddItemToArray(networks, item);
            }
    }
    return JsonResponse(request, root);
}

esp_err_t WifiSetup::Scan(httpd_req_t* request) {
    auto& self = *static_cast<WifiSetup*>(request->user_ctx);
    if (self.failed())
        return ApiError(request, "503 Service Unavailable", "网络模块异常，请重新上电");
    if (!self.CheckToken(request))
        return ApiError(request, "403 Forbidden", "页面已失效，请刷新后重试");
    bool expected = false;
    if (self.scan_pending_.compare_exchange_strong(expected, true)) {
        Command command = {};
        command.type = CommandType::Scan;
        if (xQueueSend(self.commands_, &command, 0) != pdTRUE)
            self.scan_pending_.store(false);
    }
    return Status(request);
}

esp_err_t WifiSetup::Connect(httpd_req_t* request) { return Submit(request, false); }

esp_err_t WifiSetup::Reconnect(httpd_req_t* request) { return Submit(request, true); }

esp_err_t WifiSetup::Submit(httpd_req_t* request, bool saved) {
    auto& self = *static_cast<WifiSetup*>(request->user_ctx);
    if (self.failed())
        return ApiError(request, "503 Service Unavailable", "网络模块异常，请重新上电");
    if (!self.CheckToken(request))
        return ApiError(request, "403 Forbidden", "页面已失效，请刷新后重试");
    char action[24] = {};
    const char* expected_action = saved ? "history-reconnect" : "manual-connect";
    if (httpd_req_get_hdr_value_str(request, "X-Setup-Action", action, sizeof(action)) != ESP_OK ||
        strcmp(action, expected_action) != 0)
        return ApiError(request, "400 Bad Request", "配网页已更新，请刷新后点击连接");
    if (request->content_len <= 0 || request->content_len > 512)
        return ApiError(request, "400 Bad Request", "输入内容过长或为空");
    char content_type[48] = {};
    if (httpd_req_get_hdr_value_str(request, "Content-Type", content_type, sizeof(content_type)) !=
            ESP_OK ||
        strncmp(content_type, "application/json", 16) != 0 ||
        (content_type[16] != 0 && content_type[16] != ';'))
        return ApiError(request, "415 Unsupported Media Type", "需要JSON请求");
    char body[513] = {};
    size_t total = 0;
    while (total < static_cast<size_t>(request->content_len)) {
        const int bytes = httpd_req_recv(request, body + total, request->content_len - total);
        if (bytes <= 0)
            return ESP_FAIL;
        total += bytes;
    }
    if (!FlatSetupJson(body, total))
        return ApiError(request, "400 Bad Request", "请求格式不正确");
    const char* end = nullptr;
    cJSON* root = cJSON_ParseWithLengthOpts(body, total + 1, &end, true);
    const cJSON* ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON* password = cJSON_GetObjectItemCaseSensitive(root, "password");
    const bool shape_ok = cJSON_IsObject(root) && cJSON_GetArraySize(root) == (saved ? 1 : 2) &&
                          cJSON_IsString(ssid) && (saved || cJSON_IsString(password));
    const unsigned ssid_bytes = shape_ok ? strlen(ssid->valuestring) : 0;
    const char* secret = shape_ok && !saved ? password->valuestring : "";
    const unsigned password_bytes = strlen(secret);
    if (!shape_ok || !ValidWifiCredentials(ssid->valuestring, ssid_bytes, secret, password_bytes)) {
        cJSON_Delete(root);
        memset(body, 0, sizeof(body));
        return ApiError(request, "400 Bad Request",
                        "Wi-Fi名称需1至32字节，密码需8至63字节；无密码网络请勾选对应选项");
    }
    Snapshot snapshot;
    if (!self.ReadSnapshot(snapshot) || !snapshot.manual_setup || snapshot.busy ||
        self.online_.load()) {
        cJSON_Delete(root);
        memset(body, 0, sizeof(body));
        return ApiError(request, "409 Conflict", "设备正在连接或已连接，请稍候");
    }
    if (saved) {
        bool found = false;
        for (unsigned i = 0; i < snapshot.saved_count; ++i)
            if (strcmp(snapshot.saved_networks[i], ssid->valuestring) == 0)
                found = true;
        if (!found) {
            cJSON_Delete(root);
            memset(body, 0, sizeof(body));
            return ApiError(request, "404 Not Found", "没有这个历史网络，请输入密码连接");
        }
    }
    bool expected = false;
    if (!self.submit_pending_.compare_exchange_strong(expected, true)) {
        cJSON_Delete(root);
        memset(body, 0, sizeof(body));
        return ApiError(request, "409 Conflict", "已有连接请求，请稍候");
    }
    Command command = {};
    command.type = saved ? CommandType::ConnectSaved : CommandType::Connect;
    memcpy(command.credentials.ssid, ssid->valuestring, ssid_bytes);
    memcpy(command.credentials.password, secret, password_bytes);
    cJSON_Delete(root);
    memset(body, 0, sizeof(body));
    const bool queued = xQueueSend(self.commands_, &command, 0) == pdTRUE;
    command.credentials = Credentials{};
    if (!queued) {
        self.submit_pending_.store(false);
        return ApiError(request, "503 Service Unavailable", "设备繁忙，请重试");
    }
    ESP_LOGI(kTag, "CONNECT REQUEST: action=%s; accepted from setup page; credentials not logged",
             expected_action);
    httpd_resp_set_status(request, "202 Accepted");
    cJSON* response = cJSON_CreateObject();
    if (response)
        cJSON_AddBoolToObject(response, "accepted", true);
    return JsonResponse(request, response);
}

}  // namespace stubuddy
