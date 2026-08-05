// attack_detect.cpp — see attack_detect.h. Original code for M5PORKCHOP (MIT).
#include "attack_detect.h"
#include <string.h>

namespace attackdet {

namespace {
constexpr uint8_t TYPE_MGMT        = 0;
constexpr uint8_t SUBTYPE_PROBEREQ = 0x04;
constexpr uint8_t SUBTYPE_BEACON   = 0x08;
constexpr uint8_t SUBTYPE_DISASSOC = 0x0A;
constexpr uint8_t SUBTYPE_DEAUTH   = 0x0C;
}

void AttackMonitor::onFrame(uint8_t fcType, uint8_t fcSubtype, const uint8_t* tx) {
    if (fcType != TYPE_MGMT) return;
    switch (fcSubtype) {
        case SUBTYPE_DEAUTH:
            deauth_.fetch_add(1, std::memory_order_relaxed);
            if (tx) for (int i = 0; i < 6; ++i) lastBssid_[i] = tx[i];
            break;
        case SUBTYPE_DISASSOC:
            disassoc_.fetch_add(1, std::memory_order_relaxed);
            if (tx) for (int i = 0; i < 6; ++i) lastBssid_[i] = tx[i];
            break;
        case SUBTYPE_BEACON:
            // Beacon floods (mdk4 etc.) spray beacons from many spoofed BSSIDs,
            // which are almost always locally-administered (addr2[0] bit 0x02).
            // Counting only those keeps normal AP beacons from tripping it.
            if (tx && (tx[0] & 0x02)) beaconRandom_.fetch_add(1, std::memory_order_relaxed);
            break;
        case SUBTYPE_PROBEREQ:
            probe_.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            break;
    }
}

AttackType AttackMonitor::tick(uint32_t nowMs) {
    if (windowStart_ == 0) windowStart_ = nowMs;
    if (nowMs - windowStart_ < 1000) return AttackType::None;
    windowStart_ = nowMs;

    // Snapshot + reset the window counters.
    uint16_t d  = deauth_.exchange(0, std::memory_order_relaxed);
    uint16_t da = disassoc_.exchange(0, std::memory_order_relaxed);
    uint16_t b  = beaconRandom_.exchange(0, std::memory_order_relaxed);
    uint16_t p  = probe_.exchange(0, std::memory_order_relaxed);

    stats_.deauthPerSec   = d;
    stats_.disassocPerSec = da;
    stats_.beaconPerSec   = b;
    stats_.probePerSec    = p;
    for (int i = 0; i < 6; ++i) stats_.lastBssid[i] = lastBssid_[i];

    // Report the most severe threshold crossed (deauth worst -> probe least).
    if (d  >= thr_.deauth)   return AttackType::DeauthFlood;
    if (da >= thr_.disassoc) return AttackType::DisassocFlood;
    if (b  >= thr_.beacon)   return AttackType::BeaconFlood;
    if (p  >= thr_.probe)    return AttackType::ProbeFlood;
    return AttackType::None;
}

void AttackMonitor::reset() {
    deauth_.store(0, std::memory_order_relaxed);
    disassoc_.store(0, std::memory_order_relaxed);
    beaconRandom_.store(0, std::memory_order_relaxed);
    probe_.store(0, std::memory_order_relaxed);
    windowStart_ = 0;
    memset(&stats_, 0, sizeof(stats_));
}

} // namespace attackdet
