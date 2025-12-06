// Porkchop core state machine
#pragma once

#include <Arduino.h>
#include <functional>
#include <vector>

// Operating modes
enum class PorkchopMode : uint8_t {
    IDLE = 0,       // Main screen, piglet idle
    OINK_MODE,      // Deauth + sniff mode
    WARHOG_MODE,    // Wardriving mode
    MENU,           // Menu navigation
    SETTINGS,       // Settings screen
    CAPTURES,       // View captured handshakes
    ABOUT           // About screen
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
