// Porkchop core state machine
#pragma once

#include <Arduino.h>
#include <functional>
#include <vector>

// Operating modes
enum class PorkchopMode : uint8_t {
    IDLE = 0,       // Main screen, piglet idle
    OINK_MODE,      // Deauth + sniff mode
    DNH_MODE,       // DO NO HAM - passive recon (no attacks)
    WARHOG_MODE,    // Wardriving mode
    PIGGYBLUES_MODE,// BLE notification spam
    HOGWASH_MODE,   // Karma AP - probe response honeypot
    SPECTRUM_MODE,  // WiFi spectrum analyzer
    MENU,           // Menu navigation
    SETTINGS,       // Settings screen
    CAPTURES,       // View captured handshakes
    ACHIEVEMENTS,   // View achievements
    ABOUT,          // About screen
    FILE_TRANSFER,  // WiFi file transfer mode
    LOG_VIEWER,     // View SD card logs
    SWINE_STATS,    // Lifetime stats and buffs overlay
    BOAR_BROS,      // Manage excluded networks
    WIGLE_MENU,     // WiGLE file uploads
    UNLOCKABLES,    // Secret challenges menu
    CALL_PAPA_MODE  // BLE sync receiver (from Sirloin)
};

// Events for async callbacks
enum class PorkchopEvent : uint8_t {
    NONE = 0,
    MODE_CHANGE,
    ML_RESULT,
    GPS_FIX,
    GPS_LOST,
    HANDSHAKE_CAPTURED,
    NETWORK_FOUND,
    DEAUTH_SENT,
    ROGUE_AP_DETECTED,
    OTA_AVAILABLE,
    LOW_BATTERY
};

// Event callback type
using EventCallback = std::function<void(PorkchopEvent, void*)>;

class Porkchop {
public:
    Porkchop();
    
    void init();
    void update();
    
    // Mode control
    void setMode(PorkchopMode mode);
    PorkchopMode getMode() const { return currentMode; }
    
    // Event system
    void postEvent(PorkchopEvent event, void* data = nullptr);
    void registerCallback(PorkchopEvent event, EventCallback callback);
    
    // Stats
    uint32_t getUptime() const;
    uint16_t getHandshakeCount() const;  // Gets from OinkMode
    uint16_t getNetworkCount() const;     // Gets from OinkMode
    uint16_t getDeauthCount() const;      // Gets from OinkMode
    
private:
    PorkchopMode currentMode;
    PorkchopMode previousMode;
    
    uint32_t startTime;
    uint16_t handshakeCount;
    uint16_t networkCount;
    uint16_t deauthCount;
    
    // Event queue
    struct EventItem {
        PorkchopEvent event;
        void* data;
    };
    std::vector<EventItem> eventQueue;
    std::vector<std::pair<PorkchopEvent, EventCallback>> callbacks;
    
    void processEvents();
    void handleInput();
    void updateMode();
};
