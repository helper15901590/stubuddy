#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "audio_levels.h"

namespace stubuddy {
constexpr unsigned UniformBreath(unsigned elapsed_ms) {
    const unsigned phase = elapsed_ms % 2000;
    const uint64_t x = phase <= 1000 ? phase : 2000 - phase;
    return unsigned(x * x * (3000 - 2 * x) * 1023 / 1000000000ULL);
}

class PlaybackEnvelope {
public:
    unsigned Update(unsigned mean_square) {
        const double rms = std::sqrt(double(mean_square));
        const unsigned target =
            rms < 4 ? 0 : unsigned(1023 * std::sqrt(std::min(1.0, rms / kOutputPeakPcm16)));
        value_ = target > value_ ? (value_ + 2 * target) / 3 : (3 * value_ + target) / 4;
        if (value_ < 2)
            value_ = 0;
        return value_;
    }
    void Reset() { value_ = 0; }

private:
    unsigned value_ = 0;
};
}  // namespace stubuddy
