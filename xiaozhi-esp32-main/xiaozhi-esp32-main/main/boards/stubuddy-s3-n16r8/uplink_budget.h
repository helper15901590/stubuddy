#pragma once

#include <atomic>

namespace stubuddy {
// Fixed PSRAM queue, with audio credits leaving room for stop/abort/tool replies.
// Credits are returned on dequeue, including stale turns and connection epochs.
class UplinkBudget {
public:
    static constexpr unsigned kAudioSlots = 56;
    static constexpr unsigned kControlSlots = 8;
    static constexpr unsigned kDepth = kAudioSlots + kControlSlots;
    static constexpr unsigned kSendTimeoutMs = 2000;
    static_assert(kAudioSlots * 60 >= kSendTimeoutMs + 1000);

    bool AcquireAudio() {
        const auto previous = pending_.fetch_add(1);
        if (previous < kAudioSlots)
            return true;
        pending_.fetch_sub(1);
        return false;
    }
    void ReleaseAudio() { pending_.fetch_sub(1); }

private:
    std::atomic<unsigned> pending_{0};
};
}  // namespace stubuddy
