#include "../main/wifi_history.h"

constexpr bool WifiHistoryPass() {
    using stubuddy::WifiCredentials;
    using stubuddy::WifiHistory;
    WifiHistory history;
    if (!history.Valid() || history.Find("one") != -1)
        return false;
    const WifiCredentials one = {"one", "12345678"};
    if (!history.Remember(one) || history.count != 1 || history.Remember(one))
        return false;
    history.Remember({"two", "12345678"});
    history.Remember({"three", "12345678"});
    history.Remember({"four", "12345678"});
    history.Remember({"five", "12345678"});
    if (!history.Valid() || history.count != 5 || history.Find("one") != 4)
        return false;
    // A successful history click promotes without duplicating the SSID.
    history.Remember(one);
    if (history.Find("one") != 0 || history.Find("five") != 1 || history.count != 5)
        return false;
    history.Remember({"six", ""});  // explicitly open network
    if (history.Find("two") != -1 || history.Find("one") != 1 || !history.Valid())
        return false;
    history.Remember({"one", "new-password"});
    if (!WifiHistory::Equal(history.items[0].password, "new-password", 64) || history.count != 5 ||
        !history.Valid())
        return false;
    history.items[1] = history.items[0];
    if (history.Valid())
        return false;
    history = WifiHistory{};
    history.count = 6;
    if (history.Valid())
        return false;
    history = WifiHistory{};
    history.version = 0;
    if (history.Valid())
        return false;
    history = WifiHistory{};
    history.Remember(one);
    for (auto& c : history.items[0].ssid)
        c = 'x';
    if (history.Valid())
        return false;  // corrupt NVS without a terminating NUL
    return true;
}
static_assert(WifiHistoryPass(), "WiFi history ordering, password or NVS validation regression");
