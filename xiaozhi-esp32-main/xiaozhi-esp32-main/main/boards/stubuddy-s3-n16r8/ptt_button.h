#pragma once

namespace stubuddy {

// Ported from the hardware-validated diagnostic. Unsigned 32-bit milliseconds
// keep debounce valid when the timer wraps; playback always requires a release.
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
        if (candidate_ != pressed_ && now_ms - candidate_since_ >= kDebounceMs)
            pressed_ = candidate_;
    }

    constexpr bool pressed() const { return pressed_; }

private:
    bool pressed_ = true;
    bool candidate_ = true;
    unsigned candidate_since_ = 0;
};
}  // namespace stubuddy
