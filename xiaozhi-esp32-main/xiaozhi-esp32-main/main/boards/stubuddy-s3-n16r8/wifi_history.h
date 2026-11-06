#pragma once

#include "wifi_rules.h"

namespace stubuddy {

struct WifiCredentials {
    char ssid[33] = {};
    char password[64] = {};
};

// Most recently connected first. Only successful connections enter this list.
struct WifiHistory {
    static constexpr unsigned kCapacity = 5;
    unsigned version = 0x53544232;
    unsigned count = 0;
    WifiCredentials items[kCapacity] = {};

    static constexpr unsigned Length(const char* value, unsigned limit) {
        unsigned n = 0;
        while (n < limit && value[n])
            ++n;
        return n;
    }
    static constexpr bool Equal(const char* a, const char* b, unsigned limit) {
        for (unsigned i = 0; i < limit; ++i) {
            if (a[i] != b[i])
                return false;
            if (!a[i])
                return true;
        }
        return false;
    }
    constexpr int Find(const char* ssid) const {
        for (unsigned i = 0; i < count && i < kCapacity; ++i)
            if (Equal(items[i].ssid, ssid, 33))
                return static_cast<int>(i);
        return -1;
    }
    constexpr bool Valid() const {
        if (version != 0x53544232 || count > kCapacity)
            return false;
        for (unsigned i = 0; i < count; ++i) {
            const auto& item = items[i];
            if (!ValidWifiCredentials(item.ssid, Length(item.ssid, 33), item.password,
                                      Length(item.password, 64)))
                return false;
            for (unsigned j = 0; j < i; ++j)
                if (Equal(items[j].ssid, item.ssid, 33))
                    return false;
        }
        return true;
    }
    // Returns whether the persisted order or password actually changed.
    constexpr bool Remember(const WifiCredentials& successful) {
        const int found = Find(successful.ssid);
        if (found == 0 && Equal(items[0].password, successful.password, 64))
            return false;
        unsigned last = found >= 0 ? static_cast<unsigned>(found) : count;
        if (found < 0 && count < kCapacity)
            ++count;
        if (last >= kCapacity)
            last = kCapacity - 1;
        for (unsigned i = last; i > 0; --i)
            items[i] = items[i - 1];
        items[0] = successful;
        return true;
    }
    constexpr bool Forget(const char* ssid) {
        const int found = Find(ssid);
        if (found < 0)
            return false;
        for (unsigned i = static_cast<unsigned>(found); i + 1 < count; ++i)
            items[i] = items[i + 1];
        items[--count] = {};  // Remove the password from the unused persisted slot too.
        return true;
    }
};

// One bounded pass through the saved order. A successful connection promotes
// its entry only after the pass ends; failed attempts never reorder history.
class WifiSavedAttempts {
public:
    constexpr void Start(unsigned count) {
        count_ = count <= WifiHistory::kCapacity ? count : WifiHistory::kCapacity;
        next_ = 0;
    }
    constexpr bool pending() const { return next_ < count_; }
    constexpr unsigned count() const { return count_; }
    constexpr bool Take(unsigned& index) {
        if (!pending())
            return false;
        index = next_++;
        return true;
    }

private:
    unsigned count_ = 0, next_ = 0;
};

}  // namespace stubuddy
