#pragma once

#include <atomic>

namespace stubuddy {
enum class WifiCue : unsigned { None, Setup, Connected, Failed };

struct WifiCuePattern {
    unsigned count;
    unsigned frequencies[3];
};

constexpr WifiCuePattern CuePattern(WifiCue cue) {
    switch (cue) {
        case WifiCue::Setup:
            return {2, {440, 440, 0}};
        case WifiCue::Connected:
            return {2, {660, 880, 0}};
        case WifiCue::Failed:
            return {3, {880, 660, 440}};
        case WifiCue::None:
            return {0, {0, 0, 0}};
    }
    return {0, {0, 0, 0}};
}

// Coalesce routine status updates, but retain a failure until the audio owner
// consumes it. A rapid successful retry cannot overwrite the error feedback.
class WifiCueMailbox {
public:
    void Notify(WifiCue cue) {
        if (cue == WifiCue::Failed) {
            latest_.store(WifiCue::None);
            failed_.store(true);
        } else if (cue != WifiCue::None)
            latest_.store(cue);
    }
    WifiCue Take() {
        if (failed_.exchange(false))
            return WifiCue::Failed;
        return latest_.exchange(WifiCue::None);
    }
    bool pending() const { return failed_.load() || latest_.load() != WifiCue::None; }

private:
    std::atomic<bool> failed_{false};
    std::atomic<WifiCue> latest_{WifiCue::None};
};
}  // namespace stubuddy
