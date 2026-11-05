#include "ptt_button.h"
#include "ptt_playback.h"
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

constexpr bool CheckPlaybackInterrupt() {
    stubuddy::PttButton button;
    button.RequireRelease(true, 0);  // the old key is still held at the 60-second limit
    stubuddy::PlaybackInterrupt interrupt(button.pressed());
    for (unsigned now = 0; now < 1000; now += 10) {
        button.Update(true, now);
        if (interrupt.Update(button.pressed()))
            return false;
    }
    button.Update(false, 1000);
    button.Update(false, 1029);
    if (interrupt.Update(button.pressed()))
        return false;
    button.Update(false, 1030);
    if (interrupt.Update(button.pressed()))
        return false;
    button.Update(true, 1100);
    button.Update(false, 1110);  // contact bounce must not stop playback
    button.Update(true, 1120);
    button.Update(true, 1149);
    if (interrupt.Update(button.pressed()))
        return false;
    button.Update(true, 1150);
    if (!interrupt.Update(button.pressed()) || interrupt.Update(true))
        return false;
    // After abort, the same held press continues recording without a release gate.
    button.Update(true, 2000);
    if (!button.pressed())
        return false;
    button.Update(false, 2010);
    button.Update(false, 2040);
    if (button.pressed())
        return false;
    stubuddy::PlaybackInterrupt normal(false);
    return normal.Update(true) && !normal.Update(true);
}
static_assert(CheckPlaybackInterrupt(), "fresh-press interruption/released-key handoff regression");

constexpr bool CheckPlaybackEndRearm() {
    stubuddy::PttButton button;
    button.RequireRelease(false, 0);
    button.Update(false, 30);
    stubuddy::PlaybackInterrupt playing(button.pressed());
    if (!playing.released())
        return false;
    // Fresh press begins during final DMA drain; 30ms debounce still applies.
    button.Update(true, 100);
    button.Update(true, 129);
    if (playing.Update(button.pressed()))
        return false;
    button.Update(true, 130);
    if (!playing.Update(button.pressed()))
        return false;
    // A press straddling the return-to-idle boundary retains its debounce age.
    stubuddy::PttButton boundary;
    boundary.RequireRelease(false, 0);
    boundary.Update(false, 30);
    boundary.Update(true, 200);
    boundary.Update(true, 220);  // playback returns; do not RequireRelease again
    if (boundary.pressed())
        return false;
    boundary.Update(true, 230);
    if (!boundary.pressed())
        return false;
    // The original 60-second held key is never armed by normal completion.
    stubuddy::PlaybackInterrupt timed_out(true);
    return !timed_out.released() && !timed_out.Update(true) && !timed_out.Update(false) &&
           timed_out.released() && timed_out.Update(true);
}
static_assert(CheckPlaybackEndRearm(),
              "playback drain/end press swallowed or held timeout key rearmed");

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
        assert(sample >= -stubuddy::kOutputPeakPcm16 && sample <= stubuddy::kOutputPeakPcm16);
    }
    assert(stubuddy::ReplaySample(123, voice.size(), voice.size(), loud, 160) == 0);
    assert(std::abs(stubuddy::ReplaySample(voice[500], 500, voice.size(), loud, 160)) >= 22935);

    const int16_t quiet[] = {-1, 1};
    const auto low = stubuddy::AnalyzeRecording(quiet, 2);
    assert(low.gain == 8 && low.mean == 0);
    const int16_t biased[] = {990, 1010};
    const auto bias = stubuddy::AnalyzeRecording(biased, 2);
    assert(bias.mean == 1000 && bias.peak == 10 && bias.rms == 10 && bias.gain == 8);
}
