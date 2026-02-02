// Charging Mode - Low power battery display
#pragma once

#include <Arduino.h>
#include <M5Unified.h>

class ChargingMode {
public:
    static void init();
    static void start();
    static void stop();
    static void update();
    static void draw(M5Canvas& canvas);
    
    static bool isRunning() { return running; }
    static bool shouldExit() { return exitRequested; }
    static void clearExit() { exitRequested = false; }
    static bool areBarsHidden() { return barsHidden; }
    
    // Battery info getters for display
    static uint8_t getBatteryPercent() { return batteryPercent; }
    static float getBatteryVoltage() { return batteryVoltage; }
    static bool isCharging() { return charging; }
    static int getMinutesToFull() { return minutesToFull; }
    
private:
    static bool running;
    static bool exitRequested;
    static bool keyWasPressed;
    
    // Battery state
    static uint8_t batteryPercent;
    static float batteryVoltage;
    static bool charging;
    static int minutesToFull;
    
    // Voltage tracking for charge rate estimation
    static float voltageHistory[10];
    static uint8_t voltageHistoryIdx;
    static uint32_t lastVoltageMs;
    static uint32_t lastUpdateMs;
    
    // Animation
    static uint8_t animFrame;
    static uint32_t lastAnimMs;

    // Bar-less mode state (like SD format)
    static bool barsHidden;

    // Exit/unplug detection
    static uint32_t unplugDetectMs;

    // Charge-rate estimate tracking
    static float lastEstimateVoltage;
    static uint32_t lastEstimateMs;
    
    // Calculate battery percentage from voltage (more accurate than AXP)
    static uint8_t voltageToPercent(float voltage, bool isCharging);
    
    // Estimate minutes to full based on charge rate
    static int estimateMinutesToFull();
    
    static void handleInput();
    static void updateBattery();
};
