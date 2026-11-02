#include "ptt_button.h"
#include "recording_pcm.h"

#include <cassert>
#include <vector>

constexpr bool CheckButton() {
    stubuddy::PttButton button;
    button.RequireRelease(true, 0);
    button.Update(true, 1000);
    if (!button.pressed())
        return false;
    button.Update(false, 1000);
    button.Update(false, 1029);
    if (!button.pressed())
        return false;
    button.Update(false, 1030);
    if (button.pressed())
        return false;
    // Bounce in both directions must restart the 30-ms interval.
    button.Update(true, 1100);
    button.Update(false, 1110);
    button.Update(true, 1120);
    button.Update(true, 1149);
    if (button.pressed())
        return false;
    button.Update(true, 1150);
    if (!button.pressed())
        return false;
    button.Update(false, 1200);
    button.Update(true, 1210);
    button.Update(false, 1220);
    button.Update(false, 1249);
    if (!button.pressed())
        return false;
    button.Update(false, 1250);
    if (button.pressed())
        return false;
    // A key still held after timeout/playback cannot start another capture.
    button.RequireRelease(true, 2000);
    button.Update(true, 99999);
    if (!button.pressed())
        return false;
    button.Update(false, 100000);
    button.Update(false, 100030);
    if (button.pressed())
        return false;
    // A press already completed during playback also cannot queue a capture.
    button.RequireRelease(false, 101000);
    button.Update(false, 101029);
    if (!button.pressed())
        return false;
    button.Update(false, 101030);
    if (button.pressed())
        return false;
    button.RequireRelease(false, 0xfffffff0U);
    button.Update(false, 13);
    if (!button.pressed())
        return false;
    button.Update(false, 14);
    return !button.pressed();
}
static_assert(CheckButton(), "debounce/release-gate regression");

int main() {
    const auto empty = stubuddy::AnalyzeRecording(nullptr, 0);
    assert(empty.gain == 0);
    // Sixty seconds of constant DC must play as silence without accumulator overflow.
    std::vector<int16_t> dc(960000, INT16_MAX);
    const auto silent = stubuddy::AnalyzeRecording(dc.data(), dc.size());
    assert(silent.mean == INT16_MAX && silent.peak == 0 && silent.gain == 0);
    assert(silent.clipped == dc.size() && silent.rms == 0);
    assert(stubuddy::ReplaySample(INT16_MAX, 500, dc.size(), silent, 160) == 0);

    std::vector<int16_t> voice(1000);
    for (size_t i = 0; i < voice.size(); ++i)
        voice[i] = i % 2 ? INT16_MIN : INT16_MAX;
    const auto loud = stubuddy::AnalyzeRecording(voice.data(), voice.size());
    assert(loud.clipped == voice.size() && loud.gain > 0 && loud.gain < 1);
    assert(stubuddy::ReplaySample(voice.front(), 0, voice.size(), loud, 160) == 0);
    assert(stubuddy::ReplaySample(voice.back(), voice.size() - 1, voice.size(), loud, 160) == 0);
    for (size_t i = 0; i < voice.size(); ++i) {
        const auto sample = stubuddy::ReplaySample(voice[i], i, voice.size(), loud, 160);
        assert(sample >= -983 && sample <= 983);
    }
    assert(stubuddy::ReplaySample(123, voice.size(), voice.size(), loud, 160) == 0);

    const int16_t quiet[] = {-1, 1};
    const auto low = stubuddy::AnalyzeRecording(quiet, 2);
    assert(low.gain == 8 && low.mean == 0);
    const int16_t biased[] = {990, 1010};
    const auto bias = stubuddy::AnalyzeRecording(biased, 2);
    assert(bias.mean == 1000 && bias.peak == 10 && bias.rms == 10 && bias.gain == 8);
}
