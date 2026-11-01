#include "../main/ptt_button.h"

constexpr int RunPttButtonTests() {
    stubuddy::PttButton button;
    // A button held at boot cannot arm recording until it is released.
    button.RequireRelease(true, 0);
    button.Update(true, 1000);
    if (!button.pressed())
        return 1;
    button.Update(false, 1000);
    button.Update(false, 1029);
    if (!button.pressed())
        return 2;
    button.Update(false, 1030);
    if (button.pressed())
        return 3;

    // Contact bounce must restart the full 30 ms interval in both directions.
    button.Update(true, 1100);
    button.Update(false, 1110);
    button.Update(true, 1120);
    button.Update(true, 1149);
    if (button.pressed())
        return 4;
    button.Update(true, 1150);
    if (!button.pressed())
        return 5;
    button.Update(false, 1200);
    button.Update(true, 1210);
    button.Update(false, 1220);
    button.Update(false, 1249);
    if (!button.pressed())
        return 6;
    button.Update(false, 1250);
    if (button.pressed())
        return 7;

    // A press held across playback completion requires a release, not a replay.
    button.RequireRelease(true, 2000);
    button.Update(true, 2100);
    if (!button.pressed())
        return 8;
    button.Update(false, 2200);
    button.Update(false, 2230);
    if (button.pressed())
        return 9;

    // Presses completed during playback must not create a new recording.
    button.RequireRelease(false, 3000);
    button.Update(false, 3029);
    if (!button.pressed())
        return 10;
    button.Update(false, 3030);
    if (button.pressed())
        return 11;

    // Debounce must still work when the 32-bit millisecond timer wraps.
    button.RequireRelease(false, 0xfffffff0U);
    button.Update(false, 13);
    if (!button.pressed())
        return 12;
    button.Update(false, 14);
    if (button.pressed())
        return 13;
    return 0;
}

// These sequences execute in the compiler against the same class used on-device.
static_assert(RunPttButtonTests() == 0, "PTT debounce/release-gate regression");
