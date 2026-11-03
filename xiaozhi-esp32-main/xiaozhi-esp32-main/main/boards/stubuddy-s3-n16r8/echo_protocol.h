#pragma once

#include "opus_framing.h"

#include <array>
#include <cstring>

namespace stubuddy::echo {
inline constexpr size_t kHeaderBytes = 16;
inline constexpr size_t kMaxMessage = kHeaderBytes + kOpusMaxPacketBytes;
inline constexpr unsigned kProgressTimeoutMs = 15000;

inline uint32_t Crc32(uint32_t crc, const uint8_t* data, size_t size) {
    crc ^= 0xffffffffU;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return crc ^ 0xffffffffU;
}

inline uint32_t Read32(const uint8_t* data) {
    return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) |
           uint32_t(data[3]);
}
inline void Write32(uint8_t* data, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        data[i] = static_cast<uint8_t>(value >> (24 - 8 * i));
}

struct AudioView {
    uint32_t turn = 0;
    uint32_t sequence = 0;
    const uint8_t* data = nullptr;
    size_t size = 0;
};

inline bool ParseAudio(const uint8_t* data, size_t size, AudioView& frame) {
    frame = {};
    if (!data || size <= kHeaderBytes || size > kMaxMessage || data[0] != 'S' || data[1] != 'B' ||
        data[2] != 1 || data[3] != 0)
        return false;
    const uint32_t turn = Read32(data + 4), sequence = Read32(data + 8), length = Read32(data + 12);
    if (!turn || sequence >= kOpusMaxPackets || length != size - kHeaderBytes)
        return false;
    frame = {turn, sequence, data + kHeaderBytes, length};
    return true;
}

inline size_t PackAudio(uint8_t* output, size_t capacity, uint32_t turn, uint32_t sequence,
                        const uint8_t* payload, size_t size) {
    if (!output || !payload || !turn || sequence >= kOpusMaxPackets || !size ||
        size > kOpusMaxPacketBytes || capacity < kHeaderBytes + size)
        return 0;
    output[0] = 'S';
    output[1] = 'B';
    output[2] = 1;
    output[3] = 0;
    Write32(output + 4, turn);
    Write32(output + 8, sequence);
    Write32(output + 12, size);
    std::memcpy(output + kHeaderBytes, payload, size);
    return kHeaderBytes + size;
}

struct Metadata {
    uint32_t turn = 0, samples = 0, delay = 0, frames = 0, bytes = 0, crc32 = 0;
    bool operator==(const Metadata&) const = default;
    bool valid() const {
        return turn && samples >= 4800 && samples <= kOpusMaxSamples && delay < kOpusFrameSamples &&
               frames == OpusPacketsNeeded(samples, delay) && frames && frames <= kOpusMaxPackets &&
               bytes >= frames && bytes <= frames * kOpusMaxPacketBytes;
    }
};

inline bool JsonDepthAllowed(const uint8_t* data, size_t size) {
    if (!data || !size)
        return false;
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < size; ++i) {
        const auto c = data[i];
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
        } else if (c == '"')
            quoted = true;
        else if (c == '{' || c == '[') {
            if (++depth > 8)
                return false;
        } else if (c == '}' || c == ']') {
            if (!depth)
                return false;
            --depth;
        }
    }
    return !quoted && !depth;
}

// ESP's buffer events can split a single WS frame. Accept bounded, unfragmented
// protocol messages and reassemble their payload offsets without heap growth.
class MessageBuffer {
public:
    enum class Result { More, Complete, Invalid };
    Result Append(unsigned opcode, bool fin, size_t total, size_t offset, const uint8_t* data,
                  size_t size) {
        if (!fin || (opcode != 1 && opcode != 2) || total == 0 || total > bytes_.size() || !data ||
            size > total || offset > total - size)
            return Result::Invalid;
        if (!offset) {
            used_ = 0;
            total_ = total;
            opcode_ = opcode;
        }
        if (used_ != offset || total_ != total || opcode_ != opcode)
            return Result::Invalid;
        std::memcpy(bytes_.data() + offset, data, size);
        used_ += size;
        return used_ == total_ ? Result::Complete : Result::More;
    }
    void Reset() { used_ = total_ = opcode_ = 0; }
    const uint8_t* data() const { return bytes_.data(); }
    size_t size() const { return used_; }
    unsigned opcode() const { return opcode_; }

private:
    std::array<uint8_t, kMaxMessage> bytes_{};
    size_t used_ = 0, total_ = 0;
    unsigned opcode_ = 0;
};
}  // namespace stubuddy::echo
