// eviltwin_detect.h
// ---------------------------------------------------------------------------
// Passive evil-twin / rogue-AP detection for the DEFENSIVE SUITE ("FAKE BACON").
//
// The high-confidence tell of a Wi-Fi Pineapple / karma clone is a network name
// you trust suddenly appearing on a DIFFERENT radio with WEAKER security -- your
// WPA2 "CoffeeShop" now also beaconing OPEN from a new BSSID. We self-baseline:
// the first BSSID seen for an SSID sets the expected security, and a later BSSID
// for that SSID with a security *mismatch* is flagged EVIL TWIN. Two BSSIDs with
// matching security are treated as ordinary roaming/mesh and NOT alerted, which
// keeps false positives off enterprise and carrier networks.
//
// RX-only. Standard 802.11 beacon/probe-response fields; no proprietary data.
// Framework-agnostic core (host-testable), like flock_detect / attack_detect.
// ---------------------------------------------------------------------------
#pragma once
#include <stdint.h>

namespace eviltwin {

enum class Sec : uint8_t { Unknown = 0, Open, Encrypted };

struct TwinHit {
    bool        isTwin = false;
    char        ssid[33] = {0};
    uint8_t     baselineBssid[6] = {0};  // first BSSID seen (the real one, presumably)
    uint8_t     impostorBssid[6] = {0};  // the mismatched newcomer
    Sec         baselineSec = Sec::Unknown;
    Sec         impostorSec  = Sec::Unknown;
};

class EvilTwinMonitor {
public:
    // Feed one AP observation (from a parsed beacon / probe response). Returns
    // true and fills `out` exactly once, on the edge a new evil-twin is found.
    // Hidden/empty SSIDs are ignored.
    bool observe(const char* ssid, const uint8_t* bssid, Sec sec, TwinHit& out);

    void reset();
    uint8_t twinCount() const { return twinCount_; }

private:
    static const uint8_t kMaxSsids = 48;
    struct Entry {
        char    ssid[33];
        uint8_t bssid0[6];
        Sec     sec0;
        bool    haveSecond;
        uint8_t bssid1[6];
        bool    flagged;
    };
    Entry   table_[kMaxSsids];
    uint8_t count_ = 0;
    uint8_t twinCount_ = 0;

    int find(const char* ssid) const;
};

} // namespace eviltwin
