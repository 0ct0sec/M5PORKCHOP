// flock_proximity.cpp
// ---------------------------------------------------------------------------
// Implementation of the GPS proximity ramp. See flock_proximity.h.
// Original code for M5PORKCHOP (MIT). Map data is ODbL -- flock_map_data.h.
// ---------------------------------------------------------------------------
#include "flock_proximity.h"
#include "flock_map_data.h"
#include "../audio/sfx.h"
#include "../ui/display.h"

#include <Arduino.h>
#include <math.h>

namespace flockprox {
namespace {

// ---- tunables -------------------------------------------------------------
constexpr float kAlertFt = 1000.0f;  // start ticking at this range
constexpr float kCloseFt = 300.0f;   // "it can see you" range

// Gentle-zone grunt spacing, interpolated across kCloseFt..kAlertFt.
constexpr uint32_t kGentleFastMs = 400;   // at kCloseFt (>grunt length, no overlap)
constexpr uint32_t kGentleSlowMs = 1500;  // at kAlertFt
// Inside kCloseFt we loop the full panic squeal; interval must clear its
// ~0.7s length so the queue never overlaps itself.
constexpr uint32_t kCloseIntervalMs = 750;

// The map scan is O(kFlockMapCount) integer compares. GPS only moves at ~1 Hz,
// so re-scanning faster than this buys nothing.
constexpr uint32_t kScanIntervalMs = 500;

// Bounding box prefilter, in units of 1e-7 degrees. 0.05 deg of latitude is
// ~5.5 km and 0.06 deg of longitude is ~5.5 km at this latitude -- comfortably
// wider than kAlertFt, so nothing that could alert gets filtered out.
constexpr int64_t kBboxLatE7 = 500000;
constexpr int64_t kBboxLonE7 = 600000;

constexpr double kEarthRadiusM = 6371000.0;
constexpr double kDegToRad     = 0.017453292519943295;
constexpr float  kMetersToFeet = 3.28084f;

// ---- state ----------------------------------------------------------------
float    g_nearestM  = -1.0f;
bool     g_inRange   = false;
uint32_t g_lastScanMs = 0;
uint32_t g_lastToneMs = 0;

// Nearest camera in metres, or -1 if the table is empty / nothing near.
// Equirectangular approximation: at these distances the error against
// haversine is centimetres, and it costs one cos() instead of three trig calls.
float scanNearest(double lat, double lon) {
    const int32_t latE7 = (int32_t)lround(lat * 1e7);
    const int32_t lonE7 = (int32_t)lround(lon * 1e7);

    double bestM = -1.0;

    for (uint32_t i = 0; i < kFlockMapCount; ++i) {
        const MapPoint& p = kFlockMap[i];

        const int64_t dLatE7 = (int64_t)p.lat_e7 - (int64_t)latE7;
        if (dLatE7 > kBboxLatE7 || dLatE7 < -kBboxLatE7) continue;
        const int64_t dLonE7 = (int64_t)p.lon_e7 - (int64_t)lonE7;
        if (dLonE7 > kBboxLonE7 || dLonE7 < -kBboxLonE7) continue;

        const double pLat = (double)p.lat_e7 * 1e-7;
        const double pLon = (double)p.lon_e7 * 1e-7;

        const double midLat = (pLat + lat) * 0.5 * kDegToRad;
        const double x = (pLon - lon) * kDegToRad * cos(midLat);
        const double y = (pLat - lat) * kDegToRad;
        const double d = sqrt(x * x + y * y) * kEarthRadiusM;

        if (bestM < 0.0 || d < bestM) bestM = d;
    }

    return (float)bestM;
}

} // namespace

void init() {
    g_nearestM   = -1.0f;
    g_inRange    = false;
    g_lastScanMs = 0;
    g_lastToneMs = 0;
}

void update(double lat, double lon, bool valid) {
    const uint32_t now = millis();

    if (!valid) {
        g_nearestM = -1.0f;
        g_inRange  = false;   // regaining a fix inside the zone re-toasts
        return;
    }

    if (now - g_lastScanMs >= kScanIntervalMs) {
        g_lastScanMs = now;
        g_nearestM   = scanNearest(lat, lon);
    }

    if (g_nearestM < 0.0f) { g_inRange = false; return; }

    const float ft = g_nearestM * kMetersToFeet;
    if (ft > kAlertFt) { g_inRange = false; return; }   // out of range: silent

    if (!g_inRange) {                    // rising edge into the alert zone
        g_inRange    = true;
        g_lastToneMs = 0;                // tick immediately
        Display::showToast("ALPR AHEAD");
    }

    uint32_t   interval;
    SFX::Event tick;
    if (ft <= kCloseFt) {
        // Inside 300ft: escalate to a looping panic squeal.
        interval = kCloseIntervalMs;
        tick     = SFX::PIG_SQUEAL;
    } else {
        // 300-1000ft: low double-grunt that speeds up as the camera nears.
        const float t = (ft - kCloseFt) / (kAlertFt - kCloseFt);   // 0..1
        interval = (uint32_t)(kGentleFastMs + t * (float)(kGentleSlowMs - kGentleFastMs));
        tick     = SFX::PIG_GRUNT;
    }

    if (g_lastToneMs == 0 || now - g_lastToneMs >= interval) {
        SFX::play(tick);
        g_lastToneMs = now;
    }
}

float nearestMeters() {
    return g_nearestM;
}

} // namespace flockprox
