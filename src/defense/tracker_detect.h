// tracker_detect.h
// ---------------------------------------------------------------------------
// Passive BLE tracker detection for PORKCHOP's DEFENSIVE SUITE ("TICK CHECK").
//
// Finds the personal-item trackers that can be slipped into a bag/car to follow
// someone: Apple AirTag / Find My, Tile, Samsung SmartTag, and (best-effort)
// the Google Find My Device network. RX-only: it parses advertisements NimBLE
// already receives; it never transmits or connects.
//
// Framework-agnostic core (no Arduino/IDF/NimBLE deps) so it host-unit-tests,
// mirroring flock_detect. The caller hands in the raw AD-structure payload.
//
// Signature data (facts, not code) attribution:
//   - AirTag / Apple Find My : mfr company 0x004C, Find My type 0x12, lost
//     broadcast 0x1E ............ simeononsecurity "Eye Spy", pasadoorian
//                                  CYD_ESP32-AirTag-Scanner
//   - Tile ....................... 16-bit service UUID 0xFEED / 0xFEEC
//   - Samsung SmartTag ........... service UUID 0xFD5A, mfr company 0x0075
//   - Google Find My Device ...... service-data UUID 0xFEAA (shared with
//                                  Eddystone -> reported at low confidence)
//   - ReconGrunt/FlipDeFlock, DULT unwanted-tracker spec (methodology)
// ---------------------------------------------------------------------------
#pragma once
#include <stdint.h>

namespace trackerdet {

enum class TrackerType : uint8_t {
    None = 0,
    AppleFindMy,      // AirTag or any Find My-network item
    Tile,
    SamsungSmartTag,
    GoogleFindMy,     // Find My Device network (low confidence: Eddystone-shared UUID)
};

// Ordered low -> high, same meaning as flockdet::Confidence.
enum class Confidence : uint8_t {
    None = 0,
    Low = 1,          // shared/ambiguous UUID (e.g. Google/Eddystone)
    Medium = 2,       // a specific tracker UUID / mfr signature
    High = 3,         // AirTag lost-item broadcast (actively looking for its owner)
};

struct TrackerHit {
    TrackerType type      = TrackerType::None;
    Confidence  confidence = Confidence::None;
    bool        lost      = false;   // Apple: separated/lost-item broadcast
    int8_t      rssi      = 0;
    uint8_t     addr[6]   = {0};
    const char* label     = "";      // short UI/log label

    bool hit() const { return type != TrackerType::None; }
};

// Inspect one BLE advertisement (raw AD structures: [len][type][data...]).
//   addr   : 6-byte device address (as reported by the scanner)
//   adv    : advertisement + scan-response payload
//   advLen : length of adv
//   rssi   : advertisement RSSI
TrackerHit trackerInspectBleAdv(const uint8_t* addr, const uint8_t* adv,
                                uint8_t advLen, int8_t rssi);

const char* typeLabel(TrackerType t);

} // namespace trackerdet
