// DO NO HAM Mode implementation
// "BRAVO 6, GOING DARK"
// Passive WiFi reconnaissance - no attacks, just listening

#include "donoham.h"
#include <M5Unified.h>
#include <WiFi.h>
#include "../core/config.h"
#include "../core/sdlog.h"
#include "../core/xp.h"
#include "../core/wsl_bypasser.h"
#include "../ui/display.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include <SD.h>

// PCAP file format structures (same as OINK for WPA-SEC compatibility)
#pragma pack(push, 1)
struct DNH_PCAPHeader {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t linktype;
};

struct DNH_PCAPPacketHeader {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
};
#pragma pack(pop)

// Minimal radiotap header (8 bytes) - required for WPA-SEC
static const uint8_t DNH_RADIOTAP_HEADER[] = {
    0x00,       // Header revision
    0x00,       // Header pad
    0x08, 0x00, // Header length (8, little-endian)
    0x00, 0x00, 0x00, 0x00  // Present flags (no optional fields)
};

// Static member initialization
bool DoNoHamMode::running = false;
DNHState DoNoHamMode::state = DNHState::HOPPING;
uint8_t DoNoHamMode::currentChannel = 1;
uint8_t DoNoHamMode::channelIndex = 0;
uint32_t DoNoHamMode::lastHopTime = 0;
uint32_t DoNoHamMode::dwellStartTime = 0;
bool DoNoHamMode::dwellResolved = false;

std::vector<DetectedNetwork> DoNoHamMode::networks;
std::vector<CapturedPMKID> DoNoHamMode::pmkids;
std::vector<CapturedHandshake> DoNoHamMode::handshakes;

// Adaptive state machine
ChannelStats DoNoHamMode::channelStats[13] = {};
std::vector<IncompleteHS> DoNoHamMode::incompleteHandshakes;
uint32_t DoNoHamMode::huntStartTime = 0;
uint32_t DoNoHamMode::lastHuntTime = 0;
uint8_t DoNoHamMode::lastHuntChannel = 0;
uint32_t DoNoHamMode::lastStatsDecay = 0;
uint8_t DoNoHamMode::lastCycleActivity = 0;

// Guard flag for race condition prevention
static volatile bool dnhBusy = false;

// Single-slot deferred network add (same pattern as OINK)
static volatile bool pendingNetworkAdd = false;
static DetectedNetwork pendingNetwork;

// Single-slot deferred PMKID create
static volatile bool pendingPMKIDCreateReady = false;
static volatile bool pendingPMKIDCreateBusy = false;
struct PendingPMKIDCreate {
    uint8_t bssid[6];
    uint8_t station[6];
    uint8_t pmkid[16];
    char ssid[33];
    uint8_t channel;
};
static PendingPMKIDCreate pendingPMKIDCreate;

// Single-slot deferred handshake frame add (now stores all 4 frames)
static volatile bool pendingHandshakeAdd = false;
static volatile bool pendingHandshakeBusy = false;
struct PendingHandshakeFrame {
    uint8_t bssid[6];
    uint8_t station[6];
    uint8_t messageNum;  // DEPRECATED - kept for compatibility
    EAPOLFrame frames[4];  // Store all 4 EAPOL frames (M1-M4)
    uint8_t capturedMask;  // Bitmask: bit0=M1, bit1=M2, bit2=M3, bit3=M4
};
static PendingHandshakeFrame pendingHandshake;

// Handshake capture event for UI
static volatile bool pendingHandshakeCapture = false;
static char pendingHandshakeSSID[33] = {0};

// Deferred save flag - set during update(), processed in stop() after promiscuous disabled
// Avoids SD/WiFi SPI bus contention that can cause crashes
static volatile bool pendingSaveFlag = false;

// Channel order: 1, 6, 11 first (non-overlapping), then fill in
static const uint8_t CHANNEL_ORDER[] = {1, 6, 11, 2, 7, 12, 3, 8, 13, 4, 9, 5, 10};

// Timing
static uint32_t lastCleanupTime = 0;
static uint32_t lastSaveTime = 0;
static uint32_t lastMoodTime = 0;

void DoNoHamMode::init() {
    Serial.println("[DNH] Initialized");
}

void DoNoHamMode::start() {
    if (running) return;
    
    Serial.println("[DNH] Starting passive mode");
    SDLog::log("DNH", "Starting passive mode");
    
    // Clear previous session data
    networks.clear();
    networks.shrink_to_fit();
    pmkids.clear();
    pmkids.shrink_to_fit();
    handshakes.clear();
    handshakes.shrink_to_fit();
    incompleteHandshakes.clear();
    incompleteHandshakes.shrink_to_fit();
    
    // Initialize channel stats
    for (int i = 0; i < 13; i++) {
        channelStats[i].channel = CHANNEL_ORDER[i];
        channelStats[i].beaconCount = 0;
        channelStats[i].eapolCount = 0;
        channelStats[i].lastActivity = 0;
        channelStats[i].priority = 100;  // Baseline
        channelStats[i].deadStreak = 0;
        channelStats[i].lifetimeBeacons = 0;
    }
    
    // Reset state
    state = DNHState::HOPPING;
    channelIndex = 0;
    currentChannel = CHANNEL_ORDER[0];
    lastHopTime = millis();
    lastCleanupTime = millis();
    lastSaveTime = millis();
    lastMoodTime = millis();
    lastStatsDecay = millis();
    lastCycleActivity = 0;
    huntStartTime = 0;
    lastHuntTime = 0;
    lastHuntChannel = 0;
    dwellResolved = false;
    
    // Reset deferred flags
    pendingNetworkAdd = false;
    pendingPMKIDCreateReady = false;
    pendingPMKIDCreateBusy = false;
    pendingHandshakeAdd = false;
    pendingHandshakeBusy = false;
    pendingHandshakeCapture = false;
    
    // Randomize MAC if configured
    if (Config::wifi().randomizeMAC) {
        WSLBypasser::randomizeMAC();
    }
    
    // Initialize WiFi in promiscuous mode
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(50);
    
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    esp_wifi_start();
    delay(50);
    
    // Set channel
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    
    // Enable promiscuous mode with shared callback (OINK's callback dispatches to us)
    esp_wifi_set_promiscuous_rx_cb(OinkMode::promiscuousCallback);
    esp_wifi_set_promiscuous(true);
    
    running = true;
    
    // UI feedback
    Display::showToast("PEACEFUL VIBES - NO TROUBLE TODAY");
    Avatar::setState(AvatarState::NEUTRAL);  // Calm, passive state
    Mood::onPassiveRecon(networks.size(), currentChannel);
    
    Serial.printf("[DNH] Started on channel %d\n", currentChannel);
}

void DoNoHamMode::startSeamless() {
    if (running) return;
    
    Serial.println("[DNH] Seamless start (preserving WiFi state)");
    SDLog::log("DNH", "Seamless start");
    
    // DON'T clear vectors - let old data age out naturally
    // DON'T restart promiscuous mode - already running
    // DON'T reset channel - preserve current
    
    // Reset state machine
    state = DNHState::HOPPING;
    lastHopTime = millis();
    lastCleanupTime = millis();
    lastSaveTime = millis();
    lastMoodTime = millis();
    dwellResolved = false;
    
    // Reset deferred flags
    pendingNetworkAdd = false;
    pendingPMKIDCreateReady = false;
    pendingPMKIDCreateBusy = false;
    pendingHandshakeAdd = false;
    pendingHandshakeBusy = false;
    pendingHandshakeCapture = false;
    
    running = true;
    
    // UI feedback (toast already shown by D key handler in porkchop.cpp)
    Avatar::setState(AvatarState::NEUTRAL);  // Calm, passive state
    Mood::onPassiveRecon(networks.size(), currentChannel);
}

void DoNoHamMode::stop() {
    if (!running) return;
    
    Serial.println("[DNH] Stopping");
    SDLog::log("DNH", "Stopping");
    
    running = false;
    
    // Disable promiscuous mode
    esp_wifi_set_promiscuous(false);
    
    // Process any deferred XP saves now that WiFi is off
    XP::processPendingSave();
    
    // Process deferred capture saves now that WiFi is off (SPI bus safe)
    pendingSaveFlag = false;  // Clear flag before processing
    saveAllPMKIDs();
    saveAllHandshakes();
    
    // Free per-handshake beacon memory to prevent leaks
    for (auto& hs : handshakes) {
        if (hs.beaconData) {
            free(hs.beaconData);
            hs.beaconData = nullptr;
        }
    }
    
    // Clear vectors
    dnhBusy = true;
    networks.clear();
    networks.shrink_to_fit();
    pmkids.clear();
    pmkids.shrink_to_fit();
    handshakes.clear();
    handshakes.shrink_to_fit();
    dnhBusy = false;
    
    // Reset deferred flags
    pendingNetworkAdd = false;
    pendingPMKIDCreateReady = false;
    pendingPMKIDCreateBusy = false;
    pendingHandshakeAdd = false;
    pendingHandshakeBusy = false;
    pendingHandshakeCapture = false;
    
    Serial.println("[DNH] Stopped");
}

void DoNoHamMode::stopSeamless() {
    if (!running) return;
    
    Serial.println("[DNH] Seamless stop (preserving WiFi state)");
    SDLog::log("DNH", "Seamless stop");
    
    running = false;
    dnhBusy = false;  // Reset busy flag for clean handoff
    
    // DON'T disable promiscuous mode - OINK will take over
    // DON'T clear vectors - let them die naturally
    // DON'T save to SD - promiscuous still active, SPI bus contention risk
    // Data remains in RAM, will be saved when mode fully exits via stop()
    pendingSaveFlag = true;  // Mark for save when WiFi eventually stops
    
    // Free beacon memory since OINK will recapture its own
    for (auto& hs : handshakes) {
        if (hs.beaconData) {
            free(hs.beaconData);
            hs.beaconData = nullptr;
        }
    }
}

void DoNoHamMode::update() {
    if (!running) return;
    
    uint32_t now = millis();
    
    // Set busy flag for race protection
    dnhBusy = true;
    
    // Process deferred network add
    if (pendingNetworkAdd) {
        if (networks.size() < DNH_MAX_NETWORKS) {
            // Check if already exists
            int idx = findNetwork(pendingNetwork.bssid);
            if (idx >= 0) {
                // Update existing
                networks[idx].rssi = pendingNetwork.rssi;
                networks[idx].lastSeen = pendingNetwork.lastSeen;
                networks[idx].beaconCount++;
                // Backfill SSID if we didn't have it before (critical for PMKID save)
                if (networks[idx].ssid[0] == 0 && pendingNetwork.ssid[0] != 0) {
                    strncpy(networks[idx].ssid, pendingNetwork.ssid, 32);
                    networks[idx].ssid[32] = 0;
                    Serial.printf("[DNH] SSID backfilled for existing network: %s\n", networks[idx].ssid);
                }
            } else {
                // Add new
                networks.push_back(pendingNetwork);
                XP::addXP(XPEvent::DNH_NETWORK_PASSIVE);
            }
        }
        pendingNetworkAdd = false;
    }
    
    // Process deferred PMKID create
    if (pendingPMKIDCreateReady && !pendingPMKIDCreateBusy) {
        // Check if dwell is complete (if we needed one)
        bool canProcess = true;
        if (pendingPMKIDCreate.ssid[0] == 0 && state == DNHState::DWELLING) {
            // Still dwelling, wait for beacon or timeout
            if (!dwellResolved && (now - dwellStartTime < DNH_DWELL_TIME)) {
                canProcess = false;
            }
        }
        
        if (canProcess) {
            pendingPMKIDCreateBusy = true;
            
            // Try to find SSID if we don't have it
            if (pendingPMKIDCreate.ssid[0] == 0) {
                int netIdx = findNetwork(pendingPMKIDCreate.bssid);
                if (netIdx >= 0 && networks[netIdx].ssid[0] != 0) {
                    strncpy(pendingPMKIDCreate.ssid, networks[netIdx].ssid, 32);
                    pendingPMKIDCreate.ssid[32] = 0;
                }
            }
            
            // Create or update PMKID entry
            if (pmkids.size() < DNH_MAX_PMKIDS) {
                int idx = findOrCreatePMKID(pendingPMKIDCreate.bssid);
                if (idx >= 0) {
                    memcpy(pmkids[idx].pmkid, pendingPMKIDCreate.pmkid, 16);
                    memcpy(pmkids[idx].station, pendingPMKIDCreate.station, 6);
                    strncpy(pmkids[idx].ssid, pendingPMKIDCreate.ssid, 32);
                    pmkids[idx].ssid[32] = 0;
                    pmkids[idx].timestamp = now;
                    
                    // Announce capture + immediate safe save
                    if (pendingPMKIDCreate.ssid[0] != 0) {
                        Serial.printf("[DNH] PMKID captured: %s\n", pendingPMKIDCreate.ssid);
                        Display::showToast("BOOMBOCLAAT! PMKID");
                        if (Config::personality().soundEnabled) {
                            M5.Speaker.tone(880, 100);
                        }
                        // XP awarded via Mood::onPMKIDCaptured (don't double award)
                        Mood::onPMKIDCaptured(pendingPMKIDCreate.ssid);
                        
                        // Immediate save with brief promiscuous pause (safe SD access)
                        esp_wifi_set_promiscuous(false);
                        delay(5);
                        saveAllPMKIDs();
                        esp_wifi_set_promiscuous(true);
                    } else {
                        Serial.println("[DNH] PMKID captured but SSID unknown");
                    }
                }
            }
            
            pendingPMKIDCreateReady = false;
            pendingPMKIDCreateBusy = false;
            
            // Return to hopping if we were dwelling
            if (state == DNHState::DWELLING) {
                state = DNHState::HOPPING;
                dwellResolved = false;
            }
        }
    }
    
    // Process deferred handshake frame add (batch process all queued frames)
    if (pendingHandshakeAdd && !pendingHandshakeBusy) {
        pendingHandshakeBusy = true;
        
        // Find or create handshake entry
        int hsIdx = findOrCreateHandshake(pendingHandshake.bssid, pendingHandshake.station);
        if (hsIdx >= 0) {
            CapturedHandshake& hs = handshakes[hsIdx];
            
            // Process ALL queued frames (M1-M4) from capturedMask
            for (int msgIdx = 0; msgIdx < 4; msgIdx++) {
                if (pendingHandshake.capturedMask & (1 << msgIdx)) {
                    // Frame is present in the queued data
                    if (hs.frames[msgIdx].len == 0) {  // Not already captured
                        uint16_t copyLen = pendingHandshake.frames[msgIdx].len;
                        if (copyLen > 0 && copyLen <= 512) {
                            // EAPOL payload for hashcat 22000
                            memcpy(hs.frames[msgIdx].data, pendingHandshake.frames[msgIdx].data, copyLen);
                            hs.frames[msgIdx].len = copyLen;
                            hs.frames[msgIdx].messageNum = msgIdx + 1;
                            hs.frames[msgIdx].timestamp = now;
                            
                            // Full 802.11 frame for PCAP export (radiotap + WPA-SEC)
                            uint16_t fullCopyLen = pendingHandshake.frames[msgIdx].fullFrameLen;
                            if (fullCopyLen > 0 && fullCopyLen <= 300) {
                                memcpy(hs.frames[msgIdx].fullFrame, pendingHandshake.frames[msgIdx].fullFrame, fullCopyLen);
                                hs.frames[msgIdx].fullFrameLen = fullCopyLen;
                                hs.frames[msgIdx].rssi = pendingHandshake.frames[msgIdx].rssi;
                            }
                            
                            hs.capturedMask |= (1 << msgIdx);
                            hs.lastSeen = now;
                            
                            Serial.printf("[DNH] M%d captured (deferred)\n", msgIdx + 1);
                        }
                    }
                }
            }
            
            // Look up SSID if missing
            if (hs.ssid[0] == 0) {
                int netIdx = findNetwork(hs.bssid);
                if (netIdx >= 0 && networks[netIdx].ssid[0] != 0) {
                    strncpy(hs.ssid, networks[netIdx].ssid, 32);
                    hs.ssid[32] = 0;
                }
            }
            
            // Check if we just completed a valid pair
            if (hs.hasValidPair() && !hs.saved && !pendingHandshakeCapture) {
                strncpy(pendingHandshakeSSID, hs.ssid, 32);
                pendingHandshakeSSID[32] = 0;
                pendingHandshakeCapture = true;
                
                Serial.printf("[DNH] Handshake complete: %s\n", 
                    hs.ssid[0] ? hs.ssid : "?");
            }
        }
        
        // Clear slot for next handshake
        pendingHandshake.capturedMask = 0;
        pendingHandshakeAdd = false;
        pendingHandshakeBusy = false;
    }
    
    // Process handshake capture event (UI update + immediate safe save)
    if (pendingHandshakeCapture) {
        Display::showToast("NATURAL HANDSHAKE BLESSED - RESPECT DI HERB");
        if (Config::personality().soundEnabled) {
            M5.Speaker.tone(880, 100);
        }
        // XP awarded via Mood::onHandshakeCaptured (don't double award)
        Mood::onHandshakeCaptured(pendingHandshakeSSID);
        pendingHandshakeCapture = false;
        
        // Immediate save with brief promiscuous pause (safe SD access)
        // ~50ms gap is acceptable - we just captured what we needed
        esp_wifi_set_promiscuous(false);
        delay(5);  // Let SPI bus settle
        saveAllHandshakes();
        esp_wifi_set_promiscuous(true);
    }
    
    // Channel hopping state machine
    switch (state) {
        case DNHState::HOPPING:
            {
                uint32_t hopDelay = getAdaptiveHopDelay();
                if (now - lastHopTime > hopDelay) {
                    hopToNextChannel();
                    lastHopTime = now;
                    
                    // Check if we should enter HUNTING mode after hop
                    bool enteredHunting = checkHuntingTrigger();
                    if (!enteredHunting) {
                        // Check if all channels are dead -> IDLE_SWEEP
                        checkIdleSweep();
                    }
                }
            }
            break;
            
        case DNHState::DWELLING:
            if (dwellResolved || (now - dwellStartTime > DNH_DWELL_TIME)) {
                state = DNHState::HOPPING;
                dwellResolved = false;
            }
            break;
            
        case DNHState::HUNTING:
            if (now - huntStartTime > HUNT_DURATION) {
                // Hunt timeout, return to hopping
                state = DNHState::HOPPING;
                lastHuntTime = now;
                lastHuntChannel = currentChannel;
            }
            // HUNTING state deliberately does NOT hop - camps on hot channel
            break;
            
        case DNHState::IDLE_SWEEP:
            {
                uint32_t peekDelay = IDLE_SWEEP_TIME;
                if (now - lastHopTime > peekDelay) {
                    hopToNextChannel();
                    lastHopTime = now;
                    
                    // If we see ANY activity, exit IDLE_SWEEP
                    int idx = currentChannel - 1;
                    if (idx >= 0 && idx < 13) {
                        if (channelStats[idx].beaconCount > 0) {
                            state = DNHState::HOPPING;
                        }
                    }
                }
            }
            break;
    }
    
    // Periodic cleanup (every 10 seconds)
    if (now - lastCleanupTime > 10000) {
        ageOutStaleNetworks();
        pruneIncompleteHandshakes(); // Also prune stale handshake tracking
        lastCleanupTime = now;
    }
    
    // Periodic stats decay (every 2 minutes)
    if (now - lastStatsDecay > STATS_DECAY_INTERVAL) {
        decayChannelStats();
        lastStatsDecay = now;
    }
    
    // Backup save flag (every 30 seconds) - catches any missed immediate saves
    // Actual save deferred to stop() after WiFi promiscuous disabled
    if (now - lastSaveTime > 30000) {
        pendingSaveFlag = true;  // Will be processed in stop() after WiFi off
        lastSaveTime = now;
    }
    
    // Mood update (every 3 seconds)
    if (now - lastMoodTime > 3000) {
        Mood::onPassiveRecon(networks.size(), currentChannel);
        lastMoodTime = now;
    }
    
    dnhBusy = false;
}

void DoNoHamMode::hopToNextChannel() {
    channelIndex = (channelIndex + 1) % 13;
    currentChannel = CHANNEL_ORDER[channelIndex];
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
}

// Check if channel is primary (1, 6, or 11)
bool DoNoHamMode::isPrimaryChannel(uint8_t ch) {
    return (ch == 1 || ch == 6 || ch == 11);
}

// Calculate adaptive hop delay based on channel activity
uint16_t DoNoHamMode::getAdaptiveHopDelay() {
    ChannelStats& stats = channelStats[channelIndex];
    
    // Base timing: primary channels get more time
    uint16_t baseTime = isPrimaryChannel(stats.channel) ? HOP_BASE_PRIMARY : HOP_BASE_SECONDARY;
    
    // Adjust based on local activity
    uint16_t hopDelay;
    if (stats.beaconCount >= BUSY_THRESHOLD) {
        hopDelay = (baseTime * 3) / 2;  // Busy channel (1.5x)
    } else if (stats.beaconCount >= 2) {
        hopDelay = baseTime;  // Normal activity
    } else if (stats.deadStreak >= DEAD_STREAK_LIMIT) {
        hopDelay = HOP_MIN;  // Dead channel, minimum time
    } else {
        hopDelay = (baseTime * 7) / 10;  // Light activity (0.7x)
    }
    
    // Global activity adjustment
    if (lastCycleActivity < 5) {
        hopDelay = (hopDelay * 3) / 5;  // Quiet spectrum (0.6x)
    } else if (lastCycleActivity > 40) {
        hopDelay = (hopDelay * 6) / 5;  // Busy spectrum (1.2x)
    }
    
    return hopDelay;
}

// Decay channel stats every 2 minutes
void DoNoHamMode::decayChannelStats() {
    for (int i = 0; i < 13; i++) {
        channelStats[i].beaconCount = 0;
        channelStats[i].eapolCount = 0;
        channelStats[i].priority = 100;  // Reset to baseline
        channelStats[i].deadStreak = 0;
    }
    lastCycleActivity = 0;
}

// Check if we should enter HUNTING state
bool DoNoHamMode::checkHuntingTrigger() {
    ChannelStats& stats = channelStats[channelIndex];
    uint32_t now = millis();
    
    // Don't re-hunt same channel too quickly
    if (lastHuntChannel == currentChannel && (now - lastHuntTime) < HUNT_COOLDOWN_MS) {
        return false;
    }
    
    // Trigger: 2+ EAPOL frames or high beacon burst
    if (stats.eapolCount >= 2 || stats.beaconCount >= 8) {
        state = DNHState::HUNTING;
        huntStartTime = now;
        lastHuntChannel = currentChannel;
        lastHuntTime = now;
        Serial.printf("[DNH] HUNTING on ch %d (B:%d E:%d)\n", 
            currentChannel, stats.beaconCount, stats.eapolCount);
        return true;
    }
    return false;
}

// Check if all channels are dead → IDLE_SWEEP
void DoNoHamMode::checkIdleSweep() {
    // If just completed full cycle
    if (channelIndex == 0) {
        uint16_t totalActivity = 0;
        for (int i = 0; i < 13; i++) {
            totalActivity += channelStats[i].beaconCount;
        }
        lastCycleActivity = totalActivity;
        
        // All channels dead → enter IDLE_SWEEP
        if (totalActivity == 0) {
            state = DNHState::IDLE_SWEEP;
            Serial.println("[DNH] IDLE_SWEEP: spectrum silent");
        }
    }
}

// Track incomplete handshake for revisit
void DoNoHamMode::trackIncompleteHandshake(const uint8_t* bssid, uint8_t mask, uint8_t ch) {
    // Check if already tracked
    for (auto& ihs : incompleteHandshakes) {
        if (memcmp(ihs.bssid, bssid, 6) == 0) {
            ihs.capturedMask = mask;
            ihs.lastSeen = millis();
            return;
        }
    }
    
    // Add new if room
    if (incompleteHandshakes.size() < MAX_INCOMPLETE_HS) {
        IncompleteHS ihs;
        memcpy(ihs.bssid, bssid, 6);
        ihs.capturedMask = mask;
        ihs.channel = ch;
        ihs.lastSeen = millis();
        incompleteHandshakes.push_back(ihs);
    }
}

// Remove stale incomplete handshakes
void DoNoHamMode::pruneIncompleteHandshakes() {
    uint32_t now = millis();
    auto it = incompleteHandshakes.begin();
    while (it != incompleteHandshakes.end()) {
        if ((now - it->lastSeen) > INCOMPLETE_HS_TIMEOUT) {
            it = incompleteHandshakes.erase(it);
        } else {
            ++it;
        }
    }
}

void DoNoHamMode::startDwell() {
    state = DNHState::DWELLING;
    dwellStartTime = millis();
    dwellResolved = false;
    Serial.printf("[DNH] Dwelling on ch %d for SSID\n", currentChannel);
}

void DoNoHamMode::ageOutStaleNetworks() {
    uint32_t now = millis();
    auto it = networks.begin();
    while (it != networks.end()) {
        if (now - it->lastSeen > DNH_STALE_TIMEOUT) {
            it = networks.erase(it);
        } else {
            ++it;
        }
    }
}

void DoNoHamMode::saveAllPMKIDs() {
    // Guard: Skip if no SD card available (graceful degradation)
    if (!Config::isSDAvailable()) return;
    
    // Save PMKIDs in hashcat 22000 format
    for (auto& p : pmkids) {
        if (p.saved) continue;
        if (p.saveAttempts >= 3) continue;  // Give up after 3 failures
        
        // Exponential backoff: 0s, 2s, 5s
        static const uint32_t backoffMs[] = {0, 2000, 5000};
        uint32_t timeSinceCapture = millis() - p.timestamp;
        if (timeSinceCapture < backoffMs[p.saveAttempts]) continue;
        
        // Try to backfill SSID if missing
        if (p.ssid[0] == 0) {
            int netIdx = findNetwork(p.bssid);
            if (netIdx >= 0 && networks[netIdx].ssid[0] != 0) {
                strncpy(p.ssid, networks[netIdx].ssid, 32);
                p.ssid[32] = 0;
            }
        }
        
        // Try to backfill SSID from companion txt file (cross-mode compatibility)
        if (p.ssid[0] == 0) {
            char txtPath[64];
            snprintf(txtPath, sizeof(txtPath), "/M5PORKCHOP/handshakes/%02X%02X%02X%02X%02X%02X_pmkid.txt",
                     p.bssid[0], p.bssid[1], p.bssid[2], p.bssid[3], p.bssid[4], p.bssid[5]);
            if (SD.exists(txtPath)) {
                File txtFile = SD.open(txtPath, FILE_READ);
                if (txtFile) {
                    String ssid = txtFile.readStringUntil('\n');
                    ssid.trim();
                    if (ssid.length() > 0) {
                        strncpy(p.ssid, ssid.c_str(), 32);
                        p.ssid[32] = 0;
                        Serial.printf("[DNH] PMKID SSID backfilled from txt: %s\\n", p.ssid);
                    }
                    txtFile.close();
                }
            }
        }
        
        // Can only save if we have SSID (don't count as attempt - will retry when SSID arrives)
        if (p.ssid[0] == 0) continue;
        
        // Check for all-zero PMKID (invalid - don't count as attempt)
        bool allZeros = true;
        for (int i = 0; i < 16; i++) {
            if (p.pmkid[i] != 0) { allZeros = false; break; }
        }
        if (allZeros) {
            p.saved = true;  // Mark invalid PMKID as done
            continue;
        }
        
        // Now we actually attempt to save - increment counter
        p.saveAttempts++;
        
        // Build filename: /M5PORKCHOP/handshakes/BSSID.22000
        char filename[64];
        snprintf(filename, sizeof(filename), "/M5PORKCHOP/handshakes/%02X%02X%02X%02X%02X%02X.22000",
            p.bssid[0], p.bssid[1], p.bssid[2], p.bssid[3], p.bssid[4], p.bssid[5]);
        
        // Ensure directory exists
        if (!SD.exists("/M5PORKCHOP/handshakes")) {
            SD.mkdir("/M5PORKCHOP/handshakes");
        }
        
        File f = SD.open(filename, FILE_WRITE);
        if (!f) {
            Serial.printf("[DNH] Failed to create PMKID file: %s (attempt %d)\\n", filename, p.saveAttempts);
            if (p.saveAttempts >= 3) {
                p.saved = true;  // Give up
            }
            continue;
        }
        
        // Build hex strings
        char pmkidHex[33];
        for (int i = 0; i < 16; i++) {
            sprintf(pmkidHex + i*2, "%02x", p.pmkid[i]);
        }
        
        char macAP[13];
        sprintf(macAP, "%02x%02x%02x%02x%02x%02x",
            p.bssid[0], p.bssid[1], p.bssid[2], 
            p.bssid[3], p.bssid[4], p.bssid[5]);
        
        char macClient[13];
        sprintf(macClient, "%02x%02x%02x%02x%02x%02x",
            p.station[0], p.station[1], p.station[2], 
            p.station[3], p.station[4], p.station[5]);
        
        char essidHex[65];
        int ssidLen = strlen(p.ssid);
        if (ssidLen > 32) ssidLen = 32;  // Cap to max SSID length
        for (int i = 0; i < ssidLen; i++) {
            sprintf(essidHex + i*2, "%02x", (uint8_t)p.ssid[i]);
        }
        essidHex[ssidLen * 2] = 0;
        
        // WPA*01*PMKID*MAC_AP*MAC_CLIENT*ESSID***01
        f.printf("WPA*01*%s*%s*%s*%s***01\\n", pmkidHex, macAP, macClient, essidHex);
        f.close();
        
        // Save SSID to companion txt file (matches OINK pattern)
        char txtFilename[64];
        snprintf(txtFilename, sizeof(txtFilename), "/M5PORKCHOP/handshakes/%02X%02X%02X%02X%02X%02X_pmkid.txt",
                 p.bssid[0], p.bssid[1], p.bssid[2], p.bssid[3], p.bssid[4], p.bssid[5]);
        // Delete existing file first to ensure clean overwrite (FILE_WRITE appends on ESP32)
        if (SD.exists(txtFilename)) {
            SD.remove(txtFilename);
        }
        File txtFile = SD.open(txtFilename, FILE_WRITE);
        if (txtFile) {
            txtFile.println(p.ssid);
            txtFile.close();
        }
        
        p.saved = true;
        Serial.printf("[DNH] PMKID saved: %s\\n", filename);
        SDLog::log("DNH", "PMKID saved: %s (%s)", p.ssid, filename);
    }
}

void DoNoHamMode::saveAllHandshakes() {
    // Guard: Skip if no SD card available (graceful degradation)
    if (!Config::isSDAvailable()) return;
    
    // Save handshakes in hashcat 22000 format (WPA*02)
    for (auto& hs : handshakes) {
        if (hs.saved) continue;
        if (!hs.hasValidPair()) continue;  // Need M1+M2 or M2+M3
        if (hs.saveAttempts >= 3) continue;  // Give up after 3 failures
        
        // Exponential backoff: 0s, 2s, 5s
        static const uint32_t backoffMs[] = {0, 2000, 5000};
        uint32_t timeSinceCapture = millis() - hs.lastSeen;
        if (timeSinceCapture < backoffMs[hs.saveAttempts]) continue;
        
        // Try to backfill SSID if missing
        if (hs.ssid[0] == 0) {
            int netIdx = findNetwork(hs.bssid);
            if (netIdx >= 0 && networks[netIdx].ssid[0] != 0) {
                strncpy(hs.ssid, networks[netIdx].ssid, 32);
                hs.ssid[32] = 0;
            }
        }
        
        // Try to backfill SSID from companion txt file (cross-mode compatibility)
        if (hs.ssid[0] == 0) {
            char txtPath[64];
            snprintf(txtPath, sizeof(txtPath), "/M5PORKCHOP/handshakes/%02X%02X%02X%02X%02X%02X.txt",
                     hs.bssid[0], hs.bssid[1], hs.bssid[2], hs.bssid[3], hs.bssid[4], hs.bssid[5]);
            if (SD.exists(txtPath)) {
                File txtFile = SD.open(txtPath, FILE_READ);
                if (txtFile) {
                    String ssid = txtFile.readStringUntil('\n');
                    ssid.trim();
                    if (ssid.length() > 0) {
                        strncpy(hs.ssid, ssid.c_str(), 32);
                        hs.ssid[32] = 0;
                        Serial.printf("[DNH] Handshake SSID backfilled from txt: %s\\n", hs.ssid);
                    }
                    txtFile.close();
                }
            }
        }
        
        // Can only save if we have SSID (don't count as attempt - will retry when SSID arrives)
        if (hs.ssid[0] == 0) continue;
        
        // Determine message pair
        uint8_t msgPair = hs.getMessagePair();
        if (msgPair == 0xFF) continue;
        
        // Get frames
        const EAPOLFrame* nonceFrame = nullptr;
        const EAPOLFrame* eapolFrame = nullptr;
        
        if (msgPair == 0x00) {
            nonceFrame = &hs.frames[0];  // M1
            eapolFrame = &hs.frames[1];  // M2
        } else {
            nonceFrame = &hs.frames[2];  // M3
            eapolFrame = &hs.frames[1];  // M2
        }
        
        // Frame length validation (don't count as attempt if malformed)
        if (nonceFrame->len < 51 || eapolFrame->len < 97) continue;
        
        // Now we actually attempt to save - increment counter
        hs.saveAttempts++;
        
        // Build filename: /M5PORKCHOP/handshakes/BSSID_hs.22000
        char filename[64];
        snprintf(filename, sizeof(filename), "/M5PORKCHOP/handshakes/%02X%02X%02X%02X%02X%02X_hs.22000",
            hs.bssid[0], hs.bssid[1], hs.bssid[2], hs.bssid[3], hs.bssid[4], hs.bssid[5]);
        
        // Ensure directory exists
        if (!SD.exists("/M5PORKCHOP/handshakes")) {
            SD.mkdir("/M5PORKCHOP/handshakes");
        }
        
        File f = SD.open(filename, FILE_WRITE);
        if (!f) {
            Serial.printf("[DNH] Failed to create handshake file: %s (attempt %d)\\n", filename, hs.saveAttempts);
            if (hs.saveAttempts >= 3) {
                hs.saved = true;  // Give up
            }
            continue;
        }
        
        // Extract MIC from M2 (offset 81, 16 bytes)
        char micHex[33];
        for (int i = 0; i < 16; i++) {
            sprintf(micHex + i*2, "%02x", eapolFrame->data[81 + i]);
        }
        
        // MAC_AP
        char macAP[13];
        sprintf(macAP, "%02x%02x%02x%02x%02x%02x",
            hs.bssid[0], hs.bssid[1], hs.bssid[2],
            hs.bssid[3], hs.bssid[4], hs.bssid[5]);
        
        // MAC_CLIENT
        char macClient[13];
        sprintf(macClient, "%02x%02x%02x%02x%02x%02x",
            hs.station[0], hs.station[1], hs.station[2],
            hs.station[3], hs.station[4], hs.station[5]);
        
        // ESSID (hex-encoded, max 32 chars = 64 hex + null)
        char essidHex[65];
        int ssidLen = strlen(hs.ssid);
        if (ssidLen > 32) ssidLen = 32;  // Cap to max SSID length
        for (int i = 0; i < ssidLen; i++) {
            sprintf(essidHex + i*2, "%02x", (uint8_t)hs.ssid[i]);
        }
        essidHex[ssidLen * 2] = 0;
        
        // ANonce from M1 or M3 (offset 17, 32 bytes)
        char nonceHex[65];
        for (int i = 0; i < 32; i++) {
            sprintf(nonceHex + i*2, "%02x", nonceFrame->data[17 + i]);
        }
        
        // EAPOL frame with MIC zeroed
        uint16_t eapolLen = (eapolFrame->data[2] << 8) | eapolFrame->data[3];
        eapolLen += 4;
        if (eapolLen > eapolFrame->len) eapolLen = eapolFrame->len;
        
        char* eapolHex = (char*)malloc(eapolLen * 2 + 1);
        if (!eapolHex) {
            f.close();
            continue;
        }
        
        // Copy and zero MIC
        uint8_t eapolCopy[512];
        memcpy(eapolCopy, eapolFrame->data, eapolLen);
        memset(eapolCopy + 81, 0, 16);
        
        for (int i = 0; i < eapolLen; i++) {
            sprintf(eapolHex + i*2, "%02x", eapolCopy[i]);
        }
        eapolHex[eapolLen * 2] = 0;
        
        // WPA*02*MIC*MAC_AP*MAC_CLIENT*ESSID*ANONCE*EAPOL*MESSAGEPAIR
        f.printf("WPA*02*%s*%s*%s*%s*%s*%s*%02x\n",
            micHex, macAP, macClient, essidHex, nonceHex, eapolHex, msgPair);
        
        free(eapolHex);
        f.close();
        
        // Save SSID to companion txt file (matches OINK pattern)
        char txtFilename[64];
        snprintf(txtFilename, sizeof(txtFilename), "/M5PORKCHOP/handshakes/%02X%02X%02X%02X%02X%02X.txt",
                 hs.bssid[0], hs.bssid[1], hs.bssid[2], hs.bssid[3], hs.bssid[4], hs.bssid[5]);
        File txtFile = SD.open(txtFilename, FILE_WRITE);
        if (txtFile) {
            txtFile.println(hs.ssid);
            txtFile.close();
        }
        
        // Also save PCAP (for WPA-SEC upload and wireshark analysis)
        char pcapFilename[64];
        snprintf(pcapFilename, sizeof(pcapFilename), "/M5PORKCHOP/handshakes/%02X%02X%02X%02X%02X%02X.pcap",
            hs.bssid[0], hs.bssid[1], hs.bssid[2], hs.bssid[3], hs.bssid[4], hs.bssid[5]);
        
        File pcapFile = SD.open(pcapFilename, FILE_WRITE);
        if (pcapFile) {
            // Write PCAP global header
            DNH_PCAPHeader hdr = {
                .magic = 0xA1B2C3D4,
                .version_major = 2,
                .version_minor = 4,
                .thiszone = 0,
                .sigfigs = 0,
                .snaplen = 65535,
                .linktype = 127  // IEEE802_11_RADIOTAP
            };
            pcapFile.write((uint8_t*)&hdr, sizeof(hdr));
            
            int packetCount = 0;
            
            // Write beacon if available
            if (hs.hasBeacon()) {
                uint32_t beaconTotalLen = sizeof(DNH_RADIOTAP_HEADER) + hs.beaconLen;
                DNH_PCAPPacketHeader beaconPkt = {
                    .ts_sec = hs.firstSeen / 1000,
                    .ts_usec = (hs.firstSeen % 1000) * 1000,
                    .incl_len = beaconTotalLen,
                    .orig_len = beaconTotalLen
                };
                pcapFile.write((uint8_t*)&beaconPkt, sizeof(beaconPkt));
                pcapFile.write(DNH_RADIOTAP_HEADER, sizeof(DNH_RADIOTAP_HEADER));
                pcapFile.write(hs.beaconData, hs.beaconLen);
                packetCount++;
            }
            
            // Write EAPOL frames
            for (int i = 0; i < 4; i++) {
                if (!(hs.capturedMask & (1 << i))) continue;
                const EAPOLFrame& frame = hs.frames[i];
                if (frame.len == 0) continue;
                
                // Prefer fullFrame if available
                if (frame.fullFrameLen > 0 && frame.fullFrameLen <= 300) {
                    uint32_t totalLen = sizeof(DNH_RADIOTAP_HEADER) + frame.fullFrameLen;
                    DNH_PCAPPacketHeader pkt = {
                        .ts_sec = frame.timestamp / 1000,
                        .ts_usec = (frame.timestamp % 1000) * 1000,
                        .incl_len = totalLen,
                        .orig_len = totalLen
                    };
                    pcapFile.write((uint8_t*)&pkt, sizeof(pkt));
                    pcapFile.write(DNH_RADIOTAP_HEADER, sizeof(DNH_RADIOTAP_HEADER));
                    pcapFile.write(frame.fullFrame, frame.fullFrameLen);
                    packetCount++;
                }
            }
            
            pcapFile.close();
            Serial.printf("[DNH] PCAP saved: %s (%d packets)\n", pcapFilename, packetCount);
        }
        
        hs.saved = true;
        Serial.printf("[DNH] Handshake saved: %s\\n", filename);
        SDLog::log("DNH", "Handshake saved: %s (%s)", hs.ssid, filename);
    }
}

int DoNoHamMode::findNetwork(const uint8_t* bssid) {
    for (size_t i = 0; i < networks.size(); i++) {
        if (memcmp(networks[i].bssid, bssid, 6) == 0) {
            return i;
        }
    }
    return -1;
}

int DoNoHamMode::findOrCreatePMKID(const uint8_t* bssid) {
    // Find existing
    for (size_t i = 0; i < pmkids.size(); i++) {
        if (memcmp(pmkids[i].bssid, bssid, 6) == 0) {
            return i;
        }
    }
    // Create new
    if (pmkids.size() < DNH_MAX_PMKIDS) {
        CapturedPMKID p = {};
        memcpy(p.bssid, bssid, 6);
        pmkids.push_back(p);
        return pmkids.size() - 1;
    }
    return -1;
}

int DoNoHamMode::findOrCreateHandshake(const uint8_t* bssid, const uint8_t* station) {
    // Find existing with matching BSSID and station
    for (size_t i = 0; i < handshakes.size(); i++) {
        if (memcmp(handshakes[i].bssid, bssid, 6) == 0 &&
            memcmp(handshakes[i].station, station, 6) == 0) {
            return i;
        }
    }
    // Create new
    if (handshakes.size() < DNH_MAX_HANDSHAKES) {
        CapturedHandshake hs = {};
        memcpy(hs.bssid, bssid, 6);
        memcpy(hs.station, station, 6);
        hs.capturedMask = 0;
        hs.firstSeen = millis();
        hs.lastSeen = hs.firstSeen;
        hs.saved = false;
        hs.beaconData = nullptr;
        hs.beaconLen = 0;
        handshakes.push_back(hs);
        return handshakes.size() - 1;
    }
    return -1;
}

// Frame handlers - called from shared promiscuous callback
void DoNoHamMode::handleBeacon(const uint8_t* frame, uint16_t len, int8_t rssi) {
    if (!running) return;
    if (dnhBusy) return;  // Skip if update() is processing vectors
    
    // Beacon frame structure:
    // [0-1] Frame Control, [2-3] Duration, [4-9] DA, [10-15] SA (BSSID), [16-21] BSSID
    // [22-23] Seq, [24-35] Timestamp, [36-37] Beacon Interval, [38-39] Capability
    // [40+] IEs
    
    if (len < 40) return;
    
    const uint8_t* bssid = frame + 16;
    
    // Parse SSID from IE 0
    char ssid[33] = {0};
    uint16_t offset = 36;  // Start of fixed fields after header
    offset += 12;  // Skip timestamp(8) + beacon_interval(2) + capability(2)
    
    while (offset + 2 < len) {
        uint8_t ieType = frame[offset];
        uint8_t ieLen = frame[offset + 1];
        if (offset + 2 + ieLen > len) break;
        
        if (ieType == 0 && ieLen > 0 && ieLen <= 32) {
            memcpy(ssid, frame + offset + 2, ieLen);
            ssid[ieLen] = 0;
            break;
        }
        offset += 2 + ieLen;
    }
    
    // Check if this resolves a pending PMKID dwell
    if (state == DNHState::DWELLING && ssid[0] != 0) {
        if (memcmp(bssid, pendingPMKIDCreate.bssid, 6) == 0) {
            strncpy(pendingPMKIDCreate.ssid, ssid, 32);
            pendingPMKIDCreate.ssid[32] = 0;
            dwellResolved = true;
            Serial.printf("[DNH] Dwell resolved: %s\n", ssid);
        }
    }
    
    // Queue network for deferred add
    if (!pendingNetworkAdd) {
        memset(&pendingNetwork, 0, sizeof(pendingNetwork));
        memcpy(pendingNetwork.bssid, bssid, 6);
        strncpy(pendingNetwork.ssid, ssid, 32);
        pendingNetwork.ssid[32] = 0;
        pendingNetwork.rssi = rssi;
        pendingNetwork.channel = currentChannel;
        pendingNetwork.lastSeen = millis();
        pendingNetwork.beaconCount = 1;
        pendingNetworkAdd = true;
    }
    
    // Store beacon for any in-progress handshakes from this BSSID
    // (needed for PCAP export / WPA-SEC upload)
    for (auto& hs : handshakes) {
        if (!hs.saved && hs.beaconData == nullptr && memcmp(hs.bssid, bssid, 6) == 0) {
            // Allocate and copy beacon data
            uint16_t copyLen = (len > 512) ? 512 : len;  // Cap at reasonable size
            hs.beaconData = (uint8_t*)malloc(copyLen);
            if (hs.beaconData) {
                memcpy(hs.beaconData, frame, copyLen);
                hs.beaconLen = copyLen;
                Serial.printf("[DNH] Beacon stored for handshake: %02X:%02X:%02X:%02X:%02X:%02X\n",
                    bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
            }
            break;  // One beacon per handshake is enough
        }
    }
    
    // Track channel activity for adaptive hopping
    int idx = currentChannel - 1;
    if (idx >= 0 && idx < 13) {
        channelStats[idx].beaconCount++;
        channelStats[idx].lifetimeBeacons++;
        channelStats[idx].lastActivity = millis();
    }
}

void DoNoHamMode::handleEAPOL(const uint8_t* frame, uint16_t len, int8_t rssi) {
    if (!running) return;
    if (dnhBusy) return;  // Skip if update() is processing vectors
    
    // Parse 802.11 data frame to find EAPOL
    // Frame: FC(2) + Duration(2) + Addr1(6) + Addr2(6) + Addr3(6) + Seq(2) = 24 bytes
    // Then QoS(2) if present, then LLC/SNAP(8), then EAPOL payload
    
    if (len < 24) return;
    
    // Check To/From DS flags
    uint8_t toDs = (frame[1] & 0x01);
    uint8_t fromDs = (frame[1] & 0x02) >> 1;
    
    // Extract MACs based on To/From DS
    const uint8_t* srcMac;
    const uint8_t* dstMac;
    const uint8_t* bssid;
    
    if (toDs && !fromDs) {
        // To DS: RA=BSSID, TA=SA
        dstMac = frame + 4;
        srcMac = frame + 10;
        bssid = frame + 4;
    } else if (!toDs && fromDs) {
        // From DS: RA=DA, TA=BSSID
        dstMac = frame + 4;
        srcMac = frame + 10;
        bssid = frame + 10;
    } else if (!toDs && !fromDs) {
        // IBSS or Direct Link
        dstMac = frame + 4;
        srcMac = frame + 10;
        bssid = frame + 16;
    } else {
        // WDS (both set) - skip
        return;
    }
    
    // Calculate data offset (after 802.11 header)
    uint16_t offset = 24;
    
    // Check for QoS Data (subtype bit 3 set)
    uint8_t subtype = (frame[0] >> 4) & 0x0F;
    bool isQoS = (subtype & 0x08) != 0;
    if (isQoS) offset += 2;
    
    // Check for HTC (QoS + Order bit)
    if (isQoS && (frame[1] & 0x80)) offset += 4;
    
    if (offset + 8 > len) return;
    
    // Check LLC/SNAP for EAPOL: AA AA 03 00 00 00 88 8E
    if (frame[offset] != 0xAA || frame[offset+1] != 0xAA ||
        frame[offset+2] != 0x03 || frame[offset+3] != 0x00 ||
        frame[offset+4] != 0x00 || frame[offset+5] != 0x00 ||
        frame[offset+6] != 0x88 || frame[offset+7] != 0x8E) {
        return;  // Not EAPOL
    }
    
    // EAPOL payload starts after LLC/SNAP
    const uint8_t* eapol = frame + offset + 8;
    uint16_t eapolLen = len - offset - 8;
    
    if (eapolLen < 4) return;
    
    // EAPOL: version(1) + type(1) + length(2)
    uint8_t eapolType = eapol[1];
    if (eapolType != 3) return;  // EAPOL-Key only
    
    if (eapolLen < 99) return;  // Minimum for key frame
    
    // EAPOL-Key: descriptor_type(1) @ 4, key_info(2) @ 5-6
    uint16_t keyInfo = (eapol[5] << 8) | eapol[6];
    uint8_t install = (keyInfo >> 6) & 0x01;
    uint8_t keyAck = (keyInfo >> 7) & 0x01;
    uint8_t keyMic = (keyInfo >> 8) & 0x01;
    uint8_t secure = (keyInfo >> 9) & 0x01;
    
    // Identify message: M1 = KeyAck, no MIC; M2 = MIC, not secure; M3 = KeyAck+MIC+Install; M4 = MIC+Secure
    uint8_t messageNum = 0;
    if (keyAck && !keyMic) messageNum = 1;
    else if (!keyAck && keyMic && !secure) messageNum = 2;
    else if (keyAck && keyMic && install) messageNum = 3;
    else if (!keyAck && keyMic && secure) messageNum = 4;
    
    if (messageNum == 0) return;  // Unknown message type
    
    // Determine BSSID and station based on message direction
    // M1/M3 = AP->Station, M2/M4 = Station->AP
    uint8_t apBssid[6], station[6];
    if (messageNum == 1 || messageNum == 3) {
        memcpy(apBssid, srcMac, 6);
        memcpy(station, dstMac, 6);
    } else {
        memcpy(apBssid, dstMac, 6);
        memcpy(station, srcMac, 6);
    }
    
    // ========== PMKID EXTRACTION FROM M1 ==========
    if (messageNum == 1) {
        // Descriptor type 0x02 = RSN (WPA2/WPA3), 0xFE = WPA1
        uint8_t descriptorType = eapol[4];
        if (descriptorType == 0x02 && eapolLen >= 121) {  // WPA2/3 only
            uint16_t keyDataLen = (eapol[97] << 8) | eapol[98];
            if (keyDataLen >= 22 && eapolLen >= 99 + keyDataLen) {
                const uint8_t* keyData = eapol + 99;
                
                // Search for PMKID KDE: dd 14 00 0f ac 04 [16-byte PMKID]
                for (uint16_t i = 0; i + 22 <= keyDataLen; i++) {
                    if (keyData[i] == 0xdd && keyData[i+1] == 0x14 &&
                        keyData[i+2] == 0x00 && keyData[i+3] == 0x0f &&
                        keyData[i+4] == 0xac && keyData[i+5] == 0x04) {
                        
                        const uint8_t* pmkidData = keyData + i + 6;
                        
                        // Skip all-zero PMKIDs (invalid)
                        bool allZeros = true;
                        for (int z = 0; z < 16; z++) {
                            if (pmkidData[z] != 0) { allZeros = false; break; }
                        }
                        if (allZeros) {
                            Serial.println("[DNH] PMKID KDE found but all zeros (ignored)");
                            break;
                        }
                        
                        // Queue PMKID for creation in main thread
                        if (!pendingPMKIDCreateBusy && !pendingPMKIDCreateReady) {
                            memcpy(pendingPMKIDCreate.bssid, apBssid, 6);
                            memcpy(pendingPMKIDCreate.station, station, 6);
                            memcpy(pendingPMKIDCreate.pmkid, pmkidData, 16);
                            
                            // Try to get SSID from known networks
                            int netIdx = findNetwork(apBssid);
                            if (netIdx >= 0 && networks[netIdx].ssid[0] != 0) {
                                strncpy(pendingPMKIDCreate.ssid, networks[netIdx].ssid, 32);
                                pendingPMKIDCreate.ssid[32] = 0;
                            } else {
                                // No SSID - trigger dwell to catch beacon
                                pendingPMKIDCreate.ssid[0] = 0;
                                state = DNHState::DWELLING;
                                dwellStartTime = millis();
                                dwellResolved = false;
                                Serial.println("[DNH] PMKID needs SSID - dwelling for beacon");
                            }
                            
                            pendingPMKIDCreateReady = true;
                            Serial.printf("[DNH] PMKID queued from %02X:%02X:%02X:%02X:%02X:%02X\n",
                                apBssid[0], apBssid[1], apBssid[2], apBssid[3], apBssid[4], apBssid[5]);
                        }
                        break;  // Found PMKID, stop searching
                    }
                }
            }
        }
    }
    
    // ========== HANDSHAKE FRAME CAPTURE (M1-M4) ==========
    // Queue frame for deferred processing (natural client reconnects)
    // Batch accumulate if frames arrive in quick succession
    if (!pendingHandshakeBusy) {
        // Check if slot already has a frame for this handshake
        bool sameHandshake = pendingHandshakeAdd &&
                             memcmp(pendingHandshake.bssid, apBssid, 6) == 0 &&
                             memcmp(pendingHandshake.station, station, 6) == 0;
        
        if (!pendingHandshakeAdd || sameHandshake) {
            // Either slot is free, or it's for the same handshake (update it)
            if (!pendingHandshakeAdd) {
                // First frame for this handshake - init the slot
                memcpy(pendingHandshake.bssid, apBssid, 6);
                memcpy(pendingHandshake.station, station, 6);
                pendingHandshake.messageNum = messageNum;  // DEPRECATED
                pendingHandshake.capturedMask = 0;
            }
            
            // Store this frame in the appropriate slot (M1-M4 → index 0-3)
            uint8_t frameIdx = messageNum - 1;
            if (frameIdx < 4) {
                // EAPOL payload for hashcat 22000
                uint16_t copyLen = min((uint16_t)512, eapolLen);
                memcpy(pendingHandshake.frames[frameIdx].data, eapol, copyLen);
                pendingHandshake.frames[frameIdx].len = copyLen;
                
                // Full 802.11 frame for PCAP export (radiotap + WPA-SEC compatibility)
                uint16_t fullCopyLen = min((uint16_t)300, len);
                memcpy(pendingHandshake.frames[frameIdx].fullFrame, frame, fullCopyLen);
                pendingHandshake.frames[frameIdx].fullFrameLen = fullCopyLen;
                pendingHandshake.frames[frameIdx].rssi = rssi;
                
                pendingHandshake.capturedMask |= (1 << frameIdx);
            }
            
            pendingHandshakeAdd = true;
            Serial.printf("[DNH] EAPOL M%d queued from %02X:%02X:%02X:%02X:%02X:%02X\n",
                messageNum, apBssid[0], apBssid[1], apBssid[2], apBssid[3], apBssid[4], apBssid[5]);
        }
        // else: slot full with different handshake, drop this frame (rare race)
    }
    
    // Track channel activity for adaptive hopping
    int idx = currentChannel - 1;
    if (idx >= 0 && idx < 13) {
        channelStats[idx].eapolCount++;
        channelStats[idx].lastActivity = millis();
    }
    
    // Track incomplete handshakes for future hunting
    uint8_t captureMask = (1 << (messageNum - 1));
    trackIncompleteHandshake(apBssid, captureMask, currentChannel);
}
