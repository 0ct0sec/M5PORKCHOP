// Testable pure functions extracted from core modules
// These functions have no hardware dependencies and can be unit tested
#pragma once

#include <cstdint>
#include <cmath>

// ============================================================================
// XP System - Level Calculations
// From: src/core/xp.cpp
// ============================================================================

// XP thresholds for each level (1-40)
// Level N requires XP_THRESHOLDS[N-1] total XP
static const uint32_t XP_THRESHOLDS[40] = {
    0,       // Level 1: 0 XP
    100,     // Level 2: 100 XP
    300,     // Level 3: 300 XP
    600,     // Level 4
    1000,    // Level 5
    1500,    // Level 6
    2300,    // Level 7
    3400,    // Level 8
    4800,    // Level 9
    6500,    // Level 10
    8500,    // Level 11
    11000,   // Level 12
    14000,   // Level 13
    17500,   // Level 14
    21500,   // Level 15
    26000,   // Level 16
    31000,   // Level 17
    36500,   // Level 18
    42500,   // Level 19
    49000,   // Level 20
    56000,   // Level 21
    64000,   // Level 22
    73000,   // Level 23
    83000,   // Level 24
    94000,   // Level 25
    106000,  // Level 26
    120000,  // Level 27
    136000,  // Level 28
    154000,  // Level 29
    174000,  // Level 30
    197000,  // Level 31
    223000,  // Level 32
    252000,  // Level 33
    284000,  // Level 34
    319000,  // Level 35
    359000,  // Level 36
    404000,  // Level 37
    454000,  // Level 38
    514000,  // Level 39
    600000   // Level 40: 600,000 XP
};

static const uint8_t MAX_LEVEL = 40;

// Calculate level from total XP
// Returns level 1-40
inline uint8_t calculateLevel(uint32_t xp) {
    for (uint8_t i = MAX_LEVEL - 1; i > 0; i--) {
        if (xp >= XP_THRESHOLDS[i]) return i + 1;
    }
    return 1;
}

// Get XP required for a specific level
// Returns 0 for invalid levels
inline uint32_t getXPForLevel(uint8_t level) {
    if (level < 1 || level > MAX_LEVEL) return 0;
    return XP_THRESHOLDS[level - 1];
}

// Calculate XP remaining to next level
// Returns 0 if already at max level
inline uint32_t getXPToNextLevel(uint32_t currentXP) {
    uint8_t level = calculateLevel(currentXP);
    if (level >= MAX_LEVEL) return 0;
    return XP_THRESHOLDS[level] - currentXP;
}

// Calculate progress percentage to next level (0-100)
inline uint8_t getLevelProgress(uint32_t currentXP) {
    uint8_t level = calculateLevel(currentXP);
    if (level >= MAX_LEVEL) return 100;
    
    uint32_t currentLevelXP = XP_THRESHOLDS[level - 1];
    uint32_t nextLevelXP = XP_THRESHOLDS[level];
    uint32_t levelRange = nextLevelXP - currentLevelXP;
    uint32_t progress = currentXP - currentLevelXP;
    
    if (levelRange == 0) return 0;
    return (uint8_t)((progress * 100) / levelRange);
}

// ============================================================================
// Distance Calculations
// From: src/modes/warhog.cpp
// ============================================================================

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Haversine formula for GPS distance calculation
// Returns distance in meters between two lat/lon points
inline double haversineMeters(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371000.0;  // Earth radius in meters
    double dLat = (lat2 - lat1) * M_PI / 180.0;
    double dLon = (lon2 - lon1) * M_PI / 180.0;
    lat1 = lat1 * M_PI / 180.0;
    lat2 = lat2 * M_PI / 180.0;
    
    double a = sin(dLat / 2) * sin(dLat / 2) +
               cos(lat1) * cos(lat2) * sin(dLon / 2) * sin(dLon / 2);
    double c = 2 * atan2(sqrt(a), sqrt(1 - a));
    return R * c;
}

// ============================================================================
// Feature Extraction Helpers
// From: src/ml/features.cpp
// ============================================================================

// Check if MAC address is randomized (locally administered bit set)
// The second bit of the first octet indicates locally administered
inline bool isRandomizedMAC(const uint8_t* mac) {
    return (mac[0] & 0x02) != 0;
}

// Check if MAC is multicast (group bit set)
// The first bit of the first octet indicates multicast
inline bool isMulticastMAC(const uint8_t* mac) {
    return (mac[0] & 0x01) != 0;
}

// Normalize a value using z-score normalization
// Returns 0 if std is too small to avoid division by zero
inline float normalizeValue(float value, float mean, float std) {
    if (std < 0.001f) return 0.0f;
    return (value - mean) / std;
}

// Parse beacon interval from raw 802.11 beacon frame
// Returns default 100 if frame is too short
inline uint16_t parseBeaconInterval(const uint8_t* frame, uint16_t len) {
    if (len < 34) return 100;  // Default beacon interval
    // Beacon interval at offset 32 (after 24 byte header + 8 byte timestamp)
    return frame[32] | (frame[33] << 8);
}

// Parse capability info from raw 802.11 beacon frame
inline uint16_t parseCapability(const uint8_t* frame, uint16_t len) {
    if (len < 36) return 0;
    // Capability at offset 34
    return frame[34] | (frame[35] << 8);
}

// ============================================================================
// Anomaly Scoring
// From: src/ml/inference.cpp
// ============================================================================

// Calculate anomaly score component for signal strength
// Very strong signals (>-30 dBm) are suspicious
inline float anomalyScoreRSSI(int8_t rssi) {
    if (rssi > -30) return 0.3f;
    return 0.0f;
}

// Calculate anomaly score component for beacon interval
// Normal is ~100ms (100 TU), unusual intervals are suspicious
inline float anomalyScoreBeaconInterval(uint16_t interval) {
    if (interval < 50 || interval > 200) return 0.2f;
    return 0.0f;
}

// Calculate anomaly score for open network
inline float anomalyScoreOpenNetwork(bool hasWPA, bool hasWPA2, bool hasWPA3) {
    if (!hasWPA && !hasWPA2 && !hasWPA3) return 0.2f;
    return 0.0f;
}

// Calculate anomaly score for WPS on open network (honeypot pattern)
inline float anomalyScoreWPSHoneypot(bool hasWPS, bool hasWPA, bool hasWPA2, bool hasWPA3) {
    if (hasWPS && !hasWPA && !hasWPA2 && !hasWPA3) return 0.25f;
    return 0.0f;
}

// Calculate anomaly score for VHT without HT (inconsistent capabilities)
inline float anomalyScoreInconsistentPHY(bool hasVHT, bool hasHT) {
    if (hasVHT && !hasHT) return 0.2f;
    return 0.0f;
}

// Calculate anomaly score for beacon jitter (high jitter = software AP)
inline float anomalyScoreBeaconJitter(float jitter) {
    if (jitter > 10.0f) return 0.15f;
    return 0.0f;
}

// Calculate anomaly score for missing vendor IEs (real routers have many)
inline float anomalyScoreMissingVendorIEs(uint8_t vendorIECount) {
    if (vendorIECount < 2) return 0.1f;
    return 0.0f;
}

// ============================================================================
// Achievement Bitfield Operations
// From: src/core/xp.h
// ============================================================================

// Check if an achievement is unlocked
inline bool hasAchievement(uint64_t achievements, uint64_t achievementBit) {
    return (achievements & achievementBit) != 0;
}

// Unlock an achievement
inline uint64_t unlockAchievement(uint64_t achievements, uint64_t achievementBit) {
    return achievements | achievementBit;
}

// Count number of unlocked achievements
inline uint8_t countAchievements(uint64_t achievements) {
    uint8_t count = 0;
    while (achievements) {
        count += achievements & 1;
        achievements >>= 1;
    }
    return count;
}
