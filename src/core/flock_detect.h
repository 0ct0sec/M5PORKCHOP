// flock_detect.h
// ---------------------------------------------------------------------------
// Passive surveillance-device detector for M5PORKCHOP's WARHOG wardriving mode.
//
// Design goals:
//   * PASSIVE ONLY. This module never transmits. It inspects frames/adverts
//     that WARHOG's existing promiscuous + BLE-scan pipeline already receives.
//   * Framework-agnostic core. No Arduino/IDF/M5 dependencies here so it can be
//     unit-tested on a host (matches the repo's existing test/ directory).
//   * Confidence-scored. Generic Espressif OUIs alone are treated as weak and
//     require corroboration, which is what keeps false positives down.
//
// Integration: call inspectWifiFrame() from the promiscuous RX callback and
// inspectBleAdv() from the BLE scan callback. On a positive Detection, the
// caller stamps it with the current GPS fix and appends it to the wardrive log.
//
// Written originally for M5PORKCHOP (MIT). See flock_signatures.h for data
// attribution.
// ---------------------------------------------------------------------------
#pragma once
#include <stdint.h>

namespace flockdet {

enum class DeviceKind : uint8_t {
    None = 0,
    FlockCamera,        // Flock Safety ALPR camera (WiFi or BLE match)
    RavenDetector,      // SoundThinking/ShotSpotter Raven (BLE svc UUID)
    SuspectSurveillance // corroborated but unclassified
};

// Ordered low -> high. WARHOG can choose an alert threshold (e.g. only
// beep/log at MEDIUM+) to trade recall for precision.
enum class Confidence : uint8_t {
    None   = 0,
    Low    = 1,   // single weak signal (e.g. generic ESP OUI only)
    Medium = 2,   // one strong signal, or two weak signals combined
    High   = 3,   // strong signal + corroboration
};

struct Detection {
    DeviceKind kind      = DeviceKind::None;
    Confidence confidence = Confidence::None;
    uint8_t    mac[6]    = {0};   // the matched device MAC (addr1 or addr2, or BLE addr)
    int8_t     rssi      = 0;
    uint8_t    channel   = 0;     // WiFi channel (0 for BLE)
    const char* reason   = "";    // human-readable match reason for the log/UI

    bool hit() const { return kind != DeviceKind::None; }
};

class FlockDetect {
public:
    FlockDetect() = default;

    // Minimum confidence at/above which hit() detections are reported.
    // Default Medium keeps the generic-Espressif noise out of the log.
    void setAlertThreshold(Confidence c) { threshold_ = c; }

    // Inspect one raw 802.11 frame from promiscuous mode.
    //   payload : raw frame starting at the MAC header (frame control byte 0)
    //   len     : frame length in bytes
    //   rssi    : from the promiscuous rx_ctrl
    //   channel : current hop channel
    // Returns a Detection; check .hit() and .confidence.
    Detection inspectWifiFrame(const uint8_t* payload, uint16_t len,
                               int8_t rssi, uint8_t channel) const;

    // Inspect one BLE advertisement.
    //   addr    : 6-byte BLE device address
    //   adv     : raw advertisement/scan-response payload (AD structures)
    //   advLen  : length of adv
    //   rssi    : advertisement RSSI
    Detection inspectBleAdv(const uint8_t* addr, const uint8_t* adv,
                            uint8_t advLen, int8_t rssi) const;

    // Inspect one active-scan result (AP). WARHOG uses active scanning rather
    // than promiscuous capture, so this path matches an AP's BSSID OUI against
    // the Flock table and its SSID against name hints. Note: this only catches
    // Flock gear that beacons as an AP; catching camera *client* MACs needs the
    // promiscuous inspectWifiFrame() path above.
    Detection inspectScanResult(const uint8_t* bssid, const char* ssid,
                                int8_t rssi, uint8_t channel) const;

private:
    Confidence threshold_ = Confidence::Medium;

    // Returns index into kFlockOuis or -1.
    static int matchOui(const uint8_t* mac);
};

} // namespace flockdet
