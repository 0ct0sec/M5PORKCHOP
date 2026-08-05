// flock_detect.cpp
// ---------------------------------------------------------------------------
// Implementation of the passive surveillance detector. See flock_detect.h.
// Original code for M5PORKCHOP (MIT). Detection *methodology* references the
// public work of @NitekryDPaul, DeFlockJoplin, @wgreenberg and deflock.me.
// ---------------------------------------------------------------------------
#include "flock_detect.h"
#include "flock_signatures.h"

namespace flockdet {

// Forward declarations (defined at bottom of file).
bool containsCI(const char* hay, uint8_t hayLen, const char* needle);
bool uuid128Eq(const uint8_t* a, const uint8_t* b);

// --- 802.11 header helpers -------------------------------------------------
// MAC header layout (no addr4):
//   [0..1] frame control   [2..3] duration
//   [4..9] addr1 (RX)      [10..15] addr2 (TX)   [16..21] addr3
//   [22..23] seq ctrl      [24..] body
namespace {
inline uint8_t fcType(const uint8_t* p)    { return (p[0] >> 2) & 0x03; }
inline uint8_t fcSubtype(const uint8_t* p) { return (p[0] >> 4) & 0x0F; }
constexpr uint8_t TYPE_MGMT       = 0;
constexpr uint8_t SUBTYPE_PROBEREQ = 4;

inline bool ouiEq(const uint8_t* mac, const uint8_t* oui) {
    return mac[0] == oui[0] && mac[1] == oui[1] && mac[2] == oui[2];
}

// Group bit (I/G) set => broadcast or multicast destination, never a real
// device address. addr1 is broadcast in every beacon/probe-request, so without
// this guard the receiver-address trick would be pure noise.
// NOTE: the locally-administered bit (0x02) is deliberately NOT filtered --
// 82:6b:f2 is itself locally-administered and is an explicitly listed prefix.
inline bool isGroupAddr(const uint8_t* mac) { return (mac[0] & 0x01) != 0; }

// A wildcard (broadcast) probe request has an SSID element (id 0) of length 0
// as the first tagged parameter. On its own this is a *very* common frame, so
// it is only ever used here as corroboration, never as a standalone trigger.
bool isWildcardProbeReq(const uint8_t* p, uint16_t len) {
    if (fcType(p) != TYPE_MGMT || fcSubtype(p) != SUBTYPE_PROBEREQ) return false;
    if (len < 26) return false;          // need header + SSID tag header
    const uint8_t* body = p + 24;
    return body[0] == 0x00 && body[1] == 0x00;  // SSID element, length 0
}
} // namespace

int FlockDetect::matchOui(const uint8_t* mac) {
    for (uint8_t i = 0; i < kFlockOuiCount; ++i) {
        if (ouiEq(mac, kFlockOuis[i].oui)) return i;
    }
    return -1;
}

Detection FlockDetect::inspectWifiFrame(const uint8_t* p, uint16_t len,
                                        int8_t rssi, uint8_t channel) const {
    Detection d;
    if (!p || len < 24) return d;

    const uint8_t* addr1 = p + 4;    // receiver  (the sleeping-camera trick)
    const uint8_t* addr2 = p + 10;   // transmitter

    int i1 = isGroupAddr(addr1) ? -1 : matchOui(addr1);
    int i2 = isGroupAddr(addr2) ? -1 : matchOui(addr2);
    if (i1 < 0 && i2 < 0) return d;  // no OUI of interest anywhere

    // Prefer the addr1/receiver match — that's the signature that catches
    // cameras which aren't actively transmitting.
    bool viaAddr1 = (i1 >= 0);
    int  idx      = viaAddr1 ? i1 : i2;
    const uint8_t* mac = viaAddr1 ? addr1 : addr2;
    OuiClass cls = kFlockOuis[idx].cls;

    bool wildcard = isWildcardProbeReq(p, len);

    // ---- confidence scoring ------------------------------------------------
    Confidence conf = Confidence::Low;
    const char* why = "esp OUI (weak)";

    if (cls == OuiClass::FLOCK_LINKED) {
        // Flock-specific OUI: strong on its own, higher with corroboration.
        conf = wildcard ? Confidence::High : Confidence::Medium;
        why  = wildcard ? "flock OUI + wildcard probe" : "flock OUI";
    } else {
        // Generic Espressif OUI: weak. Needs the receiver-address trick and/or
        // a correlated wildcard probe to rise above noise.
        if (viaAddr1 && wildcard) { conf = Confidence::Medium; why = "esp OUI in addr1 + wildcard probe"; }
        else if (viaAddr1)        { conf = Confidence::Low;    why = "esp OUI in addr1 (receiver)"; }
        else if (wildcard)        { conf = Confidence::Low;    why = "esp OUI + wildcard probe"; }
        // else stays Low / "esp OUI (weak)"
    }

    if (conf < threshold_) return d;    // below caller's alert bar -> ignore

    d.kind       = DeviceKind::FlockCamera;   // WiFi-side is Flock-family
    d.confidence = conf;
    d.rssi       = rssi;
    d.channel    = channel;
    d.reason     = why;
    for (int k = 0; k < 6; ++k) d.mac[k] = mac[k];
    return d;
}

Detection FlockDetect::inspectBleAdv(const uint8_t* addr, const uint8_t* adv,
                                     uint8_t advLen, int8_t rssi) const {
    Detection d;
    if (!addr || !adv) return d;

    bool  mfrMatch = false;
    bool  nameMatch = false;
    bool  ravenMatch = false;

    // Walk the AD structures: [len][type][data...] repeated.
    uint8_t i = 0;
    while (i + 1 < advLen) {
        uint8_t fieldLen = adv[i];
        if (fieldLen == 0 || i + 1 + fieldLen > advLen) break;
        uint8_t type = adv[i + 1];
        const uint8_t* data = &adv[i + 2];
        uint8_t dataLen = fieldLen - 1;

        // 0xFF: Manufacturer Specific Data -> first 2 bytes are company ID (LE).
        if (type == 0xFF && dataLen >= 2) {
            uint16_t company = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
            if (company == kFlockBleCompanyId) mfrMatch = true;
        }

        // 0x08/0x09: Shortened / Complete Local Name -> substring hints.
        if ((type == 0x08 || type == 0x09) && dataLen > 0) {
            for (uint8_t h = 0; h < kFlockBleNameHintCount; ++h) {
                if (containsCI((const char*)data, dataLen, kFlockBleNameHints[h])) {
                    nameMatch = true; break;
                }
            }
        }

        // 0x06/0x07: Incomplete/Complete 128-bit Service UUIDs -> Raven check.
        if ((type == 0x06 || type == 0x07) && dataLen >= 16) {
            for (uint8_t u = 0; u < kRavenServiceUuidCount; ++u) {
                if (uuid128Eq(data, kRavenServiceUuids[u].bytes)) { ravenMatch = true; break; }
            }
        }

        i += 1 + fieldLen;
    }

    if (!(mfrMatch || nameMatch || ravenMatch)) return d;

    // Scoring: the XUNTONG manufacturer ID is a strong Flock/Raven tell; a name
    // hint corroborates; a Raven service UUID is a definitive Raven classify.
    if (ravenMatch) {
        d.kind = DeviceKind::RavenDetector;
        d.confidence = Confidence::High;
        d.reason = "raven BLE service UUID";
    } else if (mfrMatch && nameMatch) {
        d.kind = DeviceKind::FlockCamera;
        d.confidence = Confidence::High;
        d.reason = "BLE mfr 0x09C8 + name";
    } else if (mfrMatch) {
        d.kind = DeviceKind::FlockCamera;
        d.confidence = Confidence::Medium;
        d.reason = "BLE mfr 0x09C8";
    } else { // nameMatch only
        d.kind = DeviceKind::SuspectSurveillance;
        d.confidence = Confidence::Low;
        d.reason = "BLE name hint only";
    }

    if (d.confidence < threshold_) { d.kind = DeviceKind::None; return d; }

    d.rssi = rssi;
    d.channel = 0;
    for (int k = 0; k < 6; ++k) d.mac[k] = addr[k];
    return d;
}

Detection FlockDetect::inspectScanResult(const uint8_t* bssid, const char* ssid,
                                         int8_t rssi, uint8_t channel) const {
    Detection d;
    if (!bssid) return d;

    int idx = matchOui(bssid);

    bool nameHit = false;
    if (ssid) {
        uint8_t len = 0;
        while (ssid[len] && len < 64) ++len;
        for (uint8_t h = 0; h < kFlockBleNameHintCount; ++h) {
            if (containsCI(ssid, len, kFlockBleNameHints[h])) { nameHit = true; break; }
        }
    }

    if (idx < 0 && !nameHit) return d;

    Confidence conf = Confidence::Low;
    const char* why = "ssid name hint";
    DeviceKind  kind = DeviceKind::SuspectSurveillance;

    if (idx >= 0) {
        kind = DeviceKind::FlockCamera;
        if (kFlockOuis[idx].cls == OuiClass::FLOCK_LINKED) {
            conf = nameHit ? Confidence::High : Confidence::Medium;
            why  = nameHit ? "flock OUI + SSID" : "flock OUI (AP)";
        } else { // generic Espressif OUI on an AP: weak unless the SSID agrees
            conf = nameHit ? Confidence::Medium : Confidence::Low;
            why  = nameHit ? "esp OUI + flock SSID" : "esp OUI (AP, weak)";
        }
    }

    if (conf < threshold_) return d;   // below the alert bar -> not reported

    d.kind       = kind;
    d.confidence = conf;
    d.rssi       = rssi;
    d.channel    = channel;
    d.reason     = why;
    for (int k = 0; k < 6; ++k) d.mac[k] = bssid[k];
    return d;
}

// --- small helpers (kept out of the header for host-side testability) ------
// case-insensitive substring search over a non-null-terminated buffer
static inline char lc(char c){ return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
bool containsCI(const char* hay, uint8_t hayLen, const char* needle) {
    if (!needle || !*needle) return false;
    for (uint8_t s = 0; s < hayLen; ++s) {
        uint8_t k = 0;
        while (needle[k] && (s + k) < hayLen &&
               lc(hay[s + k]) == lc(needle[k])) ++k;
        if (needle[k] == '\0') return true;
    }
    return false;
}

bool uuid128Eq(const uint8_t* a, const uint8_t* b) {
    for (int k = 0; k < 16; ++k) if (a[k] != b[k]) return false;
    return true;
}

} // namespace flockdet
