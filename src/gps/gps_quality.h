// GPS fix quality constants and inline helpers
// Shared by local GPS (gps.cpp) and C5 GPS (janus_hog.cpp)
#pragma once

#include <cstdint>

namespace GPSQuality {

// Minimum satellites for an acceptable fix (3 = 2D fix, no altitude but lat/lon usable)
constexpr uint8_t  MIN_SATELLITES   = 3;

// Maximum HDOP in centiprecision (800 = HDOP 8.0 ≈ 80m accuracy, acceptable for wardriving)
constexpr uint16_t MAX_HDOP         = 800;

// Maximum age of location data before fix is considered stale
constexpr uint32_t MAX_AGE_MS       = 30000;

// Coasting thresholds — decent fixes earn hold-over credit
constexpr uint8_t  COAST_MIN_SATS   = 5;
constexpr uint16_t COAST_MAX_HDOP   = 500;   // HDOP 5.0
constexpr uint32_t COAST_MS         = 8000;   // 8-second hold-over window

// Returns true if the fix meets minimum quality for use (logging, WiGLE, etc.)
inline bool isFixAcceptable(uint8_t satellites, uint16_t hdop) {
    return (satellites >= MIN_SATELLITES) && (hdop > 0) && (hdop <= MAX_HDOP);
}

// Returns true if the fix is high enough quality to earn coasting credit
inline bool isCoastWorthy(uint8_t satellites, uint16_t hdop) {
    return (satellites >= COAST_MIN_SATS) && (hdop > 0) && (hdop <= COAST_MAX_HDOP);
}

} // namespace GPSQuality
