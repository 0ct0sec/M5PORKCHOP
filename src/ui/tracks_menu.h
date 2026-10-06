// Tracks Menu - View wardriving files with sync support
#pragma once

#include <Arduino.h>
#include <M5Unified.h>
#include <vector>
#include <FS.h>
#include <SD.h>

// Upload status for display
enum class WigleFileStatus {
    LOCAL,      // Not uploaded yet
    UPLOADED    // Uploaded to WiGLE
};

struct WigleFileInfo {
    char filename[48];
    uint32_t fileSize;
    uint32_t networkCount;  // Approximate based on file size
    WigleFileStatus status;
};

// Sync state machine for WiGLE operations
enum class WigleSyncState {
    IDLE,
    CONNECTING_WIFI,
    FREEING_MEMORY,
    UPLOADING,
    FETCHING_STATS,
    COMPLETE,
    ERROR
};

class TracksMenu {
public:
    static void init();
    static void show();
    static void hide();
    static void update();
    static void draw(M5Canvas& canvas);
    static bool isActive() { return active; }
    static size_t getCount() { return files.size(); }
    static void getSelectedInfo(char* out, size_t len);
    
private:
    static std::vector<WigleFileInfo> files;
    static uint8_t selectedIndex;
    static uint8_t scrollOffset;
    static bool active;
    static bool keyWasPressed;
    static bool detailViewActive;   // File detail view
    static bool nukeConfirmActive;  // Nuke confirmation modal
    
    static const uint8_t VISIBLE_ITEMS = 5;
    
    static void scanFiles();
    static void handleInput();
    static void drawDetailView(M5Canvas& canvas);
    static void drawNukeConfirm(M5Canvas& canvas);
    static void nukeTrack();
    static void formatSize(char* out, size_t len, uint32_t bytes);
    
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
    
    // WiGLE Sync modal state
    static bool syncModalActive;
    static WigleSyncState syncState;
    static char syncStatusText[48];
    static uint8_t syncProgress;
    static uint8_t syncTotal;
    static unsigned long syncStartTime;
    static uint8_t syncUploaded;
    static uint8_t syncFailed;
    static uint8_t syncSkipped;
    static bool syncStatsFetched;
    static char syncError[48];
    static bool reconWasRunning;

    // Sync operations
    static void startSync();
    static void processSyncState();
    static void drawSyncModal(M5Canvas& canvas);
    static void cancelSync();
    static bool connectToWiFi();
    static void disconnectWiFi();
    
    // Sync progress callback (static for C-style callback)
    static void onSyncProgress(const char* status, uint8_t progress, uint8_t total);
};
