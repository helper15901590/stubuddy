#pragma once

#include "echo_protocol.h"
#include "recording_pcm.h"

namespace stubuddy {
inline constexpr size_t kAiReplyPacketBytes = 1275;
inline constexpr size_t kAiPrebufferSamples = 4800;

inline RecordingLevels AiStreamLevels(const int16_t* pcm, size_t samples) {
    auto levels = AnalyzeRecording(pcm, samples);
    // A silent/quiet TTS prefix must never mute or boost the entire answer.
    // Fix one gain per turn, retaining ReplaySample's accepted 3% peak cap.
    levels.gain = levels.peak >= 983 ? std::min(levels.gain, 0.1f) : 0.04f;
    return levels;
}

// A checkpoint releases at most five newly received 60ms frames. Prefixes
// contain only real PCM; encoder padding can be exposed only at the final trim.
inline bool ValidAiCheckpoint(const echo::Metadata& next, const echo::Metadata& previous,
                              bool final) {
    return next.turn && next.samples && next.samples <= kOpusMaxSamples &&
           (!final || next.samples >= kAiPrebufferSamples) && next.delay < kOpusFrameSamples &&
           next.frames && next.frames <= kOpusMaxPackets &&
           next.frames == (next.samples + next.delay + kOpusFrameSamples - 1) / kOpusFrameSamples &&
           next.bytes >= next.frames && next.bytes <= next.frames * kAiReplyPacketBytes &&
           next.frames >= previous.frames && next.frames - previous.frames <= 5 &&
           (final || next.frames > previous.frames) && next.samples >= previous.samples &&
           (next.frames != previous.frames || next.samples == previous.samples);
}
}  // namespace stubuddy
