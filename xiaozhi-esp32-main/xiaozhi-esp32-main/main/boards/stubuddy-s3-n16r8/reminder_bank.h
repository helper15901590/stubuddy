#pragma once
#include <cstdio>
#include <memory>
#include "reminder_cache.h"
#include "scheduled_state.h"
namespace stubuddy {
// Four pinned active clips plus one staging clip. Creating a reminder must
// never overwrite any already accepted audio. Storage is bounded in PSRAM.
class ReminderBank {
public:
    static constexpr size_t kCapacity = ReminderCache::kCapacity * 5;
    explicit ReminderBank(uint8_t* storage) {
        for (unsigned i = 0; i < 5; ++i)
            caches_[i] = std::make_unique<ReminderCache>(storage + i * ReminderCache::kCapacity);
    }
    void Keep(const std::array<ReminderEntry, 4>& entries) {
        for (unsigned i = 0; i < 4; ++i)
            std::strcpy(pins_[i].data(), entries[i].deadline ? entries[i].id : "");
    }
    bool Begin(const char* id) {
        auto* slot = Find(id);
        if (!slot) {
            for (unsigned n = 0; n < 5; ++n) {
                const auto i = (next_ + n) % 5;
                bool pinned = false;
                for (const auto& pin : pins_)
                    pinned |= pin[0] && !std::strcmp(pin.data(), ids_[i].data());
                if (pinned)
                    continue;
                slot = caches_[i].get();
                std::snprintf(ids_[i].data(), 17, "%s", id);
                next_ = (i + 1) % 5;
                break;
            }
        }
        return slot && slot->Begin(id);
    }
    bool Push(const char* id, unsigned index, const uint8_t* data, size_t size) {
        auto* slot = Find(id);
        return slot && slot->Push(id, index, data, size);
    }
    bool Finish(const char* id, unsigned samples, unsigned delay, unsigned frames, unsigned crc) {
        auto* slot = Find(id);
        return slot && slot->Finish(id, samples, delay, frames, crc);
    }
    bool Ready(const char* id) const {
        const auto* slot = Find(id);
        return slot && slot->Ready(id);
    }
    bool Read(const char* id, size_t& cursor, uint8_t* data, size_t& size,
              echo::Metadata& metadata) {
        auto* slot = Find(id);
        return slot && slot->Read(id, cursor, data, size, metadata);
    }

private:
    ReminderCache* Find(const char* id) const {
        for (unsigned i = 0; i < 5; ++i)
            if (ids_[i][0] && !std::strcmp(id, ids_[i].data()))
                return caches_[i].get();
        return nullptr;
    }
    std::array<std::unique_ptr<ReminderCache>, 5> caches_;
    std::array<std::array<char, 17>, 5> ids_{};
    std::array<std::array<char, 17>, 4> pins_{};
    unsigned next_ = 0;
};
}  // namespace stubuddy
