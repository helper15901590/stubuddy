#include "captive_dns.h"
#include "captive_dns_packet.h"

#include <esp_log.h>
#include <freertos/task.h>
#include <cerrno>

namespace stubuddy {

esp_err_t CaptiveDns::Start(const struct ifreq& interface) {
    if (task_active_)
        return ESP_OK;
    if (!stopped_)
        stopped_ = xSemaphoreCreateBinary();
    if (!stopped_)
        return ESP_ERR_NO_MEM;
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ < 0)
        return ESP_FAIL;
    const timeval timeout = {.tv_sec = 0, .tv_usec = 250000};
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(53);
    local.sin_addr.s_addr = inet_addr("192.168.4.1");
    if (setsockopt(socket_, SOL_SOCKET, SO_BINDTODEVICE, &interface, sizeof(interface)) != 0 ||
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        bind(socket_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        Stop();
        return ESP_FAIL;
    }
    running_.store(true);
    if (xTaskCreate(TaskEntry, "stubuddy_dns", 4096, this, 2, nullptr) != pdPASS) {
        Stop();
        return ESP_ERR_NO_MEM;
    }
    task_active_ = true;
    ESP_LOGI("STUBUDDY_WIFI", "CAPTIVE DNS: AP-only 192.168.4.1:53");
    return ESP_OK;
}

void CaptiveDns::Stop() {
    running_.store(false);
    if (task_active_) {
        // The receiver has a 250ms timeout. Never close a descriptor that its
        // task could still be using, or let it answer after AP shutdown.
        xSemaphoreTake(stopped_, portMAX_DELAY);
        task_active_ = false;
    }
    if (socket_ >= 0) {
        close(socket_);
        socket_ = -1;
    }
}

void CaptiveDns::TaskEntry(void* context) {
    auto& self = *static_cast<CaptiveDns*>(context);
    self.Run();
    xSemaphoreGive(self.stopped_);
    vTaskDelete(nullptr);
}

void CaptiveDns::Run() {
    constexpr uint8_t address[] = {192, 168, 4, 1};
    uint8_t packet[512];
    while (running_.load()) {
        sockaddr_in client = {};
        socklen_t client_size = sizeof(client);
        const int length = recvfrom(socket_, packet, sizeof(packet), 0,
                                    reinterpret_cast<sockaddr*>(&client), &client_size);
        if (length < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            ESP_LOGW("STUBUDDY_WIFI", "Captive DNS receive failed: errno=%d", errno);
            break;
        }
        if (!running_.load() || (ntohl(client.sin_addr.s_addr) & 0xffffff00U) != 0xc0a80400U)
            continue;
        const unsigned reply = CaptiveDnsReply(packet, length, sizeof(packet), address);
        if (reply)
            sendto(socket_, packet, reply, 0, reinterpret_cast<sockaddr*>(&client), client_size);
    }
}

}  // namespace stubuddy
