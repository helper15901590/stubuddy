#include "board.h"
#include "startup_audio.h"

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

extern "C" void app_main() {
    constexpr char kTag[] = "STUBUDDY_STARTUP";
    auto& board = Board::GetInstance();
    ESP_LOGI(kTag, "CHECKPOINT 3.1; firmware=%s; board=%s; variant=%s",
             esp_app_get_description()->version, board.GetBoardType().c_str(), BOARD_NAME);
    const bool success = stubuddy::PlayStartupPrompt(board);
    ESP_LOGI(kTag, "%s: local standby; amplifier OFF; LED OFF; no automatic repeat",
             success ? "DONE" : "FAILED");
    ESP_LOGI(kTag, "HEAP: internal_free=%u; PSRAM_free=%u",
             unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}
