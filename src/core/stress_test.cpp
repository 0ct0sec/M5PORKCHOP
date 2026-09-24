// Stress Test Module - Inject fake data to test modes without RF
#include "stress_test.h"
#include "../modes/spectrum.h"
#include "../modes/oink.h"
#include "../modes/do_no_ham.h"
#include "../ui/display.h"
#include "heap_policy.h"
#include <M5Cardputer.h>

// Static member definitions
bool StressTest::active = false;
StressScenario StressTest::scenario = StressScenario::IDLE;
uint32_t StressTest::lastInjectTime = 0;
uint32_t StressTest::injectedCount = 0;
uint32_t StressTest::injectRate = 0;
uint32_t StressTest::lastRateCalc = 0;
uint32_t StressTest::injectsSinceLastCalc = 0;
uint8_t StressTest::networkCounter = 0;
uint8_t StressTest::clientCounter = 0;

// Stress guardrails (keep test heavy without crashing the device)
static const size_t STRESS_MAX_OINK_NETWORKS = 75;
static const size_t STRESS_MAX_DNH_NETWORKS = 60;

// Realistic SSID pool
const char* StressTest::ssidPool[] = {
    "NETGEAR", "linksys", "ATT-WIFI", "xfinitywifi", "ORBI",
    "MySpectrumWiFi", "Verizon_5G", "DIRECT-TV", "HP-Print",
    "Ring-12ab34", "Nest-Audio", "Chromecast", "Amazon-Fire",
    "Tesla-Guest", "Starlink", "5G_Home", "CenturyLink",
    "Frontier_WiFi", "Cox_Guest", "Optimum_WiFi", "T-Mobile_Home",
    "GoogleFiber", "AT&T_5G", "Hidden_Network", "FBI_VAN",
    "PrettyFlyForAWiFi", "GetOffMyLAN", "TheLANBeforeTime",
    "WuTangLAN", "BillWiTheScienceFi", "LANDownUnder"
};
const uint8_t StressTest::ssidPoolSize = sizeof(ssidPool) / sizeof(ssidPool[0]);

void StressTest::init() {
    active = false;
    scenario = StressScenario::IDLE;
    lastInjectTime = 0;
    injectedCount = 0;
    injectRate = 0;
    lastRateCalc = millis();
    injectsSinceLastCalc = 0;
    networkCounter = 0;
    clientCounter = 0;
}

void StressTest::checkActivation() {
    // Disabled: stress test activation removed to reduce heap churn risk
}

void StressTest::setScenario(StressScenario s) {
    scenario = s;
    Serial.printf("[STRESS] Scenario: %d\n", (int)s);
}

void StressTest::nextScenario() {
    // Disabled
}

void StressTest::update() {
    if (!active || scenario == StressScenario::IDLE) return;
    
    uint32_t now = millis();
    
    // Calculate injection rate (per second)
    if (now - lastRateCalc >= 1000) {
        injectRate = injectsSinceLastCalc;
        injectsSinceLastCalc = 0;
        lastRateCalc = now;
    }
    
    // Inject interval depends on scenario
    uint32_t interval = 50;  // Default: 20/sec
    switch (scenario) {
        case StressScenario::NETWORK_FLOOD:  interval = 20;  break;  // 50/sec
        case StressScenario::CLIENT_FLOOD:   interval = 30;  break;  // 33/sec
        case StressScenario::CHURN:          interval = 100; break;  // 10/sec
        case StressScenario::HIDDEN_REVEAL:  interval = 200; break;  // 5/sec
        case StressScenario::RSSI_CHAOS:     interval = 10;  break;  // 100/sec
        case StressScenario::MIXED_AUTH:     interval = 50;  break;  // 20/sec
        default: break;
    }
    
    if (now - lastInjectTime < interval) return;
    lastInjectTime = now;
    
    // Execute scenario
    switch (scenario) {
        case StressScenario::NETWORK_FLOOD:
            injectNetwork();
            break;
        case StressScenario::CLIENT_FLOOD:
            injectClient();
            break;
        case StressScenario::CHURN:
            updateChurn();
            break;
        case StressScenario::HIDDEN_REVEAL:
            injectHidden();
            break;
        case StressScenario::RSSI_CHAOS:
            updateRSSIChaos();
            break;
        case StressScenario::MIXED_AUTH:
            injectNetwork();  // Same as flood but with mixed auth
            break;
        default:
            break;
    }
    
    injectedCount++;
    injectsSinceLastCalc++;
}

void StressTest::injectNetwork() {
    if (ESP.getFreeHeap() < HeapPolicy::kStressMinHeap) {
        return;
    }

    uint8_t bssid[6];
    randomBSSID(bssid);
    
    wifi_auth_mode_t auth = (scenario == StressScenario::MIXED_AUTH) 
        ? randomAuthMode() 
        : WIFI_AUTH_WPA2_PSK;
    
    bool hasPMF = (random(100) < 20);  // 20% have PMF
    uint8_t channel = randomChannel();
    int8_t rssi = randomRSSI();
    const char* ssid = randomSSID();
    
    // Inject into whichever mode is running
    if (SpectrumMode::isRunning()) {
        SpectrumMode::onBeacon(bssid, channel, true, rssi, ssid, auth, hasPMF, false);
    }
    if (OinkMode::isRunning() && OinkMode::getNetworkCount() < STRESS_MAX_OINK_NETWORKS) {
        OinkMode::injectTestNetwork(bssid, ssid, channel, rssi, auth, hasPMF);
    }
    if (DoNoHamMode::isRunning() && DoNoHamMode::getNetworkCount() < STRESS_MAX_DNH_NETWORKS) {
        DoNoHamMode::injectTestNetwork(bssid, ssid, channel, rssi, auth, hasPMF);
    }
}

void StressTest::injectClient() {
    // Only works if spectrum is monitoring
    if (!SpectrumMode::isRunning() || !SpectrumMode::isMonitoring()) {
        return;
    }
    
    // We can't directly inject clients - they come from data frames
    // Instead, we inject a fake beacon to keep the network alive
    // TODO: Add client injection API to SpectrumMode if needed
    injectNetwork();
}

void StressTest::updateChurn() {
    // Alternate between adding new networks and letting old ones expire
    static uint8_t phase = 0;
    phase = (phase + 1) % 10;
    
    if (phase < 7) {
        // 70% of the time: add networks
        injectNetwork();
    }
    // 30%: do nothing, let prune remove stale ones
}

void StressTest::injectHidden() {
    if (ESP.getFreeHeap() < HeapPolicy::kStressMinHeap) {
        return;
    }

    uint8_t bssid[6];
    randomBSSID(bssid);
    
    static uint8_t revealCounter = 0;
    revealCounter++;
    
    uint8_t channel = randomChannel();
    int8_t rssi = randomRSSI();
    
    // First time: hidden (no SSID), every 3rd: reveal
    bool reveal = (revealCounter % 3 == 0);
    const char* ssid = reveal ? "REVEALED_HIDDEN" : "";
    
    if (SpectrumMode::isRunning()) {
        SpectrumMode::onBeacon(bssid, channel, true, rssi, ssid, WIFI_AUTH_WPA2_PSK, false, reveal);
    }
    if (OinkMode::isRunning() && OinkMode::getNetworkCount() < STRESS_MAX_OINK_NETWORKS) {
        OinkMode::injectTestNetwork(bssid, ssid, channel, rssi, WIFI_AUTH_WPA2_PSK, false);
    }
    if (DoNoHamMode::isRunning() && DoNoHamMode::getNetworkCount() < STRESS_MAX_DNH_NETWORKS) {
        DoNoHamMode::injectTestNetwork(bssid, ssid, channel, rssi, WIFI_AUTH_WPA2_PSK, false);
    }
}

void StressTest::updateRSSIChaos() {
    if (ESP.getFreeHeap() < HeapPolicy::kStressMinHeap) {
        return;
    }

    // Re-inject existing networks with wildly varying RSSI
    // This tests UI stability with rapid signal changes
    uint8_t bssid[6];
    // Use low counter to hit same BSSIDs repeatedly
    bssid[0] = 0xAA;
    bssid[1] = 0xBB;
    bssid[2] = 0xCC;
    bssid[3] = 0x00;
    bssid[4] = 0x00;
    bssid[5] = networkCounter % 10;  // Only 10 unique networks
    
    int8_t rssi = randomRSSI();
    
    if (SpectrumMode::isRunning()) {
        SpectrumMode::onBeacon(bssid, 6, true, rssi, "RSSI_TEST", WIFI_AUTH_WPA2_PSK, false, false);
    }
    if (OinkMode::isRunning() && OinkMode::getNetworkCount() < STRESS_MAX_OINK_NETWORKS) {
        OinkMode::injectTestNetwork(bssid, "RSSI_TEST", 6, rssi, WIFI_AUTH_WPA2_PSK, false);
    }
    if (DoNoHamMode::isRunning() && DoNoHamMode::getNetworkCount() < STRESS_MAX_DNH_NETWORKS) {
        DoNoHamMode::injectTestNetwork(bssid, "RSSI_TEST", 6, rssi, WIFI_AUTH_WPA2_PSK, false);
    }
}

// === Random Data Generators ===

void StressTest::randomBSSID(uint8_t* bssid) {
    // Use recognizable OUI prefix for stress test networks
    bssid[0] = 0xDE;  // "DE:AD:..."
    bssid[1] = 0xAD;
    bssid[2] = 0xBE;
    bssid[3] = (networkCounter >> 8) & 0xFF;
    bssid[4] = networkCounter & 0xFF;
    bssid[5] = random(256);
    networkCounter++;
}

void StressTest::randomMAC(uint8_t* mac) {
    mac[0] = 0xCA;  // "CA:FE:..."
    mac[1] = 0xFE;
    mac[2] = 0xBA;
    mac[3] = 0xBE;
    mac[4] = (clientCounter >> 8) & 0xFF;
    mac[5] = clientCounter & 0xFF;
    clientCounter++;
}

int8_t StressTest::randomRSSI() {
    // Random RSSI between -90 and -30 dBm
    return -90 + random(60);
}

uint8_t StressTest::randomChannel() {
    return 1 + random(13);  // Channels 1-13
}

wifi_auth_mode_t StressTest::randomAuthMode() {
    uint8_t r = random(100);
    if (r < 10) return WIFI_AUTH_OPEN;
    if (r < 15) return WIFI_AUTH_WEP;
    if (r < 25) return WIFI_AUTH_WPA_PSK;
    if (r < 60) return WIFI_AUTH_WPA2_PSK;
    if (r < 80) return WIFI_AUTH_WPA_WPA2_PSK;
    return WIFI_AUTH_WPA3_PSK;
}

const char* StressTest::randomSSID() {
    return ssidPool[random(ssidPoolSize)];
}
