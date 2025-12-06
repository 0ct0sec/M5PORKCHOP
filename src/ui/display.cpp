// Display management implementation

#include "display.h"
#include <M5Cardputer.h>
#include "../core/porkchop.h"
#include "../core/config.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include "../modes/oink.h"
#include "menu.h"

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
            // Draw piglet avatar and info line
            Avatar::draw(mainCanvas);
            drawModeInfo(mainCanvas, mode);
            break;
            
        case PorkchopMode::MENU:
            // Draw menu
            Menu::update();
            Menu::draw(mainCanvas);
            break;
            
        case PorkchopMode::SETTINGS:
            drawSettingsScreen(mainCanvas);
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
    topBar.setTextDatum(top_left);
    
    // Left side: hostname and mode
    String hostname = Config::personality().name;
    topBar.drawString(hostname + ">", 2, 2);
    
    // Mode indicator
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
    topBar.setTextDatum(top_center);
    topBar.drawString(modeStr, DISPLAY_W / 2, 2);
    
    // Right side: status icons
    topBar.setTextDatum(top_right);
    topBar.setTextColor(COLOR_FG);
    
    String status = "";
    status += gpsStatus ? "G" : "-";
    status += wifiStatus ? "W" : "-";
    status += mlStatus ? "M" : "-";
    
    topBar.drawString(status, DISPLAY_W - 2, 2);
    
    // Draw separator line
    topBar.drawLine(0, TOP_BAR_H - 1, DISPLAY_W, TOP_BAR_H - 1, COLOR_ACCENT);
}

void Display::drawBottomBar() {
    bottomBar.fillSprite(COLOR_BG);
    bottomBar.setTextColor(COLOR_ACCENT);  // Use accent color for stats
    
    // Draw separator line
    bottomBar.drawLine(0, 0, DISPLAY_W, 0, COLOR_ACCENT);
    
    // Left: stats - Networks, Handshakes, Deauths
    uint16_t netCount = porkchop.getNetworkCount();
    uint16_t hsCount = porkchop.getHandshakeCount();
    uint16_t deauthCount = porkchop.getDeauthCount();
    
    bottomBar.setTextDatum(top_left);
    String stats = "N:" + String(netCount) + " HS:" + String(hsCount) + " D:" + String(deauthCount);
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
        while (true) {
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
    
    while (true) {
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
        // Show recent network info and handshake status
        const auto& networks = OinkMode::getNetworks();
        const auto& handshakes = OinkMode::getHandshakes();
        
        int y = MAIN_H - 35;
        
        if (!networks.empty()) {
            // Show most recent network - SSID only, no MAC
            const auto& net = networks.back();
            String ssid = String(net.ssid);
            if (ssid.length() == 0) ssid = "<hidden>";
            
            canvas.drawString(ssid.substring(0, 18), 2, y);
            canvas.drawString("CH:" + String(net.channel), DISPLAY_W - 45, y);
            y += 10;
            canvas.setTextColor(COLOR_ACCENT);
            canvas.drawString(String(net.rssi) + "dB  N:" + String(networks.size()), 2, y);
        } else {
            canvas.drawString("Scanning for networks...", 2, y);
        }
        
        // Show handshake count
        uint16_t hsCount = OinkMode::getCompleteHandshakeCount();
        if (hsCount > 0) {
            canvas.setTextColor(COLOR_SUCCESS);
            canvas.drawString("HS:" + String(hsCount), DISPLAY_W - 35, y);
        }
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
    canvas.setTextSize(1);
    
    canvas.drawString("=== ABOUT ===", DISPLAY_W / 2, 5);
    
    canvas.setTextSize(2);
    canvas.setTextColor(COLOR_ACCENT);
    canvas.drawString("M5PORKCHOP", DISPLAY_W / 2, 25);
    
    canvas.setTextSize(1);
    canvas.setTextColor(COLOR_FG);
    canvas.drawString("by 0ct0", DISPLAY_W / 2, 48);
    
    canvas.setTextColor(COLOR_SUCCESS);
    canvas.drawString("github.com/neledov", DISPLAY_W / 2, 65);
    canvas.drawString("/M5Porkchop", DISPLAY_W / 2, 77);
    
    canvas.setTextColor(COLOR_ACCENT);
    canvas.drawString("[Enter] to go back", DISPLAY_W / 2, MAIN_H - 12);
}
