// Tracks Menu - View wardriving files with sync support

#include "tracks_menu.h"
#include <M5Cardputer.h>
#include <SD.h>
#include <WiFi.h>
#include <string.h>
#include "display.h"
#include "../core/tls.h"
#include "../web/wigle.h"
#include "../core/config.h"
#include "../core/sd_layout.h"
#include "../core/wifi_utils.h"
#include "../core/network_recon.h"
#include "../core/heap_health.h"
#include <esp_heap_caps.h>

// Static member initialization
std::vector<WigleFileInfo> TracksMenu::files;
uint8_t TracksMenu::selectedIndex = 0;
uint8_t TracksMenu::scrollOffset = 0;
bool TracksMenu::active = false;
bool TracksMenu::keyWasPressed = false;
bool TracksMenu::detailViewActive = false;
bool TracksMenu::nukeConfirmActive = false;
bool TracksMenu::scanInProgress = false;
bool TracksMenu::scanDeferredHeap = false;
unsigned long TracksMenu::lastScanTime = 0;
char TracksMenu::scanBaseDir[32] = "";
File TracksMenu::scanDir;
File TracksMenu::currentFile;
bool TracksMenu::scanComplete = false;
size_t TracksMenu::scanProgress = 0;

// Sync state
bool TracksMenu::syncModalActive = false;
WigleSyncState TracksMenu::syncState = WigleSyncState::IDLE;
char TracksMenu::syncStatusText[48] = "";
uint8_t TracksMenu::syncProgress = 0;
uint8_t TracksMenu::syncTotal = 0;
unsigned long TracksMenu::syncStartTime = 0;
uint8_t TracksMenu::syncUploaded = 0;
uint8_t TracksMenu::syncFailed = 0;
uint8_t TracksMenu::syncSkipped = 0;
bool TracksMenu::syncStatsFetched = false;
char TracksMenu::syncError[48] = "";
bool TracksMenu::reconWasRunning = false;

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

static bool dirHasWigleFiles(const char* dirPath) {
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
            if (endsWithIgnoreCase(name, ".wigle.csv")) {
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

static const char* resolveWigleScanDir() {
    const char* preferredDir = SDLayout::wardrivingDir();
    const char* fallbackDir = SDLayout::usingNewLayout() ? "/wardriving" : "/m5porkchop/wardriving";
    if (strcmp(preferredDir, fallbackDir) == 0) return preferredDir;

    const bool preferredHasFiles = dirHasWigleFiles(preferredDir);
    const bool fallbackHasFiles = dirHasWigleFiles(fallbackDir);
    if (!preferredHasFiles && fallbackHasFiles) {
        return fallbackDir;
    }

    if (SD.exists(preferredDir)) return preferredDir;
    if (SD.exists(fallbackDir)) return fallbackDir;
    return preferredDir;
}

} // namespace

static void formatDisplayName(const char* filename, char* out, size_t len, size_t maxChars,
                              const char* ellipsis, bool stripDecorators) {
    if (!out || len == 0) return;
    out[0] = '\0';

    const char* name = filename;
    size_t total = strlen(name);
    size_t start = 0;
    size_t end = total;

    if (stripDecorators) {
        if (total >= 7 && strncmp(name, "warhog_", 7) == 0) start = 7;
        const char* suffix = ".wigle.csv";
        const size_t suffixLen = 10;
        if (total >= suffixLen && endsWithIgnoreCase(name, suffix)) {
            end = total - suffixLen;
        }
    }

    if (end < start) end = start;
    size_t avail = end - start;
    if (avail == 0) return;

    size_t limit = maxChars;
    if (limit >= len) limit = len - 1;
    if (limit == 0) return;

    size_t ellLen = (ellipsis ? strlen(ellipsis) : 0);
    bool truncated = avail > limit;
    size_t copyLen = avail;
    if (truncated) {
        copyLen = (limit > ellLen) ? (limit - ellLen) : limit;
    }
    if (copyLen >= len) copyLen = len - 1;
    memcpy(out, name + start, copyLen);
    if (truncated && ellipsis && ellLen > 0 && (copyLen + ellLen) < len) {
        memcpy(out + copyLen, ellipsis, ellLen);
        out[copyLen + ellLen] = '\0';
    } else {
        out[copyLen] = '\0';
    }
}

void TracksMenu::init() {
    files.clear();
    selectedIndex = 0;
    scrollOffset = 0;
    scanDeferredHeap = false;
}

void TracksMenu::show() {
    active = true;
    selectedIndex = 0;
    scrollOffset = 0;
    detailViewActive = false;
    nukeConfirmActive = false;
    syncModalActive = false;
    syncState = WigleSyncState::IDLE;
    keyWasPressed = true;  // Ignore enter that brought us here
    scanFiles();
}

void TracksMenu::hide() {
    active = false;
    detailViewActive = false;
    syncModalActive = false;
    files.clear();  // Release memory when not in menu
    files.shrink_to_fit();
    WiGLE::freeUploadedListMemory();
    scanDeferredHeap = false;
    scanBaseDir[0] = '\0';
}

void TracksMenu::scanFiles() {
    // Initialize async scan
    files.clear();
    files.reserve(8);  // Grow naturally — reserve(50) was 6.8KB contiguous
    scanDeferredHeap = false;
    
    if (!Config::isSDAvailable()) {
        Serial.println("[TRACKS] SD card not available");
        scanComplete = true;
        scanInProgress = false;
        return;
    }

    // Guard: Skip SD scan at Critical pressure — file listing only needs small FAT buffers
    if (HeapHealth::getPressureLevel() >= HeapPressureLevel::Critical) {
        Serial.println("[TRACKS] Scan deferred: heap pressure");
        scanDeferredHeap = true;
        scanComplete = true;
        scanInProgress = false;
        return;
    }
    
    const char* preferredDir = SDLayout::wardrivingDir();
    const char* wigleDir = resolveWigleScanDir();
    if (strcmp(wigleDir, preferredDir) != 0) {
        Serial.printf("[TRACKS] Using fallback scan dir: %s (preferred %s)\n",
                      wigleDir, preferredDir);
    }
    strncpy(scanBaseDir, wigleDir, sizeof(scanBaseDir) - 1);
    scanBaseDir[sizeof(scanBaseDir) - 1] = '\0';

    scanDir = SD.open(wigleDir);
    if (!scanDir || !scanDir.isDirectory()) {
        Serial.println("[TRACKS] Wardriving directory not found");
        scanComplete = true;
        scanInProgress = false;
        scanDir.close();
        return;
    }
    
    scanInProgress = true;
    scanComplete = false;
    scanProgress = 0;
    lastScanTime = millis();
}

void TracksMenu::processAsyncScan() {
    if (!scanInProgress || scanComplete) {
        return;
    }
    
    // Throttle the scan to avoid blocking the UI
    if (millis() - lastScanTime < SCAN_DELAY) {
        return;
    }
    
    lastScanTime = millis();
    
    // Process a chunk of files
    const char* scanRoot = (scanBaseDir[0] != '\0') ? scanBaseDir : SDLayout::wardrivingDir();
    size_t processed = 0;
    while (processed < SCAN_CHUNK_SIZE && !scanComplete) {
        currentFile = scanDir.openNextFile();
        
        if (!currentFile) {
            // No more files, we're done
            scanComplete = true;
            scanInProgress = false;
            scanDir.close();
            
            // Sort by filename (newest first - filenames include timestamp)
            std::sort(files.begin(), files.end(), [](const WigleFileInfo& a, const WigleFileInfo& b) {
                return strcmp(a.filename, b.filename) > 0;
            });
            
            Serial.printf("[TRACKS] Async scan complete. Found %d WiGLE files\n", files.size());
            break;
        }
        
        if (!currentFile.isDirectory()) {
            const char* rawName = currentFile.name();
            const char* slash = strrchr(rawName, '/');
            const char* base = slash ? slash + 1 : rawName;
            // Only show WiGLE format files (*.wigle.csv)
            if (endsWithIgnoreCase(base, ".wigle.csv")) {
                WigleFileInfo info;
                memset(&info, 0, sizeof(info));
                strncpy(info.filename, base, sizeof(info.filename) - 1);
                info.fileSize = currentFile.size();
                // Estimate network count: ~150 bytes per line after header
                info.networkCount = info.fileSize > 300 ? (info.fileSize - 300) / 150 : 0;

                // Check upload status (reconstruct path on stack — no need to store 80B per entry)
                char tmpPath[80];
                snprintf(tmpPath, sizeof(tmpPath), "%s/%s", scanRoot, base);
                info.status = WiGLE::isUploaded(tmpPath) ?
                    WigleFileStatus::UPLOADED : WigleFileStatus::LOCAL;
                
                files.push_back(info);
                
                // Cap at 50 files to prevent memory issues
                if (files.size() >= 50) {
                    scanComplete = true;
                    scanInProgress = false;
                    currentFile.close();
                    scanDir.close();
                    break;
                }
            }
        }
        
        currentFile.close();
        processed++;
        scanProgress++;
        
        // Yield periodically to allow other tasks to run
        if (processed >= SCAN_CHUNK_SIZE) {
            // Still more to do, but yield control back to other tasks
            break;
        }
    }
}

void TracksMenu::handleInput() {
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
        if (syncState == WigleSyncState::ERROR || syncState == WigleSyncState::COMPLETE) {
            // Enter closes the modal after completion/error
            if (keys.enter || M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
                syncModalActive = false;
                syncState = WigleSyncState::IDLE;
                scanFiles();  // Rescan files after sync
                if (reconWasRunning) {
                    NetworkRecon::resume();
                    reconWasRunning = false;
                }
            }
        } else {
            // ESC cancels during sync
            if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
                cancelSync();
            }
        }
        return;  // Block other inputs during sync
    }
    
    // Handle detail view input - any key closes
    if (detailViewActive) {
        detailViewActive = false;
        return;
    }
    
    // Handle nuke confirmation modal
    if (nukeConfirmActive) {
        if (M5Cardputer.Keyboard.isKeyPressed('y') || M5Cardputer.Keyboard.isKeyPressed('Y')) {
            nukeTrack();
            nukeConfirmActive = false;
            Display::clearBottomOverlay();
            return;
        }
        if (M5Cardputer.Keyboard.isKeyPressed('n') || M5Cardputer.Keyboard.isKeyPressed('N') ||
            M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
            nukeConfirmActive = false;  // Cancel
            Display::clearBottomOverlay();
            return;
        }
        return;  // Ignore other keys when modal active
    }
    
    // Backspace - go back
    if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
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
    
    // S key triggers WiGLE sync
    if (M5Cardputer.Keyboard.isKeyPressed('s') || M5Cardputer.Keyboard.isKeyPressed('S')) {
        startSync();
    }
    
    // D key - nuke selected track
    if ((M5Cardputer.Keyboard.isKeyPressed('d') || M5Cardputer.Keyboard.isKeyPressed('D')) && !files.empty()) {
        if (selectedIndex < files.size()) {
            nukeConfirmActive = true;
            Display::setBottomOverlay("PERMANENT | NO UNDO");
        }
    }
}

void TracksMenu::formatSize(char* out, size_t len, uint32_t bytes) {
    if (!out || len == 0) return;
    if (bytes < 1024) {
        snprintf(out, len, "%uB", (unsigned)bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(out, len, "%uKB", (unsigned)(bytes / 1024));
    } else {
        snprintf(out, len, "%uMB", (unsigned)(bytes / (1024 * 1024)));
    }
}

void TracksMenu::getSelectedInfo(char* out, size_t len) {
    if (!out || len == 0) return;
    snprintf(out, len, "ENT=DET S=SYNC D=NUKE");
}

void TracksMenu::update() {
    if (!active) return;
    
    // Process sync state machine if active
    if (syncModalActive && syncState != WigleSyncState::IDLE && 
        syncState != WigleSyncState::COMPLETE && syncState != WigleSyncState::ERROR) {
        processSyncState();
    }
    
    // Process async file scanning if in progress (not during sync)
    if (!syncModalActive) {
        processAsyncScan();
    }
    
    handleInput();
}

void TracksMenu::draw(M5Canvas& canvas) {
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
    
    // Draw sync modal FIRST - takes precedence over empty files message
    if (syncModalActive) {
        drawSyncModal(canvas);
        return;
    }
    
    // Empty state
    if (files.empty()) {
        if (scanDeferredHeap) {
            canvas.setCursor(4, 36);
            canvas.print("SCAN DEFERRED");
            canvas.setCursor(4, 52);
            canvas.print("HEAP PRESSURE TOO HIGH");
            canvas.setCursor(4, 68);
            canvas.print("FREE MEMORY THEN RETRY");
        } else {
            canvas.setCursor(4, 36);
            canvas.print("NO WIGLE FILES");
            canvas.setCursor(4, 52);
            canvas.print("PRESS [W] FOR WARHOG");
            canvas.setCursor(4, 68);
            canvas.print("[S] TO SYNC");
        }
        return;
    }
    
    // Summary line
    uint16_t total = files.size();
    uint16_t uploaded = 0;
    uint32_t netSum = 0;
    for (const auto& file : files) {
        if (file.status == WigleFileStatus::UPLOADED) uploaded++;
        netSum += file.networkCount;
    }
    uint16_t local = total - uploaded;
    char summary[64];
    snprintf(summary, sizeof(summary), "WIGLE %u UP %u LOC %u NETS~%lu",
             (unsigned)total, (unsigned)uploaded, (unsigned)local, (unsigned long)netSum);
    canvas.setCursor(4, 2);
    canvas.print(summary);

    // Header row
    canvas.setCursor(4, 12);
    canvas.print("FILE");
    canvas.setCursor(105, 12);
    canvas.print("ST");
    canvas.setCursor(135, 12);
    canvas.print("NETS");
    canvas.setCursor(210, 12);
    canvas.print("SIZE");
    
    // File list (always drawn, modals overlay on top)
    int y = 22;
    int lineHeight = 16;
    
    for (uint8_t i = scrollOffset; i < files.size() && i < scrollOffset + VISIBLE_ITEMS; i++) {
        const WigleFileInfo& file = files[i];
        
        // Highlight selected
        if (i == selectedIndex) {
            canvas.fillRect(0, y - 1, canvas.width(), lineHeight, COLOR_FG);
            canvas.setTextColor(COLOR_BG);
        } else {
            canvas.setTextColor(COLOR_FG);
        }
        
        // Filename first (truncated) - extract just the date/time part
        char displayName[24];
        formatDisplayName(file.filename, displayName, sizeof(displayName), 15, "..", true);
        canvas.setCursor(4, y);
        canvas.print(displayName);
        
        // Status indicator (second column, matches LOOT menu)
        canvas.setCursor(105, y);
        if (file.status == WigleFileStatus::UPLOADED) {
            canvas.print("[OK]");
        } else {
            canvas.print("[--]");
        }
        
        // Network count and size
        canvas.setCursor(135, y);
        char sizeBuf[12];
        formatSize(sizeBuf, sizeof(sizeBuf), file.fileSize);
        canvas.printf("~%u", (unsigned)file.networkCount);
        
        canvas.setCursor(210, y);
        canvas.print(sizeBuf);
        
        y += lineHeight;
    }
    
    // Scroll indicators
    if (scrollOffset > 0) {
        canvas.setCursor(canvas.width() - 10, 22);
        canvas.setTextColor(COLOR_FG);
        canvas.print("^");
    }
    if (scrollOffset + VISIBLE_ITEMS < files.size()) {
        canvas.setCursor(canvas.width() - 10, 22 + (VISIBLE_ITEMS - 1) * lineHeight);
        canvas.setTextColor(COLOR_FG);
        canvas.print("v");
    }
    
    // Draw modals on top of list (matching captures_menu pattern)
    if (nukeConfirmActive) {
        drawNukeConfirm(canvas);
    }
    
    if (detailViewActive) {
        drawDetailView(canvas);
    }
    
    if (syncModalActive) {
        drawSyncModal(canvas);
    }
}

void TracksMenu::drawDetailView(M5Canvas& canvas) {
    if (files.empty() || selectedIndex >= files.size()) return;

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
    char displayName[32];
    formatDisplayName(file.filename, displayName, sizeof(displayName), 22, "...", false);
    canvas.drawString(displayName, boxX + boxW / 2, boxY + 8);
    
    // Stats
    char sizeBuf[12];
    formatSize(sizeBuf, sizeof(sizeBuf), file.fileSize);
    char statsBuf[64];
    snprintf(statsBuf, sizeof(statsBuf), "~%u networks, %s", (unsigned)file.networkCount, sizeBuf);
    canvas.drawString(statsBuf, boxX + boxW / 2, boxY + 24);
    
    // Status
    const char* statusText = (file.status == WigleFileStatus::UPLOADED) ? "UPLOADED" : "NOT UPLOADED";
    canvas.drawString(statusText, boxX + boxW / 2, boxY + 40);
    
    // Action hint
    canvas.drawString("PRESS [S] TO SYNC", boxX + boxW / 2, boxY + 56);
    
    canvas.setTextDatum(top_left);
}

void TracksMenu::drawNukeConfirm(M5Canvas& canvas) {
    if (files.empty() || selectedIndex >= files.size()) return;
    
    const WigleFileInfo& file = files[selectedIndex];
    
    // Modal box dimensions - matches other confirmation dialogs
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
    
    // Truncate filename for display
    canvas.drawString("!! NUKE THE TRACK !!", centerX, boxY + 8);
    char displayName[32];
    formatDisplayName(file.filename, displayName, sizeof(displayName), 22, "...", false);
    canvas.drawString(displayName, centerX, boxY + 24);
    canvas.drawString("THIS KILLS THE FILE.", centerX, boxY + 38);
    canvas.drawString("[Y] DO IT  [N] ABORT", centerX, boxY + 54);
    
    canvas.setTextDatum(top_left);
}

void TracksMenu::nukeTrack() {
    if (files.empty() || selectedIndex >= files.size()) return;
    
    const WigleFileInfo& file = files[selectedIndex];

    // Reconstruct full path from scanBaseDir + filename
    const char* scanRoot = (scanBaseDir[0] != '\0') ? scanBaseDir : SDLayout::wardrivingDir();
    char fullPath[80];
    snprintf(fullPath, sizeof(fullPath), "%s/%s", scanRoot, file.filename);

    Serial.printf("[TRACKS] Nuking track: %s\n", fullPath);

    // Delete the .wigle.csv file
    bool deleted = SD.remove(fullPath);

    // Also delete matching internal CSV if exists (same name without .wigle)
    char internalPath[80];
    strncpy(internalPath, fullPath, sizeof(internalPath) - 1);
    internalPath[sizeof(internalPath) - 1] = '\0';
    const size_t wigleSuffixLen = 10;
    size_t internalLen = strlen(internalPath);
    if (internalLen > wigleSuffixLen && endsWithIgnoreCase(internalPath, ".wigle.csv")) {
        internalPath[internalLen - wigleSuffixLen] = '\0';
        strncat(internalPath, ".csv", sizeof(internalPath) - strlen(internalPath) - 1);
        if (SD.exists(internalPath)) {
            SD.remove(internalPath);
            Serial.printf("[TRACKS] Also nuked: %s\n", internalPath);
        }
    }

    // Remove from uploaded tracking if present
    WiGLE::removeFromUploaded(fullPath);
    
    if (deleted) {
        Display::setTopBarMessage("TRACK NUKED!", 4000);
    } else {
        Display::setTopBarMessage("NUKE FAILED", 4000);
    }

    // Refresh the file list
    scanFiles();
    
    // Adjust selection if needed
    if (files.empty()) {
        selectedIndex = 0;
        scrollOffset = 0;
    } else if (selectedIndex >= files.size()) {
        selectedIndex = files.size() - 1;
    }
    if (scrollOffset > selectedIndex) {
        scrollOffset = selectedIndex;
    }
}

// ============================================================================
// WiGLE Sync Operations
// ============================================================================

void TracksMenu::onSyncProgress(const char* status, uint8_t progress, uint8_t total) {
    // Update sync state for UI
    strncpy(syncStatusText, status, sizeof(syncStatusText) - 1);
    syncStatusText[sizeof(syncStatusText) - 1] = '\0';
    syncProgress = progress;
    syncTotal = total;
}

bool TracksMenu::connectToWiFi() {
    const char* ssid = Config::wifi().otaSSID;
    const char* password = Config::wifi().otaPassword;
    
    if (!ssid || ssid[0] == '\0') {
        strncpy(syncError, "NO WIFI SSID CONFIG", sizeof(syncError) - 1);
        return false;
    }
    
    Serial.printf("[TRACKS] Connecting to WiFi: %s\n", ssid);
    strncpy(syncStatusText, "CONNECTING WIFI...", sizeof(syncStatusText) - 1);
    
    WiFiUtils::hardReset();
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
    
    Serial.printf("[TRACKS] WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
}

void TracksMenu::disconnectWiFi() {
    // Keep driver alive to avoid esp_wifi_init 257 on fragmented heap.
    WiFiUtils::shutdown();
    Serial.println("[TRACKS] WiFi disconnected");
}

void TracksMenu::startSync() {
    Serial.println("[TRACKS] Starting WiGLE sync...");
    
    // Reset sync state
    syncModalActive = true;
    syncState = WigleSyncState::CONNECTING_WIFI;
    syncStatusText[0] = '\0';
    syncError[0] = '\0';
    syncProgress = 0;
    syncTotal = 0;
    syncUploaded = 0;
    syncFailed = 0;
    syncSkipped = 0;
    syncStatsFetched = false;
    syncStartTime = millis();
    
    // Pre-flight checks
    if (!WiGLE::hasCredentials()) {
        strncpy(syncError, "NO WIGLE CREDENTIALS", sizeof(syncError) - 1);
        syncState = WigleSyncState::ERROR;
        return;
    }
    
    // Free memory before heavy operations
    files.clear();
    files.shrink_to_fit();
    WiGLE::freeUploadedListMemory();

    // Pause NetworkRecon before WiFi mode changes — promiscuous callbacks
    // crash if they fire during STA mode transition
    reconWasRunning = NetworkRecon::isRunning();
    if (reconWasRunning) {
        NetworkRecon::pause();
        NetworkRecon::freeNetworks();  // Release ~19KB for TLS headroom
    }

    Serial.printf("[TRACKS] Heap after freeing: %u\n", (unsigned int)ESP.getFreeHeap());
}

void TracksMenu::cancelSync() {
    Serial.println("[TRACKS] Sync cancelled");

    // Clean up
    disconnectWiFi();
    syncModalActive = false;
    syncState = WigleSyncState::IDLE;

    if (reconWasRunning) {
        NetworkRecon::resume();
        reconWasRunning = false;
    }

    // Rescan files
    scanFiles();
}

void TracksMenu::processSyncState() {
    if (!syncModalActive || syncState == WigleSyncState::IDLE) {
        return;
    }
    
    switch (syncState) {
        case WigleSyncState::CONNECTING_WIFI:
            strncpy(syncStatusText, "CONNECTING WIFI...", sizeof(syncStatusText) - 1);
            if (connectToWiFi()) {
                syncState = WigleSyncState::FREEING_MEMORY;
            } else {
                syncState = WigleSyncState::ERROR;
            }
            break;
            
        case WigleSyncState::FREEING_MEMORY:
            strncpy(syncStatusText, "PREPARING...", sizeof(syncStatusText) - 1);
            // Defer heap gating to WiGLE::syncFiles() so conditioning can run.
            syncState = WigleSyncState::UPLOADING;
            break;
            
        case WigleSyncState::UPLOADING:
            {
                // Run sync (blocking but with progress callback)
                strncpy(syncStatusText, "SYNCING...", sizeof(syncStatusText) - 1);

                // Lend the idle main-canvas buffer to mbedTLS as its allocation
                // arena for the blocking sync. The ~16KB TLS IN record buffer comes
                // from this static buffer instead of the heap, so the handshake fits
                // without freeing/re-allocating anything (no heap fragmentation).
                // Safe: the render loop is blocked here, so the canvas isn't drawn.
                Tls::arenaBegin(Display::mainCanvasBuffer(), Display::mainCanvasBufferSize());
                WigleSyncResult result = WiGLE::syncFiles(onSyncProgress);
                Tls::arenaEnd();

                syncUploaded = result.uploaded;
                syncFailed = result.failed;
                syncSkipped = result.skipped;
                syncStatsFetched = result.statsFetched;
                
                if (result.error[0] != '\0') {
                    strncpy(syncError, result.error, sizeof(syncError) - 1);
                }
                
                syncState = WigleSyncState::COMPLETE;
            }
            break;
            
        case WigleSyncState::FETCHING_STATS:
            // Handled within UPLOADING state via syncFiles
            break;
            
        case WigleSyncState::COMPLETE:
            // Stay in complete state until user dismisses
            disconnectWiFi();
            break;
            
        case WigleSyncState::ERROR:
            // Stay in error state until user dismisses
            disconnectWiFi();
            break;
            
        default:
            break;
    }
}

void TracksMenu::drawSyncModal(M5Canvas& canvas) {
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
    canvas.drawString("WIGLE SYNC", centerX, boxY + 6);
    
    if (syncState == WigleSyncState::ERROR) {
        // Error state
        canvas.drawString("!! ERROR !!", centerX, boxY + 24);
        canvas.drawString(syncError, centerX, boxY + 42);
        canvas.drawString("[ENTER] CLOSE", centerX, boxY + 68);
    } else if (syncState == WigleSyncState::COMPLETE) {
        // Complete state
        canvas.drawString("SYNC COMPLETE", centerX, boxY + 24);
        
        char stats[48];
        snprintf(stats, sizeof(stats), "UP:%u FAIL:%u SKIP:%u", 
                 (unsigned)syncUploaded, (unsigned)syncFailed, (unsigned)syncSkipped);
        canvas.drawString(stats, centerX, boxY + 42);
        
        // Stats status
        const char* statsMsg = syncStatsFetched ? "STATS UPDATED" : "STATS FAILED";
        canvas.drawString(statsMsg, centerX, boxY + 54);
        
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
    
    canvas.setTextDatum(top_left);
}
