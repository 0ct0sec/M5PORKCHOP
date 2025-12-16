// WiGLE Menu - View and upload wardriving files to wigle.net
#pragma once

#include <Arduino.h>
#include <M5Unified.h>
#include <vector>

// Upload status for display
enum class WigleFileStatus {
    LOCAL,      // Not uploaded yet
    UPLOADED    // Uploaded to WiGLE
};

struct WigleFileInfo {
    String filename;
    String fullPath;
    uint32_t fileSize;
    uint32_t networkCount;  // Approximate based on file size
    WigleFileStatus status;
};

class WigleMenu {
public:
    static void init();
    static void show();
    static void hide();
    static void update();
    static void draw(M5Canvas& canvas);
    static bool isActive() { return active; }
    static size_t getCount() { return files.size(); }
    
private:
    static std::vector<WigleFileInfo> files;
    static uint8_t selectedIndex;
    static uint8_t scrollOffset;
    static bool active;
    static bool keyWasPressed;
    static bool detailViewActive;   // File detail view
    static bool connectingWiFi;     // WiFi connection in progress
    static bool uploadingFile;      // Upload in progress
    
    static const uint8_t VISIBLE_ITEMS = 5;
    
    static void scanFiles();
    static void handleInput();
    static void drawDetailView(M5Canvas& canvas);
    static void drawConnecting(M5Canvas& canvas);
    static void uploadSelected();
    static String formatSize(uint32_t bytes);
};
