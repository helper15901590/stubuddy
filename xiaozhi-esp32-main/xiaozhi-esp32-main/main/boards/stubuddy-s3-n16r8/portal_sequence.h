#pragma once

namespace stubuddy {
inline constexpr unsigned kPortalCloseDelayMs = 3000;

// Phones probe for a captive portal immediately after acquiring their IP.
// Publish DHCP only after both services can answer that first probe. A failed
// service must leave DHCP held; never advertise a partially ready portal.
template <class StartHttp, class StartDns, class StartDhcp>
int StartPortalServices(StartHttp http, StartDns dns, StartDhcp dhcp) {
    int result = http();
    if (result == 0)
        result = dns();
    if (result == 0)
        result = dhcp();
    return result;
}
}  // namespace stubuddy
