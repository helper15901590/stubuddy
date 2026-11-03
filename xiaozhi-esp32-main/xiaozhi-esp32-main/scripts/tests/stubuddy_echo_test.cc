#include "echo_protocol.h"

#include <cassert>
#include <string>
#include <vector>

int main() {
    using namespace stubuddy;
    using namespace stubuddy::echo;
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
