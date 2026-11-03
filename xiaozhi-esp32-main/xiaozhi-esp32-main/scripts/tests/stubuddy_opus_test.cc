#include "opus_framing.h"

#include <array>
#include <cassert>
#include <vector>

int main() {
    using namespace stubuddy;
    static_assert(kOpusFrameSamples == 16000 * 60 / 1000);
    static_assert(OpusPacketsNeeded(960000, 104) == 1001);
    static_assert(OpusPacketsNeeded(960, 104) == 2);
    static_assert(OpusPacketsNeeded(100, 104) == 1);
    static_assert(OpusPacketsNeeded(0, 104) == 0);
    static_assert(OpusPacketsNeeded(960001, 104) == 0);
    static_assert(OpusPacketsNeeded(960, 961) == 0);

    std::array<uint8_t, 50> storage{};
    OpusPacketStore packets(storage.data(), storage.size(), 20);
    const uint8_t payload[] = {0x7f, 0x12, 0x00, 0x45};
    assert(!packets.Push(nullptr, 4) && !packets.Push(payload, 0));
    assert(!packets.Push(payload, 21));
    assert(packets.Push(payload, sizeof(payload)));
    assert(packets.Push(payload, sizeof(payload)));
    assert(packets.count() == 2 && packets.used() == 12 && packets.payload_bytes() == 8);
    size_t cursor = 0;
    OpusPacketView view;
    assert(packets.Next(cursor, view) && view.size == 4 && view.data[3] == 0x45);
    assert(packets.Next(cursor, view) && cursor == packets.used());
    assert(!packets.Next(cursor, view) && !view.data);
    // Bad framing fails without advancing the cursor or returning stale data.
    storage[0] = 49;
    cursor = 0;
    assert(!packets.Next(cursor, view) && cursor == 0 && !view.data);
    storage[0] = 4;
    packets.Reset();
    assert(!packets.Next(cursor, view) && packets.count() == 0);
    assert(packets.Push(payload, 1));
    packets.Erase();
    // Clear the old high-water range, even after replacing a long round with a short one.
    for (size_t i = 0; i < 12; ++i)
        assert(storage[i] == 0);
    std::vector<uint8_t> full(kOpusMaxPackets * 3);
    OpusPacketStore bounded(full.data(), full.size(), 1);
    for (size_t i = 0; i < kOpusMaxPackets; ++i)
        assert(bounded.Push(payload, 1));
    assert(!bounded.Push(payload, 1));
    OpusPacketStore tiny(storage.data(), 2, 20);
    assert(!tiny.Push(payload, 1));

    // Synthetic delayed frames prove exact output count, preserved first/last
    // samples, and sentinel protection for sub-frame and full-minute inputs.
    for (size_t count : {size_t(1), size_t(100), size_t(959), size_t(960), size_t(961),
                         size_t(12345), kOpusMaxSamples}) {
        for (size_t delay : {size_t(0), size_t(1), size_t(104), size_t(959), size_t(960)}) {
            std::vector<int16_t> output(count + 2, -30000);
            size_t copied = 0;
            std::array<int16_t, kOpusFrameSamples> frame{};
            for (size_t i = 0; i < OpusPacketsNeeded(count, delay); ++i) {
                for (size_t j = 0; j < frame.size(); ++j) {
                    const size_t absolute = i * frame.size() + j;
                    frame[j] = absolute < delay || absolute >= delay + count
                                   ? int16_t(-12345)
                                   : int16_t((absolute - delay) % 20000);
                }
                copied += CopyOpusPcm(frame.data(), frame.size(), i * frame.size(), delay, count,
                                      output.data() + 1, count);
            }
            assert(copied == count && output.front() == -30000 && output.back() == -30000);
            for (size_t i = 0; i < count; ++i)
                assert(output[i + 1] == int16_t(i % 20000));
            assert(CopyOpusPcm(frame.data(), frame.size(), 0, delay, count, output.data(),
                               count - 1) == 0);
        }
    }

    std::vector<int16_t> reference(kOpusFrameSamples * 3);
    std::vector<int16_t> decoded(kOpusFrameSamples * 4);
    uint32_t random = 0x53545542;
    for (auto& sample : reference) {
        random = random * 1664525U + 1013904223U;
        sample = int16_t(int32_t(random >> 16) / 4 - 8192);
    }
    for (size_t delay : {size_t(0), size_t(104), size_t(960)}) {
        std::fill(decoded.begin(), decoded.end(), 0);
        for (size_t i = 0; i < reference.size(); ++i)
            decoded[i + delay] = reference[i] / 2 + 100;
        const auto found =
            EstimateOpusDelay(reference.data(), reference.size(), decoded.data(), decoded.size());
        assert(found.samples == delay && found.correlation > 0.999);
    }
    std::fill(decoded.begin(), decoded.end(), 123);
    assert(EstimateOpusDelay(reference.data(), reference.size(), decoded.data(), decoded.size())
               .correlation == 0);
    assert(EstimateOpusDelay(nullptr, 0, nullptr, 0).correlation == 0);
}
