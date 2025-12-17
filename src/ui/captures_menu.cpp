// Captures Menu - View saved handshake captures

#include "captures_menu.h"
#include <M5Cardputer.h>
#include <SD.h>
#include <time.h>
#include "display.h"
#include "../web/wpasec.h"
#include "../core/config.h"

// Static member initialization
std::vector<CaptureInfo> CapturesMenu::captures;
uint8_t CapturesMenu::selectedIndex = 0;
uint8_t CapturesMenu::scrollOffset = 0;
bool CapturesMenu::active = false;
bool CapturesMenu::keyWasPressed = false;
bool CapturesMenu::nukeConfirmActive = false;
bool CapturesMenu::detailViewActive = false;
bool CapturesMenu::connectingWiFi = false;
bool CapturesMenu::uploadingFile = false;
bool CapturesMenu::refreshingResults = false;

void CapturesMenu::init() {
    captures.clear();
    selectedIndex = 0;
    scrollOffset = 0;
}

void CapturesMenu::show() {
    active = true;
    selectedIndex = 0;
    scrollOffset = 0;
    keyWasPressed = true;  // Ignore the Enter that selected us from menu
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
        bool isPCAP = name.endsWith(".pcap");
        bool isPMKID = name.endsWith(".22000") && !name.endsWith("_hs.22000");
        bool isHS22000 = name.endsWith("_hs.22000");
        
        // Skip PCAP if we have the corresponding _hs.22000 (avoid duplicates)
        // We prefer showing _hs.22000 because it's hashcat-ready
        if (isPCAP) {
            String baseName = name.substring(0, name.indexOf('.'));
            String hs22kPath = "/handshakes/" + baseName + "_hs.22000";
            if (SD.exists(hs22kPath)) {
                file = dir.openNextFile();
                continue;  // Skip this PCAP, _hs.22000 will be shown instead
            }
        }
        
        if (isPCAP || isPMKID || isHS22000) {
            CaptureInfo info;
            info.filename = name;
            info.fileSize = file.size();
            info.captureTime = file.getLastWrite();
            info.isPMKID = isPMKID;  // Only true for actual PMKID files
            
            // Extract BSSID from filename (e.g., "64EEB7208286.pcap" or "64EEB7208286_hs.22000")
            String baseName = name.substring(0, name.indexOf('.'));
            // Handle _hs suffix for handshake 22000 files
            if (baseName.endsWith("_hs")) {
                baseName = baseName.substring(0, baseName.length() - 3);
            }
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
            // PMKID uses _pmkid.txt suffix, handshake uses .txt
            String txtPath = isPMKID ? 
                "/handshakes/" + baseName + "_pmkid.txt" :
                "/handshakes/" + baseName + ".txt";
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
            
            // Check WPA-SEC status
            info.status = CaptureStatus::LOCAL;
            info.password = "";
            
            captures.push_back(info);
        }
        file = dir.openNextFile();
    }
    dir.close();
    
    // Update WPA-SEC status for all captures
    updateWPASecStatus();
    
    // Sort by capture time (newest first)
    std::sort(captures.begin(), captures.end(), [](const CaptureInfo& a, const CaptureInfo& b) {
        return a.captureTime > b.captureTime;
    });
    
    Serial.printf("[CAPTURES] Found %d captures\n", captures.size());
}

void CapturesMenu::updateWPASecStatus() {
    // Load WPA-SEC cache (lazy, only loads once)
    WPASec::loadCache();
    
    for (auto& cap : captures) {
        // Normalize BSSID for lookup (remove colons)
        String normalBssid = cap.bssid;
        normalBssid.replace(":", "");
        
        if (WPASec::isCracked(normalBssid.c_str())) {
            cap.status = CaptureStatus::CRACKED;
            cap.password = WPASec::getPassword(normalBssid.c_str());
        } else if (WPASec::isUploaded(normalBssid.c_str())) {
            cap.status = CaptureStatus::UPLOADED;
        } else {
            cap.status = CaptureStatus::LOCAL;
        }
    }
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
    
    // Handle nuke confirmation modal
    if (nukeConfirmActive) {
        if (M5Cardputer.Keyboard.isKeyPressed('y') || M5Cardputer.Keyboard.isKeyPressed('Y')) {
            nukeLoot();
            nukeConfirmActive = false;
            Display::clearBottomOverlay();
            scanCaptures();  // Refresh list (should be empty now)
        } else if (M5Cardputer.Keyboard.isKeyPressed('n') || M5Cardputer.Keyboard.isKeyPressed('N') ||
                   M5Cardputer.Keyboard.isKeyPressed('`') || keys.enter) {
            nukeConfirmActive = false;  // Cancel
            Display::clearBottomOverlay();
        }
        return;
    }
    
    // Handle detail view modal - Enter/backtick closes, U/R trigger actions
    if (detailViewActive) {
        if (keys.enter || M5Cardputer.Keyboard.isKeyPressed('`')) {
            detailViewActive = false;
            return;
        }
        // Allow U/R in modal - close modal and trigger action
        if (M5Cardputer.Keyboard.isKeyPressed('u') || M5Cardputer.Keyboard.isKeyPressed('U')) {
            detailViewActive = false;
            if (!captures.empty() && selectedIndex < captures.size()) {
                uploadSelected();
            }
            return;
        }
        if (M5Cardputer.Keyboard.isKeyPressed('r') || M5Cardputer.Keyboard.isKeyPressed('R')) {
            detailViewActive = false;
            refreshResults();
            return;
        }
        return;  // Block other inputs while detail view is open
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
        if (!captures.empty() && selectedIndex < captures.size() - 1) {
            selectedIndex++;
            if (selectedIndex >= scrollOffset + VISIBLE_ITEMS) {
                scrollOffset = selectedIndex - VISIBLE_ITEMS + 1;
            }
        }
    }
    
    // Enter shows detail view (password if cracked)
    if (keys.enter) {
        if (!captures.empty() && selectedIndex < captures.size()) {
            detailViewActive = true;
        }
    }
    
    // Nuke all loot with D key
    if (M5Cardputer.Keyboard.isKeyPressed('d') || M5Cardputer.Keyboard.isKeyPressed('D')) {
        if (!captures.empty()) {
            nukeConfirmActive = true;
            Display::setBottomOverlay("PERMANENT | NO UNDO");
        }
    }
    
    // U key uploads selected capture to WPA-SEC
    if (M5Cardputer.Keyboard.isKeyPressed('u') || M5Cardputer.Keyboard.isKeyPressed('U')) {
        if (!captures.empty() && selectedIndex < captures.size()) {
            uploadSelected();
        }
    }
    
    // R key refreshes results from WPA-SEC
    if (M5Cardputer.Keyboard.isKeyPressed('r') || M5Cardputer.Keyboard.isKeyPressed('R')) {
        refreshResults();
    }
    
    // Exit with backtick
    if (M5Cardputer.Keyboard.isKeyPressed('`')) {
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
    
    canvas.fillSprite(COLOR_BG);
    canvas.setTextColor(COLOR_FG);
    canvas.setTextSize(1);
    
    if (captures.empty()) {
        canvas.setCursor(4, 40);
        canvas.print("No captures found");
        canvas.setCursor(4, 55);
        canvas.print("[O] to hunt.");
        return;
    }
    
    // Draw captures list
    int y = 2;
    int lineHeight = 18;
    
    for (uint8_t i = scrollOffset; i < captures.size() && i < scrollOffset + VISIBLE_ITEMS; i++) {
        const CaptureInfo& cap = captures[i];
        
        // Highlight selected
        if (i == selectedIndex) {
            canvas.fillRect(0, y - 1, canvas.width(), lineHeight, COLOR_FG);
            canvas.setTextColor(COLOR_BG);
        } else {
            canvas.setTextColor(COLOR_FG);
        }
        
        // SSID (truncated if needed) - show [P] prefix for PMKID, status indicator
        canvas.setCursor(4, y);
        String displaySSID = cap.isPMKID ? "[P]" : "";
        displaySSID += cap.ssid;
        if (displaySSID.length() > 10) {
            displaySSID = displaySSID.substring(0, 8) + "..";
        }
        canvas.print(displaySSID);
        
        // Status indicator
        canvas.setCursor(75, y);
        if (cap.status == CaptureStatus::CRACKED) {
            canvas.print("[OK]");
        } else if (cap.status == CaptureStatus::UPLOADED) {
            canvas.print("[..]");
        } else {
            canvas.print("[--]");
        }
        
        // Date/time
        canvas.setCursor(105, y);
        canvas.print(formatTime(cap.captureTime));
        
        // File size (KB)
        canvas.setCursor(180, y);
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
    
    // Draw nuke confirmation modal if active
    if (nukeConfirmActive) {
        drawNukeConfirm(canvas);
    }
    
    // Draw detail view modal if active
    if (detailViewActive) {
        drawDetailView(canvas);
    }
    
    // Draw connecting overlay if active
    if (connectingWiFi || uploadingFile || refreshingResults) {
        drawConnecting(canvas);
    }
    // BSSID shown in bottom bar via getSelectedBSSID()
}

void CapturesMenu::drawNukeConfirm(M5Canvas& canvas) {
    // Modal box dimensions - matches PIGGYBLUES warning style
    const int boxW = 200;
    const int boxH = 70;
    const int boxX = (canvas.width() - boxW) / 2;
    const int boxY = (canvas.height() - boxH) / 2 - 5;
    
    // Black border then pink fill
    canvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);
    
    // Black text on pink background
    canvas.setTextColor(COLOR_BG, COLOR_FG);
    canvas.setTextDatum(top_center);
    canvas.setTextSize(1);
    
    int centerX = canvas.width() / 2;
    
    // Hacker edgy message
    canvas.drawString("!! SCORCHED EARTH !!", centerX, boxY + 8);
    canvas.drawString("rm -rf /handshakes/*", centerX, boxY + 22);
    canvas.drawString("This kills the loot.", centerX, boxY + 36);
    canvas.drawString("[Y] Do it  [N] Abort", centerX, boxY + 54);
}

void CapturesMenu::nukeLoot() {
    Serial.println("[CAPTURES] Nuking all loot...");
    
    if (!SD.exists("/handshakes")) {
        return;
    }
    
    File dir = SD.open("/handshakes");
    if (!dir || !dir.isDirectory()) {
        return;
    }
    
    // Collect filenames first (can't delete while iterating)
    std::vector<String> files;
    File file = dir.openNextFile();
    while (file) {
        files.push_back(String("/handshakes/") + file.name());
        file = dir.openNextFile();
    }
    dir.close();
    
    // Delete all files
    int deleted = 0;
    for (const auto& path : files) {
        if (SD.remove(path)) {
            deleted++;
        }
    }
    
    Serial.printf("[CAPTURES] Nuked %d files\n", deleted);
    
    // Reset selection
    selectedIndex = 0;
    scrollOffset = 0;
    captures.clear();
}

String CapturesMenu::getSelectedBSSID() {
    if (selectedIndex < captures.size()) {
        return captures[selectedIndex].bssid;
    }
    return "CR4CK TH3 L00T: [U] [R] [D]";
}
void CapturesMenu::drawDetailView(M5Canvas& canvas) {
    if (selectedIndex >= captures.size()) return;
    
    const CaptureInfo& cap = captures[selectedIndex];
    
    // Modal box dimensions
    const int boxW = 220;
    const int boxH = 85;
    const int boxX = (canvas.width() - boxW) / 2;
    const int boxY = (canvas.height() - boxH) / 2 - 5;
    
    // Black border then pink fill
    canvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);
    
    // Black text on pink background
    canvas.setTextColor(COLOR_BG, COLOR_FG);
    canvas.setTextDatum(top_center);
    canvas.setTextSize(1);
    
    int centerX = canvas.width() / 2;
    
    // SSID
    String ssidLine = cap.ssid;
    if (ssidLine.length() > 24) ssidLine = ssidLine.substring(0, 22) + "..";
    canvas.drawString(ssidLine, centerX, boxY + 6);
    
    // BSSID
    canvas.drawString(cap.bssid, centerX, boxY + 20);
    
    // Status and password
    if (cap.status == CaptureStatus::CRACKED) {
        canvas.drawString("** CR4CK3D **", centerX, boxY + 38);
        
        // Password in larger text
        String pwLine = cap.password;
        if (pwLine.length() > 20) pwLine = pwLine.substring(0, 18) + "..";
        canvas.drawString(pwLine, centerX, boxY + 54);
    } else if (cap.status == CaptureStatus::UPLOADED) {
        canvas.drawString("Uploaded, waiting...", centerX, boxY + 38);
        canvas.drawString("[R] Refresh results", centerX, boxY + 54);
    } else {
        canvas.drawString("Not uploaded yet", centerX, boxY + 38);
        canvas.drawString("[U] Upload to WPA-SEC", centerX, boxY + 54);
    }
    
    canvas.drawString("[Enter/`] Close", centerX, boxY + 72);
}

void CapturesMenu::drawConnecting(M5Canvas& canvas) {
    // Overlay message
    const int boxW = 180;
    const int boxH = 40;
    const int boxX = (canvas.width() - boxW) / 2;
    const int boxY = (canvas.height() - boxH) / 2;
    
    canvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 6, COLOR_BG);
    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 6, COLOR_FG);
    
    canvas.setTextColor(COLOR_BG, COLOR_FG);
    canvas.setTextDatum(top_center);
    
    int centerX = canvas.width() / 2;
    
    if (connectingWiFi) {
        canvas.drawString("Connecting WiFi...", centerX, boxY + 8);
        canvas.drawString(WPASec::getStatus(), centerX, boxY + 22);
    } else if (uploadingFile) {
        canvas.drawString("Uploading...", centerX, boxY + 8);
        canvas.drawString(WPASec::getStatus(), centerX, boxY + 22);
    } else if (refreshingResults) {
        canvas.drawString("Fetching results...", centerX, boxY + 8);
        canvas.drawString(WPASec::getStatus(), centerX, boxY + 22);
    }
}

void CapturesMenu::uploadSelected() {
    if (selectedIndex >= captures.size()) return;
    
    const CaptureInfo& cap = captures[selectedIndex];
    
    // Check if WPA-SEC key is configured
    if (Config::wifi().wpaSecKey.isEmpty()) {
        Display::showToast("Set WPA-SEC key first");
        delay(500);
        return;
    }
    
    // Already cracked? No need to upload
    if (cap.status == CaptureStatus::CRACKED) {
        Display::showToast("Already cracked!");
        delay(500);
        return;
    }
    
    // Find the PCAP file for this capture
    String baseName = cap.bssid;
    baseName.replace(":", "");
    String pcapPath = "/handshakes/" + baseName + ".pcap";
    
    if (!SD.exists(pcapPath)) {
        Display::showToast("No PCAP file found");
        delay(500);
        return;
    }
    
    // Connect to WiFi if needed
    bool weConnected = false;
    connectingWiFi = true;
    
    // Force a redraw before blocking operation
    Display::update();
    delay(100);
    
    if (!WPASec::isConnected()) {
        if (!WPASec::connect()) {
            connectingWiFi = false;
            Display::showToast(WPASec::getLastError());
            delay(500);
            return;
        }
        weConnected = true;
    }
    connectingWiFi = false;
    
    // Upload the file
    uploadingFile = true;
    Display::update();
    delay(100);
    
    bool success = WPASec::uploadCapture(pcapPath.c_str());
    uploadingFile = false;
    
    if (success) {
        Display::showToast("Upload OK!");
        delay(500);
        // Update status
        captures[selectedIndex].status = CaptureStatus::UPLOADED;
    } else {
        Display::showToast(WPASec::getLastError());
        delay(500);
    }
    
    // Disconnect WiFi only if we initiated the connection
    if (weConnected) {
        WPASec::disconnect();
    }
}

void CapturesMenu::refreshResults() {
    // Check if WPA-SEC key is configured
    if (Config::wifi().wpaSecKey.isEmpty()) {
        Display::showToast("Set WPA-SEC key first");
        delay(500);
        return;
    }
    
    // Connect to WiFi if needed
    bool weConnected = false;
    connectingWiFi = true;
    Display::update();
    delay(100);
    
    if (!WPASec::isConnected()) {
        if (!WPASec::connect()) {
            connectingWiFi = false;
            Display::showToast(WPASec::getLastError());
            delay(500);
            return;
        }
        weConnected = true;
    }
    connectingWiFi = false;
    
    // Fetch results
    refreshingResults = true;
    Display::update();
    delay(100);
    
    bool success = WPASec::fetchResults();
    refreshingResults = false;
    
    if (success) {
        Display::showToast(WPASec::getStatus());
        delay(500);
        // Update status for all captures
        updateWPASecStatus();
    } else {
        Display::showToast(WPASec::getLastError());
        delay(500);
    }
    
    // Disconnect WiFi only if we initiated the connection
    if (weConnected) {
        WPASec::disconnect();
    }
}