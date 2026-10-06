// Badges Menu - View unlocked achievements
#pragma once

#include <Arduino.h>
#include <M5Unified.h>

class BadgesMenu {
public:
    static void init();
    static void show();
    static void hide();
    static void update();
    static void draw(M5Canvas& canvas);
    static bool isActive() { return active; }
    static const uint8_t TOTAL_ACHIEVEMENTS = 64;  // 48 base + 12 DNH/BOAR + 3 CLIENT MONITOR + 1 COMPLETIONIST
    
private:
    static uint8_t selectedIndex;
    static uint8_t scrollOffset;
    static bool active;
    static bool keyWasPressed;
    static bool showingDetail;  // Showing achievement detail popup
    
    static const uint8_t VISIBLE_ITEMS = 5;
    
    static void handleInput();
    static void drawDetail(M5Canvas& canvas);
    static void updateBottomOverlay();
};
