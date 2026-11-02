#pragma once

namespace stubuddy {

enum class LedState {
    Idle,
    CaptureStopped,
    Recording,
    Playback,
    ModelWaiting,
    WifiSetup,
    WifiConnecting,
    Error
};
enum class NetworkLedState { None, Setup, Connecting, RetryError, Fault };
enum class PttLedPhase { ReleaseGate, ArmedIdle, Recording, PreparingReplay, Playback };

// A held key only lights an armed idle session. Warmup/release gates and replay
// preparation stay dark, even when the old press continues across playback.
constexpr LedState PttLedState(PttLedPhase phase, bool key_pressed = false) {
    switch (phase) {
        case PttLedPhase::Recording:
            return LedState::Recording;
        case PttLedPhase::Playback:
            return LedState::Playback;
        case PttLedPhase::ArmedIdle:
            return key_pressed ? LedState::Recording : LedState::Idle;
        case PttLedPhase::ReleaseGate:
            return LedState::Idle;
        case PttLedPhase::PreparingReplay:
            return LedState::CaptureStopped;
    }
    return LedState::Idle;
}

constexpr LedState EffectiveLedState(LedState audio, NetworkLedState network) {
    if (audio == LedState::Error || network == NetworkLedState::RetryError ||
        network == NetworkLedState::Fault)
        return LedState::Error;
    if (audio == LedState::Recording || audio == LedState::Playback ||
        audio == LedState::CaptureStopped)
        return audio;
    if (network == NetworkLedState::Setup)
        return LedState::WifiSetup;
    if (network == NetworkLedState::Connecting)
        return LedState::WifiConnecting;
    return audio;
}

inline constexpr unsigned kLedMaxDuty = 1023;

// Logical brightness. The LEDC output inversion owns GPIO18's active-low wiring.
constexpr unsigned LedBrightness(LedState state, unsigned elapsed_ms) {
    switch (state) {
        case LedState::Idle:
        case LedState::CaptureStopped:
            return 0;
        case LedState::Recording:
        case LedState::Playback:
            return kLedMaxDuty;
        case LedState::ModelWaiting:
        case LedState::WifiSetup:
        case LedState::WifiConnecting: {
            const unsigned phase = elapsed_ms % 2000;
            const unsigned ramp = phase <= 1000 ? phase : 2000 - phase;
            return ramp * ramp * kLedMaxDuty / 1000000;
        }
        case LedState::Error: {
            const unsigned phase = elapsed_ms % 2000;
            return phase < 500 && (phase / 100) % 2 == 0 ? kLedMaxDuty : 0;
        }
    }
    return 0;
}

constexpr const char* LedStateName(LedState state) {
    switch (state) {
        case LedState::Idle:
            return "IDLE/OFF";
        case LedState::CaptureStopped:
            return "CAPTURE_STOPPED/OFF";
        case LedState::Recording:
            return "RECORDING/ON";
        case LedState::Playback:
            return "PLAYBACK/ON";
        case LedState::ModelWaiting:
            return "MODEL_WAITING/BREATH";
        case LedState::WifiSetup:
            return "WIFI_SETUP/BREATH";
        case LedState::WifiConnecting:
            return "WIFI_CONNECTING/BREATH";
        case LedState::Error:
            return "ERROR/TRIPLE-FLASH";
    }
    return "UNKNOWN";
}
}  // namespace stubuddy
