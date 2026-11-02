#include "portal_sequence.h"
#include "ptt_button.h"
#include "stubuddy_captive_dns_test.cc"
#include "stubuddy_wifi_history_test.cc"
#include "stubuddy_wifi_rules_test.cc"
#include "wifi_audio_rules.h"
#include "wifi_cue.h"

#include <cassert>
#include <cstddef>

// This blob is the existing diagnostic's persistent NVS API, not a new layout.
static_assert(sizeof(stubuddy::WifiCredentials) == 97);
static_assert(sizeof(stubuddy::WifiHistory) == 496);
static_assert(offsetof(stubuddy::WifiHistory, items) == 8);

int main() {
    static_assert(stubuddy::kPortalCloseDelayMs == 3000);
    int calls[3] = {};
    unsigned count = 0;
    auto http = [&]() {
        calls[count++] = 1;
        return 0;
    };
    auto dns = [&]() {
        calls[count++] = 2;
        return 0;
    };
    auto dhcp = [&]() {
        calls[count++] = 3;
        return 0;
    };
    assert(stubuddy::StartPortalServices(http, dns, dhcp) == 0);
    assert(count == 3 && calls[0] == 1 && calls[1] == 2 && calls[2] == 3);
    count = 0;
    assert(stubuddy::StartPortalServices(
               [&]() {
                   ++count;
                   return 7;
               },
               dns, dhcp) == 7);
    assert(count == 1);  // no DHCP lease when HTTP is unavailable
    count = 0;
    assert(stubuddy::StartPortalServices(
               http,
               [&]() {
                   ++count;
                   return 8;
               },
               dhcp) == 8);
    assert(count == 2);  // no DHCP lease when DNS is unavailable

    stubuddy::WifiCueMailbox cues;
    cues.Notify(stubuddy::WifiCue::Setup);
    cues.Notify(stubuddy::WifiCue::Failed);
    cues.Notify(stubuddy::WifiCue::Connected);  // a rapid retry must not erase the failure
    assert(cues.pending());
    assert(cues.Take() == stubuddy::WifiCue::Failed);
    assert(cues.Take() == stubuddy::WifiCue::Connected);
    assert(cues.Take() == stubuddy::WifiCue::None && !cues.pending());
    cues.Notify(stubuddy::WifiCue::Failed);
    cues.Notify(stubuddy::WifiCue::Failed);
    assert(cues.Take() == stubuddy::WifiCue::Failed && !cues.pending());
    const auto failure = stubuddy::CuePattern(stubuddy::WifiCue::Failed);
    assert(failure.count == 3 && failure.frequencies[0] == 880 && failure.frequencies[1] == 660 &&
           failure.frequencies[2] == 440);

    using stubuddy::NetworkRecordingAllowed;
    assert(!NetworkRecordingAllowed(false, 7, 7, false));
    assert(!NetworkRecordingAllowed(true, 7, 8, false));  // fast drop/reconnect
    assert(!NetworkRecordingAllowed(true, 8, 8, true));   // cue must own idle audio first
    assert(NetworkRecordingAllowed(true, 8, 8, false));

    // Reconnection cannot turn a held setup/old recording key into a new press.
    stubuddy::PttButton key;
    key.RequireRelease(true, 1000);
    key.Update(true, 60000);
    assert(key.pressed());
    key.Update(false, 60010);
    key.Update(false, 60039);
    assert(key.pressed());
    key.Update(false, 60040);
    assert(!key.pressed());
    key.Update(true, 60100);
    key.Update(true, 60130);
    assert(key.pressed());

    // Forced setup is sampled before slow startup; a later release cannot undo it.
    const bool boot_setup = stubuddy::BootSetupRequested(true, true);
    stubuddy::WifiConnectionPolicy policy;
    policy.Start(true, boot_setup, true);
    for (int scan_or_refresh = 0; scan_or_refresh < 100; ++scan_or_refresh)
        assert(!policy.AutomaticReconnectAllowed());
    policy.SetupCompleted();
    assert(policy.AutomaticReconnectAllowed());
    policy.EnterManualSetup();
    assert(!policy.AutomaticReconnectAllowed());
}
