#include "wifi_rules.h"

constexpr bool WifiRulesPass() {
    using stubuddy::FlatSetupJson;
    using stubuddy::ValidWifiCredentials;
    constexpr char json[] = "{\"ssid\":\"home\",\"password\":\"12345678\"}";
    if (!FlatSetupJson(json, sizeof(json) - 1))
        return false;
    constexpr char nested[] = "{\"a\":{\"b\":1}}";
    if (FlatSetupJson(nested, sizeof(nested) - 1))
        return false;
    constexpr char nul[] = "{\"ssid\":\"abc\\u0000evil\"}";
    if (FlatSetupJson(nul, sizeof(nul) - 1))
        return false;
    constexpr char literal[] = "{\"ssid\":\"abc\\\\u0000\"}";
    if (!FlatSetupJson(literal, sizeof(literal) - 1))
        return false;
    if (!ValidWifiCredentials("home", 4, "12345678", 8))
        return false;
    if (!ValidWifiCredentials("home", 4, "", 0))
        return false;
    if (ValidWifiCredentials("", 0, "12345678", 8))
        return false;
    if (ValidWifiCredentials("home", 4, "1234567", 7))
        return false;
    if (ValidWifiCredentials("bad\nname", 8, "12345678", 8))
        return false;
    if (ValidWifiCredentials("bad\0name", 8, "12345678", 8))
        return false;
    if (!ValidWifiCredentials("\xe5\xad\xa6\xe4\xb9\xa0", 6, "12345678", 8))
        return false;
    if (ValidWifiCredentials("\xe5\xad", 2, "12345678", 8))
        return false;
    if (ValidWifiCredentials("\xc0\x80", 2, "12345678", 8))
        return false;
    if (ValidWifiCredentials("\xed\xa0\x80", 3, "12345678", 8))
        return false;
    if (ValidWifiCredentials("\xf4\x90\x80\x80", 4, "12345678", 8))
        return false;
    char ssid[33] = {};
    char password[64] = {};
    for (unsigned i = 0; i < 33; ++i)
        ssid[i] = 's';
    for (unsigned i = 0; i < 64; ++i)
        password[i] = 'p';
    if (!ValidWifiCredentials(ssid, 32, password, 63))
        return false;
    if (ValidWifiCredentials(ssid, 33, password, 63))
        return false;
    if (ValidWifiCredentials(ssid, 32, password, 64))
        return false;
    if (!stubuddy::BootSetupRequested(true, true) || stubuddy::BootSetupRequested(false, true) ||
        stubuddy::BootSetupRequested(true, false) || stubuddy::BootSetupRequested(false, false))
        return false;
    stubuddy::WifiConnectionPolicy policy;
    policy.Start(true, false, true);
    if (policy.manual_setup() || !policy.AutomaticReconnectAllowed())
        return false;  // normal saved-network boot is station-only
    policy.Start(true, true, true);
    if (!policy.manual_setup() || policy.AutomaticReconnectAllowed())
        return false;  // boot-button setup cannot use saved WiFi automatically
    policy.Start(false, false, false);
    if (!policy.manual_setup() || policy.AutomaticReconnectAllowed())
        return false;
    policy.Start(true, false, false);
    if (!policy.manual_setup() || policy.AutomaticReconnectAllowed())
        return false;  // legacy history alone cannot auto-connect after migration
    policy.Start(true, false, true);
    policy.EnterManualSetup();
    if (policy.AutomaticReconnectAllowed())
        return false;  // a failed automatic attempt opens a latched manual session
    policy.SetupCompleted();
    if (policy.manual_setup() || !policy.AutomaticReconnectAllowed())
        return false;
    policy.EnterManualSetup();
    if (policy.AutomaticReconnectAllowed())
        return false;
    return true;
}
static_assert(WifiRulesPass(), "WiFi input or boot-gesture validation regression");
