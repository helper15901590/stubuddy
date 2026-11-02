#include "led_pattern.h"
#include "ptt_button.h"

#include <cassert>
#include <initializer_list>

using namespace stubuddy;

constexpr bool HeldPlaybackCompletionStaysDark() {
    PttButton button;
    button.RequireRelease(false, 0);
    button.Update(false, 30);
    if (PttLedState(PttLedPhase::ArmedIdle, button.pressed()) != LedState::Idle)
        return false;
    button.Update(true, 100);
    button.Update(true, 130);
    if (PttLedState(PttLedPhase::ArmedIdle, button.pressed()) != LedState::Recording)
        return false;
    if (PttLedState(PttLedPhase::Recording, true) != LedState::Recording ||
        PttLedState(PttLedPhase::PreparingReplay, true) != LedState::CaptureStopped ||
        PttLedState(PttLedPhase::Playback, true) != LedState::Playback)
        return false;
    // Timeout playback ends while still held: remain dark for any hold length.
    button.RequireRelease(true, 60130);
    for (unsigned now = 60130; now < 180130; now += 10) {
        button.Update(true, now);
        if (!button.pressed() ||
            PttLedState(PttLedPhase::ReleaseGate, button.pressed()) != LedState::Idle)
            return false;
    }
    // Release bounce cannot relight the gate or arm a spurious recording.
    button.Update(false, 180130);
    button.Update(true, 180140);
    button.Update(false, 180150);
    button.Update(false, 180179);
    if (!button.pressed() || PttLedState(PttLedPhase::ReleaseGate, true) != LedState::Idle)
        return false;
    button.Update(false, 180180);
    if (button.pressed() || PttLedState(PttLedPhase::ArmedIdle, false) != LedState::Idle)
        return false;
    button.Update(true, 181000);
    button.Update(true, 181030);
    return button.pressed() &&
           PttLedState(PttLedPhase::ArmedIdle, button.pressed()) == LedState::Recording;
}

constexpr bool PatternsAndPriorityPass() {
    for (bool held : {false, true}) {
        if (PttLedState(PttLedPhase::ReleaseGate, held) != LedState::Idle ||
            PttLedState(PttLedPhase::PreparingReplay, held) != LedState::CaptureStopped ||
            PttLedState(PttLedPhase::Playback, held) != LedState::Playback)
            return false;
    }
    for (unsigned ms = 0; ms < 4000; ++ms) {
        if (LedBrightness(LedState::Idle, ms) != 0 ||
            LedBrightness(LedState::CaptureStopped, ms) != 0 ||
            LedBrightness(LedState::Recording, ms) != kLedMaxDuty ||
            LedBrightness(LedState::Playback, ms) != kLedMaxDuty)
            return false;
    }
    constexpr unsigned error_edges[] = {0,   99,  100, 199, 200,  299, 300,
                                        399, 400, 499, 500, 1999, 2000};
    constexpr bool error_on[] = {true,  true, false, false, true,  true, false,
                                 false, true, true,  false, false, true};
    for (unsigned i = 0; i < sizeof(error_edges) / sizeof(error_edges[0]); ++i) {
        if (LedBrightness(LedState::Error, error_edges[i]) != (error_on[i] ? kLedMaxDuty : 0))
            return false;
    }
    for (LedState wait : {LedState::ModelWaiting, LedState::WifiSetup, LedState::WifiConnecting}) {
        if (LedBrightness(wait, 0) != 0 || LedBrightness(wait, 1000) != kLedMaxDuty ||
            LedBrightness(wait, 2000) != 0 || LedBrightness(wait, 500) == 0)
            return false;
        for (unsigned ms = 1; ms <= 1000; ++ms) {
            if (LedBrightness(wait, ms) < LedBrightness(wait, ms - 1) ||
                LedBrightness(wait, ms) != LedBrightness(wait, 2000 - ms))
                return false;
        }
    }
    for (LedState local : {LedState::Idle, LedState::CaptureStopped, LedState::Recording,
                           LedState::Playback, LedState::ModelWaiting, LedState::Error}) {
        if (EffectiveLedState(local, NetworkLedState::Fault) != LedState::Error ||
            EffectiveLedState(local, NetworkLedState::RetryError) != LedState::Error)
            return false;
    }
    for (NetworkLedState net :
         {NetworkLedState::None, NetworkLedState::Setup, NetworkLedState::Connecting}) {
        if (EffectiveLedState(LedState::Error, net) != LedState::Error ||
            EffectiveLedState(LedState::CaptureStopped, net) != LedState::CaptureStopped ||
            EffectiveLedState(LedState::Recording, net) != LedState::Recording ||
            EffectiveLedState(LedState::Playback, net) != LedState::Playback)
            return false;
    }
    return EffectiveLedState(LedState::Idle, NetworkLedState::Setup) == LedState::WifiSetup &&
           EffectiveLedState(LedState::Idle, NetworkLedState::Connecting) ==
               LedState::WifiConnecting;
}

static_assert(PttLedState(PttLedPhase::ReleaseGate, true) == LedState::Idle,
              "Held-key release gates must stay dark");
int main() {
    assert(HeldPlaybackCompletionStaysDark());
    assert(PatternsAndPriorityPass());
}
