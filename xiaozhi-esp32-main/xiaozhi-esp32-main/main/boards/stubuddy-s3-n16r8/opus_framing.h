#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace stubuddy {
inline constexpr size_t kOpusFrameSamples = 960;
inline constexpr size_t kOpusMaxSamples = 960000;
inline constexpr size_t kOpusMaxPackets = kOpusMaxSamples / kOpusFrameSamples + 1;
inline constexpr size_t kOpusMaxPacketBytes = 4096;

constexpr size_t OpusPacketsNeeded(size_t samples, size_t delay) {
    return samples == 0 || samples > kOpusMaxSamples || delay > kOpusFrameSamples
               ? 0
               : (samples + delay + kOpusFrameSamples - 1) / kOpusFrameSamples;
}

struct OpusPacketView {
    uint8_t* data = nullptr;
    size_t size = 0;
};

// Local RAM framing only. A future WebSocket sends each payload without this
// two-byte length prefix. No per-packet allocations or persisted recordings.
class OpusPacketStore {
public:
    OpusPacketStore(uint8_t* data, size_t capacity, size_t packet_limit)
        : data_(data), capacity_(capacity), packet_limit_(packet_limit) {}

    bool Push(const uint8_t* packet, size_t size) {
        if (!data_ || !packet || !size || size > packet_limit_ || size > kOpusMaxPacketBytes ||
            count_ >= kOpusMaxPackets || used_ > capacity_ || size + 2 > capacity_ - used_)
            return false;
        data_[used_] = static_cast<uint8_t>(size);
        data_[used_ + 1] = static_cast<uint8_t>(size >> 8);
        std::memcpy(data_ + used_ + 2, packet, size);
        used_ += size + 2;
        payload_bytes_ += size;
        ++count_;
        dirty_ = std::max(dirty_, used_);
        return true;
    }

    bool Next(size_t& cursor, OpusPacketView& packet) const {
        packet = {};
        if (!data_ || cursor > used_ || used_ - cursor < 2)
            return false;
        const size_t size = size_t(data_[cursor]) | (size_t(data_[cursor + 1]) << 8);
        if (!size || size > packet_limit_ || size > kOpusMaxPacketBytes ||
            size > used_ - cursor - 2)
            return false;
        packet = {data_ + cursor + 2, size};
        cursor += size + 2;
        return true;
    }

    // Invalidate old audio immediately on interruption. Erase the high-water
    // range once the microphone is off, so clearing never delays a first word.
    void Reset() { used_ = count_ = payload_bytes_ = 0; }
    void Erase() {
        if (data_ && dirty_)
            std::memset(data_, 0, dirty_);
        dirty_ = 0;
        Reset();
    }
    size_t used() const { return used_; }
    size_t count() const { return count_; }
    size_t payload_bytes() const { return payload_bytes_; }

private:
    uint8_t* data_;
    size_t capacity_;
    size_t packet_limit_;
    size_t used_ = 0;
    size_t count_ = 0;
    size_t payload_bytes_ = 0;
    size_t dirty_ = 0;
};

// Intersect one decoded frame with [delay, delay + original sample count).
// Padding and codec delay never extend playback or overrun the PCM buffer.
inline size_t CopyOpusPcm(const int16_t* frame, size_t frame_samples, size_t stream_offset,
                          size_t delay, size_t original_samples, int16_t* output, size_t capacity) {
    if (!frame || !output || !frame_samples || frame_samples > kOpusFrameSamples ||
        original_samples > capacity || original_samples > kOpusMaxSamples ||
        delay > kOpusFrameSamples || stream_offset > kOpusMaxPackets * kOpusFrameSamples)
        return 0;
    const size_t first = std::max(stream_offset, delay);
    const size_t last = std::min(stream_offset + frame_samples, delay + original_samples);
    if (last <= first)
        return 0;
    std::memcpy(output + first - delay, frame + first - stream_offset,
                (last - first) * sizeof(int16_t));
    return last - first;
}

struct OpusDelayEstimate {
    size_t samples = 0;
    double correlation = 0;
};

// The public codec wrapper does not expose OPUS_GET_LOOKAHEAD. Measure its
// effective delay with a deterministic, silent RAM-only loopback at boot.
inline OpusDelayEstimate EstimateOpusDelay(const int16_t* reference, size_t reference_size,
                                           const int16_t* decoded, size_t decoded_size) {
    OpusDelayEstimate best;
    constexpr size_t start = kOpusFrameSamples;
    constexpr size_t window = kOpusFrameSamples;
    if (!reference || !decoded || reference_size < start + window ||
        decoded_size < start + window + kOpusFrameSamples)
        return best;
    for (size_t delay = 0; delay <= kOpusFrameSamples; ++delay) {
        int64_t cross = 0, energy_x = 0, energy_y = 0, sum_x = 0, sum_y = 0;
        for (size_t i = 0; i < window; ++i) {
            const int64_t x = reference[start + i];
            const int64_t y = decoded[start + i + delay];
            cross += x * y;
            energy_x += x * x;
            energy_y += y * y;
            sum_x += x;
            sum_y += y;
        }
        const double covariance = double(cross) - double(sum_x) * sum_y / window;
        const double variance_x = double(energy_x) - double(sum_x) * sum_x / window;
        const double variance_y = double(energy_y) - double(sum_y) * sum_y / window;
        if (variance_x <= 0 || variance_y <= 0)
            continue;
        const double correlation = covariance / std::sqrt(variance_x * variance_y);
        if (correlation > best.correlation)
            best = {delay, correlation};
    }
    return best;
}
}  // namespace stubuddy
