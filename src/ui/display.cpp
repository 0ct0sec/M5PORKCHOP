// Display management implementation

#include "display.h"
#include <M5Cardputer.h>
#include "../core/porkchop.h"
#include "../core/config.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include "../modes/oink.h"
#include "../modes/warhog.h"
#include "../gps/gps.h"
#include "menu.h"
#include "settings_menu.h"
#include "captures_menu.h"

// Static member initialization
M5Canvas Display::topBar(&M5.Display);
M5Canvas Display::mainCanvas(&M5.Display);
M5Canvas Display::bottomBar(&M5.Display);
bool Display::gpsStatus = false;
bool Display::wifiStatus = false;
bool Display::mlStatus = false;

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
    
    Serial.println("[DISPLAY] Initialized");
}

void Display::update() {
    drawTopBar();
    
    // Draw main content based on mode
    mainCanvas.fillSprite(COLOR_BG);
    mainCanvas.setTextColor(COLOR_FG);
    
    PorkchopMode mode = porkchop.getMode();
    
    switch (mode) {
        case PorkchopMode::IDLE:
            // Draw piglet avatar and mood
            Avatar::draw(mainCanvas);
            Mood::draw(mainCanvas);
            break;
            
        case PorkchopMode::OINK_MODE:
        case PorkchopMode::WARHOG_MODE:
            // Draw piglet avatar and mood bubble (info embedded in bubble)
            Avatar::draw(mainCanvas);
            Mood::draw(mainCanvas);
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
            
        case PorkchopMode::ABOUT:
            drawAboutScreen(mainCanvas);
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
        case PorkchopMode::MENU:
            modeStr = "MENU";
            break;
        case PorkchopMode::SETTINGS:
            modeStr = "CONFIG";
            break;
        case PorkchopMode::ABOUT:
            modeStr = "ABOUT";
            break;
    }
    
    topBar.setTextColor(modeColor);
    topBar.setTextDatum(top_left);
    topBar.drawString(modeStr, 2, 2);
    
    // Center: clock (from GPS or --:--)
    topBar.setTextDatum(top_center);
    topBar.setTextColor(COLOR_FG);
    String timeStr = GPS::hasFix() ? GPS::getTimeString() : "--:--";
    topBar.drawString(timeStr, DISPLAY_W / 2, 2);
    
    // Right side: battery + status icons
    topBar.setTextDatum(top_right);
    
    // Battery percentage
    int battLevel = M5.Power.getBatteryLevel();
    String battStr = String(battLevel) + "%";
    
    // Status icons
    String status = "";
    status += gpsStatus ? "G" : "-";
    status += wifiStatus ? "W" : "-";
    status += mlStatus ? "M" : "-";
    
    // Draw battery then status
    String rightStr = battStr + " " + status;
    topBar.drawString(rightStr, DISPLAY_W - 2, 2);
}

void Display::drawBottomBar() {
    bottomBar.fillSprite(COLOR_BG);
    bottomBar.setTextColor(COLOR_ACCENT);  // Use accent color for stats
    bottomBar.setTextSize(1);
    bottomBar.setTextDatum(top_left);
    
    PorkchopMode mode = porkchop.getMode();
    String stats;
    
    if (mode == PorkchopMode::WARHOG_MODE) {
        // WARHOG: show unique networks, saved records, and GPS coords
        uint32_t unique = WarhogMode::getTotalNetworks();
        uint32_t saved = WarhogMode::getSavedCount();
        GPSData gps = GPS::getData();
        
        if (GPS::hasFix()) {
            // Show coords with satellite count: "U:5 S:3 [42.36,-71.05]"
            char buf[64];
            snprintf(buf, sizeof(buf), "U:%lu S:%lu [%.2f,%.2f] S:%d", 
                     unique, saved, gps.latitude, gps.longitude, gps.satellites);
            stats = String(buf);
        } else {
            // No fix - show satellite count searching
            stats = "U:" + String(unique) + " S:" + String(saved) + " GPS:" + String(gps.satellites) + "sat";
        }
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
            canvas.drawString("Scanning...", 2, 2);
            canvas.setTextColor(COLOR_ACCENT);
            char buf[32];
            snprintf(buf, sizeof(buf), "Found %d networks", (int)networks.size());
            canvas.drawString(buf, 2, 14);
        } else {
            canvas.drawString("Scanning for networks...", 2, MAIN_H / 2 - 5);
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

void Display::drawAboutScreen(M5Canvas& canvas) {
    canvas.setTextColor(COLOR_FG);
    canvas.setTextDatum(top_center);
    
    canvas.setTextSize(2);
    canvas.setTextColor(COLOR_ACCENT);
    canvas.drawString("M5PORKCHOP", DISPLAY_W / 2, 10);
    
    canvas.setTextSize(1);
    canvas.setTextColor(COLOR_FG);
    canvas.drawString("by 0ct0", DISPLAY_W / 2, 35);
    
    canvas.setTextColor(COLOR_SUCCESS);
    canvas.drawString("github.com/neledov", DISPLAY_W / 2, 52);
    canvas.drawString("/M5Porkchop", DISPLAY_W / 2, 64);
    
    canvas.setTextColor(COLOR_ACCENT);
    canvas.drawString("[Enter] to go back", DISPLAY_W / 2, MAIN_H - 12);
}
