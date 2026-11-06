#pragma once
#include "clock_age.h"

namespace stubuddy {
// Monotonic bounds shared by actual playback, DMA drain and the silent gap.
// A long clip is cut at the overall deadline; a short clip repeats after 3s.
class ReminderCycle {
public:
    static constexpr unsigned kMaximumMs = 180000;
    static constexpr unsigned kGapMs = 3000;
    explicit ReminderCycle(unsigned started) : started_(started) {}
    void Begin(unsigned now) { started_ = now; }
    bool Poll(unsigned now, bool stop, bool enforce_limit = true) {
        stopped_ = stopped_ || stop;
        return !stopped_ && !(enforce_limit && Expired(now));
    }
    bool stopped() const { return stopped_; }
    bool Expired(unsigned now) const { return ElapsedMs(now, started_) >= kMaximumMs; }
    bool NextReady(unsigned now, unsigned finished) const {
        return !Expired(now) && ElapsedMs(now, finished) >= kGapMs;
    }

private:
    unsigned started_;
    bool stopped_ = false;
};
}  // namespace stubuddy
