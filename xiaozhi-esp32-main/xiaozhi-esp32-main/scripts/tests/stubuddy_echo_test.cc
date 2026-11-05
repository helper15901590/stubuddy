#include "ai_stream.h"
#include "clock_age.h"
#include "echo_protocol.h"

#include <cassert>
#include <string>
#include <vector>

int main() {
    using namespace stubuddy;
    using namespace stubuddy::echo;
    std::atomic<unsigned> timestamp{1000};
    const auto concurrent_clock = [&]() {
        timestamp.store(1001);  // producer advances between stamp and clock reads
        return 1001U;
    };
    assert(SnapshotAgeMs(timestamp, concurrent_clock) == 1);
    assert(ElapsedMs(1000, 1001) == 0);          // defensive future-stamp guard
    assert(ElapsedMs(0x10, 0xfffffff0U) == 32);  // real timer wrap
    timestamp.store(1000);
    assert(SnapshotAgeMs(timestamp, [] { return 45999U; }) < 45000);
    assert(SnapshotAgeMs(timestamp, [] { return 46000U; }) == 45000);
    assert(ElapsedMs(138572, 138573) < 20000);  // cannot revoke a live connection
    std::vector<int16_t> quiet_prefix(4800, 0);
    const auto stream_levels = AiStreamLevels(quiet_prefix.data(), quiet_prefix.size());
    assert(stream_levels.gain > 0);
    assert(ReplaySample(10000, 1000, 4800, stream_levels, 160) > 0);
    assert(ReplaySample(32767, 1000, 4800, stream_levels, 160) == kOutputPeakPcm16);
    assert(ReplaySample(-32768, 1000, 4800, stream_levels, 160) == -kOutputPeakPcm16);
    assert(ReplaySample(10000, 1000, 4800, stream_levels, 160) > 9000);
    const int16_t loud_prefix[] = {-30000, 30000};
    const auto loud_levels = AiStreamLevels(loud_prefix, 2);
    assert(std::abs(ReplaySample(30000, 1000, 4800, loud_levels, 160) - kOutputPeakPcm16) <= 1);
    // Regression: the real CosyVoice first packet was 244 bytes, exceeding
    // the unrelated local encoder's 227-byte capacity.
    std::vector<uint8_t> ai_storage(2 * (kAiReplyPacketBytes + 2));
    OpusPacketStore ai_packets(ai_storage.data(), ai_storage.size(), kAiReplyPacketBytes);
    std::vector<uint8_t> first_packet(244, 42);
    assert(ai_packets.Push(first_packet.data(), first_packet.size()));
    assert(!ai_packets.Push(first_packet.data(), kAiReplyPacketBytes + 1));
    Metadata prefix{1, 4696, 104, 5, 900, 123};
    Metadata empty{};
    assert(ValidAiCheckpoint(prefix, empty, false));
    assert(!ValidAiCheckpoint(prefix, empty, true));
    auto final = prefix;
    final.frames = 6;
    final.samples = 4800;
    final.bytes = 1080;
    assert(ValidAiCheckpoint(final, prefix, true));
    assert(ValidAiCheckpoint(final, final, true));
    assert(!ValidAiCheckpoint(final, final, false));
    auto corrupt = final;
    corrupt.samples = 4799;
    assert(!ValidAiCheckpoint(corrupt, prefix, true));
    corrupt = prefix;
    corrupt.frames = 6;
    assert(!ValidAiCheckpoint(corrupt, empty, false));
    corrupt = final;
    corrupt.frames = 11;
    corrupt.samples = 10456;
    assert(!ValidAiCheckpoint(corrupt, prefix, false));
    const uint8_t text[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    assert(Crc32(0, text, 9) == 0xcbf43926U);
    assert(Crc32(Crc32(0, text, 4), text + 4, 5) == 0xcbf43926U);
    std::vector<uint8_t> wire(kMaxMessage);
    auto size = PackAudio(wire.data(), wire.size(), 0x12345678, 1000, text, sizeof(text));
    assert(size == 25 && wire[4] == 0x12 && wire[7] == 0x78 && wire[10] == 3 && wire[11] == 0xe8);
    AudioView audio;
    assert(ParseAudio(wire.data(), size, audio));
    assert(audio.turn == 0x12345678 && audio.sequence == 1000 && audio.size == 9 &&
           audio.data[8] == '9');
    assert(!ParseAudio(wire.data(), size - 1, audio) && audio.data == nullptr);
    wire[0] = 0;
    assert(!ParseAudio(wire.data(), size, audio));
    wire[0] = 'S';
    wire[2] = 2;
    assert(!ParseAudio(wire.data(), size, audio));
    wire[2] = 1;
    Write32(wire.data() + 4, 0);
    assert(!ParseAudio(wire.data(), size, audio));
    assert(!PackAudio(wire.data(), wire.size(), 1, 1001, text, 9));
    assert(!PackAudio(wire.data(), 24, 1, 0, text, 9));
    std::vector<uint8_t> full(kOpusMaxPacketBytes, 42);
    assert(PackAudio(wire.data(), wire.size(), 1, 0, full.data(), full.size()) == kMaxMessage);
    MessageBuffer buffer;
    assert(buffer.Append(2, true, kMaxMessage, 0, wire.data(), 1024) ==
           MessageBuffer::Result::More);
    assert(buffer.Append(2, true, kMaxMessage, 1024, wire.data() + 1024, kMaxMessage - 1024) ==
           MessageBuffer::Result::Complete);
    assert(buffer.size() == kMaxMessage && ParseAudio(buffer.data(), buffer.size(), audio));
    buffer.Reset();
    assert(buffer.Append(2, false, 25, 0, wire.data(), 25) == MessageBuffer::Result::Invalid);
    assert(buffer.Append(2, true, kMaxMessage + 1, 0, wire.data(), 1) ==
           MessageBuffer::Result::Invalid);
    assert(buffer.Append(2, true, 25, 1, wire.data(), 24) == MessageBuffer::Result::Invalid);
    assert(buffer.Append(2, true, 25, 0, wire.data(), 10) == MessageBuffer::Result::More);
    assert(buffer.Append(1, true, 25, 10, wire.data() + 10, 15) == MessageBuffer::Result::Invalid);
    Metadata good{1, 960000, 104, 1001, 1001, 0};
    assert(good.valid());
    auto bad = good;
    bad.frames = 1000;
    assert(!bad.valid());
    bad = good;
    bad.delay = 960;
    assert(!bad.valid());
    bad = good;
    bad.samples = 4799;
    assert(!bad.valid());
    bad = good;
    bad.bytes = 0;
    assert(!bad.valid());
    bad = good;
    bad.bytes = 4096 * 1001 + 1;
    assert(!bad.valid());
    for (const std::string valid : {"{}", "{\"escaped\":\"\\\"{[\\\\\"}", "[[[[[[[[]]]]]]]]"})
        assert(JsonDepthAllowed(reinterpret_cast<const uint8_t*>(valid.data()), valid.size()));
    for (const std::string invalid : {"[[[[[[[[[]]]]]]]]]", "{", "}", "\"unfinished"})
        assert(!JsonDepthAllowed(reinterpret_cast<const uint8_t*>(invalid.data()), invalid.size()));
}
