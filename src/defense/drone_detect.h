// drone_detect.h
// ---------------------------------------------------------------------------
// Passive drone Remote ID detection for the DEFENSIVE SUITE ("SKY HOGS").
//
// Drones broadcasting under ASTM F3411 / OpenDroneID announce themselves in the
// clear -- by law in the US. Over BLE the advertisement carries 16-bit service
// data for UUID 0xFFFA with application code 0x0D, an 8-bit message counter, and
// a 25-byte Remote ID message. We flag the presence and, for a Basic ID message
// (message type 0x0), pull the 20-byte UAS ID string.
//
// RX-only. ASTM F3411 is a public spec; the UUID/app-code are facts.
// Attribution: opendroneid/opendroneid-core-c, colonelpanichacks/Sky-Spy,
// simeononsecurity "Eye Spy" (UUID 0xFFFA + app code 0x0D). Verified 2026-08.
// Framework-agnostic core, host-testable.
// ---------------------------------------------------------------------------
#pragma once
#include <stdint.h>

namespace dronedet {

struct DroneHit {
    bool    isDrone = false;
    char    idText[21] = {0};   // UAS ID (Basic ID msg), ASCII, may be empty
    int8_t  rssi = 0;
    uint8_t addr[6] = {0};

    bool hit() const { return isDrone; }
};

// Inspect one BLE advertisement (raw AD structures) for an OpenDroneID frame.
DroneHit droneInspectBleAdv(const uint8_t* addr, const uint8_t* adv,
                            uint8_t advLen, int8_t rssi);

} // namespace dronedet
