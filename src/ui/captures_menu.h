// Captures Menu - View saved handshake captures
#pragma once

#include <Arduino.h>
#include <M5Unified.h>
#include <vector>

// WPA-SEC status for display
enum class CaptureStatus {
    LOCAL,      // Not uploaded yet
    UPLOADED,   // Uploaded, waiting for crack
    CRACKED     // Password found!
};

struct CaptureInfo {
    String filename;
    String ssid;
    String bssid;
    uint32_t fileSize;
    time_t captureTime;  // File modification time
    bool isPMKID;        // true = .22000 PMKID, false = .pcap handshake
    CaptureStatus status; // WPA-SEC status
    String password;      // Cracked password (if status == CRACKED)
};

class CapturesMenu {
public:
    static void init();
    static void show();
    static void hide();
    static void update();
    static void draw(M5Canvas& canvas);
    static bool isActive() { return active; }
    static String getSelectedBSSID();
    static size_t getCount() { return captures.size(); }
    
private:
    static std::vector<CaptureInfo> captures;
    static uint8_t selectedIndex;
    static uint8_t scrollOffset;
    static bool active;
    static bool keyWasPressed;
    static bool nukeConfirmActive;  // Nuke confirmation modal
    static bool detailViewActive;   // Password detail view
    static bool connectingWiFi;     // WiFi connection in progress
    static bool uploadingFile;      // Upload in progress
    static bool refreshingResults;  // Fetching WPA-SEC results
    
    static const uint8_t VISIBLE_ITEMS = 5;
    
    static void scanCaptures();
    static void handleInput();
    static void drawNukeConfirm(M5Canvas& canvas);
    static void drawDetailView(M5Canvas& canvas);
    static void drawConnecting(M5Canvas& canvas);
    static void nukeLoot();
    static void updateWPASecStatus();
    static void uploadSelected();
    static void refreshResults();
    static String formatTime(time_t t);
};
