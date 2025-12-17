// HOG ON SPECTRUM Mode - WiFi Spectrum Analyzer
#pragma once

#include <Arduino.h>
#include <M5Unified.h>
#include <vector>
#include <esp_wifi_types.h>

// Client monitoring constants
#define MAX_SPECTRUM_CLIENTS 8
#define CLIENT_STALE_TIMEOUT_MS 30000  // 30s before client considered gone
#define VISIBLE_CLIENTS 4              // How many fit on screen
#define SIGNAL_LOST_TIMEOUT_MS 15000   // 15s no beacon = signal lost
#define CLIENT_BEEP_LIMIT 4            // Only beep for first N clients

// Client tracking for monitored network
struct SpectrumClient {
    uint8_t mac[6];
    int8_t rssi;
    uint32_t lastSeen;
    const char* vendor;  // Cached OUI lookup
};

struct SpectrumNetwork {
    uint8_t bssid[6];
    char ssid[33];
    uint8_t channel;         // 1-13
    int8_t rssi;             // Latest RSSI
    uint32_t lastSeen;       // millis() of last beacon
    wifi_auth_mode_t authmode; // Security type (OPEN/WEP/WPA/WPA2/WPA3)
    bool hasPMF;             // Protected Management Frames (immune to deauth)
    bool isHidden;           // Hidden SSID (beacon had empty SSID)
    bool wasRevealed;        // SSID was revealed via probe response
    // Client tracking (only populated when monitoring THIS network)
    SpectrumClient clients[MAX_SPECTRUM_CLIENTS];
    uint8_t clientCount;
};

// MAC comparison helper [P8]
inline bool macEqual(const uint8_t* a, const uint8_t* b) {
    return memcmp(a, b, 6) == 0;
}

class SpectrumMode {
public:
    static void init();
    static void start();
    static void stop();
    static void update();
    static void draw(M5Canvas& canvas);
    static bool isRunning() { return running; }
    
    // For promiscuous callback - updates network RSSI
    static void onBeacon(const uint8_t* bssid, uint8_t channel, int8_t rssi, const char* ssid, wifi_auth_mode_t authmode, bool hasPMF, bool isProbeResponse);
    
    // Bottom bar info
    static String getSelectedInfo();
    
    // Client monitoring accessors [P3]
    static bool isMonitoring() { return monitoringNetwork; }
    static String getMonitoredSSID();
    static int getClientCount();
    static uint8_t getMonitoredChannel() { return monitoredChannel; }
    
private:
    static bool running;
    static volatile bool busy;       // Guard against callback race
    static std::vector<SpectrumNetwork> networks;
    static float viewCenterMHz;      // Center of visible spectrum
    static float viewWidthMHz;       // Visible bandwidth
    static int selectedIndex;        // Currently highlighted network
    static uint32_t lastUpdateTime;
    static bool keyWasPressed;
    static uint8_t currentChannel;   // Current hop channel
    static uint32_t lastHopTime;     // Last channel hop time
    static uint32_t startTime;       // When mode started (for achievement)
    
    // Deferred logging for revealed SSIDs (avoid Serial in callback)
    static volatile bool pendingReveal;
    static char pendingRevealSSID[33];
    
    // Client monitoring state [P1] [P2]
    static bool monitoringNetwork;       // True when locked on network
    static int monitoredNetworkIndex;    // Index of network being monitored
    static uint8_t monitoredBSSID[6];    // [P2] Store BSSID, not just index!
    static uint8_t monitoredChannel;     // Locked channel
    static int clientScrollOffset;       // For scrolling client list
    static int selectedClientIndex;      // Currently highlighted client
    static uint32_t lastClientPrune;     // Last stale client cleanup
    static uint8_t clientsDiscoveredThisSession;  // For limiting beeps
    static volatile bool pendingClientBeep;       // Deferred beep for new client
    
    static void handleInput();
    static void handleClientMonitorInput();  // Input when monitoring
    static void drawSpectrum(M5Canvas& canvas);
    static void drawClientOverlay(M5Canvas& canvas);  // Client list overlay
    static void drawGaussianLobe(M5Canvas& canvas, float centerFreqMHz, int8_t rssi, bool filled);
    static void drawAxis(M5Canvas& canvas);
    static void drawChannelMarkers(M5Canvas& canvas);
    static void pruneStale();            // Remove networks not seen recently
    static void pruneStaleClients();     // Remove clients not seen recently
    
    // Client monitoring control
    static void enterClientMonitor();    // Enter overlay mode
    static void exitClientMonitor();     // Return to spectrum
    static void deauthClient(int idx);  // Send deauth burst to selected client
    
    // Data frame processing
    static void processDataFrame(const uint8_t* payload, uint16_t len, int8_t rssi);
    static void trackClient(const uint8_t* bssid, const uint8_t* clientMac, int8_t rssi);
    
    // Coordinate mapping
    static int freqToX(float freqMHz);
    static int rssiToY(int8_t rssi);
    static float channelToFreq(uint8_t channel);
    
    // Security helpers
    static bool isVulnerable(wifi_auth_mode_t mode);
    static const char* authModeToShortString(wifi_auth_mode_t mode);
    static bool detectPMF(const uint8_t* payload, uint16_t len);
    
    // Promiscuous mode
    static void promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type);
};
