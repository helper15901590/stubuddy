#include <cassert>
#include <deque>
#include "uplink_budget.h"

int main() {
    stubuddy::UplinkBudget budget;
    std::deque<unsigned> fifo;
    // Two seconds of blocked sending plus the encoder tail fits, unlike depth 8.
    for (unsigned sequence = 0; sequence < 36; ++sequence) {
        assert(budget.AcquireAudio());
        fifo.push_back(sequence);
    }
    // Stop stays behind all audio. Control traffic can use the reserved slots.
    fifo.push_back(1000);
    for (unsigned sequence = 0; sequence < 36; ++sequence) {
        assert(fifo.front() == sequence);
        fifo.pop_front();
        budget.ReleaseAudio();
    }
    assert(fifo.front() == 1000);
    fifo.clear();
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        for (unsigned i = 0; i < budget.kAudioSlots; ++i) {
            assert(budget.AcquireAudio());
            fifo.push_back(i);
        }
        for (unsigned i = 0; i < 100; ++i)
            assert(!budget.AcquireAudio());  // Failure must not leak credits.
        for (unsigned i = 0; i < budget.kControlSlots; ++i)
            fifo.push_back(1000 + i);
        assert(fifo.size() == budget.kDepth);
        // Worker discards cancelled/old-epoch audio, returning every credit.
        while (!fifo.empty()) {
            if (fifo.front() < 1000)
                budget.ReleaseAudio();
            fifo.pop_front();
        }
    }
    assert(budget.AcquireAudio());
    budget.ReleaseAudio();
}
