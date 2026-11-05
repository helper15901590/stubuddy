#pragma once
#include <atomic>

namespace stubuddy {
// Durations here are far below half the 32-bit millisecond wrap interval.
constexpr unsigned ElapsedMs(unsigned now, unsigned then) {
    const unsigned elapsed = now - then;
    return elapsed <= 0x7fffffffU ? elapsed : 0;
}

template <typename Clock>
unsigned SnapshotAgeMs(const std::atomic<unsigned>& stamp, Clock clock) {
    // Snapshot the asynchronously updated stamp FIRST, then read the clock.
    const unsigned then = stamp.load();
    return ElapsedMs(clock(), then);
}
}  // namespace stubuddy
