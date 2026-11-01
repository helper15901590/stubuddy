#pragma once

namespace stubuddy {

// Keep this logic independent of ESP-IDF so bounce and release gating can be
// exercised on the host. All times are unsigned 32-bit milliseconds.
class PttButton {
public:
    static_assert(sizeof(unsigned) == 4);
    static constexpr unsigned kDebounceMs = 30;

    constexpr void RequireRelease(bool raw_pressed, unsigned now_ms) {
        pressed_ = true;
        candidate_ = raw_pressed;
        candidate_since_ = now_ms;
    }

    constexpr void Update(bool raw_pressed, unsigned now_ms) {
        if (raw_pressed != candidate_) {
            candidate_ = raw_pressed;
            candidate_since_ = now_ms;
        }
        // Unsigned subtraction also handles millisecond counter wraparound.
        if (candidate_ != pressed_ && now_ms - candidate_since_ >= kDebounceMs) {
            pressed_ = candidate_;
        }
    }

    constexpr bool pressed() const { return pressed_; }

private:
    bool pressed_ = true;
    bool candidate_ = true;
    unsigned candidate_since_ = 0;
};

}  // namespace stubuddy
