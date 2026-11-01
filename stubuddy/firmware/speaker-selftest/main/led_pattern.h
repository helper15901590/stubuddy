#pragma once

namespace stubuddy {

enum class LedState {
    Idle,
    KeyHeld,
    Recording,
    Playback,
    ModelWaiting,
    WifiSetup,
    WifiConnecting,
    Error
};
enum class NetworkLedState { None, Setup, Connecting, RetryError, Fault };

constexpr LedState EffectiveLedState(LedState audio, NetworkLedState network) {
    if (audio == LedState::Error || network == NetworkLedState::Fault ||
        network == NetworkLedState::RetryError)
        return LedState::Error;
    if (audio == LedState::Recording || audio == LedState::Playback || audio == LedState::KeyHeld)
        return audio;
    if (network == NetworkLedState::Setup)
        return LedState::WifiSetup;
    if (network == NetworkLedState::Connecting)
        return LedState::WifiConnecting;
    return audio;
}

inline constexpr unsigned kLedMaxDuty = 1023;

// Logical brightness, independent of LED wiring polarity or the hardware timer.
constexpr unsigned LedBrightness(LedState state, unsigned elapsed_ms) {
    switch (state) {
        case LedState::Idle:
            return 0;
        case LedState::KeyHeld:
        case LedState::Recording:
        case LedState::Playback:
            return kLedMaxDuty;
        case LedState::WifiSetup:
        case LedState::WifiConnecting:
        case LedState::ModelWaiting: {
            // Two-second breath, with a quadratic ramp for a gentle low end.
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

// Local preparation and release gates follow the key level, not a model wait.
// The recording-limit transition explicitly overrides this with Idle before
// preparing automatic replay, even if the timed-out press remains held.
constexpr LedState LocalWaitingLedState(bool key_pressed) {
    return key_pressed ? LedState::KeyHeld : LedState::Idle;
}

constexpr const char* LedStateName(LedState state) {
    switch (state) {
        case LedState::Idle:
            return "IDLE/OFF";
        case LedState::KeyHeld:
            return "KEY_HELD/ON";
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
