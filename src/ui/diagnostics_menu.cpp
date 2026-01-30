// Diagnostics Menu - System status snapshot

#include "diagnostics_menu.h"
#include <M5Cardputer.h>
#include <SD.h>
#include <time.h>
#include <string.h>
#include "display.h"
#include "../core/config.h"
#include "../web/wpasec.h"
#include "../web/wigle.h"
#include "../core/sd_layout.h"
#include "../core/heap_health.h"
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>

// Static member initialization
bool DiagnosticsMenu::active = false;
bool DiagnosticsMenu::keyWasPressed = false;
uint16_t DiagnosticsMenu::cachedWpaCracked = 0;
uint16_t DiagnosticsMenu::cachedWigleUploaded = 0;
uint32_t DiagnosticsMenu::lastStatRefreshMs = 0;
uint32_t DiagnosticsMenu::statRefreshIntervalMs = 2000;  // tighter refresh interval

void DiagnosticsMenu::show() {
    active = true;
    keyWasPressed = true;  // Ignore the Enter that brought us here
    lastStatRefreshMs = 0; // force immediate refresh
}

void DiagnosticsMenu::hide() {
    active = false;
    // Release caches when leaving
    WPASec::freeCacheMemory();
    WiGLE::freeUploadedListMemory();
}

void DiagnosticsMenu::update() {
    if (!active) return;

    bool anyPressed = M5Cardputer.Keyboard.isPressed();

    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }

    if (keyWasPressed) return;
    keyWasPressed = true;

    auto keys = M5Cardputer.Keyboard.keysState();

    // Enter/S - save snapshot
    if (keys.enter || M5Cardputer.Keyboard.isKeyPressed('s') || M5Cardputer.Keyboard.isKeyPressed('S')) {
        saveSnapshot();
        Display::setTopBarMessage("DIAG SNAPSHOT SAVED", 3000);
        return;
    }

    // R key - reset WiFi stack
    if (M5Cardputer.Keyboard.isKeyPressed('r') || M5Cardputer.Keyboard.isKeyPressed('R')) {
        resetWiFi();
        Display::setTopBarMessage("WIFI RESET", 3000);
        return;
    }

    // H key - append quick heap log
    if (M5Cardputer.Keyboard.isKeyPressed('h') || M5Cardputer.Keyboard.isKeyPressed('H')) {
        logHeapSnapshot();
        Display::setTopBarMessage("HEAP LOGGED", 3000);
        return;
    }

    // G key - free caches / pseudo GC
    if (M5Cardputer.Keyboard.isKeyPressed('g') || M5Cardputer.Keyboard.isKeyPressed('G')) {
        collectGarbage();
        Display::setTopBarMessage("CACHE CLEARED", 3000);
        return;
    }

    // Periodically refresh stats (e.g., every 5 seconds)
    if (millis() - lastStatRefreshMs > statRefreshIntervalMs) {
        refreshStats();
    }

    // Backspace - go back to previous menu
    if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
        hide();
    }
}

void DiagnosticsMenu::saveSnapshot() {
    // Create a diagnostic snapshot with system information
    if (!SD.exists("/")) {
        Display::notify(NoticeKind::WARNING, "NO SD CARD");
        return;
    }

    // Generate filename with timestamp
    time_t now = time(nullptr);
    struct tm* timeinfo = localtime(&now);
    char filename[64];
    const char* diagDir = SDLayout::diagnosticsDir();
    const bool hasDiagDir = (strcmp(diagDir, "/") != 0);
    if (hasDiagDir && !SD.exists(diagDir)) {
        SD.mkdir(diagDir);
    }
    const char* sep = hasDiagDir ? "/" : "";
    snprintf(filename, sizeof(filename), "%s%sdiag_%04d%02d%02d_%02d%02d%02d.txt",
             diagDir, sep,
             timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
             timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);

    File file = SD.open(filename, FILE_WRITE);
    if (!file) {
        Display::notify(NoticeKind::WARNING, "SAVE FAILED");
        return;
    }

    // Write system diagnostics
    file.printf("=== PORKCHOP DIAGNOSTICS SNAPSHOT ===\n");
    file.printf("Timestamp: %04d-%02d-%02d %02d:%02d:%02d\n",
                timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
                timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
    file.printf("\n");

    // WiFi Status
    file.printf("WIFI STATUS:\n");
    file.printf("  Mode: %s\n", WiFi.getMode() == 0 ? "NULL" : WiFi.getMode() == 1 ? "STA" : WiFi.getMode() == 2 ? "AP" : "AP_STA");
    file.printf("  Status: %s\n", WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");
    if (WiFi.status() == WL_CONNECTED) {
        file.printf("  SSID: %s\n", WiFi.SSID().c_str());
        file.printf("  IP: %s\n", WiFi.localIP().toString().c_str());
        file.printf("  MAC: %s\n", WiFi.macAddress().c_str());
    }
    file.printf("\n");

    // Memory Status
    file.printf("MEMORY STATUS:\n");
    file.printf("  Free Heap: %u bytes\n", (unsigned int)ESP.getFreeHeap());
    file.printf("  Largest Block: %u bytes\n", (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    file.printf("  Min Free Heap: %u bytes\n", (unsigned int)ESP.getMinFreeHeap());
    if (psramFound()) {
        file.printf("  PSRAM Size: %u bytes\n", (unsigned int)ESP.getPsramSize());
        file.printf("  PSRAM Free: %u bytes\n", (unsigned int)ESP.getFreePsram());
    }
    file.printf("\n");

    // System Info
    file.printf("SYSTEM INFO:\n");
    file.printf("  SDK Version: %s\n", ESP.getSdkVersion());
    file.printf("  Chip Model: %s\n", ESP.getChipModel());
    file.printf("  Chip Cores: %d\n", ESP.getChipCores());
    file.printf("  CPU Freq: %d MHz\n", ESP.getCpuFreqMHz());
    file.printf("  Flash Size: %u MB\n", (unsigned int)(ESP.getFlashChipSize() / (1024 * 1024)));
    file.printf("\n");

    // Battery Status
    file.printf("POWER STATUS:\n");
    file.printf("  Battery Voltage: %.2f V\n", M5.Power.getBatteryVoltage() / 1000.0f);
    file.printf("  Battery Level: %d%%\n", M5.Power.getBatteryLevel());
    file.printf("  Is Charging: %s\n", M5.Power.isCharging() ? "YES" : "NO");
    file.printf("\n");

    file.close();
}

void DiagnosticsMenu::resetWiFi() {
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(100);
    WiFi.mode(WIFI_STA);
}

void DiagnosticsMenu::logHeapSnapshot() {
    if (!Config::isSDAvailable()) {
        Display::setTopBarMessage("NO SD CARD", 2000);
        return;
    }
    const char* heapPath = SDLayout::heapLogPath();
    const char* diagDir = SDLayout::diagnosticsDir();
    if (strcmp(diagDir, "/") != 0 && !SD.exists(diagDir)) {
        SD.mkdir(diagDir);
    }
    File f = SD.open(heapPath, FILE_APPEND);
    if (!f) {
        Display::setTopBarMessage("LOG FAILED", 2000);
        return;
    }
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    f.printf("%04d-%02d-%02d %02d:%02d:%02d free=%u largest=%u min=%u min_largest=%u hmin_free=%u\n",
             t ? t->tm_year + 1900 : 0,
             t ? t->tm_mon + 1 : 0,
             t ? t->tm_mday : 0,
             t ? t->tm_hour : 0,
             t ? t->tm_min : 0,
             t ? t->tm_sec : 0,
             (unsigned int)ESP.getFreeHeap(),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned int)ESP.getMinFreeHeap(),
             (unsigned int)HeapHealth::getMinLargest(),
             (unsigned int)HeapHealth::getMinFree());
    f.close();
}

void DiagnosticsMenu::collectGarbage() {
    // Free optional caches to claw back heap
    WPASec::freeCacheMemory();
    WiGLE::freeUploadedListMemory();
    delay(200);
    yield();
}

void DiagnosticsMenu::refreshStats() {
    // Skip refresh if network ops are busy to avoid heap churn
    if (!WPASec::isBusy()) {
        cachedWpaCracked = WPASec::getCrackedCount();
    }
    if (!WiGLE::isBusy()) {
        cachedWigleUploaded = WiGLE::getUploadedCount();
    }
    WPASec::freeCacheMemory();
    WiGLE::freeUploadedListMemory();
    lastStatRefreshMs = millis();
}

void DiagnosticsMenu::draw(M5Canvas& canvas) {
    if (!active) return;

    canvas.fillSprite(COLOR_BG);
    canvas.setTextColor(COLOR_FG);
    canvas.setTextSize(1);

    int y = 2;
    int lineH = 14;

    // Heap
    canvas.drawString("HEAP:", 4, y);
    char heapBuf[16];
    snprintf(heapBuf, sizeof(heapBuf), "%u", (unsigned)ESP.getFreeHeap());
    canvas.drawString(heapBuf, 80, y);
    y += lineH;
    canvas.drawString("LARGEST:", 4, y);
    snprintf(heapBuf, sizeof(heapBuf), "%u", (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    canvas.drawString(heapBuf, 80, y);
    y += lineH;
    canvas.drawString("MIN FREE:", 4, y);
    snprintf(heapBuf, sizeof(heapBuf), "%u", (unsigned)ESP.getMinFreeHeap());
    canvas.drawString(heapBuf, 80, y);
    y += lineH;
    canvas.drawString("MIN LRG:", 4, y);
    snprintf(heapBuf, sizeof(heapBuf), "%u", (unsigned)HeapHealth::getMinLargest());
    canvas.drawString(heapBuf, 80, y);
    y += lineH + 4;

    // PSRAM (if present)
    if (psramFound()) {
        canvas.drawString("PSRAM:", 4, y);
        char ps[32];
        snprintf(ps, sizeof(ps), "%u/%u", (unsigned int)ESP.getFreePsram(), (unsigned int)ESP.getPsramSize());
        canvas.drawString(ps, 80, y);
        y += lineH;
    }

    // WiFi
    bool wifiUp = WiFi.status() == WL_CONNECTED;
    canvas.drawString("WIFI:", 4, y);
    canvas.drawString(wifiUp ? "CONNECTED" : "DISCONNECTED", 80, y);
    y += lineH;
    canvas.drawString("SSID:", 4, y);
    char ssidBuf[33];
    strncpy(ssidBuf, "-", sizeof(ssidBuf) - 1);
    ssidBuf[sizeof(ssidBuf) - 1] = '\0';
    if (wifiUp) {
        wifi_config_t conf;
        if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK && conf.sta.ssid[0]) {
            strncpy(ssidBuf, reinterpret_cast<const char*>(conf.sta.ssid), sizeof(ssidBuf) - 1);
            ssidBuf[sizeof(ssidBuf) - 1] = '\0';
        }
    }
    canvas.drawString(ssidBuf, 80, y);
    y += lineH;
    canvas.drawString("IP:", 4, y);
    char ipBuf[20];
    if (wifiUp) {
        IPAddress ip = WiFi.localIP();
        snprintf(ipBuf, sizeof(ipBuf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    } else {
        strncpy(ipBuf, "-", sizeof(ipBuf) - 1);
        ipBuf[sizeof(ipBuf) - 1] = '\0';
    }
    canvas.drawString(ipBuf, 80, y);
    y += lineH + 4;

    // SD status / size
    canvas.drawString("SD:", 4, y);
    if (Config::isSDAvailable()) {
        uint64_t cardSize = SD.cardSize();
        uint64_t cardFree = SD.totalBytes() > SD.usedBytes() ? SD.totalBytes() - SD.usedBytes() : 0;
        uint32_t mb = (uint32_t)(cardSize / (1024ULL * 1024ULL));
        uint32_t freeMb = (uint32_t)(cardFree / (1024ULL * 1024ULL));
        char sdLine[24];
        snprintf(sdLine, sizeof(sdLine), "%u/%uMB", freeMb, mb);
        canvas.drawString(sdLine, 80, y);
    } else {
        canvas.drawString("MISSING", 80, y);
    }
    y += lineH + 4;

    // Caches / uploads
    canvas.drawString("WPA-SEC:", 4, y);
    char cacheBuf[24];
    snprintf(cacheBuf, sizeof(cacheBuf), "%u CRACKED", (unsigned)cachedWpaCracked);
    canvas.drawString(cacheBuf, 80, y);
    y += lineH;
    canvas.drawString("WIGLE:", 4, y);
    snprintf(cacheBuf, sizeof(cacheBuf), "%u UPLOADED", (unsigned)cachedWigleUploaded);
    canvas.drawString(cacheBuf, 80, y);
    y += lineH + 6;

    // Power
    canvas.drawString("BATT:", 4, y);
    char batt[32];
    snprintf(batt, sizeof(batt), "%d%% (%.2fV)", M5.Power.getBatteryLevel(), M5.Power.getBatteryVoltage() / 1000.0f);
    canvas.drawString(batt, 80, y);
    y += lineH;
    canvas.drawString("CHARGING:", 4, y);
    canvas.drawString(M5.Power.isCharging() ? "YES" : "NO", 80, y);
    y += lineH + 6;

    // SD status
    canvas.drawString("SD:", 4, y);
    canvas.drawString(Config::isSDAvailable() ? "OK" : "MISSING", 80, y);
    y += lineH + 6;

    // Controls (compressed)
    canvas.drawString("[ENT]SAVE [R]WIFI", 4, y);
    y += lineH;
    canvas.drawString("[H]HEAP [G]GC [BKSPC]BACK", 4, y);
}
