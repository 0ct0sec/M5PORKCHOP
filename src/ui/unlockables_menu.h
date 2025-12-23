// Unlockables Menu - Secret challenges for the worthy
#pragma once

#include <M5GFX.h>
#include <cstdint>

// Unlockable item definition
struct UnlockableItem {
    const char* name;       // Display name: "PROPHECY"
    const char* hint;       // Hint for bottom bar
    const char* hashHex;    // SHA256 of unlock phrase (64 hex chars)
    uint8_t bitIndex;       // Bit position in unlockables field (0-31)
};

class UnlockablesMenu {
public:
    static void init();
    static void show();
    static void hide();
    static void update();
    static void draw(M5Canvas& canvas);
    static bool isActive() { return active; }
    static bool wantsExit() { return exitRequested; }
    static void clearExit() { exitRequested = false; }
    
private:
    static void handleInput();
    static void handleTextInput();
    static void drawTextInput(M5Canvas& canvas);
    static bool validatePhrase(const char* phrase, const char* expectedHash);
    static void updateBottomOverlay();
    
    static uint8_t selectedIndex;
    static uint8_t scrollOffset;
    static bool active;
    static bool keyWasPressed;
    static bool exitRequested;
    static bool textEditing;
    static String textBuffer;
    
    static const uint8_t VISIBLE_ITEMS = 5;
};
