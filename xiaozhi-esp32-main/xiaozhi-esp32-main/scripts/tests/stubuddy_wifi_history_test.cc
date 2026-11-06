#include "wifi_history.h"

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
    // Removing a middle entry preserves every other SSID/password and order.
    if (!history.Forget("five") || history.count != 4 || history.Find("five") != -1 ||
        history.Find("one") != 0 || history.Find("six") != 1 || history.Find("four") != 2 ||
        history.Find("three") != 3 || !history.Valid())
        return false;
    if (history.Forget("absent") || history.count != 4)
        return false;
    // The whole unused slot is blank in the blob, including the old password.
    for (char c : history.items[4].ssid)
        if (c)
            return false;
    for (char c : history.items[4].password)
        if (c)
            return false;
    if (!WifiHistory::Equal(history.items[0].password, "new-password", 64) ||
        history.items[1].password[0])
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
    WifiHistory corrupt;
    corrupt.count = 1;
    for (unsigned i = 0; i < sizeof(corrupt.items[0].ssid); ++i)
        corrupt.items[0].ssid[i] = 'x';
    if (corrupt.Valid())
        return false;  // corrupt NVS without a terminating NUL
    return true;
}
static_assert(WifiHistoryPass(), "WiFi history ordering, password or NVS validation regression");

constexpr bool SavedFallbackPass() {
    stubuddy::WifiHistory history;
    history.Remember({"home", "home-password"});
    history.Remember({"office", "office-password"});
    stubuddy::WifiSavedAttempts attempts;
    attempts.Start(history.count);
    unsigned index = 99;
    if (!attempts.Take(index) || index != 0 ||
        !stubuddy::WifiHistory::Equal(history.items[index].ssid, "office", 33))
        return false;
    // Office unavailable: the next candidate must be home, never office again.
    if (!attempts.pending() || !attempts.Take(index) || index != 1 ||
        !stubuddy::WifiHistory::Equal(history.items[index].ssid, "home", 33))
        return false;
    const auto connected = history.items[index];
    attempts.Start(0);
    history.Remember(connected);
    if (attempts.pending() || attempts.Take(index) || history.Find("home") != 0)
        return false;
    // A new boot/drop begins a fresh bounded pass in the new priority order.
    attempts.Start(history.count);
    if (!attempts.Take(index) || index != 0 || !attempts.Take(index) || index != 1 ||
        attempts.pending() || attempts.Take(index))
        return false;
    // Forget all records, serialize/reload the resulting blob, then reboot.
    if (!history.Forget("office") || !history.Forget("home") || !history.Valid())
        return false;
    const auto reloaded = history;
    attempts.Start(reloaded.count);
    return reloaded.count == 0 && reloaded.Find("home") == -1 && !attempts.Take(index);
}
static_assert(SavedFallbackPass(), "Saved WiFi fallback, exhaustion or forget-all regression");
