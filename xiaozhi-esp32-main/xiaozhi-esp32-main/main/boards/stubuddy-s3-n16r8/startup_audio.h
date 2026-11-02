#pragma once

class Board;

namespace stubuddy {
// Shared, unchanged 3.1 memory check and single prompt through AudioCodec.
using PlaybackIndicator = void (*)(bool playing);
bool PlayStartupPrompt(Board& board, PlaybackIndicator indicator = nullptr);
}  // namespace stubuddy
