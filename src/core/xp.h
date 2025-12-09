// Porkchop RPG XP and Leveling System
#pragma once

#include <M5Unified.h>
#include <Preferences.h>

// XP event types for tracking
enum class XPEvent : uint8_t {
    NETWORK_FOUND,          // +1 XP
    NETWORK_HIDDEN,         // +5 XP
    NETWORK_WPA3,           // +10 XP
    NETWORK_OPEN,           // +3 XP
    HANDSHAKE_CAPTURED,     // +50 XP
    PMKID_CAPTURED,         // +75 XP
    DEAUTH_SENT,            // +2 XP
    DEAUTH_SUCCESS,         // +15 XP
    WARHOG_LOGGED,          // +2 XP
    DISTANCE_KM,            // +25 XP
    BLE_BURST,              // +1 XP
    BLE_APPLE,              // +3 XP
    GPS_LOCK,               // +10 XP
    ML_ROGUE_DETECTED,      // +25 XP
    SESSION_30MIN,          // +50 XP
    SESSION_60MIN,          // +100 XP
    SESSION_120MIN,         // +200 XP
    LOW_BATTERY_CAPTURE     // +20 XP bonus
};

// Achievement bitflags
enum PorkAchievement : uint32_t {
    ACH_NONE            = 0,
    ACH_FIRST_BLOOD     = 1 << 0,   // First handshake
    ACH_CENTURION       = 1 << 1,   // 100 networks in one session
    ACH_MARATHON_PIG    = 1 << 2,   // 10km walked
    ACH_NIGHT_OWL       = 1 << 3,   // Session after midnight
    ACH_GHOST_HUNTER    = 1 << 4,   // 10 hidden networks
    ACH_APPLE_FARMER    = 1 << 5,   // 100 Apple BLE hits
    ACH_WARDRIVER       = 1 << 6,   // 1000 lifetime networks
    ACH_DEAUTH_KING     = 1 << 7,   // 100 successful deauths
    ACH_PMKID_HUNTER    = 1 << 8,   // Capture PMKID
    ACH_WPA3_SPOTTER    = 1 << 9,   // Find WPA3 network
    ACH_GPS_MASTER      = 1 << 10,  // 100 GPS-tagged networks
    ACH_TOUCH_GRASS     = 1 << 11,  // 50km total walked
    ACH_SILICON_PSYCHO  = 1 << 12,  // 5000 lifetime networks
    ACH_CLUTCH_CAPTURE  = 1 << 13,  // Handshake at <10% battery
    ACH_SPEED_RUN       = 1 << 14,  // 50 networks in 10 minutes
    ACH_CHAOS_AGENT     = 1 << 15,  // 1000 BLE packets sent
};

// Persistent XP data structure (stored in NVS)
struct PorkXPData {
    uint32_t totalXP;           // Lifetime XP
    uint32_t achievements;      // Achievement bitfield
    uint32_t lifetimeNetworks;  // Counter
    uint32_t lifetimeHS;        // Counter
    uint32_t lifetimePMKID;     // PMKID counter
    uint32_t lifetimeDeauths;   // Counter
    uint32_t lifetimeDistance;  // Meters
    uint32_t lifetimeBLE;       // BLE packets
    uint32_t hiddenNetworks;    // Hidden network count
    uint32_t wpa3Networks;      // WPA3 network count
    uint32_t gpsNetworks;       // GPS-tagged networks
    uint16_t sessions;          // Session count
    uint8_t  cachedLevel;       // Cached level for quick access
};

// Session-only stats (not persisted)
struct SessionStats {
    uint32_t xp;
    uint32_t networks;
    uint32_t handshakes;
    uint32_t deauths;
    uint32_t distanceM;
    uint32_t blePackets;
    uint32_t startTime;
    uint32_t firstNetworkTime;  // Time first network was found (for speed run)
    bool gpsLockAwarded;
    bool session30Awarded;
    bool session60Awarded;
    bool session120Awarded;
    bool nightOwlAwarded;       // Hunt after midnight
};

class XP {
public:
    static void init();
    static void save();
    
    // XP operations
    static void addXP(XPEvent event);
    static void addXP(uint16_t amount);  // Direct XP add
    
    // Level info
    static uint8_t getLevel();
    static uint32_t getTotalXP();
    static uint32_t getXPForLevel(uint8_t level);
    static uint32_t getXPToNextLevel();
    static uint8_t getProgress();  // 0-100%
    static const char* getTitle();
    static const char* getTitleForLevel(uint8_t level);
    
    // Achievements
    static void unlockAchievement(PorkAchievement ach);
    static bool hasAchievement(PorkAchievement ach);
    static uint32_t getAchievements();
    static const char* getAchievementName(PorkAchievement ach);
    
    // Stats access
    static const PorkXPData& getData();
    static const SessionStats& getSession();
    
    // Session management
    static void startSession();
    static void endSession();
    static void updateSessionTime();  // Check time-based bonuses
    
    // Distance tracking (call from WARHOG)
    static void addDistance(uint32_t meters);
    
    // Draw XP bar on canvas
    static void drawBar(M5Canvas& canvas);
    
    // Level up callback (set by display to show popup)
    static void setLevelUpCallback(void (*callback)(uint8_t oldLevel, uint8_t newLevel));

private:
    static PorkXPData data;
    static SessionStats session;
    static Preferences prefs;
    static bool initialized;
    static void (*levelUpCallback)(uint8_t, uint8_t);
    
    static void load();
    static void checkAchievements();
    static uint8_t calculateLevel(uint32_t xp);
};
