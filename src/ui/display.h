// Display management for M5Cardputer
#pragma once

#include <M5Unified.h>

// Forward declarations
enum class PorkchopMode : uint8_t;

// Display layout constants (240x135 screen)
#define DISPLAY_W 240
#define DISPLAY_H 135
#define TOP_BAR_H 14
#define BOTTOM_BAR_H 14
#define MAIN_H (DISPLAY_H - TOP_BAR_H - BOTTOM_BAR_H)

// Colors - Pink on Black theme (single pink only)
#define COLOR_BG TFT_BLACK
#define COLOR_FG 0xFD75  // Piglet pink
#define COLOR_ACCENT COLOR_FG  // Same pink for everything
#define COLOR_WARNING COLOR_FG  // Pink only
#define COLOR_DANGER COLOR_FG   // Pink only
#define COLOR_SUCCESS COLOR_FG  // Pink only

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
    static void showBootSplash();  // 3-screen boot animation
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
    static void drawModeInfo(M5Canvas& canvas, PorkchopMode mode);
    static void drawSettingsScreen(M5Canvas& canvas);
    static void drawAboutScreen(M5Canvas& canvas);
};
