// Charging Mode implementation

#include "charging.h"
#include "../ui/display.h"
#include "../core/wifi_utils.h"
#include "../core/network_recon.h"
#include "../gps/gps.h"
#include "../core/config.h"
#include <M5Cardputer.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <NimBLEDevice.h>

// Static member initialization
bool ChargingMode::running = false;
bool ChargingMode::exitRequested = false;
bool ChargingMode::keyWasPressed = false;

uint8_t ChargingMode::batteryPercent = 0;
float ChargingMode::batteryVoltage = 0.0f;
bool ChargingMode::charging = false;
int ChargingMode::minutesToFull = -1;

float ChargingMode::voltageHistory[10] = {0};
uint8_t ChargingMode::voltageHistoryIdx = 0;
uint32_t ChargingMode::lastVoltageMs = 0;
uint32_t ChargingMode::lastUpdateMs = 0;

uint8_t ChargingMode::animFrame = 0;
uint32_t ChargingMode::lastAnimMs = 0;
uint32_t ChargingMode::unplugDetectMs = 0;
float ChargingMode::lastEstimateVoltage = 0.0f;
uint32_t ChargingMode::lastEstimateMs = 0;

bool ChargingMode::reconWasActive = false;
bool ChargingMode::gpsWasActive = false;
uint8_t ChargingMode::wifiModeBefore = static_cast<uint8_t>(WIFI_MODE_NULL);
bool ChargingMode::wifiWasOn = false;
bool ChargingMode::powerPresent = false;
bool ChargingMode::powerSeen = false;
uint32_t ChargingMode::lastChargingMs = 0;

static const uint32_t kChargeHoldMs = 10000;
static const int16_t kVbusPresentMv = 4000;
static const uint32_t kUnplugExitDelayMs = 3000;

// Li-ion voltage curves (approximate)
// Discharge curve is different from charge curve due to internal resistance
static const float kDischargeVoltages[] = {3.00f, 3.30f, 3.50f, 3.60f, 3.70f, 3.75f, 3.80f, 3.90f, 4.00f, 4.10f, 4.20f};
static const uint8_t kDischargePercents[] = {0, 5, 10, 20, 30, 40, 50, 60, 70, 85, 100};

// Charging curve - voltage reads higher due to charge current
static const float kChargeVoltages[] = {3.50f, 3.70f, 3.85f, 3.95f, 4.05f, 4.10f, 4.15f, 4.18f, 4.20f, 4.20f, 4.20f};
static const uint8_t kChargePercents[] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};

void ChargingMode::init() {
    running = false;
    exitRequested = false;
    batteryPercent = 0;
    batteryVoltage = 0.0f;
    charging = false;
    minutesToFull = -1;
    unplugDetectMs = 0;
    lastEstimateVoltage = 0.0f;
    lastEstimateMs = 0;
    reconWasActive = false;
    gpsWasActive = false;
    wifiModeBefore = static_cast<uint8_t>(WIFI_MODE_NULL);
    wifiWasOn = false;
    powerPresent = false;
    powerSeen = false;
    lastChargingMs = 0;
    
    memset(voltageHistory, 0, sizeof(voltageHistory));
    voltageHistoryIdx = 0;
    lastVoltageMs = 0;
    lastUpdateMs = 0;
    animFrame = 0;
    lastAnimMs = 0;
}

void ChargingMode::start() {
    Serial.println("[CHARGING] Starting charging mode - shutting down services");
    
    // Stop NetworkRecon if running
    reconWasActive = NetworkRecon::isRunning() || NetworkRecon::isPaused();
    if (reconWasActive) {
        NetworkRecon::stop();
        Serial.println("[CHARGING] NetworkRecon stopped");
    }
    
    // Stop GPS to save power
    gpsWasActive = GPS::isActive();
    if (gpsWasActive) {
        GPS::sleep();
        Serial.println("[CHARGING] GPS sleeping");
    }
    
    // Shutdown WiFi completely
    wifiModeBefore = static_cast<uint8_t>(WiFi.getMode());
    wifiWasOn = (wifiModeBefore != WIFI_MODE_NULL);
    if (wifiWasOn) {
        WiFiUtils::shutdown();
        Serial.println("[CHARGING] WiFi stopped");
    } else {
        Serial.println("[CHARGING] WiFi already off");
    }
    
    // Deinit BLE if initialized
    if (NimBLEDevice::isInitialized()) {
        NimBLEScan* pScan = NimBLEDevice::getScan();
        if (pScan && pScan->isScanning()) {
            pScan->stop();
        }
        NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
        if (pAdv && pAdv->isAdvertising()) {
            pAdv->stop();
        }
        NimBLEDevice::deinit(true);
        Serial.println("[CHARGING] BLE deinitialized");
    }
    
    // Dim display to minimum
    M5.Display.setBrightness(10);
    
    // Initialize state
    running = true;
    exitRequested = false;
    keyWasPressed = true;  // Prevent immediate key detection
    unplugDetectMs = 0;
    lastEstimateVoltage = 0.0f;
    lastEstimateMs = 0;
    powerPresent = false;
    powerSeen = false;
    lastChargingMs = 0;
    
    // Initialize voltage history
    memset(voltageHistory, 0, sizeof(voltageHistory));
    voltageHistoryIdx = 0;
    lastVoltageMs = 0;
    lastUpdateMs = 0;
    animFrame = 0;
    lastAnimMs = millis();
    
    // Initial battery read
    updateBattery();
    
    Serial.printf("[CHARGING] Mode started. Battery: %d%% (%.2fV) Charging: %s\n",
                  batteryPercent, batteryVoltage, charging ? "YES" : "NO");
}

void ChargingMode::stop() {
    if (!running) return;
    
    Serial.println("[CHARGING] Stopping charging mode - restoring services");
    
    running = false;
    exitRequested = false;
    unplugDetectMs = 0;
    lastEstimateVoltage = 0.0f;
    lastEstimateMs = 0;
    
    // Restore display brightness
    uint8_t brightness = Config::personality().brightness;
    M5.Display.setBrightness(brightness * 255 / 100);
    
    // Restore WiFi mode if it was active on entry
    if (wifiWasOn) {
        WiFi.mode(static_cast<wifi_mode_t>(wifiModeBefore));
    }
    
    // Wake GPS if it was enabled
    if (gpsWasActive) {
        GPS::wake();
    }

    // Restore NetworkRecon if it was active on entry
    if (reconWasActive) {
        NetworkRecon::start();
    }
    
    Serial.println("[CHARGING] Mode stopped, services restored");
}

void ChargingMode::update() {
    if (!running) return;
    
    handleInput();
    
    // Update battery state every 2 seconds
    uint32_t now = millis();
    if (now - lastUpdateMs >= 2000) {
        lastUpdateMs = now;
        updateBattery();
    }
    
    // Update animation every 500ms
    if (now - lastAnimMs >= 500) {
        lastAnimMs = now;
        animFrame = (animFrame + 1) % 4;
    }
    
    // Auto-exit if unplugged and not charging
    if (powerSeen && !powerPresent) {
        // Small delay to avoid false triggers
        if (unplugDetectMs == 0) {
            unplugDetectMs = now;
        } else if (now - unplugDetectMs > kUnplugExitDelayMs) {
            Serial.println("[CHARGING] Unplugged detected, exiting charging mode");
            exitRequested = true;
            unplugDetectMs = 0;
        }
    } else {
        unplugDetectMs = 0;
    }

    if (exitRequested) {
        stop();
        return;
    }
}

void ChargingMode::handleInput() {
    bool anyPressed = M5Cardputer.Keyboard.isPressed();
    
    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }
    
    if (keyWasPressed) return;
    keyWasPressed = true;
    
    // Any key exits
    if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE) ||
        M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER) ||
        M5Cardputer.Keyboard.isPressed()) {
        exitRequested = true;
    }
}

void ChargingMode::updateBattery() {
    // Read raw voltage (in mV)
    float voltage = M5.Power.getBatteryVoltage() / 1000.0f;
    auto chargeState = M5.Power.isCharging();
    bool isCharging = (chargeState == m5::Power_Class::is_charging_t::is_charging);
    uint32_t now = millis();

    if (isCharging) {
        lastChargingMs = now;
    }
    int16_t vbusMv = M5.Power.getVBUSVoltage();
    bool vbusPresent = (vbusMv >= kVbusPresentMv);
    if (vbusMv <= 0) {
        vbusPresent = false;  // Not supported or invalid reading
    }
    powerPresent = vbusPresent ||
                   isCharging ||
                   (lastChargingMs != 0 && (now - lastChargingMs) < kChargeHoldMs);
    if (powerPresent) {
        powerSeen = true;
    }
    
    // Average with previous readings for stability
    voltageHistory[voltageHistoryIdx] = voltage;
    voltageHistoryIdx = (voltageHistoryIdx + 1) % 10;
    
    float avgVoltage = 0;
    int validCount = 0;
    for (int i = 0; i < 10; i++) {
        if (voltageHistory[i] > 0) {
            avgVoltage += voltageHistory[i];
            validCount++;
        }
    }
    if (validCount > 0) {
        avgVoltage /= validCount;
    } else {
        avgVoltage = voltage;
    }
    
    batteryVoltage = avgVoltage;
    charging = isCharging;
    batteryPercent = voltageToPercent(avgVoltage, powerPresent);
    
    // Estimate time to full if charging
    if (charging && batteryPercent < 100) {
        minutesToFull = estimateMinutesToFull();
    } else {
        minutesToFull = -1;
    }
}

uint8_t ChargingMode::voltageToPercent(float voltage, bool isCharging) {
    const float* voltages = isCharging ? kChargeVoltages : kDischargeVoltages;
    const uint8_t* percents = isCharging ? kChargePercents : kDischargePercents;
    const int count = 11;
    
    // Clamp to valid range
    if (voltage <= voltages[0]) return percents[0];
    if (voltage >= voltages[count - 1]) return percents[count - 1];
    
    // Linear interpolation between curve points
    for (int i = 0; i < count - 1; i++) {
        if (voltage >= voltages[i] && voltage <= voltages[i + 1]) {
            float range = voltages[i + 1] - voltages[i];
            float offset = voltage - voltages[i];
            float ratio = (range > 0) ? (offset / range) : 0;
            
            uint8_t pctRange = percents[i + 1] - percents[i];
            return percents[i] + (uint8_t)(ratio * pctRange);
        }
    }
    
    return percents[count - 1];
}

int ChargingMode::estimateMinutesToFull() {
    // Need at least 5 samples spanning 30+ seconds to estimate
    uint32_t now = millis();
    
    if (lastEstimateVoltage == 0.0f || lastEstimateMs == 0) {
        lastEstimateVoltage = batteryVoltage;
        lastEstimateMs = now;
        return -1;  // Not enough data yet
    }
    
    // Calculate rate every 30 seconds
    if (now - lastEstimateMs < 30000) {
        return minutesToFull;  // Return previous estimate
    }
    
    float deltaV = batteryVoltage - lastEstimateVoltage;
    float deltaMinutes = (now - lastEstimateMs) / 60000.0f;
    
    if (deltaV <= 0 || deltaMinutes <= 0) {
        lastEstimateVoltage = batteryVoltage;
        lastEstimateMs = now;
        return -1;  // Not charging or invalid
    }
    
    // Voltage rate per minute
    float ratePerMin = deltaV / deltaMinutes;
    
    // Estimate time to 4.20V (full)
    float remainingV = 4.20f - batteryVoltage;
    if (remainingV <= 0) {
        lastEstimateVoltage = batteryVoltage;
        lastEstimateMs = now;
        return 0;  // Already full
    }
    
    int estimate = (int)(remainingV / ratePerMin);
    
    // Sanity check - cap at 5 hours
    if (estimate > 300) estimate = 300;
    if (estimate < 0) estimate = -1;
    
    lastEstimateVoltage = batteryVoltage;
    lastEstimateMs = now;
    
    return estimate;
}

void ChargingMode::draw(M5Canvas& canvas) {
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();
    
    canvas.fillSprite(bg);
    canvas.setTextColor(fg);
    
    // Title with charging animation
    canvas.setTextDatum(top_center);
    canvas.setTextSize(2);
    
    const char* animChars[] = {"~", "~~", "~~~", "~~"};
    char titleBuf[32];
    if (powerPresent) {
        snprintf(titleBuf, sizeof(titleBuf), "%s CHARGING %s", 
                 animChars[animFrame], animChars[(animFrame + 2) % 4]);
    } else {
        snprintf(titleBuf, sizeof(titleBuf), "BATTERY");
    }
    const int centerX = DISPLAY_W / 2;
    const int lineH = 18;
    const int lineGap = 6;
    const int blockH = lineH * 3 + lineGap * 2;
    int startY = (MAIN_H - blockH) / 2;
    if (startY < 0) startY = 0;

    canvas.drawString(titleBuf, centerX, startY);

    // Percent + time on same line
    char pctBuf[8];
    snprintf(pctBuf, sizeof(pctBuf), "%d%%", batteryPercent);

    char timeBuf[16];
    bool showTime = false;
    if (charging && minutesToFull > 0) {
        if (minutesToFull >= 60) {
            snprintf(timeBuf, sizeof(timeBuf), "~%dh%02dm",
                     minutesToFull / 60, minutesToFull % 60);
        } else {
            snprintf(timeBuf, sizeof(timeBuf), "~%dm", minutesToFull);
        }
        showTime = true;
    } else if (powerPresent && batteryPercent >= 100) {
        snprintf(timeBuf, sizeof(timeBuf), "FULL");
        showTime = true;
    }

    int midY = startY + lineH + lineGap;
    canvas.setTextSize(2);
    if (showTime) {
        const int gap = 6;
        int pctW = canvas.textWidth(pctBuf);
        int timeW = canvas.textWidth(timeBuf);
        int totalW = pctW + gap + timeW;
        if (totalW <= (DISPLAY_W - 8)) {
            int startX = centerX - totalW / 2;
            canvas.setTextDatum(top_left);
            canvas.drawString(pctBuf, startX, midY);
            canvas.drawString(timeBuf, startX + pctW + gap, midY);
        } else {
            canvas.setTextDatum(top_center);
            canvas.drawString(pctBuf, centerX, midY);
        }
    } else {
        canvas.setTextDatum(top_center);
        canvas.drawString(pctBuf, centerX, midY);
    }

    // Voltage line
    char voltBuf[16];
    snprintf(voltBuf, sizeof(voltBuf), "%.2fV", batteryVoltage);
    int voltY = midY + lineH + lineGap;
    canvas.setTextDatum(top_center);
    canvas.drawString(voltBuf, centerX, voltY);
}
