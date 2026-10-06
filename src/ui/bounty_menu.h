// Bounty Menu - View bounties to send to kid (Sirloin)
// Porkchop sends wardriven networks to Sirloin for hunting
#pragma once

#include <Arduino.h>
#include <M5Unified.h>

class BountyMenu {
public:
    static void init();
    static void show();
    static void hide();
    static void update();
    static void draw(M5Canvas& canvas);
    static bool isActive() { return active; }
    static void getSelectedInfo(char* out, size_t len);
    
private:
    static uint16_t selectedIndex;   // uint16_t: supports up to 65535 bounties (MAX_SEEN_BSSIDS=5000)
    static uint16_t scrollOffset;    // uint16_t: matches selectedIndex
    static bool active;
    static bool keyWasPressed;
    
    // Layout constants (match boar_bros_menu pattern - no header)
    static const uint8_t VISIBLE_ITEMS = 6;  // 6 items, full canvas
    static const int LINE_H = 17;            // Line height (107px / 6 items)
    static const int COL_LEFT = 4;           // Left margin
    
    static void handleInput();
    static void drawList(M5Canvas& canvas);
    static void drawEmpty(M5Canvas& canvas);
};
