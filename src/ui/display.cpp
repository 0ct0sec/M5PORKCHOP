// Display management implementation

#include "display.h"
#include <M5Cardputer.h>
#include <SD.h>
#include "../core/porkchop.h"
#include "../core/config.h"
#include "../core/xp.h"
#include "../build_info.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include "../modes/oink.h"
#include "../modes/warhog.h"
#include "../modes/piggyblues.h"
#include "../modes/spectrum.h"
#include "../gps/gps.h"
#include "../web/fileserver.h"
#include "menu.h"
#include "settings_menu.h"
#include "captures_menu.h"
#include "achievements_menu.h"
#include "swine_stats.h"
#include "boar_bros_menu.h"

// Theme color getters - read from config
// Theme definitions (single copy, declared extern in display.h)
const PorkTheme THEMES[THEME_COUNT] = {
    // Dark modes (colored text on black)
    {"P1NK",      0xFD75, 0x0000},  // Default piglet pink
    {"CYB3R",     0x07FF, 0x0000},  // Cyan/tron
    {"M4TR1X",    0x07E0, 0x0000},  // Green
    {"AMB3R",     0xFD20, 0x0000},  // Amber terminal
    {"BL00D",     0xF800, 0x0000},  // Red
    {"GH0ST",     0xFFFF, 0x0000},  // White mono
    // Inverted modes (black text on colored bg)
    {"PAP3R",     0x0000, 0xFFFF},  // Black on white
    {"BUBBLEGUM", 0x0000, 0xFD75},  // Black on pink
    {"M1NT",      0x0000, 0x07FF},  // Black on cyan
    {"SUNBURN",   0x0000, 0xFD20},  // Black on amber
    // Retro modes
    {"L1TTL3M1XY", 0x0B80, 0x9DE7}, // OG Game Boy LCD
    {"B4NSH33",   0x37E0, 0x0000},  // P1 phosphor green CRT
};

uint16_t getColorFG() {
    uint8_t idx = Config::personality().themeIndex;
    if (idx >= THEME_COUNT) idx = 0;
    return THEMES[idx].fg;
}

uint16_t getColorBG() {
    uint8_t idx = Config::personality().themeIndex;
    if (idx >= THEME_COUNT) idx = 0;
    return THEMES[idx].bg;
}

// Static member initialization
M5Canvas Display::topBar(&M5.Display);
M5Canvas Display::mainCanvas(&M5.Display);
M5Canvas Display::bottomBar(&M5.Display);
bool Display::gpsStatus = false;
bool Display::wifiStatus = false;
bool Display::mlStatus = false;
uint32_t Display::lastActivityTime = 0;
bool Display::dimmed = false;
bool Display::snapping = false;
String Display::bottomOverlay = "";

// PWNED banner state (displayed in top bar, persists until reboot)
static String lootSSID = "";

void Display::showLoot(const String& ssid) {
    lootSSID = ssid;
}

extern Porkchop porkchop;

void Display::init() {
    M5.Display.setRotation(1);
    M5.Display.fillScreen(COLOR_BG);
    M5.Display.setTextColor(COLOR_FG);
    
    // Create canvas sprites
    topBar.createSprite(DISPLAY_W, TOP_BAR_H);
    mainCanvas.createSprite(DISPLAY_W, MAIN_H);
    bottomBar.createSprite(DISPLAY_W, BOTTOM_BAR_H);
    
    topBar.setTextSize(1);
    mainCanvas.setTextSize(1);
    bottomBar.setTextSize(1);
    
    // Initialize dimming state
    lastActivityTime = millis();
    dimmed = false;
    
    Serial.println("[DISPLAY] Initialized");
}

void Display::update() {
    // Check for screen dimming
    updateDimming();
    
    drawTopBar();
    
    // Draw main content based on mode - reset all canvas state
    mainCanvas.fillSprite(COLOR_BG);
    mainCanvas.setTextColor(COLOR_FG);
    mainCanvas.setTextDatum(TL_DATUM);  // Reset to top-left (default)
    mainCanvas.setFont(&fonts::Font0);  // Reset to default font
    
    PorkchopMode mode = porkchop.getMode();
    
    switch (mode) {
        case PorkchopMode::IDLE:
            // Draw piglet avatar and mood
            Avatar::draw(mainCanvas);
            Mood::draw(mainCanvas);
            XP::drawBar(mainCanvas);  // XP bar below grass
            break;
            
        case PorkchopMode::OINK_MODE:
        case PorkchopMode::WARHOG_MODE:
        case PorkchopMode::PIGGYBLUES_MODE:
            // Draw piglet avatar and mood bubble (info embedded in bubble)
            Avatar::draw(mainCanvas);
            Mood::draw(mainCanvas);
            XP::drawBar(mainCanvas);  // XP bar below grass
            break;
            
        case PorkchopMode::SPECTRUM_MODE:
            // Spectrum mode draws its own content including XP bar
            SpectrumMode::draw(mainCanvas);
            break;
            
        case PorkchopMode::MENU:
            // Draw menu
            Menu::update();
            Menu::draw(mainCanvas);
            break;
            
        case PorkchopMode::SETTINGS:
            SettingsMenu::update();
            SettingsMenu::draw(mainCanvas);
            break;
            
        case PorkchopMode::CAPTURES:
            CapturesMenu::draw(mainCanvas);
            break;
            
        case PorkchopMode::ACHIEVEMENTS:
            AchievementsMenu::draw(mainCanvas);
            break;
            
        case PorkchopMode::ABOUT:
            drawAboutScreen(mainCanvas);
            break;
            
        case PorkchopMode::FILE_TRANSFER:
            drawFileTransferScreen(mainCanvas);
            break;
            
        case PorkchopMode::LOG_VIEWER:
            // LogViewer::render() handles main canvas and bottom bar
            // We only need to draw top bar here (mode indicator, clock, battery)
            drawTopBar();
            return;
            
        case PorkchopMode::SWINE_STATS:
            SwineStats::draw(mainCanvas);
            break;
            
        case PorkchopMode::BOAR_BROS:
            BoarBrosMenu::draw(mainCanvas);
            break;
    }
    
    drawBottomBar();
    pushAll();
}

void Display::clear() {
    topBar.fillSprite(COLOR_BG);
    mainCanvas.fillSprite(COLOR_BG);
    bottomBar.fillSprite(COLOR_BG);
    pushAll();
}

void Display::pushAll() {
    M5.Display.startWrite();
    topBar.pushSprite(0, 0);
    mainCanvas.pushSprite(0, TOP_BAR_H);
    bottomBar.pushSprite(0, DISPLAY_H - BOTTOM_BAR_H);
    M5.Display.endWrite();
}

void Display::drawTopBar() {
    topBar.fillSprite(COLOR_BG);
    topBar.setTextColor(COLOR_FG);
    topBar.setTextSize(1);
    
    // Left side: mode indicator
    PorkchopMode mode = porkchop.getMode();
    String modeStr;
    uint16_t modeColor = COLOR_FG;
    
    switch (mode) {
        case PorkchopMode::IDLE:
            modeStr = "IDLE";
            break;
        case PorkchopMode::OINK_MODE:
            modeStr = "OINK";
            modeColor = COLOR_ACCENT;
            break;
        case PorkchopMode::WARHOG_MODE:
            modeStr = "WARHOG";
            modeColor = COLOR_DANGER;
            break;
        case PorkchopMode::PIGGYBLUES_MODE:
            modeStr = "PIGGY BLUES";
            modeColor = COLOR_ACCENT;
            break;
        case PorkchopMode::SPECTRUM_MODE:
            modeStr = "HOG ON SPECTRUM";
            modeColor = COLOR_ACCENT;
            break;
        case PorkchopMode::MENU:
            modeStr = "MENU";
            break;
        case PorkchopMode::SETTINGS:
            modeStr = "CONFIG";
            break;
        case PorkchopMode::ABOUT:
            modeStr = "ABOUT";
            break;
        case PorkchopMode::FILE_TRANSFER:
            modeStr = "XFER";
            modeColor = COLOR_SUCCESS;
            break;
        case PorkchopMode::LOG_VIEWER:
            modeStr = "LOG VIEWER";
            break;
        case PorkchopMode::CAPTURES:
            {
                char buf[24];
                snprintf(buf, sizeof(buf), "L00T (%d)", CapturesMenu::getCount());
                modeStr = buf;
            }
            modeColor = COLOR_ACCENT;
            break;
        case PorkchopMode::ACHIEVEMENTS:
            {
                char buf[24];
                snprintf(buf, sizeof(buf), "PR00F (%d/%d)", XP::getUnlockedCount(), AchievementsMenu::TOTAL_ACHIEVEMENTS);
                modeStr = buf;
            }
            modeColor = COLOR_ACCENT;
            break;
        case PorkchopMode::SWINE_STATS:
            modeStr = "SW1N3 ST4TS";
            modeColor = COLOR_ACCENT;
            break;
        case PorkchopMode::BOAR_BROS:
            {
                char buf[24];
                snprintf(buf, sizeof(buf), "B04R BR0S (%d)", BoarBrosMenu::getCount());
                modeStr = buf;
            }
            modeColor = COLOR_ACCENT;
            break;
    }
    
    // Append mood indicator
    int happiness = Mood::getCurrentHappiness();
    String moodLabel;
    if (happiness > 70) moodLabel = "HYP3";
    else if (happiness > 30) moodLabel = "GUD";
    else if (happiness > -10) moodLabel = "0K";
    else if (happiness > -50) moodLabel = "M3H";
    else moodLabel = "S4D";
    modeStr += " " + moodLabel;
    
    // Append PWNED banner if active (only in OINK mode, persists until reboot)
    if (mode == PorkchopMode::OINK_MODE && lootSSID.length() > 0) {
        modeStr += " PWNED " + lootSSID;
    }
    
    topBar.setTextColor(modeColor);
    topBar.setTextDatum(top_left);
    
    // Calculate right side width first for truncation
    String timeStr = GPS::hasFix() ? GPS::getTimeString() : "--:--";
    int battLevel = M5.Power.getBatteryLevel();
    String battStr = String(battLevel) + "%";
    String status = "";
    status += gpsStatus ? "G" : "-";
    status += wifiStatus ? "W" : "-";
    status += mlStatus ? "M" : "-";
    String rightStr = battStr + " " + status + " " + timeStr;
    int rightWidth = topBar.textWidth(rightStr);
    
    // Truncate left string if it would overlap right side
    int maxLeftWidth = DISPLAY_W - rightWidth - 8;  // 8px margin
    while (topBar.textWidth(modeStr) > maxLeftWidth && modeStr.length() > 10) {
        modeStr = modeStr.substring(0, modeStr.length() - 1);
    }
    if (topBar.textWidth(modeStr) > maxLeftWidth && modeStr.length() > 3) {
        modeStr = modeStr.substring(0, modeStr.length() - 2) + "..";
    }
    
    topBar.drawString(modeStr, 2, 2);

    // Clock (from GPS or --:--)
    topBar.setTextColor(COLOR_FG);
    
    // Right side: battery + status icons
    topBar.setTextDatum(top_right);
    
    // Draw battery then status
    topBar.drawString(rightStr, DISPLAY_W - 2, 2);
}

void Display::drawBottomBar() {
    bottomBar.fillSprite(COLOR_BG);
    bottomBar.setTextColor(COLOR_ACCENT);  // Use accent color for stats
    bottomBar.setTextSize(1);
    bottomBar.setTextDatum(top_left);
    
    // Check for overlay message (used during confirmation dialogs)
    if (bottomOverlay.length() > 0) {
        bottomBar.setTextDatum(top_center);
        bottomBar.drawString(bottomOverlay, DISPLAY_W / 2, 3);
        return;
    }
    
    PorkchopMode mode = porkchop.getMode();
    String stats;
    
    if (mode == PorkchopMode::WARHOG_MODE) {
        // WARHOG: show unique networks, saved, distance, GPS info
        uint32_t unique = WarhogMode::getTotalNetworks();
        uint32_t saved = WarhogMode::getSavedCount();
        uint32_t distM = XP::getSession().distanceM;
        GPSData gps = GPS::getData();
        
        char buf[64];
        if (GPS::hasFix()) {
            // Format distance nicely: meters or km
            if (distM >= 1000) {
                // Show as km with 1 decimal: "1.2km"
                snprintf(buf, sizeof(buf), "U:%lu S:%lu D:%.1fkm [%.2f,%.2f]", 
                         unique, saved, distM / 1000.0, gps.latitude, gps.longitude);
            } else {
                // Show as meters: "456m"
                snprintf(buf, sizeof(buf), "U:%lu S:%lu D:%lum [%.2f,%.2f]", 
                         unique, saved, distM, gps.latitude, gps.longitude);
            }
        } else {
            // No fix - show satellite count
            snprintf(buf, sizeof(buf), "U:%lu S:%lu D:%lum GPS:%dsat", 
                     unique, saved, distM, gps.satellites);
        }
        stats = String(buf);
    } else if (mode == PorkchopMode::CAPTURES) {
        // CAPTURES: show selected capture's BSSID
        stats = CapturesMenu::getSelectedBSSID();
    } else if (mode == PorkchopMode::SETTINGS) {
        // SETTINGS: show description of selected item
        stats = SettingsMenu::getSelectedDescription();
    } else if (mode == PorkchopMode::MENU) {
        // MENU: show description of selected item
        stats = Menu::getSelectedDescription();
    } else if (mode == PorkchopMode::LOG_VIEWER) {
        // LOG_VIEWER: show scroll hint
        stats = "[;/.] scroll  [Bksp] exit";
    } else if (mode == PorkchopMode::OINK_MODE) {
        // OINK: show Networks, Handshakes, Channel, and optionally Deauths/BRO count
        // (PWNED banner is shown in top bar)
        // In DO NO HAM mode, hide D: counter since we're passive
        // In LOCKING state, show target SSID and client discovery count
        uint16_t netCount = OinkMode::getNetworkCount();
        uint16_t hsCount = OinkMode::getCompleteHandshakeCount();
        uint32_t deauthCount = OinkMode::getDeauthCount();
        uint8_t channel = OinkMode::getChannel();
        uint16_t broCount = OinkMode::getExcludedCount();
        bool passive = Config::wifi().doNoHam;
        bool locking = OinkMode::isLocking();
        char buf[64];
        
        if (locking) {
            // LOCKING state: show target and discovered clients
            const char* targetSSID = OinkMode::getTargetSSID();
            uint8_t clients = OinkMode::getTargetClientCount();
            // Truncate SSID if too long for display
            char ssidShort[16];
            strncpy(ssidShort, targetSSID, 15);
            ssidShort[15] = '\0';
            snprintf(buf, sizeof(buf), "LOCK:%s C:%d CH:%d", ssidShort, clients, channel);
        } else if (passive) {
            // DO NO HAM: no D: counter (we don't deauth)
            if (broCount > 0) {
                snprintf(buf, sizeof(buf), "N:%d HS:%d CH:%d BRO:%d DOIN NO HAM", netCount, hsCount, channel, broCount);
            } else {
                snprintf(buf, sizeof(buf), "N:%d HS:%d CH:%d DOIN NO HAM", netCount, hsCount, channel);
            }
        } else {
            // Attack mode: show D: counter
            if (broCount > 0) {
                snprintf(buf, sizeof(buf), "N:%d HS:%d D:%lu CH:%d BRO:%d", netCount, hsCount, deauthCount, channel, broCount);
            } else {
                snprintf(buf, sizeof(buf), "N:%d HS:%d D:%lu CH:%d", netCount, hsCount, deauthCount, channel);
            }
        }
        stats = String(buf);
    } else if (mode == PorkchopMode::PIGGYBLUES_MODE) {
        // PIGGYBLUES: TX:total A:apple G:android S:samsung W:windows
        uint32_t total = PiggyBluesMode::getTotalPackets();
        uint32_t apple = PiggyBluesMode::getAppleCount();
        uint32_t android = PiggyBluesMode::getAndroidCount();
        uint32_t samsung = PiggyBluesMode::getSamsungCount();
        uint32_t windows = PiggyBluesMode::getWindowsCount();
        char buf[48];
        snprintf(buf, sizeof(buf), "TX:%lu A:%lu G:%lu S:%lu W:%lu", total, apple, android, samsung, windows);
        stats = String(buf);
    } else if (mode == PorkchopMode::SPECTRUM_MODE) {
        // SPECTRUM: show selected network info or scan status
        stats = SpectrumMode::getSelectedInfo();
    } else if (mode == PorkchopMode::BOAR_BROS) {
        // BOAR BROS: show delete hint
        stats = "[D] delete";
    } else {
        // Default: Networks, Handshakes, Deauths
        uint16_t netCount = porkchop.getNetworkCount();
        uint16_t hsCount = porkchop.getHandshakeCount();
        uint16_t deauthCount = porkchop.getDeauthCount();
        stats = "N:" + String(netCount) + " HS:" + String(hsCount) + " D:" + String(deauthCount);
    }
    
    bottomBar.drawString(stats, 2, 3);
    
    // Right: uptime
    bottomBar.setTextDatum(top_right);
    uint32_t uptime = porkchop.getUptime();
    uint16_t mins = uptime / 60;
    uint16_t secs = uptime % 60;
    String uptimeStr = String(mins) + ":" + (secs < 10 ? "0" : "") + String(secs);
    bottomBar.drawString(uptimeStr, DISPLAY_W - 2, 3);
}

void Display::showInfoBox(const String& title, const String& line1, 
                          const String& line2, bool blocking) {
    mainCanvas.fillSprite(COLOR_BG);
    mainCanvas.setTextColor(COLOR_FG);
    
    // Draw border
    mainCanvas.drawRect(10, 5, DISPLAY_W - 20, MAIN_H - 10, COLOR_FG);
    
    // Title
    mainCanvas.setTextDatum(top_center);
    mainCanvas.setTextSize(2);
    mainCanvas.drawString(title, DISPLAY_W / 2, 15);
    
    // Content
    mainCanvas.setTextSize(1);
    mainCanvas.drawString(line1, DISPLAY_W / 2, 45);
    if (line2.length() > 0) {
        mainCanvas.drawString(line2, DISPLAY_W / 2, 60);
    }
    
    if (blocking) {
        mainCanvas.drawString("[ENTER to continue]", DISPLAY_W / 2, MAIN_H - 20);
    }
    
    pushAll();
    
    if (blocking) {
        uint32_t startTime = millis();
        while ((millis() - startTime) < 60000) {  // 60s timeout
            M5.update();
            M5Cardputer.update();
            if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
                while (M5Cardputer.Keyboard.isPressed()) {
                    M5.update();
                    M5Cardputer.update();
                    delay(10);
                }
                break;
            }
            delay(10);
        }
    }
}

bool Display::showConfirmBox(const String& title, const String& message) {
    mainCanvas.fillSprite(COLOR_BG);
    mainCanvas.setTextColor(COLOR_FG);
    
    mainCanvas.drawRect(10, 5, DISPLAY_W - 20, MAIN_H - 10, COLOR_FG);
    
    mainCanvas.setTextDatum(top_center);
    mainCanvas.setTextSize(2);
    mainCanvas.drawString(title, DISPLAY_W / 2, 15);
    
    mainCanvas.setTextSize(1);
    mainCanvas.drawString(message, DISPLAY_W / 2, 45);
    mainCanvas.drawString("[Y]es / [N]o", DISPLAY_W / 2, MAIN_H - 20);
    
    pushAll();
    
    uint32_t startTime = millis();
    while ((millis() - startTime) < 30000) {  // 30s timeout, default No
        M5.update();
        M5Cardputer.update();
        
        if (M5Cardputer.Keyboard.isChange()) {
            auto keys = M5Cardputer.Keyboard.keysState();
            for (auto c : keys.word) {
                if (c == 'y' || c == 'Y') return true;
                if (c == 'n' || c == 'N') return false;
            }
        }
        delay(10);
    }
    return false;  // Timeout = No
}

// Boot splash - 3 screens: OINK OINK, MY NAME IS, PORKCHOP
void Display::showBootSplash() {
    // Screen 1: OINK OINK
    M5.Display.fillScreen(COLOR_BG);
    M5.Display.setTextColor(COLOR_FG);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextSize(4);
    M5.Display.drawString("OINK", DISPLAY_W / 2, DISPLAY_H / 2 - 20);
    M5.Display.drawString("OINK", DISPLAY_W / 2, DISPLAY_H / 2 + 20);
    delay(800);
    
    // Screen 2: MY NAME IS
    M5.Display.fillScreen(COLOR_BG);
    M5.Display.setTextSize(3);
    M5.Display.drawString("MY NAME IS", DISPLAY_W / 2, DISPLAY_H / 2);
    delay(800);
    
    // Screen 3: PORKCHOP in big stylized text
    M5.Display.fillScreen(COLOR_BG);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextSize(3);
    M5.Display.drawString("PORKCHOP", DISPLAY_W / 2, DISPLAY_H / 2 - 15);
    
    // Subtitle
    M5.Display.setTextSize(1);
    M5.Display.drawString("Basically you, but as an ASCII pig.", DISPLAY_W / 2, DISPLAY_H / 2 + 20);
    M5.Display.drawString("BETA", DISPLAY_W / 2, DISPLAY_H / 2 + 35);
    
    delay(1200);
}


void Display::showProgress(const String& title, uint8_t percent) {
    mainCanvas.fillSprite(COLOR_BG);
    mainCanvas.setTextColor(COLOR_FG);
    
    mainCanvas.setTextDatum(top_center);
    mainCanvas.setTextSize(2);
    mainCanvas.drawString(title, DISPLAY_W / 2, 20);
    
    // Progress bar
    int barW = DISPLAY_W - 40;
    int barH = 15;
    int barX = 20;
    int barY = MAIN_H / 2;
    
    mainCanvas.drawRect(barX, barY, barW, barH, COLOR_FG);
    int fillW = (barW - 2) * percent / 100;
    mainCanvas.fillRect(barX + 1, barY + 1, fillW, barH - 2, COLOR_ACCENT);
    
    // Percentage text
    mainCanvas.setTextSize(1);
    mainCanvas.drawString(String(percent) + "%", DISPLAY_W / 2, barY + barH + 10);
    
    pushAll();
}

void Display::showToast(const String& message) {
    // Draw a centered pink box with black text - inverted from normal theme
    int boxW = 160;
    int boxH = 50;
    int boxX = (DISPLAY_W - boxW) / 2;
    int boxY = (MAIN_H - boxH) / 2;
    
    mainCanvas.fillSprite(COLOR_BG);
    
    // Black border then pink fill
    mainCanvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
    mainCanvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);
    
    // Black text on pink background
    mainCanvas.setTextColor(COLOR_BG, COLOR_FG);
    mainCanvas.setTextDatum(MC_DATUM);
    mainCanvas.setTextSize(1);
    mainCanvas.setFont(&fonts::Font0);
    mainCanvas.drawString(message, DISPLAY_W / 2, boxY + boxH / 2);
    
    pushAll();
}

void Display::showLevelUp(uint8_t oldLevel, uint8_t newLevel) {
    // Level up popup - pink filled box with black text, auto-dismiss after 2.5s
    // Level up phrases
    static const char* LEVELUP_PHRASES[] = {
        "snout grew stronger",
        "new truffle unlocked",
        "skill issue? not anymore",
        "gg ez level up",
        "evolution complete",
        "power level rising",
        "oink intensifies",
        "XP printer go brrr",
        "grinding them levels",
        "swine on the rise"
    };
    static const uint8_t PHRASE_COUNT = 10;
    
    int boxW = 200;
    int boxH = 70;
    int boxX = (DISPLAY_W - boxW) / 2;
    int boxY = (MAIN_H - boxH) / 2;
    
    mainCanvas.fillSprite(COLOR_BG);
    
    // Black border then pink fill
    mainCanvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
    mainCanvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);
    
    // Black text on pink background
    mainCanvas.setTextColor(COLOR_BG, COLOR_FG);
    mainCanvas.setTextDatum(top_center);
    mainCanvas.setTextSize(1);
    mainCanvas.setFont(&fonts::Font0);
    
    int centerX = DISPLAY_W / 2;
    
    // Header
    mainCanvas.drawString("* LEVEL UP! *", centerX, boxY + 8);
    
    // Level change
    char levelStr[24];
    snprintf(levelStr, sizeof(levelStr), "LV %d -> LV %d", oldLevel, newLevel);
    mainCanvas.drawString(levelStr, centerX, boxY + 22);
    
    // New title
    const char* title = XP::getTitleForLevel(newLevel);
    mainCanvas.drawString(title, centerX, boxY + 36);
    
    // Random phrase
    int phraseIdx = random(0, PHRASE_COUNT);
    mainCanvas.drawString(LEVELUP_PHRASES[phraseIdx], centerX, boxY + 52);
    
    pushAll();
    
    // Celebratory beep sequence
    if (Config::personality().soundEnabled) {
        M5.Speaker.tone(800, 100);
        delay(120);
        M5.Speaker.tone(1000, 100);
        delay(120);
        M5.Speaker.tone(1200, 150);
    }
    
    // Auto-dismiss after 2.5 seconds or on any key press
    uint32_t startTime = millis();
    while ((millis() - startTime) < 2500) {
        M5.update();
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isChange()) {
            break;  // Any key dismisses
        }
        delay(50);
    }
}

void Display::showClassPromotion(const char* oldClass, const char* newClass) {
    // Class promotion popup - piglet got a new class tier!
    static const char* CLASS_PHRASES[] = {
        "new powers acquired",
        "rank up complete",
        "class tier unlocked", 
        "evolution in progress",
        "truffle mastery grows",
        "snout sharpened",
        "oink level: elite"
    };
    static const uint8_t PHRASE_COUNT = 7;
    
    int boxW = 210;
    int boxH = 60;
    int boxX = (DISPLAY_W - boxW) / 2;
    int boxY = (MAIN_H - boxH) / 2;
    
    mainCanvas.fillSprite(COLOR_BG);
    
    // Black border then pink fill
    mainCanvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
    mainCanvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);
    
    // Black text on pink background
    mainCanvas.setTextColor(COLOR_BG, COLOR_FG);
    mainCanvas.setTextDatum(top_center);
    mainCanvas.setTextSize(1);
    mainCanvas.setFont(&fonts::Font0);
    
    int centerX = DISPLAY_W / 2;
    
    // Header
    mainCanvas.drawString("* CL4SS PR0M0T10N *", centerX, boxY + 8);
    
    // Class change
    char classStr[48];
    snprintf(classStr, sizeof(classStr), "%s -> %s", oldClass, newClass);
    mainCanvas.drawString(classStr, centerX, boxY + 24);
    
    // Random phrase
    int phraseIdx = random(0, PHRASE_COUNT);
    mainCanvas.drawString(CLASS_PHRASES[phraseIdx], centerX, boxY + 40);
    
    pushAll();
    
    // Distinct beep sequence (different from level up)
    if (Config::personality().soundEnabled) {
        M5.Speaker.tone(500, 80);
        delay(100);
        M5.Speaker.tone(700, 80);
        delay(100);
        M5.Speaker.tone(900, 80);
        delay(100);
        M5.Speaker.tone(1100, 150);
    }
    
    // Auto-dismiss after 2.5 seconds or on any key press
    uint32_t startTime = millis();
    while ((millis() - startTime) < 2500) {
        M5.update();
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isChange()) {
            break;
        }
        delay(50);
    }
}

void Display::setBottomOverlay(const String& message) {
    bottomOverlay = message;
}

void Display::clearBottomOverlay() {
    bottomOverlay = "";
}

void Display::setGPSStatus(bool hasFix) {
    gpsStatus = hasFix;
}

void Display::setWiFiStatus(bool connected) {
    wifiStatus = connected;
}

void Display::setMLStatus(bool active) {
    mlStatus = active;
}

// Helper functions for mode screens
void Display::drawModeInfo(M5Canvas& canvas, PorkchopMode mode) {
    canvas.setTextColor(COLOR_FG);
    canvas.setTextDatum(top_left);
    canvas.setTextSize(1);
    
    if (mode == PorkchopMode::OINK_MODE) {
        const auto& networks = OinkMode::getNetworks();
        int selIdx = OinkMode::getSelectionIndex();
        DetectedNetwork* target = OinkMode::getTarget();
        
        // Show current target being attacked (like M5Gotchi)
        if (target) {
            canvas.setTextColor(COLOR_SUCCESS);
            String ssid = String(target->ssid);
            if (ssid.length() == 0) ssid = "<hidden>";
            canvas.drawString("ATTACKING:", 2, 2);
            canvas.setTextColor(COLOR_ACCENT);
            canvas.drawString(ssid.substring(0, 16), 2, 14);
            
            char info[32];
            snprintf(info, sizeof(info), "CH%d %ddB", target->channel, target->rssi);
            canvas.setTextColor(COLOR_FG);
            canvas.drawString(info, 2, 26);
        } else if (!networks.empty()) {
            canvas.setTextColor(COLOR_FG);
            canvas.drawString("sniffin", 2, 2);
            canvas.setTextColor(COLOR_ACCENT);
            char buf[32];
            snprintf(buf, sizeof(buf), "found %d truffles", (int)networks.size());
            canvas.drawString(buf, 2, 14);
        } else {
            canvas.drawString("hunting truffles", 2, MAIN_H / 2 - 5);
        }
        
        // Show stats at bottom
        canvas.setTextColor(COLOR_FG);
        uint16_t hsCount = OinkMode::getCompleteHandshakeCount();
        uint32_t deauthCnt = OinkMode::getDeauthCount();
        char stats[48];
        snprintf(stats, sizeof(stats), "N:%d HS:%d D:%lu [Bksp]=Stop", 
                 (int)networks.size(), hsCount, deauthCnt);
        canvas.drawString(stats, 2, MAIN_H - 12);
    } else if (mode == PorkchopMode::WARHOG_MODE) {
        // Show wardriving info
        canvas.drawString("Wardriving mode active", 2, MAIN_H - 25);
        canvas.drawString("Collecting GPS + WiFi data", 2, MAIN_H - 15);
    }
}

void Display::drawSettingsScreen(M5Canvas& canvas) {
    canvas.setTextColor(COLOR_FG);
    canvas.setTextDatum(top_center);
    canvas.setTextSize(1);
    
    canvas.drawString("=== SETTINGS ===", DISPLAY_W / 2, 5);
    
    canvas.setTextDatum(top_left);
    int y = 20;
    canvas.drawString("Sound: ON", 10, y); y += 12;
    canvas.drawString("Brightness: 100%", 10, y); y += 12;
    canvas.drawString("Auto-save HS: ON", 10, y); y += 12;
    canvas.drawString("CH Hop: 100ms", 10, y); y += 12;
    canvas.drawString("Deauth delay: 50ms", 10, y);
    
    canvas.setTextDatum(top_center);
    canvas.setTextColor(COLOR_ACCENT);
    canvas.drawString("[Enter] to go back", DISPLAY_W / 2, MAIN_H - 12);
}

// Hacker culture quotes for About screen
static const char* ABOUT_QUOTES[] = {
    "hack the planet",
    "the gibson is ours",
    "mess with the best",
    "there is no spoon",
    "i am invincible!",
    "shall we play a game?",
    "the only winning move",
    "ph34r the piglet",
    "0wn3d by 0ct0",
    "root@porkchop:~#",
    "sudo make me bacon",
    "rm -rf /trust",
    "#!/usr/bin/oink",
    "while(1) { pwn(); }",
    "segfault in the matrix",
    "buffer overflow ur life",
    "0xDEADBEEF",
    "0xCAFEBABE",
    "all your base",
    "never gonna give u up"
};
static const int ABOUT_QUOTES_COUNT = sizeof(ABOUT_QUOTES) / sizeof(ABOUT_QUOTES[0]);
static int aboutQuoteIndex = 0;
static int aboutEnterCount = 0;
static bool aboutAchievementShown = false;

void Display::resetAboutState() {
    // Pick new random quote each time we enter About
    aboutQuoteIndex = random(0, ABOUT_QUOTES_COUNT);
    aboutEnterCount = 0;
    aboutAchievementShown = false;
}

void Display::onAboutEnterPressed() {
    aboutEnterCount++;
    
    // Easter egg: 5 presses unlocks achievement
    if (aboutEnterCount >= 5 && !aboutAchievementShown) {
        if (!XP::hasAchievement(ACH_ABOUT_JUNKIE)) {
            XP::unlockAchievement(ACH_ABOUT_JUNKIE);
            showToast("AB0UT_JUNK13 unlocked!");
        }
        aboutAchievementShown = true;
    }
    
    // Cycle to next quote
    aboutQuoteIndex = (aboutQuoteIndex + 1) % ABOUT_QUOTES_COUNT;
}

void Display::drawAboutScreen(M5Canvas& canvas) {
    canvas.setTextColor(COLOR_FG);
    canvas.setTextDatum(top_center);
    
    // Title
    canvas.setTextSize(2);
    canvas.setTextColor(COLOR_ACCENT);
    canvas.drawString("M5PORKCHOP", DISPLAY_W / 2, 5);
    
    // Version
    canvas.setTextSize(1);
    canvas.drawString("v" BUILD_VERSION, DISPLAY_W / 2, 25);
    
    // Author
    canvas.setTextColor(COLOR_FG);
    canvas.drawString("by 0ct0", DISPLAY_W / 2, 38);
    
    // GitHub (single line)
    canvas.drawString("github.com/0ct0sec/M5PORKCHOP", DISPLAY_W / 2, 50);
    
    // Build date
    canvas.setTextColor(COLOR_ACCENT);
    canvas.drawString("built: " __DATE__, DISPLAY_W / 2, 64);
    
    // Random quote
    canvas.setTextColor(COLOR_FG);
    char quoteBuf[48];
    snprintf(quoteBuf, sizeof(quoteBuf), "\"%s\"", ABOUT_QUOTES[aboutQuoteIndex]);
    canvas.drawString(quoteBuf, DISPLAY_W / 2, 78);
    
    // Easter egg hint
    canvas.setTextColor(COLOR_ACCENT);
    canvas.drawString("[Enter] ???", DISPLAY_W / 2, MAIN_H - 12);
}

void Display::drawFileTransferScreen(M5Canvas& canvas) {
    canvas.setTextColor(COLOR_FG);
    canvas.setTextDatum(top_center);
    
    canvas.setTextSize(2);
    canvas.setTextColor(COLOR_ACCENT);
    canvas.drawString("FILE TRANSFER", DISPLAY_W / 2, 5);
    
    canvas.setTextSize(1);
    canvas.setTextColor(COLOR_FG);
    
    if (FileServer::isConnecting()) {
        // Show connection progress
        canvas.drawString("jacking in.", DISPLAY_W / 2, 30);
        canvas.setTextColor(COLOR_ACCENT);
        canvas.drawString(Config::wifi().otaSSID, DISPLAY_W / 2, 45);
        canvas.setTextColor(COLOR_FG);
        canvas.drawString(FileServer::getStatus(), DISPLAY_W / 2, 60);
    } else if (FileServer::isRunning() && FileServer::isConnected()) {
        // Show IP address
        canvas.drawString("Connected! Browse to:", DISPLAY_W / 2, 30);
        
        canvas.setTextColor(COLOR_SUCCESS);
        String url = "http://" + FileServer::getIP();
        canvas.drawString(url, DISPLAY_W / 2, 45);
        
        canvas.setTextColor(COLOR_FG);
        canvas.drawString("or http://porkchop.local", DISPLAY_W / 2, 60);
    } else if (FileServer::isRunning()) {
        // Server running but WiFi lost
        canvas.drawString("link dead.", DISPLAY_W / 2, 35);
        canvas.setTextColor(COLOR_ACCENT);
        canvas.drawString("retry hack.", DISPLAY_W / 2, 50);
    } else {
        // Not running - check why
        canvas.setTextColor(COLOR_ACCENT);
        String ssid = Config::wifi().otaSSID;
        if (ssid.length() > 0) {
            canvas.drawString("Connection failed", DISPLAY_W / 2, 35);
            canvas.drawString("SSID: " + ssid, DISPLAY_W / 2, 50);
            canvas.setTextColor(COLOR_FG);
            canvas.drawString(FileServer::getStatus(), DISPLAY_W / 2, 65);
        } else {
            canvas.drawString("no creds lol.", DISPLAY_W / 2, 35);
            canvas.drawString("Set SSID in Settings", DISPLAY_W / 2, 50);
        }
    }
    
    canvas.setTextColor(COLOR_ACCENT);
    canvas.drawString("[Bksp] to stop", DISPLAY_W / 2, MAIN_H - 12);
}

void Display::resetDimTimer() {
    lastActivityTime = millis();
    if (dimmed) {
        // Restore full brightness
        dimmed = false;
        uint8_t brightness = Config::personality().brightness;
        M5.Display.setBrightness(brightness * 255 / 100);
    }
}

void Display::updateDimming() {
    uint16_t timeout = Config::personality().dimTimeout;
    if (timeout == 0) return;  // Dimming disabled
    
    uint32_t elapsed = (millis() - lastActivityTime) / 1000;
    
    if (!dimmed && elapsed >= timeout) {
        // Time to dim
        dimmed = true;
        uint8_t dimLevel = Config::personality().dimLevel;
        M5.Display.setBrightness(dimLevel * 255 / 100);
    }
}

// Screenshot constants
static const int SCREENSHOT_RETRY_COUNT = 3;
static const int SCREENSHOT_RETRY_DELAY_MS = 10;

// Helper: find next screenshot number by scanning directory
static uint16_t getNextScreenshotNumber() {
    uint16_t maxNum = 0;
    
    File dir = SD.open("/screenshots");
    if (!dir || !dir.isDirectory()) {
        return 1;
    }
    
    File entry;
    while ((entry = dir.openNextFile())) {
        String name = entry.name();
        entry.close();
        
        // Parse "screenshotNNN.bmp" format
        if (name.startsWith("screenshot") && name.endsWith(".bmp")) {
            String numStr = name.substring(10, name.length() - 4);
            uint16_t num = numStr.toInt();
            if (num > maxNum) maxNum = num;
        }
    }
    dir.close();
    
    return maxNum + 1;
}

bool Display::takeScreenshot() {
    // Prevent re-entry
    if (snapping) return false;
    
    // Check SD availability
    if (!Config::isSDAvailable()) {
        showToast("No SD card!");
        return false;
    }
    
    snapping = true;
    
    // Ensure screenshots directory exists
    if (!SD.exists("/screenshots")) {
        SD.mkdir("/screenshots");
    }
    
    // Find next available number
    uint16_t num = getNextScreenshotNumber();
    char path[48];
    snprintf(path, sizeof(path), "/screenshots/screenshot%03d.bmp", num);
    
    Serial.printf("[DISPLAY] Taking screenshot: %s\n", path);
    
    // Open file with retry
    File file;
    for (int retry = 0; retry < SCREENSHOT_RETRY_COUNT; retry++) {
        file = SD.open(path, FILE_WRITE);
        if (file) break;
        delay(SCREENSHOT_RETRY_DELAY_MS);
    }
    
    if (!file) {
        Serial.println("[DISPLAY] Failed to open screenshot file");
        showToast("SD write failed!");
        snapping = false;
        return false;
    }
    
    // BMP file structure for 240x135 24-bit image
    int image_width = DISPLAY_W;
    int image_height = DISPLAY_H;
    
    // Horizontal lines must be padded to multiple of 4 bytes
    const uint32_t pad = (4 - (3 * image_width) % 4) % 4;
    uint32_t filesize = 54 + (3 * image_width + pad) * image_height;
    
    // BMP header (54 bytes)
    unsigned char header[54] = {
        'B', 'M',           // BMP signature
        0, 0, 0, 0,         // File size (filled below)
        0, 0, 0, 0,         // Reserved
        54, 0, 0, 0,        // Pixel data offset
        40, 0, 0, 0,        // DIB header size
        0, 0, 0, 0,         // Width (filled below)
        0, 0, 0, 0,         // Height (filled below)
        1, 0,               // Color planes
        24, 0,              // Bits per pixel
        0, 0, 0, 0,         // Compression (none)
        0, 0, 0, 0,         // Image size (can be 0 for uncompressed)
        0, 0, 0, 0,         // Horizontal resolution
        0, 0, 0, 0,         // Vertical resolution
        0, 0, 0, 0,         // Colors in palette
        0, 0, 0, 0          // Important colors
    };
    
    // Fill in size fields
    for (uint32_t i = 0; i < 4; i++) {
        header[2 + i] = (filesize >> (8 * i)) & 0xFF;
        header[18 + i] = (image_width >> (8 * i)) & 0xFF;
        header[22 + i] = (image_height >> (8 * i)) & 0xFF;
    }
    
    file.write(header, 54);
    
    // Line buffer with padding
    unsigned char line_data[image_width * 3 + pad];
    
    // Initialize padding bytes to 0
    for (int i = image_width * 3; i < image_width * 3 + (int)pad; i++) {
        line_data[i] = 0;
    }
    
    // BMP stores bottom-to-top, so read from bottom up
    for (int y = image_height - 1; y >= 0; y--) {
        // Read one line of RGB data from display
        M5.Display.readRectRGB(0, y, image_width, 1, line_data);
        
        // Swap R and B (BMP uses BGR order)
        for (int x = 0; x < image_width; x++) {
            unsigned char temp = line_data[x * 3];
            line_data[x * 3] = line_data[x * 3 + 2];
            line_data[x * 3 + 2] = temp;
        }
        
        file.write(line_data, image_width * 3 + pad);
    }
    
    file.close();
    
    Serial.printf("[DISPLAY] Screenshot saved: %s (%lu bytes)\n", path, filesize);
    
    // Show success toast and hold for visibility
    char msg[32];
    snprintf(msg, sizeof(msg), "SNAP! #%d", num);
    showToast(msg);
    delay(1000);  // Hold toast for 1 second so user sees it
    
    snapping = false;
    return true;
}
