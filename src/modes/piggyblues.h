// Piggy Blues Mode - BLE Notification Spam
#pragma once

#include <Arduino.h>
#include <vector>

// Target device vendor types
enum class BLEVendor : uint8_t {
    UNKNOWN = 0,
    APPLE,
    ANDROID,
    SAMSUNG,
    WINDOWS
};

struct BLETarget {
    uint8_t addr[6];      // BLE address
    int8_t rssi;          // Signal strength
    BLEVendor vendor;     // Identified vendor
    uint32_t lastSeen;    // Last seen timestamp
};

class PiggyBluesMode {
public:
    static void init();
    static void start();
    static void stop();
    static void update();
    static bool isRunning() { return running; }
    
    // Statistics
    static uint32_t getTotalPackets() { return totalPackets; }
    static uint16_t getAppleCount() { return appleCount; }
    static uint16_t getAndroidCount() { return androidCount; }
    static uint16_t getSamsungCount() { return samsungCount; }
    static uint16_t getWindowsCount() { return windowsCount; }
    
private:
    static bool running;
    static bool confirmed;  // User confirmed warning dialog
    
    // Attack state
    static uint32_t lastBurstTime;
    static uint16_t burstInterval;  // ms between bursts
    
    // Targets
    static std::vector<BLETarget> targets;
    static uint8_t activeTargets[4];  // Indices of 4 active targets
    static uint8_t activeCount;
    
    // Statistics
    static uint32_t totalPackets;
    static uint16_t appleCount;
    static uint16_t androidCount;
    static uint16_t samsungCount;
    static uint16_t windowsCount;
    
    // Internal methods
    static bool showWarningDialog();
    static void scanForDevices();
    static void selectTargets();
    static BLEVendor identifyVendor(const uint8_t* mfgData, size_t len);
    static void sendAppleJuice();
    static void sendAndroidFastPair();
    static void sendSamsungSpam();
    static void sendWindowsSwiftPair();
    static void sendRandomPayload();
};

