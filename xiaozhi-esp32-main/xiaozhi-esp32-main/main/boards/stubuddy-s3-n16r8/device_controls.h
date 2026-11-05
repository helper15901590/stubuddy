#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace stubuddy {
enum class TimerAction { None, Get, Start, Cancel, Volume };
struct VolumeRequest {
    unsigned turn = 0;
    std::array<char, 17> id{};
    bool set = false;
    bool restore = false;
    int volume = -1;
#ifdef CONFIG_STUBUDDY_COUNTDOWN
    TimerAction action = TimerAction::None;
    uint32_t seconds = 0;
    std::array<char, 17> cache_id{};
    std::array<char, 121> text{};
#endif
    bool operator==(const VolumeRequest&) const = default;
};
struct VolumeResult {
    VolumeRequest request;
    bool ok = false;
    int volume = 100;
#ifdef CONFIG_STUBUDDY_COUNTDOWN
    bool active = false;
    unsigned remaining = 0;
    int alarm_volume = 50;
#endif
};

// Called under the transport mutex. At most four serial calls per turn;
// duplicate calls reuse their result, never reapply a persistent write.
class VolumeMailbox {
public:
    enum class Arrival { New, Waiting, Cached, Invalid };
    Arrival Receive(const VolumeRequest& request, VolumeResult& cached) {
        for (unsigned i = 0; i < count_; ++i) {
            if (entries_[i].result.request.id == request.id) {
                if (!(entries_[i].result.request == request))
                    return Arrival::Invalid;
                cached = entries_[i].result;
                return entries_[i].complete ? Arrival::Cached : Arrival::Waiting;
            }
        }
        if (count_ == entries_.size() || (count_ && !entries_[count_ - 1].complete))
            return Arrival::Invalid;
        entries_[count_] = {};
        entries_[count_++].result.request = request;
        return Arrival::New;
    }
    bool Take(VolumeRequest& request) {
        if (!count_)
            return false;
        auto& entry = entries_[count_ - 1];
        if (entry.taken || entry.complete)
            return false;
        entry.taken = true;
        request = entry.result.request;
        return true;
    }
    bool Complete(const VolumeResult& result) {
        if (!count_)
            return false;
        auto& entry = entries_[count_ - 1];
        if (!entry.taken || entry.complete || !(entry.result.request == result.request))
            return false;
        entry.result = result;
        entry.complete = true;
        return true;
    }
    void Reset() { count_ = 0; }

private:
    struct Entry {
        VolumeResult result;
        bool taken = false, complete = false;
    };
    std::array<Entry, 4> entries_{};
    unsigned count_ = 0;
};
}  // namespace stubuddy
