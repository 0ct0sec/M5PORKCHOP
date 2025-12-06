// Captures Menu - View saved handshake captures

#include "captures_menu.h"
#include <M5Cardputer.h>
#include <SD.h>
#include <time.h>
#include "display.h"

// Static member initialization
std::vector<CaptureInfo> CapturesMenu::captures;
uint8_t CapturesMenu::selectedIndex = 0;
uint8_t CapturesMenu::scrollOffset = 0;
bool CapturesMenu::active = false;
bool CapturesMenu::keyWasPressed = false;

void CapturesMenu::init() {
    captures.clear();
    selectedIndex = 0;
    scrollOffset = 0;
}

void CapturesMenu::show() {
    active = true;
    selectedIndex = 0;
    scrollOffset = 0;
    scanCaptures();
}

void CapturesMenu::hide() {
    active = false;
}

void CapturesMenu::scanCaptures() {
    captures.clear();
    
    if (!SD.exists("/handshakes")) {
        Serial.println("[CAPTURES] No handshakes directory");
        return;
    }
    
    File dir = SD.open("/handshakes");
    if (!dir || !dir.isDirectory()) {
        Serial.println("[CAPTURES] Failed to open handshakes directory");
        return;
    }
    
    File file = dir.openNextFile();
    while (file) {
        String name = file.name();
        if (name.endsWith(".pcap")) {
            CaptureInfo info;
            info.filename = name;
            info.fileSize = file.size();
            info.captureTime = file.getLastWrite();
            
            // Extract BSSID from filename (e.g., "64EEB7208286.pcap")
            String baseName = name.substring(0, name.indexOf('.'));
            if (baseName.length() >= 12) {
                info.bssid = baseName.substring(0, 2) + ":" +
                             baseName.substring(2, 4) + ":" +
                             baseName.substring(4, 6) + ":" +
                             baseName.substring(6, 8) + ":" +
                             baseName.substring(8, 10) + ":" +
                             baseName.substring(10, 12);
            } else {
                info.bssid = baseName;
            }
            
            // Try to get SSID from companion .txt file if exists
            String txtPath = "/handshakes/" + baseName + ".txt";
            if (SD.exists(txtPath)) {
                File txtFile = SD.open(txtPath, FILE_READ);
                if (txtFile) {
                    info.ssid = txtFile.readStringUntil('\n');
                    info.ssid.trim();
                    txtFile.close();
                }
            }
            if (info.ssid.isEmpty()) {
                info.ssid = "[unknown]";
            }
            
            captures.push_back(info);
        }
        file = dir.openNextFile();
    }
    dir.close();
    
    // Sort by capture time (newest first)
    std::sort(captures.begin(), captures.end(), [](const CaptureInfo& a, const CaptureInfo& b) {
        return a.captureTime > b.captureTime;
    });
    
    Serial.printf("[CAPTURES] Found %d captures\n", captures.size());
}

void CapturesMenu::update() {
    if (!active) return;
    handleInput();
}

void CapturesMenu::handleInput() {
    bool anyPressed = M5Cardputer.Keyboard.isPressed();
    
    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }
    
    if (keyWasPressed) return;
    keyWasPressed = true;
    
    auto keys = M5Cardputer.Keyboard.keysState();
    
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
        if (selectedIndex < captures.size() - 1) {
            selectedIndex++;
            if (selectedIndex >= scrollOffset + VISIBLE_ITEMS) {
                scrollOffset = selectedIndex - VISIBLE_ITEMS + 1;
            }
        }
    }
    
    // Exit with backtick or Enter
    if (keys.enter || M5Cardputer.Keyboard.isKeyPressed('`')) {
        hide();
    }
}

String CapturesMenu::formatTime(time_t t) {
    if (t == 0) return "Unknown";
    
    struct tm* timeinfo = localtime(&t);
    if (!timeinfo) return "Unknown";
    
    char buf[32];
    // Format: "Dec 06 14:32"
    strftime(buf, sizeof(buf), "%b %d %H:%M", timeinfo);
    return String(buf);
}

void CapturesMenu::draw(M5Canvas& canvas) {
    if (!active) return;
    
    canvas.fillScreen(TFT_BLACK);
    
    // Title
    canvas.setTextColor(COLOR_FG);
    canvas.setTextSize(1);
    canvas.setCursor(4, 2);
    canvas.printf("CAPTURES (%d)", captures.size());
    
    // Divider line
    canvas.drawFastHLine(0, 12, canvas.width(), COLOR_FG);
    
    if (captures.empty()) {
        canvas.setCursor(4, 50);
        canvas.print("No captures found");
        canvas.setCursor(4, 65);
        canvas.print("Start OINK mode to hunt!");
        return;
    }
    
    // Draw captures list
    int y = 16;
    int lineHeight = 18;
    
    for (uint8_t i = scrollOffset; i < captures.size() && i < scrollOffset + VISIBLE_ITEMS; i++) {
        const CaptureInfo& cap = captures[i];
        
        // Highlight selected
        if (i == selectedIndex) {
            canvas.fillRect(0, y - 1, canvas.width(), lineHeight, COLOR_FG);
            canvas.setTextColor(TFT_BLACK);
        } else {
            canvas.setTextColor(COLOR_FG);
        }
        
        // SSID (truncated if needed)
        canvas.setCursor(4, y);
        String displaySSID = cap.ssid;
        if (displaySSID.length() > 14) {
            displaySSID = displaySSID.substring(0, 12) + "..";
        }
        canvas.print(displaySSID);
        
        // Date/time
        canvas.setCursor(95, y);
        canvas.print(formatTime(cap.captureTime));
        
        // File size (KB)
        canvas.setCursor(170, y);
        canvas.printf("%dK", cap.fileSize / 1024);
        
        y += lineHeight;
    }
    
    // Scroll indicators
    if (scrollOffset > 0) {
        canvas.setCursor(canvas.width() - 10, 16);
        canvas.setTextColor(COLOR_FG);
        canvas.print("^");
    }
    if (scrollOffset + VISIBLE_ITEMS < captures.size()) {
        canvas.setCursor(canvas.width() - 10, 16 + (VISIBLE_ITEMS - 1) * lineHeight);
        canvas.setTextColor(COLOR_FG);
        canvas.print("v");
    }
    
    // Footer with BSSID of selected
    if (selectedIndex < captures.size()) {
        canvas.drawFastHLine(0, canvas.height() - 14, canvas.width(), COLOR_FG);
        canvas.setTextColor(COLOR_FG);
        canvas.setCursor(4, canvas.height() - 11);
        canvas.print(captures[selectedIndex].bssid);
    }
}
