// WPA-SEC distributed cracking service client
// https://wpa-sec.stanev.org/
#pragma once

#include <Arduino.h>
#include <vector>
#include "../core/heap_policy.h"

// Upload status for tracking
enum class WPASecUploadStatus {
    NOT_UPLOADED,
    UPLOADED,
    CRACKED
};

// Sync operation result
struct WPASecSyncResult {
    bool success;
    uint8_t uploaded;
    uint8_t failed;
    uint8_t skipped;     // Already uploaded
    uint16_t cracked;    // Total cracked after potfile download
    uint16_t newCracked; // New cracks found this sync
    char error[48];
};

// Sync progress callback for UI updates
typedef void (*WPASecProgressCallback)(const char* status, uint8_t progress, uint8_t total);

class WPASec {
public:
    // Sync status
    static bool isBusy();

    // Local cache queries (no WiFi needed)
    static bool loadCache();                         // Load cache from SD
    static bool isCracked(const char* bssid);        // Check if BSSID is cracked
    static const char* getPassword(const char* bssid);  // Get password for BSSID (returns "" if not found)
    static const char* getSSID(const char* bssid);      // Get SSID for BSSID (returns "" if not found)
    static uint16_t getCrackedCount();               // Total cracked in cache
    static void normalizeBSSID_Char(const char* bssid, char* output, size_t outLen);

    // Upload tracking
    static bool isUploaded(const char* bssid);       // Check if already uploaded
    static void markAsUploaded(const char* bssid);   // Mark BSSID as uploaded

    // Batch upload mode (reduces SD writes from N to 1)
    static void beginBatchUpload();                  // Start batch mode
    static void endBatchUpload();                    // End batch mode and save

    // Network operations (require WiFi + sufficient heap)
    static bool hasApiKey();                         // Check if WPA-SEC key configured
    static bool canSync();                           // Check heap requirements (~35KB)
    static WPASecSyncResult syncCaptures(WPASecProgressCallback cb = nullptr);  // Full sync

    // Status
    static const char* getLastError();

    /**
     * @brief Free cached WPA-SEC results from memory.
     *
     * This releases the internal cracked and uploaded vectors to return heap
     * space prior to large TLS operations.  After calling this, the cache
     * will be reloaded from disk on the next lookup or fetch.
     */
    static void freeCacheMemory();

    // Async cache loading support
    static bool isCacheLoaded() { return cacheLoaded; }

 private:
    static bool cacheLoaded;
    static char lastError[64];
    static volatile bool busy;
    static bool batchMode;  // Batch upload mode flag

    // Flat cache entries — no std::map, no String. One contiguous vector each.
    struct CrackedEntry {
        char bssid[13];    // Normalized BSSID (no colons, uppercase)
        char ssid[33];
        char password[64];
    };
    struct UploadedEntry {
        char bssid[13];
    };
    static std::vector<CrackedEntry> crackedCache;
    static std::vector<UploadedEntry> uploadedCache;

    static const CrackedEntry* findCracked(const char* normalizedBssid);
    static bool findUploaded(const char* normalizedBssid);

    // Helpers
    static bool loadUploadedList();
    static bool saveUploadedList();

    // Network helpers (internal)
    static bool uploadSingleCapture(const char* filepath, const char* bssid);
    static bool downloadPotfile(uint16_t& newCracks);
};
