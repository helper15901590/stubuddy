#define CONFIG_STUBUDDY_COUNTDOWN 1
#include <cassert>
#include <vector>
#include "countdown_state.h"
#include "device_controls.h"
#include "ptt_playback.h"
#include "reminder_cache.h"
#include "reminder_cycle.h"

int main() {
    using namespace stubuddy;
    constexpr int64_t now = 1793836800;
    const char* id = "0123456789abcdef";
    CountdownState state;
    assert(state.Valid() && state.alarm_volume == 50);
    for (unsigned seconds : {0, 1, 59, 86401})
        assert(!state.Start(seconds, now, id, "time"));
    assert(!state.Start(60, 0, id, "time"));
    assert(!state.Start(60, now, "invalid", "time"));
    assert(state.Start(60, now, id, "Drink some water"));
    assert(state.Valid() && state.Remaining(now) == 60);
    assert(!state.Start(60, now, id, "second timer"));
    assert(!state.Due(now + 59) && state.Due(now + 60));
    assert(state.Decision(now + 59, true, true, false) == CountdownDecision::Wait);
    assert(state.Decision(now + 60, true, true, false) == CountdownDecision::Play);
    assert(state.Decision(now + 60, false, true, false) == CountdownDecision::Skip);
    assert(state.Decision(now + 60, true, false, false) == CountdownDecision::Skip);
    assert(state.Decision(now + 60, true, true, true) == CountdownDecision::Skip);
    assert(!state.Due(0) && state.Remaining(now + 61) == 0);
    state.alarm_volume = 0;
    state.Clear();
    assert(!state.deadline && !state.Due(now + 100) && state.alarm_volume == 0);
    assert(state.Start(86400, now, id, "24 hours"));
    assert(state.Remaining(now) == 86400 && state.Due(now + 86400));
    // NVS byte round trip retains the deadline and independent alarm mute.
    CountdownState reboot;
    std::memcpy(&reboot, &state, sizeof(state));
    assert(reboot.Valid() && reboot.Remaining(now + 3600) == 82800);
    reboot.version = 2;
    assert(!reboot.Valid());
    std::vector<uint8_t> storage(ReminderCache::kCapacity);
    ReminderCache cache(storage.data());
    const uint8_t packet[] = {1, 2, 3};
    assert(cache.Begin(id) && !cache.Ready(id));
    assert(!cache.Push(id, 1, packet, 3));
    unsigned crc = 0;
    for (unsigned i = 0; i < 6; ++i) {
        assert(cache.Push(id, i, packet, 3));
        crc = echo::Crc32(crc, packet, 3);
    }
    assert(!cache.Finish(id, 4800, 104, 6, crc + 1));
    assert(cache.Finish(id, 4800, 104, 6, crc));
    assert(cache.Ready(id));
    assert(cache.Begin(id));  // Reload of a completed clip cannot change it.
    assert(cache.Push(id, 0, packet, 3));
    assert(cache.Finish(id, 0, 0, 0, 0) && cache.Ready(id));
    size_t cursor = 0, size = 0;
    uint8_t out[227];
    echo::Metadata metadata;
    assert(cache.Read(id, cursor, out, size, metadata));
    assert(size == 3 && metadata.samples == 4800 && !std::memcmp(out, packet, 3));
    assert(!cache.Read("ffffffffffffffff", cursor, out, size, metadata));
    VolumeMailbox mailbox;
    VolumeRequest request;
    request.turn = 1;
    request.action = TimerAction::Start;
    request.seconds = 60;
    request.id[0] = 'a';
    VolumeResult result;
    assert(mailbox.Receive(request, result) == VolumeMailbox::Arrival::New);
    VolumeRequest taken;
    assert(mailbox.Take(taken));
    assert(mailbox.Complete({taken, true}));
    assert(mailbox.Receive(request, result) == VolumeMailbox::Arrival::Cached);
    request.seconds = 120;
    assert(mailbox.Receive(request, result) == VolumeMailbox::Arrival::Invalid);
    // A key already held when a reminder preempts capture isn't a stop press.
    PlaybackInterrupt interrupt(true);
    assert(!interrupt.Update(true));
    assert(!interrupt.Update(false));
    assert(interrupt.Update(true));
    ReminderCycle cycle(1000);
    assert(!cycle.Expired(180999) && cycle.Expired(181000));
    assert(!cycle.NextReady(6999, 4000));
    assert(cycle.NextReady(7000, 4000));
    assert(!cycle.NextReady(181000, 177000));
    // Waiting does not turn a 3s pause near the deadline into a 61s reminder.
    assert(!cycle.NextReady(180999, 179000) && !cycle.NextReady(182000, 179000));
    ReminderCycle wrapped(0xfffff000U);
    assert(!wrapped.Expired(0xfffff000U + 179999U));
    assert(wrapped.Expired(0xfffff000U + 180000U));
    assert(wrapped.NextReady(0x500U, 0x500U - 3000U));
    ReminderCycle stopped(0);
    assert(stopped.Poll(1, false));
    assert(!stopped.Poll(2, true));
    assert(!stopped.Poll(3, false) &&
           stopped.stopped());  // Polling twice cannot consume a stop edge.
    ReminderCycle decoding(0);
    assert(decoding.Poll(90000, false, false));
    decoding.Begin(90000);
    assert(decoding.Poll(90001, false));
    assert(!decoding.Poll(270000, false));
}
