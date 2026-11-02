#pragma once

namespace stubuddy {

// Input is the existing 30ms-debounced button. A timed-out recording's old held
// key cannot cancel its own automatic replay; require release then a fresh press.
class PlaybackInterrupt {
public:
    constexpr explicit PlaybackInterrupt(bool pressed_at_start) : released_(!pressed_at_start) {}

    constexpr bool Update(bool pressed) {
        if (!pressed)
            released_ = true;
        if (released_ && pressed && !fired_) {
            fired_ = true;
            return true;
        }
        return false;
    }

private:
    bool released_ = false;
    bool fired_ = false;
};
}  // namespace stubuddy
