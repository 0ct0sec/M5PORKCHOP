// WiGLE Menu - View and upload wardriving files to wigle.net

#include "wigle_menu.h"
#include <M5Cardputer.h>
#include <SD.h>
#include "display.h"
#include "../web/wigle.h"
#include "../core/config.h"

// Static member initialization
std::vector<WigleFileInfo> WigleMenu::files;
uint8_t WigleMenu::selectedIndex = 0;
uint8_t WigleMenu::scrollOffset = 0;
bool WigleMenu::active = false;
bool WigleMenu::keyWasPressed = false;
bool WigleMenu::detailViewActive = false;
bool WigleMenu::connectingWiFi = false;
bool WigleMenu::uploadingFile = false;

void WigleMenu::init() {
    files.clear();
    selectedIndex = 0;
    scrollOffset = 0;
}

void WigleMenu::show() {
    active = true;
    selectedIndex = 0;
    scrollOffset = 0;
    detailViewActive = false;
    connectingWiFi = false;
    uploadingFile = false;
    keyWasPressed = true;  // Ignore enter that brought us here
    scanFiles();
}

void WigleMenu::hide() {
    active = false;
}

void WigleMenu::scanFiles() {
    files.clear();
    
    if (!Config::isSDAvailable()) {
        Serial.println("[WIGLE_MENU] SD card not available");
        return;
    }
    
    // Scan /wardriving/ directory for .wigle.csv files
    File dir = SD.open("/wardriving");
    if (!dir || !dir.isDirectory()) {
        Serial.println("[WIGLE_MENU] /wardriving directory not found");
        return;
    }
    
    while (File entry = dir.openNextFile()) {
        if (!entry.isDirectory()) {
            String name = entry.name();
            // Only show WiGLE format files (*.wigle.csv)
            if (name.endsWith(".wigle.csv")) {
                WigleFileInfo info;
                info.filename = name;
                info.fullPath = String("/wardriving/") + name;
                info.fileSize = entry.size();
                // Estimate network count: ~150 bytes per line after header
                info.networkCount = info.fileSize > 300 ? (info.fileSize - 300) / 150 : 0;
                
                // Check upload status
                info.status = WiGLE::isUploaded(info.fullPath.c_str()) ? 
                    WigleFileStatus::UPLOADED : WigleFileStatus::LOCAL;
                
                files.push_back(info);
            }
        }
        entry.close();
    }
    dir.close();
    
    // Sort by filename (newest first - filenames include timestamp)
    std::sort(files.begin(), files.end(), [](const WigleFileInfo& a, const WigleFileInfo& b) {
        return a.filename > b.filename;
    });
    
    Serial.printf("[WIGLE_MENU] Found %d WiGLE files\n", files.size());
}

void WigleMenu::handleInput() {
    M5Cardputer.update();
    
    if (!M5Cardputer.Keyboard.isChange()) {
        keyWasPressed = false;
        return;
    }
    
    if (!M5Cardputer.Keyboard.isPressed()) {
        keyWasPressed = false;
        return;
    }
    
    if (keyWasPressed) return;
    keyWasPressed = true;
    
    // Handle detail view input
    if (detailViewActive) {
        // Any key closes detail view
        detailViewActive = false;
        return;
    }
    
    // Handle connecting/uploading states - ignore input
    if (connectingWiFi || uploadingFile) {
        return;
    }
    
    Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
    
    // Backtick or ESC - exit menu
    if (keys.word[0] == '`' || keys.fn) {
        hide();
        return;
    }
    
    // Backspace - exit
    if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
        hide();
        return;
    }
    
    // Navigation
    if (keys.word[0] == ';' || keys.word[0] == ',') {
        // Previous
        if (selectedIndex > 0) {
            selectedIndex--;
            if (selectedIndex < scrollOffset) {
                scrollOffset = selectedIndex;
            }
        }
    } else if (keys.word[0] == '.' || keys.word[0] == '/') {
        // Next
        if (selectedIndex < files.size() - 1) {
            selectedIndex++;
            if (selectedIndex >= scrollOffset + VISIBLE_ITEMS) {
                scrollOffset = selectedIndex - VISIBLE_ITEMS + 1;
            }
        }
    }
    
    // Enter - show detail / upload options
    if (keys.enter && !files.empty()) {
        detailViewActive = true;
    }
    
    // U key - upload selected file
    if ((keys.word[0] == 'u' || keys.word[0] == 'U') && !files.empty()) {
        uploadSelected();
    }
    
    // R key - refresh list
    if (keys.word[0] == 'r' || keys.word[0] == 'R') {
        scanFiles();
        Display::showToast("Refreshed");
        delay(300);
    }
}

void WigleMenu::uploadSelected() {
    if (files.empty() || selectedIndex >= files.size()) return;
    
    WigleFileInfo& file = files[selectedIndex];
    
    // Check if already uploaded
    if (file.status == WigleFileStatus::UPLOADED) {
        Display::showToast("Already uploaded");
        delay(500);
        return;
    }
    
    // Check for credentials
    if (!WiGLE::hasCredentials()) {
        Display::showToast("No WiGLE API key");
        delay(500);
        return;
    }
    
    // Track if we initiated WiFi connection
    bool weConnected = false;
    
    // Connect to WiFi if needed
    connectingWiFi = true;
    if (!WiGLE::isConnected()) {
        Display::showToast("Connecting...");
        if (!WiGLE::connect()) {
            connectingWiFi = false;
            Display::showToast(WiGLE::getLastError());
            delay(500);
            return;
        }
        weConnected = true;
    }
    connectingWiFi = false;
    
    // Upload the file
    uploadingFile = true;
    Display::showToast("Uploading...");
    
    bool success = WiGLE::uploadFile(file.fullPath.c_str());
    uploadingFile = false;
    
    if (success) {
        file.status = WigleFileStatus::UPLOADED;
        Display::showToast("Upload OK!");
    } else {
        Display::showToast(WiGLE::getLastError());
    }
    delay(500);
    
    // Disconnect if we connected
    if (weConnected) {
        WiGLE::disconnect();
    }
}

String WigleMenu::formatSize(uint32_t bytes) {
    if (bytes < 1024) {
        return String(bytes) + "B";
    } else if (bytes < 1024 * 1024) {
        return String(bytes / 1024) + "KB";
    } else {
        return String(bytes / (1024 * 1024)) + "MB";
    }
}

void WigleMenu::update() {
    handleInput();
}

void WigleMenu::draw(M5Canvas& canvas) {
    canvas.fillScreen(COLOR_BG);
    
    // Title bar
    canvas.setTextSize(1);
    canvas.setTextDatum(top_center);
    canvas.setTextColor(COLOR_BG, COLOR_FG);
    canvas.fillRect(0, 0, 240, 12);
    canvas.drawString("PORK TRACKS", 120, 2);
    
    // Show detail overlay if active
    if (detailViewActive && !files.empty()) {
        drawDetailView(canvas);
        return;
    }
    
    // Show connecting overlay
    if (connectingWiFi || uploadingFile) {
        drawConnecting(canvas);
        return;
    }
    
    // Empty state
    if (files.empty()) {
        canvas.setTextColor(COLOR_FG, COLOR_BG);
        canvas.setTextDatum(middle_center);
        canvas.drawString("No WiGLE files found", 120, 60);
        canvas.setTextSize(1);
        canvas.drawString("Go wardriving first!", 120, 80);
        
        // Bottom bar with controls
        canvas.fillRect(0, 121, 240, 14);
        canvas.setTextColor(COLOR_BG, COLOR_FG);
        canvas.setTextDatum(middle_center);
        canvas.drawString("[`] Exit", 120, 128);
        return;
    }
    
    // File list
    canvas.setTextColor(COLOR_FG, COLOR_BG);
    canvas.setTextDatum(top_left);
    
    int y = 16;
    for (uint8_t i = 0; i < VISIBLE_ITEMS && (scrollOffset + i) < files.size(); i++) {
        uint8_t idx = scrollOffset + i;
        const WigleFileInfo& file = files[idx];
        
        // Selection highlight
        if (idx == selectedIndex) {
            canvas.fillRect(0, y, 240, 20);
            canvas.setTextColor(COLOR_BG, COLOR_FG);
        } else {
            canvas.setTextColor(COLOR_FG, COLOR_BG);
        }
        
        // Status indicator
        const char* statusStr;
        switch (file.status) {
            case WigleFileStatus::UPLOADED: statusStr = "[OK]"; break;
            default: statusStr = "[--]"; break;
        }
        canvas.drawString(statusStr, 2, y + 2);
        
        // Filename (truncated) - extract just the date/time part
        String displayName = file.filename;
        // Remove "warhog_" prefix and ".wigle.csv" suffix for cleaner display
        if (displayName.startsWith("warhog_")) {
            displayName = displayName.substring(7);
        }
        if (displayName.endsWith(".wigle.csv")) {
            displayName = displayName.substring(0, displayName.length() - 10);
        }
        canvas.drawString(displayName.substring(0, 15), 34, y + 2);
        
        // Network count and size
        String stats = "~" + String(file.networkCount) + " " + formatSize(file.fileSize);
        canvas.setTextDatum(top_right);
        canvas.drawString(stats, 238, y + 2);
        canvas.setTextDatum(top_left);
        
        y += 20;
    }
    
    // Scroll indicator
    if (files.size() > VISIBLE_ITEMS) {
        canvas.setTextColor(COLOR_FG, COLOR_BG);
        canvas.setTextDatum(top_right);
        canvas.drawString(String(selectedIndex + 1) + "/" + String(files.size()), 238, 118);
    }
    
    // Bottom bar with controls
    canvas.fillRect(0, 121, 240, 14);
    canvas.setTextColor(COLOR_BG, COLOR_FG);
    canvas.setTextDatum(middle_center);
    canvas.drawString("[U]pload [R]efresh [`]Exit", 120, 128);
}

void WigleMenu::drawDetailView(M5Canvas& canvas) {
    const WigleFileInfo& file = files[selectedIndex];
    
    // Semi-transparent overlay
    int boxW = 200;
    int boxH = 80;
    int boxX = (240 - boxW) / 2;
    int boxY = (135 - boxH) / 2;
    
    // Black border then pink fill
    canvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);
    
    canvas.setTextColor(COLOR_BG, COLOR_FG);
    canvas.setTextDatum(top_center);
    
    // Filename
    String displayName = file.filename;
    if (displayName.length() > 24) {
        displayName = displayName.substring(0, 21) + "...";
    }
    canvas.drawString(displayName, 120, boxY + 8);
    
    // Stats
    canvas.drawString("~" + String(file.networkCount) + " networks", 120, boxY + 24);
    canvas.drawString(formatSize(file.fileSize), 120, boxY + 38);
    
    // Status
    const char* statusText;
    switch (file.status) {
        case WigleFileStatus::UPLOADED: statusText = "UPLOADED"; break;
        default: statusText = "NOT UPLOADED"; break;
    }
    canvas.drawString(statusText, 120, boxY + 52);
    
    // Action hint
    canvas.drawString("[U] Upload  [Any] Close", 120, boxY + 66);
}

void WigleMenu::drawConnecting(M5Canvas& canvas) {
    int boxW = 160;
    int boxH = 50;
    int boxX = (240 - boxW) / 2;
    int boxY = (135 - boxH) / 2;
    
    canvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);
    
    canvas.setTextColor(COLOR_BG, COLOR_FG);
    canvas.setTextDatum(middle_center);
    
    if (connectingWiFi) {
        canvas.drawString("Connecting...", 120, boxY + 18);
    } else if (uploadingFile) {
        canvas.drawString("Uploading...", 120, boxY + 18);
    }
    
    canvas.drawString(WiGLE::getStatus(), 120, boxY + 34);
}
