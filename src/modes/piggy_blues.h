// Piggy Blues Mode - BLE Notification Spam
// Uses continuous passive scanning with opportunistic payload delivery
#pragma once

#include <Arduino.h>
#include <vector>
#include <NimBLEDevice.h>

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

// Forward declaration
class PiggyBluesScanCallbacks;

class PiggyBluesMode {
public:
    static void init();
    static void start();
    static void stop();
    static void update();
    static bool isRunning() { return running; }
    
    // Statistics
    static uint32_t getTotalPackets() { return totalPackets; }
    static uint32_t getAppleCount() { return appleCount; }
    static uint32_t getAndroidCount() { return androidCount; }
    static uint32_t getSamsungCount() { return samsungCount; }
    static uint32_t getWindowsCount() { return windowsCount; }
    
    // Vendor identification (public for callback use)
    static BLEVendor identifyVendor(const uint8_t* mfgData, size_t len);
    
private:
    static bool running;
    static bool confirmed;  // User confirmed warning dialog
    
    // Attack state
    static uint32_t lastBurstTime;
    static uint16_t burstInterval;  // ms between bursts
    
    // Continuous scan state
    static bool scanRunning;
    static bool advertisingNow;  // True while sending advertisement burst
    
    // Targets
    static std::vector<BLETarget> targets;
    static uint8_t activeCount;
    
    // Statistics
    static uint32_t totalPackets;
    static uint32_t appleCount;
    static uint32_t androidCount;
    static uint32_t samsungCount;
    static uint32_t windowsCount;

    // BLE state synchronization helpers
    static bool getAdvertisingNow();
    static void setAdvertisingNow(bool value);
    static bool getScanRunning();
    static void setScanRunning(bool value);
    
    // Internal methods
    static bool showWarningDialog();
    static void startContinuousScan();
    static void stopContinuousScan();
    static void processTargets();
    static void upsertTarget(const BLETarget& target);
    static void ageOutStaleTargets();
    static void selectActiveTargets();
    static void sendAppleJuice();
    static void sendAndroidFastPair();
    static void sendSamsungSpam();
    static void sendWindowsSwiftPair();
    static void sendRandomPayload();
    
    // Scan callbacks friend access
    friend class PiggyBluesScanCallbacks;
};

// Scan callback class for continuous async scanning
class PiggyBluesScanCallbacks : public NimBLEScanCallbacks {
public:
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override;
    void onScanEnd(const NimBLEScanResults& results, int reason) override;
};


