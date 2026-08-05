// tracker_detect.cpp
// ---------------------------------------------------------------------------
// Implementation of the passive BLE tracker detector. See tracker_detect.h.
// Original code for M5PORKCHOP (MIT). Signature values are public facts from
// the reference projects credited in the header.
// ---------------------------------------------------------------------------
#include "tracker_detect.h"
#include <string.h>

namespace trackerdet {
namespace {

// ---- BLE company identifiers (little-endian on air) -----------------------
constexpr uint16_t kApple   = 0x004C;
constexpr uint16_t kSamsung = 0x0075;

// ---- 16-bit service UUIDs -------------------------------------------------
constexpr uint16_t kTileA    = 0xFEED;
constexpr uint16_t kTileB    = 0xFEEC;
constexpr uint16_t kSmartTag = 0xFD5A;
constexpr uint16_t kGoogle   = 0xFEAA;   // Eddystone / Find My Device (shared)

// ---- Apple Find My type bytes (byte after the 0x004C company id) ----------
constexpr uint8_t kFindMyType = 0x12;    // Find My advertisement
constexpr uint8_t kLostType   = 0x1E;    // lost-item broadcast variant

inline uint16_t le16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

} // namespace

const char* typeLabel(TrackerType t) {
    switch (t) {
        case TrackerType::AppleFindMy:     return "AIRTAG/FINDMY";
        case TrackerType::Tile:            return "TILE";
        case TrackerType::SamsungSmartTag: return "SMARTTAG";
        case TrackerType::GoogleFindMy:    return "GOOGLE FIND";
        default:                           return "?";
    }
}

TrackerHit trackerInspectBleAdv(const uint8_t* addr, const uint8_t* adv,
                                uint8_t advLen, int8_t rssi) {
    TrackerHit h;
    if (!addr || !adv) return h;

    // Walk AD structures: [len][type][data...] repeated.
    uint8_t i = 0;
    while (i + 1 < advLen) {
        uint8_t fieldLen = adv[i];
        if (fieldLen == 0 || (uint16_t)(i + 1 + fieldLen) > advLen) break;
        uint8_t type = adv[i + 1];
        const uint8_t* data = &adv[i + 2];
        uint8_t dataLen = fieldLen - 1;

        // 0xFF: Manufacturer Specific Data -> [company LE][payload...]
        if (type == 0xFF && dataLen >= 2) {
            uint16_t company = le16(data);
            if (company == kApple && dataLen >= 3) {
                uint8_t appleType = data[2];
                if (appleType == kFindMyType || appleType == kLostType) {
                    h.type = TrackerType::AppleFindMy;
                    // A separated/lost AirTag broadcasts the long (~0x1E) frame
                    // carrying its rotating public key; nearby (owner present)
                    // is the short variant. Treat lost as the stronger signal.
                    h.lost = (appleType == kLostType) || (fieldLen >= 0x1E);
                    h.confidence = h.lost ? Confidence::High : Confidence::Medium;
                    h.label = "AIRTAG/FINDMY";
                }
            } else if (company == kSamsung) {
                h.type = TrackerType::SamsungSmartTag;
                h.confidence = Confidence::Medium;
                h.label = "SMARTTAG";
            }
        }

        // Service UUIDs: 16-bit lists (0x02/0x03) or 16-bit service data (0x16).
        if ((type == 0x02 || type == 0x03) && dataLen >= 2) {
            for (uint8_t off = 0; off + 2 <= dataLen; off += 2) {
                uint16_t uuid = le16(data + off);
                if (uuid == kTileA || uuid == kTileB) {
                    h.type = TrackerType::Tile; h.confidence = Confidence::Medium; h.label = "TILE";
                } else if (uuid == kSmartTag) {
                    h.type = TrackerType::SamsungSmartTag; h.confidence = Confidence::Medium; h.label = "SMARTTAG";
                } else if (uuid == kGoogle && h.type == TrackerType::None) {
                    h.type = TrackerType::GoogleFindMy; h.confidence = Confidence::Low; h.label = "GOOGLE FIND";
                }
            }
        }
        if (type == 0x16 && dataLen >= 2) {
            uint16_t uuid = le16(data);
            if (uuid == kTileA || uuid == kTileB) {
                h.type = TrackerType::Tile; h.confidence = Confidence::Medium; h.label = "TILE";
            } else if (uuid == kSmartTag) {
                h.type = TrackerType::SamsungSmartTag; h.confidence = Confidence::Medium; h.label = "SMARTTAG";
            } else if (uuid == kGoogle && h.type == TrackerType::None) {
                h.type = TrackerType::GoogleFindMy; h.confidence = Confidence::Low; h.label = "GOOGLE FIND";
            }
        }

        i += 1 + fieldLen;
    }

    if (h.hit()) {
        h.rssi = rssi;
        memcpy(h.addr, addr, 6);
    }
    return h;
}

} // namespace trackerdet
