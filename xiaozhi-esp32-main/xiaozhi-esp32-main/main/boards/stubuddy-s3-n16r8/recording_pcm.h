#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace stubuddy {

struct RecordingLevels {
    int32_t mean = 0;
    int32_t peak = 0;
    size_t clipped = 0;
    double rms = 0;
    float gain = 0;
};

// Same DC removal and bounded 3% playback peak as the accepted diagnostic.
// Keep this independent of IDF to exercise silence/DC/extreme PCM on the host.
inline RecordingLevels AnalyzeRecording(const int16_t* samples, size_t count) {
    RecordingLevels levels;
    if (!count)
        return levels;
    int64_t sum = 0;
    for (size_t i = 0; i < count; ++i)
        sum += samples[i];
    levels.mean = static_cast<int32_t>(sum / static_cast<int64_t>(count));
    double energy = 0;
    for (size_t i = 0; i < count; ++i) {
        const int32_t centered = int32_t(samples[i]) - levels.mean;
        levels.peak = std::max(levels.peak, std::abs(centered));
        energy += double(centered) * centered;
        levels.clipped += samples[i] == INT16_MIN || samples[i] == INT16_MAX;
    }
    levels.rms = std::sqrt(energy / count);
    levels.gain = levels.peak > 0 ? std::min(8.0f, 983.0f / levels.peak) : 0.0f;
    return levels;
}

inline int16_t ReplaySample(int16_t raw, size_t index, size_t count, const RecordingLevels& levels,
                            unsigned fade_samples) {
    if (index >= count || fade_samples == 0)
        return 0;
    const size_t edge = std::min(index, count - 1 - index);
    const float envelope = std::min(1.0f, float(edge) / fade_samples);
    const float value = (int32_t(raw) - levels.mean) * levels.gain * envelope;
    return static_cast<int16_t>(std::clamp(value, -983.0f, 983.0f));
}
}  // namespace stubuddy
