// flipper_detect.h
// ---------------------------------------------------------------------------
// Passive Flipper Zero detection for the DEFENSIVE SUITE ("FLIPPER FINDER").
//
// A Flipper Zero with Bluetooth on advertises with a complete local name that
// starts "Flipper" and a BLE address in one of its known ranges. RX-only.
//
// Signature (facts): BLE MAC prefixes 0C:FA:22, 80:E1:26, 80:E1:27 and the
// "Flipper" advertised-name prefix. Attribution: community Flipper BLE research
// / ReconGrunt. Framework-agnostic core, host-testable.
// ---------------------------------------------------------------------------
#pragma once
#include <stdint.h>

namespace flipperdet {

struct FlipperHit {
    bool    isFlipper = false;
    char    name[24] = {0};   // advertised name if present
    int8_t  rssi = 0;
    uint8_t addr[6] = {0};

    bool hit() const { return isFlipper; }
};

// Inspect a BLE advertisement + its address for a Flipper Zero.
FlipperHit flipperInspect(const uint8_t* addr, const uint8_t* adv,
                          uint8_t advLen, int8_t rssi);

} // namespace flipperdet
