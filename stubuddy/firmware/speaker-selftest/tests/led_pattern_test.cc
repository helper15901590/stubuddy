#include "../main/led_pattern.h"

constexpr bool LedPatternsPass() {
    using stubuddy::kLedMaxDuty;
    using stubuddy::LedBrightness;
    using stubuddy::LedState;
    using stubuddy::LocalWaitingLedState;

    // Idle stays dark; key holds, recording and playback stay lit throughout.
    for (unsigned ms = 0; ms <= 65000; ms += 20) {
        if (LedBrightness(LedState::Idle, ms) != 0 ||
            LedBrightness(LedState::KeyHeld, ms) != kLedMaxDuty ||
            LedBrightness(LedState::Recording, ms) != kLedMaxDuty ||
            LedBrightness(LedState::Playback, ms) != kLedMaxDuty) {
            return false;
        }
        // Preparation, mic warmup, and waiting for key release must never
        // select the model-only breathing state, however long they last.
        if (LocalWaitingLedState(false) != LedState::Idle ||
            LocalWaitingLedState(true) != LedState::KeyHeld ||
            LedBrightness(LocalWaitingLedState(false), ms) != 0 ||
            LedBrightness(LocalWaitingLedState(true), ms) != kLedMaxDuty) {
            return false;
        }
    }
    // Reserved model-only breath: no offline code path selects this state.
    for (unsigned ms = 1; ms <= 1000; ++ms) {
        const unsigned up = LedBrightness(LedState::ModelWaiting, ms);
        if (up < LedBrightness(LedState::ModelWaiting, ms - 1) || up > kLedMaxDuty ||
            up != LedBrightness(LedState::ModelWaiting, 2000 - ms)) {
            return false;
        }
    }
    if (LedBrightness(LedState::ModelWaiting, 0) != 0 ||
        LedBrightness(LedState::ModelWaiting, 1000) != kLedMaxDuty ||
        LedBrightness(LedState::ModelWaiting, 2000) != 0) {
        return false;
    }
    // Error: exactly three 100 ms flashes, then a 1.5 s dark interval.
    for (unsigned ms = 0; ms < 4000; ++ms) {
        const unsigned phase = ms % 2000;
        const bool expected =
            phase < 100 || (phase >= 200 && phase < 300) || (phase >= 400 && phase < 500);
        if (LedBrightness(LedState::Error, ms) != (expected ? kLedMaxDuty : 0)) {
            return false;
        }
    }
    return true;
}

static_assert(LedPatternsPass(), "LED pattern timing/brightness regression");
