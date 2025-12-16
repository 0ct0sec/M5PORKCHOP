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
    detailViewActive = false;
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
    bool anyPressed = M5Cardputer.Keyboard.isPressed();
    
    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }
    
    if (keyWasPressed) return;
    keyWasPressed = true;
    
    auto keys = M5Cardputer.Keyboard.keysState();
    
    // Handle detail view input - U uploads, any other key closes
    if (detailViewActive) {
        if (M5Cardputer.Keyboard.isKeyPressed('u') || M5Cardputer.Keyboard.isKeyPressed('U')) {
            detailViewActive = false;
            if (!files.empty() && selectedIndex < files.size()) {
                uploadSelected();
            }
            return;
        }
        detailViewActive = false;
        return;
    }
    
    // Handle connecting/uploading states - ignore input
    if (connectingWiFi || uploadingFile) {
        return;
    }
    
    // Backtick or Backspace - exit menu
    if (M5Cardputer.Keyboard.isKeyPressed('`') || M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
        hide();
        return;
    }
    
    // Navigation with ; (prev) and . (next)
    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        if (selectedIndex > 0) {
            selectedIndex--;
            if (selectedIndex < scrollOffset) {
                scrollOffset = selectedIndex;
            }
        }
    }
    
    if (M5Cardputer.Keyboard.isKeyPressed('.')) {
        if (!files.empty() && selectedIndex < files.size() - 1) {
            selectedIndex++;
            if (selectedIndex >= scrollOffset + VISIBLE_ITEMS) {
                scrollOffset = selectedIndex - VISIBLE_ITEMS + 1;
            }
        }
    }
    
    // Enter - show detail view
    if (keys.enter && !files.empty()) {
        detailViewActive = true;
    }
    
    // U key - upload selected file
    if ((M5Cardputer.Keyboard.isKeyPressed('u') || M5Cardputer.Keyboard.isKeyPressed('U')) && !files.empty()) {
        uploadSelected();
    }
    
    // R key - refresh list
    if (M5Cardputer.Keyboard.isKeyPressed('r') || M5Cardputer.Keyboard.isKeyPressed('R')) {
        scanFiles();
        Display::showToast("Refreshed");
        delay(500);
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
    if (!active) return;
    handleInput();
}

void WigleMenu::draw(M5Canvas& canvas) {
    if (!active) return;
    
    canvas.fillSprite(COLOR_BG);
    canvas.setTextColor(COLOR_FG);
    canvas.setTextSize(1);
    
    // Empty state
    if (files.empty()) {
        canvas.setCursor(4, 35);
        canvas.print("No WiGLE files found");
        canvas.setCursor(4, 50);
        canvas.print("Go wardriving first!");
        canvas.setCursor(4, 65);
        canvas.print("[W] for WARHOG mode.");
        return;
    }
    
    // File list (always drawn, modals overlay on top)
    int y = 2;
    int lineHeight = 18;
    
    for (uint8_t i = scrollOffset; i < files.size() && i < scrollOffset + VISIBLE_ITEMS; i++) {
        const WigleFileInfo& file = files[i];
        
        // Highlight selected
        if (i == selectedIndex) {
            canvas.fillRect(0, y - 1, canvas.width(), lineHeight, COLOR_FG);
            canvas.setTextColor(COLOR_BG);
        } else {
            canvas.setTextColor(COLOR_FG);
        }
        
        // Status indicator
        canvas.setCursor(4, y);
        if (file.status == WigleFileStatus::UPLOADED) {
            canvas.print("[OK]");
        } else {
            canvas.print("[--]");
        }
        
        // Filename (truncated) - extract just the date/time part
        String displayName = file.filename;
        // Remove "warhog_" prefix and ".wigle.csv" suffix for cleaner display
        if (displayName.startsWith("warhog_")) {
            displayName = displayName.substring(7);
        }
        if (displayName.endsWith(".wigle.csv")) {
            displayName = displayName.substring(0, displayName.length() - 10);
        }
        if (displayName.length() > 15) {
            displayName = displayName.substring(0, 13) + "..";
        }
        canvas.setCursor(35, y);
        canvas.print(displayName);
        
        // Network count and size
        canvas.setCursor(140, y);
        canvas.printf("~%d %s", file.networkCount, formatSize(file.fileSize).c_str());
        
        y += lineHeight;
    }
    
    // Scroll indicators
    if (scrollOffset > 0) {
        canvas.setCursor(canvas.width() - 10, 2);
        canvas.setTextColor(COLOR_FG);
        canvas.print("^");
    }
    if (scrollOffset + VISIBLE_ITEMS < files.size()) {
        canvas.setCursor(canvas.width() - 10, 2 + (VISIBLE_ITEMS - 1) * lineHeight);
        canvas.setTextColor(COLOR_FG);
        canvas.print("v");
    }
    
    // Draw modals on top of list (matching captures_menu pattern)
    if (detailViewActive) {
        drawDetailView(canvas);
    }
    
    if (connectingWiFi || uploadingFile) {
        drawConnecting(canvas);
    }
}

void WigleMenu::drawDetailView(M5Canvas& canvas) {
    const WigleFileInfo& file = files[selectedIndex];
    
    // Modal box dimensions - matches other confirmation dialogs
    const int boxW = 200;
    const int boxH = 75;
    const int boxX = (canvas.width() - boxW) / 2;
    const int boxY = (canvas.height() - boxH) / 2 - 5;
    
    // Black border then pink fill
    canvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);
    
    // Black text on pink
    canvas.setTextColor(COLOR_BG, COLOR_FG);
    canvas.setTextDatum(top_center);
    
    // Filename
    String displayName = file.filename;
    if (displayName.length() > 22) {
        displayName = displayName.substring(0, 19) + "...";
    }
    canvas.drawString(displayName, boxX + boxW / 2, boxY + 8);
    
    // Stats
    String stats = "~" + String(file.networkCount) + " networks, " + formatSize(file.fileSize);
    canvas.drawString(stats, boxX + boxW / 2, boxY + 24);
    
    // Status
    const char* statusText = (file.status == WigleFileStatus::UPLOADED) ? "UPLOADED" : "NOT UPLOADED";
    canvas.drawString(statusText, boxX + boxW / 2, boxY + 40);
    
    // Action hint
    canvas.drawString("[U]pload  [Any]Close", boxX + boxW / 2, boxY + 56);
    
    canvas.setTextDatum(top_left);
}

void WigleMenu::drawConnecting(M5Canvas& canvas) {
    const int boxW = 160;
    const int boxH = 50;
    const int boxX = (canvas.width() - boxW) / 2;
    const int boxY = (canvas.height() - boxH) / 2 - 5;
    
    canvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);
    
    canvas.setTextColor(COLOR_BG, COLOR_FG);
    canvas.setTextDatum(top_center);
    
    if (connectingWiFi) {
        canvas.drawString("Connecting...", boxX + boxW / 2, boxY + 12);
    } else if (uploadingFile) {
        canvas.drawString("Uploading...", boxX + boxW / 2, boxY + 12);
    }
    
    canvas.drawString(WiGLE::getStatus(), boxX + boxW / 2, boxY + 30);
    
    canvas.setTextDatum(top_left);
}
