// Captures Menu - View saved handshake captures
#pragma once

#include <Arduino.h>
#include <M5Unified.h>
#include <vector>

struct CaptureInfo {
    String filename;
    String ssid;
    String bssid;
    uint32_t fileSize;
    time_t captureTime;  // File modification time
};

class CapturesMenu {
public:
    static void init();
    static void show();
    static void hide();
    static void update();
    static void draw(M5Canvas& canvas);
    static bool isActive() { return active; }
    
private:
    static std::vector<CaptureInfo> captures;
    static uint8_t selectedIndex;
    static uint8_t scrollOffset;
    static bool active;
    static bool keyWasPressed;
    
    static const uint8_t VISIBLE_ITEMS = 5;
    
    static void scanCaptures();
    static void handleInput();
    static String formatTime(time_t t);
    static String extractSSID(const String& filename);
};
