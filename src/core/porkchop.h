// Porkchop core state machine
#pragma once

#include <Arduino.h>
#include <functional>
#include <vector>

// Operating modes
enum class PorkchopMode : uint8_t {
    IDLE = 0,              // Main screen, piglet idle
    OINK_MODE = 1,         // Deauth + sniff mode
    DNH_MODE = 2,          // DO NO HAM - passive recon (no attacks)
    WARHOG_MODE = 3,       // Wardriving mode
    PIGGYBLUES_MODE = 4,   // BLE notification spam
    SPECTRUM_MODE = 5,     // WiFi spectrum analyzer
    MENU = 6,              // Menu navigation
    SETTINGS = 7,          // Settings screen
    HASHES = 8,            // View captured handshakes / PMKIDs
    BADGES = 9,            // View achievements (UI: BADGES)
    ABOUT = 10,            // About screen
    XFER = 11,             // WiFi file transfer mode (UI: TRANSFR/XFER)
    COREDUMP = 12,         // Crash/coredump viewer
    DIAGDATA = 13,         // System diagnostics snapshot
    FLEXES = 14,           // Lifetime stats and buffs overlay
    BOAR_BROS = 15,        // Manage excluded networks
    TRACKS = 16,           // WiGLE tracks/uploads
    UNLOCKABLES = 17,      // Secret challenges menu
    BOUNTY = 18,           // View active bounties
    PIGSYNC_DEVICE_SELECT = 19, // PigSync device selection
    PIGSYNC_CALL = 20,     // PigSync active call
    BACON_MODE = 21,       // Hide and seek beacon broadcaster
    JANUS_HOG_MODE = 22,   // Janus Hog (ESP32-C5) UART coprocessor status
    SD_FORMAT = 23,        // SD card format utility
    CHARGING = 24,         // Low power charging mode

    // Legacy aliases (deprecated; remove in v0.2.0)
    CAPTURES = HASHES,
    ACHIEVEMENTS = BADGES,
    FILE_TRANSFER = XFER,
    CRASH_VIEWER = COREDUMP,
    DIAGNOSTICS = DIAGDATA,
    SWINE_STATS = FLEXES,
    WIGLE_MENU = TRACKS,
    BOUNTY_STATUS = BOUNTY,
    MONSTER_C5_MODE = JANUS_HOG_MODE
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

    // Boot mode auto-entry
    bool bootModePending = false;
    PorkchopMode bootModeTarget = PorkchopMode::IDLE;
    uint32_t bootModeStartMs = 0;
    
    // Event queue with max capacity to prevent memory exhaustion
    static constexpr size_t MAX_EVENT_QUEUE_SIZE = 32;  // Prevent runaway allocations
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
