// HOGWASH Mode Implementation - Karma AP
// Responds to probe requests with matching SSIDs to lure devices
//
// Technical notes:
// - Uses ESP32 promiscuous mode to capture probe requests
// - Runs soft AP with dynamically changing SSID
// - Tracks connected stations for XP awards
//
// Achievement storage note:
// HOGWASH achievements use the second uint64_t bitfield (achievements2)
// This was chosen over __uint128_t because:
// - Native 64-bit operations on ESP32 (no software emulation)
// - Easy NVS storage (two putUInt calls, existing pattern)
// - Backward compatible with existing save format

#include "hogwash.h"
#include "../core/config.h"
#include "../core/xp.h"
#include "../core/sdlog.h"
#include "../ui/display.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <SD.h>

// DNS and HTTP servers for captive portal
static DNSServer dnsServer;
static WebServer webServer(80);

// Static member initialization
bool HogwashMode::running = false;
bool HogwashMode::confirmed = false;
bool HogwashMode::portalEnabled = false;
bool HogwashMode::portalRunning = false;
String HogwashMode::portalHTML = "";
SSIDEntry HogwashMode::ssidQueue[HOGWASH_SSID_QUEUE_SIZE] = {0};
uint8_t HogwashMode::ssidQueueHead = 0;
uint8_t HogwashMode::ssidQueueCount = 0;
char HogwashMode::currentSSID[33] = {0};
uint32_t HogwashMode::lastSSIDChange = 0;
uint16_t HogwashMode::ssidCycleIntervalMs = 5000;  // 5 seconds per SSID
std::vector<HookedStation> HogwashMode::hookedStations;
uint8_t HogwashMode::hookedCount = 0;
uint8_t HogwashMode::appleHookCount = 0;
uint32_t HogwashMode::probeCount = 0;
uint32_t HogwashMode::uniqueSSIDCount = 0;
uint32_t HogwashMode::sessionStartTime = 0;
uint16_t HogwashMode::sessionProbeXP = 0;
bool HogwashMode::probeXPCapWarned = false;
uint8_t HogwashMode::channel = 6;

// Deferred XP queue (callback runs in WiFi task, XP must be granted in main loop)
static volatile bool pendingNewSSID = false;
static char pendingSSID[33] = {0};

// Pig phrases for HOGWASH mode
static const char* HOGWASH_PHRASES_IDLE[] = {
    "come to papa...",
    "here piggy piggy...",
    "the wifi is free...",
    "trust me bro",
    "totally legit network"
};
static const uint8_t HOGWASH_PHRASES_IDLE_COUNT = 5;

static const char* HOGWASH_PHRASES_HOOK[] = {
    "GOTCHA!",
    "welcome to the farm",
    "another one",
    "yoink",
    "GET OVER HERE"
};
static const uint8_t HOGWASH_PHRASES_HOOK_COUNT = 5;

void HogwashMode::init() {
    running = false;
    confirmed = false;
    memset(ssidQueue, 0, sizeof(ssidQueue));
    ssidQueueHead = 0;
    ssidQueueCount = 0;
    memset(currentSSID, 0, sizeof(currentSSID));
    hookedStations.clear();
    hookedCount = 0;
    appleHookCount = 0;
    probeCount = 0;
    uniqueSSIDCount = 0;
    sessionStartTime = 0;
    sessionProbeXP = 0;
    probeXPCapWarned = false;
}

void HogwashMode::start() {
    if (running) return;
    
    // Show warning dialog (same pattern as PIGGYBLUES)
    if (!confirmed) {
        if (!showWarningDialog()) {
            return;  // User cancelled
        }
        confirmed = true;
    }
    
    // Reset session stats
    memset(ssidQueue, 0, sizeof(ssidQueue));
    ssidQueueHead = 0;
    ssidQueueCount = 0;
    hookedStations.clear();
    hookedCount = 0;
    appleHookCount = 0;
    probeCount = 0;
    uniqueSSIDCount = 0;
    sessionStartTime = millis();
    sessionProbeXP = 0;
    probeXPCapWarned = false;
    
    // Load SSID cycle interval from config
    ssidCycleIntervalMs = Config::wifi().hogwashSSIDCycleMs;
    
    // Start probe monitoring
    startProbeMonitor();
    
    // Initial SSID (will be updated when probes arrive)
    strcpy(currentSSID, "FreeWiFi");
    
    // Start soft AP
    startSoftAP();
    
    // Set station inactivity timeout to 10 seconds (default is 300s!)
    // This makes ESP32 detect client disconnections much faster
    esp_wifi_set_inactive_time(WIFI_IF_AP, 10);
    
    // Start captive portal if enabled in settings
    portalEnabled = Config::wifi().hogwashCaptivePortal;
    if (portalEnabled) {
        startCaptivePortal();
    }
    
    running = true;
    
    // Set pig mood - DEVIOUS for the scheming karma AP pig
    Avatar::setState(AvatarState::DEVIOUS);
    Display::showToast(portalEnabled ? "KARMA+PORTAL" : "KARMA ACTIVE");
    
    SDLog::log("HOG", "HOGWASH mode started on channel %d (portal: %s)", channel, portalEnabled ? "ON" : "OFF");
    Serial.println("[HOGWASH] Mode started");
}

void HogwashMode::stop() {
    if (!running) return;
    
    Serial.println("[HOGWASH] Stopping...");
    
    // Mark as not running first to stop callback processing
    running = false;
    
    // Stop probe monitoring (disables promiscuous mode)
    stopProbeMonitor();
    
    // Small delay before AP disconnect
    delay(50);
    
    // Stop captive portal if running
    if (portalRunning) {
        stopCaptivePortal();
    }
    
    // Stop soft AP
    stopSoftAP();
    
    // Process any pending XP save
    XP::processPendingSave();
    
    // Reset avatar state
    Avatar::setState(AvatarState::NEUTRAL);
    
    SDLog::log("HOG", "HOGWASH stopped: %lu probes, %d hooks", probeCount, hookedCount);
    Serial.printf("[HOGWASH] Stopped - Probes: %lu, Hooks: %d, Free heap: %lu\n", 
                  probeCount, hookedCount, (unsigned long)ESP.getFreeHeap());
}

void HogwashMode::update() {
    if (!running) return;
    
    uint32_t now = millis();
    
    // Process deferred XP grant from callback (callback runs in WiFi task)
    if (pendingNewSSID) {
        pendingNewSSID = false;
        XP::addXP(XPEvent::HOGWASH_PROBE_NEW);
        Serial.printf("[HOGWASH] XP granted for new SSID: %s\n", pendingSSID);
    }
    
    // Cycle SSID periodically (but NOT while clients are CURRENTLY connected)
    // Changing SSID disconnects all clients, so pause rotation to keep hooks alive
    uint8_t connectedStations = WiFi.softAPgetStationNum();
    
    if (connectedStations == 0 && now - lastSSIDChange > ssidCycleIntervalMs) {
        cycleToNextSSID();
        lastSSIDChange = now;
    }
    
    // Check for newly connected stations
    checkConnectedStations();
    
    // Award session time XP (every 5 minutes)
    uint32_t sessionMinutes = (now - sessionStartTime) / 60000;
    static uint32_t lastSessionXPMinute = 0;
    if (sessionMinutes >= 5 && sessionMinutes > lastSessionXPMinute && sessionMinutes % 5 == 0) {
        XP::addXP(XPEvent::HOGWASH_SESSION_5MIN);
        lastSessionXPMinute = sessionMinutes;
    }
    
    // Update mood with current stats
    static uint32_t lastMoodUpdate = 0;
    if (now - lastMoodUpdate > 3000) {  // Every 3 seconds
        Mood::onHogwashUpdate(currentSSID, hookedCount, probeCount);
        lastMoodUpdate = now;
    }
    
    // Process captive portal requests (DNS and HTTP)
    if (portalRunning) {
        handleCaptivePortal();
    }
}

bool HogwashMode::showWarningDialog() {
    // Warning dialog styled like PIGGYBLUES - pink box on black background
    M5Canvas& canvas = Display::getMain();
    
    // Dialog dimensions
    const uint16_t DIALOG_WIDTH = 200;
    const uint16_t DIALOG_HEIGHT = 70;
    const uint32_t DIALOG_TIMEOUT_MS = 5000;
    
    int boxW = DIALOG_WIDTH;
    int boxH = DIALOG_HEIGHT;
    int boxX = (DISPLAY_W - boxW) / 2;
    int boxY = (MAIN_H - boxH) / 2;
    
    uint32_t startTime = millis();
    uint32_t timeout = DIALOG_TIMEOUT_MS;
    
    while ((millis() - startTime) < timeout) {
        M5.update();
        M5Cardputer.update();
        
        uint32_t remaining = (timeout - (millis() - startTime)) / 1000 + 1;
        
        // Clear and redraw
        canvas.fillSprite(COLOR_BG);
        
        // Black border then pink fill
        canvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
        canvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);
        
        // Black text on pink background
        canvas.setTextColor(COLOR_BG, COLOR_FG);
        canvas.setTextDatum(top_center);
        canvas.setTextSize(1);
        canvas.setFont(&fonts::Font0);
        
        int centerX = DISPLAY_W / 2;
        canvas.drawString("!! WARNING !!", centerX, boxY + 8);
        canvas.drawString("KARMA AP FAKE NETWORKS", centerX, boxY + 22);
        canvas.drawString("EDUCATIONAL USE ONLY!", centerX, boxY + 36);
        
        char buf[24];
        snprintf(buf, sizeof(buf), "[Y] Yes  [`] No (%lu)", remaining);
        canvas.drawString(buf, centerX, boxY + 54);
        
        Display::pushAll();
        
        if (M5Cardputer.Keyboard.isChange()) {
            if (M5Cardputer.Keyboard.isKeyPressed('`')) {
                return false;
            }
            if (M5Cardputer.Keyboard.isKeyPressed('y') || M5Cardputer.Keyboard.isKeyPressed('Y')) {
                return true;
            }
        }
        
        delay(50);
    }
    
    // Timeout = abort
    return false;
}

void HogwashMode::startProbeMonitor() {
    // Disconnect any existing WiFi
    WiFi.disconnect();
    WiFi.mode(WIFI_MODE_NULL);
    delay(50);
    
    // Enable promiscuous mode
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_NULL);
    esp_wifi_start();
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(probeCallback);
    
    // Set to a common channel
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    
    Serial.printf("[HOGWASH] Probe monitor started on channel %d\n", channel);
}

void HogwashMode::stopProbeMonitor() {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
}

void HogwashMode::startSoftAP() {
    // Stop promiscuous mode temporarily
    // Note: ESP32 can run promiscuous mode and soft AP simultaneously on same channel
    
    // Start soft AP with current SSID
    WiFi.mode(WIFI_AP);
    WiFi.softAP(currentSSID, nullptr, channel, 0, 4);  // Open network, max 4 connections
    
    // Re-enable promiscuous mode
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(probeCallback);
    
    Serial.printf("[HOGWASH] Soft AP started: %s\n", currentSSID);
}

void HogwashMode::stopSoftAP() {
    // Disconnect stations first
    WiFi.softAPdisconnect(false);  // Don't turn off WiFi yet
    
    // Small delay to let disconnect complete
    delay(100);
}

void HogwashMode::updateSoftAPSSID() {
    if (currentSSID[0] == '\0') return;
    
    // Change soft AP SSID (requires restart of AP)
    WiFi.softAPdisconnect(false);
    WiFi.softAP(currentSSID, nullptr, channel, 0, 4);
    
    Serial.printf("[HOGWASH] SSID changed to: %s\n", currentSSID);
}

void HogwashMode::checkConnectedStations() {
    wifi_sta_list_t stationList;
    esp_wifi_ap_get_sta_list(&stationList);
    
    // Check for new connections
    for (int i = 0; i < stationList.num; i++) {
        wifi_sta_info_t* sta = &stationList.sta[i];
        
        // Check if already tracked
        bool found = false;
        for (const auto& hooked : hookedStations) {
            if (memcmp(hooked.mac, sta->mac, 6) == 0) {
                found = true;
                break;
            }
        }
        
        if (!found) {
            // New connection!
            HookedStation newSta;
            memcpy(newSta.mac, sta->mac, 6);
            newSta.connectedAt = millis();
            newSta.isApple = (sta->mac[0] & 0x02) == 0 && 
                            (sta->mac[0] == 0xF0 || sta->mac[0] == 0xAC ||
                             sta->mac[0] == 0x70 || sta->mac[0] == 0x60);  // Common Apple OUIs
            
            hookedStations.push_back(newSta);
            hookedCount++;
            if (newSta.isApple) appleHookCount++;
            
            // XP award
            if (newSta.isApple) {
                XP::addXP(XPEvent::HOGWASH_APPLE_HOOK);
            } else {
                XP::addXP(XPEvent::HOGWASH_HOOK);
            }
            
            // First hook achievement
            if (!XP::hasAchievement2(HACH_F1RST_H00K)) {
                XP::unlockAchievement2(HACH_F1RST_H00K);
            }
            
            // H0N3Y P0T: 5 devices connected simultaneously
            if (stationList.num >= 5 && !XP::hasAchievement2(HACH_H0N3Y_P0T)) {
                XP::unlockAchievement2(HACH_H0N3Y_P0T);
            }
            
            // 4PPL3 P1CK3R: 10 Apple devices hooked lifetime
            if (appleHookCount >= 10 && !XP::hasAchievement2(HACH_4PPL3_P1CK3R)) {
                XP::unlockAchievement2(HACH_4PPL3_P1CK3R);
            }
            
            // Show hook phrase and celebratory sound
            Display::showToast(HOGWASH_PHRASES_HOOK[random(0, HOGWASH_PHRASES_HOOK_COUNT)]);
            
            // Celebratory beep for hooked device (similar to handshake capture)
            if (Config::personality().soundEnabled) {
                if (newSta.isApple) {
                    // Apple device = premium catch = triple beep
                    M5.Speaker.tone(1200, 80);
                    delay(100);
                    M5.Speaker.tone(1500, 80);
                    delay(100);
                    M5.Speaker.tone(1800, 120);
                } else {
                    // Regular hook = double ascending beep
                    M5.Speaker.tone(1200, 100);
                    delay(120);
                    M5.Speaker.tone(1600, 150);
                }
            }
            
            // Check if MAC is randomized (locally-administered bit)
            bool isRandomizedMAC = (sta->mac[0] & 0x02) != 0;
            
            Serial.printf("[HOGWASH] HOOKED! %02X:%02X:%02X:%02X:%02X:%02X%s%s\n",
                          sta->mac[0], sta->mac[1], sta->mac[2],
                          sta->mac[3], sta->mac[4], sta->mac[5],
                          newSta.isApple ? " (Apple)" : "",
                          isRandomizedMAC ? " [RandomMAC]" : "");
            
            SDLog::log("HOG", "HOOK: %02X:%02X:%02X:%02X:%02X:%02X%s",
                       sta->mac[0], sta->mac[1], sta->mac[2],
                       sta->mac[3], sta->mac[4], sta->mac[5],
                       isRandomizedMAC ? " [R]" : "");
        }
    }
}

bool HogwashMode::addSSIDToQueue(const char* ssid, uint32_t timestamp) {
    if (ssid == nullptr || ssid[0] == '\0') return false;
    if (strlen(ssid) > 32) return false;
    
    // Check for duplicate
    for (uint8_t i = 0; i < ssidQueueCount; i++) {
        uint8_t idx = (ssidQueueHead + i) % HOGWASH_SSID_QUEUE_SIZE;
        if (strcmp(ssidQueue[idx].ssid, ssid) == 0) {
            // Update existing
            ssidQueue[idx].timestamp = timestamp;
            ssidQueue[idx].probeCount++;
            return false;  // Duplicate
        }
    }
    
    // Add new entry
    uint8_t newIdx;
    if (ssidQueueCount < HOGWASH_SSID_QUEUE_SIZE) {
        newIdx = (ssidQueueHead + ssidQueueCount) % HOGWASH_SSID_QUEUE_SIZE;
        ssidQueueCount++;
    } else {
        // Queue full, overwrite oldest
        newIdx = ssidQueueHead;
        ssidQueueHead = (ssidQueueHead + 1) % HOGWASH_SSID_QUEUE_SIZE;
    }
    
    strncpy(ssidQueue[newIdx].ssid, ssid, 32);
    ssidQueue[newIdx].ssid[32] = '\0';
    ssidQueue[newIdx].timestamp = timestamp;
    ssidQueue[newIdx].probeCount = 1;
    
    uniqueSSIDCount++;
    return true;  // New entry
}

const char* HogwashMode::getLatestSSID() {
    if (ssidQueueCount == 0) return nullptr;
    uint8_t latestIdx = (ssidQueueHead + ssidQueueCount - 1) % HOGWASH_SSID_QUEUE_SIZE;
    return ssidQueue[latestIdx].ssid;
}

const char* HogwashMode::getLastProbeSSID() {
    return getLatestSSID();
}

void HogwashMode::cycleToNextSSID() {
    const char* nextSSID = getLatestSSID();
    if (nextSSID != nullptr && strcmp(nextSSID, currentSSID) != 0) {
        strncpy(currentSSID, nextSSID, 32);
        currentSSID[32] = '\0';
        updateSoftAPSSID();
    }
}

// Probe request frame type (IEEE 802.11)
#define WIFI_PKT_MGMT 0
#define SUBTYPE_PROBE_REQ 0x40

void HogwashMode::probeCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (!running) return;
    if (type != WIFI_PKT_MGMT) return;
    
    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    const uint8_t* frame = pkt->payload;
    uint16_t len = pkt->rx_ctrl.sig_len;
    
    if (len < 26) return;  // Too short
    
    // Check if probe request (type 0x40)
    uint8_t frameType = frame[0];
    if (frameType != SUBTYPE_PROBE_REQ) return;
    
    probeCount++;
    
    // Parse SSID from tagged parameters (offset 24)
    const uint8_t* tagPtr = frame + 24;
    const uint8_t* endPtr = frame + len;
    
    while (tagPtr + 2 <= endPtr) {
        uint8_t tagNum = tagPtr[0];
        uint8_t tagLen = tagPtr[1];
        
        if (tagPtr + 2 + tagLen > endPtr) break;
        
        if (tagNum == 0) {  // SSID tag
            if (tagLen == 0 || tagLen > 32) break;  // Broadcast or invalid
            
            char ssid[33] = {0};
            memcpy(ssid, tagPtr + 2, tagLen);
            ssid[tagLen] = '\0';
            
            // Validate printable
            bool valid = true;
            for (uint8_t i = 0; i < tagLen; i++) {
                if (ssid[i] < 32 || ssid[i] > 126) {
                    valid = false;
                    break;
                }
            }
            
            if (valid) {
                bool isNew = addSSIDToQueue(ssid, millis());
                
                if (isNew) {
                    // DEFERRED: Queue XP grant for main loop (callback runs in WiFi task)
                    // Direct XP::addXP() calls here don't work correctly
                    if (!pendingNewSSID) {
                        strncpy(pendingSSID, ssid, 32);
                        pendingSSID[32] = '\0';
                        pendingNewSSID = true;
                    }
                    
                    // Switch SSID only if no clients connected (would disconnect them)
                    uint8_t numStations = WiFi.softAPgetStationNum();
                    
                    if (numStations == 0) {
                        strncpy(currentSSID, ssid, 32);
                        currentSSID[32] = '\0';
                        updateSoftAPSSID();
                    }
                    // Note: New probes still queue for XP even when clients are connected
                }
            }
            break;
        }
        
        tagPtr += 2 + tagLen;
    }
}

void HogwashMode::stationEventHandler(void* arg, esp_event_base_t event_base,
                                       int32_t event_id, void* event_data) {
    // Handle station connect/disconnect events
    // Called from ESP-IDF event loop
}

// ============ CAPTIVE PORTAL IMPLEMENTATION ============

// Default HTML served if no custom file exists
static const char* DEFAULT_PORTAL_HTML = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Free WiFi</title>
    <style>
        body {
            background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
            color: #fff;
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            display: flex;
            flex-direction: column;
            align-items: center;
            justify-content: center;
            min-height: 100vh;
            margin: 0;
            text-align: center;
            padding: 20px;
            box-sizing: border-box;
        }
        .logo { font-size: 4rem; margin-bottom: 20px; }
        h1 { font-size: 2rem; margin: 0 0 10px 0; color: #ff6b9d; }
        p { font-size: 1rem; color: #aaa; margin: 5px 0; }
        .spinner {
            width: 40px;
            height: 40px;
            border: 4px solid #333;
            border-top: 4px solid #ff6b9d;
            border-radius: 50%;
            animation: spin 1s linear infinite;
            margin: 30px auto;
        }
        @keyframes spin {
            0% { transform: rotate(0deg); }
            100% { transform: rotate(360deg); }
        }
        .footer { margin-top: 40px; font-size: 0.8rem; color: #555; }
    </style>
</head>
<body>
    <div class="logo">🐷</div>
    <h1>Welcome to Free WiFi</h1>
    <p>Connecting you to the internet...</p>
    <div class="spinner"></div>
    <p>Please wait while we verify your connection.</p>
    <div class="footer">Powered by PORKCHOP 🥓</div>
</body>
</html>
)rawliteral";

void HogwashMode::loadPortalHTML() {
    // Try to load custom HTML from SD card
    portalHTML = "";
    
    if (SD.exists("/portal.html")) {
        File file = SD.open("/portal.html", FILE_READ);
        if (file) {
            portalHTML = file.readString();
            file.close();
            Serial.printf("[HOGWASH] Loaded custom portal.html (%d bytes)\n", portalHTML.length());
            SDLog::log("HOG", "Custom portal loaded: %d bytes", portalHTML.length());
        }
    }
    
    // Use default if no custom file
    if (portalHTML.length() == 0) {
        portalHTML = DEFAULT_PORTAL_HTML;
        Serial.println("[HOGWASH] Using default portal HTML");
    }
}

void HogwashMode::startCaptivePortal() {
    if (portalRunning) return;
    
    // Load HTML from SD or use default
    loadPortalHTML();
    
    // Start DNS server - redirect all domains to our IP
    IPAddress apIP = WiFi.softAPIP();
    dnsServer.start(53, "*", apIP);
    
    // HTTP handlers
    webServer.onNotFound([]() {
        webServer.send(200, "text/html", portalHTML);
    });
    
    // Captive portal detection endpoints
    webServer.on("/generate_204", []() {
        webServer.send(200, "text/html", portalHTML);
    });
    webServer.on("/gen_204", []() {
        webServer.send(200, "text/html", portalHTML);
    });
    webServer.on("/hotspot-detect.html", []() {
        webServer.send(200, "text/html", portalHTML);
    });
    webServer.on("/connecttest.txt", []() {
        webServer.send(200, "text/html", portalHTML);
    });
    webServer.on("/success.txt", []() {
        webServer.send(200, "text/html", portalHTML);
    });
    
    webServer.begin();
    portalRunning = true;
    
    Serial.printf("[HOGWASH] Captive portal started on %s\n", apIP.toString().c_str());
    SDLog::log("HOG", "Portal started: %s", apIP.toString().c_str());
}

void HogwashMode::stopCaptivePortal() {
    if (!portalRunning) return;
    
    webServer.stop();
    dnsServer.stop();
    portalRunning = false;
    portalHTML = "";  // Free memory
    
    Serial.println("[HOGWASH] Captive portal stopped");
}

void HogwashMode::handleCaptivePortal() {
    if (!portalRunning) return;
    
    dnsServer.processNextRequest();
    webServer.handleClient();
}
