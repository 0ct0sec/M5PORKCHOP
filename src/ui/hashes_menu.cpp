// Hashes Menu - View saved handshake captures

#include "hashes_menu.h"
#include <M5Cardputer.h>
#include <SD.h>
#include <WiFi.h>
#include <time.h>
#include <ctype.h>
#include <string.h>
#include "display.h"
#include "../web/wpasec.h"
#include "../core/config.h"
#include "../core/sd_layout.h"
#include "../core/wifi_utils.h"
#include "../core/heap_health.h"
#include <esp_heap_caps.h>

// Static member initialization
std::vector<CaptureInfo> HashesMenu::captures;
uint8_t HashesMenu::selectedIndex = 0;
uint8_t HashesMenu::scrollOffset = 0;
bool HashesMenu::active = false;
bool HashesMenu::keyWasPressed = false;
bool HashesMenu::nukeConfirmActive = false;
bool HashesMenu::detailViewActive = false;
bool HashesMenu::scanInProgress = false;
bool HashesMenu::scanDeferredHeap = false;
unsigned long HashesMenu::lastScanTime = 0;
char HashesMenu::scanBaseDir[32] = "";
File HashesMenu::scanDir;
File HashesMenu::currentFile;
bool HashesMenu::scanComplete = false;
size_t HashesMenu::scanProgress = 0;
bool HashesMenu::wpasecUpdateInProgress = false;
unsigned long HashesMenu::lastWpasecUpdateTime = 0;
size_t HashesMenu::wpasecUpdateProgress = 0;

// Hint rotation
uint8_t HashesMenu::hintIndex = 0;
const char* const HashesMenu::HINTS[] = {
    "FEED YO HASHCAT.",
    "COLLECTED PAIN. COMPRESSED.",
    "ENT:DET  S:SYNC  D:NUKE",
    "MALLOC SAID NAH.",
    "YOUR LOOT. YOUR PROBLEM."
};

// WPA-SEC Sync state
bool HashesMenu::syncModalActive = false;
SyncState HashesMenu::syncState = SyncState::IDLE;
char HashesMenu::syncStatusText[48] = "";
uint8_t HashesMenu::syncProgress = 0;
uint8_t HashesMenu::syncTotal = 0;
unsigned long HashesMenu::syncStartTime = 0;
uint8_t HashesMenu::syncUploaded = 0;
uint8_t HashesMenu::syncFailed = 0;
uint16_t HashesMenu::syncCracked = 0;
char HashesMenu::syncError[48] = "";

namespace {

static bool endsWithIgnoreCase(const char* value, const char* suffix) {
    if (!value || !suffix) return false;
    size_t valueLen = strlen(value);
    size_t suffixLen = strlen(suffix);
    if (suffixLen == 0 || valueLen < suffixLen) return false;
    const char* tail = value + valueLen - suffixLen;
    for (size_t i = 0; i < suffixLen; i++) {
        char a = tail[i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static bool dirHasCaptureFiles(const char* dirPath) {
    if (!dirPath || !SD.exists(dirPath)) return false;

    File dir = SD.open(dirPath);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return false;
    }

    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            const char* rawName = entry.name();
            const char* slash = strrchr(rawName, '/');
            const char* name = slash ? slash + 1 : rawName;
            if (endsWithIgnoreCase(name, ".pcap") || endsWithIgnoreCase(name, ".22000")) {
                entry.close();
                dir.close();
                return true;
            }
        }
        entry.close();
        entry = dir.openNextFile();
        yield();
    }

    dir.close();
    return false;
}

static const char* resolveCaptureScanDir() {
    const char* preferredDir = SDLayout::handshakesDir();
    const char* fallbackDir = SDLayout::usingNewLayout() ? "/handshakes" : "/m5porkchop/handshakes";
    if (strcmp(preferredDir, fallbackDir) == 0) return preferredDir;

    const bool preferredHasFiles = dirHasCaptureFiles(preferredDir);
    const bool fallbackHasFiles = dirHasCaptureFiles(fallbackDir);
    if (!preferredHasFiles && fallbackHasFiles) {
        return fallbackDir;
    }

    if (SD.exists(preferredDir)) return preferredDir;
    if (SD.exists(fallbackDir)) return fallbackDir;
    return preferredDir;
}

} // namespace

void HashesMenu::init() {
    captures.clear();
    selectedIndex = 0;
    scrollOffset = 0;
    scanDeferredHeap = false;
}

void HashesMenu::show() {
    active = true;
    selectedIndex = 0;
    scrollOffset = 0;
    keyWasPressed = true;  // Ignore the Enter that selected us from menu
    hintIndex = esp_random() % HINT_COUNT;

    // If scan fails, the captures list will remain empty
    // This is handled by the draw function which shows "No captures found"
    scanCaptures();
}

void HashesMenu::hide() {
    active = false;
    
    // FIX: Always call emergencyCleanup first - ensures file handles closed
    emergencyCleanup();
    
    // Enhanced: Force cleanup even if interrupted
    captures.clear();
    captures.shrink_to_fit();  // Release vector capacity
    WPASec::freeCacheMemory();
    
    // Reset all async state to prevent leaks (redundant after emergencyCleanup but safe)
    scanInProgress = false;
    wpasecUpdateInProgress = false;
    if (scanDir) {
        scanDir.close();
    }
    if (currentFile) {
        currentFile.close();
    }
    scanDeferredHeap = false;
    scanBaseDir[0] = '\0';
}

void HashesMenu::emergencyCleanup() {
    // Can be called from main loop when heap is critical
    if (!active) return;
    
    Serial.println("[HASHES] Emergency cleanup triggered");
    captures.clear();
    captures.shrink_to_fit();
    WPASec::freeCacheMemory();
    
    // Stop any in-progress operations
    scanInProgress = false;
    wpasecUpdateInProgress = false;
    if (scanDir) {
        scanDir.close();
    }
    if (currentFile) {
        currentFile.close();
    }
    scanDeferredHeap = false;
    scanBaseDir[0] = '\0';
}

bool HashesMenu::scanCaptures() {
    // Initialize async scan
    captures.clear();
    captures.reserve(8);  // Grow naturally — reserve(100) was ~17KB contiguous, crash-prone on fragmented heap
    scanDeferredHeap = false;

    // Guard: Skip if no SD card available
    if (!Config::isSDAvailable()) {
        Serial.println("[HASHES] No SD card available");
        scanComplete = true;
        scanInProgress = false;
        return false;
    }

    // Guard: Skip SD scan at Critical pressure — file listing only needs small FAT buffers
    if (HeapHealth::getPressureLevel() >= HeapPressureLevel::Critical) {
        Serial.println("[HASHES] Scan deferred: heap pressure");
        scanDeferredHeap = true;
        scanComplete = true;
        scanInProgress = false;
        return false;
    }

    const char* preferredDir = SDLayout::handshakesDir();
    const char* handshakesDir = resolveCaptureScanDir();
    if (strcmp(handshakesDir, preferredDir) != 0) {
        Serial.printf("[HASHES] Using fallback scan dir: %s (preferred %s)\n",
                      handshakesDir, preferredDir);
    }
    strncpy(scanBaseDir, handshakesDir, sizeof(scanBaseDir) - 1);
    scanBaseDir[sizeof(scanBaseDir) - 1] = '\0';

    // Create directory if it doesn't exist
    if (!SD.exists(handshakesDir)) {
        Serial.println("[HASHES] No handshakes directory, creating...");
        if (!SD.mkdir(handshakesDir)) {
            Serial.println("[HASHES] Failed to create handshakes directory");
            scanComplete = true;
            scanInProgress = false;
            return false;
        }
    }

    scanDir = SD.open(handshakesDir);
    if (!scanDir || !scanDir.isDirectory()) {
        Serial.println("[HASHES] Failed to open handshakes directory");
        scanComplete = true;
        scanInProgress = false;
        scanDir.close();
        return false;
    }

    scanInProgress = true;
    scanComplete = false;
    scanProgress = 0;
    lastScanTime = millis();
    
    return true;
}

// Helper: check if string of length n is all hex chars
static bool isAllHex(const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

void HashesMenu::processAsyncScan() {
    if (!scanInProgress || scanComplete) {
        return;
    }

    // Throttle the scan to avoid blocking the UI
    if (millis() - lastScanTime < SCAN_DELAY) {
        return;
    }

    lastScanTime = millis();

    // Process a chunk of files
    const char* scanRoot = (scanBaseDir[0] != '\0') ? scanBaseDir : SDLayout::handshakesDir();
    size_t processed = 0;
    while (processed < SCAN_CHUNK_SIZE && !scanComplete) {
        currentFile = scanDir.openNextFile();

        if (!currentFile) {
            // No more files, we're done with scanning
            scanComplete = true;
            scanInProgress = false;
            scanDir.close();

            // Sort by capture time (newest first)
            std::sort(captures.begin(), captures.end(), [](const CaptureInfo& a, const CaptureInfo& b) {
                return a.captureTime > b.captureTime;
            });

            // Start async WPA-SEC status update after scanning is complete
            if (!captures.empty()) {
                wpasecUpdateInProgress = true;
                wpasecUpdateProgress = 0;
                lastWpasecUpdateTime = millis();
            }

            Serial.printf("[HASHES] Async scan complete. Found %d captures\n", captures.size());
            break;
        }

        // Normalize to basename: some FS drivers return full paths.
        const char* rawName = currentFile.name();
        const char* slash = strrchr(rawName, '/');
        const char* name = slash ? slash + 1 : rawName;
        size_t nameLen = strlen(name);

        bool isPCAP = endsWithIgnoreCase(name, ".pcap");
        bool isHS22000 = endsWithIgnoreCase(name, "_hs.22000");
        bool isPMKID = !isHS22000 && endsWithIgnoreCase(name, ".22000");

        // Skip PCAP if we have the corresponding _hs.22000 (avoid duplicates).
        if (isPCAP) {
            // Build base name: everything before the dot
            const char* dot = strrchr(name, '.');
            size_t baseLen = dot ? (size_t)(dot - name) : nameLen;
            char hs22kPath[80];
            snprintf(hs22kPath, sizeof(hs22kPath), "%s/%.*s_hs.22000",
                     scanRoot, (int)baseLen, name);
            if (SD.exists(hs22kPath)) {
                currentFile.close();
                processed++;
                continue;
            }
        }

        if (isPCAP || isPMKID || isHS22000) {
            CaptureInfo info;
            memset(&info, 0, sizeof(info));
            strncpy(info.filename, name, sizeof(info.filename) - 1);
            info.fileSize = currentFile.size();
            info.captureTime = currentFile.getLastWrite();
            info.isPMKID = isPMKID;

            // Compute base name (strip extension and _hs suffix)
            const char* dot = strrchr(name, '.');
            size_t baseLen = dot ? (size_t)(dot - name) : nameLen;
            if (baseLen > 3 && strncmp(name + baseLen - 3, "_hs", 3) == 0) {
                baseLen -= 3;
            }

            // Dual-format detection:
            // Legacy: base name is exactly 12 hex chars (BSSID only)
            // New format: last 12 chars are hex, preceded by '_' (SSID_BSSID)
            if (baseLen == 12 && isAllHex(name, 12)) {
                // Legacy format: BSSID is first 12 chars
                const char* b = name;
                snprintf(info.bssid, sizeof(info.bssid),
                         "%.2s:%.2s:%.2s:%.2s:%.2s:%.2s",
                         b, b+2, b+4, b+6, b+8, b+10);

                // Try companion .txt for SSID (legacy files)
                char txtPath[80];
                if (isPMKID) {
                    snprintf(txtPath, sizeof(txtPath), "%s/%.12s_pmkid.txt",
                             scanRoot, name);
                } else {
                    snprintf(txtPath, sizeof(txtPath), "%s/%.12s.txt",
                             scanRoot, name);
                }
                if (SD.exists(txtPath)) {
                    File txtFile = SD.open(txtPath, FILE_READ);
                    if (txtFile) {
                        char buf[34];
                        int n = txtFile.readBytesUntil('\n', buf, sizeof(buf) - 1);
                        buf[n] = '\0';
                        while (n > 0 && (buf[n-1] == ' ' || buf[n-1] == '\r' || buf[n-1] == '\t')) buf[--n] = '\0';
                        if (n > 0) {
                            strncpy(info.ssid, buf, sizeof(info.ssid) - 1);
                        }
                        txtFile.close();
                    }
                }
            } else if (baseLen > 13 && name[baseLen - 13] == '_' &&
                       isAllHex(name + baseLen - 12, 12)) {
                // New format: SSID_BSSID — extract BSSID from last 12 chars
                const char* b = name + baseLen - 12;
                snprintf(info.bssid, sizeof(info.bssid),
                         "%.2s:%.2s:%.2s:%.2s:%.2s:%.2s",
                         b, b+2, b+4, b+6, b+8, b+10);

                // Extract SSID from chars before _BSSID
                size_t ssidLen = baseLen - 13;
                if (ssidLen > sizeof(info.ssid) - 1) ssidLen = sizeof(info.ssid) - 1;
                memcpy(info.ssid, name, ssidLen);
                info.ssid[ssidLen] = '\0';
            } else {
                // Unknown format — use full base as BSSID display
                size_t copyLen = baseLen < sizeof(info.bssid) - 1 ? baseLen : sizeof(info.bssid) - 1;
                memcpy(info.bssid, name, copyLen);
                info.bssid[copyLen] = '\0';
            }

            if (info.ssid[0] == '\0') {
                strncpy(info.ssid, "[UNKNOWN]", sizeof(info.ssid) - 1);
            }

            info.status = CaptureStatus::LOCAL;

            captures.push_back(info);

            if (captures.size() >= MAX_CAPTURES) {
                scanComplete = true;
                scanInProgress = false;
                currentFile.close();
                scanDir.close();
                Serial.println("[HASHES] Hit capture limit, stopped scan");
                break;
            }
        }

        currentFile.close();
        processed++;
        scanProgress++;

        if (processed >= SCAN_CHUNK_SIZE) {
            break;
        }
    }
}

void HashesMenu::updateWPASecStatus() {
    // Load WPA-SEC cache (lazy, only loads once)
    WPASec::loadCache();
    
    char normalized[13] = {0};
    for (auto& cap : captures) {
        // Normalize BSSID for lookup (remove colons)
        WPASec::normalizeBSSID_Char(cap.bssid, normalized, sizeof(normalized));
        if (normalized[0] == '\0') {
            cap.status = CaptureStatus::LOCAL;
            continue;
        }
        
        // Use getPassword() directly — avoids redundant binary search vs isCracked()+getPassword()
        const char* pw = WPASec::getPassword(normalized);
        if (pw[0] != '\0') {
            cap.status = CaptureStatus::CRACKED;
            strncpy(cap.password, pw, sizeof(cap.password) - 1);
            cap.password[sizeof(cap.password) - 1] = '\0';
        } else if (WPASec::isUploaded(normalized)) {
            cap.status = CaptureStatus::UPLOADED;
        } else {
            cap.status = CaptureStatus::LOCAL;
        }
    }
}

void HashesMenu::processAsyncWPASecUpdate() {
    if (!wpasecUpdateInProgress || captures.empty()) {
        wpasecUpdateInProgress = false;
        return;
    }
    
    // Throttle the update to avoid blocking the UI
    if (millis() - lastWpasecUpdateTime < WPASEC_UPDATE_DELAY) {
        return;
    }
    
    lastWpasecUpdateTime = millis();
    
    // Process a chunk of captures
    size_t processed = 0;
    while (processed < WPASEC_UPDATE_CHUNK_SIZE && wpasecUpdateProgress < captures.size()) {
        auto& cap = captures[wpasecUpdateProgress];
        
        // Normalize BSSID for lookup (remove colons)
        char normalized[13] = {0};
        WPASec::normalizeBSSID_Char(cap.bssid, normalized, sizeof(normalized));
        
        if (normalized[0] != '\0') {
            // Use getPassword() directly — avoids redundant binary search vs isCracked()+getPassword()
            const char* pw = WPASec::getPassword(normalized);
            if (pw[0] != '\0') {
                cap.status = CaptureStatus::CRACKED;
                strncpy(cap.password, pw, sizeof(cap.password) - 1);
                cap.password[sizeof(cap.password) - 1] = '\0';
            } else if (WPASec::isUploaded(normalized)) {
                cap.status = CaptureStatus::UPLOADED;
            } else {
                cap.status = CaptureStatus::LOCAL;
            }
        } else {
            cap.status = CaptureStatus::LOCAL;
        }

        wpasecUpdateProgress++;
        processed++;
        
        // Yield periodically to allow other tasks to run
        if (processed >= WPASEC_UPDATE_CHUNK_SIZE) {
            // Still more to do, but yield control back to other tasks
            break;
        }
    }
    
    // Check if we're done with all captures
    if (wpasecUpdateProgress >= captures.size()) {
        wpasecUpdateInProgress = false;
        Serial.printf("[HASHES] Async WPA-SEC update complete. Updated %d captures\n", captures.size());
    }
}

void HashesMenu::update() {
    if (!active) return;
    
    // Process sync state machine if active
    if (syncModalActive && syncState != SyncState::IDLE && 
        syncState != SyncState::COMPLETE && syncState != SyncState::ERROR) {
        processSyncState();
    }
    
    // Process async file scanning if in progress (not during sync)
    if (!syncModalActive) {
        processAsyncScan();
        
        // Process async WPA-SEC status updates if in progress
        processAsyncWPASecUpdate();
    }
    
    handleInput();
}

void HashesMenu::handleInput() {
    bool anyPressed = M5Cardputer.Keyboard.isPressed();
    
    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }
    
    if (keyWasPressed) return;
    keyWasPressed = true;
    
    auto keys = M5Cardputer.Keyboard.keysState();

    // Handle sync modal
    if (syncModalActive) {
        if (syncState == SyncState::ERROR || syncState == SyncState::COMPLETE) {
            // Enter closes the modal after completion/error
            if (keys.enter || M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
                syncModalActive = false;
                syncState = SyncState::IDLE;
                scanCaptures();  // Rescan captures after sync
            }
        } else {
            // ESC cancels during sync
            if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
                cancelSync();
            }
        }
        return;  // Block other inputs during sync
    }

    // Handle nuke confirmation modal
    if (nukeConfirmActive) {
        if (M5Cardputer.Keyboard.isKeyPressed('y') || M5Cardputer.Keyboard.isKeyPressed('Y')) {
            nukeLoot();
            nukeConfirmActive = false;
            Display::clearBottomOverlay();
            scanCaptures();  // Refresh list (should be empty now)
        } else if (M5Cardputer.Keyboard.isKeyPressed('n') || M5Cardputer.Keyboard.isKeyPressed('N') ||
                   M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE) || keys.enter) {
            nukeConfirmActive = false;  // Cancel
            Display::clearBottomOverlay();
        }
        return;
    }
    
    // Handle detail view modal - Enter/backspace closes
    if (detailViewActive) {
        if (keys.enter || M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
            detailViewActive = false;
            return;
        }
        return;  // Block other inputs while detail view is open
    }
    
    // Navigation with ; (up) and . (down) — also rotates hints
    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        hintIndex = (hintIndex + 1) % HINT_COUNT;
        if (selectedIndex > 0) {
            selectedIndex--;
            if (selectedIndex < scrollOffset) {
                scrollOffset = selectedIndex;
            }
        }
    }

    if (M5Cardputer.Keyboard.isKeyPressed('.')) {
        hintIndex = (hintIndex + 1) % HINT_COUNT;
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
    
    // S key triggers WPA-SEC sync
    if (M5Cardputer.Keyboard.isKeyPressed('s') || M5Cardputer.Keyboard.isKeyPressed('S')) {
        startSync();
    }
    
    // Nuke all loot with D key
    if (M5Cardputer.Keyboard.isKeyPressed('d') || M5Cardputer.Keyboard.isKeyPressed('D')) {
        if (!captures.empty()) {
            nukeConfirmActive = true;
            Display::setBottomOverlay("PERMANENT | NO UNDO");
        }
    }
    
    // Backspace - go back
    if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
        hide();
    }
}

void HashesMenu::formatTime(char* out, size_t len, time_t t) {
    if (!out || len == 0) return;
    if (t == 0) {
        strncpy(out, "UNKNOWN", len - 1);
        out[len - 1] = '\0';
        return;
    }
    
    struct tm* timeinfo = localtime(&t);
    if (!timeinfo) {
        strncpy(out, "UNKNOWN", len - 1);
        out[len - 1] = '\0';
        return;
    }
    
    // Format: "Dec 06 14:32"
    strftime(out, len, "%b %d %H:%M", timeinfo);
}

static void formatSize(char* out, size_t len, uint32_t bytes) {
    if (!out || len == 0) return;
    if (bytes < 1024) {
        snprintf(out, len, "%uB", (unsigned)bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(out, len, "%uKB", (unsigned)(bytes / 1024));
    } else {
        snprintf(out, len, "%uMB", (unsigned)(bytes / (1024 * 1024)));
    }
}

void HashesMenu::draw(M5Canvas& canvas) {
    if (!active) return;

    canvas.fillSprite(COLOR_BG);
    canvas.setTextColor(COLOR_FG);
    canvas.setTextSize(1);

    // Check if SD card is not available
    if (!Config::isSDAvailable()) {
        canvas.setCursor(4, 40);
        canvas.print("NO SD CARD");
        canvas.setCursor(4, 55);
        canvas.print("INSERT AND RESTART");
        return;
    }

    // Draw sync modal FIRST - takes precedence over empty captures message
    if (syncModalActive) {
        drawSyncModal(canvas);
        return;
    }

    if (captures.empty()) {
        if (scanDeferredHeap) {
            canvas.setCursor(4, 36);
            canvas.print("SCAN DEFERRED");
            canvas.setCursor(4, 52);
            canvas.print("HEAP PRESSURE TOO HIGH");
            canvas.setCursor(4, 68);
            canvas.print("FREE MEMORY THEN RETRY");
        } else {
            canvas.setCursor(4, 36);
            canvas.print("NO CAPTURES FOUND");
            canvas.setCursor(4, 52);
            canvas.print("PRESS [O] FOR OINK");
            canvas.setCursor(4, 68);
            canvas.print("SYNC VIA COMMANDER");
        }
        return;
    }

    // Summary stats line
    uint16_t total = captures.size();
    uint16_t cracked = 0, uploaded = 0, local = 0;
    for (const auto& cap : captures) {
        if (cap.status == CaptureStatus::CRACKED) cracked++;
        else if (cap.status == CaptureStatus::UPLOADED) uploaded++;
        else local++;
    }
    char summary[64];
    snprintf(summary, sizeof(summary), "LOOT %u OK %u UP %u LOC %u",
             (unsigned)total, (unsigned)cracked, (unsigned)uploaded, (unsigned)local);
    canvas.setCursor(4, 2);
    canvas.print(summary);

    // Column headers
    canvas.setCursor(4, 12);
    canvas.print("SSID");
    canvas.setCursor(120, 12);
    canvas.print("ST");
    canvas.setCursor(150, 12);
    canvas.print("TYPE");
    canvas.setCursor(190, 12);
    canvas.print("SIZE");

    // Capture list
    int y = 22;
    int lineHeight = 16;

    for (uint8_t i = scrollOffset; i < captures.size() && i < scrollOffset + VISIBLE_ITEMS; i++) {
        const CaptureInfo& cap = captures[i];

        // Inverted selection bar
        if (i == selectedIndex) {
            canvas.fillRect(0, y - 1, canvas.width(), lineHeight, COLOR_FG);
            canvas.setTextColor(COLOR_BG);
        } else {
            canvas.setTextColor(COLOR_FG);
        }

        // SSID column — uppercase, max 17 chars, truncate with ..
        canvas.setCursor(4, y);
        char ssidBuf[20];
        size_t pos = 0;
        const char* ssidSrc = cap.ssid;
        while (*ssidSrc && pos < 17) {
            ssidBuf[pos++] = (char)toupper((unsigned char)*ssidSrc++);
        }
        ssidBuf[pos] = '\0';
        if (*ssidSrc) {
            // Truncated — add ..
            if (pos >= 2) {
                ssidBuf[pos - 2] = '.';
                ssidBuf[pos - 1] = '.';
            }
        }
        canvas.print(ssidBuf);

        // Status column
        canvas.setCursor(120, y);
        if (cap.status == CaptureStatus::CRACKED) {
            canvas.print("[OK]");
        } else if (cap.status == CaptureStatus::UPLOADED) {
            canvas.print("[..]");
        } else {
            canvas.print("[--]");
        }

        // Type column
        canvas.setCursor(150, y);
        canvas.print(cap.isPMKID ? "PM" : "HS");

        // Size column
        canvas.setCursor(190, y);
        char sizeBuf[12];
        formatSize(sizeBuf, sizeof(sizeBuf), cap.fileSize);
        canvas.print(sizeBuf);

        y += lineHeight;
    }

    // Scroll indicators
    canvas.setTextColor(COLOR_FG);
    if (scrollOffset > 0) {
        canvas.setCursor(canvas.width() - 10, 22);
        canvas.print("^");
    }
    if (scrollOffset + VISIBLE_ITEMS < captures.size()) {
        canvas.setCursor(canvas.width() - 10, 22 + (VISIBLE_ITEMS - 1) * lineHeight);
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

    // Draw sync modal if active
    if (syncModalActive) {
        drawSyncModal(canvas);
    }
}

void HashesMenu::drawNukeConfirm(M5Canvas& canvas) {
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
    const char* scanRoot = (scanBaseDir[0] != '\0') ? scanBaseDir : SDLayout::handshakesDir();
    char cmd[56];
    snprintf(cmd, sizeof(cmd), "rm -rf %s/*", scanRoot);
    canvas.drawString(cmd, centerX, boxY + 22);
    canvas.drawString("THIS KILLS THE LOOT.", centerX, boxY + 36);
    canvas.drawString("[Y] DO IT  [N] ABORT", centerX, boxY + 54);
}

void HashesMenu::nukeLoot() {
    Serial.println("[HASHES] Nuking all loot...");
    
    const char* handshakesDir = (scanBaseDir[0] != '\0') ? scanBaseDir : SDLayout::handshakesDir();
    if (!SD.exists(handshakesDir)) {
        return;
    }

    File dir = SD.open(handshakesDir);
    if (!dir || !dir.isDirectory()) {
        return;
    }
    
    // Batch collect + delete to avoid vector<String> fragmentation
    // Can't delete while iterating SD, so collect batches of 20
    int deleted = 0;
    bool moreFiles = true;
    while (moreFiles) {
        char paths[4][80];
        uint8_t batchCount = 0;

        dir = SD.open(handshakesDir);
        if (!dir) break;

        File file = dir.openNextFile();
        while (file && batchCount < 4) {
            const char* base = file.name();
            const char* slash = strrchr(base, '/');
            const char* name = slash ? slash + 1 : base;
            snprintf(paths[batchCount], sizeof(paths[0]), "%s/%s", handshakesDir, name);
            batchCount++;
            file.close();
            file = dir.openNextFile();
        }
        if (file) file.close();
        dir.close();

        if (batchCount == 0) break;
        moreFiles = (batchCount == 4);  // Might have more

        for (uint8_t i = 0; i < batchCount; i++) {
            if (SD.remove(paths[i])) deleted++;
        }
        yield();
    }
    
    Serial.printf("[HASHES] Nuked %d files\n", deleted);
    
    // Reset selection
    selectedIndex = 0;
    scrollOffset = 0;
    captures.clear();
}

const char* HashesMenu::getSelectedBSSID() {
    return HINTS[hintIndex];
}
// HS detail parsing for .22000 files
struct HSDetail {
    uint8_t type;       // 1=PMKID, 2=4-way
    uint8_t msgPair;
    char anonce[17];    // first 16 hex of ANonce + null
    char clientMac[18]; // AA:BB:CC:DD:EE:FF + null
    char apMac[18];
    bool valid;
};

static bool parseHS22000Line(const char* line, HSDetail* out) {
    if (!line || !out) return false;
    memset(out, 0, sizeof(HSDetail));

    // WPA*TYPE*field2*MAC_AP*MAC_CLIENT*ESSID*ANONCE*...
    if (strncmp(line, "WPA*", 4) != 0) return false;

    // Tokenize on '*' using a stack copy
    char buf[512];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* fields[10] = {};
    int fieldCount = 0;
    char* p = buf;
    fields[fieldCount++] = p;
    while (*p && fieldCount < 10) {
        if (*p == '*') {
            *p = '\0';
            fields[fieldCount++] = p + 1;
        }
        p++;
    }

    if (fieldCount < 5) return false;

    // fields[0]="WPA", fields[1]=type, fields[2]=MIC/PMKID, fields[3]=MAC_AP,
    // fields[4]=MAC_CLIENT, fields[5]=ESSID, fields[6]=ANONCE, ...
    out->type = (uint8_t)atoi(fields[1]);

    // MAC_AP -> formatted
    const char* ap = fields[3];
    if (strlen(ap) >= 12) {
        snprintf(out->apMac, sizeof(out->apMac), "%.2s:%.2s:%.2s:%.2s:%.2s:%.2s",
                 ap, ap+2, ap+4, ap+6, ap+8, ap+10);
    }

    // MAC_CLIENT -> formatted
    const char* cl = fields[4];
    if (strlen(cl) >= 12) {
        snprintf(out->clientMac, sizeof(out->clientMac), "%.2s:%.2s:%.2s:%.2s:%.2s:%.2s",
                 cl, cl+2, cl+4, cl+6, cl+8, cl+10);
    }

    // ANonce (field 6 for type 02)
    if (out->type == 2 && fieldCount > 6 && strlen(fields[6]) >= 16) {
        memcpy(out->anonce, fields[6], 16);
        out->anonce[16] = '\0';
    }

    // Message pair is field index 8 (9th field) for type 02
    // Format: WPA*02*MIC*MAC_AP*MAC_CLIENT*ESSID*ANONCE*EAPOL*MESSAGEPAIR
    if (out->type == 2 && fieldCount >= 9 && fields[8][0] != '\0') {
        out->msgPair = (uint8_t)strtol(fields[8], nullptr, 16);
    }

    out->valid = true;
    return true;
}

void HashesMenu::drawDetailView(M5Canvas& canvas) {
    if (selectedIndex >= captures.size()) return;

    const CaptureInfo& cap = captures[selectedIndex];

    // Modal box dimensions (shrunk from 85 to 72)
    const int boxW = 220;
    const int boxH = 72;
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
    char ssidLine[24];
    size_t ssidPos = 0;
    const char* ssidSrc = cap.ssid;
    while (*ssidSrc && ssidPos + 1 < sizeof(ssidLine)) {
        ssidLine[ssidPos++] = (char)toupper((unsigned char)*ssidSrc++);
    }
    ssidLine[ssidPos] = '\0';
    if (ssidPos > 20) {
        ssidLine[18] = '.';
        ssidLine[19] = '.';
        ssidLine[20] = '\0';
    }
    canvas.drawString(ssidLine, centerX, boxY + 4);

    // BSSID
    canvas.drawString(cap.bssid, centerX, boxY + 16);

    // Cracked captures: show password (more useful than HS details)
    if (cap.status == CaptureStatus::CRACKED) {
        canvas.drawString("** CR4CK3D **", centerX, boxY + 32);
        char pwLine[24];
        size_t pwLen = strlen(cap.password);
        if (pwLen > 20) {
            memcpy(pwLine, cap.password, 18);
            pwLine[18] = '.';
            pwLine[19] = '.';
            pwLine[20] = '\0';
        } else {
            strncpy(pwLine, cap.password, sizeof(pwLine) - 1);
            pwLine[sizeof(pwLine) - 1] = '\0';
        }
        canvas.drawString(pwLine, centerX, boxY + 48);
        return;
    }

    // Try to parse .22000 file for HS details
    // Build path to the .22000 file
    const char* scanRoot = (scanBaseDir[0] != '\0') ? scanBaseDir : SDLayout::handshakesDir();
    char hsPath[80];
    // Get base from filename
    const char* dot = strrchr(cap.filename, '.');
    size_t baseLen = dot ? (size_t)(dot - cap.filename) : strlen(cap.filename);
    // Strip _hs if present
    bool hasHsSuffix = (baseLen > 3 && strncmp(cap.filename + baseLen - 3, "_hs", 3) == 0);

    if (cap.isPMKID) {
        // PMKID: filename is already .22000
        snprintf(hsPath, sizeof(hsPath), "%s/%s", scanRoot, cap.filename);
    } else if (hasHsSuffix) {
        // _hs.22000 file
        snprintf(hsPath, sizeof(hsPath), "%s/%s", scanRoot, cap.filename);
    } else {
        // .pcap — try corresponding _hs.22000
        snprintf(hsPath, sizeof(hsPath), "%s/%.*s_hs.22000",
                 scanRoot, (int)baseLen, cap.filename);
    }

    // Cache: only parse once per detail view open
    static HSDetail cachedDetail;
    static char cachedFilename[48] = "";
    if (strcmp(cachedFilename, cap.filename) != 0) {
        memset(&cachedDetail, 0, sizeof(cachedDetail));
        strncpy(cachedFilename, cap.filename, sizeof(cachedFilename) - 1);

        if (SD.exists(hsPath)) {
            File f = SD.open(hsPath, FILE_READ);
            if (f) {
                char lineBuf[512];
                int n = f.readBytesUntil('\n', lineBuf, sizeof(lineBuf) - 1);
                lineBuf[n] = '\0';
                f.close();
                parseHS22000Line(lineBuf, &cachedDetail);
            }
        }
    }

    if (cachedDetail.valid) {
        if (cachedDetail.type == 2) {
            // 4-way handshake — msgPair 0x00=M1+M2, 0x02=M2+M3
            char typeLine[24];
            snprintf(typeLine, sizeof(typeLine), "4-WAY HS (%s)",
                     cachedDetail.msgPair == 0x02 ? "M2+M3" : "M1+M2");
            canvas.drawString(typeLine, centerX, boxY + 30);
            char anLine[24];
            snprintf(anLine, sizeof(anLine), "AN: %s", cachedDetail.anonce);
            canvas.drawString(anLine, centerX, boxY + 42);
            char clLine[24];
            snprintf(clLine, sizeof(clLine), "CL: %s", cachedDetail.clientMac);
            canvas.drawString(clLine, centerX, boxY + 54);
        } else if (cachedDetail.type == 1) {
            // PMKID
            canvas.drawString("PMKID CAPTURE", centerX, boxY + 30);
            char clLine[24];
            snprintf(clLine, sizeof(clLine), "CL: %s", cachedDetail.clientMac);
            canvas.drawString(clLine, centerX, boxY + 42);
            canvas.drawString("hashcat -m 22000", centerX, boxY + 54);
        }
    } else {
        // Fallback if no .22000 parseable
        if (cap.status == CaptureStatus::UPLOADED) {
            canvas.drawString("UPLOADED - PENDING CRACK", centerX, boxY + 34);
            canvas.drawString("PRESS [S] TO CHECK", centerX, boxY + 50);
        } else if (cap.isPMKID) {
            canvas.drawString("PMKID - LOCAL CRACK ONLY", centerX, boxY + 34);
            canvas.drawString("hashcat -m 22000", centerX, boxY + 50);
        } else {
            canvas.drawString("NOT UPLOADED YET", centerX, boxY + 34);
            canvas.drawString("PRESS [S] TO SYNC", centerX, boxY + 50);
        }
    }
}

// ============================================================================
// WPA-SEC Sync Operations
// ============================================================================

void HashesMenu::onSyncProgress(const char* status, uint8_t progress, uint8_t total) {
    // Update sync state for UI
    strncpy(syncStatusText, status, sizeof(syncStatusText) - 1);
    syncStatusText[sizeof(syncStatusText) - 1] = '\0';
    syncProgress = progress;
    syncTotal = total;
}

bool HashesMenu::connectToWiFi() {
    const char* ssid = Config::wifi().otaSSID;
    const char* password = Config::wifi().otaPassword;
    
    if (!ssid || ssid[0] == '\0') {
        strncpy(syncError, "NO WIFI SSID CONFIG", sizeof(syncError) - 1);
        return false;
    }
    
    Serial.printf("[HASHES] Connecting to WiFi: %s\n", ssid);
    strncpy(syncStatusText, "CONNECTING WIFI...", sizeof(syncStatusText) - 1);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    unsigned long startTime = millis();
    const unsigned long timeout = 15000;  // 15 second timeout
    
    while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < timeout) {
        delay(100);
        yield();
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        strncpy(syncError, "WIFI CONNECT FAILED", sizeof(syncError) - 1);
        // Keep driver alive to avoid esp_wifi_init 257 on fragmented heap.
        WiFiUtils::shutdown();
        return false;
    }
    
    Serial.printf("[HASHES] WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
}

void HashesMenu::disconnectWiFi() {
    // Keep driver alive to avoid esp_wifi_init 257 on fragmented heap.
    WiFiUtils::shutdown();
    Serial.println("[HASHES] WiFi disconnected");
}

void HashesMenu::startSync() {
    Serial.println("[HASHES] Starting WPA-SEC sync...");
    
    // Reset sync state
    syncModalActive = true;
    syncState = SyncState::CONNECTING_WIFI;
    syncStatusText[0] = '\0';
    syncError[0] = '\0';
    syncProgress = 0;
    syncTotal = 0;
    syncUploaded = 0;
    syncFailed = 0;
    syncCracked = 0;
    syncStartTime = millis();
    
    // Pre-flight checks
    if (!WPASec::hasApiKey()) {
        strncpy(syncError, "NO WPA-SEC KEY", sizeof(syncError) - 1);
        syncState = SyncState::ERROR;
        return;
    }
    
    // Free memory before heavy operations
    captures.clear();
    captures.shrink_to_fit();
    WPASec::freeCacheMemory();
    
    Serial.printf("[HASHES] Heap after freeing: %u\n", (unsigned int)ESP.getFreeHeap());
}

void HashesMenu::cancelSync() {
    Serial.println("[HASHES] Sync cancelled");
    
    // Clean up
    disconnectWiFi();
    syncModalActive = false;
    syncState = SyncState::IDLE;
    
    // Rescan captures
    scanCaptures();
}

void HashesMenu::processSyncState() {
    if (!syncModalActive || syncState == SyncState::IDLE) {
        return;
    }
    
    switch (syncState) {
        case SyncState::CONNECTING_WIFI:
            strncpy(syncStatusText, "CONNECTING WIFI...", sizeof(syncStatusText) - 1);
            if (connectToWiFi()) {
                syncState = SyncState::FREEING_MEMORY;
            } else {
                syncState = SyncState::ERROR;
            }
            break;
            
        case SyncState::FREEING_MEMORY:
            strncpy(syncStatusText, "PREPARING...", sizeof(syncStatusText) - 1);
            // Defer heap gating to WPASec::syncCaptures() so conditioning can run.
            syncState = SyncState::UPLOADING;
            break;
            
        case SyncState::UPLOADING:
            {
                // Run sync (blocking but with progress callback)
                strncpy(syncStatusText, "SYNCING...", sizeof(syncStatusText) - 1);
                
                WPASecSyncResult result = WPASec::syncCaptures(onSyncProgress);
                
                syncUploaded = result.uploaded;
                syncFailed = result.failed;
                syncCracked = result.cracked;
                
                if (result.error[0] != '\0') {
                    strncpy(syncError, result.error, sizeof(syncError) - 1);
                }
                
                syncState = SyncState::COMPLETE;
            }
            break;
            
        case SyncState::DOWNLOADING_POTFILE:
            // Handled within UPLOADING state via syncCaptures
            break;
            
        case SyncState::COMPLETE:
            // Stay in complete state until user dismisses
            disconnectWiFi();
            break;
            
        case SyncState::ERROR:
            // Stay in error state until user dismisses
            disconnectWiFi();
            break;
            
        default:
            break;
    }
}

void HashesMenu::drawSyncModal(M5Canvas& canvas) {
    // Modal box dimensions
    const int boxW = 200;
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
    
    // Title
    canvas.drawString("WPA-SEC SYNC", centerX, boxY + 6);
    
    if (syncState == SyncState::ERROR) {
        // Error state
        canvas.drawString("!! ERROR !!", centerX, boxY + 24);
        canvas.drawString(syncError, centerX, boxY + 42);
        canvas.drawString("[ENTER] CLOSE", centerX, boxY + 68);
    } else if (syncState == SyncState::COMPLETE) {
        // Complete state
        canvas.drawString("SYNC COMPLETE", centerX, boxY + 24);
        
        char stats[48];
        snprintf(stats, sizeof(stats), "UP:%u FAIL:%u CRACK:%u", 
                 (unsigned)syncUploaded, (unsigned)syncFailed, (unsigned)syncCracked);
        canvas.drawString(stats, centerX, boxY + 42);
        
        if (syncError[0] != '\0') {
            canvas.drawString(syncError, centerX, boxY + 54);
        }
        
        canvas.drawString("[ENTER] CLOSE", centerX, boxY + 68);
    } else {
        // In progress
        canvas.drawString(syncStatusText, centerX, boxY + 24);
        
        // Progress bar
        if (syncTotal > 0) {
            const int barW = 160;
            const int barH = 10;
            const int barX = boxX + (boxW - barW) / 2;
            const int barY = boxY + 42;
            
            // Background
            canvas.fillRect(barX, barY, barW, barH, COLOR_BG);
            
            // Fill
            int fillW = (barW * syncProgress) / syncTotal;
            if (fillW > 0) {
                canvas.fillRect(barX, barY, fillW, barH, COLOR_FG);
            }
            
            // Progress text
            char progText[16];
            snprintf(progText, sizeof(progText), "%u/%u", (unsigned)syncProgress, (unsigned)syncTotal);
            canvas.drawString(progText, centerX, barY + barH + 4);
        } else {
            // Heap display
            char heapText[32];
            snprintf(heapText, sizeof(heapText), "HEAP: %uKB",
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024));
            canvas.drawString(heapText, centerX, boxY + 42);
        }
        
        canvas.drawString("[ESC] CANCEL", centerX, boxY + 68);
    }
}
