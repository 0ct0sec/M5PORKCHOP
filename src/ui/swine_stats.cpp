// SWINE STATS - Lifetime statistics and active buff/debuff overlay

#include "swine_stats.h"
#include "display.h"
#include "../core/xp.h"
#include "../core/config.h"
#include "../piglet/mood.h"
#include <M5Cardputer.h>

// Static member initialization
bool SwineStats::active = false;
bool SwineStats::keyWasPressed = false;
BuffState SwineStats::currentBuffs = {0, 0};
uint8_t SwineStats::currentClassBuffs = 0;
uint32_t SwineStats::lastBuffUpdate = 0;
StatsTab SwineStats::currentTab = StatsTab::STATS;

// Buff names and descriptions (leet one-word style)
static const char* BUFF_NAMES[] = {
    "R4G3",
    "SNOUT$HARP",
    "H0TSTR3AK",
    "C4FF31N4T3D"
};

static const char* BUFF_DESCS[] = {
    "+50% deauth pwr",
    "+25% XP gain",
    "+10% deauth eff",
    "-30% hop delay"
};

static const char* DEBUFF_NAMES[] = {
    "SLOP$LUG",
    "F0GSNOUT",
    "TR0UGHDR41N",
    "HAM$TR1NG"
};

static const char* DEBUFF_DESCS[] = {
    "-30% deauth pwr",
    "-15% XP gain",
    "+2ms jitter",
    "+50% hop delay"
};

// Class buff names and descriptions
static const char* CLASS_BUFF_NAMES[] = {
    "P4CK3T NOSE",   // SN1FF3R
    "H4RD SNOUT",    // PWNER
    "R04D H0G",      // R00T
    "SH4RP TUSKS",   // R0GU3
    "CR4CK NOSE",    // EXPL01T
    "1R0N TUSKS",    // WARL0RD
    "0MN1P0RK"       // L3G3ND
};

static const char* CLASS_BUFF_DESCS[] = {
    "-10% hop",
    "+1 burst",
    "+15% dist XP",
    "-1s lock",
    "+10% cap XP",
    "-1ms jitter",
    "+5% all"
};

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

void SwineStats::init() {
    active = false;
    keyWasPressed = false;
    currentBuffs = {0, 0};
    currentClassBuffs = 0;
    lastBuffUpdate = 0;
    currentTab = StatsTab::STATS;
}

void SwineStats::show() {
    active = true;
    keyWasPressed = true;  // Ignore the key that activated us
    currentBuffs = calculateBuffs();
    currentClassBuffs = calculateClassBuffs();
    lastBuffUpdate = millis();
    currentTab = StatsTab::STATS;
}

void SwineStats::hide() {
    active = false;
}

void SwineStats::update() {
    if (!active) return;
    
    // Update buffs periodically
    if (millis() - lastBuffUpdate > 1000) {
        currentBuffs = calculateBuffs();
        currentClassBuffs = calculateClassBuffs();
        lastBuffUpdate = millis();
    }
    
    handleInput();
}

void SwineStats::handleInput() {
    bool anyPressed = M5Cardputer.Keyboard.isPressed();
    
    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }
    
    if (keyWasPressed) return;
    keyWasPressed = true;
    
    // Tab switching with , (left) and / (right)
    if (M5Cardputer.Keyboard.isKeyPressed(',')) {
        currentTab = StatsTab::STATS;
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed('/')) {
        currentTab = StatsTab::BOOSTS;
        return;
    }
    
    // Exit on backtick or Esc
    if (M5Cardputer.Keyboard.isKeyPressed('`') || M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
        hide();
    }
}

BuffState SwineStats::calculateBuffs() {
    BuffState state = {0, 0};
    int happiness = Mood::getEffectiveHappiness();
    const SessionStats& session = XP::getSession();
    
    // === BUFFS ===
    
    // R4G3: happiness > 70 = +50% deauth
    if (happiness > 70) {
        state.buffs |= (uint8_t)PorkBuff::R4G3;
    }
    
    // SNOUT$HARP: happiness > 50 = +25% XP
    if (happiness > 50) {
        state.buffs |= (uint8_t)PorkBuff::SNOUT_SHARP;
    }
    
    // H0TSTR3AK: 2+ handshakes in session
    if (session.handshakes >= 2) {
        state.buffs |= (uint8_t)PorkBuff::H0TSTR3AK;
    }
    
    // C4FF31N4T3D: happiness > 80 = faster hopping
    if (happiness > 80) {
        state.buffs |= (uint8_t)PorkBuff::C4FF31N4T3D;
    }
    
    // === DEBUFFS ===
    
    // SLOP$LUG: happiness < -50 = -30% deauth
    if (happiness < -50) {
        state.debuffs |= (uint8_t)PorkDebuff::SLOP_SLUG;
    }
    
    // F0GSNOUT: happiness < -30 = -15% XP
    if (happiness < -30) {
        state.debuffs |= (uint8_t)PorkDebuff::F0GSNOUT;
    }
    
    // TR0UGHDR41N: no activity for 5 minutes (uses Mood's activity tracking)
    uint32_t lastActivity = Mood::getLastActivityTime();
    uint32_t idleTime = (lastActivity > 0) ? (millis() - lastActivity) : 0;
    if (idleTime > 300000) {
        state.debuffs |= (uint8_t)PorkDebuff::TR0UGHDR41N;
    }
    
    // HAM$TR1NG: happiness < -70 = slow hopping
    if (happiness < -70) {
        state.debuffs |= (uint8_t)PorkDebuff::HAM_STR1NG;
    }
    
    return state;
}

uint8_t SwineStats::calculateClassBuffs() {
    uint8_t level = XP::getLevel();
    uint8_t buffs = 0;
    
    // Cumulative buffs based on class tier
    if (level >= 6)  buffs |= (uint8_t)ClassBuff::P4CK3T_NOSE;  // SN1FF3R
    if (level >= 11) buffs |= (uint8_t)ClassBuff::H4RD_SNOUT;   // PWNER
    if (level >= 16) buffs |= (uint8_t)ClassBuff::R04D_H0G;     // R00T
    if (level >= 21) buffs |= (uint8_t)ClassBuff::SH4RP_TUSKS;  // R0GU3
    if (level >= 26) buffs |= (uint8_t)ClassBuff::CR4CK_NOSE;   // EXPL01T
    if (level >= 31) buffs |= (uint8_t)ClassBuff::IR0N_TUSKS;   // WARL0RD
    if (level >= 36) buffs |= (uint8_t)ClassBuff::OMNI_P0RK;    // L3G3ND
    
    return buffs;
}

bool SwineStats::hasClassBuff(ClassBuff cb) {
    return (calculateClassBuffs() & (uint8_t)cb) != 0;
}

uint8_t SwineStats::getDeauthBurstCount() {
    BuffState buffs = calculateBuffs();
    uint8_t classBuffs = calculateClassBuffs();
    uint8_t base = 5;  // Default burst count
    
    // Class buff: H4RD_SNOUT +1 burst (applied first)
    if (classBuffs & (uint8_t)ClassBuff::H4RD_SNOUT) {
        base = 6;
    }
    
    // Class buff: OMNI_P0RK +5% (applied to base)
    if (classBuffs & (uint8_t)ClassBuff::OMNI_P0RK) {
        base = (base * 105 + 50) / 100;  // Round
    }
    
    // Mood buff: R4G3 +50%
    if (buffs.hasBuff(PorkBuff::R4G3)) {
        base = (base * 15) / 10;  // 150%
    }
    // Mood buff: H0TSTR3AK +10% (only if no R4G3)
    else if (buffs.hasBuff(PorkBuff::H0TSTR3AK)) {
        base = (base * 11) / 10;  // 110%
    }
    
    // Mood debuff: SLOP$LUG -30%
    if (buffs.hasDebuff(PorkDebuff::SLOP_SLUG)) {
        base = (base * 7) / 10;  // 70%
        if (base < 2) base = 2;  // Minimum 2 frames
    }
    
    return base;
}

uint8_t SwineStats::getDeauthJitterMax() {
    BuffState buffs = calculateBuffs();
    uint8_t classBuffs = calculateClassBuffs();
    uint8_t base = 5;  // Default 1-5ms jitter
    
    // Class buff: IR0N_TUSKS -1ms min jitter (1-5 -> 0-4)
    if (classBuffs & (uint8_t)ClassBuff::IR0N_TUSKS) {
        base = 4;
    }
    
    // Mood debuff: TR0UGHDR41N +2ms jitter
    if (buffs.hasDebuff(PorkDebuff::TR0UGHDR41N)) {
        base += 2;
    }
    
    return base;
}

uint16_t SwineStats::getChannelHopInterval() {
    BuffState buffs = calculateBuffs();
    uint8_t classBuffs = calculateClassBuffs();
    uint16_t base = Config::wifi().channelHopInterval;  // Default from config
    
    // Class buff: P4CK3T_NOSE -10% interval
    if (classBuffs & (uint8_t)ClassBuff::P4CK3T_NOSE) {
        base = (base * 9) / 10;
    }
    
    // Class buff: OMNI_P0RK -5% interval
    if (classBuffs & (uint8_t)ClassBuff::OMNI_P0RK) {
        base = (base * 95) / 100;
    }
    
    // Mood buff: C4FF31N4T3D -30% interval (faster)
    if (buffs.hasBuff(PorkBuff::C4FF31N4T3D)) {
        base = (base * 7) / 10;
    }
    
    // Mood debuff: HAM$TR1NG +50% interval (slower)
    if (buffs.hasDebuff(PorkDebuff::HAM_STR1NG)) {
        base = (base * 15) / 10;
    }
    
    return base;
}

float SwineStats::getXPMultiplier() {
    BuffState buffs = calculateBuffs();
    uint8_t classBuffs = calculateClassBuffs();
    float mult = 1.0f;
    
    // Class buff: OMNI_P0RK +5% XP
    if (classBuffs & (uint8_t)ClassBuff::OMNI_P0RK) {
        mult += 0.05f;
    }
    
    // Mood buff: SNOUT$HARP +25% XP
    if (buffs.hasBuff(PorkBuff::SNOUT_SHARP)) {
        mult += 0.25f;
    }
    
    // Mood debuff: F0GSNOUT -15% XP
    if (buffs.hasDebuff(PorkDebuff::F0GSNOUT)) {
        mult -= 0.15f;
    }
    
    return mult;
}

uint32_t SwineStats::getLockTime() {
    uint8_t classBuffs = calculateClassBuffs();
    uint32_t base = 3000;  // Default 3 seconds
    
    // Class buff: SH4RP_TUSKS -1s lock time
    if (classBuffs & (uint8_t)ClassBuff::SH4RP_TUSKS) {
        base -= 1000;
    }
    
    // Class buff: OMNI_P0RK -5%
    if (classBuffs & (uint8_t)ClassBuff::OMNI_P0RK) {
        base = (base * 95) / 100;
    }
    
    return base;
}

float SwineStats::getDistanceXPMultiplier() {
    uint8_t classBuffs = calculateClassBuffs();
    float mult = 1.0f;
    
    // Class buff: R04D_H0G +15% distance XP
    if (classBuffs & (uint8_t)ClassBuff::R04D_H0G) {
        mult += 0.15f;
    }
    
    // Class buff: OMNI_P0RK +5%
    if (classBuffs & (uint8_t)ClassBuff::OMNI_P0RK) {
        mult *= 1.05f;
    }
    
    return mult;
}

float SwineStats::getCaptureXPMultiplier() {
    uint8_t classBuffs = calculateClassBuffs();
    float mult = 1.0f;
    
    // Class buff: CR4CK_NOSE +10% capture XP
    if (classBuffs & (uint8_t)ClassBuff::CR4CK_NOSE) {
        mult += 0.10f;
    }
    
    // Class buff: OMNI_P0RK +5%
    if (classBuffs & (uint8_t)ClassBuff::OMNI_P0RK) {
        mult *= 1.05f;
    }
    
    return mult;
}

const char* SwineStats::getClassBuffName(ClassBuff cb) {
    switch (cb) {
        case ClassBuff::P4CK3T_NOSE: return CLASS_BUFF_NAMES[0];
        case ClassBuff::H4RD_SNOUT:  return CLASS_BUFF_NAMES[1];
        case ClassBuff::R04D_H0G:    return CLASS_BUFF_NAMES[2];
        case ClassBuff::SH4RP_TUSKS: return CLASS_BUFF_NAMES[3];
        case ClassBuff::CR4CK_NOSE:  return CLASS_BUFF_NAMES[4];
        case ClassBuff::IR0N_TUSKS:  return CLASS_BUFF_NAMES[5];
        case ClassBuff::OMNI_P0RK:   return CLASS_BUFF_NAMES[6];
        default: return "???";
    }
}

const char* SwineStats::getClassBuffDesc(ClassBuff cb) {
    switch (cb) {
        case ClassBuff::P4CK3T_NOSE: return CLASS_BUFF_DESCS[0];
        case ClassBuff::H4RD_SNOUT:  return CLASS_BUFF_DESCS[1];
        case ClassBuff::R04D_H0G:    return CLASS_BUFF_DESCS[2];
        case ClassBuff::SH4RP_TUSKS: return CLASS_BUFF_DESCS[3];
        case ClassBuff::CR4CK_NOSE:  return CLASS_BUFF_DESCS[4];
        case ClassBuff::IR0N_TUSKS:  return CLASS_BUFF_DESCS[5];
        case ClassBuff::OMNI_P0RK:   return CLASS_BUFF_DESCS[6];
        default: return "";
    }
}

const char* SwineStats::getBuffName(PorkBuff b) {
    switch (b) {
        case PorkBuff::R4G3: return BUFF_NAMES[0];
        case PorkBuff::SNOUT_SHARP: return BUFF_NAMES[1];
        case PorkBuff::H0TSTR3AK: return BUFF_NAMES[2];
        case PorkBuff::C4FF31N4T3D: return BUFF_NAMES[3];
        default: return "???";
    }
}

const char* SwineStats::getDebuffName(PorkDebuff d) {
    switch (d) {
        case PorkDebuff::SLOP_SLUG: return DEBUFF_NAMES[0];
        case PorkDebuff::F0GSNOUT: return DEBUFF_NAMES[1];
        case PorkDebuff::TR0UGHDR41N: return DEBUFF_NAMES[2];
        case PorkDebuff::HAM_STR1NG: return DEBUFF_NAMES[3];
        default: return "???";
    }
}

const char* SwineStats::getBuffDesc(PorkBuff b) {
    switch (b) {
        case PorkBuff::R4G3: return BUFF_DESCS[0];
        case PorkBuff::SNOUT_SHARP: return BUFF_DESCS[1];
        case PorkBuff::H0TSTR3AK: return BUFF_DESCS[2];
        case PorkBuff::C4FF31N4T3D: return BUFF_DESCS[3];
        default: return "";
    }
}

const char* SwineStats::getDebuffDesc(PorkDebuff d) {
    switch (d) {
        case PorkDebuff::SLOP_SLUG: return DEBUFF_DESCS[0];
        case PorkDebuff::F0GSNOUT: return DEBUFF_DESCS[1];
        case PorkDebuff::TR0UGHDR41N: return DEBUFF_DESCS[2];
        case PorkDebuff::HAM_STR1NG: return DEBUFF_DESCS[3];
        default: return "";
    }
}

void SwineStats::draw(M5Canvas& canvas) {
    if (!active) return;
    
    canvas.fillSprite(COLOR_BG);
    canvas.setTextColor(COLOR_FG);
    
    // Draw tab bar at top
    drawTabBar(canvas);
    
    // Draw content based on current tab
    if (currentTab == StatsTab::STATS) {
        drawStatsTab(canvas);
    } else {
        drawBuffsTab(canvas);
    }
    
    // Footer hint - use MAIN_H since we're drawing on mainCanvas
    canvas.setTextDatum(bottom_center);
    canvas.setTextSize(1);
    canvas.drawString("<  >", DISPLAY_W / 2, MAIN_H - 2);
}

void SwineStats::drawTabBar(M5Canvas& canvas) {
    canvas.setTextSize(1);
    
    // Tab 1: ST4TS
    if (currentTab == StatsTab::STATS) {
        canvas.fillRect(2, 0, 60, 10, COLOR_FG);
        canvas.setTextColor(COLOR_BG);
    } else {
        canvas.drawRect(2, 0, 60, 10, COLOR_FG);
        canvas.setTextColor(COLOR_FG);
    }
    canvas.setTextDatum(middle_center);
    canvas.drawString("ST4TS", 32, 5);
    
    // Tab 2: B00STS
    if (currentTab == StatsTab::BOOSTS) {
        canvas.fillRect(65, 0, 60, 10, COLOR_FG);
        canvas.setTextColor(COLOR_BG);
    } else {
        canvas.drawRect(65, 0, 60, 10, COLOR_FG);
        canvas.setTextColor(COLOR_FG);
    }
    canvas.drawString("B00STS", 95, 5);
    
    // Reset text color
    canvas.setTextColor(COLOR_FG);
}

void SwineStats::drawStatsTab(M5Canvas& canvas) {
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    
    // Level and class info
    uint8_t level = XP::getLevel();
    const char* title = XP::getTitle();
    const char* className = XP::getClassName();
    uint8_t progress = XP::getProgress();
    
    char lvlBuf[48];
    snprintf(lvlBuf, sizeof(lvlBuf), "LVL %d: %s", level, title);
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

void SwineStats::drawBuffsTab(M5Canvas& canvas) {
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
        for (int i = 0; i < 7; i++) {
            ClassBuff cb = (ClassBuff)(1 << i);
            if (currentClassBuffs & (uint8_t)cb) {
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
        for (int i = 0; i < 4; i++) {
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
        for (int i = 0; i < 4; i++) {
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

void SwineStats::drawStats(M5Canvas& canvas) {
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
}


