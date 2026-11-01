#pragma once

namespace stubuddy {

// Reject nesting before recursive JSON parsing and escaped NUL truncation.
constexpr bool FlatSetupJson(const char* text, unsigned length) {
    bool quoted = false, escape = false;
    unsigned depth = 0;
    for (unsigned i = 0; i < length; ++i) {
        const unsigned char c = text[i];
        if (quoted) {
            if (c < 32)
                return false;
            if (escape) {
                if (c == 'u' && i + 4 < length && text[i + 1] == '0' && text[i + 2] == '0' &&
                    text[i + 3] == '0' && text[i + 4] == '0')
                    return false;
                escape = false;
            } else if (c == '\\')
                escape = true;
            else if (c == '"')
                quoted = false;
        } else if (c == '"')
            quoted = true;
        else if (c == '{') {
            if (++depth > 1)
                return false;
        } else if (c == '}') {
            if (depth == 0)
                return false;
            --depth;
        } else if (c == '[' || c == ']' || (c < 32 && c != '\n' && c != '\r' && c != '\t'))
            return false;
    }
    return !quoted && depth == 0;
}

constexpr bool ValidWifiText(const char* text, unsigned length) {
    for (unsigned i = 0; i < length; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 32 || c == 127)
            return false;
        if (c < 128)
            continue;
        unsigned extra = 0;
        unsigned code = 0;
        unsigned minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) {
            extra = 1;
            code = c & 31;
            minimum = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            extra = 2;
            code = c & 15;
            minimum = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            extra = 3;
            code = c & 7;
            minimum = 0x10000;
        } else
            return false;
        if (i + extra >= length)
            return false;
        for (unsigned j = 0; j < extra; ++j) {
            const auto next = static_cast<unsigned char>(text[++i]);
            if ((next & 0xc0) != 0x80)
                return false;
            code = (code << 6) | (next & 63);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
            return false;
    }
    return true;
}

constexpr bool ValidWifiCredentials(const char* ssid, unsigned ssid_bytes, const char* password,
                                    unsigned password_bytes) {
    return ssid_bytes >= 1 && ssid_bytes <= 32 &&
           (password_bytes == 0 || (password_bytes >= 8 && password_bytes <= 63)) &&
           ValidWifiText(ssid, ssid_bytes) && ValidWifiText(password, password_bytes);
}

// Sampled once at application startup and once after the 30ms contact debounce.
// A later runtime press cannot enter provisioning.
constexpr bool BootSetupRequested(bool initially_pressed, bool after_debounce) {
    return initially_pressed && after_debounce;
}

// An open setup session belongs to the parent. No timer, scan or stale
// disconnect event may turn it into an automatic saved-network attempt.
class WifiConnectionPolicy {
public:
    constexpr void Start(bool have_saved, bool force_setup, bool setup_confirmed) {
        manual_setup_ = force_setup || !have_saved || !setup_confirmed;
    }
    constexpr void EnterManualSetup() { manual_setup_ = true; }
    constexpr void SetupCompleted() { manual_setup_ = false; }
    constexpr bool manual_setup() const { return manual_setup_; }
    constexpr bool AutomaticReconnectAllowed() const { return !manual_setup_; }

private:
    bool manual_setup_ = true;
};

}  // namespace stubuddy
