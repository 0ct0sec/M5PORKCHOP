// Settings menu system
#pragma once

#include <Arduino.h>
#include <M5Unified.h>

enum class SettingType {
    TOGGLE,     // ON/OFF
    VALUE,      // Numeric value with min/max
    ACTION,     // Trigger action (like Save)
    TEXT        // Text input (SSID, password, etc.)
};

class SettingsMenu {
public:
    static void init();
    static void update();
    static void draw(M5Canvas& canvas);
    
    static void show();
    static void hide();
    static bool isActive() { return active; }
    static bool shouldExit() { return exitRequested; }
    static void clearExit() { exitRequested = false; }
    static const char* getSelectedDescription();
    
private:
    static bool active;
    static bool exitRequested;
    static bool keyWasPressed;
    static bool editing;  // Currently adjusting a value
    static bool textEditing;  // Currently editing text
    static char textBuffer[80];   // Buffer for text input (max field is 64 chars)
    static uint8_t textLen;
    static uint8_t rootIndex;
    static uint8_t rootScroll;
    static uint8_t groupIndex;
    static uint8_t groupScroll;
    static uint8_t activeGroup;
    static uint8_t textEditId;
    static uint32_t lastInputMs;
    static bool dirtyConfig;
    static bool dirtyPersonality;
    static uint8_t origGpsRxPin;
    static uint8_t origGpsTxPin;
    static uint32_t origGpsBaud;
    static uint8_t origGpsSource;
    static bool    origC5Enabled;
    static uint8_t origC5TxPin;
    static uint8_t origC5RxPin;
    static uint32_t origC5Baud;

    static const uint8_t VISIBLE_ROOT_ITEMS = 5;
    static const uint8_t VISIBLE_GROUP_ITEMS = 4;
    static const uint32_t AUTO_SAVE_MS = 3000;

    static void handleInput();
    static void handleTextInput();
    static void maybeAutoSave();
    static void saveIfDirty(bool showToast);
};
