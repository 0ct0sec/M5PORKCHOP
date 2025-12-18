// BOAR BROS Menu - Manage excluded networks

#include "boar_bros_menu.h"
#include <M5Cardputer.h>
#include <SD.h>
#include "display.h"
#include "../modes/oink.h"

// Static member initialization
std::vector<BroInfo> BoarBrosMenu::bros;
uint8_t BoarBrosMenu::selectedIndex = 0;
uint8_t BoarBrosMenu::scrollOffset = 0;
bool BoarBrosMenu::active = false;
bool BoarBrosMenu::keyWasPressed = false;
bool BoarBrosMenu::deleteConfirmActive = false;

static const char* BOAR_BROS_FILE = "/boar_bros.txt";

void BoarBrosMenu::init() {
    bros.clear();
    selectedIndex = 0;
    scrollOffset = 0;
}

void BoarBrosMenu::show() {
    active = true;
    selectedIndex = 0;
    scrollOffset = 0;
    keyWasPressed = true;  // Ignore the Enter that selected us from menu
    deleteConfirmActive = false;
    loadBros();
}

void BoarBrosMenu::hide() {
    active = false;
    deleteConfirmActive = false;
}

void BoarBrosMenu::loadBros() {
    bros.clear();
    
    if (!SD.exists(BOAR_BROS_FILE)) {
        Serial.println("[BOAR_BROS] No file found");
        return;
    }
    
    File f = SD.open(BOAR_BROS_FILE, FILE_READ);
    if (!f) {
        Serial.println("[BOAR_BROS] Failed to open file");
        return;
    }
    
    // Cap at 50 entries (same as MAX_BOAR_BROS in oink.cpp)
    while (f.available() && bros.size() < 50) {
        String line = f.readStringUntil('\n');
        line.trim();
        
        // Skip empty lines and comments
        if (line.length() == 0 || line.startsWith("#")) continue;
        
        // Format: AABBCCDDEEFF [SSID]
        if (line.length() >= 12) {
            String hexBssid = line.substring(0, 12);
            hexBssid.toUpperCase();
            
            uint64_t bssid = 0;
            bool valid = true;
            for (int i = 0; i < 12; i++) {
                char c = hexBssid.charAt(i);
                uint8_t nibble;
                if (c >= '0' && c <= '9') nibble = c - '0';
                else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
                else { valid = false; break; }
                bssid = (bssid << 4) | nibble;
            }
            
            if (valid) {
                BroInfo info;
                info.bssid = bssid;
                info.bssidStr = formatBSSID(bssid);
                
                // Extract SSID from rest of line (after space)
                if (line.length() > 13) {
                    info.ssid = line.substring(13);
                    info.ssid.trim();
                } else {
                    info.ssid = "";
                }
                
                bros.push_back(info);
            }
        }
    }
    
    f.close();
    Serial.printf("[BOAR_BROS] Loaded %d bros\n", (int)bros.size());
}

String BoarBrosMenu::formatBSSID(uint64_t bssid) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             (uint8_t)((bssid >> 40) & 0xFF),
             (uint8_t)((bssid >> 32) & 0xFF),
             (uint8_t)((bssid >> 24) & 0xFF),
             (uint8_t)((bssid >> 16) & 0xFF),
             (uint8_t)((bssid >> 8) & 0xFF),
             (uint8_t)(bssid & 0xFF));
    return String(buf);
}

size_t BoarBrosMenu::getCount() {
    return OinkMode::getExcludedCount();
}

String BoarBrosMenu::getSelectedInfo() {
    if (bros.empty()) return "[B] Add from OINK mode";
    if (selectedIndex < bros.size()) {
        return bros[selectedIndex].bssidStr;
    }
    return "";
}

void BoarBrosMenu::update() {
    if (!active) return;
    handleInput();
}

void BoarBrosMenu::handleInput() {
    bool anyPressed = M5Cardputer.Keyboard.isPressed();
    
    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }
    
    if (keyWasPressed) return;
    keyWasPressed = true;
    
    auto keys = M5Cardputer.Keyboard.keysState();
    
    // Handle delete confirmation modal
    if (deleteConfirmActive) {
        if (M5Cardputer.Keyboard.isKeyPressed('y') || M5Cardputer.Keyboard.isKeyPressed('Y')) {
            deleteSelected();
            deleteConfirmActive = false;
        } else if (M5Cardputer.Keyboard.isKeyPressed('n') || M5Cardputer.Keyboard.isKeyPressed('N') ||
                   M5Cardputer.Keyboard.isKeyPressed('`') || keys.enter) {
            deleteConfirmActive = false;  // Cancel
        }
        return;
    }
    
    // Navigation with ; (prev/up) and . (next/down)
    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        if (selectedIndex > 0) {
            selectedIndex--;
            if (selectedIndex < scrollOffset) {
                scrollOffset = selectedIndex;
            }
        }
    }
    
    if (M5Cardputer.Keyboard.isKeyPressed('.')) {
        if (!bros.empty() && selectedIndex < bros.size() - 1) {
            selectedIndex++;
            if (selectedIndex >= scrollOffset + VISIBLE_ITEMS) {
                scrollOffset = selectedIndex - VISIBLE_ITEMS + 1;
            }
        }
    }
    
    // D key - delete selected
    if ((M5Cardputer.Keyboard.isKeyPressed('d') || M5Cardputer.Keyboard.isKeyPressed('D')) && !bros.empty()) {
        deleteConfirmActive = true;
    }
    
    // Backtick or Backspace - exit
    if (M5Cardputer.Keyboard.isKeyPressed('`') || M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
        hide();
        // Return to menu handled by porkchop.cpp
    }
}

void BoarBrosMenu::deleteSelected() {
    if (selectedIndex >= bros.size()) return;
    
    uint64_t targetBssid = bros[selectedIndex].bssid;
    
    // Remove from OinkMode's set and save
    OinkMode::removeBoarBro(targetBssid);
    
    // Refresh our list
    loadBros();
    
    // Adjust selection if needed
    if (selectedIndex >= bros.size() && selectedIndex > 0) {
        selectedIndex--;
    }
    if (scrollOffset > 0 && scrollOffset >= bros.size()) {
        scrollOffset = bros.size() > 0 ? bros.size() - 1 : 0;
    }
    
    Display::showToast("Bro removed!");
    delay(500);
}

void BoarBrosMenu::draw(M5Canvas& canvas) {
    if (!active) return;
    
    canvas.fillSprite(COLOR_BG);
    canvas.setTextColor(COLOR_FG);
    canvas.setTextSize(1);
    
    if (bros.empty()) {
        canvas.setCursor(4, 35);
        canvas.print("No BOAR BROS yet!");
        canvas.setCursor(4, 50);
        canvas.print("Press [B] in OINK mode");
        canvas.setCursor(4, 65);
        canvas.print("to exclude a network.");
        return;
    }
    
    // Draw bros list
    int y = 2;
    int lineHeight = 18;
    
    for (uint8_t i = scrollOffset; i < bros.size() && i < scrollOffset + VISIBLE_ITEMS; i++) {
        const BroInfo& bro = bros[i];
        
        // Highlight selected
        if (i == selectedIndex) {
            canvas.fillRect(0, y - 1, canvas.width(), lineHeight, COLOR_FG);
            canvas.setTextColor(COLOR_BG);
        } else {
            canvas.setTextColor(COLOR_FG);
        }
        
        // SSID or "NONAME BRO" for hidden networks
        canvas.setCursor(4, y);
        String displayName = bro.ssid.length() > 0 ? bro.ssid : "NONAME BRO";
        displayName.toUpperCase();
        if (displayName.length() > 14) {
            displayName = displayName.substring(0, 12) + "..";
// 
        }
        canvas.print(displayName);
        
        // Full BSSID (fits at x=80, 17 chars * 6px = 102px, ends at 182px)
        canvas.setCursor(80, y);
        canvas.print(bro.bssidStr);
        
        y += lineHeight;
    }
    
    // Scroll indicators
    if (scrollOffset > 0) {
        canvas.setCursor(canvas.width() - 10, 2);
        canvas.setTextColor(COLOR_FG);
        canvas.print("^");
    }
    if (scrollOffset + VISIBLE_ITEMS < bros.size()) {
        canvas.setCursor(canvas.width() - 10, 2 + (VISIBLE_ITEMS - 1) * lineHeight);
        canvas.setTextColor(COLOR_FG);
        canvas.print("v");
    }
    
    // Draw delete confirmation modal if active
    if (deleteConfirmActive) {
        drawDeleteConfirm(canvas);
    }
}

void BoarBrosMenu::drawDeleteConfirm(M5Canvas& canvas) {
    // Modal box dimensions - matches other confirmation dialogs
    const int boxW = 180;
    const int boxH = 55;
    const int boxX = (canvas.width() - boxW) / 2;
    const int boxY = (canvas.height() - boxH) / 2 - 5;
    
    // Black border then pink fill
    canvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);
    
    // Black text on pink
    canvas.setTextColor(COLOR_BG, COLOR_FG);
    canvas.setTextDatum(top_center);
    
    canvas.drawString("Remove this bro?", boxX + boxW / 2, boxY + 10);
    
    String broName = bros[selectedIndex].ssid.length() > 0 ? 
                     bros[selectedIndex].ssid : bros[selectedIndex].bssidStr;
    broName.toUpperCase();
    if (broName.length() > 18) broName = broName.substring(0, 16) + "..";
    canvas.drawString(broName, boxX + boxW / 2, boxY + 24);
    
    canvas.drawString("[Y]es  [N]o", boxX + boxW / 2, boxY + 40);
    
    canvas.setTextDatum(top_left);
}
