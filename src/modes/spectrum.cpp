// HOG ON SPECTRUM Mode - WiFi Spectrum Analyzer Implementation

#include "spectrum.h"
#include "../core/config.h"
#include "../core/wsl_bypasser.h"
#include "../core/xp.h"
#include "../ui/display.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <algorithm>
#include <cmath>

// Layout constants - spectrum fills canvas above XP bar
const int SPECTRUM_LEFT = 20;       // Space for dB labels
const int SPECTRUM_RIGHT = 238;     // Right edge
const int SPECTRUM_TOP = 2;         // Top margin
const int SPECTRUM_BOTTOM = 75;     // Above channel labels
const int CHANNEL_LABEL_Y = 78;     // Channel number row
const int XP_BAR_Y = 91;            // XP bar starts here

// RSSI scale
const int8_t RSSI_MIN = -95;        // Bottom of scale (weak signals)
const int8_t RSSI_MAX = -30;        // Top of scale (very strong)

// View defaults
const float DEFAULT_CENTER_MHZ = 2437.0f;  // Channel 6
const float DEFAULT_WIDTH_MHZ = 60.0f;     // ~12 channels visible
const float MIN_CENTER_MHZ = 2412.0f;      // Channel 1
const float MAX_CENTER_MHZ = 2472.0f;      // Channel 13
const float PAN_STEP_MHZ = 5.0f;           // One channel per pan

// Timing
const uint32_t STALE_TIMEOUT_MS = 5000;    // Remove networks after 5s silence
const uint32_t UPDATE_INTERVAL_MS = 100;   // 10 FPS update rate

// Static members
bool SpectrumMode::running = false;
volatile bool SpectrumMode::busy = false;
std::vector<SpectrumNetwork> SpectrumMode::networks;
float SpectrumMode::viewCenterMHz = DEFAULT_CENTER_MHZ;
float SpectrumMode::viewWidthMHz = DEFAULT_WIDTH_MHZ;
int SpectrumMode::selectedIndex = -1;
uint32_t SpectrumMode::lastUpdateTime = 0;
bool SpectrumMode::keyWasPressed = false;
uint8_t SpectrumMode::currentChannel = 1;
uint32_t SpectrumMode::lastHopTime = 0;
uint32_t SpectrumMode::startTime = 0;
volatile bool SpectrumMode::pendingReveal = false;
char SpectrumMode::pendingRevealSSID[33] = {0};

void SpectrumMode::init() {
    networks.clear();
    viewCenterMHz = DEFAULT_CENTER_MHZ;
    viewWidthMHz = DEFAULT_WIDTH_MHZ;
    selectedIndex = -1;
    keyWasPressed = false;
    currentChannel = 1;
    lastHopTime = 0;
    startTime = 0;
    busy = false;
    pendingReveal = false;
    pendingRevealSSID[0] = 0;
}

void SpectrumMode::start() {
    if (running) return;
    
    Serial.println("[SPECTRUM] Starting HOG ON SPECTRUM mode...");
    
    init();
    
    // Initialize WiFi in promiscuous mode
    WiFi.mode(WIFI_STA);
    
    // Randomize MAC if enabled (stealth)
    if (Config::wifi().randomizeMAC) {
        WSLBypasser::randomizeMAC();
    }
    
    WiFi.disconnect();
    delay(100);
    
    // Set promiscuous callback and enable
    esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);
    esp_wifi_set_promiscuous(true);
    
    // Start on channel 1, will hop through all
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    
    running = true;
    lastUpdateTime = millis();
    startTime = millis();
    
    Display::setWiFiStatus(true);
    Serial.println("[SPECTRUM] Running - scanning all channels");
}

void SpectrumMode::stop() {
    if (!running) return;
    
    Serial.println("[SPECTRUM] Stopping...");
    
    esp_wifi_set_promiscuous(false);
    
    running = false;
    Display::setWiFiStatus(false);
    
    Serial.printf("[SPECTRUM] Stopped - tracked %d networks\n", networks.size());
}

void SpectrumMode::update() {
    if (!running) return;
    
    uint32_t now = millis();
    
    // Process deferred reveal logging (from callback)
    if (pendingReveal) {
        Serial.printf("[SPECTRUM] Hidden SSID revealed: %s\n", pendingRevealSSID);
        pendingReveal = false;
    }
    
    // Handle input
    handleInput();
    
    // Channel hopping - cycle through all channels for full spectrum view
    if (now - lastHopTime > 100) {  // 100ms per channel = ~1.3s full sweep
        currentChannel = (currentChannel % 13) + 1;
        esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
        lastHopTime = now;
    }
    
    // Prune stale networks periodically
    if (now - lastUpdateTime > UPDATE_INTERVAL_MS) {
        pruneStale();
        lastUpdateTime = now;
    }
    
    // N13TZSCH3 achievement - stare into the ether for 15 minutes
    if (startTime > 0 && (now - startTime) >= 15 * 60 * 1000) {
        if (!XP::hasAchievement(ACH_NIETZSWINE)) {
            XP::unlockAchievement(ACH_NIETZSWINE);
            Display::showToast("the ether deauths back");
        }
    }
}

void SpectrumMode::handleInput() {
    bool anyPressed = M5Cardputer.Keyboard.isPressed();
    
    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }
    
    if (keyWasPressed) return;
    keyWasPressed = true;
    
    Display::resetDimTimer();
    
    auto keys = M5Cardputer.Keyboard.keysState();
    
    // Pan spectrum with , (left) and / (right)
    if (M5Cardputer.Keyboard.isKeyPressed(',')) {
        viewCenterMHz = fmax(MIN_CENTER_MHZ, viewCenterMHz - PAN_STEP_MHZ);
    }
    if (M5Cardputer.Keyboard.isKeyPressed('/')) {
        viewCenterMHz = fmin(MAX_CENTER_MHZ, viewCenterMHz + PAN_STEP_MHZ);
    }
    
    // Cycle through networks with Enter
    if (keys.enter && !networks.empty()) {
        selectedIndex = (selectedIndex + 1) % (int)networks.size();
        // Center view on selected network
        if (selectedIndex >= 0 && selectedIndex < (int)networks.size()) {
            viewCenterMHz = channelToFreq(networks[selectedIndex].channel);
        }
    }
}

void SpectrumMode::draw(M5Canvas& canvas) {
    canvas.fillSprite(COLOR_BG);
    
    // Draw spectrum visualization
    drawAxis(canvas);
    drawSpectrum(canvas);
    drawChannelMarkers(canvas);
    
    // Draw status indicators if network is selected
    if (selectedIndex >= 0 && selectedIndex < (int)networks.size()) {
        const auto& net = networks[selectedIndex];
        canvas.setTextSize(1);
        canvas.setTextColor(COLOR_FG);
        canvas.setTextDatum(top_left);
        
        // Build status string: [VULN!] and/or [DEAUTH]
        String status = "";
        if (isVulnerable(net.authmode)) {
            status += "[VULN!]";
        }
        if (!net.hasPMF) {
            status += "[DEAUTH]";
        }
        if (status.length() > 0) {
            canvas.drawString(status, SPECTRUM_LEFT + 2, SPECTRUM_TOP);
        }
    }
    
    // Draw XP bar at bottom (y=91+)
    XP::drawBar(canvas);
}

void SpectrumMode::drawAxis(M5Canvas& canvas) {
    // Y-axis line
    canvas.drawFastVLine(SPECTRUM_LEFT - 2, SPECTRUM_TOP, SPECTRUM_BOTTOM - SPECTRUM_TOP, COLOR_FG);
    
    // dB labels on left
    canvas.setTextSize(1);
    canvas.setTextColor(COLOR_FG);
    canvas.setTextDatum(middle_right);
    
    for (int8_t rssi = -30; rssi >= -90; rssi -= 20) {
        int y = rssiToY(rssi);
        // Shift label down if it would be cut off by top bar (font height ~8px, so 4px minimum)
        int labelY = (y < 6) ? 6 : y;
        canvas.drawFastHLine(SPECTRUM_LEFT - 4, y, 3, COLOR_FG);
        canvas.drawString(String(rssi), SPECTRUM_LEFT - 5, labelY);
    }
    
    // Baseline
    canvas.drawFastHLine(SPECTRUM_LEFT, SPECTRUM_BOTTOM, SPECTRUM_RIGHT - SPECTRUM_LEFT, COLOR_FG);
}

void SpectrumMode::drawChannelMarkers(M5Canvas& canvas) {
    canvas.setTextSize(1);
    canvas.setTextColor(COLOR_FG);
    canvas.setTextDatum(top_center);
    
    // Draw channel numbers for visible channels
    for (uint8_t ch = 1; ch <= 13; ch++) {
        float freq = channelToFreq(ch);
        int x = freqToX(freq);
        
        // Only draw if in visible area
        if (x >= SPECTRUM_LEFT && x <= SPECTRUM_RIGHT) {
            // Tick mark
            canvas.drawFastVLine(x, SPECTRUM_BOTTOM, 3, COLOR_FG);
            // Channel number
            canvas.drawString(String(ch), x, CHANNEL_LABEL_Y);
        }
    }
    
    // Scroll indicators
    float leftEdge = viewCenterMHz - viewWidthMHz / 2;
    float rightEdge = viewCenterMHz + viewWidthMHz / 2;
    
    canvas.setTextDatum(middle_left);
    if (leftEdge > 2407) {  // More channels to the left
        canvas.drawString("<", 2, SPECTRUM_BOTTOM / 2);
    }
    canvas.setTextDatum(middle_right);
    if (rightEdge < 2477) {  // More channels to the right
        canvas.drawString(">", SPECTRUM_RIGHT + 1, SPECTRUM_BOTTOM / 2);
    }
}

void SpectrumMode::drawSpectrum(M5Canvas& canvas) {
    // Guard against callback modifying networks during copy
    busy = true;
    
    // Sort networks by RSSI (weakest first, so strongest draws on top)
    std::vector<SpectrumNetwork> sorted = networks;
    
    busy = false;
    
    std::sort(sorted.begin(), sorted.end(), [](const SpectrumNetwork& a, const SpectrumNetwork& b) {
        return a.rssi < b.rssi;
    });
    
    // Draw each network's Gaussian lobe
    for (size_t i = 0; i < sorted.size(); i++) {
        const auto& net = sorted[i];
        float freq = channelToFreq(net.channel);
        
        // Check if selected (compare by BSSID)
        bool isSelected = false;
        if (selectedIndex >= 0 && selectedIndex < (int)networks.size()) {
            isSelected = (memcmp(net.bssid, networks[selectedIndex].bssid, 6) == 0);
        }
        
        drawGaussianLobe(canvas, freq, net.rssi, isSelected);
    }
}

void SpectrumMode::drawGaussianLobe(M5Canvas& canvas, float centerFreqMHz, 
                                     int8_t rssi, bool filled) {
    // 2.4GHz WiFi channels are 22MHz wide
    // Gaussian sigma for -3dB at ±11MHz: sigma ≈ 6.6
    const float sigma = 6.6f;
    
    int peakY = rssiToY(rssi);
    int baseY = SPECTRUM_BOTTOM;
    
    // Don't draw if peak is below baseline
    if (peakY >= baseY) return;
    
    // Draw lobe from center-15MHz to center+15MHz
    int prevX = -1;
    int prevY = baseY;
    
    for (float freq = centerFreqMHz - 15; freq <= centerFreqMHz + 15; freq += 0.5f) {
        int x = freqToX(freq);
        
        // Skip if outside visible area
        if (x < SPECTRUM_LEFT || x > SPECTRUM_RIGHT) {
            prevX = x;
            prevY = baseY;
            continue;
        }
        
        // Gaussian amplitude falloff
        float dist = freq - centerFreqMHz;
        float amplitude = expf(-0.5f * (dist * dist) / (sigma * sigma));
        int y = baseY - (int)((baseY - peakY) * amplitude);
        
        if (prevX >= SPECTRUM_LEFT && prevX <= SPECTRUM_RIGHT) {
            if (filled) {
                // Filled lobe - draw vertical line from baseline to curve
                if (y < baseY) {
                    canvas.drawFastVLine(x, y, baseY - y, COLOR_FG);
                }
            } else {
                // Outline only - connect points
                canvas.drawLine(prevX, prevY, x, y, COLOR_FG);
            }
        }
        
        prevX = x;
        prevY = y;
    }
}

int SpectrumMode::freqToX(float freqMHz) {
    float leftFreq = viewCenterMHz - viewWidthMHz / 2;
    int width = SPECTRUM_RIGHT - SPECTRUM_LEFT;
    return SPECTRUM_LEFT + (int)((freqMHz - leftFreq) * width / viewWidthMHz);
}

int SpectrumMode::rssiToY(int8_t rssi) {
    // Clamp to range
    if (rssi < RSSI_MIN) rssi = RSSI_MIN;
    if (rssi > RSSI_MAX) rssi = RSSI_MAX;
    
    // Map RSSI to Y (inverted - stronger = higher on screen = lower Y)
    int height = SPECTRUM_BOTTOM - SPECTRUM_TOP;
    return SPECTRUM_BOTTOM - (int)(((float)(rssi - RSSI_MIN) / (RSSI_MAX - RSSI_MIN)) * height);
}

float SpectrumMode::channelToFreq(uint8_t channel) {
    // 2.4GHz band: Ch1=2412MHz, 5MHz spacing, Ch13=2472MHz
    if (channel < 1) channel = 1;
    if (channel > 13) channel = 13;
    return 2412.0f + (channel - 1) * 5.0f;
}

void SpectrumMode::pruneStale() {
    // Guard against callback modifying networks during prune
    busy = true;
    
    uint32_t now = millis();
    
    // Remove networks not seen recently
    networks.erase(
        std::remove_if(networks.begin(), networks.end(), 
            [now](const SpectrumNetwork& n) {
                return (now - n.lastSeen) > STALE_TIMEOUT_MS;
            }),
        networks.end()
    );
    
    // Fix selectedIndex if it's now out of bounds
    if (selectedIndex >= (int)networks.size()) {
        selectedIndex = networks.empty() ? -1 : 0;
    }
    
    busy = false;
}

void SpectrumMode::onBeacon(const uint8_t* bssid, uint8_t channel, int8_t rssi, const char* ssid, wifi_auth_mode_t authmode, bool hasPMF, bool isProbeResponse) {
    // Skip if main thread is accessing networks
    if (busy) return;
    
    bool hasSSID = (ssid && ssid[0] != 0);
    
    // Look for existing network
    for (auto& net : networks) {
        if (memcmp(net.bssid, bssid, 6) == 0) {
            // Update existing
            net.rssi = rssi;
            net.lastSeen = millis();
            net.authmode = authmode;  // Update auth mode
            net.hasPMF = hasPMF;      // Update PMF status
            
            // Probe response can reveal hidden SSID
            if (hasSSID && net.isHidden && net.ssid[0] == 0) {
                strncpy(net.ssid, ssid, 32);
                net.ssid[32] = 0;
                net.wasRevealed = true;
                // Defer logging to main thread (avoid Serial in WiFi callback)
                if (!pendingReveal) {
                    strncpy(pendingRevealSSID, ssid, 32);
                    pendingRevealSSID[32] = 0;
                    pendingReveal = true;
                }
            }
            // Also update if we had no SSID before
            else if (hasSSID && net.ssid[0] == 0) {
                strncpy(net.ssid, ssid, 32);
                net.ssid[32] = 0;
            }
            return;
        }
    }
    
    // Add new network (limit to prevent OOM)
    if (networks.size() >= 100) return;
    
    SpectrumNetwork net = {0};
    memcpy(net.bssid, bssid, 6);
    if (hasSSID) {
        strncpy(net.ssid, ssid, 32);
        net.ssid[32] = 0;
        net.isHidden = false;
    } else {
        // Empty SSID = hidden network
        net.isHidden = true;
    }
    net.channel = channel;
    net.rssi = rssi;
    net.lastSeen = millis();
    net.authmode = authmode;
    net.hasPMF = hasPMF;
    net.wasRevealed = false;
    
    networks.push_back(net);
    
    // Auto-select first network
    if (selectedIndex < 0) {
        selectedIndex = 0;
    }
}

String SpectrumMode::getSelectedInfo() {
    // Guard against callback race
    if (busy) return "Scanning...";
    
    if (selectedIndex >= 0 && selectedIndex < (int)networks.size()) {
        const auto& net = networks[selectedIndex];
        
        // Bottom bar: ~33 chars available (240px - margins - uptime)
        // Fixed part: " -XXdB chXX YYYY" = ~16 chars worst case
        // SSID gets max 15 chars + ".." if truncated
        const int MAX_SSID_DISPLAY = 15;
        
        String ssid;
        if (net.ssid[0]) {
            ssid = net.wasRevealed ? String("*") + net.ssid : net.ssid;
        } else {
            ssid = "[hidden]";
        }
        if (ssid.length() > MAX_SSID_DISPLAY) {
            ssid = ssid.substring(0, MAX_SSID_DISPLAY) + "..";
        }
        
        char buf[64];
        snprintf(buf, sizeof(buf), "%s %ddB ch%d %s", 
                 ssid.c_str(), 
                 net.rssi, net.channel,
                 authModeToShortString(net.authmode));
        return String(buf);
    }
    if (networks.empty()) {
        return "Scanning...";
    }
    return "Press Enter to select";
}

// Promiscuous callback - extract beacon info
void SpectrumMode::promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (!running) return;
    if (type != WIFI_PKT_MGMT) return;
    
    const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    const uint8_t* payload = pkt->payload;
    uint16_t len = pkt->rx_ctrl.sig_len;
    int8_t rssi = pkt->rx_ctrl.rssi;
    uint8_t channel = pkt->rx_ctrl.channel;
    
    if (len < 36) return;
    
    // Check frame type - beacon (0x80) or probe response (0x50)
    uint8_t frameType = payload[0];
    if (frameType != 0x80 && frameType != 0x50) return;
    
    bool isProbeResponse = (frameType == 0x50);
    
    // BSSID is at offset 16
    const uint8_t* bssid = payload + 16;
    
    // Parse SSID from tagged parameters (starts at offset 36)
    char ssid[33] = {0};
    uint16_t offset = 36;
    
    while (offset + 2 < len) {
        uint8_t tagNum = payload[offset];
        uint8_t tagLen = payload[offset + 1];
        
        if (offset + 2 + tagLen > len) break;
        
        if (tagNum == 0 && tagLen <= 32) {  // SSID tag
            memcpy(ssid, payload + offset + 2, tagLen);
            ssid[tagLen] = 0;
            break;
        }
        
        offset += 2 + tagLen;
    }
    
    // Parse auth mode from RSN (0x30) and WPA (0xDD) IEs
    wifi_auth_mode_t authmode = WIFI_AUTH_OPEN;  // Default to open
    bool hasRSN = false;
    offset = 36;
    while (offset + 2 < len) {
        uint8_t tagNum = payload[offset];
        uint8_t tagLen = payload[offset + 1];
        
        if (offset + 2 + tagLen > len) break;
        
        if (tagNum == 0x30 && tagLen >= 2) {  // RSN IE = WPA2/WPA3
            hasRSN = true;
            authmode = WIFI_AUTH_WPA2_PSK;
        } else if (tagNum == 0xDD && tagLen >= 8) {  // Vendor specific
            // Check for WPA1 OUI: 00:50:F2:01
            if (payload[offset + 2] == 0x00 && payload[offset + 3] == 0x50 &&
                payload[offset + 4] == 0xF2 && payload[offset + 5] == 0x01) {
                // WPA1 - only set if not already WPA2
                if (!hasRSN) {
                    authmode = WIFI_AUTH_WPA_PSK;
                } else {
                    authmode = WIFI_AUTH_WPA_WPA2_PSK;
                }
            }
        }
        
        offset += 2 + tagLen;
    }
    
    // Detect PMF (Protected Management Frames)
    bool hasPMF = detectPMF(payload, len);
    
    // If PMF is required and we have RSN, it's WPA3 (or WPA2/3 transitional)
    if (hasPMF && authmode == WIFI_AUTH_WPA2_PSK) {
        authmode = WIFI_AUTH_WPA3_PSK;
    }
    
    // Update spectrum data
    onBeacon(bssid, channel, rssi, ssid, authmode, hasPMF, isProbeResponse);
}

// Check if auth mode is considered vulnerable (OPEN, WEP, WPA1)
bool SpectrumMode::isVulnerable(wifi_auth_mode_t mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN:
        case WIFI_AUTH_WEP:
        case WIFI_AUTH_WPA_PSK:
            return true;
        default:
            return false;
    }
}

// Convert auth mode to short display string
const char* SpectrumMode::authModeToShortString(wifi_auth_mode_t mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN: return "OPEN";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/2";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/3";
        case WIFI_AUTH_WAPI_PSK: return "WAPI";
        default: return "?";
    }
}

// Detect PMF (Protected Management Frames) from RSN IE
// Networks with PMF required (MFPR=1) are immune to deauth attacks
bool SpectrumMode::detectPMF(const uint8_t* payload, uint16_t len) {
    uint16_t offset = 36;  // After fixed beacon fields
    
    while (offset + 2 < len) {
        uint8_t tag = payload[offset];
        uint8_t tagLen = payload[offset + 1];
        
        if (offset + 2 + tagLen > len) break;
        
        if (tag == 0x30 && tagLen >= 8) {  // RSN IE
            // RSN IE structure: version(2) + group cipher(4) + pairwise count(2) + ...
            uint16_t rsnOffset = offset + 2;
            uint16_t rsnEnd = rsnOffset + tagLen;
            
            // Skip version (2), group cipher (4)
            rsnOffset += 6;
            if (rsnOffset + 2 > rsnEnd) break;
            
            // Pairwise cipher count and suites
            uint16_t pairwiseCount = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            rsnOffset += 2 + (pairwiseCount * 4);
            if (rsnOffset + 2 > rsnEnd) break;
            
            // AKM count and suites
            uint16_t akmCount = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            rsnOffset += 2 + (akmCount * 4);
            if (rsnOffset + 2 > rsnEnd) break;
            
            // RSN Capabilities (2 bytes)
            uint16_t rsnCaps = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            
            // Bit 7: MFPR (Management Frame Protection Required)
            bool mfpr = (rsnCaps >> 7) & 0x01;
            
            if (mfpr) {
                return true;  // PMF required - deauth won't work
            }
        }
        
        offset += 2 + tagLen;
    }
    
    return false;
}
