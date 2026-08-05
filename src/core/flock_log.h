// flock_log.h
// ---------------------------------------------------------------------------
// Dual-sink logging for Flock detections in M5PORKCHOP's WARHOG mode.
//
// Confirmed design:
//   * Every qualifying detection is written to BOTH sinks:
//       1. the existing WiGLE wardrive export  -> helps wigle.net
//       2. a separate flock.csv                -> ready to submit to deflock.me
//   * Default alert/log threshold is Confidence::Medium (set on FlockDetect).
//
// Data-hygiene note: the WiGLE row carries the AP as a normal WiFi observation
// with its real (usually empty) SSID. We deliberately do NOT inject a "[FLOCK]"
// marker into the WiGLE SSID/AuthMode fields — that would pollute the public
// WiGLE dataset with synthetic values. The Flock *classification* lives in
// flock.csv, which is the correct place for it. deflock.me gets the rich
// record; WiGLE gets a clean observation. Both win.
//
// Header-only, snprintf-based, no dynamic allocation. Original code (MIT).
// ---------------------------------------------------------------------------
#pragma once
#include <stdint.h>
#include <stdio.h>
#include "flock_detect.h"

namespace flockdet {

// Minimal GPS fix the caller fills from WARHOG's existing GPS source.
// utc is ISO-8601, e.g. "2026-02-14T18:03:07Z".
struct GpsFix {
    double lat = 0.0;
    double lon = 0.0;
    float  altM = 0.0f;
    float  accM = 0.0f;      // horizontal accuracy in meters (HDOP-derived)
    char   utc[24] = {0};
    bool   valid = false;
};

inline const char* kindStr(DeviceKind k) {
    switch (k) {
        case DeviceKind::FlockCamera:        return "flock_camera";
        case DeviceKind::RavenDetector:      return "raven_detector";
        case DeviceKind::SuspectSurveillance:return "suspect";
        default:                             return "none";
    }
}

inline const char* confStr(Confidence c) {
    switch (c) {
        case Confidence::High:   return "high";
        case Confidence::Medium: return "medium";
        case Confidence::Low:    return "low";
        default:                 return "none";
    }
}

inline void macStr(const uint8_t* m, char out[18]) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0], m[1], m[2], m[3], m[4], m[5]);
}

// ---- flock.csv --------------------------------------------------------------
// One clean, deflock.me-friendly schema. Write flockCsvHeader() once when the
// file is first created, then flockCsvRow() per detection.
inline const char* flockCsvHeader() {
    return "utc,lat,lon,alt_m,acc_m,mac,kind,confidence,rssi,channel,reason\n";
}

// Returns bytes written (excluding NUL), or 0 on truncation/invalid input.
inline int flockCsvRow(char* buf, int bufLen, const Detection& d, const GpsFix& g) {
    if (!buf || bufLen <= 0 || !d.hit()) return 0;
    char mac[18]; macStr(d.mac, mac);
    int n = snprintf(buf, bufLen,
        "%s,%.7f,%.7f,%.1f,%.1f,%s,%s,%s,%d,%u,%s\n",
        g.valid ? g.utc : "",
        g.lat, g.lon, (double)g.altM, (double)g.accM,
        mac, kindStr(d.kind), confStr(d.confidence),
        (int)d.rssi, (unsigned)d.channel, d.reason ? d.reason : "");
    return (n > 0 && n < bufLen) ? n : 0;
}

// ---- WiGLE row --------------------------------------------------------------
// IMPORTANT: match this column order to PORKCHOP's existing WigleWifi writer so
// rows append consistently under the same header. This is the common WiGLE 1.6
// WiFi column order; adjust if WARHOG's header differs.
//   MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,
//   CurrentLongitude,AltitudeMeters,AccuracyMeters,Type
inline int wigleRow(char* buf, int bufLen, const Detection& d, const GpsFix& g) {
    if (!buf || bufLen <= 0 || !d.hit()) return 0;
    char mac[18]; macStr(d.mac, mac);
    int n = snprintf(buf, bufLen,
        "%s,,,%s,%u,%d,%.7f,%.7f,%.1f,%.1f,WIFI\n",
        mac,
        g.valid ? g.utc : "",
        (unsigned)d.channel, (int)d.rssi,
        g.lat, g.lon, (double)g.altM, (double)g.accM);
    return (n > 0 && n < bufLen) ? n : 0;
}

} // namespace flockdet
