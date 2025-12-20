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
    
    // Reset state
    state = DNHState::HOPPING;
    channelIndex = 0;
    currentChannel = CHANNEL_ORDER[0];
    lastHopTime = millis();
    lastCleanupTime = millis();
    lastSaveTime = millis();
    lastMoodTime = millis();
    dwellResolved = false;
    
    // Reset deferred flags
    pendingNetworkAdd = false;
    pendingPMKIDCreateReady = false;
    pendingPMKIDCreateBusy = false;
    
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
    Display::showToast("BRAVO 6, GOING DARK");
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
    
    running = true;
    
    // UI feedback
    Display::showToast("BRAVO 6, GOING DARK");
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
    
    // Save any unsaved data
    saveAllPMKIDs();
    saveAllHandshakes();
    
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
    
    Serial.println("[DNH] Stopped");
}

void DoNoHamMode::stopSeamless() {
    if (!running) return;
    
    Serial.println("[DNH] Seamless stop (preserving WiFi state)");
    SDLog::log("DNH", "Seamless stop");
    
    running = false;
    
    // DON'T disable promiscuous mode - OINK will take over
    // DON'T clear vectors - let them die naturally
    
    // Save any unsaved data
    saveAllPMKIDs();
    saveAllHandshakes();
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
                    
                    // Announce capture
                    if (pendingPMKIDCreate.ssid[0] != 0) {
                        Serial.printf("[DNH] PMKID captured: %s\n", pendingPMKIDCreate.ssid);
                        Display::showToast("GHOST PMKID!");
                        M5.Speaker.tone(880, 100);
                        delay(50);
                        M5.Speaker.tone(1100, 100);
                        delay(50);
                        M5.Speaker.tone(1320, 100);
                        XP::addXP(XPEvent::DNH_PMKID_GHOST);
                        Mood::onPMKIDCaptured();
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
    
    // Channel hopping state machine
    switch (state) {
        case DNHState::HOPPING:
            if (now - lastHopTime > DNH_HOP_INTERVAL) {
                hopToNextChannel();
                lastHopTime = now;
            }
            break;
            
        case DNHState::DWELLING:
            if (dwellResolved || (now - dwellStartTime > DNH_DWELL_TIME)) {
                state = DNHState::HOPPING;
                dwellResolved = false;
            }
            break;
    }
    
    // Periodic cleanup (every 10 seconds)
    if (now - lastCleanupTime > 10000) {
        ageOutStaleNetworks();
        lastCleanupTime = now;
    }
    
    // Periodic save (every 2 seconds)
    if (now - lastSaveTime > 2000) {
        saveAllPMKIDs();
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
    // TODO: Implement PMKID saving to SD card
    // Format: hashcat 22000 WPA*01 format
    for (auto& p : pmkids) {
        if (p.saved) continue;
        if (p.ssid[0] == 0) {
            // Try to backfill SSID
            int netIdx = findNetwork(p.bssid);
            if (netIdx >= 0 && networks[netIdx].ssid[0] != 0) {
                strncpy(p.ssid, networks[netIdx].ssid, 32);
                p.ssid[32] = 0;
            }
        }
        if (p.ssid[0] != 0 && !p.saved) {
            // Mark as saved (actual save code from OINK will be ported later)
            p.saved = true;
            Serial.printf("[DNH] Would save PMKID for %s\n", p.ssid);
        }
    }
}

void DoNoHamMode::saveAllHandshakes() {
    // TODO: Implement handshake saving to SD card
    // Format: hashcat 22000 WPA*02 format
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
}

void DoNoHamMode::handleEAPOL(const uint8_t* frame, uint16_t len, int8_t rssi) {
    if (!running) return;
    if (dnhBusy) return;  // Skip if update() is processing vectors
    
    // TODO: Implement EAPOL parsing for PMKID extraction
    // This will be ported from OINK in Phase 3
    
    // For now, just log that we saw EAPOL
    Serial.println("[DNH] EAPOL frame detected");
}
