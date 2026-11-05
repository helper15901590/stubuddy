#pragma once
#include <cstdint>

namespace stubuddy {
// One versioned NVS blob stores current and last audible volume atomically.
// The original speech_volume key remains a read-only migration source.
struct SpeechVolumeState {
    uint32_t version = 0x53564232;
    int32_t volume = 100;
    int32_t audible = 100;
    bool Valid() const {
        return version == 0x53564232 && volume >= 0 && volume <= 100 && audible > 0 &&
               audible <= 100 && (volume == 0 || volume == audible);
    }
    static SpeechVolumeState Legacy(int value) {
        return {0x53564232, value, value > 0 ? value : 100};
    }
    SpeechVolumeState WithVolume(int value) const {
        return {version, value, value > 0 ? value : audible};
    }
};
static_assert(sizeof(SpeechVolumeState) == 12);
}  // namespace stubuddy
