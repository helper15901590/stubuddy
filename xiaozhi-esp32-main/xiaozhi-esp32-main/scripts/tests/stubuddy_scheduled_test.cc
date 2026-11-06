#include <nvs.h>
#include <cassert>
#include <map>
#include <string>
#include <vector>
#include "reminder_bank.h"
#include "reminder_cycle.h"
#include "scheduled_reminders.h"

using namespace stubuddy;
static std::map<std::string, std::vector<uint8_t>> flash;
static int64_t clock_now = 1793836800;  // 2026-11-05 08:00 Beijing, Thursday.
static bool write_failure = false;
esp_err_t nvs_open(const char*, int, nvs_handle_t* handle) {
    *handle = 1;
    return 0;
}
void nvs_close(nvs_handle_t) {}
esp_err_t nvs_commit(nvs_handle_t) { return write_failure ? 9 : 0; }
esp_err_t nvs_set_blob(nvs_handle_t, const char* key, const void* value, size_t size) {
    if (write_failure)
        return 9;
    const auto* data = static_cast<const uint8_t*>(value);
    flash[key] = {data, data + size};
    return 0;
}
esp_err_t nvs_get_blob(nvs_handle_t, const char* key, void* value, size_t* size) {
    if (!flash.count(key))
        return ESP_ERR_NVS_NOT_FOUND;
    const auto& data = flash.at(key);
    if (*size < data.size())
        return 9;
    std::memcpy(value, data.data(), data.size());
    *size = data.size();
    return 0;
}
static ReminderEntry Entry(int64_t deadline, const char* id, unsigned mask = 0) {
    ReminderEntry entry;
    entry.deadline = deadline;
    entry.minute = unsigned((deadline + 28800) % 86400 / 60);
    entry.weekdays = mask;
    std::strcpy(entry.id, id);
    std::strcpy(entry.item, "drink");
    std::strcpy(entry.text, "Drink water now.");
    return entry;
}
int main() {
    const char* a = "aaaaaaaaaaaaaaaa";
    const char* b = "bbbbbbbbbbbbbbbb";
    const char* c = "cccccccccccccccc";
    const char* d = "dddddddddddddddd";
    const char* e = "eeeeeeeeeeeeeeee";
    auto once = Entry(clock_now + 600, a);
    auto four = Entry(clock_now + 840, b);
    auto five = Entry(clock_now + 900, b);
    assert(ReminderConflict(once, four));
    assert(!ReminderConflict(once, five));
    ScheduledState state;
    assert(state.Valid() && state.Add(once, false, clock_now) == 0);
    assert(state.Add(four, false, clock_now) == 2);
    assert(state.Add(five, false, clock_now) == 0);
    assert(state.Add(Entry(clock_now + 1200, c), false, clock_now) == 0);
    assert(state.Add(Entry(clock_now + 1500, d), false, clock_now) == 1);
    assert(state.Add(Entry(clock_now + 61, d), true, clock_now) == 0);
    assert(state.Add(Entry(clock_now + 120, e), true, clock_now) == 1);
    assert(state.Valid());
    // Sunday 23:58 to Monday 00:02 is a real four-minute conflict.
    auto sun = Entry(NextWeekly(1438, 64, clock_now), a, 64);
    auto mon = Entry(NextWeekly(2, 1, clock_now), b, 1);
    assert(ReminderConflict(sun, mon));
    mon.minute = 3;
    mon.deadline = NextWeekly(3, 1, clock_now);
    assert(!ReminderConflict(sun, mon));
    auto thu = Entry(NextWeekly(600, 8, clock_now), a, 8);
    auto fri = Entry(NextWeekly(600, 16, clock_now), b, 16);
    assert(!ReminderConflict(thu, fri));
    auto daily = Entry(NextWeekly(600, 127, clock_now), c, 127);
    assert(ReminderConflict(thu, daily));
    // Once-vs-weekly checks future dates beyond the next weekly occurrence.
    auto future = Entry(thu.deadline + 21 * 86400 + 240, b);
    assert(ReminderConflict(thu, future));
    future.deadline += 60;
    assert(!ReminderConflict(thu, future));
    for (unsigned mask = 1; mask <= 127; ++mask) {
        const auto next = NextWeekly(30, mask, clock_now);
        assert(next > clock_now && next <= clock_now + 7 * 86400);
        assert(mask & (1U << (((next + 28800) / 86400 + 3) % 7)));
        assert(NextWeekly(30, mask, next) > next);
    }
    // Real persistence adapter: migrate v1, restart, commit, consume and reboot.
    CountdownState legacy;
    assert(legacy.Start(600, clock_now, a, "Old water reminder"));
    legacy.alarm_volume = 60;
    nvs_set_blob(1, "countdown_v1", &legacy, sizeof(legacy));
    ScheduledReminders reminders([] { return clock_now; });
    assert(reminders.Load() == ESP_OK && reminders.state().entries[0].deadline == clock_now + 600);
    VolumeRequest request;
    request.action = TimerAction::Get;
    auto receipt = reminders.Apply(request, false);
    assert(receipt.ok && receipt.records[0].deadline && receipt.alarm_volume == 60);
    request.action = TimerAction::Cancel;
    std::strcpy(request.cache_id.data(), a);
    assert(reminders.Apply(request, false).ok);
    ScheduledReminders reboot([] { return clock_now; });
    assert(reboot.Load() == ESP_OK && !reboot.state().entries[0].deadline);
    // Same-second countdown + recurring plan form a single claimed group.
    request.action = TimerAction::ScheduledStart;
    request.deadline = NextWeekly(600, 127, clock_now + 59);
    request.minute = 600;
    request.weekdays = 127;
    std::strcpy(request.cache_id.data(), b);
    std::strcpy(request.item.data(), "read");
    std::strcpy(request.text.data(), "Read now.");
    assert(!reboot.Apply(request, false).ok);
    assert(reboot.Apply(request, true).ok);
    const auto deadline = request.deadline;
    request.action = TimerAction::Start;
    request.seconds = unsigned(deadline - clock_now);
    request.minute = 0;
    request.weekdays = 0;
    std::strcpy(request.cache_id.data(), c);
    assert(reboot.Apply(request, true).ok);
    clock_now = deadline;
    assert(reboot.Due());
    std::array<ReminderEntry, 4> group;
    unsigned count = 0;
    assert(reboot.Claim(group, count) == ESP_OK && count == 2);
    assert(!reboot.state().entries[0].deadline &&
           reboot.state().entries[1].deadline == deadline + 86400);
    ScheduledReminders after_power_cut([] { return clock_now; });
    assert(after_power_cut.Load() == ESP_OK && !after_power_cut.Due());
    // Miss several days: advance directly to the next valid date, no catch-up.
    clock_now += 4 * 86400 + 1;
    assert(after_power_cut.Due());
    assert(after_power_cut.Claim(group, count) == ESP_OK && count == 1);
    assert(after_power_cut.state().entries[1].deadline > clock_now);
    write_failure = true;
    request.action = TimerAction::Volume;
    request.volume = 20;
    assert(!after_power_cut.Apply(request, true).ok && after_power_cut.state().alarm_volume == 60);
    write_failure = false;
    // Four active clips must survive arbitrary staging/reload churn.
    std::vector<uint8_t> storage(ReminderBank::kCapacity);
    ReminderBank bank(storage.data());
    const char* ids[] = {a, b, c, d};
    std::array<ReminderEntry, 4> pinned;
    const uint8_t packet[] = {1, 2, 3};
    auto fill = [&](const char* id) {
        assert(bank.Begin(id));
        unsigned crc = 0;
        for (unsigned i = 0; i < 6; ++i) {
            assert(bank.Push(id, i, packet, 3));
            crc = echo::Crc32(crc, packet, 3);
        }
        assert(bank.Finish(id, 4800, 104, 6, crc));
    };
    for (unsigned i = 0; i < 4; ++i)
        pinned[i] = Entry(clock_now + 600, ids[i]);
    bank.Keep(pinned);
    for (auto* id : ids)
        fill(id);
    fill(e);
    fill("ffffffffffffffff");
    for (auto* id : ids)
        assert(bank.Ready(id));
    assert(ReminderCycle::kMaximumMs == 180000);
    ReminderCycle cycle(100);
    assert(!cycle.Expired(180099) && cycle.Expired(180100));
}
