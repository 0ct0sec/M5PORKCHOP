// Porkchop RPG XP and Leveling System
#pragma once

#include <M5Unified.h>
#include <Preferences.h>

// Class tiers (every 5 levels)
enum class PorkClass : uint8_t {
    SH0AT    = 0,   // L1-5   - Script kiddie
    SN1FF3R  = 1,   // L6-10  - Packet capture
    PWNER    = 2,   // L11-15 - Active attacks
    R00T     = 3,   // L16-20 - Wardriving, root
    R0GU3    = 4,   // L21-25 - Rogue APs
    EXPL01T  = 5,   // L26-30 - Famous exploits
    WARL0RD  = 6,   // L31-35 - WiFi mastery
    L3G3ND   = 7    // L36-40 - Legendary hackers
};

// Title overrides - special playstyle-based titles
enum class TitleOverride : uint8_t {
    NONE          = 0,   // Use standard level-based rank
    SH4D0W_H4M    = 1,   // Unlocked by ACH_SHADOW_BROKER (500 passive nets)
    P4C1F1ST_P0RK = 2,   // Unlocked by ACH_WITNESS_PROTECT (25 bros)
    Z3N_M4ST3R    = 3    // Unlocked by ACH_ZEN_MASTER
};

// XP event types for tracking
enum class XPEvent : uint8_t {
    NETWORK_FOUND,          // +1 XP
    NETWORK_HIDDEN,         // +3 XP
    NETWORK_WPA3,           // +10 XP
    NETWORK_OPEN,           // +3 XP
    NETWORK_WEP,            // +5 XP (rare find!)
    HANDSHAKE_CAPTURED,     // +50 XP
    PMKID_CAPTURED,         // +75 XP
    DEAUTH_SENT,            // +2 XP
    DEAUTH_SUCCESS,         // +15 XP
    WARHOG_LOGGED,          // +2 XP
    DISTANCE_KM,            // +25 XP
    BLE_BURST,              // +2 XP
    BLE_APPLE,              // +3 XP
    BLE_ANDROID,            // +2 XP
    BLE_SAMSUNG,            // +2 XP
    BLE_WINDOWS,            // +2 XP
    GPS_LOCK,               // +5 XP
    ML_ROGUE_DETECTED,      // +25 XP
    SESSION_30MIN,          // +10 XP
    SESSION_60MIN,          // +25 XP
    SESSION_120MIN,         // +50 XP
    LOW_BATTERY_CAPTURE,    // +20 XP bonus
    // DO NO HAM / BOAR BROS events (v0.1.4+)
    DNH_NETWORK_PASSIVE,    // +2 XP - network found in passive mode
    DNH_PMKID_GHOST,        // +100 XP - PMKID captured passively (rare!)
    BOAR_BRO_ADDED,         // +5 XP - added network to BOAR BROS
    BOAR_BRO_MERCY          // +15 XP - excluded mid-attack target
};

// Achievement bitflags (uint64_t for 60 achievements)
enum PorkAchievement : uint64_t {
    ACH_NONE            = 0,
    // Original 17 achievements (bits 0-16)
    ACH_FIRST_BLOOD     = 1ULL << 0,   // First handshake
    ACH_CENTURION       = 1ULL << 1,   // 100 networks in one session
    ACH_MARATHON_PIG    = 1ULL << 2,   // 10km walked in session
    ACH_NIGHT_OWL       = 1ULL << 3,   // Session after midnight
    ACH_GHOST_HUNTER    = 1ULL << 4,   // 10 hidden networks
    ACH_APPLE_FARMER    = 1ULL << 5,   // 100 Apple BLE hits
    ACH_WARDRIVER       = 1ULL << 6,   // 1000 lifetime networks
    ACH_DEAUTH_KING     = 1ULL << 7,   // 100 successful deauths
    ACH_PMKID_HUNTER    = 1ULL << 8,   // Capture PMKID
    ACH_WPA3_SPOTTER    = 1ULL << 9,   // Find WPA3 network
    ACH_GPS_MASTER      = 1ULL << 10,  // 100 GPS-tagged networks
    ACH_TOUCH_GRASS     = 1ULL << 11,  // 50km total walked
    ACH_SILICON_PSYCHO  = 1ULL << 12,  // 5000 lifetime networks
    ACH_CLUTCH_CAPTURE  = 1ULL << 13,  // Handshake at <10% battery
    ACH_SPEED_RUN       = 1ULL << 14,  // 50 networks in 10 minutes
    ACH_CHAOS_AGENT     = 1ULL << 15,  // 1000 BLE packets sent
    ACH_NIETZSWINE      = 1ULL << 16,  // Stare at spectrum for 15 minutes
    
    // New achievements (bits 17-46)
    // Network milestones
    ACH_TEN_THOUSAND    = 1ULL << 17,  // 10,000 networks lifetime
    ACH_NEWB_SNIFFER    = 1ULL << 18,  // First 10 networks
    ACH_FIVE_HUNDRED    = 1ULL << 19,  // 500 networks in session
    ACH_OPEN_SEASON     = 1ULL << 20,  // 50 open networks
    ACH_WEP_LOLZER      = 1ULL << 21,  // Find a WEP network
    
    // Handshake/PMKID milestones
    ACH_HANDSHAKE_HAM   = 1ULL << 22,  // 10 handshakes lifetime
    ACH_FIFTY_SHAKES    = 1ULL << 23,  // 50 handshakes lifetime
    ACH_PMKID_FIEND     = 1ULL << 24,  // 10 PMKIDs captured
    ACH_TRIPLE_THREAT   = 1ULL << 25,  // 3 handshakes in session
    ACH_HOT_STREAK      = 1ULL << 26,  // 5 handshakes in session
    
    // Deauth milestones
    ACH_FIRST_DEAUTH    = 1ULL << 27,  // First successful deauth
    ACH_DEAUTH_THOUSAND = 1ULL << 28,  // 1000 successful deauths
    ACH_RAMPAGE         = 1ULL << 29,  // 10 deauths in session
    
    // Distance/WARHOG milestones
    ACH_HALF_MARATHON   = 1ULL << 30,  // 21km in session
    ACH_HUNDRED_KM      = 1ULL << 31,  // 100km lifetime
    ACH_GPS_ADDICT      = 1ULL << 32,  // 500 GPS-tagged networks
    ACH_ULTRAMARATHON   = 1ULL << 33,  // 50km in session
    
    // BLE/PIGGYBLUES milestones
    ACH_PARANOID_ANDROID = 1ULL << 34, // 100 Android FastPair spam
    ACH_SAMSUNG_SPRAY   = 1ULL << 35,  // 100 Samsung spam
    ACH_WINDOWS_PANIC   = 1ULL << 36,  // 100 Windows SwiftPair spam
    ACH_BLE_BOMBER      = 1ULL << 37,  // 5000 BLE packets
    ACH_OINKAGEDDON     = 1ULL << 38,  // 10000 BLE packets
    
    // Time/session milestones
    ACH_SESSION_VET     = 1ULL << 39,  // 100 sessions
    ACH_FOUR_HOUR_GRIND = 1ULL << 40,  // 4 hour session
    ACH_EARLY_BIRD      = 1ULL << 41,  // Active 5-7am
    ACH_WEEKEND_WARRIOR = 1ULL << 42,  // Session on weekend
    
    // Special/rare
    ACH_ROGUE_SPOTTER   = 1ULL << 43,  // ML detects rogue AP
    ACH_HIDDEN_MASTER   = 1ULL << 44,  // 50 hidden networks
    ACH_WPA3_HUNTER     = 1ULL << 45,  // 25 WPA3 networks
    ACH_MAX_LEVEL       = 1ULL << 46,  // Reach level 40
    ACH_ABOUT_JUNKIE    = 1ULL << 47,  // Press Enter 5x in About screen
    
    // DO NO HAM achievements (bits 48-52) - pacifist/stealth playstyle
    ACH_GOING_DARK      = 1ULL << 48,  // 5 minutes in passive mode this session
    ACH_GHOST_PROTOCOL  = 1ULL << 49,  // 30 min passive + 50 networks in session
    ACH_SHADOW_BROKER   = 1ULL << 50,  // 500 passive networks lifetime (unlocks SH4D0W_H4M)
    ACH_SILENT_ASSASSIN = 1ULL << 51,  // First PMKID captured in passive mode
    ACH_ZEN_MASTER      = 1ULL << 52,  // 5 passive PMKIDs (unlocks Z3N_M4ST3R title)
    
    // BOAR BROS achievements (bits 53-57) - network protection playstyle
    ACH_FIRST_BRO       = 1ULL << 53,  // First network added to BOAR BROS
    ACH_FIVE_FAMILIES   = 1ULL << 54,  // 5 bros added lifetime
    ACH_MERCY_MODE      = 1ULL << 55,  // First mid-attack exclusion
    ACH_WITNESS_PROTECT = 1ULL << 56,  // 25 bros added lifetime (unlocks P4C1F1ST_P0RK)
    ACH_FULL_ROSTER     = 1ULL << 57,  // Currently have 50 bros (max limit)
    
    // Combined DO NO HAM + BOAR BROS achievements (bits 58-59)
    ACH_INNER_PEACE     = 1ULL << 58,  // 1hr passive + 10 bros + 0 deauths this session
    ACH_PACIFIST_RUN    = 1ULL << 59,  // 50+ networks discovered, all added to bros
    
    // CLIENT MONITOR achievements (bits 60-62) - v0.1.6 hunting features
    ACH_QUICK_DRAW      = 1ULL << 60,  // Deauth 5 clients in under 30 seconds
    ACH_DEAD_EYE        = 1ULL << 61,  // Deauth within 2 seconds of entering monitor
    ACH_HIGH_NOON       = 1ULL << 62,  // Deauth during 12:00 hour (noon)
};

// Persistent XP data structure (stored in NVS)
struct PorkXPData {
    uint32_t totalXP;           // Lifetime XP
    uint64_t achievements;      // Achievement bitfield (expanded for 60 achievements)
    uint32_t lifetimeNetworks;  // Counter
    uint32_t lifetimeHS;        // Counter
    uint32_t lifetimePMKID;     // PMKID counter
    uint32_t lifetimeDeauths;   // Counter
    uint32_t lifetimeDistance;  // Meters
    uint32_t lifetimeBLE;       // BLE packets
    uint32_t hiddenNetworks;    // Hidden network count
    uint32_t wpa3Networks;      // WPA3 network count
    uint32_t gpsNetworks;       // GPS-tagged networks
    uint32_t openNetworks;      // Open network count (new)
    uint32_t androidBLE;        // Android FastPair count (new)
    uint32_t samsungBLE;        // Samsung BLE count (new)
    uint32_t windowsBLE;        // Windows SwiftPair count (new)
    uint16_t sessions;          // Session count
    uint8_t  cachedLevel;       // Cached level for quick access
    bool     wepFound;          // WEP network ever found (new)
    // DO NO HAM / BOAR BROS persistent counters (v0.1.4+)
    uint32_t passiveNetworks;   // Networks found in DNH mode
    uint32_t passivePMKIDs;     // PMKIDs captured in DNH mode
    uint32_t passiveTimeS;      // Seconds in pure passive mode (no deauth ever)
    uint32_t boarBrosAdded;     // Total networks added to BOAR BROS
    uint32_t mercyCount;        // Mid-attack exclusions (mercy kills)
    TitleOverride titleOverride; // Player-selected title override
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
    bool session240Awarded;     // 4 hour session (new)
    bool earlyBirdAwarded;      // 5-7am session (new)
    bool weekendWarriorAwarded; // Weekend session (new)
    bool rogueSpotterAwarded;   // ML rogue detected (new)
    // DO NO HAM / BOAR BROS session counters (v0.1.4+)
    uint32_t passiveNetworks;   // Networks in DNH mode this session
    uint32_t passivePMKIDs;     // PMKIDs in DNH mode this session
    uint32_t passiveTimeStart;  // millis() when DNH enabled (0 = not in DNH)
    uint32_t boarBrosThisSession; // Bros added this session (for PACIFIST_RUN)
    uint32_t mercyCount;        // Mid-attack exclusions this session
    bool everDeauthed;          // Has player ever sent deauth? (for Silent Witness)
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
    
    // Title override system (v0.1.4+)
    static const char* getDisplayTitle();  // Returns override title if set, else level title
    static TitleOverride getTitleOverride();
    static void setTitleOverride(TitleOverride override);
    static const char* getTitleOverrideName(TitleOverride override);
    static bool canUseTitleOverride(TitleOverride override);  // Check if player has unlocked it
    static TitleOverride getNextAvailableOverride();  // Cycle through unlocked overrides
    
    // Class info
    static PorkClass getClass();
    static PorkClass getClassForLevel(uint8_t level);
    static const char* getClassName();
    static const char* getClassNameFor(PorkClass cls);
    static uint8_t getClassIndex();  // 0-7
    
    // Achievements
    static void unlockAchievement(PorkAchievement ach);
    static bool hasAchievement(PorkAchievement ach);
    static uint64_t getAchievements();
    static uint8_t getUnlockedCount();  // Count of unlocked achievements
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
    
    // SD backup - immortal pig survives M5Burner
    static bool backupToSD();
    static bool restoreFromSD();
};
