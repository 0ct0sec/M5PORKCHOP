// Achievements Menu - View unlocked achievements

#include "achievements_menu.h"
#include <M5Cardputer.h>
#include "display.h"
#include "../core/xp.h"

// Static member initialization
uint8_t AchievementsMenu::selectedIndex = 0;
uint8_t AchievementsMenu::scrollOffset = 0;
bool AchievementsMenu::active = false;
bool AchievementsMenu::keyWasPressed = false;
bool AchievementsMenu::showingDetail = false;

// Achievement info - order must match PorkAchievement enum bit positions
static const struct {
    PorkAchievement flag;
    const char* name;
    const char* howTo;
} ACHIEVEMENTS[] = {
    { ACH_FIRST_BLOOD,    "FIRST BLOOD",    "Capture your first handshake" },
    { ACH_CENTURION,      "CENTURION",      "Find 100 networks in one session" },
    { ACH_MARATHON_PIG,   "MARATHON PIG",   "Walk 10km in a single session" },
    { ACH_NIGHT_OWL,      "NIGHT OWL",      "Hunt after midnight" },
    { ACH_GHOST_HUNTER,   "GHOST HUNTER",   "Find 10 hidden networks" },
    { ACH_APPLE_FARMER,   "APPLE FARMER",   "Send 100 Apple BLE packets" },
    { ACH_WARDRIVER,      "WARDRIVER",      "Log 1000 networks lifetime" },
    { ACH_DEAUTH_KING,    "DEAUTH KING",    "Land 100 successful deauths" },
    { ACH_PMKID_HUNTER,   "PMKID HUNTER",   "Capture a PMKID" },
    { ACH_WPA3_SPOTTER,   "WPA3 SPOTTER",   "Find a WPA3 network" },
    { ACH_GPS_MASTER,     "GPS MASTER",     "Log 100 GPS-tagged networks" },
    { ACH_TOUCH_GRASS,    "TOUCH GRASS",    "Walk 50km total lifetime" },
    { ACH_SILICON_PSYCHO, "SILICON PSYCHO", "Log 5000 networks lifetime" },
    { ACH_CLUTCH_CAPTURE, "CLUTCH CAPTURE", "Handshake at <10% battery" },
    { ACH_SPEED_RUN,      "SPEED RUN",      "50 networks in 10 minutes" },
    { ACH_CHAOS_AGENT,    "CHAOS AGENT",    "Send 1000 BLE packets" },
};

void AchievementsMenu::init() {
    selectedIndex = 0;
    scrollOffset = 0;
    showingDetail = false;
}

void AchievementsMenu::show() {
    active = true;
    selectedIndex = 0;
    scrollOffset = 0;
    showingDetail = false;
    keyWasPressed = true;  // Ignore the Enter that selected us from menu
}

void AchievementsMenu::hide() {
    active = false;
    showingDetail = false;
}

void AchievementsMenu::update() {
    if (!active) return;
    handleInput();
}

void AchievementsMenu::handleInput() {
    bool anyPressed = M5Cardputer.Keyboard.isPressed();
    
    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }
    
    if (keyWasPressed) return;
    keyWasPressed = true;
    
    auto keys = M5Cardputer.Keyboard.keysState();
    
    // If showing detail, any key closes it
    if (showingDetail) {
        showingDetail = false;
        return;
    }
    
    // Navigation with ; (up) and . (down)
    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        if (selectedIndex > 0) {
            selectedIndex--;
            if (selectedIndex < scrollOffset) {
                scrollOffset = selectedIndex;
            }
        }
    }
    
    if (M5Cardputer.Keyboard.isKeyPressed('.')) {
        if (selectedIndex < TOTAL_ACHIEVEMENTS - 1) {
            selectedIndex++;
            if (selectedIndex >= scrollOffset + VISIBLE_ITEMS) {
                scrollOffset = selectedIndex - VISIBLE_ITEMS + 1;
            }
        }
    }
    
    // Enter shows detail for selected achievement
    if (keys.enter) {
        showingDetail = true;
        return;
    }
    
    // Exit with backtick
    if (M5Cardputer.Keyboard.isKeyPressed('`')) {
        hide();
    }
}

void AchievementsMenu::draw(M5Canvas& canvas) {
    if (!active) return;
    
    // If showing detail popup, draw that instead
    if (showingDetail) {
        drawDetail(canvas);
        return;
    }
    
    canvas.fillScreen(TFT_BLACK);
    
    // Count unlocked
    uint32_t unlocked = XP::getAchievements();
    int unlockedCount = 0;
    for (int i = 0; i < TOTAL_ACHIEVEMENTS; i++) {
        if (unlocked & ACHIEVEMENTS[i].flag) unlockedCount++;
    }
    
    // Title
    canvas.setTextColor(COLOR_FG);
    canvas.setTextSize(1);
    canvas.setCursor(4, 2);
    canvas.printf("ACHIEVEMENTS %d/%d", unlockedCount, TOTAL_ACHIEVEMENTS);
    
    // Divider line
    canvas.drawFastHLine(0, 12, canvas.width(), COLOR_FG);
    
    // Draw achievements list
    int y = 16;
    int lineHeight = 18;
    
    for (uint8_t i = scrollOffset; i < TOTAL_ACHIEVEMENTS && i < scrollOffset + VISIBLE_ITEMS; i++) {
        bool hasIt = (unlocked & ACHIEVEMENTS[i].flag) != 0;
        
        // Highlight selected
        if (i == selectedIndex) {
            canvas.fillRect(0, y - 1, canvas.width(), lineHeight, COLOR_FG);
            canvas.setTextColor(TFT_BLACK);
        } else {
            // Use dimmer pink (0x7A8A) for locked, full pink for unlocked
            canvas.setTextColor(hasIt ? COLOR_FG : 0x7A8A);
        }
        
        // Lock/unlock indicator
        canvas.setCursor(4, y);
        canvas.print(hasIt ? "[X]" : "[ ]");
        
        // Achievement name (show ??? if locked)
        canvas.setCursor(28, y);
        canvas.print(hasIt ? ACHIEVEMENTS[i].name : "???");
        
        y += lineHeight;
    }
    
    // Scroll indicators
    if (scrollOffset > 0) {
        canvas.setCursor(canvas.width() - 10, 16);
        canvas.setTextColor(COLOR_FG);
        canvas.print("^");
    }
    if (scrollOffset + VISIBLE_ITEMS < TOTAL_ACHIEVEMENTS) {
        canvas.setCursor(canvas.width() - 10, 16 + (VISIBLE_ITEMS - 1) * lineHeight);
        canvas.setTextColor(COLOR_FG);
        canvas.print("v");
    }
}

void AchievementsMenu::drawDetail(M5Canvas& canvas) {
    canvas.fillScreen(TFT_BLACK);
    
    // Draw border
    canvas.drawRect(10, 15, canvas.width() - 20, 75, COLOR_FG);
    
    bool hasIt = (XP::getAchievements() & ACHIEVEMENTS[selectedIndex].flag) != 0;
    
    // Achievement name centered at top (show UNKNOWN if locked)
    canvas.setTextColor(COLOR_FG);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_center);
    canvas.drawString(hasIt ? ACHIEVEMENTS[selectedIndex].name : "UNKNOWN", canvas.width() / 2, 22);
    
    // Status (use pig pink for unlocked, grey for locked)
    canvas.setTextDatum(top_center);
    canvas.setTextColor(hasIt ? COLOR_FG : TFT_DARKGREY);
    canvas.drawString(hasIt ? "UNLOCKED" : "LOCKED", canvas.width() / 2, 36);
    
    // How to get it (show ??? if locked)
    canvas.setTextColor(COLOR_FG);
    canvas.setTextDatum(top_center);
    
    // Word wrap the howTo text (max ~30 chars per line)
    String howTo = hasIt ? ACHIEVEMENTS[selectedIndex].howTo : "???";
    int y = 52;
    while (howTo.length() > 0) {
        String line;
        if (howTo.length() <= 30) {
            line = howTo;
            howTo = "";
        } else {
            // Find last space before char 30
            int lastSpace = howTo.lastIndexOf(' ', 30);
            if (lastSpace > 0) {
                line = howTo.substring(0, lastSpace);
                howTo = howTo.substring(lastSpace + 1);
            } else {
                line = howTo.substring(0, 30);
                howTo = howTo.substring(30);
            }
        }
        canvas.drawString(line, canvas.width() / 2, y);
        y += 12;
    }
    
    // Reset text datum
    canvas.setTextDatum(top_left);
}
