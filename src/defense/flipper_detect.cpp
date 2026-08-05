// flipper_detect.cpp — see flipper_detect.h. Original code for M5PORKCHOP (MIT).
// NOTE: `addr` is expected in human display order (addr[0] = first printed
// octet / OUI), so the caller must un-reverse NimBLE's little-endian address.
#include "flipper_detect.h"
#include <string.h>

namespace flipperdet {
namespace {
// Known Flipper Zero BLE address prefixes (first three printed octets).
const uint8_t kPrefixes[][3] = {
    {0x0C, 0xFA, 0x22},
    {0x80, 0xE1, 0x26},
    {0x80, 0xE1, 0x27},
};
constexpr uint8_t kPrefixCount = sizeof(kPrefixes) / sizeof(kPrefixes[0]);

bool prefixMatch(const uint8_t* addr) {
    for (uint8_t i = 0; i < kPrefixCount; ++i) {
        if (addr[0] == kPrefixes[i][0] && addr[1] == kPrefixes[i][1] &&
            addr[2] == kPrefixes[i][2]) return true;
    }
    return false;
}

// case-sensitive "starts with Flipper"
bool startsWithFlipper(const uint8_t* s, uint8_t len) {
    static const char* kF = "Flipper";
    if (len < 7) return false;
    for (uint8_t i = 0; i < 7; ++i) if ((char)s[i] != kF[i]) return false;
    return true;
}
}

FlipperHit flipperInspect(const uint8_t* addr, const uint8_t* adv,
                          uint8_t advLen, int8_t rssi) {
    FlipperHit h;
    if (!addr) return h;

    bool byPrefix = prefixMatch(addr);
    bool byName = false;

    if (adv) {
        uint8_t i = 0;
        while (i + 1 < advLen) {
            uint8_t fieldLen = adv[i];
            if (fieldLen == 0 || (uint16_t)(i + 1 + fieldLen) > advLen) break;
            uint8_t type = adv[i + 1];
            const uint8_t* data = &adv[i + 2];
            uint8_t dataLen = fieldLen - 1;
            if ((type == 0x08 || type == 0x09) && dataLen > 0) {   // shortened/complete name
                if (startsWithFlipper(data, dataLen)) {
                    byName = true;
                    uint8_t n = dataLen < 23 ? dataLen : 23;
                    memcpy(h.name, data, n);
                    h.name[n] = '\0';
                }
            }
            i += 1 + fieldLen;
        }
    }

    if (byPrefix || byName) {
        h.isFlipper = true;
        h.rssi = rssi;
        memcpy(h.addr, addr, 6);
        if (h.name[0] == '\0') strncpy(h.name, "Flipper", sizeof(h.name) - 1);
    }
    return h;
}

} // namespace flipperdet
