// FLEXES - Lifetime statistics and active buff/debuff overlay
#pragma once

#include <M5Unified.h>

// Buff/Debuff flags (can have multiple active)
enum class PorkBuff : uint8_t {
    NONE = 0,
    // Buffs (positive effects)
    // vNext Neon Operator buffs (see swine_stats.cpp for exact effects)
    R4G3           = (1 << 0),  // NE0N H1GH: -18% Street Sweep; faster mood decay
    SNOUT_SHARP    = (1 << 1),  // SNOUT$HARP: +18% global XP gain
    H0TSTR3AK      = (1 << 2),  // H0TSTR3AK: +6% capture XP when on a streak
    C4FF31N4T3D    = (1 << 3),  // C0LD F0CU5: +10% Glass Stare, +5% Street Sweep
    CL34R_SKY      = (1 << 4),  // CL34R$KY: +5% Signal Drip in clear weather
};

enum class PorkDebuff : uint8_t {
    NONE = 0,
    // Debuffs (negative effects)
    // vNext Neon Operator debuffs
    SLOP_SLUG      = (1 << 0),  // SLOP$LUG: +12% Street Sweep (slower scans) when very unhappy
    F0GSNOUT       = (1 << 1),  // F0GSNOUT: -10% XP gain when a bit unhappy
    TR0UGHDR41N    = (1 << 2),  // TR0UGHDR41N: +1ms jitter after inactivity
    HAM_STR1NG     = (1 << 3),  // HAM$TR1NG: +35% Street Sweep when extremely unhappy
    TH0ND3R_SLAB   = (1 << 4),  // TH0ND3R$LAB: +8% Street Sweep during storms
};

// Class buff flags (permanent, cumulative based on level)
enum class ClassBuff : uint16_t {
    NONE         = 0,
    P4CK3T_NOSE  = (1 << 0),  // A1R R34D3R (SN1FF3R L6+): -8% Street Sweep
    H4RD_SNOUT   = (1 << 1),  // T4RG3T F0CU5 (PWNER L11+): +0.6s Glass Stare
    R04D_H0G     = (1 << 2),  // R04M CR3D (R00T L16+): +12% distance XP
    SH4RP_TUSKS  = (1 << 3),  // GL4SS ST4R3+ (R0GU3 L21+): +0.8s Glass Stare
    CR4CK_NOSE   = (1 << 4),  // L00T M3M0RY (EXPL01T L26+): +10% capture XP
    IR0N_TUSKS   = (1 << 5),  // CL0CK NERV3S (WARL0RD L31+): -10% jitter (Clock Nerves)
    OMNI_P0RK    = (1 << 6),  // 0MN1P0RK (L3G3ND L36+): +4% to all modifiers
    K3RN3L_H0G   = (1 << 7),  // PR0T0C0L 5EER (L41+): +6% cap/dist XP
    B4C0NM4NC3R  = (1 << 8)   // B4C0N 0V3RDR1V3 (L46+): +8% cap/dist XP
};

// Active buff/debuff state
struct BuffState {
    uint8_t buffs;    // PorkBuff flags
    uint8_t debuffs;  // PorkDebuff flags
    
    bool hasBuff(PorkBuff b) const { return buffs & (uint8_t)b; }
    bool hasDebuff(PorkDebuff d) const { return debuffs & (uint8_t)d; }
};

// Tab selection for FLEXES
enum class StatsTab : uint8_t {
    STATS = 0,
    BOOSTS = 1,
    WIGLE = 2
};

class FlexesScreen {
public:
    static void init();
    static void show();
    static void hide();
    static void update();
    static void draw(M5Canvas& canvas);
    static bool isActive() { return active; }
    
    // Buff/debuff calculation (called by modes)
    static BuffState calculateBuffs();
    static uint16_t calculateClassBuffs();  // Returns ClassBuff flags
    
    // Buff effect getters for game mechanics
    static uint8_t getDeauthBurstCount();     // Base 5, modified by buffs
    static uint8_t getDeauthJitterMax();      // Base 5ms, modified by debuffs
    static uint16_t getChannelHopInterval();  // Base from config, modified
    static float getXPMultiplier();           // 1.0 base, modified
    static uint32_t getLockTime();            // Base 4000ms (configurable), modified by class
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
    static uint16_t currentClassBuffs;
    static uint32_t lastBuffUpdate;
    static StatsTab currentTab;
    
    static void handleInput();
    static void drawStatsTab(M5Canvas& canvas);
    static void drawBuffsTab(M5Canvas& canvas);
    static void drawTabBar(M5Canvas& canvas);
    static void drawStats(M5Canvas& canvas);  // Stat grid helper

    // Draw the WiGLE statistics tab. This tab displays the user's rank
    // and total observations as read from the WiGLE stats cache. See
    // WiGLE::getUserStats() for details.
    static void drawWigleTab(M5Canvas& canvas);
};
