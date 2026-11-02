#include "captive_dns_packet.h"

constexpr bool CaptiveDnsPass() {
    // example.com A IN with recursion desired, no answers.
    constexpr uint8_t query[] = {0x12, 0x34, 1,   0,   0,   1, 0,   0,   0,   0, 0, 0, 7, 'e', 'x',
                                 'a',  'm',  'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1};
    constexpr uint8_t ip[] = {192, 168, 4, 1};
    uint8_t packet[512] = {};
    for (unsigned i = 0; i < sizeof(query); ++i)
        packet[i] = query[i];
    const unsigned size = stubuddy::CaptiveDnsReply(packet, sizeof(query), sizeof(packet), ip);
    if (size != sizeof(query) + 16 || packet[0] != 0x12 || packet[1] != 0x34 || packet[2] != 0x85 ||
        packet[7] != 1 || packet[size - 4] != 192 || packet[size - 3] != 168 ||
        packet[size - 2] != 4 || packet[size - 1] != 1 || packet[size - 8] || packet[size - 7])
        return false;
    for (unsigned length = 0; length < sizeof(query); ++length) {
        for (unsigned i = 0; i < sizeof(query); ++i)
            packet[i] = query[i];
        if (stubuddy::CaptiveDnsReply(packet, length, sizeof(packet), ip))
            return false;
    }
    if (stubuddy::CaptiveDnsReply(packet, sizeof(query), sizeof(query) + 15, ip))
        return false;  // no output overflow
    packet[26] = 28;   // AAAA receives NODATA, not an invalid IPv4 answer
    if (stubuddy::CaptiveDnsReply(packet, sizeof(query), sizeof(packet), ip) != sizeof(query) ||
        packet[7] != 0)
        return false;
    for (unsigned i = 0; i < sizeof(query); ++i)
        packet[i] = query[i];
    packet[12] = 0xc0;
    if (stubuddy::CaptiveDnsReply(packet, sizeof(query), sizeof(packet), ip))
        return false;  // refuse client-controlled compression pointers
    packet[12] = 64;
    if (stubuddy::CaptiveDnsReply(packet, sizeof(query), sizeof(packet), ip))
        return false;
    packet[12] = 7;
    packet[5] = 2;
    if (stubuddy::CaptiveDnsReply(packet, sizeof(query), sizeof(packet), ip))
        return false;
    return true;
}
static_assert(CaptiveDnsPass(), "Captive DNS packet boundary or response regression");
