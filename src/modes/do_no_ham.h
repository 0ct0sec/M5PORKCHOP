// DO NO HAM Mode - Passive WiFi Reconnaissance
// "BRAVO 6, GOING DARK"
#pragma once

#include <Arduino.h>
#include <esp_wifi.h>
#include <vector>
#include <atomic>
#include "oink.h"  // Reuse DetectedNetwork, CapturedPMKID, CapturedHandshake
#include "../core/network_recon.h"

// DNH-specific constants
static const size_t DNH_MAX_NETWORKS = 100;
static const size_t DNH_MAX_PMKIDS = 50;
static const size_t DNH_MAX_HANDSHAKES = 25;
static const uint32_t DNH_STALE_TIMEOUT = 30000;  // 30s
static const uint16_t DNH_HOP_INTERVAL = 200;     // Legacy default (now adaptive)
static const uint16_t DNH_DWELL_TIME = 300;       // 300ms dwell for SSID

// Adaptive state machine parameters
static const uint16_t HOP_BASE_PRIMARY = 250;      // Ch 1,6,11 baseline
static const uint16_t HOP_BASE_SECONDARY = 150;    // Other channels baseline
static const uint16_t HOP_MIN = 120;               // Dead channel minimum
static const uint16_t HUNT_DURATION = 600;         // EAPOL burst camp time
static const uint8_t BUSY_THRESHOLD = 5;           // Beacons = "busy"
static const uint8_t DEAD_STREAK_LIMIT = 3;        // Visits = "dead"
static const uint32_t HUNT_COOLDOWN_MS = 10000;    // 10s re-hunt delay
static const uint8_t MAX_INCOMPLETE_HS = 20;       // Track incomplete handshakes
static const uint32_t INCOMPLETE_HS_TIMEOUT = 60000; // 60s age-out
static const uint32_t STATS_DECAY_INTERVAL = 120000; // 2min reset

// DNH State Machine - adaptive timing based on channel activity
enum class DNHState : uint8_t {
    HOPPING = 0,   // Adaptive channel hopping
    DWELLING,      // Paused to catch beacon for SSID backfill
    HUNTING,       // High activity detected, extended dwell for handshake
    IDLE_SWEEP     // All channels dead, fast sweeps
};

// Channel activity tracking
struct ChannelStats {
    uint8_t channel;
    uint8_t beaconCount;       // Last visit
    uint8_t eapolCount;        // Last visit
    uint32_t lastActivity;     // millis()
    uint8_t priority;          // 0-255 (100 = baseline)
    uint8_t deadStreak;        // Consecutive dead visits
    uint16_t lifetimeBeacons;  // Total for stats
};

// Incomplete handshake tracking for revisit
struct IncompleteHS {
    uint8_t bssid[6];
    uint8_t capturedMask;      // Bits 0-3 for M1-M4
    uint8_t channel;
    uint32_t lastSeen;
};

class DoNoHamMode {
public:
    static void init();
    static void start();
    static void stop();
    static void update();
    static bool isRunning() { return running; }
    
    
    // Channel info for display
    static uint8_t getCurrentChannel() { return currentChannel; }
    
    // Stats for display
    static size_t getNetworkCount() { return NetworkRecon::getNetworkCount(); }
    static size_t getPMKIDCount() { return pmkids.size(); }
    static size_t getHandshakeCount() { return handshakes.size(); }
    
    // Packet callback for NetworkRecon
    static void promiscuousCallback(const wifi_promiscuous_pkt_t* pkt, wifi_promiscuous_pkt_type_t type);
    
    // Frame handlers (called from promiscuousCallback)
    static void handleBeacon(const uint8_t* frame, uint16_t len, int8_t rssi);
    static void handleProbeResponse(const uint8_t* frame, uint16_t len, int8_t rssi);
    static void handleEAPOL(const uint8_t* frame, uint16_t len, int8_t rssi);
    
    // Stress test injection (no RF)
    static void injectTestNetwork(const uint8_t* bssid, const char* ssid, uint8_t channel, int8_t rssi, wifi_auth_mode_t authmode, bool hasPMF);
    
private:
    static std::atomic<bool> running;
    static DNHState state;
    static uint8_t currentChannel;
    static uint8_t channelIndex;
    static uint32_t dwellStartTime;
    static volatile bool dwellResolved;
    
    // Data storage (uses shared networks from NetworkRecon)
    // networks vector is now in NetworkRecon
    static std::vector<CapturedPMKID> pmkids;
    static std::vector<CapturedHandshake> handshakes;
    
    // Adaptive state machine
    static ChannelStats channelStats[13];
    static std::vector<IncompleteHS> incompleteHandshakes;
    static uint32_t huntStartTime;
    static uint32_t lastHuntTime;          // Per-channel cooldown
    static uint8_t lastHuntChannel;
    static uint32_t lastStatsDecay;
    static uint8_t lastCycleActivity;      // Total beacons in last cycle
    static uint32_t adaptiveDwellUntil;
    
    // Channel hopping
    static void startDwell();
    static uint16_t getAdaptiveHopDelay();
    static void decayChannelStats();
    static bool isPrimaryChannel(uint8_t ch);
    static bool checkHuntingTrigger();     // Returns true if entered HUNTING
    static void checkIdleSweep();
    static void trackIncompleteHandshake(const uint8_t* bssid, uint8_t mask, uint8_t ch);
    static void pruneIncompleteHandshakes();
    
    // Cleanup (network cleanup handled by NetworkRecon)
    static void saveAllPMKIDs();
    static void saveAllHandshakes();
    
    // Network lookup
    static int findNetwork(const uint8_t* bssid);
    static int findOrCreatePMKID(const uint8_t* bssid);
    static int findOrCreateHandshake(const uint8_t* bssid, const uint8_t* station);
};
