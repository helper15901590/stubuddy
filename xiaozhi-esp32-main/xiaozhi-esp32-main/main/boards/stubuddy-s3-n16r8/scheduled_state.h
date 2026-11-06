#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "countdown_state.h"

namespace stubuddy {
// Fixed Beijing offset, Monday is bit zero. No holiday/calendar service.
inline int64_t NextWeekly(unsigned minute, unsigned weekdays, int64_t after) {
    const int64_t day = (after + 28800) / 86400;
    for (int i = 0; i <= 7; ++i) {
        const auto candidate = (day + i) * 86400 - 28800 + minute * 60;
        if (candidate > after && (weekdays & (1U << ((day + i + 3) % 7))))
            return candidate;
    }
    return 0;
}
struct ReminderEntry {
    int64_t deadline = 0;
    uint32_t minute = 0, weekdays = 0;
    char id[17] = {}, item[61] = {}, text[121] = {};
    bool Valid(bool countdown = false) const {
        if (!deadline)
            return true;
        return CountdownState::TimeReady(deadline) && deadline < 4102444800LL && minute < 1440 &&
               weekdays <= 127 && id[16] == 0 && item[60] == 0 && text[120] == 0 &&
               std::strlen(id) == 16 && std::strspn(id, "0123456789abcdef") == 16 && item[0] &&
               text[0] &&
               (countdown
                    ? !weekdays
                    : deadline % 60 == 0 && unsigned((deadline + 28800) % 86400 / 60) == minute &&
                          (!weekdays ||
                           (weekdays & (1U << (((deadline + 28800) / 86400 + 3) % 7)))));
    }
    void Advance(int64_t now) {
        if (weekdays)
            deadline = NextWeekly(minute, weekdays, now);
        else
            *this = {};
    }
};
// Same weekly rules recur forever. Compare the complete rule, including the
// Sunday/Monday boundary, instead of accepting conflicting later occurrences.
inline bool ReminderConflict(const ReminderEntry& a, const ReminderEntry& b) {
    const auto close = [](int64_t x, int64_t y) { return x > y ? x - y < 300 : y - x < 300; };
    if (!a.weekdays && !b.weekdays)
        return close(a.deadline, b.deadline);
    if (!a.weekdays || !b.weekdays) {
        const auto& once = a.weekdays ? b : a;
        const auto& repeat = a.weekdays ? a : b;
        return close(once.deadline, NextWeekly(repeat.minute, repeat.weekdays,
                                               std::max(once.deadline - 300, repeat.deadline - 1)));
    }
    for (int i = 0; i < 7; ++i)
        for (int j = 0; j < 7; ++j)
            if ((a.weekdays & (1U << i)) && (b.weekdays & (1U << j))) {
                const int distance = std::abs(i * 1440 + int(a.minute) - j * 1440 - int(b.minute));
                if (std::min(distance, 10080 - distance) < 5)
                    return true;
            }
    return false;
}
struct ScheduledState {
    uint32_t version = 2;
    int32_t alarm_volume = 50;
    std::array<ReminderEntry, 4> entries{};  // countdown, then three schedules.
    bool Valid() const {
        if (version != 2 || alarm_volume < 0 || alarm_volume > 100)
            return false;
        for (unsigned i = 0; i < entries.size(); ++i) {
            if (!entries[i].Valid(i == 0))
                return false;
            for (unsigned j = 0; j < i; ++j)
                if (entries[i].deadline && entries[j].deadline &&
                    !std::strcmp(entries[i].id, entries[j].id))
                    return false;
        }
        return true;
    }
    // 0 success, 1 occupied/full, 2 spacing conflict, 3 invalid.
    unsigned Add(const ReminderEntry& entry, bool countdown, int64_t now) {
        if (!entry.Valid(countdown) || entry.deadline < now + 60 ||
            (countdown && entry.deadline > now + 86400) ||
            (entry.weekdays &&
             entry.deadline != NextWeekly(entry.minute, entry.weekdays, now + 59)))
            return 3;
        for (const auto& old : entries)
            if (old.deadline && !std::strcmp(old.id, entry.id))
                return 3;
        if (countdown) {
            if (entries[0].deadline)
                return 1;
            entries[0] = entry;
            return 0;
        }
        for (unsigned i = 1; i < entries.size(); ++i)
            if (entries[i].deadline && ReminderConflict(entries[i], entry))
                return 2;
        for (unsigned i = 1; i < entries.size(); ++i)
            if (!entries[i].deadline) {
                entries[i] = entry;
                return 0;
            }
        return 1;
    }
};
}  // namespace stubuddy
