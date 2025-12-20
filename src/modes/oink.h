// Oink Mode - Deauth and Packet Sniffing
#pragma once

#include <Arduino.h>
#include <esp_wifi.h>
#include <vector>
#include <set>
#include <map>
#include <FS.h>
#include "../ml/features.h"

// Maximum clients to track per network
#define MAX_CLIENTS_PER_NETWORK 8

struct DetectedClient {
    uint8_t mac[6];
    int8_t rssi;
    uint32_t lastSeen;
};

struct DetectedNetwork {
    uint8_t bssid[6];
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    wifi_auth_mode_t authmode;
    WiFiFeatures features;
    uint32_t lastSeen;
    uint16_t beaconCount;
    bool isTarget;
    bool hasPMF;  // Protected Management Frames (immune to deauth)
    bool hasHandshake;  // Already captured handshake for this network
    uint8_t attackAttempts;  // Number of attack attempts (for retry logic)
    bool isHidden;  // Hidden SSID (needs probe response)
    DetectedClient clients[MAX_CLIENTS_PER_NETWORK];
    uint8_t clientCount;
};

struct EAPOLFrame {
    uint8_t data[512];
    uint16_t len;
    uint8_t messageNum;  // 1-4
    uint32_t timestamp;
};

struct CapturedHandshake {
    uint8_t bssid[6];
    uint8_t station[6];
    char ssid[33];
    EAPOLFrame frames[4];  // M1, M2, M3, M4
    uint8_t capturedMask;  // Bits 0-3 for M1-M4
    uint32_t firstSeen;
    uint32_t lastSeen;
    bool saved;  // Already saved to SD
    uint8_t* beaconData;   // Beacon frame for this AP
    uint16_t beaconLen;    // Beacon frame length
    
    bool hasM1() const { return capturedMask & 0x01; }
    bool hasM2() const { return capturedMask & 0x02; }
    bool hasM3() const { return capturedMask & 0x04; }
    bool hasM4() const { return capturedMask & 0x08; }
    bool hasBeacon() const { return beaconData != nullptr && beaconLen > 0; }
    
    // Valid crackable pairs: M1+M2 (preferred) or M2+M3 (fallback if M1 missed)
    bool hasValidPair() const { return (hasM1() && hasM2()) || (hasM2() && hasM3()); }
    bool isComplete() const { return hasValidPair(); }  // Alias for backward compat
    bool isFull() const { return (capturedMask & 0x0F) == 0x0F; }
    
    // Get message pair type for hashcat 22000 format:
    // Returns 0x00 for M1+M2, 0x02 for M2+M3, 0xFF for invalid
    uint8_t getMessagePair() const {
        if (hasM1() && hasM2()) return 0x00;  // M1+M2: EAPOL from M2 (challenge)
        if (hasM2() && hasM3()) return 0x02;  // M2+M3: EAPOL from M2 (authorized)
        return 0xFF;  // Invalid
    }
};

// PMKID capture - clientless attack, extracted from EAPOL M1
struct CapturedPMKID {
    uint8_t bssid[6];
    uint8_t station[6];
    char ssid[33];
    uint8_t pmkid[16];
    uint32_t timestamp;
    bool saved;
};

class OinkMode {
public:
    static void init();
    static void start();
    static void stop();
    static void update();
    static bool isRunning() { return running; }
    
    // Scanning
    static void startScan();
    static void stopScan();
    static const std::vector<DetectedNetwork>& getNetworks() { return networks; }
    
    // Target selection
    static void selectTarget(int index);
    static void clearTarget();
    static DetectedNetwork* getTarget();
    
    // Deauth (educational use only)
    static void startDeauth();
    static void stopDeauth();
    static bool isDeauthing() { return deauthing; }
    
    // Handshake capture
    static const std::vector<CapturedHandshake>& getHandshakes() { return handshakes; }
    static uint16_t getCompleteHandshakeCount();
    static bool saveHandshakePCAP(const CapturedHandshake& hs, const char* path);
    static bool saveAllHandshakes();
    static void autoSaveCheck();
    
    // PMKID capture (clientless attack)
    static const std::vector<CapturedPMKID>& getPMKIDs() { return pmkids; }
    static uint16_t getPMKIDCount() { return pmkids.size(); }
    static bool savePMKID22000(const CapturedPMKID& p, const char* path);
    static bool saveAllPMKIDs();
    
    // Hashcat 22000 format (direct cracking, no conversion)
    static bool saveHandshake22000(const CapturedHandshake& hs, const char* path);
    
    // Channel hopping
    static void setChannel(uint8_t ch);
    static uint8_t getChannel() { return currentChannel; }
    static void enableChannelHop(bool enable);
    
    // Statistics
    static uint32_t getPacketCount() { return packetCount; }
    static uint32_t getDeauthCount() { return deauthCount; }
    static uint16_t getNetworkCount() { return networks.size(); }
    
    // LOCKING state info (for display)
    static bool isLocking();
    static const char* getTargetSSID();
    static uint8_t getTargetClientCount();
    static const uint8_t* getTargetBSSID();
    static bool isTargetHidden();
    
    // Network selection cursor
    static int getSelectionIndex() { return selectionIndex; }
    static void moveSelectionUp();
    static void moveSelectionDown();
    static void confirmSelection();
    
    // BOAR BROS - network exclusion list
    static bool loadBoarBros();           // Load from SD
    static bool saveBoarBros();           // Save to SD
    static bool excludeNetwork(int index); // Add selected network to exclusion list
    static bool excludeNetworkByBSSID(const uint8_t* bssid, const char* ssid); // Add by BSSID directly
    static bool isExcluded(const uint8_t* bssid);  // Check if BSSID is excluded
    static uint16_t getExcludedCount();   // Number of excluded networks
    static void removeBoarBro(uint64_t bssid);  // Remove from exclusion list
    static const std::map<uint64_t, String>& getExcludedMap() { return boarBros; }
    
    // Promiscuous mode callback (public for shared use with DO NO HAM mode)
    static void promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type);
    
private:
    static bool running;
    static bool scanning;
    static bool deauthing;
    static bool channelHopping;
    static uint8_t currentChannel;
    static uint32_t lastHopTime;
    static uint32_t lastScanTime;
    
    static std::vector<DetectedNetwork> networks;
    static std::vector<CapturedHandshake> handshakes;
    static std::vector<CapturedPMKID> pmkids;
    static int targetIndex;
    static uint8_t targetBssid[6];  // Store BSSID to handle index invalidation
    static int selectionIndex;  // Cursor for network selection
    static uint32_t packetCount;
    static uint32_t deauthCount;
    
    // Beacon frame storage (for PCAP)
    static uint8_t* beaconFrame;
    static uint16_t beaconFrameLen;
    static bool beaconCaptured;
    
    // Private processing functions (callback dispatches here)
    static void processBeacon(const uint8_t* payload, uint16_t len, int8_t rssi);
    static void processProbeResponse(const uint8_t* payload, uint16_t len, int8_t rssi);
    static void processDataFrame(const uint8_t* payload, uint16_t len, int8_t rssi);
    static void processEAPOL(const uint8_t* payload, uint16_t len, const uint8_t* srcMac, const uint8_t* dstMac);
    
    static void sendDeauthFrame(const uint8_t* bssid, const uint8_t* station, uint8_t reason);
    static void sendDeauthBurst(const uint8_t* bssid, const uint8_t* station, uint8_t count);
    static void sendDisassocFrame(const uint8_t* bssid, const uint8_t* station, uint8_t reason);
    static void hopChannel();
    static void trackClient(const uint8_t* bssid, const uint8_t* clientMac, int8_t rssi);
    static bool detectPMF(const uint8_t* payload, uint16_t len);

    static int findNetwork(const uint8_t* bssid);
    static int findOrCreateHandshake(const uint8_t* bssid, const uint8_t* station);
    static int findOrCreatePMKID(const uint8_t* bssid, const uint8_t* station);
    static int findOrCreateHandshakeSafe(const uint8_t* bssid, const uint8_t* station);  // Main thread only
    static int findOrCreatePMKIDSafe(const uint8_t* bssid, const uint8_t* station);      // Main thread only
    static void sortNetworksByPriority();
    static bool hasHandshakeFor(const uint8_t* bssid);
    static int getNextTarget();  // Smart target selection
    static void writePCAPHeader(fs::File& f);
    static void writePCAPPacket(fs::File& f, const uint8_t* data, uint16_t len, uint32_t ts);
    
    // BOAR BROS storage
    static std::map<uint64_t, String> boarBros;  // Excluded BSSIDs -> SSID
    static uint64_t bssidToUint64(const uint8_t* bssid);  // Convert 6-byte BSSID to uint64
};
