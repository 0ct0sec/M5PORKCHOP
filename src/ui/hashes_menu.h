// Hashes Menu - View saved handshake captures
#pragma once

#include <Arduino.h>
#include <M5Unified.h>
#include <vector>
#include <FS.h>
#include <SD.h>

// WPA-SEC status for display
enum class CaptureStatus {
    LOCAL,      // Not uploaded yet
    UPLOADED,   // Uploaded, waiting for crack
    CRACKED     // Password found!
};

struct CaptureInfo {
    char filename[48];
    char ssid[33];
    char bssid[18];
    bool isPMKID;         // Packed after bssid — eliminates 4 bytes padding
    uint32_t fileSize;
    time_t captureTime;   // File modification time
    CaptureStatus status; // WPA-SEC status
    char password[64];    // Cracked password (if status == CRACKED)
};

// Sync state machine for WPA-SEC operations
enum class SyncState {
    IDLE,
    CONNECTING_WIFI,
    FREEING_MEMORY,
    UPLOADING,
    DOWNLOADING_POTFILE,
    COMPLETE,
    ERROR
};

class HashesMenu {
public:
    static void init();
    static void show();
    static void hide();
    static void update();
    static void draw(M5Canvas& canvas);
    
    // Emergency cleanup for low heap situations
    static void emergencyCleanup();
    static bool isActive() { return active; }
    static const char* getSelectedBSSID();
    static size_t getCount() { return captures.size(); }
    
private:
    static std::vector<CaptureInfo> captures;
    static uint8_t selectedIndex;
    static uint8_t scrollOffset;
    static bool active;
    static bool keyWasPressed;
    static bool nukeConfirmActive;  // Nuke confirmation modal
    static bool detailViewActive;   // Password detail view
    
    static const uint8_t VISIBLE_ITEMS = 5;
    
    static bool scanCaptures();  // Returns true if successful, false if SD access failed
    static void handleInput();
    static void drawNukeConfirm(M5Canvas& canvas);
    static void drawDetailView(M5Canvas& canvas);
    static void nukeLoot();
    static void updateWPASecStatus();
    static void formatTime(char* out, size_t len, time_t t);
    static const size_t MAX_CAPTURES = 100;
    
    // Async scan state
    static bool scanInProgress;
    static bool scanDeferredHeap;   // true when scan was skipped due to heap pressure
    static unsigned long lastScanTime;
    static const unsigned long SCAN_DELAY = 50; // ms between scan chunks
    static char scanBaseDir[32];   // Actual directory used for current scan
    static File scanDir;
    static File currentFile;
    static bool scanComplete;
    static size_t scanProgress;
    static const size_t SCAN_CHUNK_SIZE = 5; // files to process per chunk
    
    // Async scan processing
    static void processAsyncScan();
    
    // Async WPA-SEC status update state
    static bool wpasecUpdateInProgress;
    static unsigned long lastWpasecUpdateTime;
    static size_t wpasecUpdateProgress;
    static const unsigned long WPASEC_UPDATE_DELAY = 25; // ms between updates
    static const size_t WPASEC_UPDATE_CHUNK_SIZE = 3; // captures to process per chunk
    
    // Async WPA-SEC status update processing
    static void processAsyncWPASecUpdate();
    
    // WPA-SEC Sync modal state
    static bool syncModalActive;
    static SyncState syncState;
    static char syncStatusText[48];
    static uint8_t syncProgress;
    static uint8_t syncTotal;
    static unsigned long syncStartTime;
    static uint8_t syncUploaded;
    static uint8_t syncFailed;
    static uint16_t syncCracked;
    static char syncError[48];
    
    // Sync operations
    static void startSync();
    static void processSyncState();
    static void drawSyncModal(M5Canvas& canvas);
    static void cancelSync();
    static bool connectToWiFi();
    static void disconnectWiFi();
    
    // Sync progress callback (static for C-style callback)
    static void onSyncProgress(const char* status, uint8_t progress, uint8_t total);

    // Hint rotation
    static uint8_t hintIndex;
    static const char* const HINTS[];
    static const uint8_t HINT_COUNT = 5;
};
