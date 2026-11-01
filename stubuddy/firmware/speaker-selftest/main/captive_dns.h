#pragma once

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lwip/sockets.h>
#include <atomic>

namespace stubuddy {

// Controlled by the WiFi task; the UDP socket exists only while the AP is open.
class CaptiveDns {
public:
    esp_err_t Start(const struct ifreq& interface);
    void Stop();

private:
    static void TaskEntry(void* context);
    void Run();
    int socket_ = -1;
    bool task_active_ = false;
    SemaphoreHandle_t stopped_ = nullptr;
    std::atomic<bool> running_{false};
};

}  // namespace stubuddy
