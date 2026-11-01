#pragma once

#include <cstdint>

namespace stubuddy {

// A single uncompressed IN question. Ignore EDNS extras; never parse pointers
// supplied by clients. AAAA/other types receive a valid empty answer.
constexpr unsigned CaptiveDnsReply(uint8_t* packet, unsigned length, unsigned capacity,
                                   const uint8_t address[4]) {
    if (length < 17 || length > capacity || (packet[2] & 0xfa) || packet[3] & 15 ||
        packet[4] != 0 || packet[5] != 1 || packet[6] || packet[7] || packet[8] || packet[9])
        return 0;
    unsigned end = 12;
    while (end < length && packet[end]) {
        const unsigned label = packet[end++];
        if (label > 63 || label > length - end)
            return 0;
        end += label;
        if (end - 12 > 254)
            return 0;
    }
    if (end >= length || end - 12 > 254 || length - end < 5)
        return 0;
    ++end;
    const bool ipv4 = packet[end] == 0 && packet[end + 1] == 1;
    if (packet[end + 2] != 0 || packet[end + 3] != 1)
        return 0;
    end += 4;
    if (ipv4 && capacity - end < 16)
        return 0;
    packet[2] = 0x84 | (packet[2] & 1);  // response, authoritative, preserve RD
    packet[3] = 0;
    packet[6] = 0;
    packet[7] = ipv4 ? 1 : 0;
    packet[8] = packet[9] = packet[10] = packet[11] = 0;
    if (ipv4) {
        const uint8_t answer[] = {0xc0, 0x0c, 0, 1, 0,          1,          0,          0,
                                  0,    0,    0, 4, address[0], address[1], address[2], address[3]};
        for (const auto byte : answer)
            packet[end++] = byte;
    }
    return end;
}

}  // namespace stubuddy
