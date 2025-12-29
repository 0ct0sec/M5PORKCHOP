// HOGWASH Mode - Karma AP (Probe Request WiFi Honeypot)
// Listens for probe requests and responds with matching SSIDs
// Key: K
#pragma once

#include <Arduino.h>
#include <esp_wifi.h>
#include <vector>

// Maximum SSIDs in the ring buffer
#define HOGWASH_SSID_QUEUE_SIZE 8

// SSID entry with metadata
struct SSIDEntry {
    char ssid[33];      // SSID + null terminator
    uint32_t timestamp; // When first seen
    uint8_t probeCount; // How many times probed
};

// Connected station tracking
struct HookedStation {
    uint8_t mac[6];     // Station MAC
    uint32_t connectedAt; // When connected
    bool isApple;       // Identified as Apple device
};

class HogwashMode {
public:
    static void init();
    static void start();
    static void stop();
    static void update();
    static bool isRunning() { return running; }
    
    // Statistics
    static uint32_t getProbeCount() { return probeCount; }
    static uint32_t getUniqueSSIDCount() { return uniqueSSIDCount; }
    static uint8_t getHookedCount() { return hookedCount; }
    static uint8_t getAppleHookCount() { return appleHookCount; }
    static const char* getCurrentSSID() { return currentSSID; }
    static const char* getLastProbeSSID();
    
private:
    static bool running;
    static bool confirmed;  // User confirmed warning dialog
    
    // SSID ring buffer
    static SSIDEntry ssidQueue[HOGWASH_SSID_QUEUE_SIZE];
    static uint8_t ssidQueueHead;
    static uint8_t ssidQueueCount;
    
    // Current broadcast SSID
    static char currentSSID[33];
    static uint32_t lastSSIDChange;
    static uint16_t ssidCycleIntervalMs;
    
    // Connected stations
    static std::vector<HookedStation> hookedStations;
    static uint8_t hookedCount;
    static uint8_t appleHookCount;
    
    // Statistics
    static uint32_t probeCount;
    static uint32_t uniqueSSIDCount;
    static uint32_t sessionStartTime;
    
    // XP anti-farm
    static uint16_t sessionProbeXP;
    static bool probeXPCapWarned;
    static const uint16_t PROBE_XP_CAP = 200;
    
    // Channel
    static uint8_t channel;
    
    // Internal methods
    static bool showWarningDialog();
    static void startProbeMonitor();
    static void stopProbeMonitor();
    static void startSoftAP();
    static void stopSoftAP();
    static void updateSoftAPSSID();
    static void checkConnectedStations();
    
    // SSID queue operations
    static bool addSSIDToQueue(const char* ssid, uint32_t timestamp);
    static const char* getLatestSSID();
    static void cycleToNextSSID();
    
    // Probe request callback (registered with esp_wifi)
    static void probeCallback(void* buf, wifi_promiscuous_pkt_type_t type);
    
    // Station event callback
    static void stationEventHandler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data);
    
    // Captive portal
    static bool portalEnabled;
    static bool portalRunning;
    static String portalHTML;
    static void startCaptivePortal();
    static void stopCaptivePortal();
    static void handleCaptivePortal();  // Process DNS and HTTP
    static void loadPortalHTML();       // Load from SD card
};
