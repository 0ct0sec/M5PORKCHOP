// SWINE STATS - Lifetime statistics and active buff/debuff overlay
#pragma once

#include <M5Unified.h>

// Buff/Debuff flags (can have multiple active)
enum class PorkBuff : uint8_t {
    NONE = 0,
    // Buffs (positive effects)
    R4G3           = (1 << 0),  // +50% deauth burst when happiness > 70
    SNOUT_SHARP    = (1 << 1),  // +25% XP when happiness > 50
    H0TSTR3AK      = (1 << 2),  // +10% deauth when 2+ handshakes in session
    C4FF31N4T3D    = (1 << 3),  // -30% channel hop interval when happiness > 80
};

enum class PorkDebuff : uint8_t {
    NONE = 0,
    // Debuffs (negative effects)
    SLOP_SLUG      = (1 << 0),  // -30% deauth burst when happiness < -50
    F0GSNOUT       = (1 << 1),  // -15% XP when happiness < -30
    TR0UGHDR41N    = (1 << 2),  // +2ms deauth jitter when no activity 5min
    HAM_STR1NG     = (1 << 3),  // +50% channel hop interval when happiness < -70
};

// Class buff flags (permanent, cumulative based on level)
enum class ClassBuff : uint8_t {
    NONE         = 0,
    P4CK3T_NOSE  = (1 << 0),  // SN1FF3R L6+   -10% hop interval
    H4RD_SNOUT   = (1 << 1),  // PWNER L11+    +1 deauth burst
    R04D_H0G     = (1 << 2),  // R00T L16+     +15% distance XP
    SH4RP_TUSKS  = (1 << 3),  // R0GU3 L21+    +1s lock time (better client discovery)
    CR4CK_NOSE   = (1 << 4),  // EXPL01T L26+  +10% capture XP
    IR0N_TUSKS   = (1 << 5),  // WARL0RD L31+  -1ms jitter min
    OMNI_P0RK    = (1 << 6),  // L3G3ND L36+   +5% all stats
};

// Active buff/debuff state
struct BuffState {
    uint8_t buffs;    // PorkBuff flags
    uint8_t debuffs;  // PorkDebuff flags
    
    bool hasBuff(PorkBuff b) const { return buffs & (uint8_t)b; }
    bool hasDebuff(PorkDebuff d) const { return debuffs & (uint8_t)d; }
};

// Tab selection for SWINE STATS
enum class StatsTab : uint8_t {
    STATS = 0,
    BOOSTS = 1
};

class SwineStats {
public:
    static void init();
    static void show();
    static void hide();
    static void update();
    static void draw(M5Canvas& canvas);
    static bool isActive() { return active; }
    
    // Buff/debuff calculation (called by modes)
    static BuffState calculateBuffs();
    static uint8_t calculateClassBuffs();  // Returns ClassBuff flags
    
    // Buff effect getters for game mechanics
    static uint8_t getDeauthBurstCount();     // Base 5, modified by buffs
    static uint8_t getDeauthJitterMax();      // Base 5ms, modified by debuffs
    static uint16_t getChannelHopInterval();  // Base from config, modified
    static float getXPMultiplier();           // 1.0 base, modified
    static uint32_t getLockTime();            // Base 3000ms, modified by class
    static float getDistanceXPMultiplier();   // 1.0 base, modified by class
    static float getCaptureXPMultiplier();    // 1.0 base, modified by class
    
    // Class buff helpers
    static bool hasClassBuff(ClassBuff cb);
    static const char* getClassBuffName(ClassBuff cb);
    static const char* getClassBuffDesc(ClassBuff cb);
    
    // Buff/debuff name getters for display
    static const char* getBuffName(PorkBuff b);
    static const char* getDebuffName(PorkDebuff d);
    static const char* getBuffDesc(PorkBuff b);
    static const char* getDebuffDesc(PorkDebuff d);

private:
    static bool active;
    static bool keyWasPressed;
    static BuffState currentBuffs;
    static uint8_t currentClassBuffs;
    static uint32_t lastBuffUpdate;
    static StatsTab currentTab;
    
    static void handleInput();
    static void drawStatsTab(M5Canvas& canvas);
    static void drawBuffsTab(M5Canvas& canvas);
    static void drawTabBar(M5Canvas& canvas);
    static void drawStats(M5Canvas& canvas);  // Stat grid helper
};

