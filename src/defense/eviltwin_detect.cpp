// eviltwin_detect.cpp — see eviltwin_detect.h. Original code for M5PORKCHOP (MIT).
#include "eviltwin_detect.h"
#include <string.h>

namespace eviltwin {

int EvilTwinMonitor::find(const char* ssid) const {
    for (uint8_t i = 0; i < count_; ++i) {
        if (strncmp(table_[i].ssid, ssid, 32) == 0) return i;
    }
    return -1;
}

bool EvilTwinMonitor::observe(const char* ssid, const uint8_t* bssid, Sec sec, TwinHit& out) {
    if (!ssid || !bssid || ssid[0] == '\0') return false;   // ignore hidden/empty

    int idx = find(ssid);
    if (idx < 0) {
        if (count_ >= kMaxSsids) return false;   // table full: stop learning (bounded)
        Entry& e = table_[count_++];
        memset(&e, 0, sizeof(e));
        strncpy(e.ssid, ssid, 32);
        memcpy(e.bssid0, bssid, 6);
        e.sec0 = sec;
        e.haveSecond = false;
        e.flagged = false;
        return false;
    }

    Entry& e = table_[idx];
    if (memcmp(e.bssid0, bssid, 6) == 0) {
        // Same radio again — refine security if we learn something stronger.
        if (e.sec0 == Sec::Unknown) e.sec0 = sec;
        return false;
    }
    if (e.haveSecond && memcmp(e.bssid1, bssid, 6) == 0) {
        return false;   // already know this second radio
    }

    // A different BSSID for a known SSID. Alert only on a security MISMATCH
    // (the classic open-clone of an encrypted network); matching security is
    // ordinary roaming/mesh and stays quiet.
    bool mismatch = (e.sec0 != Sec::Unknown && sec != Sec::Unknown && sec != e.sec0);

    if (!e.haveSecond) {
        e.haveSecond = true;
        memcpy(e.bssid1, bssid, 6);
    }

    if (mismatch && !e.flagged) {
        e.flagged = true;
        if (twinCount_ < 255) twinCount_++;
        out.isTwin = true;
        strncpy(out.ssid, ssid, 32);
        out.ssid[32] = '\0';
        memcpy(out.baselineBssid, e.bssid0, 6);
        memcpy(out.impostorBssid, bssid, 6);
        out.baselineSec = e.sec0;
        out.impostorSec = sec;
        return true;
    }
    return false;
}

void EvilTwinMonitor::reset() {
    count_ = 0;
    twinCount_ = 0;
}

} // namespace eviltwin
