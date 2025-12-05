// Display management for M5Cardputer
#pragma once

#include <M5Unified.h>

// Display layout constants (240x135 screen)
#define DISPLAY_W 240
#define DISPLAY_H 135
#define TOP_BAR_H 14
#define BOTTOM_BAR_H 14
#define MAIN_H (DISPLAY_H - TOP_BAR_H - BOTTOM_BAR_H)

// Colors - Pink on Black theme
#define COLOR_BG TFT_BLACK
#define COLOR_FG 0xFC9F  // Hot pink
#define COLOR_ACCENT 0xF81F  // Magenta/Pink
#define COLOR_WARNING TFT_YELLOW
#define COLOR_DANGER TFT_RED
#define COLOR_SUCCESS TFT_GREEN

class Display {
public:
    static void init();
    static void update();
    static void clear();
    
    // Canvas access for direct drawing
    static M5Canvas& getTopBar() { return topBar; }
    static M5Canvas& getMain() { return mainCanvas; }
    static M5Canvas& getBottomBar() { return bottomBar; }
    
    // Helper functions
    static void pushAll();
    static void showInfoBox(const String& title, const String& line1, 
                           const String& line2 = "", bool blocking = true);
    static bool showConfirmBox(const String& title, const String& message);
    static void showProgress(const String& title, uint8_t percent);
    
    // Status indicators
    static void setGPSStatus(bool hasFix);
    static void setWiFiStatus(bool connected);
    static void setMLStatus(bool active);
    
private:
    static M5Canvas topBar;
    static M5Canvas mainCanvas;
    static M5Canvas bottomBar;
    
    static bool gpsStatus;
    static bool wifiStatus;
    static bool mlStatus;
    
    static void drawTopBar();
    static void drawBottomBar();
};
