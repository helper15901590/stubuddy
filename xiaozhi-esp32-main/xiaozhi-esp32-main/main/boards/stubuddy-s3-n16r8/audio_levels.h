#pragma once

#include <cstdint>

namespace stubuddy {
// One board-local reference for PCM normalization, cues and the playback LED.
inline constexpr int kOutputPeakPercent = 70;
inline constexpr int kOutputPeakPcm16 = (INT16_MAX * kOutputPeakPercent + 50) / 100;
inline constexpr float kOutputPeakRatio = float(kOutputPeakPercent) / 100;
static_assert(kOutputPeakPcm16 == 22937);
}  // namespace stubuddy
