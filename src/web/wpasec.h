// WPA-SEC distributed cracking service client
// https://wpa-sec.stanev.org/
#pragma once

#include <Arduino.h>
#include <map>

// Upload status for tracking
enum class WPASecUploadStatus {
    NOT_UPLOADED,
    UPLOADED,
    CRACKED
};

class WPASec {
public:
    static void init();
    
    // WiFi connection (standalone, uses otaSSID/otaPassword from config)
    static bool connect();
    static void disconnect();
    static bool isConnected();
    
    // API operations (require WiFi connection)
    static bool fetchResults();                      // GET cracked passwords, cache to SD
    static bool uploadCapture(const char* pcapPath); // POST pcap file to WPA-SEC
    
    // Local cache queries (no WiFi needed)
    static bool loadCache();                         // Load cache from SD
    static bool isCracked(const char* bssid);        // Check if BSSID is cracked
    static String getPassword(const char* bssid);    // Get password for BSSID
    static String getSSID(const char* bssid);        // Get SSID for BSSID (from cache)
    static uint16_t getCrackedCount();               // Total cracked in cache
    
    // Upload tracking
    static bool isUploaded(const char* bssid);       // Check if already uploaded
    static void markUploaded(const char* bssid);     // Mark as uploaded
    
    // Status
    static const char* getLastError();
    static const char* getStatus();
    
private:
    static bool cacheLoaded;
    static char lastError[64];
    static char statusMessage[64];
    
    // Cache: BSSID (no colons, uppercase) -> {ssid, password}
    struct CacheEntry {
        String ssid;
        String password;
    };
    static std::map<String, CacheEntry> crackedCache;
    static std::map<String, bool> uploadedCache;
    
    // File paths
    static constexpr const char* CACHE_FILE = "/wpasec_results.txt";
    static constexpr const char* UPLOADED_FILE = "/wpasec_uploaded.txt";
    
    // API endpoints
    static constexpr const char* API_HOST = "wpa-sec.stanev.org";
    static constexpr const char* RESULTS_PATH = "/?api&key=";
    static constexpr const char* SUBMIT_PATH = "/?submit";
    
    // Helpers
    static String normalizeBSSID(const char* bssid);  // Remove colons, uppercase
    static bool saveCache();
    static bool loadUploadedList();
    static bool saveUploadedList();
};
