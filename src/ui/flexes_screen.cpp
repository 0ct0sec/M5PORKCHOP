// FLEXES - Lifetime statistics and active buff/debuff overlay

#include "flexes_screen.h"
#include "display.h"
#include "../core/xp.h"
#include "../core/config.h"
#include "../piglet/mood.h"
#include "../piglet/weather.h"
#include "../web/wigle.h"
#include <M5Cardputer.h>

// Static member initialization
bool FlexesScreen::active = false;
bool FlexesScreen::keyWasPressed = false;
BuffState FlexesScreen::currentBuffs = {0, 0};
uint16_t FlexesScreen::currentClassBuffs = 0;
uint32_t FlexesScreen::lastBuffUpdate = 0;
StatsTab FlexesScreen::currentTab = StatsTab::STATS;

// Buff names and descriptions (leet one-word style)
// Buff names and descriptions (vNext Neon Operator)
static const char* BUFF_NAMES[] = {
    "NE0N H1GH",    // happiness >80: faster sweep, faster decay
    "SNOUT$HARP",   // happiness >50: global XP boost
    "H0TSTR3AK",    // 2+ captures: capture XP boost
    "C0LD F0CU5",   // happiness 60-75: balanced focus
    "CL34R$KY"      // clear weather: signal boost
};

static const char* BUFF_DESCS[] = {
    "Street Sweep -18%",               // NE0N H1GH: faster band sweep
    "Signal Drip +18%",                // SNOUT$HARP: global XP weight
    "Capture XP +6%",                  // H0TSTR3AK: capture streak bonus
    "Glass Stare +10% / Street Sweep +5%",  // C0LD F0CU5: longer locks & slower sweep
    "Signal Drip +5%"                  // CL34R$KY: weather XP boost
};

// Debuff names and descriptions (vNext Neon Operator)
static const char* DEBUFF_NAMES[] = {
    "SLOP$LUG",
    "F0GSNOUT",
    "TR0UGHDR41N",
    "HAM$TR1NG",
    "TH0ND3R$LAB"
};

static const char* DEBUFF_DESCS[] = {
    "Street Sweep +12%",   // SLOP$LUG: slower sweep
    "Signal Drip -10%",    // F0GSNOUT: global XP penalty
    "+1ms jitter",         // TR0UGHDR41N: extra jitter
    "Street Sweep +35%",   // HAM$TR1NG: very slow sweep
    "Street Sweep +8%"     // TH0ND3R$LAB: storm slowdown
};

// Class buff names and descriptions
// Class buff names and descriptions (vNext Neon Operator)
static const char* CLASS_BUFF_NAMES[] = {
    "A1R R34D3R",    // SN1FF3R (L6): Street Sweep -8%
    "T4RG3T F0CU5",  // PWNER (L11): Glass Stare +0.6s
    "R04M CR3D",     // R00T (L16): +12% distance XP
    "GL4SS ST4R3+",  // R0GU3 (L21): Glass Stare +0.8s
    "L00T M3M0RY",   // EXPL01T (L26): +10% capture XP
    "CL0CK NERV3S",  // WARL0RD (L31): -10% jitter
    "0MN1P0RK",      // L3G3ND (L36): +4% all
    "PR0T0C0L 5EER", // K3RN3L H0G (L41): +6% cap/dist XP
    "B4C0N 0V3RDR1V3" // B4C0NM4NC3R (L46): +8% cap/dist XP
};

static const char* CLASS_BUFF_DESCS[] = {
    "-8% Street Sweep",         // A1R R34D3R
    "+0.6s Glass Stare",        // T4RG3T F0CU5
    "+12% distance XP",         // R04M CR3D
    "+0.8s Glass Stare",        // GL4SS ST4R3+
    "+10% capture XP",          // L00T M3M0RY
    "-10% Clock Nerves",        // CL0CK NERV3S
    "+4% all",                  // 0MN1P0RK
    "+6% cap/dist XP",          // PR0T0C0L 5EER
    "+8% cap/dist XP"           // B4C0N 0V3RDR1V3
};
static const uint8_t CLASS_BUFF_COUNT = sizeof(CLASS_BUFF_NAMES) / sizeof(CLASS_BUFF_NAMES[0]);

// Stat names (leet one-word)
static const char* STAT_LABELS[] = {
    "N3TW0RKS",
    "H4NDSH4K3S",
    "PMK1DS",
    "D34UTHS",
    "D1ST4NC3",
    "BL3 BL4STS",
    "S3SS10NS",
    "GH0STS",
    "WP4THR33",
    "G30L0CS"
};

void FlexesScreen::init() {
    active = false;
    keyWasPressed = false;
    currentBuffs = {0, 0};
    currentClassBuffs = 0;
    lastBuffUpdate = 0;
    currentTab = StatsTab::STATS;
}

void FlexesScreen::show() {
    active = true;
    keyWasPressed = true;  // Ignore the key that activated us
    currentBuffs = calculateBuffs();
    currentClassBuffs = calculateClassBuffs();
    lastBuffUpdate = millis();
    currentTab = StatsTab::STATS;
}

void FlexesScreen::hide() {
    active = false;
}

void FlexesScreen::update() {
    if (!active) return;
    
    // Update buffs periodically
    if (millis() - lastBuffUpdate > 1000) {
        currentBuffs = calculateBuffs();
        currentClassBuffs = calculateClassBuffs();
        lastBuffUpdate = millis();
    }
    
    handleInput();
}

void FlexesScreen::handleInput() {
    bool anyPressed = M5Cardputer.Keyboard.isPressed();
    
    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }
    
    if (keyWasPressed) return;
    keyWasPressed = true;
    
    // Tab cycling: ',' cycles left, '/' cycles right
    if (M5Cardputer.Keyboard.isKeyPressed(',')) {
        // Cycle left: STATS -> WIGLE -> BOOSTS -> STATS
        switch (currentTab) {
            case StatsTab::STATS:
                currentTab = StatsTab::WIGLE;
                break;
            case StatsTab::BOOSTS:
                currentTab = StatsTab::STATS;
                break;
            case StatsTab::WIGLE:
                currentTab = StatsTab::BOOSTS;
                break;
        }
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed('/')) {
        // Cycle right: STATS -> BOOSTS -> WIGLE -> STATS
        switch (currentTab) {
            case StatsTab::STATS:
                currentTab = StatsTab::BOOSTS;
                break;
            case StatsTab::BOOSTS:
                currentTab = StatsTab::WIGLE;
                break;
            case StatsTab::WIGLE:
                currentTab = StatsTab::STATS;
                break;
        }
        return;
    }
    
    // Enter key cycles through available title overrides (only on STATS tab)
    if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER) && currentTab == StatsTab::STATS) {
        TitleOverride next = XP::getNextAvailableOverride();
        XP::setTitleOverride(next);
        
        // Show toast confirming title change
        const char* newTitle = XP::getDisplayTitle();
        if (next == TitleOverride::NONE) {
            Display::showToast("T1TLE: DEFAULT");
        } else {
            char buf[32];
            snprintf(buf, sizeof(buf), "T1TLE: %s", newTitle);
            Display::showToast(buf);
        }
        return;
    }
    
    // Backspace - go back
    if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
        hide();
    }
}

BuffState FlexesScreen::calculateBuffs() {
    BuffState state = {0, 0};
    int happiness = Mood::getEffectiveHappiness();
    const SessionStats& session = XP::getSession();
    
    // === BUFFS ===
    
    // NE0N H1GH: happiness > 80 = Street Sweep -18%
    if (happiness > 80) {
        state.buffs |= (uint8_t)PorkBuff::R4G3;
    }
    
    // SNOUT$HARP: happiness > 50 = Signal Drip +18%
    if (happiness > 50) {
        state.buffs |= (uint8_t)PorkBuff::SNOUT_SHARP;
    }
    
    // H0TSTR3AK: 2+ captures in session (handshakes)
    if (session.handshakes >= 2) {
        state.buffs |= (uint8_t)PorkBuff::H0TSTR3AK;
    }
    
    // C0LD F0CU5: happiness between 60 and 75 inclusive
    if (happiness >= 60 && happiness <= 75) {
        state.buffs |= (uint8_t)PorkBuff::C4FF31N4T3D;
    }
    
    // === DEBUFFS ===
    
    // SLOP$LUG: happiness < -50 = Street Sweep +12%
    if (happiness < -50) {
        state.debuffs |= (uint8_t)PorkDebuff::SLOP_SLUG;
    }
    
    // F0GSNOUT: happiness < -30 = Signal Drip -10%
    if (happiness < -30) {
        state.debuffs |= (uint8_t)PorkDebuff::F0GSNOUT;
    }
    
    // TR0UGHDR41N: no activity for 5 minutes (uses Mood's activity tracking)
    uint32_t lastActivity = Mood::getLastActivityTime();
    uint32_t idleTime = (lastActivity > 0) ? (millis() - lastActivity) : 0;
    if (idleTime > 300000) {
        state.debuffs |= (uint8_t)PorkDebuff::TR0UGHDR41N;
    }
    
    // HAM$TR1NG: happiness < -70 = Street Sweep +35%
    if (happiness < -70) {
        state.debuffs |= (uint8_t)PorkDebuff::HAM_STR1NG;
    }

    // === WEATHER ===

    // CL34R$KY: clear weather = Signal Drip +5%
    if (!Weather::isRaining()) {
        state.buffs |= (uint8_t)PorkBuff::CL34R_SKY;
    }

    // TH0ND3R$LAB: storm = Street Sweep +8%
    if (Weather::isThunderFlashing()) {
        state.debuffs |= (uint8_t)PorkDebuff::TH0ND3R_SLAB;
    }

    return state;
}

uint16_t FlexesScreen::calculateClassBuffs() {
    uint8_t level = XP::getLevel();
    uint16_t buffs = 0;
    
    // Cumulative buffs based on class tier
    if (level >= 6)  buffs |= (uint16_t)ClassBuff::P4CK3T_NOSE;  // SN1FF3R
    if (level >= 11) buffs |= (uint16_t)ClassBuff::H4RD_SNOUT;   // PWNER
    if (level >= 16) buffs |= (uint16_t)ClassBuff::R04D_H0G;     // R00T
    if (level >= 21) buffs |= (uint16_t)ClassBuff::SH4RP_TUSKS;  // R0GU3
    if (level >= 26) buffs |= (uint16_t)ClassBuff::CR4CK_NOSE;   // EXPL01T
    if (level >= 31) buffs |= (uint16_t)ClassBuff::IR0N_TUSKS;   // WARL0RD
    if (level >= 36) buffs |= (uint16_t)ClassBuff::OMNI_P0RK;    // L3G3ND
    if (level >= 41) buffs |= (uint16_t)ClassBuff::K3RN3L_H0G;   // K3RN3L H0G
    if (level >= 46) {
        buffs |= (uint16_t)ClassBuff::B4C0NM4NC3R;               // B4C0NM4NC3R
        buffs &= ~(uint16_t)ClassBuff::K3RN3L_H0G;               // Supersede lower tier
    }
    
    return buffs;
}

bool FlexesScreen::hasClassBuff(ClassBuff cb) {
    return (calculateClassBuffs() & (uint16_t)cb) != 0;
}

uint8_t FlexesScreen::getDeauthBurstCount() {
    // In vNext the deauth burst count is no longer significantly boosted by class or mood.
    // Preserve a small global perk from 0MN1P0RK (+4%) but remove spammy stacking.
    uint16_t classBuffs = calculateClassBuffs();
    // Preserve the original baseline burst count (5 packets). vNext removes most stacking
    // but does not nerf the base itself. The goal is precision, not reduced throughput.
    uint8_t base = 5;

    // Apply only the 0MN1P0RK perk: a small +4% bump (rounded) to reward mastery
    if (classBuffs & (uint16_t)ClassBuff::OMNI_P0RK) {
        base = (uint8_t)((base * 104 + 50) / 100);  // 4% boost with rounding
    }

    // Do not modify burst count based on mood or other debuffs to emphasize restraint
    return base;
}

uint8_t FlexesScreen::getDeauthJitterMax() {
    BuffState buffs = calculateBuffs();
    uint16_t classBuffs = calculateClassBuffs();
    // Base maximum jitter in milliseconds
    const float base = 5.0f;
    float mod = 0.0f;
    uint8_t addMs = 0;
    
    // Class perk: CL0CK NERV3S (WARL0RD) -> -10% jitter
    if (classBuffs & (uint16_t)ClassBuff::IR0N_TUSKS) {
        mod -= 0.10f;
    }
    
    // Mood debuff: TR0UGHDR41N -> +1ms constant jitter
    if (buffs.hasDebuff(PorkDebuff::TR0UGHDR41N)) {
        addMs += 1;
    }
    
    // Compute multiplier and clamp to [0.75, 1.30]
    float mult = 1.0f + mod;
    if (mult < 0.75f) mult = 0.75f;
    if (mult > 1.30f) mult = 1.30f;
    
    float jitter = base * mult;
    uint8_t jitterMax = (uint8_t)jitter;
    jitterMax += addMs;
    if (jitterMax < 1) jitterMax = 1;
    return jitterMax;
}

uint16_t FlexesScreen::getChannelHopInterval() {
    BuffState buffs = calculateBuffs();
    uint16_t classBuffs = calculateClassBuffs();
    uint16_t base = Config::wifi().channelHopInterval;  // Default from config
    
    // Compute additive modifiers for Street Sweep (channel hop interval)
    float mod = 0.0f;
    // Class perk: A1R R34D3R (SN1FF3R) -> -8% interval
    if (classBuffs & (uint16_t)ClassBuff::P4CK3T_NOSE) {
        mod -= 0.08f;
    }
    // Class perk: 0MN1P0RK (L3G3ND) -> +4% interval
    if (classBuffs & (uint16_t)ClassBuff::OMNI_P0RK) {
        mod += 0.04f;
    }
    // Mood buff: NE0N H1GH -> -18% interval
    if (buffs.hasBuff(PorkBuff::R4G3)) {
        mod -= 0.18f;
    }
    // Mood buff: C0LD F0CU5 -> +5% interval
    if (buffs.hasBuff(PorkBuff::C4FF31N4T3D)) {
        mod += 0.05f;
    }
    // Debuff: SLOP$LUG -> +12% interval
    if (buffs.hasDebuff(PorkDebuff::SLOP_SLUG)) {
        mod += 0.12f;
    }
    // Debuff: HAM$TR1NG -> +35% interval
    if (buffs.hasDebuff(PorkDebuff::HAM_STR1NG)) {
        mod += 0.35f;
    }
    // Debuff: TH0ND3R$LAB -> +8% interval (storm interference)
    if (buffs.hasDebuff(PorkDebuff::TH0ND3R_SLAB)) {
        mod += 0.08f;
    }

    // Compute final multiplier and clamp to [0.65, 1.45]
    float finalMult = 1.0f + mod;
    if (finalMult < 0.65f) finalMult = 0.65f;
    if (finalMult > 1.45f) finalMult = 1.45f;
    
    uint16_t interval = (uint16_t)((float)base * finalMult);
    return interval;
}

float FlexesScreen::getXPMultiplier() {
    BuffState buffs = calculateBuffs();
    uint16_t classBuffs = calculateClassBuffs();
    // vNext Signal Drip (global XP multiplier)
    float mod = 0.0f;
    // Class perk: 0MN1P0RK -> +4% global XP
    if (classBuffs & (uint16_t)ClassBuff::OMNI_P0RK) {
        mod += 0.04f;
    }
    // Mood buff: SNOUT$HARP -> +18% global XP
    if (buffs.hasBuff(PorkBuff::SNOUT_SHARP)) {
        mod += 0.18f;
    }
    // Mood debuff: F0GSNOUT -> -10% global XP
    if (buffs.hasDebuff(PorkDebuff::F0GSNOUT)) {
        mod -= 0.10f;
    }
    // Weather buff: CL34R$KY -> +5% global XP
    if (buffs.hasBuff(PorkBuff::CL34R_SKY)) {
        mod += 0.05f;
    }

    float finalMult = 1.0f + mod;
    // Clamp Signal Drip to [0.80, 1.60]
    if (finalMult < 0.80f) finalMult = 0.80f;
    if (finalMult > 1.60f) finalMult = 1.60f;
    return finalMult;
}

uint32_t FlexesScreen::getLockTime() {
    uint16_t classBuffs = calculateClassBuffs();
    BuffState buffs = calculateBuffs();
    uint32_t base = Config::wifi().lockTime;  // From settings
    
    // Constant additions (milliseconds) for Glass Stare
    uint32_t addMs = 0;
    float mod = 0.0f;
    
    // Class perk: T4RG3T F0CU5 -> +0.6s
    if (classBuffs & (uint16_t)ClassBuff::H4RD_SNOUT) {
        addMs += 600;
    }
    // Class perk: GL4SS ST4R3+ -> +0.8s
    if (classBuffs & (uint16_t)ClassBuff::SH4RP_TUSKS) {
        addMs += 800;
    }
    // Class perk: 0MN1P0RK -> +4% lock time
    if (classBuffs & (uint16_t)ClassBuff::OMNI_P0RK) {
        mod += 0.04f;
    }
    
    // Mood buff: C0LD F0CU5 -> +10% lock time
    if (buffs.hasBuff(PorkBuff::C4FF31N4T3D)) {
        mod += 0.10f;
    }
    
    // Apply clamp for Glass Stare multiplier [0.80, 1.50]
    float mult = 1.0f + mod;
    if (mult < 0.80f) mult = 0.80f;
    if (mult > 1.50f) mult = 1.50f;
    
    uint32_t result = (uint32_t)((float)base * mult) + addMs;
    return result;
}

float FlexesScreen::getDistanceXPMultiplier() {
    uint16_t classBuffs = calculateClassBuffs();
    // vNext R04M CR3D (distance XP multiplier)
    float mod = 0.0f;
    // Class perk: R04M CR3D (R00T) -> +12%
    if (classBuffs & (uint16_t)ClassBuff::R04D_H0G) {
        mod += 0.12f;
    }
    // Class perk: 0MN1P0RK -> +4%
    if (classBuffs & (uint16_t)ClassBuff::OMNI_P0RK) {
        mod += 0.04f;
    }
    // Class perk: PR0T0C0L 5EER -> +6%
    if ((classBuffs & (uint16_t)ClassBuff::K3RN3L_H0G) && !(classBuffs & (uint16_t)ClassBuff::B4C0NM4NC3R)) {
        mod += 0.06f;
    }
    // Class perk: B4C0N 0V3RDR1V3 supersedes PR0T0C0L 5EER -> +8%
    if (classBuffs & (uint16_t)ClassBuff::B4C0NM4NC3R) {
        mod += 0.08f;
    }
    
    // Rig mode: SCOUT -> +5% distance XP (optional; only if implemented)
    // (No rig mode implemented in this version)
    
    float finalMult = 1.0f + mod;
    return finalMult;
}

float FlexesScreen::getCaptureXPMultiplier() {
    uint16_t classBuffs = calculateClassBuffs();
    BuffState buffs = calculateBuffs();
    // vNext L00T M3M0RY (capture XP multiplier)
    float mod = 0.0f;
    // Class perk: L00T M3M0RY (EXPL01T) -> +10%
    if (classBuffs & (uint16_t)ClassBuff::CR4CK_NOSE) {
        mod += 0.10f;
    }
    // Class perk: 0MN1P0RK -> +4%
    if (classBuffs & (uint16_t)ClassBuff::OMNI_P0RK) {
        mod += 0.04f;
    }
    // Class perk: PR0T0C0L 5EER -> +6% (unless overridden by B4C0N)
    if ((classBuffs & (uint16_t)ClassBuff::K3RN3L_H0G) && !(classBuffs & (uint16_t)ClassBuff::B4C0NM4NC3R)) {
        mod += 0.06f;
    }
    // Class perk: B4C0N 0V3RDR1V3 -> +8%
    if (classBuffs & (uint16_t)ClassBuff::B4C0NM4NC3R) {
        mod += 0.08f;
    }
    // Mood buff: H0TSTR3AK -> +6%
    if (buffs.hasBuff(PorkBuff::H0TSTR3AK)) {
        mod += 0.06f;
    }
    // Rig mode: HUNTER -> +5% capture XP (optional; not implemented)
    float finalMult = 1.0f + mod;
    return finalMult;
}

const char* FlexesScreen::getClassBuffName(ClassBuff cb) {
    switch (cb) {
        case ClassBuff::P4CK3T_NOSE: return CLASS_BUFF_NAMES[0];
        case ClassBuff::H4RD_SNOUT:  return CLASS_BUFF_NAMES[1];
        case ClassBuff::R04D_H0G:    return CLASS_BUFF_NAMES[2];
        case ClassBuff::SH4RP_TUSKS: return CLASS_BUFF_NAMES[3];
        case ClassBuff::CR4CK_NOSE:  return CLASS_BUFF_NAMES[4];
        case ClassBuff::IR0N_TUSKS:  return CLASS_BUFF_NAMES[5];
        case ClassBuff::OMNI_P0RK:   return CLASS_BUFF_NAMES[6];
        case ClassBuff::K3RN3L_H0G:  return CLASS_BUFF_NAMES[7];
        case ClassBuff::B4C0NM4NC3R: return CLASS_BUFF_NAMES[8];
        default: return "???";
    }
}

const char* FlexesScreen::getClassBuffDesc(ClassBuff cb) {
    switch (cb) {
        case ClassBuff::P4CK3T_NOSE: return CLASS_BUFF_DESCS[0];
        case ClassBuff::H4RD_SNOUT:  return CLASS_BUFF_DESCS[1];
        case ClassBuff::R04D_H0G:    return CLASS_BUFF_DESCS[2];
        case ClassBuff::SH4RP_TUSKS: return CLASS_BUFF_DESCS[3];
        case ClassBuff::CR4CK_NOSE:  return CLASS_BUFF_DESCS[4];
        case ClassBuff::IR0N_TUSKS:  return CLASS_BUFF_DESCS[5];
        case ClassBuff::OMNI_P0RK:   return CLASS_BUFF_DESCS[6];
        case ClassBuff::K3RN3L_H0G:  return CLASS_BUFF_DESCS[7];
        case ClassBuff::B4C0NM4NC3R: return CLASS_BUFF_DESCS[8];
        default: return "";
    }
}

const char* FlexesScreen::getBuffName(PorkBuff b) {
    switch (b) {
        case PorkBuff::R4G3: return BUFF_NAMES[0];
        case PorkBuff::SNOUT_SHARP: return BUFF_NAMES[1];
        case PorkBuff::H0TSTR3AK: return BUFF_NAMES[2];
        case PorkBuff::C4FF31N4T3D: return BUFF_NAMES[3];
        case PorkBuff::CL34R_SKY: return BUFF_NAMES[4];
        default: return "???";
    }
}

const char* FlexesScreen::getDebuffName(PorkDebuff d) {
    switch (d) {
        case PorkDebuff::SLOP_SLUG: return DEBUFF_NAMES[0];
        case PorkDebuff::F0GSNOUT: return DEBUFF_NAMES[1];
        case PorkDebuff::TR0UGHDR41N: return DEBUFF_NAMES[2];
        case PorkDebuff::HAM_STR1NG: return DEBUFF_NAMES[3];
        case PorkDebuff::TH0ND3R_SLAB: return DEBUFF_NAMES[4];
        default: return "???";
    }
}

const char* FlexesScreen::getBuffDesc(PorkBuff b) {
    switch (b) {
        case PorkBuff::R4G3: return BUFF_DESCS[0];
        case PorkBuff::SNOUT_SHARP: return BUFF_DESCS[1];
        case PorkBuff::H0TSTR3AK: return BUFF_DESCS[2];
        case PorkBuff::C4FF31N4T3D: return BUFF_DESCS[3];
        case PorkBuff::CL34R_SKY: return BUFF_DESCS[4];
        default: return "";
    }
}

const char* FlexesScreen::getDebuffDesc(PorkDebuff d) {
    switch (d) {
        case PorkDebuff::SLOP_SLUG: return DEBUFF_DESCS[0];
        case PorkDebuff::F0GSNOUT: return DEBUFF_DESCS[1];
        case PorkDebuff::TR0UGHDR41N: return DEBUFF_DESCS[2];
        case PorkDebuff::HAM_STR1NG: return DEBUFF_DESCS[3];
        case PorkDebuff::TH0ND3R_SLAB: return DEBUFF_DESCS[4];
        default: return "";
    }
}

void FlexesScreen::draw(M5Canvas& canvas) {
    if (!active) return;
    
    canvas.fillSprite(COLOR_BG);
    canvas.setTextColor(COLOR_FG);
    
    // Draw tab bar at top
    drawTabBar(canvas);
    
    // Draw content based on current tab
    if (currentTab == StatsTab::STATS) {
        drawStatsTab(canvas);
    } else if (currentTab == StatsTab::BOOSTS) {
        drawBuffsTab(canvas);
    } else if (currentTab == StatsTab::WIGLE) {
        drawWigleTab(canvas);
    }
    

}

void FlexesScreen::drawTabBar(M5Canvas& canvas) {
    canvas.setTextSize(1);
    const int tabY = 0;
    const int tabH = 12;
    const int tabTextY = 6;  // Add 1px top padding to match bottom margin

    // Calculate dynamic widths for three tabs. Distribute any remainder pixels
    // across the first tabs so that the tabs fill the available space evenly.
    const int totalTabs = 3;
    const int margin = 2;
    const int spacing = 3;
    const int availableW = DISPLAY_W - margin * 2 - spacing * (totalTabs - 1);
    const int baseW = availableW / totalTabs;
    const int remainder = availableW % totalTabs;

    canvas.setTextDatum(middle_center);
    int x = margin;
    for (int i = 0; i < totalTabs; i++) {
        int w = baseW + (i < remainder ? 1 : 0);
        bool isActive;
        const char* label;
        if (i == 0) {
            isActive = (currentTab == StatsTab::STATS);
            label = "ST4TS";
        } else if (i == 1) {
            isActive = (currentTab == StatsTab::BOOSTS);
            label = "B00STS";
        } else {
            isActive = (currentTab == StatsTab::WIGLE);
            label = "W1GL3";
        }
        if (isActive) {
            canvas.fillRect(x, tabY, w, tabH, COLOR_FG);
            canvas.setTextColor(COLOR_BG);
        } else {
            canvas.drawRect(x, tabY, w, tabH, COLOR_FG);
            canvas.setTextColor(COLOR_FG);
        }
        canvas.drawString(label, x + w / 2, tabTextY);
        x += w + spacing;
    }
    // Reset text color
    canvas.setTextColor(COLOR_FG);
}

void FlexesScreen::drawStatsTab(M5Canvas& canvas) {
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    
    // Level and class info
    uint8_t level = XP::getLevel();
    const char* title = XP::getDisplayTitle();  // Use display title (may be override)
    const char* className = XP::getClassName();
    uint8_t progress = XP::getProgress();
    
    // Show title with indicator if it's an override
    char lvlBuf[48];
    if (XP::getTitleOverride() != TitleOverride::NONE) {
        // Show override title with asterisk
        snprintf(lvlBuf, sizeof(lvlBuf), "LVL %d: %s*", level, title);
    } else {
        snprintf(lvlBuf, sizeof(lvlBuf), "LVL %d: %s", level, title);
    }
    canvas.drawString(lvlBuf, 5, 14);
    
    // Class on right
    char classBuf[24];
    snprintf(classBuf, sizeof(classBuf), "T13R: %s", className);
    canvas.setTextDatum(top_right);
    canvas.drawString(classBuf, DISPLAY_W - 5, 14);
    
    // XP bar
    int barX = 5;
    int barY = 24;
    int barW = DISPLAY_W - 10;
    int barH = 6;
    canvas.drawRect(barX, barY, barW, barH, COLOR_FG);
    int fillW = (barW - 2) * progress / 100;
    if (fillW > 0) {
        canvas.fillRect(barX + 1, barY + 1, fillW, barH - 2, COLOR_FG);
    }
    
    // XP text centered under bar
    char xpBuf[32];
    snprintf(xpBuf, sizeof(xpBuf), "%lu XP (%d%%)", (unsigned long)XP::getTotalXP(), progress);
    canvas.setTextDatum(top_center);
    canvas.drawString(xpBuf, DISPLAY_W / 2, 32);
    
    // Stats grid
    drawStats(canvas);
}

void FlexesScreen::drawBuffsTab(M5Canvas& canvas) {
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    
    int y = 14;
    int buffCount = 0;
    
    // === CLASS BUFFS SECTION ===
    char classPerksBuf[32];
    snprintf(classPerksBuf, sizeof(classPerksBuf), "%s T13R P3RKS:", XP::getClassName());
    canvas.drawString(classPerksBuf, 5, y);
    y += 10;
    
    // Show all active class buffs (permanent, based on level)
    if (currentClassBuffs != 0) {
        for (uint8_t i = 0; i < CLASS_BUFF_COUNT; i++) {
            ClassBuff cb = (ClassBuff)(1 << i);
            if (currentClassBuffs & (uint16_t)cb) {
                char buf[48];
                snprintf(buf, sizeof(buf), "[*] %s %s", getClassBuffName(cb), getClassBuffDesc(cb));
                canvas.drawString(buf, 5, y);
                y += 10;
                buffCount++;
                if (y > 60) break;  // Prevent overflow
            }
        }
    }
    
    if (buffCount == 0) {
        canvas.drawString("[=] N0N3 (LVL 6+)", 5, y);
        y += 10;
    }
    
    // === MOOD BUFFS SECTION ===
    y += 4;  // Small gap
    canvas.drawString("M00D B00STS:", 5, y);
    y += 10;
    
    int moodCount = 0;
    
    // Draw active mood buffs
    if (currentBuffs.buffs != 0) {
        for (int i = 0; i < 5; i++) {
            PorkBuff b = (PorkBuff)(1 << i);
            if (currentBuffs.hasBuff(b)) {
                char buf[48];
                snprintf(buf, sizeof(buf), "[+] %s %s", getBuffName(b), getBuffDesc(b));
                canvas.drawString(buf, 5, y);
                y += 10;
                moodCount++;
                if (y > 90) break;
            }
        }
    }
    
    // Draw active mood debuffs
    if (currentBuffs.debuffs != 0) {
        for (int i = 0; i < 5; i++) {
            PorkDebuff d = (PorkDebuff)(1 << i);
            if (currentBuffs.hasDebuff(d)) {
                char buf[48];
                snprintf(buf, sizeof(buf), "[-] %s %s", getDebuffName(d), getDebuffDesc(d));
                canvas.drawString(buf, 5, y);
                y += 10;
                moodCount++;
                if (y > 90) break;
            }
        }
    }
    
    if (moodCount == 0) {
        canvas.drawString("[=] N0N3 ACT1V3", 5, y);
    }
}

void FlexesScreen::drawStats(M5Canvas& canvas) {
    const PorkXPData& data = XP::getData();
    
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    
    int y = 44;  // Start after XP bar and XP text
    int lineH = 10;
    int col1 = 5;
    int col2 = 75;
    int col3 = 125;
    int col4 = 195;
    
    // Row 1: Networks, Handshakes
    canvas.drawString("N3TW0RKS:", col1, y);
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)data.lifetimeNetworks);
    canvas.drawString(buf, col2, y);
    
    canvas.drawString("H4NDSH4K3S:", col3, y);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)data.lifetimeHS);
    canvas.drawString(buf, col4, y);
    
    y += lineH;
    
    // Row 2: PMKIDs, Deauths
    canvas.drawString("PMK1DS:", col1, y);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)data.lifetimePMKID);
    canvas.drawString(buf, col2, y);
    
    canvas.drawString("D34UTHS:", col3, y);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)data.lifetimeDeauths);
    canvas.drawString(buf, col4, y);
    
    y += lineH;
    
    // Row 3: Distance, BLE
    canvas.drawString("D1ST4NC3:", col1, y);
    snprintf(buf, sizeof(buf), "%.1fkm", data.lifetimeDistance / 1000.0f);
    canvas.drawString(buf, col2, y);
    
    canvas.drawString("BL3 BL4STS:", col3, y);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)data.lifetimeBLE);
    canvas.drawString(buf, col4, y);
    
    y += lineH;
    
    // Row 4: Sessions, Hidden
    canvas.drawString("S3SS10NS:", col1, y);
    snprintf(buf, sizeof(buf), "%u", data.sessions);
    canvas.drawString(buf, col2, y);
    
    canvas.drawString("GH0STS:", col3, y);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)data.hiddenNetworks);
    canvas.drawString(buf, col4, y);

    y += lineH;

    // Row 5: Roulette (PiggyBlues no-reboot)
    canvas.drawString("JST R0UL3T:", col1, y);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)data.rouletteWins);
    canvas.drawString(buf, col2, y);
}

// Draw the WiGLE statistics tab. This function reads the cached
// statistics from the WiGLE service and displays them in a simple
// key/value format. If no cache is available, a placeholder message
// instructs the user to refresh the WiGLE menu.
void FlexesScreen::drawWigleTab(M5Canvas& canvas) {
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    int y = 14;
    // Header
    canvas.drawString("W1GL3 ST4TS", 5, y);
    y += 12;
    // Fetch cached stats
    WiGLE::WigleUserStats stats = WiGLE::getUserStats();
    if (!stats.valid) {
        canvas.drawString("N0 W1GL3 D4TA", 5, y);
        y += 10;
        canvas.drawString("PR3SS R 1N W1GL3", 5, y);
        return;
    }
    // Display rank and counts
    char buf[32];
    // Rank
    canvas.drawString("R4NK:", 5, y);
    snprintf(buf, sizeof(buf), "%lld", (long long)stats.rank);
    canvas.drawString(buf, 80, y);
    y += 10;
    // WiFi
    canvas.drawString("W1F1:", 5, y);
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)stats.wifi);
    canvas.drawString(buf, 80, y);
    y += 10;
    // Cell
    canvas.drawString("C3LL:", 5, y);
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)stats.cell);
    canvas.drawString(buf, 80, y);
    y += 10;
    // Bluetooth
    canvas.drawString("BL3:", 5, y);
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)stats.bt);
    canvas.drawString(buf, 80, y);
}
