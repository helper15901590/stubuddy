#pragma once

#include <cstdint>
#include <cstring>

namespace stubuddy {
enum class CountdownDecision { Wait, Skip, Play };
// Small versioned NVS record. Audio stays in bounded PSRAM and is reloaded
// after reboot; no partition changes or writes on each countdown tick.
struct CountdownState {
    uint32_t version = 1;
    int32_t alarm_volume = 50;
    int64_t deadline = 0;
    char id[17] = {};
    char text[121] = {};
    bool Valid() const {
        return version == 1 && alarm_volume >= 0 && alarm_volume <= 100 && deadline >= 0 &&
               id[16] == 0 && text[120] == 0 &&
               (!deadline ||
                (std::strlen(id) == 16 && std::strspn(id, "0123456789abcdef") == 16 && text[0]));
    }
    static bool TimeReady(int64_t now) { return now >= 1767225600; }  // 2026-01-01 UTC.
    bool Due(int64_t now) const { return deadline && TimeReady(now) && now >= deadline; }
    CountdownDecision Decision(int64_t now, bool online, bool cached, bool reboot_pending) const {
        if (!Due(now))
            return CountdownDecision::Wait;
        return reboot_pending || !online || !cached ? CountdownDecision::Skip
                                                    : CountdownDecision::Play;
    }
    unsigned Remaining(int64_t now) const {
        if (!deadline || !TimeReady(now) || now >= deadline)
            return 0;
        return unsigned(deadline - now);
    }
    bool Start(unsigned seconds, int64_t now, const char* identity, const char* speech) {
        if (deadline || seconds < 60 || seconds > 86400 || !TimeReady(now) || !identity ||
            std::strlen(identity) != 16 || std::strspn(identity, "0123456789abcdef") != 16 ||
            !speech || !speech[0] || std::strlen(speech) > 120)
            return false;
        deadline = now + seconds;
        std::strcpy(id, identity);
        std::strcpy(text, speech);
        return true;
    }
    void Clear() {
        deadline = 0;
        std::memset(id, 0, sizeof(id));
        std::memset(text, 0, sizeof(text));
    }
};
}  // namespace stubuddy
