#pragma once
#include <array>
#include "echo_protocol.h"

namespace stubuddy {
// Independent of the conversation packet store. A new conversation cannot
// erase this prepared clip. All access is under WsEcho's RX mutex.
class ReminderCache {
public:
    static constexpr size_t kCapacity = 47000;
    explicit ReminderCache(uint8_t* storage) : packets_(storage, kCapacity, 227) {}
    bool Begin(const char* id) {
        if (!id || std::strlen(id) != 16 || std::strspn(id, "0123456789abcdef") != 16)
            return false;
        // Reconnect reloads must not mutate an already verified playing clip.
        ignore_ = ready_ && std::strcmp(id, id_.data()) == 0;
        if (ignore_)
            return true;
        packets_.Erase();
        std::strcpy(id_.data(), id);
        ready_ = false;
        crc_ = 0;
        return true;
    }
    bool Push(const char* id, unsigned index, const uint8_t* data, size_t size) {
        if (std::strcmp(id, id_.data()))
            return true;  // Stale reload from an earlier countdown.
        if (ignore_)
            return true;
        if (ready_ || index != packets_.count() || index >= 201 || !packets_.Push(data, size))
            return false;
        crc_ = echo::Crc32(crc_, data, size);
        return true;
    }
    bool Finish(const char* id, unsigned samples, unsigned delay, unsigned frames, unsigned crc) {
        if (std::strcmp(id, id_.data()) || ignore_)
            return true;
        if (samples < 4800 || samples > 192000 || delay >= 960 || frames != packets_.count() ||
            frames != OpusPacketsNeeded(samples, delay) || crc != crc_)
            return false;
        metadata_ = {1, samples, delay, frames, unsigned(packets_.payload_bytes()), crc};
        ready_ = true;
        return true;
    }
    bool Ready(const char* id) const { return ready_ && !std::strcmp(id, id_.data()); }
    bool Read(const char* id, size_t& cursor, uint8_t* data, size_t& size,
              echo::Metadata& metadata) {
        OpusPacketView packet;
        if (!Ready(id) || !packets_.Next(cursor, packet))
            return false;
        std::memcpy(data, packet.data, packet.size);
        size = packet.size;
        metadata = metadata_;
        return true;
    }

private:
    OpusPacketStore packets_;
    std::array<char, 17> id_{};
    echo::Metadata metadata_;
    unsigned crc_ = 0;
    bool ready_ = false, ignore_ = false;
};
}  // namespace stubuddy
