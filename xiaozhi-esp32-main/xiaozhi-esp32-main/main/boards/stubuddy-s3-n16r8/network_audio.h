#pragma once

#include <esp_err.h>

class AudioCodec;
namespace stubuddy {
class StatusLed;
enum class WifiCue : unsigned;

// Called only by the PTT owner, with RX closed and no recording/replay active.
esp_err_t PlayWifiCue(AudioCodec& codec, StatusLed& led, WifiCue cue);
}  // namespace stubuddy
