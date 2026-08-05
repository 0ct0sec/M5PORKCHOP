// attack_detect.h
// ---------------------------------------------------------------------------
// Passive 802.11 attack detection for PORKCHOP's DEFENSIVE SUITE ("SQUEAL
// ALERT"). Watches the management-frame mix the promiscuous path already sees
// and squeals when the air turns hostile:
//   * deauthentication flood  (mgmt type 0, subtype 0x0C)
//   * disassociation flood    (mgmt type 0, subtype 0x0A)
//   * beacon flood            (subtype 0x08 from many random/LA BSSIDs, e.g. mdk4)
//   * probe-request flood     (subtype 0x04)
//
// RX-only: it only counts frames. Deauth/disassoc/evil-twin are all standard
// 802.11 (IEEE 802.11 mgmt frames); no proprietary signature is involved.
//
// Frames arrive on the WiFi task; the per-second roll-up runs on the main loop.
// Counters are std::atomic so onFrame() stays lock-free and callback-safe.
// ---------------------------------------------------------------------------
#pragma once
#include <stdint.h>
#include <atomic>

namespace attackdet {

enum class AttackType : uint8_t {
    None = 0,
    DeauthFlood,
    DisassocFlood,
    BeaconFlood,
    ProbeFlood,
};

struct AttackStats {
    uint16_t deauthPerSec   = 0;
    uint16_t disassocPerSec = 0;
    uint16_t beaconPerSec   = 0;   // beacons from random/locally-administered BSSIDs
    uint16_t probePerSec    = 0;
    uint8_t  lastBssid[6]   = {0}; // transmitter of the last deauth/disassoc seen
};

// Tunable per-second thresholds. Normal air sits far below these; a live mdk4 /
// aireplay flood blows past them immediately.
struct AttackThresholds {
    uint16_t deauth   = 8;
    uint16_t disassoc = 8;
    uint16_t beacon   = 30;
    uint16_t probe    = 60;
};

class AttackMonitor {
public:
    // Callback-safe: called from the promiscuous RX callback per mgmt frame.
    //   fcType/fcSubtype : 802.11 frame-control type/subtype
    //   tx               : transmitter address (addr2), may be null
    void onFrame(uint8_t fcType, uint8_t fcSubtype, const uint8_t* tx);

    // Main-loop: roll the 1-second window. Returns the highest-severity attack
    // whose threshold was crossed this second (edge per second), else None.
    AttackType tick(uint32_t nowMs);

    const AttackStats& stats() const { return stats_; }
    AttackThresholds&  thresholds()  { return thr_; }

    void reset();

private:
    std::atomic<uint16_t> deauth_{0};
    std::atomic<uint16_t> disassoc_{0};
    std::atomic<uint16_t> beaconRandom_{0};
    std::atomic<uint16_t> probe_{0};
    volatile uint8_t lastBssid_[6] = {0};

    uint32_t windowStart_ = 0;
    AttackStats stats_;
    AttackThresholds thr_;
};

inline const char* attackLabel(AttackType t) {
    switch (t) {
        case AttackType::DeauthFlood:   return "DEAUTH FLOOD";
        case AttackType::DisassocFlood: return "DISASSOC FLOOD";
        case AttackType::BeaconFlood:   return "BEACON FLOOD";
        case AttackType::ProbeFlood:    return "PROBE FLOOD";
        default:                        return "";
    }
}

} // namespace attackdet
